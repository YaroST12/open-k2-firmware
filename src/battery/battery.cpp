// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Battery efficiency recorder, pure logic: see k2/battery/battery.hpp.
#include "k2/battery/battery.hpp"

#include "k2/core/ringlog.hpp"

namespace k2::battery {

namespace {
// The area of one trapezoid of a and b over dt (in their unit x ms), the part above zero to `pos` and the part below to
// `neg`; a step that changes sign is split where the line crosses zero, so the two parts are the two triangles.
void split(std::int64_t a, std::int64_t b, std::int64_t dt, std::uint64_t &pos, std::uint64_t &neg) {
    if (a >= 0 && b >= 0) { pos += std::uint64_t((a + b) * dt / 2); return; }
    if (a <= 0 && b <= 0) { neg += std::uint64_t(-(a + b) * dt / 2); return; }
    const std::int64_t span = a > b ? a - b : b - a;   // |a| + |b|
    const std::int64_t up = a > 0 ? a : b, down = a > 0 ? -b : -a;
    pos += std::uint64_t(up * up * dt / (2 * span));
    neg += std::uint64_t(down * down * dt / (2 * span));
}
}  // namespace

void integrate(Flow &f, std::int32_t mv0, std::int32_t ma0, std::int32_t mv1, std::int32_t ma1, std::uint32_t dt_ms) {
    // mW = mV x mA / 1000; the power changes sign with the current (VBUS is never negative).
    const std::int64_t p0 = std::int64_t(mv0) * ma0 / 1000, p1 = std::int64_t(mv1) * ma1 / 1000;
    split(p0, p1, dt_ms, f.uj_fwd, f.uj_rev);
    split(ma0, ma1, dt_ms, f.uc_fwd, f.uc_rev);
}

// ---- Curve -----------------------------------------------------------------------------------------------------------------

void Curve::pack(std::uint8_t *p, std::int32_t mv, std::int32_t ma) {
    std::int32_t v = (mv + 5) / 10, i = ma >= 0 ? (ma + 2) / 4 : -((-ma + 2) / 4);
    v = v < 0 ? 0 : v > 4095 ? 4095 : v;
    i = i < -2048 ? -2048 : i > 2047 ? 2047 : i;
    const std::uint32_t u = std::uint32_t(i) & 0xFFFu;
    p[0] = std::uint8_t(v);
    p[1] = std::uint8_t(v >> 8 | (u & 0xF) << 4);
    p[2] = std::uint8_t(u >> 4);
}

void Curve::unpack(const std::uint8_t *p, std::int32_t &mv, std::int32_t &ma) {
    mv = (p[0] | (p[1] & 0xF) << 8) * 10;
    std::int32_t i = p[1] >> 4 | p[2] << 4;
    if (i & 0x800) i -= 0x1000;
    ma = i * 4;
}

void Curve::point(int i, std::int32_t &mv, std::int32_t &ma) const {
    if (i < 0 || i >= n) { mv = ma = 0; return; }
    unpack(&pts[std::size_t(i) * 3], mv, ma);
}

void Curve::add(std::uint32_t dt_ms, std::int32_t mv, std::int32_t ma) {
    acc_mv_ms += std::int64_t(mv) * dt_ms;
    acc_ma_ms += std::int64_t(ma) * dt_ms;
    acc_ms += dt_ms;
    if (acc_ms < std::uint32_t(interval_s) * 1000u) return;
    if (n == kPoints) {   // full: every pair becomes one point of twice the interval
        if (interval_s > 0x7FFF) { acc_mv_ms = acc_ma_ms = 0; acc_ms = 0; return; }   // ~22 days: the curve stops here
        for (int k = 0; k < kPoints / 2; k++) {
            std::int32_t v0, i0, v1, i1;
            unpack(&pts[std::size_t(2 * k) * 3], v0, i0);
            unpack(&pts[std::size_t(2 * k + 1) * 3], v1, i1);
            pack(&pts[std::size_t(k) * 3], (v0 + v1) / 2, (i0 + i1) / 2);
        }
        n = kPoints / 2;
        if (split != 0xFF) split = std::uint8_t(split / 2);
        interval_s = std::uint16_t(interval_s * 2);
        if (acc_ms < std::uint32_t(interval_s) * 1000u) return;   // the open point now runs to the new interval
    }
    pack(&pts[std::size_t(n) * 3], std::int32_t(acc_mv_ms / acc_ms), std::int32_t(acc_ma_ms / acc_ms));
    n++;
    acc_mv_ms = acc_ma_ms = 0;
    acc_ms = 0;
}

// ---- Recorder --------------------------------------------------------------------------------------------------------------

Recorder::Start Recorder::start(Mode m, Wiring w, const Config &c, std::uint16_t id) {
    if (active()) return Start::Busy;
    if (!valid(c) || static_cast<std::uint8_t>(m) > 2 || static_cast<std::uint8_t>(w) > 1) return Start::BadArg;
    *this = Recorder{};
    mode_ = m;
    wiring_ = w;
    cfg_ = c;
    id_ = id;
    begin_phase(m == Mode::Discharge ? Kind::Discharge : Kind::Charge);
    return Start::Ok;
}

void Recorder::begin_phase(Kind k) {
    Phase &p = ph_[phases_ < 2 ? phases_ : 1];
    if (phases_ < 2) phases_++;
    p.kind = k;
    p.end = End::Running;
    if (phases_ == 2) curve_.mark_split();
    state_ = State::Measuring;
    armed_ = quiet_ = false;
    dirty_ = true;
}

void Recorder::close_phase(End e, std::uint32_t tail_ms) {
    Phase &p = ph_[current()];
    p.end = e;
    p.ms = p.ms > tail_ms ? p.ms - tail_ms : 0;
    const bool more = mode_ == Mode::RoundTrip && phases_ == 1;
    state_ = more ? State::Between : State::Done;
    armed_ = quiet_ = false;
    primed_ = false;
    if (more) ph_[1] = Phase{.kind = Kind::Discharge};
    dirty_ = true;
}

std::int32_t Recorder::out_ma(std::int32_t ma) const { return wiring_ == Wiring::Moved ? ma : -ma; }

// One integration step into phase p: both directions, mapped to the bank's in and out, and the curve.
void Recorder::step(std::uint32_t dt, std::int32_t mv, std::int32_t ma, Phase &p) {
    p.ms += dt;
    if (dt > kGapMs) { p.gaps++; return; }
    Flow f;
    integrate(f, last_mv_, last_ma_, mv, ma, dt);
    const bool fwd_in = in_is_forward(p.kind);
    p.uj_in += fwd_in ? f.uj_fwd : f.uj_rev;
    p.uj_out += fwd_in ? f.uj_rev : f.uj_fwd;
    p.uc_in += fwd_in ? f.uc_fwd : f.uc_rev;
    p.uc_out += fwd_in ? f.uc_rev : f.uc_fwd;
    curve_.add(dt, (last_mv_ + mv) / 2, (last_ma_ + ma) / 2);
}

void Recorder::sample(std::uint32_t now_ms, std::int32_t mv, std::int32_t ma, bool online) {
    if (!active()) return;
    if (!online) { primed_ = false; return; }
    ma = deadband(ma);
    if (!primed_) {
        primed_ = true;
        last_ms_ = now_ms;
        last_mv_ = mv;
        last_ma_ = ma;
        return;
    }
    const std::uint32_t dt = now_ms - last_ms_;
    const std::uint32_t mag = ma < 0 ? std::uint32_t(-ma) : std::uint32_t(ma);
    if (state_ == State::Measuring) {
        step(dt, mv, ma, ph_[current()]);
        if (mag >= cfg_.end_ma) {
            armed_ = true;
            quiet_ = false;
        } else if (armed_ && !quiet_) {
            quiet_ = true;
            quiet_ms_ = now_ms;
        } else if (armed_ && now_ms - quiet_ms_ >= std::uint32_t(cfg_.end_s) * 1000u) {
            close_phase(End::Auto, now_ms - quiet_ms_);
        }
    } else {   // Between: wait for the swap, then for the outflow to hold
        if (!armed_) {
            armed_ = wiring_ == Wiring::Moved ? mv < kVbusGoneMv : mag < cfg_.end_ma;
        } else if (out_ma(ma) >= std::int32_t(cfg_.end_ma)) {
            if (!quiet_) { quiet_ = true; quiet_ms_ = last_ms_; }
            step(dt, mv, ma, ph_[1]);   // counted from the first outflow sample
            if (now_ms - quiet_ms_ >= kStartHoldMs) {
                begin_phase(Kind::Discharge);   // keeps what the hold counted
                armed_ = true;                  // it flows: the end rule is armed
            }
        } else if (quiet_) {   // the outflow stopped before it held: what it counted goes
            quiet_ = false;
            ph_[1] = Phase{.kind = Kind::Discharge};
            curve_.acc_mv_ms = curve_.acc_ma_ms = 0;
            curve_.acc_ms = 0;
        }
    }
    last_ms_ = now_ms;
    last_mv_ = mv;
    last_ma_ = ma;
}

bool Recorder::end_phase() {
    if (state_ != State::Measuring) return false;
    close_phase(End::Manual, 0);
    return true;
}

bool Recorder::next() {
    if (state_ != State::Between) return false;
    begin_phase(Kind::Discharge);   // an outflow that had not held yet still counts
    return true;
}

bool Recorder::finish() {
    if (state_ == State::Measuring) close_phase(End::Manual, 0);
    if (state_ == State::Between) {
        ph_[1] = Phase{};
        state_ = State::Done;
        dirty_ = true;
    }
    return state_ == State::Done;
}

void Recorder::to_record(Record &r) const {
    r = Record{};
    r.mode = mode_;
    r.wiring = wiring_;
    r.state = state_ == State::Measuring ? RunState::Measuring : state_ == State::Between ? RunState::Between : RunState::Done;
    r.phases = phases_;
    r.resumes = resumes_;
    r.id = id_;
    r.cfg = cfg_;
    r.interval_s = curve_.interval_s;
    r.points = curve_.n;
    r.split = curve_.split;
    for (int i = 0; i < phases_; i++) {
        const Phase &p = ph_[i];
        r.ph[i] = PhaseRec{p.kind, p.end, p.gaps, p.ms / 1000u, std::uint32_t(p.uj_in / 3600u), std::uint32_t(p.uj_out / 3600u),
                           std::uint32_t(p.uc_in / 3600u), std::uint32_t(p.uc_out / 3600u)};
    }
    r.pts = curve_.pts;
}

bool Recorder::restore(const Record &r) {
    if (r.phases == 0 || r.phases > 2 || r.id == 0) return false;
    *this = Recorder{};
    mode_ = r.mode;
    wiring_ = r.wiring;
    cfg_ = r.cfg;
    id_ = r.id;
    phases_ = r.phases;
    resumes_ = r.resumes;
    for (int i = 0; i < r.phases; i++) {
        const PhaseRec &q = r.ph[i];
        ph_[i] = Phase{q.kind, q.end, q.gaps, q.secs * 1000u, std::uint64_t(q.uwh_in) * 3600u, std::uint64_t(q.uwh_out) * 3600u,
                       std::uint64_t(q.uah_in) * 3600u, std::uint64_t(q.uah_out) * 3600u};
    }
    curve_.pts = r.pts;
    curve_.n = r.points;
    curve_.split = r.split;
    curve_.interval_s = r.interval_s;
    switch (r.state) {
    case RunState::Between:
        state_ = State::Between;
        ph_[1] = Phase{.kind = Kind::Discharge};
        armed_ = wiring_ == Wiring::Moved;   // a moved bank has been unplugged by the time the K2 boots again
        if (resumes_ < 63) resumes_++;
        break;
    case RunState::Measuring:
        ph_[current()].end = End::Lost;      // what it measured since the phase began was never written
        state_ = State::Done;
        dirty_ = true;
        break;
    case RunState::Done:
        state_ = State::Done;
        break;
    }
    return true;
}

namespace {
void put16(std::uint8_t *p, std::uint32_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
void put32(std::uint8_t *p, std::uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
std::uint16_t get16(const std::uint8_t *p) { return std::uint16_t(p[0] | p[1] << 8); }
std::uint32_t get32(const std::uint8_t *p) { return get16(p) | std::uint32_t(get16(p + 2)) << 16; }
}  // namespace

std::size_t encode(const Record &r, std::span<std::uint8_t> out) {
    if (out.size() < kRecordBytes) return 0;
    std::uint8_t *b = out.data();
    for (std::size_t i = 0; i < kRecordBytes; i++) b[i] = 0;
    b[0] = 'B';
    b[1] = 'R';
    b[2] = kRecordVersion;
    b[3] = std::uint8_t(std::uint8_t(r.mode) | std::uint8_t(r.wiring) << 4);
    b[4] = std::uint8_t(r.state);
    b[5] = std::uint8_t((r.phases & 3) | (r.resumes < 63 ? r.resumes : 63) << 2);
    put16(b + 6, r.id);
    put16(b + 8, r.cfg.end_ma);
    put16(b + 10, r.cfg.end_s);
    put16(b + 12, r.interval_s);
    b[14] = r.points;
    b[15] = r.split;
    for (int i = 0; i < 2; i++) {
        std::uint8_t *q = b + 16 + i * kPhaseBytes;
        const PhaseRec &p = r.ph[i];
        q[0] = std::uint8_t(p.kind);
        q[1] = std::uint8_t(p.end);
        put16(q + 2, p.gaps);
        put32(q + 4, p.secs);
        put32(q + 8, p.uwh_in);
        put32(q + 12, p.uwh_out);
        put32(q + 16, p.uah_in);
        put32(q + 20, p.uah_out);
    }
    for (std::size_t i = 0; i < r.pts.size(); i++) b[kHeadBytes + i] = r.pts[i];
    put16(b + kRecordBytes - 2, ringlog::crc16(0xFFFF, {b, kRecordBytes - 2}));
    return kRecordBytes;
}

bool decode(std::span<const std::uint8_t> in, Record &r) {
    if (in.size() != kRecordBytes) return false;
    const std::uint8_t *b = in.data();
    if (b[0] != 'B' || b[1] != 'R' || b[2] != kRecordVersion) return false;
    if (get16(b + kRecordBytes - 2) != ringlog::crc16(0xFFFF, {b, kRecordBytes - 2})) return false;
    if ((b[3] & 0xF) > 2 || (b[3] >> 4) > 1 || b[4] > 2 || (b[5] & 3) > 2 || b[14] > Curve::kPoints) return false;
    r = Record{};
    r.mode = Mode(b[3] & 0xF);
    r.wiring = Wiring(b[3] >> 4);
    r.state = RunState(b[4]);
    r.phases = b[5] & 3;
    r.resumes = b[5] >> 2;
    r.id = get16(b + 6);
    r.cfg = Config{get16(b + 8), get16(b + 10)};
    r.interval_s = get16(b + 12);
    r.points = b[14];
    r.split = b[15];
    for (int i = 0; i < 2; i++) {
        const std::uint8_t *q = b + 16 + i * kPhaseBytes;
        if (q[0] > 1 || q[1] > 3) return false;
        r.ph[i] = PhaseRec{Kind(q[0]), End(q[1]), get16(q + 2), get32(q + 4), get32(q + 8), get32(q + 12), get32(q + 16), get32(q + 20)};
    }
    for (std::size_t i = 0; i < r.pts.size(); i++) r.pts[i] = b[kHeadBytes + i];
    return true;
}

Sums sums(const Record &r) {
    Sums s;
    for (int i = 0; i < r.phases && i < 2; i++) {
        s.uwh_in += r.ph[i].uwh_in;
        s.uwh_out += r.ph[i].uwh_out;
        s.uah_in += r.ph[i].uah_in;
        s.uah_out += r.ph[i].uah_out;
        s.secs += r.ph[i].secs;
    }
    return s;
}

std::uint16_t ratio_permille(std::uint64_t out, std::uint64_t in) {
    if (in == 0) return kNoRatio;
    const std::uint64_t p = (out * 1000u + in / 2) / in;
    return p >= kNoRatio ? kNoRatio - 1 : std::uint16_t(p);
}

bool has_efficiency(const Record &r) {
    return r.mode == Mode::RoundTrip && r.phases == 2 && r.ph[1].end != End::Running && r.ph[1].end != End::Lost && sums(r).uwh_in > 0;
}

void Recorder::resumed() {
    if (!active()) return;
    primed_ = false;
    if (resumes_ < 63) resumes_++;
    if (state_ == State::Measuring) ph_[current()].gaps++;
    // Not dirty: the record on flash already says "running" with this id, and a write now would land mid-measurement. The
    // resume count reaches flash with the next phase boundary.
}

}  // namespace k2::battery
