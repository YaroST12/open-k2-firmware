// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID-cable loop probe, pure half: see k2/loopprobe/loop_probe.hpp.
#include "k2/loopprobe/loop_probe.hpp"

#include <algorithm>
#include <cstring>

#include "k2/core/bbstore.hpp"

namespace k2::loopprobe {

// ---- Acc --------------------------------------------------------------------------------------------------------------------

void Acc::add(std::int32_t v) {
    if (n == 0) lo = hi = v;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    sum += v;
    if (n < 0xFFFF) n++;
}

namespace {
std::int16_t sat16(std::int32_t v) { return std::int16_t(std::clamp<std::int32_t>(v, -32768, 32767)); }
}  // namespace

Stat Acc::stat() const {
    if (n == 0) return {};
    const std::int32_t half = std::int32_t(n / 2);
    const std::int32_t avg = sum >= 0 ? (sum + half) / std::int32_t(n) : (sum - half) / std::int32_t(n);
    return {sat16(avg), sat16(lo), sat16(hi)};
}

// ---- the layout ---------------------------------------------------------------------------------------------------------------
// One walk over the fields serves both directions: Put reads the record into bytes, Get writes the bytes into the record.

namespace {

struct Put {
    std::uint8_t* p;
    void u8(const std::uint8_t& v) { *p++ = v; }
    void u16(const std::uint16_t& v) { *p++ = std::uint8_t(v); *p++ = std::uint8_t(v >> 8); }
    void i16(const std::int16_t& v) { u16(std::uint16_t(v)); }
    void u32(const std::uint32_t& v) { u16(std::uint16_t(v)); u16(std::uint16_t(v >> 16)); }
    void chars(const char* s, std::size_t n) { for (std::size_t i = 0; i < n; i++) *p++ = std::uint8_t(s[i]); }
};

struct Get {
    const std::uint8_t* p;
    void u8(std::uint8_t& v) { v = *p++; }
    void u16(std::uint16_t& v) { v = std::uint16_t(p[0] | p[1] << 8); p += 2; }
    void i16(std::int16_t& v) { std::uint16_t u; u16(u); v = std::int16_t(u); }
    void u32(std::uint32_t& v) { std::uint16_t a, b; u16(a); u16(b); v = a | std::uint32_t(b) << 16; }
    void chars(char* s, std::size_t n) { for (std::size_t i = 0; i < n; i++) s[i] = char(*p++); }
};

template <class IO, class S>
void stat(IO& io, S& s) {
    io.i16(s.avg);
    io.i16(s.min);
    io.i16(s.max);
}

template <class IO, class H>
void hid(IO& io, H& h) {
    io.u8(h.bits);
    io.u16(h.frame);
    io.u16(h.resets);
}

// Everything after the version byte, in the order of the layout comment in the header.
template <class IO, class R>
void walk(IO& io, R& r) {
    io.u8(r.flags);
    io.chars(r.fw.data(), kFwLen);
    io.u32(r.start_ms);
    for (auto& s : r.step) {
        io.u8(s.status);
        io.u8(s.why);
        io.u32(s.t_ms);
        io.u16(s.dur_ms);
    }
    io.u8(r.base_n);
    for (auto& s : r.base) stat(io, s);
    hid(io, r.hid0);
    hid(io, r.hid1);
    io.u8(r.link.active_cc);
    io.u8(r.link.flags);
    io.u8(r.link.guard);
    io.u8(r.bc);
    io.u8(r.drop_n);
    for (auto& d : r.drop) {
        io.u8(d.load);
        io.u8(d.n);
        stat(io, d.vbus);
        stat(io, d.ibus);
        io.i16(d.aux);
    }
    io.u8(r.dp_n);
    for (auto& d : r.dp) {
        io.u8(d.drive);
        io.i16(d.dp_mv);
        io.i16(d.dm_mv);
        io.u8(d.hid);
        io.i16(d.vbus_max);
    }
    io.u8(r.pull_method);
    io.u8(r.pull_n);
    for (auto& p : r.pull) {
        io.u8(p.on);
        io.i16(p.dp_mv);
        io.i16(p.dm_mv);
        io.u8(p.hid);
    }
    io.u8(r.emk.valid);
    io.u8(r.emk.num_vdos);
    for (auto& v : r.emk.vdos) io.u32(v);
    io.u16(r.emk.seq);
    io.u8(r.emk.read_mode);
    io.u8(r.cc.refusal);
    io.u8(r.cc.sw0);
    io.u8(r.cc.pc);
    io.u8(r.cc.flags);
    io.u8(r.cc.n);
    for (auto& c : r.cc.rows) {
        io.u8(c.cfg);
        io.u16(c.cc1_raw);
        io.u16(c.cc2_raw);
        io.u8(c.bc);
        io.u16(c.vbus_mv);
    }
}

}  // namespace

std::size_t encode(const Record& r, std::span<std::uint8_t> out) {
    if (out.size() < kRecordBytes) return 0;
    Put w{out.data()};
    const std::uint8_t v = kVersion;
    w.u8(v);
    walk(w, r);
    return std::size_t(w.p - out.data());
}

std::optional<Record> decode(std::span<const std::uint8_t> in) {
    if (in.size() != kRecordBytes || in[0] != kVersion) return std::nullopt;
    Record r;
    Get g{in.data() + 1};
    walk(g, r);
    r.fw[kFwLen - 1] = 0;
    return r;
}

std::size_t encode_file(const Record& r, std::span<std::uint8_t> out) {
    if (out.size() < kFileBytes) return 0;
    const std::size_t n = encode(r, out.subspan(kFileHeader));
    const std::uint16_t crc = bbstore::crc16(0xFFFF, out.subspan(kFileHeader, n));
    out[0] = 'L'; out[1] = 'P'; out[2] = 'R'; out[3] = kVersion;
    out[4] = std::uint8_t(n); out[5] = std::uint8_t(n >> 8);
    out[6] = std::uint8_t(crc); out[7] = std::uint8_t(crc >> 8);
    return kFileHeader + n;
}

std::optional<Record> decode_file(std::span<const std::uint8_t> in) {
    if (in.size() < kFileHeader || in[0] != 'L' || in[1] != 'P' || in[2] != 'R' || in[3] != kVersion) return std::nullopt;
    const std::size_t n = std::size_t(in[4] | in[5] << 8);
    if (in.size() != kFileHeader + n) return std::nullopt;
    const auto rec = in.subspan(kFileHeader, n);
    if (bbstore::crc16(0xFFFF, rec) != std::uint16_t(in[6] | in[7] << 8)) return std::nullopt;
    return decode(rec);
}

// ---- store ----------------------------------------------------------------------------------------------------------------------

bool save(Store& s, const Record& r) {
    std::array<std::uint8_t, kFileBytes> f{};
    const std::size_t n = encode_file(r, f);
    return n != 0 && s.write(std::span<const std::uint8_t>(f.data(), n));
}

std::optional<Record> load(Store& s) {
    std::array<std::uint8_t, kFileBytes + 1> f{};
    std::size_t n = 0;
    if (!s.read(f, n)) return std::nullopt;
    return decode_file(std::span<const std::uint8_t>(f.data(), n));
}

std::optional<Step> boot_check(Store& s) {
    auto r = load(s);
    if (!r || !(r->flags & kFlagInProgress)) return std::nullopt;
    r->flags &= std::uint8_t(~kFlagInProgress);
    Record::StepRec& cc = r->at(Step::Cc);
    cc.status = std::uint8_t(Status::Interrupted);
    cc.why = std::uint8_t(Why::Rebooted);
    save(s, *r);
    return Step::Cc;
}

// ---- runner -----------------------------------------------------------------------------------------------------------------------

Start Runner::start(std::uint32_t now, const char* fw) {
    if (running()) return Start::Running;
    const Sample s = env_.sample();
    if (!s.pm_ok) return Start::NoMonitor;
    if (s.vbus_mv > kMaxVbusMv) return Start::VbusHigh;
    if (s.adc_ok && s.aux_mv > kMaxAuxMv) return Start::AuxHigh;

    rec_ = {};
    for (std::size_t i = 0; fw && fw[i] && i < kFwLen - 1; i++) rec_.fw[i] = fw[i];
    rec_.start_ms = now;
    done_ = drove_ = pulled_ = loaded_ = false;
    acc_ = {};
    begin(Step::Baseline, now);
    rec_.hid0 = env_.hid();
    phase_ = Phase::Baseline;
    // The start sample is the first baseline sample.
    acc_[kVbus].add(s.vbus_mv);
    acc_[kIbus].add(s.ibus_ma);
    if (s.adc_ok) {
        acc_[kAux].add(s.aux_mv); acc_[kDp].add(s.dp_mv); acc_[kDm].add(s.dm_mv);
        acc_[kCc1].add(s.cc1_mv); acc_[kCc2].add(s.cc2_mv);
    }
    idx_ = 1;
    last_sample_ = now;
    return Start::Ok;
}

void Runner::begin(Step s, std::uint32_t now) {
    step_ = s;
    rec_.at(s).t_ms = now - rec_.start_ms;
}

void Runner::end(Step s, Status st, Why why, std::uint32_t now) {
    Record::StepRec& r = rec_.at(s);
    r.status = std::uint8_t(st);
    r.why = std::uint8_t(why);
    r.dur_ms = std::uint16_t(std::min<std::uint32_t>(now - rec_.start_ms - r.t_ms, 0xFFFF));
}

void Runner::restore() {
    if (drove_) { env_.drive({dpdm::Drive::V0, dpdm::Drive::V0}); env_.dpdm_release(); drove_ = false; }
    if (pulled_) { env_.pull_restore(); pulled_ = false; }
    if (loaded_) { env_.load(Load::BlOn); loaded_ = false; }
}

void Runner::abort(Why why, std::uint32_t now) {
    restore();
    end(step_, Status::Aborted, why, now);
    rec_.flags &= std::uint8_t(~kFlagInProgress);
    phase_ = Phase::Idle;
    done_ = true;
    env_.save(rec_);
}

void Runner::cancel(std::uint32_t now) {
    if (running()) abort(Why::Cancelled, now);
}

void Runner::finish(std::uint32_t) {
    rec_.flags &= std::uint8_t(~kFlagInProgress);
    phase_ = Phase::Idle;
    done_ = true;
    env_.save(rec_);
}

bool Runner::guard(const Sample& s, std::uint32_t now) {
    if (!s.pm_ok) { abort(Why::NoMonitor, now); return false; }
    if (s.vbus_mv > kMaxVbusMv) { abort(Why::VbusHigh, now); return false; }
    if (s.adc_ok && s.aux_mv > kMaxAuxMv) { abort(Why::AuxHigh, now); return false; }
    return true;
}

bool Runner::guard_fast(std::uint32_t now) {
    const std::int32_t v = env_.vbus_fast_mv();
    vmax_ = std::max(vmax_, v);
    if (v > kMaxVbusMv) { abort(Why::VbusHigh, now); return false; }
    return true;
}

bool Runner::host_on_hid() const {
    const bool sof = (rec_.hid0.bits & kHidCore) && (rec_.hid1.bits & kHidCore) && rec_.hid0.frame != rec_.hid1.frame;
    return ((rec_.hid0.bits | rec_.hid1.bits) & kHidConfigured) || sof;
}

void Runner::next_after(Step s, std::uint32_t now) {
    switch (s) {
    case Step::Baseline: start_drop(now); break;
    case Step::Drop: start_dp(now); break;
    case Step::DpDm: start_emarker(now); break;
    case Step::Emarker: start_cc(now); break;
    case Step::Cc: finish(now); break;
    }
}

void Runner::start_drop(std::uint32_t now) {
    begin(Step::Drop, now);
    if (rec_.base_n == 0 || rec_.base[kVbus].avg < kVbusPresentMv) {
        end(Step::Drop, Status::Skipped, Why::NoVbus, now);
        next_after(Step::Drop, now);
        return;
    }
    idx_ = 0;
    env_.load(kDropPlan[0]);
    loaded_ = true;
    t_ = now;
    phase_ = Phase::DropSettle;
}

void Runner::start_dp(std::uint32_t now) {
    begin(Step::DpDm, now);
    if (host_on_hid()) {
        end(Step::DpDm, Status::Skipped, Why::HostOnHid, now);
        next_after(Step::DpDm, now);
        return;
    }
    idx_ = 0;
    vmax_ = 0;
    env_.drive({dpdm::Drive::V0, dpdm::Drive::V0});
    drove_ = true;
    t_ = now;
    phase_ = Phase::DpDischarge;
}

void Runner::start_emarker(std::uint32_t now) {
    begin(Step::Emarker, now);
    rec_.emk = env_.emarker();
    if (rec_.emk.valid) end(Step::Emarker, Status::Done, Why::None, now);
    else end(Step::Emarker, Status::Skipped, Why::NoEmarker, now);
    next_after(Step::Emarker, now);
}

void Runner::start_cc(std::uint32_t now) {
    begin(Step::Cc, now);
    phase_ = Phase::Cc;
    const std::int32_t v = env_.vbus_fast_mv();
    Status st = Status::Done;
    Why why = Why::None;
    if (host_on_hid()) { st = Status::Skipped; why = Why::HostOnHid; }
    else if (v > kMaxVbusMv) { abort(Why::VbusHigh, now); return; }
    else if (v < kCcMinVbusMv) { st = Status::Skipped; why = Why::NoVbus; }
    else {
        rec_.flags |= kFlagInProgress;
        if (!env_.save(rec_)) {
            rec_.flags &= std::uint8_t(~kFlagInProgress);
            st = Status::Skipped;
            why = Why::StoreFailed;
        } else {
            env_.cc_hold(kCcPlan, kCcHoldUs, rec_.cc);
            if (rec_.cc.refusal != 0) { st = Status::Skipped; why = Why::CcRefused; }
            else if (rec_.cc.flags & kCcAborted) { st = Status::Aborted; why = Why::VbusHigh; }
        }
    }
    end(Step::Cc, st, why, now);
    finish(now);
}

void Runner::poll(std::uint32_t now) {
    switch (phase_) {
    case Phase::Idle:
    case Phase::Cc:
        return;

    case Phase::Baseline: {
        if (now - last_sample_ < kSampleGapMs) return;
        last_sample_ = now;
        const Sample s = env_.sample();
        if (!guard(s, now)) return;
        acc_[kVbus].add(s.vbus_mv);
        acc_[kIbus].add(s.ibus_ma);
        if (s.adc_ok) {
            acc_[kAux].add(s.aux_mv); acc_[kDp].add(s.dp_mv); acc_[kDm].add(s.dm_mv);
            acc_[kCc1].add(s.cc1_mv); acc_[kCc2].add(s.cc2_mv);
        }
        if (++idx_ < kBaseSamples) return;
        rec_.base_n = idx_;
        for (std::size_t c = 0; c < kChannels; c++) rec_.base[c] = acc_[c].stat();
        rec_.hid1 = env_.hid();
        rec_.link = env_.link();
        static constexpr std::uint8_t kAsFound[] = {kCcAsFound};
        CcResult look;
        env_.cc_hold(kAsFound, kCcHoldUs, look);
        if (look.refusal == 0 && look.n == 1) rec_.bc = look.rows[0].bc;
        end(Step::Baseline, Status::Done, Why::None, now);
        next_after(Step::Baseline, now);
        return;
    }

    case Phase::DropSettle:
        if (!guard_fast(now) || now - t_ < kDropSettleMs) return;
        phase_ = Phase::DropSample;
        sub_ = 0;
        acc_ = {};
        last_sample_ = now - kSampleGapMs;
        [[fallthrough]];

    case Phase::DropSample: {
        if (now - last_sample_ < kSampleGapMs) return;
        last_sample_ = now;
        const Load l = kDropPlan[idx_];
        if (busy(l)) env_.spin(kSpinMs);
        const Sample s = env_.sample();
        if (!guard(s, now)) return;
        acc_[kVbus].add(s.vbus_mv);
        acc_[kIbus].add(s.ibus_ma);
        if (s.adc_ok) acc_[kAux].add(s.aux_mv);
        if (++sub_ < kDropSamples) return;
        DropPoint& p = rec_.drop[idx_];
        p.load = std::uint8_t(l);
        p.n = sub_;
        p.vbus = acc_[kVbus].stat();
        p.ibus = acc_[kIbus].stat();
        p.aux = acc_[kAux].stat().avg;
        rec_.drop_n = ++idx_;
        if (idx_ < kDropPlan.size()) {
            env_.load(kDropPlan[idx_]);
            t_ = now;
            phase_ = Phase::DropSettle;
            return;
        }
        env_.load(Load::BlOn);
        loaded_ = false;
        end(Step::Drop, Status::Done, Why::None, now);
        next_after(Step::Drop, now);
        return;
    }

    case Phase::DpDischarge:
        if (!guard_fast(now) || now - t_ < kDischargeMs) return;
        env_.drive(kDpPlan[idx_]);
        t_ = now;
        vmax_ = env_.vbus_fast_mv();
        phase_ = Phase::DpHold;
        return;

    case Phase::DpHold: {
        if (!guard_fast(now) || now - t_ < kHoldMs) return;
        DpRow& row = rec_.dp[idx_];
        row.drive = std::uint8_t(std::uint8_t(kDpPlan[idx_].dp) << 4 | std::uint8_t(kDpPlan[idx_].dm));
        std::int32_t dp = 0, dm = 0;
        if (env_.bus_dpdm(dp, dm)) { row.dp_mv = sat16(dp); row.dm_mv = sat16(dm); }
        row.hid = env_.hid().bits;
        row.vbus_max = sat16(vmax_);
        rec_.dp_n = ++idx_;
        t_ = now;
        if (idx_ < kDpPlan.size()) {
            env_.drive({dpdm::Drive::V0, dpdm::Drive::V0});
            phase_ = Phase::DpDischarge;
            return;
        }
        // Reverse direction: the bus lines released, the HID D+ pull-up off, then on.
        env_.drive({dpdm::Drive::HiZ, dpdm::Drive::HiZ});
        sub_ = 0;
        rec_.pull_method = std::uint8_t(env_.pull(false));
        pulled_ = true;
        phase_ = Phase::PullSettle;
        return;
    }

    case Phase::PullSettle: {
        if (!guard_fast(now) || now - t_ < kPullSettleMs) return;
        PullRow& row = rec_.pull[sub_];
        row.on = sub_;
        std::int32_t dp = 0, dm = 0;
        if (env_.bus_dpdm(dp, dm)) { row.dp_mv = sat16(dp); row.dm_mv = sat16(dm); }
        row.hid = env_.hid().bits;
        rec_.pull_n = ++sub_;
        t_ = now;
        if (sub_ < rec_.pull.size()) {
            env_.pull(true);
            return;
        }
        env_.pull_restore();
        pulled_ = false;
        env_.dpdm_release();
        drove_ = false;
        end(Step::DpDm, Status::Done, Why::None, now);
        next_after(Step::DpDm, now);
        return;
    }
    }
}

// ---- names --------------------------------------------------------------------------------------------------------------------------

const char* step_name(Step s) {
    switch (s) {
    case Step::Baseline: return "Baseline";
    case Step::Drop: return "Drop";
    case Step::DpDm: return "D+/D-";
    case Step::Emarker: return "E-marker";
    case Step::Cc: return "CC";
    }
    return "?";
}

const char* why_name(Why w) {
    switch (w) {
    case Why::None: return "";
    case Why::NoMonitor: return "no monitor";
    case Why::VbusHigh: return "VBUS above 5.5 V";
    case Why::AuxHigh: return "aux above 5.8 V";
    case Why::NoVbus: return "no 5 V";
    case Why::HostOnHid: return "host on HID";
    case Why::CcRefused: return "refused";
    case Why::NoEmarker: return "none sniffed";
    case Why::Cancelled: return "cancelled";
    case Why::StoreFailed: return "store failed";
    case Why::Rebooted: return "rebooted";
    }
    return "?";
}

}  // namespace k2::loopprobe
