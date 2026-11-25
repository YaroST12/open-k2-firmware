// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box diagnosis, see k2/pd/diag.hpp. Pure logic over records: no I/O, no heap.
#include "k2/pd/diag.hpp"

#include "k2/core/strpack.hpp"

#include "k2/pd/diag_out.hpp"
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
constexpr std::uint16_t rd16(const std::uint8_t* p) {
    return std::uint16_t(p[0] | p[1] << 8);
}

// A request waiting for its answer.
enum : std::uint8_t { kPendNone, kPendRequest, kPendPr, kPendDr, kPendVconn, kPendFr, kPendEusb };
constexpr std::uint8_t swap_bit(std::uint8_t pend) {
    return (pend >= kPendPr && pend <= kPendFr) ? std::uint8_t(1u << (pend - kPendPr)) : 0;
}
constexpr std::uint16_t kSvidPd = 0xFF00, kSvidDp = 0xFF01, kSvidTbt = 0x8087;

// Saturating counter step.
template <class T>
void inc(T& v) {
    if (v < T(~T(0))) v++;
}

std::uint64_t udivmod64_by_1000(std::uint64_t x, std::uint16_t& rem_out) {
    const std::uint32_t hi = std::uint32_t(x >> 32);
    if (hi == 0) {
        const std::uint32_t lo = std::uint32_t(x);
        rem_out = std::uint16_t(lo % 1000u);
        return lo / 1000u;
    }
    const std::uint32_t q_hi = hi / 1000u;
    std::uint32_t rem = hi % 1000u;
    const std::uint32_t lo = std::uint32_t(x);
    const std::uint32_t w2 = (rem << 16) | (lo >> 16);
    const std::uint32_t q2 = w2 / 1000u;
    rem = w2 % 1000u;
    const std::uint32_t w1 = (rem << 16) | (lo & 0xFFFFu);
    const std::uint32_t q1 = w1 / 1000u;
    rem_out = std::uint16_t(w1 % 1000u);
    return (std::uint64_t(q_hi) << 32) | (std::uint64_t(q2) << 16) | q1;
}

std::uint64_t udiv64(std::uint64_t num, std::uint64_t den) {
    if (den == 0 || num < den) return 0;
    if ((num >> 32) == 0 && (den >> 32) == 0) {
        return std::uint32_t(num) / std::uint32_t(den);
    }
    int shift = __builtin_clzll(den) - __builtin_clzll(num);
    den <<= shift;
    std::uint64_t q = 0;
    for (; shift >= 0; --shift) {
        q <<= 1;
        if (num >= den) {
            num -= den;
            q |= 1;
        }
        den >>= 1;
    }
    return q;
}

std::uint32_t mul_div_u32(std::uint32_t a, std::uint32_t k, std::uint32_t b) {
    if (b == 0) return 0;
    if (a <= 0xFFFFFFFFu / k) return (a * k) / b;
    const std::uint32_t q = a / b;
    const std::uint32_t r = a % b;
    const std::uint32_t rem_q = (r <= 0xFFFFFFFFu / k)
                                    ? (r * k) / b
                                    : std::uint32_t(udiv64(std::uint64_t(r) * k, b));
    return q * k + rem_q;
}

// sum += prod / 1000, carrying the remainder (milli-units of the product). The sum is 64 bit: it only grows by addition.
void acc(std::uint64_t& sum, std::uint16_t& frac, std::uint64_t prod) {
    sum += udivmod64_by_1000(prod + frac, frac);
}

// The mean per second of a 64-bit sum over `ms` milliseconds: sum * 1000 / ms (no 64-bit division: udiv64 shifts).
std::uint32_t mean_of(std::uint64_t sum, std::uint32_t ms) {
    return ms ? std::uint32_t(udiv64(sum * 1000u, ms)) : 0;
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

[[gnu::noinline]] bool share_at_least(std::uint32_t part, std::uint32_t total, unsigned pct) {
    return total == 0 || mul_div_u32(part, 100u, total) >= pct;
}

[[gnu::noinline]] void reset_session(Session& s) { s = Session(); }

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

[[gnu::noinline]] void Analyzer::begin(bool by_attach) {
    reset_session(cur_);
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
[[gnu::noinline]] void Analyzer::close_burst() {
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

[[gnu::noinline]] void Analyzer::finish() {
    if (!cur_.open) return;
    close_burst();
    hold_to(cur_.ended ? cur_.detach_ms : now_ms_);
    cur_.end_ms = cur_.ended ? cur_.detach_ms : now_ms_;
    cur_.open = cur_.attached = false;
    last_ = cur_;
    have_last_ = true;
    finished_++;
    reset_session(cur_);
}

// The K2's sink session ends where the device's begins: CC stays attached, so no detach splits them. The source's
// offer carries over, and so does a negotiation the device already started (its Request came before the latch).
void Analyzer::hand_over() {
    const std::array<std::uint32_t, 7> pdos = src_pdos_;
    const bool dev_req = cur_.req_dev, dev_contract = dev_req && cur_.contract && cur_.contract_ms >= cur_.rdo_ms;
    const std::uint8_t pend = dev_req ? pend_ : std::uint8_t(kPendNone), pend_src = pend_from_src_;
    const std::uint32_t pend_ms = pend_ms_, accept_ms = accept_ms_;
    const bool wait = dev_req && wait_psrdy_;
    finish();
    begin(true);
    const Session& o = last_;
    Session& s = cur_;
    src_pdos_ = pdos;
    s.n_src = o.n_src;
    s.offer_mw = o.offer_mw;
    s.offer_ma = o.offer_ma;
    s.offer_top_mv = o.offer_top_mv;
    s.src_idh = o.src_idh;
    s.src_host = o.src_host;
    if (!dev_req) return;
    s.rdo = o.rdo;
    s.rdo_ms = o.rdo_ms;
    s.epr = o.epr;
    s.mismatch = o.mismatch;
    s.req_dev = true;
    pend_ = pend;
    pend_from_src_ = pend_src;
    pend_ms_ = pend_ms;
    wait_psrdy_ = wait;
    accept_ms_ = accept_ms;
    if (dev_contract) {
        s.contract = s.contract_seen = true;
        s.contract_ms = o.contract_ms;
        contract_from_rdo();
    }
}

// Time-weighted accounting of the held sample up to `ms`.
void Analyzer::hold_to(std::uint32_t ms) {
    Session& s = cur_;
    if (!s.open || !s.have_v || !s.attached) { v_ms_ = ms; return; }
    if (std::int32_t(ms - v_ms_) <= 0) return;
    const std::uint32_t dt = ms - v_ms_;
    v_ms_ = ms;
    const std::uint32_t ma = i_ma_ > 0 ? std::uint32_t(i_ma_) : 0;
    const std::uint32_t mw = (std::uint32_t(v_mv_) * ma) / 1000u;
    s.obs_ms += dt;
    acc(s.mj, mj_frac_, std::uint64_t(mw) * dt);
    acc(s.mv_s, mv_frac_, std::uint64_t(v_mv_) * dt);
    if (s.contract && ms - s.contract_ms > kSettleMs) {
        s.cobs_ms += dt;
        acc(s.cmj, cmj_frac_, std::uint64_t(mw) * dt);
        if (s.c_ma && ma * 1000 >= std::uint32_t(s.c_ma) * kPinPermille) s.pin_ms += dt;
        if (s.c_ma && ma > over_limit_ma(s.c_ma)) s.over_ms += dt;
        sustain(dt, ma);
    } else {
        blk_ms_ = blk_over_ms_ = blk_over_mas_ = 0;   // a block covers one settled contract only
    }
}

[[gnu::noinline]]
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
    if (share_at_least(blk_over_ms_, blk_ms_, kSustainPct)) {
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

[[gnu::noinline]] bool Analyzer::prep_vbus(std::uint16_t mv, std::uint16_t vmin, std::int16_t ma, bool is_win) {
    if (is_win || !win_) { pre_mv_ = mv; pre_ma_ = ma; pre_have_ = true; }
    if (is_win) win_ = true;
    Session& s = cur_;
    if (s.open && s.ended && vmin < kLostMv) unplug_ = true;
    if (!s.open && first_vbus_opens(vmin)) begin(false);
    if (!s.open || !s.attached) return false;
    if (mv > kUpMv && s.up_mv == 0) s.up_mv = mv;
    if (settled() && s.c_mv && vmin < kLostMv && !lost_pend_) {
        lost_pend_ = true;
        lost_pend_ms_ = now_ms_;
        lost_pend_ma_ = s.have_v ? i_ma_ : 0;
    }
    return true;
}

// A ring VBUS record. It is emitted on large changes, so it stays the fast witness of VBUS loss and of the unplug; the
// draw and band evidence come from it only while the session has no power windows.
void Analyzer::sample(std::uint16_t mv, std::int16_t ma) {
    if (!prep_vbus(mv, mv, ma, false)) return;
    if (win_) {   // a real sample at its own moment (often between the windows' 20 Hz ones): VBUS evidence only
        band(mv, mv, ma > 0 ? std::uint16_t(ma) : 0);
        return;
    }
    hold_to(now_ms_);
    cur_.have_v = true;
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
    if (!prep_vbus(w.mv, w.vmin, w.ma, true)) return;
    Session& s = cur_;
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

[[gnu::noinline]] void Analyzer::note_miss(std::uint8_t& cnt, std::uint32_t since, bool only_if_zero) {
    if (!blind(since) && cnt++ == 0 && (!only_if_zero || cur_.miss_ms == 0)) cur_.miss_ms = since;
}

[[gnu::noinline]] void Analyzer::refuse_swap(std::uint8_t kind, std::uint8_t b, bool by_src, std::uint8_t ans, std::uint32_t ms) {
    cur_.refused |= b;
    if (by_src) cur_.refused_by_src |= b;
    if (kind == kPendPr) {
        cur_.refused_ans = ans;
        cur_.refused_ms = ms;
        cur_.refused_mv = cur_.contract ? cur_.c_mv : v_mv_;
    }
}

[[gnu::noinline]] void Analyzer::record_mode_nak(unsigned svid, bool cable) {
    const std::uint8_t bit = svid == kSvidDp ? 1 : svid == kSvidTbt ? 2 : 4;
    const unsigned sh = cable ? 4 : 0;
    if (!(cur_.mode_nak & (7u << sh))) (cable ? cur_.cable_nak_ms : cur_.mode_nak_ms) = now_ms_;
    cur_.mode_nak |= std::uint8_t(bit << sh);
}

[[gnu::noinline]] void Analyzer::timers() {
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
            note_miss(s.no_accept, pend_ms_, false);
        } else if (const std::uint8_t b = blind(pend_ms_) ? 0 : swap_bit(pend_)) {
            refuse_swap(pend_, b, !pend_from_src_, 0, pend_ms_);   // never answered
        }
        pend_ = kPendNone;
    }
    if (wait_psrdy_ && now_ms_ - accept_ms_ > kPsRdyMissingMs) {
        wait_psrdy_ = false;
        note_miss(s.no_psrdy, accept_ms_, true);
    }
}

[[gnu::noinline]] void Analyzer::ps_rdy() {
    Session& s = cur_;
    wait_psrdy_ = false;
    const std::uint32_t dt = now_ms_ - accept_ms_;
    const std::uint32_t lim = (s.epr ? spec::kPsTransitionEprMaxMs : spec::kPsTransitionMaxMs) + kAnswerSlackMs;
    if (dt > lim && dt > s.late_psrdy_ms) s.late_psrdy_ms = std::uint16_t(dt);
    s.contract = s.contract_seen = true;
    s.contract_ms = now_ms_;
    contract_from_rdo();
    last_pwr_ms_ = now_ms_;
}

[[gnu::noinline]] void Analyzer::gap() {
    inc(cur_.gaps);
    gap_ms_ = now_ms_;
}

bool Analyzer::blind(std::uint32_t since) const { return cur_.gaps && std::int32_t(gap_ms_ - since) >= 0; }

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
        if (s.open && !s.cc_seen && s.msgs == 0 && now_ms_ - s.start_ms < kDrawSettleMs) reset_session(cur_);
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
        if (p.size() >= 4) sample(rd16(&p[0]), std::int16_t(rd16(&p[2])));
        return;
    case Type::Rx:
    case Type::Tx: {
        const bool rx = Type(type) == Type::Rx;
        if (p.size() < (rx ? 7u : 2u)) return;
        const std::size_t off = rx ? 1 : 0;
        const bool has_token = !rx && (p.size() - 2) % 4 == 1;
        const std::uint8_t token = rx ? p[0] : (has_token ? p[p.size() - 1] : 0xE0);
        const std::size_t link_len = p.size() - (has_token ? 1 : 0);
        const std::size_t obj_len = p.size() - (rx ? 7 : 2 + (has_token ? 1 : 0));
        message(rx, token, rd16(&p[off]), p.subspan(off + 2, obj_len), rx ? rd32(&p[p.size() - 4]) : 0);
        if (cur_.open) cur_.link.feed_message(rx, p.first(link_len));
        return;
    }
    case Type::TxResult:
        if (s.open && p.size() >= 1 && (p[0] == std::uint8_t(pdtrace::TxResult::RetryFail) || p[0] == std::uint8_t(pdtrace::TxResult::Timeout)))
            inc(s.tx_fail);   // TxSent and RxGoodCrc were delivered
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
    case Type::Link:
        // The firmware's inline latch (pd::Link, certain evidence only) while the K2's own sink session runs: that
        // session is over and the device behind the K2 owns the bus. A passive session goes on as it is.
        if (s.open && s.active && p.size() >= 1 && (p[0] & 1)) hand_over();
        return;
    case Type::RxRepeat:
        if (s.open && p.size() >= 2) s.caps = std::uint16_t(s.caps + rd16(&p[0]));
        return;
    default:
        return;
    }
}

[[gnu::noinline]] void Analyzer::contract_from_rdo() {
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

[[gnu::noinline]] void Analyzer::cable_message(bool from_src, bool ext, unsigned type, unsigned n, const std::uint8_t* o) {
    Session& s = cur_;
    if (from_src) {   // the cable plug speaks
        s.cable_answered = true;
        if (!ext && n >= 1 && type == kVdm) {
            const std::uint32_t v = rd32(o);
            // a plug may NAK with the PD SID: the mode is the one the port asked it to enter
            const unsigned svid = (v >> 16) == kSvidPd ? cable_mode_svid_ : v >> 16, ctype = v >> 6 & 3;
            if ((v >> 15 & 1) && (v & 0x1F) == 4 && (ctype == 2 || ctype == 3))   // Enter_Mode NAK / BUSY
                record_mode_nak(svid, true);
        }
        return;
    }
    inc(s.cable_frames);
    if (!ext && n >= 1 && type == kVdm) {
        const std::uint32_t v = rd32(o);
        if ((v >> 15 & 1) && (v >> 6 & 3) == 0) {
            if ((v & 0x1F) == 1) inc(s.cable_tries);
            if ((v & 0x1F) == 4) cable_mode_svid_ = std::uint16_t(v >> 16);
        }
    }
}

[[gnu::noinline]] void Analyzer::pend_answer(bool from_src, unsigned type) {
    Session& s = cur_;
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
            inc(s.waits);
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
            refuse_swap(kind, b, from_src, std::uint8_t(type), now_ms_);
        }
    }
}

[[gnu::noinline]] void Analyzer::vdm_message(bool from_src, unsigned n, const std::uint8_t* o) {
    Session& s = cur_;
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
    if (cmd == 4 && (ctype == 2 || ctype == 3)) record_mode_nak(svid, false);
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
        // a GoodCRC to no frame the K2 decoded: the frame was missed; after a Request the source's frames are Accept, PS_RDY
        if (sop == 0 && !s.active && ((frame_[0].hdr >> 8 & 1) == from_src || (frame_[0].hdr >> 9 & 7) != id)) {
            gap();
            if (from_src) return;
            if (pend_ == kPendRequest) {
                pend_ = kPendNone;
                wait_psrdy_ = true;
                accept_ms_ = last_pwr_ms_ = now_ms_;
            } else if (wait_psrdy_) {
                ps_rdy();
            }
        }
        return;
    }
    if (rx) {   // retries and frames left without GoodCRC (both ends' GoodCRC are on the wire when the K2 is passive)
        Frame& f = frame_[sop];
        const std::uint16_t ms16 = std::uint16_t(now_ms_);
        if (frame_open_ >> sop & 1) {
            if (f.hdr == hdr && f.crc == crc && std::uint16_t(ms16 - f.ms16) <= kRetryMs) {
                inc(sop == 0 ? s.retries : s.cable_frames);
                frame_open_ |= std::uint8_t(0x10u << sop);   // retried: the sender saw no GoodCRC
                f.ms16 = ms16;
                return;   // the same message again: its meaning was taken the first time
            }
            // Source_Capabilities before the sink listens go unacknowledged by design (the source repeats them; the
            // caps-unanswered rule covers a sink that never answers): not a link fault
            const bool early_caps = (f.hdr & 0x1F) == kSrcCaps && !(f.hdr >> 15 & 1) && (f.hdr >> 12 & 7) &&
                                    !s.contract_seen;
            // a sender without GoodCRC retries the same frame: none seen and the talk moved on (the other end, a new
            // MessageID, not a Soft_Reset) means the GoodCRC was sent and the K2 missed it
            const bool moved_on = !(frame_open_ >> (4 + sop) & 1) && ((f.hdr ^ hdr) & 0x0F00) &&
                                  !(!ext && n == 0 && type == kSoftReset);
            if (sop == 0 && !s.active && !early_caps) {
                if (moved_on) gap();
                else inc(s.no_goodcrc);
            }
        }
        f.hdr = hdr;
        f.crc = crc;
        f.ms16 = ms16;
        frame_open_ = std::uint8_t((frame_open_ & ~(0x10u << sop)) | 1u << sop);
    }
    const std::uint8_t* o = objs.data();

    if (sop != 0) { cable_message(from_src, ext, type, n, o); return; }

    inc(s.msgs);
    if (!from_src) s.sink_answered = true;
    (from_src ? s.src_host : s.snk_host) = dfp;

    // An answer to the pending request (from the other side).
    if (!ext && n == 0 && pend_ != kPendNone && from_src != (pend_from_src_ != 0) &&
        (type == kAccept || type == kReject || type == kWait || type == kNotSupported)) {
        pend_answer(from_src, type);
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
            if (pend_ == kPendRequest) { note_miss(s.no_accept, pend_ms_, false); pend_ = kPendNone; }
            if (wait_psrdy_) { wait_psrdy_ = false; note_miss(s.no_psrdy, accept_ms_, true); }
            last_pwr_ms_ = now_ms_;
            return;
        case kRequest:
        case kEprRequest: {
            if (from_src) return;
            if (pend_ == kPendRequest) note_miss(s.no_accept, pend_ms_, false);
            s.rdo = rd32(o);
            s.rdo_ms = now_ms_;
            s.req_dev = rx;
            s.epr = type == kEprRequest;   // RDO bit 22 is "EPR mode capable", set on SPR requests too
            if (s.rdo >> 26 & 1) s.mismatch = true;
            const unsigned pos = s.rdo >> 28 & 0xF;
            const bool pps = pos >= 1 && pos <= s.n_src && (src_pdos_[pos - 1] >> 30) == 3;
            if (!pps && rx) inc(s.req_fixed);   // the K2's own Requests (an active test script) are not the device's
            pend_ = kPendRequest;
            pend_from_src_ = 0;
            pend_ms_ = last_pwr_ms_ = now_ms_;
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
        case kVdm:
            vdm_message(from_src, n, o);
            return;
        default:
            return;
        }
    }
    if (ext) {
        if (type == kExtSkedb && !from_src && objs.size() >= 2 + 21) {
            const unsigned eh = rd16(o);
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
        if (!from_src) return;
        if (pend_ == kPendRequest) {   // its Accept was missed
            gap();
            pend_ = kPendNone;
            wait_psrdy_ = true;
            accept_ms_ = now_ms_;
        }
        if (wait_psrdy_) ps_rdy();
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

constexpr std::uint32_t fmeta(Sev s, Group g, Code c) {
    return std::uint32_t(s) | (std::uint32_t(g) << 8) | (std::uint32_t(c) << 16);
}

[[gnu::noinline]] void add_x(Report& r, std::uint32_t meta, std::uint32_t t, std::int32_t a, std::int32_t b,
                             std::int32_t cc = 0, std::int32_t d = 0) {
    if (r.n >= kMaxFindings) return;
    r.f[r.n++] = Finding{Sev(meta & 0xFF), Group(meta >> 8 & 0xFF), Code(meta >> 16 & 0xFF), t, a, b, cc, d};
}

[[gnu::noinline]] void add(Report& r, std::uint32_t meta, std::uint32_t t = 0, std::int32_t a = 0) {
    add_x(r, meta, t, a, 0, 0, 0);
}

struct U8Rule {
    std::uint8_t Session::* v;
    std::uint32_t Session::* ts;
    std::uint8_t min_v, hi_v;
    Sev lo_sev, hi_sev;
    Group g;
    Code c;
};

[[gnu::noinline]] void eval_rules(Report& r, const Session& s, const U8Rule* rules, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const auto& ru = rules[i];
        const std::uint8_t val = s.*(ru.v);
        if (val < ru.min_v) continue;
        const Sev sev = val >= ru.hi_v ? ru.hi_sev : ru.lo_sev;
        const std::uint32_t ms = ru.ts ? s.*(ru.ts) : 0;
        const std::uint32_t t = (ru.ts && ms >= s.start_ms) ? ms - s.start_ms : 0;
        add(r, fmeta(sev, ru.g, ru.c), t, val);
    }
}

[[gnu::noinline]] std::uint32_t at_off(const Session& s, std::uint32_t ms) {
    return ms >= s.start_ms ? ms - s.start_ms : 0;
}

// The source's noun from its ID header product types (UFP bits 29:27, DFP bits 25:23), else its data role.
const char* src_noun(std::uint32_t idh, bool host) {
    const unsigned ufp = idh >> 27 & 7, dfp = idh >> 23 & 7;
    if (dfp == 3) return K2_P("charger");
    if (ufp == 1 || dfp == 1) return "hub";
    if (dfp == 2 || host) return "host";
    return K2_P("source");
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
    auto at = [&](std::uint32_t ms) { return at_off(s, ms); };

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
    r.snk_noun = s.active ? "K2" : host && r.need_mw >= 45000 ? K2_P("Laptop") : host ? "Host" : K2_P("Device");
    r.src_noun = src_noun(s.src_idh, s.src_host);
    const std::uint32_t mean_mv = mean_of(s.mv_s, s.obs_ms);
    r.vbus_mv = std::uint16_t(mean_mv);
    r.peak_ma = s.peak_ma;

    // -- power --
    const bool short_power = !s.active && r.need_mw && r.offer_mw && r.need_mw > r.offer_mw;
    if (short_power)
        add_x(r, fmeta(Sev::Fail, Group::Power, Code::PowerShort), at(s.rdo_ms), std::int32_t(r.need_mw),
              std::int32_t(r.offer_mw), r.need_mv, r.need_ma);
    if (!s.active && s.contract_seen && s.mismatch)
        add(r, fmeta(Sev::Warn, Group::Power, Code::MismatchFlag), at(s.rdo_ms));
    else if (short_power && s.contract_seen)
        add_x(r, fmeta(Sev::Info, Group::Power, Code::NoMismatchFlag), at(s.rdo_ms), s.c_mv, s.c_ma);
    if (s.active) {
        if (s.contract_seen)
            add_x(r, fmeta(Sev::Info, Group::Power, Code::ActiveSink), at(s.contract_ms), s.c_mv, s.c_ma,
                  std::int32_t(s.rdo >> 28 & 0xF));
    } else if (s.contract_seen) {
        // sustained (a share of the contract time) or in bursts (each kOverBurstMs or longer)
        const bool over = s.over_c_ma && ((s.over_ms >= kOverMinMs && share_at_least(s.over_ms, s.cobs_ms, kOverSharePct)) ||
                                          s.bursts);
        if (over) {
            // c: share % | bursts << 8 | offer at the peak (10 mA) << 16
            // d: VBUS fell / hard reset bits | shortest burst (0.1 s) << 2 | longest << 12 | above the offer << 22
            const auto ds = [](std::uint32_t ms) { return std::uint32_t(ms / 100 > 999 ? 999 : ms / 100); };
            const std::uint32_t share = s.cobs_ms ? mul_div_u32(s.over_ms, 100u, s.cobs_ms) : 0;
            add_x(r, fmeta(s.over_fault ? Sev::Fail : Sev::Warn, Group::Power, Code::OverDraw), at(s.over_first_ms),
                  s.over_peak_ma, s.over_c_ma,
                  std::int32_t((share > 100 ? 100 : share) | std::uint32_t(s.bursts) << 8 |
                               std::uint32_t(s.over_off_ma / 10) << 16),
                  std::int32_t((s.over_fault & 3) | ds(s.burst_min_ms) << 2 | ds(s.burst_max_ms) << 12 |
                               std::uint32_t(s.over_fault >> 2 & 1) << 22));
        }
        // kSustainMs blocks mostly above the contract: a Fail when the level is above the offer too, or with VBUS
        // falling / a reset. a: the mean while above, c: offer | share % << 16, d: the blocks' time (ms)
        if (s.sus_ms && s.sus_over_ms) {
            const std::uint32_t lvl = mul_div_u32(s.sus_mas, 1000u, s.sus_over_ms);
            const std::uint32_t share = mul_div_u32(s.sus_over_ms, 100u, s.sus_ms);
            const bool far = s.sus_off_ma && lvl > over_limit_ma(s.sus_off_ma);
            add_x(r, fmeta(far || (s.over_fault & 3) ? Sev::Fail : Sev::Warn, Group::Power, Code::OverSustained),
                  at(s.sus_first_ms), std::int32_t(lvl), s.sus_c_ma, std::int32_t(s.sus_off_ma | share << 16),
                  std::int32_t(s.sus_ms));
        }
        if (!over && !s.sus_ms && s.c_ma && s.cobs_ms >= kDrawSettleMs && share_at_least(s.pin_ms, s.cobs_ms, kPinSharePct))
            add_x(r, fmeta(Sev::Warn, Group::Power, Code::Pinned), at(s.contract_ms), s.c_ma,
                  std::int32_t(s.pin_ms * 100 / s.cobs_ms), std::int32_t(s.cobs_ms));
        const std::int32_t mw = std::int32_t(mean_of(s.cmj, s.cobs_ms));
        if (s.cobs_ms > 0)
            add_x(r, fmeta(Sev::Info, Group::Power, Code::Draw), at(s.contract_ms), s.c_mv, s.c_ma, mw,
                  std::int32_t(s.cobs_ms));
        // a 5 V contract while the device declares more and the offer has more
        const bool snk_more = s.n_snk && (s.snk_higher || s.snk_top_mv > kUpMv);
        if (s.c_mv && s.c_mv <= kUpMv && snk_more && s.offer_top_mv > kUpMv)
            add_x(r, fmeta(Sev::Warn, Group::Power, Code::FiveVoltOnly), at(s.contract_ms), s.offer_top_mv,
                  s.snk_top_mv > kUpMv ? s.snk_top_mv : s.offer_top_mv);
    } else if (s.caps || s.rdo) {   // an offer (or a Request) on this leg, and no Accept + PS_RDY after it
        add(r, fmeta(Sev::Info, Group::Power, Code::NoContract), 0, std::int32_t(mean_mv));
    } else {
        // PD traffic (swaps, VDMs: they need a contract) or VBUS above 5 V with no offer seen: the contract was
        // made before the K2 attached
        if (!r.no_pd)
            add_x(r, fmeta(Sev::Info, Group::Power, Code::ContractUnseen), 0, s.up_mv ? s.up_mv : std::int32_t(mean_mv),
                  s.up_mv || s.obs_ms ? 1 : 0);
        if (s.obs_ms && mean_mv >= kLostMv)   // (no VBUS: a charger waiting for a sink, nothing to draw)
            add_x(r, fmeta(Sev::Info, Group::Power, Code::Draw), 0, 0, 0, std::int32_t(mean_of(s.mj, s.obs_ms)),
                  std::int32_t(s.obs_ms));
    }
    // input cut-outs: the device's draw falls to nothing inside a window while VBUS holds in band (the source kept
    // its voltage, so the device let go); they make the separate "reduced its draw" line moot
    const bool cuts = !s.active && s.cut_n >= kCutMin;
    if (cuts)
        add_x(r, fmeta(Sev::Warn, Group::Power, Code::InputCuts), at(s.cut_ms), s.cut_n, s.win_n);
    if (!s.active && s.drops && !cuts)
        add_x(r, fmeta(Sev::Info, Group::Power, Code::DrawReduced), at(s.drop_ms), s.drop_from_ma, s.drop_min_ma,
              s.drops, s.drops_quiet ? 1 : 0);

    // -- contract instability --
    static constexpr U8Rule kContract1[] = {
        {&Session::hard, &Session::hard_ms, 1, 0xFF, Sev::Fail, Sev::Fail, Group::Contract, Code::HardReset},
        {&Session::reattaches, &Session::reattach_ms, 1, 0xFF, Sev::Warn, Sev::Warn, Group::Contract, Code::Reattach},
    };
    static constexpr U8Rule kContract2[] = {
        {&Session::rejects, &Session::reject_ms, 1, 0xFF, Sev::Warn, Sev::Warn, Group::Contract, Code::Rejected},
        {&Session::waits, nullptr, 2, 0xFF, Sev::Warn, Sev::Warn, Group::Contract, Code::WaitLoop},
        {&Session::no_accept, &Session::miss_ms, 1, 0xFF, Sev::Warn, Sev::Warn, Group::Contract, Code::NoAccept},
        {&Session::no_psrdy, &Session::miss_ms, 1, 0xFF, Sev::Warn, Sev::Warn, Group::Contract, Code::NoPsRdy},
    };
    static constexpr U8Rule kLinkRules[] = {
        {&Session::no_goodcrc, nullptr, 1, 3, Sev::Info, Sev::Warn, Group::Link, Code::NoGoodCrc},
        {&Session::retries, nullptr, 1, 3, Sev::Info, Sev::Warn, Group::Link, Code::Retries},
        {&Session::tx_fail, nullptr, 1, 0xFF, Sev::Warn, Sev::Warn, Group::Link, Code::TxFail},
        {&Session::rx_bad, nullptr, 1, 3, Sev::Info, Sev::Warn, Group::Link, Code::RxBad},
    };
    const unsigned n0 = r.n;
    eval_rules(r, s, kContract1, 2);
    if (s.caps >= 3 && !s.sink_answered) add(r, fmeta(Sev::Fail, Group::Contract, Code::CapsUnanswered), 0, s.caps);
    if (s.soft) add_x(r, fmeta(s.soft > 2 ? Sev::Fail : Sev::Warn, Group::Contract, Code::SoftReset), at(s.soft_ms),
                      s.soft, s.soft_k2);
    eval_rules(r, s, kContract2, 4);
    if (s.late_accept_ms) add(r, fmeta(Sev::Warn, Group::Contract, Code::LateAccept), 0, s.late_accept_ms);
    if (s.late_psrdy_ms) add_x(r, fmeta(Sev::Warn, Group::Contract, Code::LatePsRdy), 0, s.late_psrdy_ms, s.epr ? 1 : 0);
    if (s.req_fixed >= kManyRequests) add(r, fmeta(Sev::Warn, Group::Contract, Code::ManyRequests), 0, s.req_fixed);
    r.quiet_contract = r.n == n0;

    // -- power quality --
    const unsigned n1 = r.n;
    if (s.lost)
        add_x(r, fmeta(Sev::Fail, Group::Quality, Code::VbusLost), at(s.lost_ms), s.lost, s.lost_ma, s.lost_c_ma);
    if (s.low_mv) add_x(r, fmeta(Sev::Warn, Group::Quality, Code::VbusLow), at(s.low_ms), s.low_mv, s.low_c, s.low_ma);
    if (s.high_mv) add_x(r, fmeta(Sev::Warn, Group::Quality, Code::VbusHigh), at(s.high_ms), s.high_mv, s.high_c);
    r.quiet_quality = r.n == n1;
    r.band_checked = s.contract_seen && s.cobs_ms > 0 && !s.pps;

    // -- link quality --
    const unsigned n2 = r.n;
    eval_rules(r, s, kLinkRules, 4);
    r.quiet_link = r.n == n2;
    if (s.gaps) add(r, fmeta(Sev::Info, Group::Link, Code::CaptureGap), 0, s.gaps);

    // -- roles and modes --
    const unsigned n3 = r.n;
    for (std::uint8_t b = 1; b <= 8; b <<= 1)
        if (s.refused & b)
            add_x(r, fmeta(Sev::Info, Group::Roles, Code::SwapRefused), b == 1 ? at(s.refused_ms) : 0, b,
                  (s.refused_by_src & b) ? 1 : 0, b == 1 ? s.refused_ans : 0, b == 1 ? s.refused_mv : 0);
    if (s.mode_nak & 7) add(r, fmeta(Sev::Warn, Group::Roles, Code::ModeNak), at(s.mode_nak_ms), s.mode_nak & 7);
    // the cable plug NAKs its own Enter_Mode (SOP'): the port's mode is agreed on SOP, so this alone is Info
    if (s.mode_nak & 0x70)
        add_x(r, fmeta(Sev::Info, Group::Roles, Code::ModeNak), at(s.cable_nak_ms), s.mode_nak >> 4 & 7, 1);
    if (s.eusb_refused) add(r, fmeta(Sev::Warn, Group::Roles, Code::EnterUsbRefused));
    if (s.epr_failed) add(r, fmeta(Sev::Warn, Group::Roles, Code::EprFailed), 0, s.epr_reason);
    const auto& L = s.link.link();
    // a 3 A e-marker while the device needs more current than that: the cable caps the offer (sources hold 5 A back)
    if (L.cable_emarker && L.cable_amps == 3 && r.need_ma > 3000 && off_ma <= 3000)
        add(r, fmeta(short_power ? Sev::Warn : Sev::Info, Group::Roles, Code::CableCap), 0, r.need_ma);
    {
        const VdmKey* top = nullptr;
        for (const auto& e : s.vdm) if (e.fast >= kVdmLoopMin && (!top || e.count > top->count)) top = &e;
        if (top) add_x(r, fmeta(Sev::Info, Group::Roles, Code::VdmLoop), 0, std::int32_t(top->key), top->count, top->answer);
    }
    if (s.cable_tries && !s.cable_answered)
        add_x(r, fmeta(Sev::Info, Group::Roles, Code::CableSilent), 0, s.cable_frames, s.cable_tries);
    r.quiet_roles = r.n == n3;

    // rank: Fail, Warn, Info (stable: the group order stays inside a severity)
    for (unsigned i = 1; i < r.n; i++)
        for (unsigned j = i; j > 0 && r.f[j].sev < r.f[j - 1].sev; j--) { const Finding t = r.f[j]; r.f[j] = r.f[j - 1]; r.f[j - 1] = t; }

    bool unstable = false, degraded = false, over = false;
    for (unsigned i = 0; i < r.n; i++) {
        const Finding& f = r.f[i];
        if (f.group == Group::Contract || f.group == Group::Quality || f.group == Group::Link) {
            if (f.sev == Sev::Fail) unstable = true;
            else if (f.sev == Sev::Warn) degraded = true;
        }
        over = over || f.code == Code::OverDraw || f.code == Code::OverSustained;
    }
    r.link = unstable ? 2 : degraded ? 1 : 0;
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

using detail::Out;

const char* answer_name(unsigned a) {
    static constexpr const char* const kAns[] = {K2_P("nothing"), "ACK", "NAK", "BUSY", K2_P("Not_Supported")};
    return a >= 1 && a <= 4 ? kAns[a] : kAns[0];
}
const char* ctl_answer(unsigned t) {
    return t == kReject ? K2_P("rejected") : t == kWait ? K2_P("answered Wait") : t == kNotSupported ? K2_P("answered Not_Supported") : K2_P("not answered");
}
const char* vdm_cmd(unsigned key) {
    static constexpr const char* const kVdm1_6[] = {
        K2_P("Discover_Identity"), K2_P("Discover_SVIDs"), K2_P("Discover_Modes"),
        K2_P("Enter_Mode"), K2_P("Exit_Mode"), K2_P("Attention"),
    };
    const unsigned cmd = key & 0x1F, svid = key >> 8;
    if (cmd >= 1 && cmd <= 6) return kVdm1_6[cmd - 1];
    if (cmd == 16) return svid == kSvidDp ? K2_P("DP Status") : K2_P("VDM 16");
    if (cmd == 17) return svid == kSvidDp ? K2_P("DP Configure") : K2_P("VDM 17");
    return K2_P("VDM request");
}

enum StrId : std::uint8_t {
    S_comma, S_space, S_on, S_on_a, S_contract, S_x, S_at, S_sp_at, S_to, S_first_at,
    S_needs, S_gives, S_max, S_flagged_mm, S_no_mm, S_draws_over, S_draws_far, S_drew_less, S_reduced_draw,
    S_cuts_title, S_stays_over,
    S_pinned_t, S_contract_lim, S_contract_cap, S_draws_c, S_unseen_t, S_nocontract_t,
    S_hard_1, S_hard_n, S_soft_1, S_soft_n, S_reattach_t, S_x_3s, S_rejected_t,
    S_wait_t, S_many_req_t, S_no_acc_t, S_late_acc_t, S_ms_req, S_no_psrdy_t,
    S_late_psrdy_t, S_ms_acc, S_caps_unans_t, S_vbus_fell, S_vbus_rose, S_vbus_lost_t,
    S_nogc_1, S_nogc_n, S_retry_1, S_retry_n, S_txfail_t, S_rxbad_1, S_rxbad_n,
    S_eusb_t, S_epr_t, S_cablecap_t, S_asked, S_cablesil_t, S_gap_1, S_gap_n,
    S_mm_n, S_nomm_n1, S_nomm_n2, S_pinned_n1, S_pct_of, S_observed, S_peak,
    S_5v_n1, S_5v_n2, S_nocontract_n, S_active_n, S_reattach_n, S_wait_n, S_many_req_n,
    S_no_acc_n, S_caps_unans_n, S_cuts_n1, S_of, S_cuts_n2, S_cuts_n3, S_nogc_n_note,
    S_retry_n_note, S_txfail_n, S_rxbad_n_note, S_eusb_n, S_epr_n, S_cablecap_n,
    S_vdmloop_n1, S_vdmloop_n2, S_cablesil_n1, S_x_paren, S_req_1, S_req_n,
    S_cablesil_n2, S_gap_n_note,
    S_count
};

static constexpr const char* const kStrs[] = {
    ", ", " ", " on ", K2_P(" on a "), K2_P(" contract"), "x", "at ", " at ", " to ", K2_P("first at "),
    K2_P(" needs "), K2_P(" gives "), " max", K2_P(" flagged capability mismatch"), K2_P(" did not flag capability mismatch"),
    K2_P(" draws above its contract: up to "), K2_P(" draws far above its contract: up to "),
    K2_P(" drew less, with PD traffic around it"), K2_P(" reduced its draw, contract unchanged"),
    K2_P("'s input cuts out repeatedly"), K2_P(" stays over its contract, at "),
    K2_P("Draw pinned at the "), K2_P(" contract limit"), K2_P("Contract "), K2_P(", draws "),
    K2_P("Contract not observed (the K2 attached after it)"), K2_P("No contract made, VBUS "),
    K2_P(" hard reset"), K2_P(" hard resets"), K2_P(" soft reset on SOP"), K2_P(" soft resets on SOP"),
    K2_P("Re-attached "), K2_P("x within 3 s"), K2_P("Request rejected "),
    K2_P("Request answered Wait "), K2_P(" Requests in one session"), K2_P("Request not answered "),
    K2_P("Accept late: "), K2_P(" ms after the Request"), K2_P("No PS_RDY after Accept "),
    K2_P("PS_RDY late: "), K2_P(" ms after Accept"), K2_P("Sink never answered the offer"),
    K2_P("VBUS fell to "), K2_P("VBUS rose to "), K2_P("VBUS lost with the contract up"),
    K2_P(" message without GoodCRC"), K2_P(" messages without GoodCRC"), K2_P(" retry on SOP"), K2_P(" retries on SOP"),
    K2_P(" K2 transmissions not acknowledged"), K2_P(" garbled frame"), K2_P(" garbled frames"),
    K2_P("Enter_USB refused"), K2_P("EPR entry failed"), K2_P("Cable limits the offer to 3 A"),
    K2_P(" asked "), K2_P("Cable plug never answered on SOP'"), K2_P(" frame the K2 missed"), K2_P(" frames the K2 missed"),
    K2_P("set in its Request at "), K2_P("its Request took "), K2_P(" without the mismatch bit"),
    K2_P(">= 95 % of it for "), K2_P(" % of "), K2_P("observed "), K2_P(", peak "),
    K2_P("the offer goes to "), K2_P(", the device's sink caps to "),
    K2_P("an offer on this leg, no Accept and PS_RDY after it"), K2_P("no load on the K2, so no draw to judge"),
    K2_P("CC detached and came back, first at "), K2_P("the source asked the sink to retry"),
    K2_P("fixed-voltage, by the device; PPS keep-alives and the K2's own not counted"),
    K2_P("no Accept, Reject or Wait, first at "), K2_P(" Source_Capabilities, no GoodCRC, no Request"),
    K2_P("0 mA in "), " of ", K2_P(" windows from "), K2_P("; VBUS stayed up: the device let go"),
    K2_P("SOP frames the other end did not acknowledge"), K2_P("the same frame sent again"),
    K2_P("no GoodCRC from the partner on SOP"), K2_P("unknown FIFO token, frame dropped"),
    K2_P("not accepted by the other end"), K2_P("EPR_Mode Enter_Failed, reason "),
    K2_P("e-marker says 3 A, the device asks up to "), K2_P("answered "), K2_P("; harmless to power"),
    K2_P("Discover_Identity sent "), "x (", K2_P(" request"), K2_P(" requests"),
    K2_P(" + retries); no cable chip on this leg"),
    K2_P("seen by their GoodCRC or the next MessageID; a capture gap, not a link fault"),
};
static_assert(sizeof(kStrs) / sizeof(kStrs[0]) == S_count);
static_assert(S_count < 0xC0);

constexpr std::uint8_t S(StrId id) { return std::uint8_t(1 + id); }

enum Op : std::uint8_t {
    OP_END = 0,
    OP_CAP_SNK = 0xC0,
    OP_SRC,
    OP_WATTS_A,
    OP_WATTS_B,
    OP_AMPS_A,
    OP_AMPS_B,
    OP_AMPS_PEAK,
    OP_VOLTS_A,
    OP_VOLTS_B,
    OP_VOLTS2_A,
    OP_U_A,
    OP_U_B,
    OP_DUR_C,
    OP_DUR_D,
    OP_CLOCK,
    OP_VDM,
    OP_ANS,
    OP_U_PLUR_A,
    OP_U_PLUR_B,
    OP_SEL_D_BIT,
};

struct ProgEntry {
    Code code;
    std::uint8_t ops[10];
};
struct ProgTable {
    std::array<std::uint8_t, std::size_t(Code::CaptureGap) + 1> off{};
    std::array<std::uint8_t, 254> data{};
    std::uint8_t len = 0;
};

consteval ProgTable build_prog(std::span<const ProgEntry> entries) {
    ProgTable t;
    t.off.fill(0xFF);
    for (const auto& e : entries) {
        t.off[std::size_t(e.code)] = t.len;
        for (std::size_t i = 0; i < sizeof(e.ops); ++i) {
            t.data[t.len++] = e.ops[i];
            if (e.ops[i] == OP_END) break;
        }
    }
    return t;
}

template <ProgTable T>
consteval auto trim_prog() {
    std::array<std::uint8_t, T.len> out{};
    for (std::size_t i = 0; i < T.len; ++i) out[i] = T.data[i];
    return out;
}

consteval ProgTable make_title_tbl() {
    constexpr ProgEntry kTitleEntries[] = {
        {Code::PowerShort, {OP_CAP_SNK, S(S_needs), OP_WATTS_A, S(S_comma), OP_SRC, S(S_gives), OP_WATTS_B, S(S_max), 0}},
        {Code::MismatchFlag, {OP_CAP_SNK, S(S_flagged_mm), 0}},
        {Code::NoMismatchFlag, {OP_CAP_SNK, S(S_no_mm), 0}},
        {Code::OverDraw, {OP_CAP_SNK, OP_SEL_D_BIT, 22, S_draws_over, OP_AMPS_A, S(S_on_a), OP_AMPS_B, S(S_contract), 0}},
        {Code::Pinned, {S(S_pinned_t), OP_AMPS_A, S(S_contract_lim), 0}},
        {Code::DrawReduced, {OP_CAP_SNK, OP_SEL_D_BIT, 0xFF, S_drew_less, 0}},
        {Code::ContractUnseen, {S(S_unseen_t), 0}},
        {Code::NoContract, {S(S_nocontract_t), OP_VOLTS_A, 0}},
        {Code::HardReset, {OP_U_PLUR_A, S_hard_1, 0}},
        {Code::SoftReset, {OP_U_PLUR_A, S_soft_1, 0}},
        {Code::Reattach, {S(S_reattach_t), OP_U_A, S(S_x_3s), 0}},
        {Code::Rejected, {S(S_rejected_t), OP_U_A, S(S_x), 0}},
        {Code::WaitLoop, {S(S_wait_t), OP_U_A, S(S_x), 0}},
        {Code::ManyRequests, {OP_U_A, S(S_many_req_t), 0}},
        {Code::NoAccept, {S(S_no_acc_t), OP_U_A, S(S_x), 0}},
        {Code::LateAccept, {S(S_late_acc_t), OP_U_A, S(S_ms_req), 0}},
        {Code::NoPsRdy, {S(S_no_psrdy_t), OP_U_A, S(S_x), 0}},
        {Code::LatePsRdy, {S(S_late_psrdy_t), OP_U_A, S(S_ms_acc), 0}},
        {Code::CapsUnanswered, {S(S_caps_unans_t), 0}},
        {Code::VbusLow, {S(S_vbus_fell), OP_VOLTS2_A, S(S_on_a), OP_VOLTS_B, S(S_contract), 0}},
        {Code::VbusHigh, {S(S_vbus_rose), OP_VOLTS2_A, S(S_on_a), OP_VOLTS_B, S(S_contract), 0}},
        {Code::VbusLost, {S(S_vbus_lost_t), 0}},
        {Code::InputCuts, {OP_CAP_SNK, S(S_cuts_title), 0}},
        {Code::OverSustained, {OP_CAP_SNK, S(S_stays_over), OP_AMPS_A, S(S_on), OP_AMPS_B, 0}},
        {Code::NoGoodCrc, {OP_U_PLUR_A, S_nogc_1, 0}},
        {Code::Retries, {OP_U_PLUR_A, S_retry_1, 0}},
        {Code::TxFail, {OP_U_A, S(S_txfail_t), 0}},
        {Code::RxBad, {OP_U_PLUR_A, S_rxbad_1, 0}},
        {Code::EnterUsbRefused, {S(S_eusb_t), 0}},
        {Code::EprFailed, {S(S_epr_t), 0}},
        {Code::CableCap, {S(S_cablecap_t), 0}},
        {Code::VdmLoop, {OP_VDM, S(S_asked), OP_U_B, S(S_x), 0}},
        {Code::CableSilent, {S(S_cablesil_t), 0}},
        {Code::CaptureGap, {OP_U_PLUR_A, S_gap_1, 0}},
    };
    return build_prog(kTitleEntries);
}

consteval ProgTable make_note_tbl() {
    constexpr ProgEntry kNoteEntries[] = {
        {Code::MismatchFlag, {S(S_mm_n), OP_CLOCK, 0}},
        {Code::NoMismatchFlag, {S(S_nomm_n1), OP_VOLTS_A, S(S_space), OP_AMPS_B, S(S_nomm_n2), 0}},
        {Code::Pinned, {S(S_pinned_n1), OP_U_B, S(S_pct_of), OP_DUR_C, 0}},
        {Code::Draw, {S(S_observed), OP_DUR_D, S(S_peak), OP_AMPS_PEAK, 0}},
        {Code::FiveVoltOnly, {S(S_5v_n1), OP_VOLTS_A, S(S_5v_n2), OP_VOLTS_B, 0}},
        {Code::NoContract, {S(S_nocontract_n), 0}},
        {Code::ActiveSink, {S(S_active_n), 0}},
        {Code::HardReset, {S(S_first_at), OP_CLOCK, 0}},
        {Code::Reattach, {S(S_reattach_n), OP_CLOCK, 0}},
        {Code::Rejected, {S(S_first_at), OP_CLOCK, 0}},
        {Code::WaitLoop, {S(S_wait_n), 0}},
        {Code::ManyRequests, {S(S_many_req_n), 0}},
        {Code::NoAccept, {S(S_no_acc_n), OP_CLOCK, 0}},
        {Code::NoPsRdy, {S(S_first_at), OP_CLOCK, 0}},
        {Code::CapsUnanswered, {OP_U_A, S(S_caps_unans_n), 0}},
        {Code::VbusHigh, {S(S_at), OP_CLOCK, 0}},
        {Code::InputCuts, {S(S_cuts_n1), OP_U_A, S(S_of), OP_U_B, S(S_cuts_n2), OP_CLOCK, S(S_cuts_n3), 0}},
        {Code::NoGoodCrc, {S(S_nogc_n_note), 0}},
        {Code::Retries, {S(S_retry_n_note), 0}},
        {Code::TxFail, {S(S_txfail_n), 0}},
        {Code::RxBad, {S(S_rxbad_n_note), 0}},
        {Code::EnterUsbRefused, {S(S_eusb_n), 0}},
        {Code::EprFailed, {S(S_epr_n), OP_U_A, 0}},
        {Code::CableCap, {S(S_cablecap_n), OP_AMPS_A, 0}},
        {Code::VdmLoop, {S(S_vdmloop_n1), OP_ANS, S(S_vdmloop_n2), 0}},
        {Code::CableSilent, {S(S_cablesil_n1), OP_U_A, S(S_x_paren), OP_U_PLUR_B, S_req_1, S(S_cablesil_n2), 0}},
        {Code::CaptureGap, {S(S_gap_n_note), 0}},
    };
    return build_prog(kNoteEntries);
}

constexpr ProgTable kTitleTbl = make_title_tbl();
constexpr ProgTable kNoteTbl = make_note_tbl();
static_assert(kTitleTbl.len <= 254 && kNoteTbl.len <= 254);
static constexpr auto kTitleOff = kTitleTbl.off;
static constexpr auto kNoteOff = kNoteTbl.off;
static constexpr auto kTitleProg = trim_prog<kTitleTbl>();
static constexpr auto kNoteProg = trim_prog<kNoteTbl>();

[[gnu::noinline]] void emit_prog(const Report& r, const Finding& f, Out& o, const std::uint8_t* p) {
    while (const std::uint8_t op = *p++) {
        if (op < 0xC0) { o.s(kStrs[op - 1]); continue; }
        switch (op) {
        case OP_CAP_SNK: o.cap(r.snk_noun); break;
        case OP_SRC: o.s(r.src_noun); break;
        case OP_WATTS_A: o.watts(f.a); break;
        case OP_WATTS_B: o.watts(f.b); break;
        case OP_AMPS_A: o.amps(f.a); break;
        case OP_AMPS_B: o.amps(f.b); break;
        case OP_AMPS_PEAK: o.amps(r.peak_ma); break;
        case OP_VOLTS_A: o.volts(f.a); break;
        case OP_VOLTS_B: o.volts(f.b); break;
        case OP_VOLTS2_A: o.volts2(f.a); break;
        case OP_U_A: o.u(std::uint32_t(f.a)); break;
        case OP_U_B: o.u(std::uint32_t(f.b)); break;
        case OP_DUR_C: o.dur(std::uint32_t(f.c)); break;
        case OP_DUR_D: o.dur(std::uint32_t(f.d)); break;
        case OP_CLOCK: o.clock(f.t_ms); break;
        case OP_VDM: o.s(vdm_cmd(std::uint32_t(f.a))); break;
        case OP_ANS: o.s(answer_name(std::uint32_t(f.c))); break;
        case OP_U_PLUR_A: o.u(std::uint32_t(f.a)); o.s(kStrs[*p++ + (f.a == 1 ? 0 : 1)]); break;
        case OP_U_PLUR_B: o.u(std::uint32_t(f.b)); o.s(kStrs[*p++ + (f.b == 1 ? 0 : 1)]); break;
        case OP_SEL_D_BIT: {
            const std::uint8_t bit = *p++, idx = *p++;
            const bool set = bit >= 32 ? f.d != 0 : (std::uint32_t(f.d) >> bit & 1) != 0;
            o.s(kStrs[idx + (set ? 1 : 0)]);
            break;
        }
        default: break;
        }
    }
}

void title(const Report& r, const Finding& f, Out& o) {
    const std::uint8_t off = kTitleOff[std::size_t(f.code)];
    if (off != 0xFF) { emit_prog(r, f, o, &kTitleProg[off]); return; }
    switch (f.code) {
    case Code::Draw:
        if (f.a) { o.s(kStrs[S_contract_cap]); o.volts(f.a); o.c(' '); o.amps(f.b); o.s(kStrs[S_draws_c]); o.watts(f.c); }
        else { o.s(K2_P("Draws ")); o.watts(f.c); o.s(kStrs[S_sp_at]); o.volts(r.vbus_mv); }
        break;
    case Code::FiveVoltOnly: o.s(K2_P("5 V contract, ")); o.s(r.snk_noun[0] == 'K' ? "K2" : K2_P("the device")); o.s(K2_P(" can use ")); o.volts(f.b > f.a ? f.a : f.b); break;
    case Code::ActiveSink:
        o.s(K2_P("K2 is the sink: "));
        if (f.a) { o.volts(f.a); o.c(' '); o.amps(f.b); o.s(kStrs[S_contract]); }
        else { o.s(K2_P("contract on PDO ")); o.u(std::uint32_t(f.c)); }   // its PDO is not in the captured offer
        break;
    case Code::SwapRefused: {
        const char* who = f.b ? r.src_noun : r.snk_noun;
        o.cap(who);
        if (f.a == 1) {
            o.s(f.b ? K2_P(" refuses to take power on this port") : K2_P(" refuses to source power on this port"));
        } else {
            o.s(f.a == 2 ? K2_P(" refused a data role swap") : f.a == 4 ? K2_P(" refused a VCONN swap") : K2_P(" refused a fast role swap"));
        }
        break;
    }
    case Code::ModeNak:
        if (f.b) o.s(K2_P("Cable plug refused "));
        o.s(f.a & 1 ? (f.b ? K2_P("DP alt mode") : K2_P("DP alt mode refused")) : f.a & 2 ? (f.b ? K2_P("Thunderbolt mode") : K2_P("Thunderbolt mode refused"))
                    : (f.b ? K2_P("an alt mode") : K2_P("Alt mode refused")));
        break;
    default: break;
    }
}

void note(const Report& r, const Finding& f, Out& o) {
    const std::uint8_t off = kNoteOff[std::size_t(f.code)];
    if (off != 0xFF) { emit_prog(r, f, o, &kNoteProg[off]); return; }
    switch (f.code) {
    case Code::PowerShort:
        o.s(K2_P("need: "));
        if (r.need_from == NeedFrom::SinkCaps) o.s(K2_P("its sink caps on the wire, "));
        else if (r.need_from == NeedFrom::Skedb) { o.s(K2_P("its PDP over PD (Sink_Capabilities_Extended)")); break; }
        else {
            o.s(r.need_from == NeedFrom::AssumedTwin ? K2_P("assumed twin \"") : K2_P("twin \""));
            o.s(r.twin ? r.twin : "?");
            o.s("\", ");
        }
        o.volts(f.c); o.c(' '); o.amps(f.d);
        break;
    case Code::OverDraw: {
        const unsigned bursts = std::uint32_t(f.c) >> 8 & 0xFF, lo = std::uint32_t(f.d) >> 2 & 0x3FF, hi = std::uint32_t(f.d) >> 12 & 0x3FF;
        const auto tenths = [&o](unsigned ds) { o.u(ds / 10); o.c('.'); o.u(ds % 10); };
        // the offer's own maximum, when the peak went past it too (the "far above" title)
        const std::uint32_t offer = (std::uint32_t(f.c) >> 16) * 10;
        if (offer && (f.d >> 22 & 1)) { o.s(K2_P("offer ")); o.amps(std::int32_t(offer)); o.s("; "); }
        const std::int32_t limit = std::int32_t(over_limit_ma(std::uint32_t(f.b)));
        if (bursts > 1) {   // "2 bursts, 1.5-2.4 s, from 2:07"
            o.u(bursts); o.s(K2_P(" bursts, ")); tenths(lo);
            if (hi != lo) { o.c('-'); tenths(hi); }
            o.s(K2_P(" s, from "));
        } else if (bursts) {   // one long stretch: "above 3.46 A for 7.9 s from 0:05"
            o.s(K2_P("above ")); o.amps(limit); o.s(K2_P(" for ")); tenths(hi); o.s(K2_P(" s from "));
        } else {               // a share of the time: "40 % of the time above 3.46 A from 0:12"
            o.u(std::uint32_t(f.c) & 0xFF); o.s(K2_P(" % of the time above ")); o.amps(limit); o.s(K2_P(" from "));
        }
        o.clock(f.t_ms);
        // worst case fits a line: "offer 3.5 A; 100 % of the time above 3.46 A from 10:00; VBUS fell, hard reset"
        if ((f.d & 3) == 3) o.s(K2_P("; VBUS fell, hard reset"));
        else if (f.d & 1) o.s(K2_P("; then VBUS fell"));
        else if (f.d & 2) o.s(K2_P("; then a hard reset"));
        break;
    }
    case Code::DrawReduced:
        o.amps(f.a); o.s(kStrs[S_to]); o.amps(f.b); o.s(kStrs[S_sp_at]); o.clock(f.t_ms);
        if (f.c > 1) { o.s(kStrs[S_comma]); o.u(std::uint32_t(f.c)); o.s(K2_P(" drops")); }
        o.s(f.d ? K2_P("; no PD message, VBUS kept") : K2_P("; PD messages around it"));
        break;
    case Code::ContractUnseen:
        if (f.b) { o.s(K2_P("VBUS ")); o.volts(f.a); o.s(K2_P(" when the K2 saw it")); }
        else o.s(K2_P("no VBUS reading in this capture"));
        break;
    case Code::SoftReset:
        o.s(kStrs[S_first_at]); o.clock(f.t_ms);
        if (f.b) { o.s(kStrs[S_comma]); o.u(std::uint32_t(f.b)); o.s(K2_P(" sent by the K2")); }
        break;
    case Code::LateAccept:
    case Code::LatePsRdy:
        o.s(K2_P("limit "));
        o.u(f.code == Code::LateAccept ? spec::kAcceptLimitMs : f.b ? spec::kPsTransitionEprMaxMs : spec::kPsTransitionMaxMs);
        o.s(" ms");
        break;
    case Code::VbusLow:
        if (f.b) { o.c('-'); o.u(std::uint32_t((f.b - f.a) * 100 + f.b / 2) / std::uint32_t(f.b)); o.s(" %, "); }
        o.s(kStrs[S_at]); o.amps(f.c); o.s(kStrs[S_comma]); o.clock(f.t_ms);
        break;
    case Code::OverSustained:
        o.u(std::uint32_t(f.c) >> 16); o.s(kStrs[S_pct_of]); o.dur(std::uint32_t(f.d)); o.s(K2_P(" above "));
        o.amps(std::int32_t(over_limit_ma(std::uint32_t(f.b)))); o.s(K2_P(" from ")); o.clock(f.t_ms);
        if (f.c & 0xFFFF) { o.s(K2_P("; offer ")); o.amps(f.c & 0xFFFF); }
        break;
    case Code::VbusLost:
        o.s(kStrs[S_at]); o.clock(f.t_ms); o.s(K2_P(", CC still attached"));
        if (f.c && f.b * 1000 > f.c * 1050) { o.s(K2_P("; drew ")); o.amps(f.b); o.s(kStrs[S_on]); o.amps(f.c); o.s(K2_P(": over-current signature")); }
        break;
    case Code::SwapRefused:
        if (f.a == 1) {
            o.s(K2_P("PR_Swap ")); o.s(ctl_answer(std::uint32_t(f.c)));
            if (f.t_ms) { o.s(kStrs[S_sp_at]); o.clock(f.t_ms); }
            if (f.b) { o.s(K2_P("; it stays the ")); o.milli(f.d ? f.d : 5000, 0); o.s(K2_P(" V source")); }
            else o.s(K2_P("; it stays the sink"));
        } else {
            o.s(f.a == 2 ? K2_P("DR_Swap") : f.a == 4 ? K2_P("VCONN_Swap") : K2_P("FR_Swap")); o.s(K2_P(" not accepted"));
        }
        break;
    case Code::ModeNak:
        o.s(f.b ? K2_P("Enter_Mode NAK on SOP' at ") : K2_P("Enter_Mode NAK at ")); o.clock(f.t_ms);
        if (f.b) o.s(K2_P("; the cable plug only, not the port"));
        break;
    default: break;
    }
}

void lanes_note(const Report& r, Out& o) {
    const auto& l = r.lanes;
    o.s(K2_P("agreed over PD: "));
    bool any = false;
    auto sep = [&] { if (any) o.s(", "); any = true; };
    if (l.eusb) { sep(); o.s(l.usb_mode == 2 ? K2_P("USB4 entry") : l.usb_mode == 1 ? K2_P("USB 3 entry") : K2_P("USB 2 entry")); }
    if (l.tbt) { sep(); o.s(K2_P("Thunderbolt mode")); }
    if (l.dp) {
        sep(); o.s(K2_P("DP alt mode"));
        if (l.dp_pin) { o.s(K2_P(" pin ")); o.c(l.dp_pin); }
    }
    if (!any) o.s(K2_P("no alt mode, no USB4"));
}

const char* head_power(std::uint8_t p) {
    static constexpr const char* const kPow[] = {
        "", K2_P("power short"), K2_P("power OK"), K2_P("need not known"), K2_P("contract not observed"),
        K2_P("no contract made"), K2_P("K2 is the sink"), "", K2_P("draw over contract"),
    };
    return p < (sizeof(kPow) / sizeof(kPow[0])) ? kPow[p] : "";
}

void head(const Report& r, Out& o) {
    if (r.empty) { o.s(K2_P("No session yet")); return; }
    if (r.no_pd) {
        o.s(K2_P("No PD seen, VBUS "));
        o.volts(r.vbus_mv);
        return;
    }
    o.s(r.link == 2 ? K2_P("PD link unstable, ") : r.link == 1 ? K2_P("PD link degraded, ") : K2_P("PD link healthy, "));
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
    if (r.quiet_contract && r.quiet_link) { if (idx-- == 0) { o.s(K2_P("No resets, no retries, no rejects")); return true; } }
    if (r.quiet_quality && r.band_checked) { if (idx-- == 0) { o.s(K2_P("VBUS within the contract band")); return true; } }
    if (r.quiet_roles) { if (idx-- == 0) { o.s(K2_P("No refused swaps or modes")); return true; } }
    out.kind = LineKind::Lanes;
    if (idx == 0) { o.s(K2_P("Data lanes pass through the K2, not observed")); return true; }
    if (idx == 1) { out.kind = LineKind::Note; lanes_note(r, o); return true; }
    return false;
}

void verdict(const Report& r, std::span<char> out) {
    Out o{out};
    if (!out.empty()) out[0] = 0;
    head(r, o);
}

namespace detail {

[[gnu::noinline]] void Out::c(char ch) { if (n + 1 < b.size()) { b[n++] = ch; b[n] = 0; } }
[[gnu::noinline]] void Out::s(const char* t) {
    if (!t) return;
    if (k2::strpack::is_packed(t)) {
        k2::strpack::Reader r(t);
        for (int ch; (ch = r.next()) >= 0;) c(static_cast<char>(ch));
        return;
    }
    while (*t) c(*t++);
}
[[gnu::noinline]] void Out::cap(const char* t) {
    if (!t || !*t) return;
    if (k2::strpack::is_packed(t)) {
        char dec[32];
        k2::strpack::decode(t, dec, sizeof(dec));
        cap(dec);
        return;
    }
    c(*t >= 'a' && *t <= 'z' ? char(*t - 32) : *t);
    s(t + 1);
}
[[gnu::noinline]] void Out::unit(const char* t) { if (!tight) c(' '); s(t); }
[[gnu::noinline]] void Out::u(std::uint32_t v) {
    char t[10];
    int k = 0;
    do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
    while (k) c(t[--k]);
}
[[gnu::noinline]] void Out::milli(std::int32_t v, int dec) {
    if (v < 0) { c('-'); v = -v; }
    std::uint32_t scale = dec >= 2 ? 10 : dec == 1 ? 100 : 1000;
    const std::uint32_t q = (std::uint32_t(v) + scale / 2) / scale;
    const std::uint32_t div = dec >= 2 ? 100 : dec == 1 ? 10 : 1;
    u(q / div);
    std::uint32_t f = q % div;
    if (f == 0 || div == 1) return;
    c('.');
    if (div == 100) { c(char('0' + f / 10)); if (f % 10) c(char('0' + f % 10)); }
    else c(char('0' + f));
}
[[gnu::noinline]] void Out::volts(std::int32_t mv) { milli(mv, 1); unit("V"); }
[[gnu::noinline]] void Out::volts2(std::int32_t mv) { milli(mv, 2); unit("V"); }
[[gnu::noinline]] void Out::amps(std::int32_t ma) {
    if (ma < 100) { u(std::uint32_t(ma < 0 ? 0 : ma)); unit("mA"); return; }
    milli(ma, 2);
    unit("A");
}
[[gnu::noinline]] void Out::watts(std::int32_t mw) {
    if (mw < 10000) milli(mw, 1); else u(std::uint32_t(mw + 500) / 1000);
    unit("W");
}
[[gnu::noinline]] void Out::two(std::uint32_t v) { c(char('0' + v / 10 % 10)); c(char('0' + v % 10)); }
[[gnu::noinline]] void Out::clock(std::uint32_t ms) {
    const std::uint32_t t = ms / 1000;
    if (t >= 3600) { u(t / 3600); c(':'); two(t / 60 % 60); }
    else u(t / 60);
    c(':');
    two(t % 60);
}
[[gnu::noinline]] void Out::dur(std::uint32_t ms) {
    const std::uint32_t t = ms / 1000;
    if (t < 60) { u(t); s(tight ? "s" : " s"); return; }
    const bool min_only = t < 3600;
    const std::uint32_t hi = min_only ? t / 60 : t / 3600;
    const std::uint32_t lo = (min_only ? t : t / 60) % 60;
    u(hi);
    if (tight) { c(min_only ? 'm' : 'h'); two(lo); c(min_only ? 's' : 'm'); }
    else { s(min_only ? " min " : " h "); u(lo); s(min_only ? " s" : " min"); }
}

}  // namespace detail

const char* code_name(Code c) {
    static constexpr const char* const kCodeNames[] = {
        "power_short", "mismatch_flag", "no_mismatch_flag", "over_draw", "draw_pinned", "draw", "draw_reduced",
        "five_volt_only", "contract_unseen", "no_contract", "active_sink",
        "hard_reset", "soft_reset", "reattach", "rejected", "wait_loop", "many_requests", "no_accept", "late_accept",
        "no_ps_rdy", "late_ps_rdy", "caps_unanswered",
        "vbus_low", "vbus_high", "vbus_lost", "input_cuts", "over_sustained",
        "no_goodcrc", "retries", "tx_fail", "rx_bad",
        "swap_refused", "mode_nak", "enter_usb_refused", "epr_failed", "cable_cap", "vdm_loop", "cable_silent",
        "capture_gap",
    };
    static_assert(sizeof(kCodeNames) / sizeof(kCodeNames[0]) == static_cast<unsigned>(Code::CaptureGap) + 1);
    const unsigned i = static_cast<unsigned>(c);
    return i < (sizeof(kCodeNames) / sizeof(kCodeNames[0])) ? kCodeNames[i] : "?";
}

}  // namespace k2::pd::diag
