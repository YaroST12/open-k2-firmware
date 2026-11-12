// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The battery recorder over the remote protocol: see k2/battery/battery_wire.hpp.
#include "k2/battery/battery_wire.hpp"

#include <algorithm>

#include "k2/battery/battery.hpp"

namespace k2::battery::wire {

namespace {
void put16(std::uint8_t *p, std::uint32_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
void put32(std::uint8_t *p, std::uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
std::uint16_t get16(const std::uint8_t *p) { return std::uint16_t(p[0] | p[1] << 8); }

std::size_t status(Service &s, bool monitor, Result res, std::span<std::uint8_t> out) {
    const Recorder &r = s.rec();
    Record rec;
    r.to_record(rec);
    const Sums t = sums(rec);
    std::uint8_t *b = out.data();
    b[0] = std::uint8_t(r.state());
    b[1] = std::uint8_t(r.mode());
    b[2] = std::uint8_t(r.wiring());
    b[3] = std::uint8_t(r.phases());
    b[4] = std::uint8_t(res);
    const bool final_eff = r.state() == State::Done && has_efficiency(rec);
    b[5] = std::uint8_t((s.write_failed() ? kFlagWriteFailed : 0) | (r.dirty() ? kFlagUnwritten : 0) | (monitor ? kFlagMonitor : 0) |
                        (final_eff ? kFlagFinalEff : 0));
    put16(b + 6, r.id());
    put16(b + 8, s.config().end_ma);
    put16(b + 10, s.config().end_s);
    put32(b + 12, t.secs);
    put32(b + 16, r.phase(r.current()).ms / 1000u);
    put32(b + 20, t.uwh_in);
    put32(b + 24, t.uwh_out);
    put32(b + 28, t.uah_in);
    put32(b + 32, t.uah_out);
    // the round trip's ratio once its discharge runs (so far), final when the run is done
    const bool eff = r.mode() == Mode::RoundTrip && r.phases() == 2;
    put16(b + 36, eff ? ratio_permille(t.uwh_out, t.uwh_in) : kNoRatio);
    std::uint16_t ids[store::kKeep + 4];
    b[38] = std::uint8_t(s.volume() ? store::list(*s.volume(), ids, int(sizeof ids / sizeof ids[0])) : 0);
    b[39] = std::uint8_t(s.wiring());
    return kStatusBytes;
}

std::size_t read(Service &s, std::uint16_t id, std::uint16_t off, std::span<std::uint8_t> out) {
    Record rec;
    bool have = false;
    if (id == 0) {
        have = s.rec().state() != State::Idle;
        if (have) s.rec().to_record(rec);
    } else if (s.volume()) {
        have = store::load(*s.volume(), id, rec);
    }
    std::uint8_t file[kRecordBytes];
    const std::uint16_t total = have && encode(rec, file) == kRecordBytes ? std::uint16_t(kRecordBytes) : 0;
    const std::size_t n = off < total ? std::min<std::size_t>(kChunk, total - off) : 0;
    put16(out.data(), off);
    put16(out.data() + 2, total);
    out[4] = std::uint8_t(n);
    std::copy_n(file + (n ? off : 0), n, out.data() + 5);
    return 5 + n;
}
}  // namespace

std::size_t handle(Service &s, bool monitor_online, std::span<const std::uint8_t> p, std::span<std::uint8_t> out) {
    Result res = Result::Ok;
    switch (p[0]) {
    case kStart: {
        if (p[1] > std::uint8_t(Mode::RoundTrip) || p[2] > std::uint8_t(Wiring::Moved)) { res = Result::BadArg; break; }
        switch (s.start(Mode(p[1]), Wiring(p[2]), monitor_online)) {
        case Recorder::Start::Ok: break;
        case Recorder::Start::Busy: res = Result::Busy; break;
        case Recorder::Start::NoMonitor: res = Result::NoMonitor; break;
        case Recorder::Start::BadArg: res = Result::BadArg; break;
        }
        break;
    }
    case kEndPhase: res = s.rec().end_phase() ? Result::Ok : Result::NotNow; break;
    case kNext: res = s.rec().next() ? Result::Ok : Result::NotNow; break;
    case kFinish: res = s.rec().finish() ? Result::Ok : Result::NotNow; break;
    case kConfig: {
        const Config c{get16(&p[1]), get16(&p[3])};
        if (!valid(c) || p[5] > std::uint8_t(Wiring::Moved)) { res = Result::BadArg; break; }
        s.set_config(c);
        s.wiring() = Wiring(p[5]);
        break;
    }
    case kList: {
        std::uint16_t ids[store::kKeep + 4];
        const int n = s.volume() ? store::list(*s.volume(), ids, int(sizeof ids / sizeof ids[0])) : 0;
        out[0] = std::uint8_t(n);
        for (int i = 0; i < n; i++) put16(out.data() + 1 + 2 * i, ids[i]);
        return 1 + 2 * std::size_t(n);
    }
    case kRead: return read(s, get16(&p[1]), get16(&p[3]), out);
    default: break;
    }
    return status(s, monitor_online, res, out);
}

}  // namespace k2::battery::wire
