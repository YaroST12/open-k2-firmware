// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box diagnosis, see k2/pd/diag.hpp. Pure logic over records: no I/O, no heap.
#include "k2/pd/diag.hpp"

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"   // SinkState (the enum only)
#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pd/pdtrace.hpp"

namespace k2::pd::diag {
namespace {

using pdtrace::Type;

constexpr std::uint32_t rd32(const std::uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24;
}

// Message types (header bits 4:0) the rules read.
enum : std::uint8_t {
    kGoodCrc = 1, kAccept = 3, kReject = 4, kPsRdy = 6, kDrSwap = 9, kPrSwap = 10, kVconnSwap = 11, kWait = 12,
    kSoftReset = 13, kNotSupported = 16, kFrSwap = 19,
};
enum : std::uint8_t { kSrcCaps = 1, kRequest = 2, kSnkCaps = 4, kEnterUsb = 8, kEprRequest = 9, kEprMode = 10, kVdm = 15 };
enum : std::uint8_t { kExtSkedb = 15 };
// A request waiting for its answer.
enum : std::uint8_t { kPendNone, kPendRequest, kPendPr, kPendDr, kPendVconn, kPendFr, kPendEusb };
constexpr std::uint8_t swap_bit(std::uint8_t pend) {
    return pend == kPendPr ? 1 : pend == kPendDr ? 2 : pend == kPendVconn ? 4 : pend == kPendFr ? 8 : 0;
}
constexpr std::uint16_t kSvidPd = 0xFF00, kSvidDp = 0xFF01, kSvidTbt = 0x8087;

// Saturating counter step.
template <class T>
void inc(T& v) {
    if (v < T(~T(0))) v++;
}

// sum += prod / 1000, carrying the remainder (milli-units of the product, so u32 sums last for days).
void acc(std::uint32_t& sum, std::uint16_t& frac, std::uint64_t prod) {
    const std::uint64_t t = prod + frac;
    sum += std::uint32_t(t / 1000);
    frac = std::uint16_t(t % 1000);
}

// The best PDO of a set: its power, voltage and current, plus the highest voltage in the set.
struct Best {
    std::uint32_t mw = 0;
    std::uint16_t mv = 0, ma = 0, top_mv = 0;
};
Best best_of(std::span<const std::uint32_t> pdos) {
    Best b;
    for (const std::uint32_t w : pdos) {
        const Pdo p = decode_pdo(w);
        if (p.max_mw > b.mw) { b.mw = p.max_mw; b.mv = p.max_mv; b.ma = p.max_ma; }
        if (p.max_mv > b.top_mv) b.top_mv = p.max_mv;
    }
    return b;
}

}  // namespace

// ---- feeding ----

void Analyzer::feed(std::span<const std::uint8_t> rec) {
    std::size_t i = 0;
    while (i + pdtrace::kHeader <= rec.size() && rec[i] >= pdtrace::kHeader && i + rec[i] <= rec.size()) {
        feed_record(rec[i + 1], rd32(&rec[i + 2]), rec.subspan(i + pdtrace::kHeader, rec[i] - pdtrace::kHeader));
        i += rec[i];
    }
}

void Analyzer::tick(std::uint32_t ts_us) {
    if (!have_clock_) {
        have_clock_ = true;
        last_us_ = ts_us;
        now_ms_ = ts_us / 1000;
        frac_us_ = ts_us % 1000;
        return;
    }
    const std::int32_t d = std::int32_t(ts_us - last_us_);
    if (d <= 0) return;   // a record out of order (host files): time does not run back
    last_us_ = ts_us;
    frac_us_ += std::uint32_t(d);
    now_ms_ += frac_us_ / 1000;
    frac_us_ %= 1000;
}

void Analyzer::begin(bool by_attach) {
    cur_ = Session();
    cur_.open = cur_.attached = true;
    cur_.cc_seen = by_attach;
    cur_.start_ms = cur_.end_ms = now_ms_;
    cur_.start_us = last_us_;
    frame_open_ = 0;
    pend_ = kPendNone;
    wait_psrdy_ = lost_pend_ = low_ = unplug_ = plow_ = over_on_ = false;   // win_ is a property of the input stream
    hi_ma_ = 0;
    prev_imax_ = 0;
    blk_ms_ = blk_over_ms_ = blk_over_mas_ = 0;
    mj_frac_ = mv_frac_ = cmj_frac_ = 0;
    src_pdos_ = {};
    last_pwr_ms_ = now_ms_;
    if (pre_have_) {
        cur_.have_v = true;
        v_mv_ = pre_mv_;
        i_ma_ = pre_ma_;
        v_ms_ = now_ms_;
        if (pre_mv_ > kUpMv) cur_.up_mv = pre_mv_;
    }
}

// An over-draw burst ends (draw back under the limit for kOverGapMs, the contract changed, or the session ended):
// it counts when it lasted kOverBurstMs, first to last sample above.
void Analyzer::close_burst() {
    if (!over_on_) return;
    over_on_ = false;
    Session& s = cur_;
    const std::uint32_t d = over_last_ms_ - over_on_ms_;
    if (d < kOverBurstMs) return;
    const std::uint16_t d16 = d > 0xFFFF ? 0xFFFF : std::uint16_t(d);
    if (s.bursts == 0 || d16 < s.burst_min_ms) s.burst_min_ms = d16;
    if (d16 > s.burst_max_ms) s.burst_max_ms = d16;
    inc(s.bursts);
}

void Analyzer::finish() {
    if (!cur_.open) return;
    close_burst();
    hold_to(cur_.ended ? cur_.detach_ms : now_ms_);
    cur_.end_ms = cur_.ended ? cur_.detach_ms : now_ms_;
    cur_.open = cur_.attached = false;
    last_ = cur_;
    have_last_ = true;
    finished_++;
    cur_ = Session();
}

// Time-weighted accounting of the held sample up to `ms`.
void Analyzer::hold_to(std::uint32_t ms) {
    Session& s = cur_;
    if (!s.open || !s.have_v || !s.attached) { v_ms_ = ms; return; }
    if (std::int32_t(ms - v_ms_) <= 0) return;
    const std::uint32_t dt = ms - v_ms_;
    v_ms_ = ms;
    const std::uint32_t ma = i_ma_ > 0 ? std::uint32_t(i_ma_) : 0;
    const std::uint64_t mw = std::uint64_t(v_mv_) * ma / 1000;
    s.obs_ms += dt;
    acc(s.mj, mj_frac_, mw * dt);
    acc(s.mv_s, mv_frac_, std::uint64_t(v_mv_) * dt);
    if (s.contract && ms - s.contract_ms > kSettleMs) {
        s.cobs_ms += dt;
        acc(s.cmj, cmj_frac_, mw * dt);
        if (s.c_ma && ma * 1000 >= std::uint32_t(s.c_ma) * kPinPermille) s.pin_ms += dt;
        if (s.c_ma && ma > over_limit_ma(s.c_ma)) s.over_ms += dt;
        sustain(dt, ma);
    } else {
        blk_ms_ = blk_over_ms_ = blk_over_mas_ = 0;   // a block covers one settled contract only
    }
}

static std::uint32_t sat_add(std::uint32_t a, std::uint64_t b) {
    const std::uint64_t s = std::uint64_t(a) + b;
    return s > 0xFFFFFFFFu ? 0xFFFFFFFFu : std::uint32_t(s);
}

// Settled contract time in blocks of kSustainMs: a block above over_limit_ma(c_ma) for kSustainPct of it counts.
void Analyzer::sustain(std::uint32_t dt, std::uint32_t ma) {
    Session& s = cur_;
    if (!s.c_ma) return;
    blk_ms_ = sat_add(blk_ms_, dt);
    if (ma > over_limit_ma(s.c_ma)) {
        blk_over_ms_ = sat_add(blk_over_ms_, dt);
        blk_over_mas_ = sat_add(blk_over_mas_, std::uint64_t(ma) * dt);
    }
    if (blk_ms_ < kSustainMs) return;
    if (std::uint64_t(blk_over_ms_) * 100 >= std::uint64_t(blk_ms_) * kSustainPct) {
        if (s.sus_ms == 0) s.sus_first_ms = v_ms_ - blk_ms_;
        s.sus_ms = sat_add(s.sus_ms, blk_ms_);
        s.sus_over_ms = sat_add(s.sus_over_ms, blk_over_ms_);
        s.sus_mas = sat_add(s.sus_mas, blk_over_mas_ / 1000);
        s.sus_c_ma = s.c_ma;
        s.sus_off_ma = s.c_pdo_ma;
    }
    blk_ms_ = blk_over_ms_ = blk_over_mas_ = 0;
}

// The contract is settled: PS_RDY plus kSettleMs, and no new Request, power role swap or PS_RDY wait in flight.
bool Analyzer::settled() const {
    const Session& s = cur_;
    return s.contract && now_ms_ - s.contract_ms > kSettleMs && !wait_psrdy_ && pend_ != kPendRequest && pend_ != kPendPr;
}

// The contract band: the spec's 5 % around the contract voltage.
static void band_of(std::uint16_t c_mv, std::uint32_t& lo, std::uint32_t& hi) {
    const std::uint32_t band = spec::band_mv(c_mv);
    lo = c_mv > band ? c_mv - band : 0;
    hi = c_mv + band;
}

// A capture that starts mid-session (a stream run started late, a boot onto a live bus): VBUS already up, before any
// CC or PD record, opens the session (its contract unseen). After the first session only CC and PD do.
bool Analyzer::first_vbus_opens(std::uint16_t mv) const { return mv > kUpMv && !have_last_ && finished_ == 0 && !cc_any_; }

// A ring VBUS record. It is emitted on large changes, so it stays the fast witness of VBUS loss and of the unplug; the
// draw and band evidence come from it only while the session has no power windows.
void Analyzer::sample(std::uint16_t mv, std::int16_t ma) {
    if (!win_) { pre_mv_ = mv; pre_ma_ = ma; pre_have_ = true; }
    Session& s = cur_;
    if (s.open && s.ended && mv < kLostMv) unplug_ = true;
    if (!s.open && first_vbus_opens(mv)) begin(false);
    if (!s.open || !s.attached) return;
    if (mv > kUpMv && s.up_mv == 0) s.up_mv = mv;
    if (settled() && s.c_mv && mv < kLostMv && !lost_pend_) {
        lost_pend_ = true;
        lost_pend_ms_ = now_ms_;
        lost_pend_ma_ = s.have_v ? i_ma_ : 0;
    }
    if (win_) {   // a real sample at its own moment (often between the windows' 20 Hz ones): VBUS evidence only
        band(mv, mv, ma > 0 ? std::uint16_t(ma) : 0);
        return;
    }
    hold_to(now_ms_);
    s.have_v = true;
    v_mv_ = mv;
    i_ma_ = ma;
    v_ms_ = now_ms_;
    Window w;
    w.n = 1;
    w.mv = w.vmin = w.vmax = mv;
    w.ma = w.imin = w.imax = ma;
    observe(w);
}

void Analyzer::power_window(std::uint32_t ts_us, const Window& w) {
    tick(ts_us);
    timers();
    pre_mv_ = w.mv;
    pre_ma_ = w.ma;
    pre_have_ = true;
    Session& s = cur_;
    if (s.open && s.ended && w.vmin < kLostMv) unplug_ = true;
    win_ = true;
    if (!s.open && first_vbus_opens(w.vmin)) begin(false);
    if (!s.open || !s.attached) return;
    if (w.mv > kUpMv && s.up_mv == 0) s.up_mv = w.mv;
    if (settled() && s.c_mv && w.vmin < kLostMv && !lost_pend_) {
        lost_pend_ = true;
        lost_pend_ms_ = now_ms_;
        lost_pend_ma_ = s.have_v ? i_ma_ : 0;
    }
    // the window ends now and its mean stands for the time since the previous one
    v_mv_ = w.mv;
    i_ma_ = w.ma;
    hold_to(now_ms_);
    s.have_v = true;
    v_ms_ = now_ms_;
    observe(w);
    // An input cut-out: the draw goes from a real level to nothing inside the window (or from the previous one) while
    // VBUS stays up (in band, on a contract seen). The source kept its voltage, so the device disconnected its input.
    // A contract made before the capture still has VBUS to judge by, only no band.
    const bool unseen = !s.contract_seen && !s.contract && pend_ == kPendNone;
    if ((settled() && s.c_mv) || unseen) {
        if (s.win_n < 0xFFFF) s.win_n++;
        std::uint32_t lo = kLostMv, hi;
        if (!unseen && !s.pps) band_of(s.c_mv, lo, hi);
        const bool v_up = w.vmin >= kLostMv && w.vmin >= lo;
        if (v_up && w.imin < std::int16_t(kDrawMa) &&
            (w.imax >= std::int16_t(kDropFromMa) || prev_imax_ >= std::int16_t(kDropFromMa))) {
            if (s.cut_n == 0) s.cut_ms = now_ms_;
            if (s.cut_n < 0xFFFF) s.cut_n++;
        }
    }
    prev_imax_ = w.imax;
}

// The contract band check on the extremes of one observation (`ma`: the draw at that moment).
void Analyzer::band(std::uint16_t vmin, std::uint16_t vmax, std::uint16_t ma) {
    Session& s = cur_;
    if (!settled() || !s.c_mv || vmin < kLostMv || s.pps) return;
    std::uint32_t lo, hi;
    band_of(s.c_mv, lo, hi);
    // a low reading waits kUnplugMs: VBUS falling through the band ahead of a CC detach is an unplug
    if (vmin < lo && (!plow_ || vmin < plow_mv_)) {
        if (!plow_) plow_ms_ = now_ms_;
        plow_ = true;
        plow_mv_ = vmin;
        plow_c_ = s.c_mv;
        plow_ma_ = ma;
    }
    if (vmax > hi && vmax - s.c_mv > s.high_mv - s.high_c) {
        s.high_mv = vmax; s.high_c = s.c_mv; s.high_ms = now_ms_;
    }
}

// One observation of the bus: mean and extremes (a ring record passes its single value as all three).
void Analyzer::observe(const Window& w) {
    Session& s = cur_;
    const std::uint16_t mv = w.mv, vmin = w.vmin, vmax = w.vmax;
    // IBUS is not trusted once VBUS is gone (the monitor's averaging decays for a second): current counts above kLostMv
    const bool up = vmin >= kLostMv;
    const std::uint16_t peak = w.imax > 0 && up ? std::uint16_t(w.imax) : 0;
    const std::uint16_t a = w.ma > 0 && mv >= kLostMv ? std::uint16_t(w.ma) : 0;
    if (peak > s.peak_ma) s.peak_ma = peak;
    const bool ok = settled();
    if (ok && s.c_ma && peak > over_limit_ma(s.c_ma)) {
        if (s.over_first_ms == 0) { s.over_first_ms = now_ms_ - w.span_ms; s.over_c_ma = s.c_ma; }
        if (peak > s.over_peak_ma) { s.over_peak_ma = peak; s.over_off_ma = s.c_pdo_ma; s.over_c_ma = s.c_ma; }
        // above what the source offers at all: a Fail even with VBUS holding
        if (s.c_pdo_ma && peak > over_limit_ma(s.c_pdo_ma)) s.over_fault |= 4;
        if (!over_on_) { over_on_ = true; over_on_ms_ = now_ms_ - w.span_ms; }
        over_last_ms_ = now_ms_;
    } else if (over_on_ && (!ok || now_ms_ - over_last_ms_ > kOverGapMs)) {
        close_burst();
    }
    band(vmin, vmax, peak);
    // A draw drop: the device takes less with nothing on the PD side around it (judged when it happens, on the mean).
    if (mv >= kLostMv) {
        if (!low_) {
            if (hi_ma_ >= kDropFromMa && a * 2 < hi_ma_) {
                low_ = true;
                if (s.drops == 0) { s.drop_ms = now_ms_; s.drop_from_ma = hi_ma_; s.drop_min_ma = a; }
                if (s.drops < 0xFF) s.drops++;
                if (now_ms_ - last_pwr_ms_ < kQuietMs) s.drops_quiet = false;
            } else if (a > hi_ma_) {
                hi_ma_ = a;
            }
        } else {
            if (a < s.drop_min_ma) s.drop_min_ma = a;
            if (a * 10 >= hi_ma_ * 8u) low_ = false;
        }
    }
}

bool WindowAcc::add(std::uint32_t now_ms, std::uint16_t mv, std::int16_t ma, Window& out) {
    if (w_.n == 0) {
        t0_ = now_ms;
        sum_mv_ = sum_ma_ = 0;
        w_.vmin = w_.vmax = mv;
        w_.imin = w_.imax = ma;
    }
    if (w_.n < 0xFFFF) w_.n++;
    sum_mv_ += mv;
    sum_ma_ += ma;
    if (mv < w_.vmin) w_.vmin = mv;
    if (mv > w_.vmax) w_.vmax = mv;
    if (ma < w_.imin) w_.imin = ma;
    if (ma > w_.imax) w_.imax = ma;
    if (now_ms - t0_ < kWindowMs) return false;
    out = w_;
    out.span_ms = std::uint16_t(now_ms - t0_ > 0xFFFF ? 0xFFFF : now_ms - t0_);
    out.mv = std::uint16_t(sum_mv_ / w_.n);
    out.ma = std::int16_t(sum_ma_ / w_.n);
    w_ = Window{};
    return true;
}

void Analyzer::timers() {
    Session& s = cur_;
    if (!s.open) return;
    if (s.ended && now_ms_ - s.detach_ms > kReattachMs) { finish(); return; }
    if (lost_pend_ && now_ms_ - lost_pend_ms_ > kUnplugMs) {   // CC still attached: the source cut VBUS
        lost_pend_ = false;
        if (s.lost == 0) {
            s.lost_ms = lost_pend_ms_;
            s.lost_ma = lost_pend_ma_ > 0 ? std::uint16_t(lost_pend_ma_) : 0;
            s.lost_c_ma = s.c_ma;
        }
        if (s.lost < 0xFF) s.lost++;
        if (s.over_first_ms) s.over_fault |= 1;
        s.contract = false;
    }
    if (plow_ && now_ms_ - plow_ms_ > kUnplugMs) {   // still attached: a real dip out of the band
        plow_ = false;
        if (s.low_mv == 0 || std::uint32_t(s.low_c) - s.low_mv < std::uint32_t(plow_c_) - plow_mv_) {
            s.low_mv = plow_mv_; s.low_c = plow_c_; s.low_ma = plow_ma_; s.low_ms = plow_ms_;
        }
        if (s.over_first_ms) s.over_fault |= 1;
    }
    if (pend_ != kPendNone && now_ms_ - pend_ms_ > kMissingMs) {
        if (pend_ == kPendRequest) {
            if (s.no_accept++ == 0) s.miss_ms = pend_ms_;
        } else if (const std::uint8_t b = swap_bit(pend_)) {
            s.refused |= b;   // never answered
            if (pend_ == kPendPr) { s.refused_ans = 0; s.refused_ms = pend_ms_; s.refused_mv = s.contract ? s.c_mv : v_mv_; }
            if (!pend_from_src_) s.refused_by_src |= b;
        }
        pend_ = kPendNone;
    }
    if (wait_psrdy_ && now_ms_ - accept_ms_ > kPsRdyMissingMs) {
        wait_psrdy_ = false;
        if (s.no_psrdy++ == 0 && s.miss_ms == 0) s.miss_ms = accept_ms_;
    }
}

void Analyzer::advance(std::uint32_t ts_us) {
    tick(ts_us);
    timers();
    if (cur_.open && cur_.attached) hold_to(now_ms_);
}

void Analyzer::feed_record(std::uint8_t type, std::uint32_t ts_us, std::span<const std::uint8_t> p) {
    tick(ts_us);
    timers();
    Session& s = cur_;
    switch (Type(type)) {
    case Type::Cc:
        if (p.size() < 1) return;
        cc_any_ = true;
        // a session VBUS opened (the capture started mid-session) with no PD in it yet and too short to judge: the
        // tail of a session before the capture, dropped at its first CC event
        if (s.open && !s.cc_seen && s.msgs == 0 && now_ms_ - s.start_ms < kDrawSettleMs) cur_ = Session();
        if (p[0] == 1) {   // attach
            if (s.open && s.ended && !unplug_) {   // a CC bounce with VBUS kept: the same session goes on
                s.ended = false;
                s.attached = true;
                if (s.reattaches++ == 0) s.reattach_ms = now_ms_;
                v_ms_ = now_ms_;
            } else {
                finish();
                begin(true);
            }
        } else if (s.open && !s.ended) {   // detach
            hold_to(now_ms_);
            s.ended = true;
            s.attached = false;
            s.detach_ms = now_ms_;
            unplug_ = lost_pend_ || (s.have_v && v_mv_ < kLostMv);
            lost_pend_ = plow_ = false;   // VBUS fell, then CC went: an unplug, not a cut-out or a dip
            close_burst();
            pend_ = kPendNone;
            wait_psrdy_ = false;
        }
        return;
    case Type::Vbus:
        if (p.size() >= 4) sample(std::uint16_t(p[0] | p[1] << 8), std::int16_t(p[2] | p[3] << 8));
        return;
    case Type::Rx:
        if (p.size() < 7) return;
        message(true, p[0], std::uint16_t(p[1] | p[2] << 8), p.subspan(3, p.size() - 7), rd32(&p[p.size() - 4]));
        if (cur_.open) cur_.link.feed_message(true, p);
        return;
    case Type::Tx: {
        if (p.size() < 2) return;
        const bool has_token = (p.size() - 2) % 4 == 1;
        const std::uint8_t token = has_token ? p[p.size() - 1] : 0xE0;
        const auto objs = p.subspan(2, p.size() - 2 - (has_token ? 1 : 0));
        message(false, token, std::uint16_t(p[0] | p[1] << 8), objs, 0);
        if (cur_.open) cur_.link.feed_message(false, p.first(p.size() - (has_token ? 1 : 0)));
        return;
    }
    case Type::TxResult:
        if (s.open && p.size() >= 1 && p[0] != 0) inc(s.tx_fail);
        return;
    case Type::Reset:
        if (p.size() < 1 || p[0] > 1) return;   // soft resets count from the Soft_Reset messages (SOP only)
        if (!s.open) begin(false);
        if (cur_.hard++ == 0) cur_.hard_ms = now_ms_;
        if (cur_.over_first_ms) cur_.over_fault |= 2;
        cur_.contract = false;
        pend_ = kPendNone;
        wait_psrdy_ = false;
        last_pwr_ms_ = now_ms_;
        return;
    case Type::RxBad:
        if (s.open) inc(s.rx_bad);
        return;
    case Type::Mark:
        if (p.size() >= 1 && p[0] == pdtrace::kMarkBoot) { finish(); pre_have_ = false; }
        return;
    case Type::Sink:
        // The K2's own sink went back to idle (a re-read or a test step released its contract): the source then
        // drops VBUS by design, which is not a cut-out or a dip.
        if (s.open && s.active && p.size() >= 1 && p[0] == std::uint8_t(SinkState::Idle) && s.contract) {
            s.contract = false;
            lost_pend_ = plow_ = false;
        }
        return;
    case Type::RxRepeat:
        if (s.open && p.size() >= 2) s.caps = std::uint16_t(s.caps + (p[0] | p[1] << 8));
        return;
    default:
        return;
    }
}

void Analyzer::contract_from_rdo() {
    Session& s = cur_;
    const unsigned pos = s.rdo >> 28 & 0xF;
    s.c_mv = s.c_ma = s.c_pdo_ma = 0;
    s.pps = false;
    if (pos == 0 || pos > s.n_src) return;
    const Pdo pdo = decode_pdo(src_pdos_[pos - 1]);
    const Rdo r = decode_rdo(s.rdo, &pdo);
    s.c_mv = r.mv;
    s.c_ma = r.op_ma;
    s.c_pdo_ma = pdo.max_ma;
    s.pps = pdo.type == PdoType::Pps || pdo.type == PdoType::Avs;
}

void Analyzer::message(bool rx, std::uint8_t token, std::uint16_t hdr, std::span<const std::uint8_t> objs, std::uint32_t crc) {
    const unsigned sop = (token & 0xE0) == 0xE0 ? 0 : (token & 0xE0) == 0xC0 ? 1 : (token & 0xE0) == 0xA0 ? 2 : 3;
    if (sop > 2) return;
    const unsigned type = hdr & 0x1F, n = hdr >> 12 & 7, id = hdr >> 9 & 7;
    const bool ext = hdr >> 15 & 1, from_src = hdr >> 8 & 1, dfp = hdr >> 5 & 1;
    if (objs.size() < 4 * n) return;
    if (!cur_.open) begin(false);
    Session& s = cur_;
    if (!rx) s.active = true;

    if (!ext && n == 0 && type == kGoodCrc) {
        if ((frame_open_ >> sop & 1) && (frame_[sop].hdr >> 9 & 7) == id) frame_open_ &= std::uint8_t(~(1u << sop));
        if (sop == 0 && !from_src) s.sink_answered = true;
        if (sop != 0 && from_src) s.cable_answered = true;   // the cable plug acknowledged
        return;
    }
    if (rx) {   // retries and frames left without GoodCRC (both ends' GoodCRC are on the wire when the K2 is passive)
        Frame& f = frame_[sop];
        const std::uint16_t ms16 = std::uint16_t(now_ms_);
        if (frame_open_ >> sop & 1) {
            if (f.hdr == hdr && f.crc == crc && std::uint16_t(ms16 - f.ms16) <= kRetryMs) {
                inc(sop == 0 ? s.retries : s.cable_frames);
                f.ms16 = ms16;
                return;   // the same message again: its meaning was taken the first time
            }
            // Source_Capabilities before the sink listens go unacknowledged by design (the source repeats them; the
            // caps-unanswered rule covers a sink that never answers): not a link fault
            const bool early_caps = (f.hdr & 0x1F) == kSrcCaps && !(f.hdr >> 15 & 1) && (f.hdr >> 12 & 7) &&
                                    !s.contract_seen;
            if (sop == 0 && !s.active && !early_caps) inc(s.no_goodcrc);
        }
        f.hdr = hdr;
        f.crc = crc;
        f.ms16 = ms16;
        frame_open_ |= std::uint8_t(1u << sop);
    }
    const std::uint8_t* o = objs.data();

    if (sop != 0) {   // the cable: SOP' / SOP''
        if (from_src) {   // the cable plug speaks
            s.cable_answered = true;
            if (!ext && n >= 1 && type == kVdm) {
                const std::uint32_t v = rd32(o);
                // a plug may NAK with the PD SID: the mode is the one the port asked it to enter
                const unsigned svid = (v >> 16) == kSvidPd ? cable_mode_svid_ : v >> 16, ctype = v >> 6 & 3;
                if ((v >> 15 & 1) && (v & 0x1F) == 4 && (ctype == 2 || ctype == 3)) {   // Enter_Mode NAK / BUSY
                    if (!(s.mode_nak & 0x70)) s.cable_nak_ms = now_ms_;
                    s.mode_nak |= std::uint8_t((svid == kSvidDp ? 1 : svid == kSvidTbt ? 2 : 4) << 4);
                }
            }
            return;
        }
        inc(s.cable_frames);
        if (!ext && n >= 1 && type == kVdm) {
            const std::uint32_t v = rd32(o);
            if ((v >> 15 & 1) && (v >> 6 & 3) == 0 && (v & 0x1F) == 1) inc(s.cable_tries);
            if ((v >> 15 & 1) && (v >> 6 & 3) == 0 && (v & 0x1F) == 4) cable_mode_svid_ = std::uint16_t(v >> 16);
        }
        return;
    }

    if (s.msgs < 0xFFFF) s.msgs++;
    if (!from_src) s.sink_answered = true;
    if (from_src) s.src_host = dfp;
    else s.snk_host = dfp;

    // An answer to the pending request (from the other side).
    if (!ext && n == 0 && pend_ != kPendNone && from_src != (pend_from_src_ != 0) &&
        (type == kAccept || type == kReject || type == kWait || type == kNotSupported)) {
        const std::uint8_t kind = pend_;
        pend_ = kPendNone;
        last_pwr_ms_ = now_ms_;
        if (kind == kPendRequest) {
            if (type == kAccept) {
                const std::uint32_t dt = now_ms_ - pend_ms_;
                if (dt > spec::kAcceptLimitMs + kAnswerSlackMs && dt > s.late_accept_ms) s.late_accept_ms = std::uint16_t(dt);
                wait_psrdy_ = true;
                accept_ms_ = now_ms_;
            } else if (type == kWait) {
                if (s.waits < 0xFF) s.waits++;
            } else {
                if (s.rejects++ == 0) s.reject_ms = now_ms_;
            }
        } else if (kind == kPendEusb) {
            if (type != kAccept) s.eusb_refused = true;
        } else if (const std::uint8_t b = swap_bit(kind)) {
            if (type == kAccept) {
                if (kind == kPendPr) {   // the roles turn: what was the sink's is the source's now
                    s.contract = false;
                    s.n_src = 0;
                    s.offer_mw = s.offer_ma = s.offer_top_mv = 0;
                    s.n_snk = 0;
                    s.snk_mw = s.snk_mv = s.snk_ma = s.snk_top_mv = 0;
                    s.snk_higher = false;
                    s.snk_vid = s.snk_pid = 0;
                    const std::uint32_t t = s.snk_idh;
                    s.snk_idh = s.src_idh;
                    s.src_idh = t;
                    const bool h = s.snk_host;
                    s.snk_host = s.src_host;
                    s.src_host = h;
                }
            } else {
                s.refused |= b;
                if (from_src) s.refused_by_src |= b;
                if (kind == kPendPr) {
                    s.refused_ans = std::uint8_t(type);
                    s.refused_ms = now_ms_;
                    s.refused_mv = s.contract ? s.c_mv : v_mv_;
                }
            }
        }
        return;
    }

    if (!ext && n > 0) {
        switch (type) {
        case kSrcCaps:
            if (!from_src) return;
            inc(s.caps);
            s.n_src = std::uint8_t(n);
            for (unsigned i = 0; i < 7; i++) src_pdos_[i] = i < n ? rd32(o + 4 * i) : 0;
            {
                const Best b = best_of({src_pdos_.data(), n});
                s.offer_mw = b.mw;
                s.offer_ma = b.ma;
                s.offer_top_mv = b.top_mv;
            }
            if (pend_ == kPendRequest) { if (s.no_accept++ == 0) s.miss_ms = pend_ms_; pend_ = kPendNone; }
            if (wait_psrdy_) { wait_psrdy_ = false; if (s.no_psrdy++ == 0 && s.miss_ms == 0) s.miss_ms = accept_ms_; }
            last_pwr_ms_ = now_ms_;
            return;
        case kRequest:
        case kEprRequest: {
            if (from_src) return;
            if (pend_ == kPendRequest) { if (s.no_accept++ == 0) s.miss_ms = pend_ms_; }
            s.rdo = rd32(o);
            s.rdo_ms = now_ms_;
            s.epr = type == kEprRequest;   // RDO bit 22 is "EPR mode capable", set on SPR requests too
            if (s.rdo >> 26 & 1) s.mismatch = true;
            const unsigned pos = s.rdo >> 28 & 0xF;
            const bool pps = pos >= 1 && pos <= s.n_src && (src_pdos_[pos - 1] >> 30) == 3;
            if (!pps && rx) inc(s.req_fixed);   // the K2's own Requests (an active test script) are not the device's
            pend_ = kPendRequest;
            pend_from_src_ = 0;
            pend_ms_ = now_ms_;
            last_pwr_ms_ = now_ms_;
            return;
        }
        case kSnkCaps:
            if (from_src) return;   // the source's own sink side, never the device's need
        {
            std::array<std::uint32_t, 7> pdos{};
            for (unsigned i = 0; i < n; i++) pdos[i] = rd32(o + 4 * i);
            const Best b = best_of({pdos.data(), n});
            s.n_snk = std::uint8_t(n);
            s.snk_higher = pdos[0] >> 28 & 1;
            s.snk_mw = b.mw;
            s.snk_mv = b.mv;
            s.snk_ma = b.ma;
            s.snk_top_mv = b.top_mv;
            return;
        }
        case kEnterUsb:
            pend_ = kPendEusb;
            pend_from_src_ = from_src;
            pend_ms_ = now_ms_;
            return;
        case kEprMode: {
            const std::uint32_t v = rd32(o);
            if ((v >> 24 & 0xFF) == 4) { s.epr_failed = true; s.epr_reason = std::uint8_t(v >> 16); }
            return;
        }
        case kVdm: {
            const std::uint32_t v = rd32(o);
            if (!(v >> 15 & 1)) return;
            const unsigned svid = v >> 16, ctype = v >> 6 & 3, cmd = v & 0x1F;
            const std::uint32_t key = svid << 8 | cmd;
            VdmKey* k = nullptr;
            for (auto& e : s.vdm) if (e.count && e.key == key) k = &e;
            if (ctype == 0) {
                if (!k) {
                    k = &s.vdm[0];
                    for (auto& e : s.vdm) if (e.count < k->count) k = &e;
                    *k = VdmKey{};
                    k->key = key;
                } else if (now_ms_ - k->last_ms <= kVdmRepeatMs) {
                    inc(k->fast);
                }
                inc(k->count);
                k->last_ms = now_ms_;
            } else if (k) {
                k->answer = std::uint8_t(ctype);
            }
            if (ctype == 1 && cmd == 1 && svid == kSvidPd && n >= 4) {
                const std::uint32_t idh = rd32(o + 4), prod = rd32(o + 12);
                if (from_src) s.src_idh = idh;
                else { s.snk_idh = idh; s.snk_vid = std::uint16_t(idh); s.snk_pid = std::uint16_t(prod >> 16); }
            }
            if (cmd == 4 && (ctype == 2 || ctype == 3)) {
                if (!(s.mode_nak & 7)) s.mode_nak_ms = now_ms_;
                s.mode_nak |= svid == kSvidDp ? 1 : svid == kSvidTbt ? 2 : 4;
            }
            return;
        }
        default:
            return;
        }
    }
    if (ext) {
        if (type == kExtSkedb && !from_src && objs.size() >= 2 + 21) {
            const unsigned eh = o[0] | o[1] << 8;
            if ((eh >> 11 & 0xF) == 0 && !(eh >> 10 & 1)) {
                s.skedb_op_w = o[2 + 19];
                s.skedb_max_w = o[2 + 20];
            }
        }
        return;
    }
    // control messages
    switch (type) {
    case kNotSupported: {   // also the answer to a VDM request
        for (auto& e : s.vdm) if (e.count && e.answer == 0) e.answer = 4;
        return;
    }
    case kPsRdy:
        if (wait_psrdy_ && from_src) {
            wait_psrdy_ = false;
            const std::uint32_t dt = now_ms_ - accept_ms_;
            const std::uint32_t lim = (s.epr ? spec::kPsTransitionEprMaxMs : spec::kPsTransitionMaxMs) + kAnswerSlackMs;
            if (dt > lim && dt > s.late_psrdy_ms) s.late_psrdy_ms = std::uint16_t(dt);
            s.contract = s.contract_seen = true;
            s.contract_ms = now_ms_;
            contract_from_rdo();
            last_pwr_ms_ = now_ms_;
        }
        return;
    case kPrSwap: case kDrSwap: case kVconnSwap: case kFrSwap:
        pend_ = type == kPrSwap ? kPendPr : type == kDrSwap ? kPendDr : type == kVconnSwap ? kPendVconn : kPendFr;
        pend_from_src_ = from_src;
        pend_ms_ = now_ms_;
        if (type == kPrSwap || type == kFrSwap) last_pwr_ms_ = now_ms_;
        return;
    case kSoftReset:
        if (s.soft++ == 0) s.soft_ms = now_ms_;
        if (!rx) s.soft_k2++;
        pend_ = kPendNone;
        wait_psrdy_ = false;
        last_pwr_ms_ = now_ms_;
        return;
    default:
        return;
    }
}

// ---- evaluation ----

namespace {

constexpr std::uint8_t kPowerShort = 1, kPowerOk = 2, kPowerUnknown = 3, kPowerUnseen = 4, kPowerNone = 5, kPowerActive = 6,
                       kPowerNoPd = 7, kPowerOver = 8;

void add(Report& r, Sev sev, Group g, Code c, std::uint32_t t, std::int32_t a = 0, std::int32_t b = 0, std::int32_t cc = 0,
         std::int32_t d = 0) {
    if (r.n >= kMaxFindings) return;
    r.f[r.n++] = Finding{sev, g, c, t, a, b, cc, d};
}

// The source's noun from its ID header product types (UFP bits 29:27, DFP bits 25:23), else its data role.
const char* src_noun(std::uint32_t idh, bool host) {
    const unsigned ufp = idh >> 27 & 7, dfp = idh >> 23 & 7;
    if (dfp == 3) return "charger";
    if (ufp == 1 || dfp == 1) return "hub";
    if (dfp == 2 || host) return "host";
    return "source";
}

}  // namespace

Report evaluate(const Session& s, const TwinHint* twin, std::uint32_t now_ms) {
    Report r;
    if (!s.open && s.start_ms == 0 && s.end_ms == 0 && !s.cc_seen && s.msgs == 0) return r;   // nothing yet
    r.empty = false;
    // a twin for another device than the one that identified as the sink here does not describe this sink
    if (twin && twin->vid && s.snk_vid && (twin->vid != s.snk_vid || (twin->pid && s.snk_pid && twin->pid != s.snk_pid)))
        twin = nullptr;
    r.live = s.open;
    r.active = s.active;
    r.start_us = s.start_us;
    const std::uint32_t end = s.open ? (s.ended ? s.detach_ms : now_ms) : s.end_ms;
    r.dur_ms = end - s.start_ms;
    r.lanes = s.link.link();
    if (s.eusb_refused) r.lanes.eusb = false;   // seen, not agreed
    // PD seen: any SOP message, a reset, or the cable asked on SOP'; VBUS above 5 V means a contract was made earlier
    r.no_pd = s.msgs == 0 && s.caps == 0 && s.hard == 0 && s.cable_frames == 0 && s.up_mv == 0;
    auto at = [&](std::uint32_t ms) { return ms >= s.start_ms ? ms - s.start_ms : 0; };

    r.offer_mw = s.offer_mw;
    const std::uint16_t off_ma = s.offer_ma;
    // need, in trust order
    if (s.n_snk && s.snk_mw) {
        r.need_mw = s.snk_mw;
        r.need_mv = s.snk_mv;
        r.need_ma = s.snk_ma;
        r.need_from = NeedFrom::SinkCaps;
    } else if (s.skedb_op_w || s.skedb_max_w) {
        r.need_mw = 1000u * (s.skedb_op_w ? s.skedb_op_w : s.skedb_max_w);
        r.need_from = NeedFrom::Skedb;
    } else if (twin && !twin->pdos.empty()) {
        const Best b = best_of(twin->pdos);
        r.need_mw = b.mw;
        r.need_mv = b.mv;
        r.need_ma = b.ma;
        r.need_from = twin->assumed ? NeedFrom::AssumedTwin : NeedFrom::Twin;
        r.twin = twin->name;
    }
    if (r.need_mw == 0) r.need_from = NeedFrom::None;
    const bool host = s.snk_host || (s.snk_idh >> 31 & 1) || (twin && twin->host);
    r.snk_noun = s.active ? "K2" : host && r.need_mw >= 45000 ? "Laptop" : host ? "Host" : "Device";
    r.src_noun = src_noun(s.src_idh, s.src_host);
    const std::uint32_t mean_mv = s.obs_ms ? std::uint32_t(std::uint64_t(s.mv_s) * 1000 / s.obs_ms) : 0;
    r.vbus_mv = std::uint16_t(mean_mv);
    r.peak_ma = s.peak_ma;

    // -- power --
    const bool short_power = !s.active && r.need_mw && r.offer_mw && r.need_mw > r.offer_mw;
    if (short_power)
        add(r, Sev::Fail, Group::Power, Code::PowerShort, at(s.rdo_ms), std::int32_t(r.need_mw), std::int32_t(r.offer_mw),
            r.need_mv, r.need_ma);
    if (!s.active && s.contract_seen && s.mismatch)
        add(r, Sev::Warn, Group::Power, Code::MismatchFlag, at(s.rdo_ms));
    else if (short_power && s.contract_seen)
        add(r, Sev::Info, Group::Power, Code::NoMismatchFlag, at(s.rdo_ms), s.c_mv, s.c_ma);
    if (s.active) {
        if (s.contract_seen)
            add(r, Sev::Info, Group::Power, Code::ActiveSink, at(s.contract_ms), s.c_mv, s.c_ma, std::int32_t(s.rdo >> 28 & 0xF));
    } else if (s.contract_seen) {
        // sustained (a share of the contract time) or in bursts (each kOverBurstMs or longer)
        const bool over = s.over_c_ma && ((s.over_ms >= kOverMinMs &&
                                           std::uint64_t(s.over_ms) * 100 >= std::uint64_t(s.cobs_ms) * kOverSharePct) ||
                                          s.bursts);
        if (over) {
            // c: share % | bursts << 8 | offer at the peak (10 mA) << 16
            // d: VBUS fell / hard reset bits | shortest burst (0.1 s) << 2 | longest << 12 | above the offer << 22
            const auto ds = [](std::uint32_t ms) { return std::uint32_t(ms / 100 > 999 ? 999 : ms / 100); };
            const std::uint32_t share = s.cobs_ms ? std::uint32_t(std::uint64_t(s.over_ms) * 100 / s.cobs_ms) : 0;
            add(r, s.over_fault ? Sev::Fail : Sev::Warn, Group::Power, Code::OverDraw, at(s.over_first_ms), s.over_peak_ma,
                s.over_c_ma,
                std::int32_t((share > 100 ? 100 : share) | std::uint32_t(s.bursts) << 8 |
                             std::uint32_t(s.over_off_ma / 10) << 16),
                std::int32_t((s.over_fault & 3) | ds(s.burst_min_ms) << 2 | ds(s.burst_max_ms) << 12 |
                             std::uint32_t(s.over_fault >> 2 & 1) << 22));
        }
        // kSustainMs blocks mostly above the contract: a Fail when the level is above the offer too, or with VBUS
        // falling / a reset. a: the mean while above, c: offer | share % << 16, d: the blocks' time (ms)
        if (s.sus_ms && s.sus_over_ms) {
            const std::uint32_t lvl = std::uint32_t(std::uint64_t(s.sus_mas) * 1000 / s.sus_over_ms);
            const std::uint32_t share = std::uint32_t(std::uint64_t(s.sus_over_ms) * 100 / s.sus_ms);
            const bool far = s.sus_off_ma && lvl > over_limit_ma(s.sus_off_ma);
            add(r, far || (s.over_fault & 3) ? Sev::Fail : Sev::Warn, Group::Power, Code::OverSustained,
                at(s.sus_first_ms), std::int32_t(lvl), s.sus_c_ma, std::int32_t(s.sus_off_ma | share << 16),
                std::int32_t(s.sus_ms));
        }
        if (!over && !s.sus_ms && s.c_ma && s.cobs_ms >= kDrawSettleMs &&
            std::uint64_t(s.pin_ms) * 100 >= std::uint64_t(s.cobs_ms) * kPinSharePct)
            add(r, Sev::Warn, Group::Power, Code::Pinned, at(s.contract_ms), s.c_ma, std::int32_t(s.pin_ms * 100 / s.cobs_ms),
                std::int32_t(s.cobs_ms));
        const std::int32_t mw = s.cobs_ms ? std::int32_t(std::uint64_t(s.cmj) * 1000 / s.cobs_ms) : 0;
        if (s.cobs_ms > 0)
            add(r, Sev::Info, Group::Power, Code::Draw, at(s.contract_ms), s.c_mv, s.c_ma, mw, std::int32_t(s.cobs_ms));
        // a 5 V contract while the device declares more and the offer has more
        const bool snk_more = s.n_snk && (s.snk_higher || s.snk_top_mv > kUpMv);
        if (s.c_mv && s.c_mv <= kUpMv && snk_more && s.offer_top_mv > kUpMv)
            add(r, Sev::Warn, Group::Power, Code::FiveVoltOnly, at(s.contract_ms), s.offer_top_mv,
                s.snk_top_mv > kUpMv ? s.snk_top_mv : s.offer_top_mv);
    } else if (s.caps || s.rdo) {   // an offer (or a Request) on this leg, and no Accept + PS_RDY after it
        add(r, Sev::Info, Group::Power, Code::NoContract, 0, std::int32_t(mean_mv));
    } else {
        // PD traffic (swaps, VDMs: they need a contract) or VBUS above 5 V with no offer seen: the contract was
        // made before the K2 attached
        if (!r.no_pd)
            add(r, Sev::Info, Group::Power, Code::ContractUnseen, 0, s.up_mv ? s.up_mv : std::int32_t(mean_mv),
                s.up_mv || s.obs_ms ? 1 : 0);
        if (s.obs_ms && mean_mv >= kLostMv)   // (no VBUS: a charger waiting for a sink, nothing to draw)
            add(r, Sev::Info, Group::Power, Code::Draw, 0, 0, 0, std::int32_t(std::uint64_t(s.mj) * 1000 / s.obs_ms),
                std::int32_t(s.obs_ms));
    }
    // input cut-outs: the device's draw falls to nothing inside a window while VBUS holds in band (the source kept
    // its voltage, so the device let go); they make the separate "reduced its draw" line moot
    const bool cuts = !s.active && s.cut_n >= kCutMin;
    if (cuts)
        add(r, Sev::Warn, Group::Power, Code::InputCuts, at(s.cut_ms), s.cut_n, s.win_n);
    if (!s.active && s.drops && !cuts)
        add(r, Sev::Info, Group::Power, Code::DrawReduced, at(s.drop_ms), s.drop_from_ma, s.drop_min_ma, s.drops,
            s.drops_quiet ? 1 : 0);

    // -- contract instability --
    const unsigned n0 = r.n;
    if (s.hard) add(r, Sev::Fail, Group::Contract, Code::HardReset, at(s.hard_ms), s.hard);
    if (s.reattaches) add(r, Sev::Warn, Group::Contract, Code::Reattach, at(s.reattach_ms), s.reattaches);
    if (s.caps >= 3 && !s.sink_answered) add(r, Sev::Fail, Group::Contract, Code::CapsUnanswered, 0, s.caps);
    if (s.soft) add(r, s.soft > 2 ? Sev::Fail : Sev::Warn, Group::Contract, Code::SoftReset, at(s.soft_ms), s.soft, s.soft_k2);
    if (s.rejects) add(r, Sev::Warn, Group::Contract, Code::Rejected, at(s.reject_ms), s.rejects);
    if (s.waits >= 2) add(r, Sev::Warn, Group::Contract, Code::WaitLoop, 0, s.waits);
    if (s.no_accept) add(r, Sev::Warn, Group::Contract, Code::NoAccept, at(s.miss_ms), s.no_accept);
    if (s.no_psrdy) add(r, Sev::Warn, Group::Contract, Code::NoPsRdy, at(s.miss_ms), s.no_psrdy);
    if (s.late_accept_ms) add(r, Sev::Warn, Group::Contract, Code::LateAccept, 0, s.late_accept_ms);
    if (s.late_psrdy_ms) add(r, Sev::Warn, Group::Contract, Code::LatePsRdy, 0, s.late_psrdy_ms, s.epr ? 1 : 0);
    if (s.req_fixed >= kManyRequests) add(r, Sev::Warn, Group::Contract, Code::ManyRequests, 0, s.req_fixed);
    r.quiet_contract = r.n == n0;

    // -- power quality --
    const unsigned n1 = r.n;
    if (s.lost)
        add(r, Sev::Fail, Group::Quality, Code::VbusLost, at(s.lost_ms), s.lost, s.lost_ma, s.lost_c_ma);
    if (s.low_mv) add(r, Sev::Warn, Group::Quality, Code::VbusLow, at(s.low_ms), s.low_mv, s.low_c, s.low_ma);
    if (s.high_mv) add(r, Sev::Warn, Group::Quality, Code::VbusHigh, at(s.high_ms), s.high_mv, s.high_c);
    r.quiet_quality = r.n == n1;
    r.band_checked = s.contract_seen && s.cobs_ms > 0 && !s.pps;

    // -- link quality --
    const unsigned n2 = r.n;
    if (s.no_goodcrc) add(r, s.no_goodcrc >= 3 ? Sev::Warn : Sev::Info, Group::Link, Code::NoGoodCrc, 0, s.no_goodcrc);
    if (s.retries) add(r, s.retries >= 3 ? Sev::Warn : Sev::Info, Group::Link, Code::Retries, 0, s.retries);
    if (s.tx_fail) add(r, Sev::Warn, Group::Link, Code::TxFail, 0, s.tx_fail);
    if (s.rx_bad) add(r, s.rx_bad >= 3 ? Sev::Warn : Sev::Info, Group::Link, Code::RxBad, 0, s.rx_bad);
    r.quiet_link = r.n == n2;

    // -- roles and modes --
    const unsigned n3 = r.n;
    for (std::uint8_t b = 1; b <= 8; b <<= 1)
        if (s.refused & b)
            add(r, Sev::Info, Group::Roles, Code::SwapRefused, b == 1 ? at(s.refused_ms) : 0, b, (s.refused_by_src & b) ? 1 : 0,
                b == 1 ? s.refused_ans : 0, b == 1 ? s.refused_mv : 0);
    if (s.mode_nak & 7) add(r, Sev::Warn, Group::Roles, Code::ModeNak, at(s.mode_nak_ms), s.mode_nak & 7);
    // the cable plug NAKs its own Enter_Mode (SOP'): the port's mode is agreed on SOP, so this alone is Info
    if (s.mode_nak & 0x70) add(r, Sev::Info, Group::Roles, Code::ModeNak, at(s.cable_nak_ms), s.mode_nak >> 4 & 7, 1);
    if (s.eusb_refused) add(r, Sev::Warn, Group::Roles, Code::EnterUsbRefused, 0);
    if (s.epr_failed) add(r, Sev::Warn, Group::Roles, Code::EprFailed, 0, s.epr_reason);
    const auto& L = s.link.link();
    // a 3 A e-marker while the device needs more current than that: the cable caps the offer (sources hold 5 A back)
    if (L.cable_emarker && L.cable_amps == 3 && r.need_ma > 3000 && off_ma <= 3000)
        add(r, short_power ? Sev::Warn : Sev::Info, Group::Roles, Code::CableCap, 0, r.need_ma);
    {
        const VdmKey* top = nullptr;
        for (const auto& e : s.vdm) if (e.fast >= kVdmLoopMin && (!top || e.count > top->count)) top = &e;
        if (top) add(r, Sev::Info, Group::Roles, Code::VdmLoop, 0, std::int32_t(top->key), top->count, top->answer);
    }
    if (s.cable_tries && !s.cable_answered) add(r, Sev::Info, Group::Roles, Code::CableSilent, 0, s.cable_frames, s.cable_tries);
    r.quiet_roles = r.n == n3;

    // rank: Fail, Warn, Info (stable: the group order stays inside a severity)
    for (unsigned i = 1; i < r.n; i++)
        for (unsigned j = i; j > 0 && r.f[j].sev < r.f[j - 1].sev; j--) { const Finding t = r.f[j]; r.f[j] = r.f[j - 1]; r.f[j - 1] = t; }

    bool unstable = false, degraded = false;
    for (unsigned i = 0; i < r.n; i++) {
        const Finding& f = r.f[i];
        if (f.group == Group::Contract || f.group == Group::Quality || f.group == Group::Link) {
            if (f.sev == Sev::Fail) unstable = true;
            else if (f.sev == Sev::Warn) degraded = true;
        }
    }
    r.link = unstable ? 2 : degraded ? 1 : 0;
    bool over = false;
    for (unsigned i = 0; i < r.n; i++) over = over || r.f[i].code == Code::OverDraw || r.f[i].code == Code::OverSustained;
    r.power = r.no_pd ? kPowerNoPd
              : s.active ? kPowerActive
              : short_power ? kPowerShort
              : over ? kPowerOver
              : (s.contract_seen && r.need_mw && r.offer_mw) ? kPowerOk
              : s.contract_seen ? kPowerUnknown
              : (s.caps || s.rdo) ? kPowerNone
                                  : kPowerUnseen;
    return r;
}

// ---- text ----

namespace {

struct Out {
    std::span<char> b;
    std::size_t n = 0;
    void c(char ch) { if (n + 1 < b.size()) { b[n++] = ch; b[n] = 0; } }
    void s(const char* t) { while (t && *t) c(*t++); }
    void cap(const char* t) {   // capitalised first letter
        if (!t || !*t) return;
        c(*t >= 'a' && *t <= 'z' ? char(*t - 32) : *t);
        s(t + 1);
    }
    void u(std::uint32_t v) {
        char t[10];
        int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) c(t[--k]);
    }
    // milli-units with up to `dec` decimals, trailing zeros dropped ("20", "3.25", "19.3")
    void milli(std::int32_t v, int dec) {
        if (v < 0) { c('-'); v = -v; }
        std::uint32_t scale = dec >= 2 ? 10 : dec == 1 ? 100 : 1000;
        const std::uint32_t q = (std::uint32_t(v) + scale / 2) / scale;   // in units of 10^-dec
        const std::uint32_t div = dec >= 2 ? 100 : dec == 1 ? 10 : 1;
        u(q / div);
        std::uint32_t f = q % div;
        if (f == 0 || div == 1) return;
        c('.');
        if (div == 100) { c(char('0' + f / 10)); if (f % 10) c(char('0' + f % 10)); }
        else c(char('0' + f));
    }
    void volts(std::int32_t mv) { milli(mv, 1); s(" V"); }
    void volts2(std::int32_t mv) { milli(mv, 2); s(" V"); }   // a band edge: 18.99 V, not 19 V
    void amps(std::int32_t ma) {
        if (ma < 100) { u(std::uint32_t(ma < 0 ? 0 : ma)); s(" mA"); return; }
        milli(ma, 2);
        s(" A");
    }
    void watts(std::int32_t mw) {
        if (mw < 10000) milli(mw, 1); else u(std::uint32_t(mw + 500) / 1000);
        s(" W");
    }
    void clock(std::uint32_t ms) {   // m:ss, or h:mm:ss
        const std::uint32_t t = ms / 1000;
        if (t >= 3600) { u(t / 3600); c(':'); const std::uint32_t m = t / 60 % 60; c(char('0' + m / 10)); c(char('0' + m % 10)); }
        else u(t / 60);
        c(':');
        c(char('0' + t % 60 / 10));
        c(char('0' + t % 10));
    }
    void dur(std::uint32_t ms) {
        const std::uint32_t t = ms / 1000;
        if (t < 60) { u(t); s(" s"); return; }
        if (t < 3600) { u(t / 60); s(" min "); u(t % 60); s(" s"); return; }
        u(t / 3600); s(" h "); u(t / 60 % 60); s(" min");
    }
};

const char* answer_name(unsigned a) {
    switch (a) {
    case 1: return "ACK";
    case 2: return "NAK";
    case 3: return "BUSY";
    case 4: return "Not_Supported";
    default: return "nothing";
    }
}
const char* ctl_answer(unsigned t) {
    return t == kReject ? "rejected" : t == kWait ? "answered Wait" : t == kNotSupported ? "answered Not_Supported" : "not answered";
}
const char* vdm_cmd(unsigned key) {
    const unsigned cmd = key & 0x1F, svid = key >> 8;
    switch (cmd) {
    case 1: return "Discover_Identity";
    case 2: return "Discover_SVIDs";
    case 3: return "Discover_Modes";
    case 4: return "Enter_Mode";
    case 5: return "Exit_Mode";
    case 6: return "Attention";
    case 16: return svid == kSvidDp ? "DP Status" : "VDM 16";
    case 17: return svid == kSvidDp ? "DP Configure" : "VDM 17";
    default: return "VDM request";
    }
}

void title(const Report& r, const Finding& f, Out& o) {
    switch (f.code) {
    case Code::PowerShort: o.cap(r.snk_noun); o.s(" needs "); o.watts(f.a); o.s(", "); o.s(r.src_noun); o.s(" gives "); o.watts(f.b); o.s(" max"); break;
    case Code::MismatchFlag: o.cap(r.snk_noun); o.s(" flagged capability mismatch"); break;
    case Code::NoMismatchFlag: o.cap(r.snk_noun); o.s(" did not flag capability mismatch"); break;
    case Code::OverDraw:
        o.cap(r.snk_noun); o.s(f.d >> 22 & 1 ? " draws far above its contract: up to " : " draws above its contract: up to ");
        o.amps(f.a); o.s(" on a "); o.amps(f.b); o.s(" contract");
        break;
    case Code::InputCuts: o.cap(r.snk_noun); o.s("'s input cuts out repeatedly"); break;
    case Code::OverSustained:   // "Laptop stays over its contract, at 3.89 A on 3.25 A" (the level while over)
        o.cap(r.snk_noun); o.s(" stays over its contract, at "); o.amps(f.a); o.s(" on "); o.amps(f.b);
        break;
    case Code::Pinned: o.s("Draw pinned at the "); o.amps(f.a); o.s(" contract limit"); break;
    case Code::Draw:
        if (f.a) { o.s("Contract "); o.volts(f.a); o.c(' '); o.amps(f.b); o.s(", draws "); o.watts(f.c); }
        else { o.s("Draws "); o.watts(f.c); o.s(" at "); o.volts(r.vbus_mv); }
        break;
    case Code::DrawReduced: o.cap(r.snk_noun); o.s(f.d ? " reduced its draw, contract unchanged" : " drew less, with PD traffic around it"); break;
    case Code::FiveVoltOnly: o.s("5 V contract, "); o.s(r.snk_noun[0] == 'K' ? "K2" : "the device"); o.s(" can use "); o.volts(f.b > f.a ? f.a : f.b); break;
    case Code::ContractUnseen: o.s("Contract not observed (the K2 attached after it)"); break;
    case Code::NoContract: o.s("No contract made, VBUS "); o.volts(f.a); break;
    case Code::ActiveSink:
        o.s("K2 is the sink: ");
        if (f.a) { o.volts(f.a); o.c(' '); o.amps(f.b); o.s(" contract"); }
        else { o.s("contract on PDO "); o.u(std::uint32_t(f.c)); }   // its PDO is not in the captured offer
        break;
    case Code::HardReset: o.u(std::uint32_t(f.a)); o.s(f.a == 1 ? " hard reset" : " hard resets"); break;
    case Code::SoftReset: o.u(std::uint32_t(f.a)); o.s(f.a == 1 ? " soft reset on SOP" : " soft resets on SOP"); break;
    case Code::Reattach: o.s("Re-attached "); o.u(std::uint32_t(f.a)); o.s("x within 3 s"); break;
    case Code::Rejected: o.s("Request rejected "); o.u(std::uint32_t(f.a)); o.c('x'); break;
    case Code::WaitLoop: o.s("Request answered Wait "); o.u(std::uint32_t(f.a)); o.c('x'); break;
    case Code::ManyRequests: o.u(std::uint32_t(f.a)); o.s(" Requests in one session"); break;
    case Code::NoAccept: o.s("Request not answered "); o.u(std::uint32_t(f.a)); o.c('x'); break;
    case Code::LateAccept: o.s("Accept late: "); o.u(std::uint32_t(f.a)); o.s(" ms after the Request"); break;
    case Code::NoPsRdy: o.s("No PS_RDY after Accept "); o.u(std::uint32_t(f.a)); o.c('x'); break;
    case Code::LatePsRdy: o.s("PS_RDY late: "); o.u(std::uint32_t(f.a)); o.s(" ms after Accept"); break;
    case Code::CapsUnanswered: o.s("Sink never answered the offer"); break;
    case Code::VbusLow: o.s("VBUS fell to "); o.volts2(f.a); o.s(" on a "); o.volts(f.b); o.s(" contract"); break;
    case Code::VbusHigh: o.s("VBUS rose to "); o.volts2(f.a); o.s(" on a "); o.volts(f.b); o.s(" contract"); break;
    case Code::VbusLost: o.s("VBUS lost with the contract up"); break;
    case Code::NoGoodCrc: o.u(std::uint32_t(f.a)); o.s(f.a == 1 ? " message without GoodCRC" : " messages without GoodCRC"); break;
    case Code::Retries: o.u(std::uint32_t(f.a)); o.s(f.a == 1 ? " retry on SOP" : " retries on SOP"); break;
    case Code::TxFail: o.u(std::uint32_t(f.a)); o.s(" K2 transmissions not acknowledged"); break;
    case Code::RxBad: o.u(std::uint32_t(f.a)); o.s(f.a == 1 ? " garbled frame" : " garbled frames"); break;
    case Code::SwapRefused: {
        const char* who = f.b ? r.src_noun : r.snk_noun;
        if (f.a == 1) {
            o.cap(f.b ? r.src_noun : r.snk_noun);
            o.s(f.b ? " refuses to take power on this port" : " refuses to source power on this port");
        } else {
            o.cap(who);
            o.s(f.a == 2 ? " refused a data role swap" : f.a == 4 ? " refused a VCONN swap" : " refused a fast role swap");
        }
        break;
    }
    case Code::ModeNak:
        if (f.b) o.s("Cable plug refused ");
        o.s(f.a & 1 ? (f.b ? "DP alt mode" : "DP alt mode refused") : f.a & 2 ? (f.b ? "Thunderbolt mode" : "Thunderbolt mode refused")
                    : (f.b ? "an alt mode" : "Alt mode refused"));
        break;
    case Code::EnterUsbRefused: o.s("Enter_USB refused"); break;
    case Code::EprFailed: o.s("EPR entry failed"); break;
    case Code::CableCap: o.s("Cable limits the offer to 3 A"); break;
    case Code::VdmLoop: o.s(vdm_cmd(std::uint32_t(f.a))); o.s(" asked "); o.u(std::uint32_t(f.b)); o.c('x'); break;
    case Code::CableSilent: o.s("Cable plug never answered on SOP'"); break;
    }
}

void note(const Report& r, const Finding& f, Out& o) {
    switch (f.code) {
    case Code::PowerShort:
        o.s("need: ");
        if (r.need_from == NeedFrom::SinkCaps) o.s("its sink caps on the wire, ");
        else if (r.need_from == NeedFrom::Skedb) { o.s("its PDP over PD (Sink_Capabilities_Extended)"); break; }
        else {
            o.s(r.need_from == NeedFrom::AssumedTwin ? "assumed twin \"" : "twin \"");
            o.s(r.twin ? r.twin : "?");
            o.s("\", ");
        }
        o.volts(f.c); o.c(' '); o.amps(f.d);
        break;
    case Code::MismatchFlag: o.s("set in its Request at "); o.clock(f.t_ms); break;
    case Code::NoMismatchFlag: o.s("its Request took "); o.volts(f.a); o.c(' '); o.amps(f.b); o.s(" without the mismatch bit"); break;
    case Code::Pinned: o.s(">= 95 % of it for "); o.u(std::uint32_t(f.b)); o.s(" % of "); o.dur(std::uint32_t(f.c)); break;
    case Code::OverDraw: {
        const unsigned bursts = std::uint32_t(f.c) >> 8 & 0xFF, lo = std::uint32_t(f.d) >> 2 & 0x3FF, hi = std::uint32_t(f.d) >> 12 & 0x3FF;
        const auto tenths = [&o](unsigned ds) { o.u(ds / 10); o.c('.'); o.u(ds % 10); };
        // the offer's own maximum, when the peak went past it too (the "far above" title)
        const std::uint32_t offer = (std::uint32_t(f.c) >> 16) * 10;
        if (offer && (f.d >> 22 & 1)) { o.s("offer "); o.amps(std::int32_t(offer)); o.s("; "); }
        const std::int32_t limit = std::int32_t(over_limit_ma(std::uint32_t(f.b)));
        if (bursts > 1) {   // "2 bursts, 1.5-2.4 s, from 2:07"
            o.u(bursts); o.s(" bursts, "); tenths(lo);
            if (hi != lo) { o.c('-'); tenths(hi); }
            o.s(" s, from ");
        } else if (bursts) {   // one long stretch: "above 3.46 A for 7.9 s from 0:05"
            o.s("above "); o.amps(limit); o.s(" for "); tenths(hi); o.s(" s from ");
        } else {               // a share of the time: "40 % of the time above 3.46 A from 0:12"
            o.u(std::uint32_t(f.c) & 0xFF); o.s(" % of the time above "); o.amps(limit); o.s(" from ");
        }
        o.clock(f.t_ms);
        // worst case fits a line: "offer 3.5 A; 100 % of the time above 3.46 A from 10:00; VBUS fell, hard reset"
        if ((f.d & 3) == 3) o.s("; VBUS fell, hard reset");
        else if (f.d & 1) o.s("; then VBUS fell");
        else if (f.d & 2) o.s("; then a hard reset");
        break;
    }
    case Code::Draw:
        o.s("observed "); o.dur(std::uint32_t(f.d)); o.s(", peak "); o.amps(r.peak_ma);
        break;
    case Code::DrawReduced:
        o.amps(f.a); o.s(" to "); o.amps(f.b); o.s(" at "); o.clock(f.t_ms);
        if (f.c > 1) { o.s(", "); o.u(std::uint32_t(f.c)); o.s(" drops"); }
        o.s(f.d ? "; no PD message, VBUS kept" : "; PD messages around it");
        break;
    case Code::FiveVoltOnly: o.s("the offer goes to "); o.volts(f.a); o.s(", the device's sink caps to "); o.volts(f.b); break;
    case Code::ContractUnseen:
        if (f.b) { o.s("VBUS "); o.volts(f.a); o.s(" when the K2 saw it"); }
        else o.s("no VBUS reading in this capture");
        break;
    case Code::NoContract: o.s("an offer on this leg, no Accept and PS_RDY after it"); break;
    case Code::ActiveSink: o.s("no load on the K2, so no draw to judge"); break;
    case Code::HardReset: o.s("first at "); o.clock(f.t_ms); break;
    case Code::SoftReset:
        o.s("first at "); o.clock(f.t_ms);
        if (f.b) { o.s(", "); o.u(std::uint32_t(f.b)); o.s(" sent by the K2"); }
        break;
    case Code::Reattach: o.s("CC detached and came back, first at "); o.clock(f.t_ms); break;
    case Code::Rejected: o.s("first at "); o.clock(f.t_ms); break;
    case Code::WaitLoop: o.s("the source asked the sink to retry"); break;
    case Code::ManyRequests: o.s("fixed-voltage, by the device; PPS keep-alives and the K2's own not counted"); break;
    case Code::NoAccept: o.s("no Accept, Reject or Wait, first at "); o.clock(f.t_ms); break;
    case Code::LateAccept: o.s("limit "); o.u(spec::kAcceptLimitMs); o.s(" ms"); break;
    case Code::NoPsRdy: o.s("first at "); o.clock(f.t_ms); break;
    case Code::LatePsRdy: o.s("limit "); o.u(f.b ? spec::kPsTransitionEprMaxMs : spec::kPsTransitionMaxMs); o.s(" ms"); break;
    case Code::CapsUnanswered: o.u(std::uint32_t(f.a)); o.s(" Source_Capabilities, no GoodCRC, no Request"); break;
    case Code::VbusLow:
        if (f.b) { o.c('-'); o.u(std::uint32_t((f.b - f.a) * 100 + f.b / 2) / std::uint32_t(f.b)); o.s(" %, "); }
        o.s("at "); o.amps(f.c); o.s(", "); o.clock(f.t_ms);
        break;
    case Code::InputCuts:
        // "0 mA in 552 of 9345 windows from 0:47; VBUS stayed up: the device let go"
        o.s("0 mA in "); o.u(std::uint32_t(f.a)); o.s(" of "); o.u(std::uint32_t(f.b));
        o.s(" windows from "); o.clock(f.t_ms); o.s("; VBUS stayed up: the device let go");
        break;
    case Code::OverSustained:
        // "83 % of 20 s above 3.46 A from 16:05; offer 3.5 A" (worst case, 72 chars:
        // "100 % of 59 min 59 s above 3.46 A from 1:16:05; offer 3.5 A")
        o.u(std::uint32_t(f.c) >> 16); o.s(" % of "); o.dur(std::uint32_t(f.d)); o.s(" above ");
        o.amps(std::int32_t(over_limit_ma(std::uint32_t(f.b)))); o.s(" from "); o.clock(f.t_ms);
        if (f.c & 0xFFFF) { o.s("; offer "); o.amps(f.c & 0xFFFF); }
        break;
    case Code::VbusHigh: o.s("at "); o.clock(f.t_ms); break;
    case Code::VbusLost:
        o.s("at "); o.clock(f.t_ms); o.s(", CC still attached");
        if (f.c && f.b * 1000 > f.c * 1050) { o.s("; drew "); o.amps(f.b); o.s(" on "); o.amps(f.c); o.s(": over-current signature"); }
        break;
    case Code::NoGoodCrc: o.s("SOP frames the other end did not acknowledge"); break;
    case Code::Retries: o.s("the same frame sent again"); break;
    case Code::TxFail: o.s("no GoodCRC from the partner on SOP"); break;
    case Code::RxBad: o.s("unknown FIFO token, frame dropped"); break;
    case Code::SwapRefused:
        if (f.a == 1) {
            o.s("PR_Swap "); o.s(ctl_answer(std::uint32_t(f.c)));
            if (f.t_ms) { o.s(" at "); o.clock(f.t_ms); }
            if (f.b) { o.s("; it stays the "); o.milli(f.d ? f.d : 5000, 0); o.s(" V source"); }
            else o.s("; it stays the sink");
        } else {
            o.s(f.a == 2 ? "DR_Swap" : f.a == 4 ? "VCONN_Swap" : "FR_Swap"); o.s(" not accepted");
        }
        break;
    case Code::ModeNak:
        o.s(f.b ? "Enter_Mode NAK on SOP' at " : "Enter_Mode NAK at "); o.clock(f.t_ms);
        if (f.b) o.s("; the cable plug only, not the port");
        break;
    case Code::EnterUsbRefused: o.s("not accepted by the other end"); break;
    case Code::EprFailed: o.s("EPR_Mode Enter_Failed, reason "); o.u(std::uint32_t(f.a)); break;
    case Code::CableCap: o.s("e-marker says 3 A, the device asks up to "); o.amps(f.a); break;
    case Code::VdmLoop: o.s("answered "); o.s(answer_name(std::uint32_t(f.c))); o.s("; harmless to power"); break;
    case Code::CableSilent:
        o.s("Discover_Identity sent "); o.u(std::uint32_t(f.a)); o.s("x (");
        o.u(std::uint32_t(f.b)); o.s(f.b == 1 ? " request" : " requests"); o.s(" + retries); no cable chip on this leg");
        break;
    }
}

void lanes_note(const Report& r, Out& o) {
    const auto& l = r.lanes;
    o.s("agreed over PD: ");
    bool any = false;
    auto sep = [&] { if (any) o.s(", "); any = true; };
    if (l.eusb) { sep(); o.s(l.usb_mode == 2 ? "USB4 entry" : l.usb_mode == 1 ? "USB 3 entry" : "USB 2 entry"); }
    if (l.tbt) { sep(); o.s("Thunderbolt mode"); }
    if (l.dp) {
        sep(); o.s("DP alt mode");
        if (l.dp_pin) { o.s(" pin "); o.c(l.dp_pin); }
    }
    if (!any) o.s("no alt mode, no USB4");
}

const char* head_power(std::uint8_t p) {
    switch (p) {
    case kPowerShort: return "power short";
    case kPowerOk: return "power OK";
    case kPowerUnknown: return "need not known";
    case kPowerUnseen: return "contract not observed";
    case kPowerNone: return "no contract made";
    case kPowerActive: return "K2 is the sink";
    case kPowerOver: return "draw over contract";
    default: return "";
    }
}

void head(const Report& r, Out& o) {
    if (r.empty) { o.s("No session yet"); return; }
    if (r.no_pd) {
        o.s("No PD seen, VBUS ");
        o.volts(r.vbus_mv);
        return;
    }
    o.s(r.link == 2 ? "PD link unstable, " : r.link == 1 ? "PD link degraded, " : "PD link healthy, ");
    o.s(head_power(r.power));
}

}  // namespace

// Line plan: head, (title, note) per finding, healthy lines, lanes title + note.
unsigned line_count(const Report& r) {
    if (r.empty) return 1;
    unsigned n = 1 + 2u * r.n;
    if (r.quiet_contract && r.quiet_link) n++;
    if (r.quiet_quality && r.band_checked) n++;
    if (r.quiet_roles) n++;
    return n + 2;
}

bool line(const Report& r, unsigned idx, Line& out) {
    out = Line();
    Out o{out.text};
    out.text[0] = 0;
    if (idx == 0) {
        out.kind = LineKind::Head;
        out.sev = r.n ? r.f[0].sev : Sev::Ok;
        if (out.sev == Sev::Info) out.sev = Sev::Ok;
        head(r, o);
        return true;
    }
    if (r.empty) return false;
    idx--;
    if (idx < 2u * r.n) {
        const Finding& f = r.f[idx / 2];
        out.sev = f.sev;
        out.num = std::uint8_t(idx / 2 + 1);
        if (idx % 2 == 0) { out.kind = LineKind::Title; title(r, f, o); }
        else { out.kind = LineKind::Note; note(r, f, o); }
        return true;
    }
    idx -= 2u * r.n;
    out.sev = Sev::Ok;
    out.kind = LineKind::Healthy;
    if (r.quiet_contract && r.quiet_link) { if (idx-- == 0) { o.s("No resets, no retries, no rejects"); return true; } }
    if (r.quiet_quality && r.band_checked) { if (idx-- == 0) { o.s("VBUS within the contract band"); return true; } }
    if (r.quiet_roles) { if (idx-- == 0) { o.s("No refused swaps or modes"); return true; } }
    out.kind = LineKind::Lanes;
    if (idx == 0) { o.s("Data lanes pass through the K2, not observed"); return true; }
    if (idx == 1) { out.kind = LineKind::Note; lanes_note(r, o); return true; }
    return false;
}

void verdict(const Report& r, std::span<char> out) {
    Out o{out};
    if (!out.empty()) out[0] = 0;
    head(r, o);
}

const char* code_name(Code c) {
    switch (c) {
    case Code::PowerShort: return "power_short";
    case Code::MismatchFlag: return "mismatch_flag";
    case Code::NoMismatchFlag: return "no_mismatch_flag";
    case Code::OverDraw: return "over_draw";
    case Code::Pinned: return "draw_pinned";
    case Code::Draw: return "draw";
    case Code::DrawReduced: return "draw_reduced";
    case Code::FiveVoltOnly: return "five_volt_only";
    case Code::ContractUnseen: return "contract_unseen";
    case Code::NoContract: return "no_contract";
    case Code::ActiveSink: return "active_sink";
    case Code::HardReset: return "hard_reset";
    case Code::SoftReset: return "soft_reset";
    case Code::Reattach: return "reattach";
    case Code::Rejected: return "rejected";
    case Code::WaitLoop: return "wait_loop";
    case Code::ManyRequests: return "many_requests";
    case Code::NoAccept: return "no_accept";
    case Code::LateAccept: return "late_accept";
    case Code::NoPsRdy: return "no_ps_rdy";
    case Code::LatePsRdy: return "late_ps_rdy";
    case Code::CapsUnanswered: return "caps_unanswered";
    case Code::VbusLow: return "vbus_low";
    case Code::VbusHigh: return "vbus_high";
    case Code::VbusLost: return "vbus_lost";
    case Code::InputCuts: return "input_cuts";
    case Code::OverSustained: return "over_sustained";
    case Code::NoGoodCrc: return "no_goodcrc";
    case Code::Retries: return "retries";
    case Code::TxFail: return "tx_fail";
    case Code::RxBad: return "rx_bad";
    case Code::SwapRefused: return "swap_refused";
    case Code::ModeNak: return "mode_nak";
    case Code::EnterUsbRefused: return "enter_usb_refused";
    case Code::EprFailed: return "epr_failed";
    case Code::CableCap: return "cable_cap";
    case Code::VdmLoop: return "vdm_loop";
    case Code::CableSilent: return "cable_silent";
    }
    return "?";
}

}  // namespace k2::pd::diag
