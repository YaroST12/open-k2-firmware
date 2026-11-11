// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress engine against a simulated source (a fake Env): a clean run scores 0, every deviation of the source shows up as its own event,
// the pacing classes hold, nothing is ever asked outside the advertised list, and every abort rule stops the run.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pdstress/engine.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::pdstress;

namespace {

pd::SourceCaps from_raw(std::initializer_list<std::uint32_t> raw) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    c.rx_count = 1;
    for (auto r : raw) c.pdos[c.num_pdos++] = pd::decode_pdo(r);
    return c;
}
// NOVOO RG100: 5/9/15/20 V 3 A + PPS 5-21 V 3 A.
pd::SourceCaps rg100() { return from_raw({0x0001912C, 0x0002D12C, 0x0004B12C, 0x0006412C, 0xC1A4323C}); }

enum class V { Ok, Reject, Wait, Silent, HardReset, SoftReset };
struct Req { std::uint32_t at; pd::Choice c; };
struct Verdict {
    V v = V::Ok;
    std::uint32_t accept_ms = 5;     // Request -> Accept
    std::uint32_t rdy_ms = 60;       // Accept -> PS_RDY (the voltage ramps over it)
    std::int32_t overshoot = 0;      // mV above the target for 40 ms after PS_RDY
    std::uint32_t late_vbus_ms = 0;  // VBUS only reaches the target this long after PS_RDY
};

// A source behind the Env seam: it follows the sink's state machine closely enough for the engine (state, result, counters, stamps).
struct Sim : Env {
    pd::SourceCaps caps_ = rg100();
    Snapshot s;
    std::uint32_t now = 1000;
    std::function<Verdict(const Req&, unsigned)> policy = [](const Req&, unsigned) { return Verdict{}; };
    const char* busy_ = nullptr;
    bool proof_ = false;
    std::uint32_t cap_ = 36000;
    std::vector<Req> reqs;
    std::vector<std::uint32_t> rdy_at;   // time of every PS_RDY
    std::vector<std::string> notes;
    std::vector<std::pair<Seq, Event>> events;
    std::vector<std::pair<Seq, bool>> ends;
    std::vector<Seq> starts;
    int released = 0, finished = 0, getcaps = 0, softresets = 0;
    Stop stop = Stop::None;
    // the request in flight
    struct Pend {
        bool on = false;
        Verdict vd;
        std::uint32_t t_req = 0, t_acc = 0, t_rdy = 0;
        bool accepted = false;
        std::uint16_t target = 0, ma = 0;
        std::uint8_t pos = 0;
        int from = 0;
    } p;
    // scheduled actions
    struct Act { std::uint32_t at; std::function<void()> fn; };
    std::vector<Act> acts;
    int ramp_from = 5000, ramp_to = 5000;
    std::uint32_t ramp_t0 = 0, ramp_t1 = 0;
    int over_mv = 0;
    std::uint32_t over_until = 0;
    int hold_mv = -1;
    std::uint32_t hold_until = 0;
    unsigned nreq = 0;
    unsigned refuse = 0;   // refuse this many request() calls with Busy

    Sim() {
        s.attached = true;
        s.state = pd::SinkState::Ready;
        s.pos = 1;
        s.mv = 5000;
        s.ma = 3000;
        s.vbus_mv = 5000;
        s.caps_rx = 1;
        s.result = pd::SinkResult::Ok;
    }

    void at(std::uint32_t t, std::function<void()> f) { acts.push_back({t, std::move(f)}); }

    // Env
    Snapshot snapshot() override { Snapshot r = s; r.now_us = now * 1000u; r.caps_rx = caps_.rx_count; return r; }
    const pd::SourceCaps& caps() override { return caps_; }
    std::uint32_t cap_mv() override { return cap_; }
    bool epr_proof() override { return proof_; }
    const char* busy() override { return busy_; }
    pd::Err request(const pd::Choice& c) override {
        if (refuse > 0) { --refuse; return pd::Err::Busy; }
        if (p.on || s.state == pd::SinkState::Requesting || s.state == pd::SinkState::Transition) return pd::Err::Busy;
        if (c.pos == 0) {   // EPR entry: this source does not do EPR
            reqs.push_back({now, c});
            s.state = pd::SinkState::Requesting;
            s.result = pd::SinkResult::None;
            at(now + 50, [this] { s.state = pd::SinkState::Ready; s.result = pd::SinkResult::EprFailed; });
            return pd::Err::Ok;
        }
        const pd::Plan pl = pd::plan_request(caps_, c, cap_, s.epr_mode);
        if (pl.err != pd::Err::Ok) return pl.err;
        reqs.push_back({now, c});
        p = Pend{};
        p.on = true;
        p.vd = policy(reqs.back(), nreq++);
        p.t_req = now;
        p.t_acc = now + p.vd.accept_ms;
        p.t_rdy = p.t_acc + p.vd.rdy_ms;
        p.target = pl.mv;
        p.ma = pl.ma;
        p.pos = pl.pos;
        p.from = s.vbus_mv;
        s.state = pd::SinkState::Requesting;
        s.result = pd::SinkResult::None;
        s.req_us = now * 1000u + 1;
        return pd::Err::Ok;
    }
    bool get_caps() override {
        ++getcaps;
        at(now + 20, [this] { ++caps_.rx_count; });
        return true;
    }
    bool soft_reset() override {
        ++softresets;
        s.state = pd::SinkState::Idle;
        s.pos = 0;
        at(now + 100, [this] {   // the source re-announces, the driver re-requests PDO 1: Accept, VBUS falls, PS_RDY
            ++caps_.rx_count;
            s.state = pd::SinkState::Transition;
            ramp_from = s.vbus_mv; ramp_to = 5000; ramp_t0 = now; ramp_t1 = now + 40;
        });
        at(now + 145, [this] {
            s.state = pd::SinkState::Ready;
            s.pos = 1;
            s.mv = 5000;
            s.result = pd::SinkResult::Ok;
        });
        return true;
    }
    void release() override {
        ++released;
        at(now + 50, [this] { s.state = pd::SinkState::Ready; s.pos = 1; s.mv = 5000; s.result = pd::SinkResult::Ok; s.vbus_mv = 5000; ramp_from = ramp_to = 5000; });
    }
    void on_seq_start(Seq q, std::uint8_t, std::uint8_t, std::uint16_t) override { starts.push_back(q); }
    void on_seq_end(Seq q, const Counts&, bool skipped) override { ends.push_back({q, skipped}); }
    void on_event(Seq q, Event e, std::uint32_t) override { events.push_back({q, e}); }
    void on_note(const char* n) override { notes.push_back(n); }
    void on_finish(Stop st) override { ++finished; stop = st; }

    // Source-side injections.
    void hard_reset() {
        ++s.source_hard_resets;
        s.result = pd::SinkResult::HardReset;
        s.state = pd::SinkState::Idle;
        s.pos = 0;
        p.on = false;
        ramp_from = ramp_to = 0;
        s.vbus_mv = 0;
        hold_mv = 0;
        hold_until = now + 600;
        at(now + 900, [this] {
            ++caps_.rx_count;
            s.state = pd::SinkState::Ready;
            s.pos = 1; s.mv = 5000;
            ramp_from = ramp_to = 5000;
        });
    }
    void drop_vbus(std::uint32_t ms) { hold_mv = 0; hold_until = now + ms; }

    void tick() {
        ++now;
        for (std::size_t i = 0; i < acts.size();) {
            if (acts[i].at <= now) { auto f = std::move(acts[i].fn); acts.erase(acts.begin() + std::ptrdiff_t(i)); f(); } else ++i;
        }
        if (p.on) {
            if (p.vd.v == V::Silent) {
                if (now >= p.t_req + 1500) { p.on = false; s.state = pd::SinkState::Ready; s.result = pd::SinkResult::Timeout; }
            } else if (!p.accepted && now >= p.t_acc) {
                switch (p.vd.v) {
                    case V::Reject: ++s.rejects_rx; s.result = pd::SinkResult::Rejected; s.state = pd::SinkState::Ready; p.on = false; break;
                    case V::Wait: s.waits_rx = std::uint16_t(s.waits_rx + 3); s.result = pd::SinkResult::WaitExhausted; s.state = pd::SinkState::Ready; p.on = false; break;
                    case V::HardReset: p.on = false; hard_reset(); break;
                    case V::SoftReset: ++s.soft_resets_rx; p.on = false; s.state = pd::SinkState::Idle; s.pos = 0;
                        at(now + 100, [this] { ++caps_.rx_count; s.state = pd::SinkState::Ready; s.pos = 1; s.mv = 5000; s.result = pd::SinkResult::Ok; ramp_from = ramp_to = 5000; });
                        break;
                    default:
                        p.accepted = true;
                        s.state = pd::SinkState::Transition;
                        s.accept_at = now;
                        s.accept_us = now * 1000u + 1;
                        ramp_from = p.from; ramp_to = p.target; ramp_t0 = now; ramp_t1 = now + p.vd.rdy_ms + p.vd.late_vbus_ms;
                        break;
                }
            } else if (p.accepted && now >= p.t_rdy) {
                p.on = false;
                s.state = pd::SinkState::Ready;
                s.result = pd::SinkResult::Ok;
                s.pos = p.pos;
                s.mv = p.target;
                s.ma = p.ma;
                s.ps_rdy_at = now;
                s.ps_rdy_us = now * 1000u + 1;
                rdy_at.push_back(now);
                if (p.vd.overshoot) { over_mv = p.vd.overshoot; over_until = now + 40; }
            }
        }
        int v = ramp_to;
        if (now < ramp_t0) v = ramp_from;
        else if (now < ramp_t1) v = ramp_from + int((std::int64_t(ramp_to) - ramp_from) * std::int64_t(now - ramp_t0) / std::int64_t(ramp_t1 - ramp_t0));
        if (now < over_until) v += over_mv;
        if (hold_mv >= 0 && now < hold_until) v = hold_mv;
        s.vbus_mv = v;
    }
};

struct Rig {
    Sim sim;
    std::array<std::int16_t, 640> buf{};
    Engine eng;
    Options o;
    Rig() : eng(sim, Record(buf)) {}
    StartResult start(const Resume* r = nullptr) { return eng.start(o, sim.now, r); }
    // Run until the engine is done or `ms` simulated milliseconds passed; returns the elapsed time.
    std::uint32_t run(std::uint32_t ms) {
        const std::uint32_t t0 = sim.now;
        while (sim.now - t0 < ms && eng.running()) { sim.tick(); eng.poll(sim.now); }
        return sim.now - t0;
    }
    std::uint32_t count(Event e) const {
        std::uint32_t n = 0;
        for (const auto& x : sim.events) if (x.second == e) ++n;
        return n;
    }
    std::uint32_t total_events() const { return std::uint32_t(sim.events.size()); }
};

Options only(Seq q, unsigned pct = 100) {
    Options o;
    o.mask = seq_bit(q);
    o.cycles_pct = std::uint16_t(pct);
    return o;
}

void start_refusals() {
    {
        Rig r;
        r.sim.busy_ = "charger test running";
        CHECK_EQ(int(r.start()), int(StartResult::Busy));
        CHECK_COND(!r.eng.running());
    }
    {
        Rig r;
        r.sim.s.inline_link = true;
        CHECK_EQ(int(r.start()), int(StartResult::Inline));
    }
    {
        Rig r;
        r.sim.s.attached = false;
        CHECK_EQ(int(r.start()), int(StartResult::NoCaps));
        Rig q;
        q.sim.caps_.valid = false;
        CHECK_EQ(int(q.start()), int(StartResult::NoCaps));
    }
    {
        Rig r;
        r.o.mask = seq_bit(Seq::AvsSteps);   // nothing applies on this charger
        CHECK_EQ(int(r.start()), int(StartResult::NoSequence));
    }
    {
        Rig r;
        std::array<std::int16_t, 100> small{};
        Engine e(r.sim, Record(small));
        CHECK_EQ(int(e.start(Options{}, r.sim.now)), int(StartResult::NoStore));
    }
    {
        Rig r;
        r.o = only(Seq::Ladder, 20);
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        CHECK_EQ(int(r.start()), int(StartResult::Running));
    }
}

void clean_run_scores_zero() {
    Rig r;
    r.o = only(Seq::Ladder, 20);   // 2 cycles
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(600000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_EQ(r.total_events(), 0);
    CHECK_EQ(score_cp(r.eng.total()), 0);
    CHECK_EQ(r.eng.transitions(), r.sim.reqs.size());
    CHECK_EQ(r.sim.released, 1);
    CHECK_EQ(r.sim.finished, 1);
    CHECK_EQ(r.sim.starts.size(), 1);
    CHECK_EQ(r.sim.ends.size(), 1);
    CHECK_COND(r.sim.ends[0].first == Seq::Ladder && !r.sim.ends[0].second);
    CHECK_COND(r.eng.total().transitions > 0);
    // The release put the sink back at 5 V.
    CHECK_EQ(r.sim.s.mv, 5000);
}

void dump(const Rig& r) {
    for (const auto& n : r.sim.notes) std::printf("      note: %s\n", n.c_str());
    for (const auto& e : r.sim.events) std::printf("      event: seq %d %s\n", int(e.first), event_name(e.second));
    std::printf("      stop=%s at vbus=%d state=%d, %zu sequences started, last %d\n", stop_name(r.eng.stop()), r.sim.s.vbus_mv, int(r.sim.s.state),
                r.sim.starts.size(), r.sim.starts.empty() ? -1 : int(r.sim.starts.back()));
    for (std::size_t i = r.sim.reqs.size() > 4 ? r.sim.reqs.size() - 4 : 0; i < r.sim.reqs.size(); ++i)
        std::printf("      req at %u pos %u mv %u\n", unsigned(r.sim.reqs[i].at), unsigned(r.sim.reqs[i].c.pos), unsigned(r.sim.reqs[i].c.mv));
}

void full_default_run_is_clean() {
    Rig r;   // every default sequence on the RG100, the proposal's cycle counts
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    const std::uint32_t ms = r.run(2000000);
    if (r.eng.stop() != Stop::Done) dump(r);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_EQ(r.total_events(), 0);
    CHECK_EQ(score_cp(r.eng.total()), 0);
    CHECK_EQ(r.eng.seq_total(), 9);   // the nine sequences that apply to a PPS 5-21 V charger
    CHECK_EQ(r.sim.ends.size(), 9);
    CHECK_COND(r.eng.total().transitions >= 700 && r.eng.total().transitions <= 760);
    CHECK_EQ(int(r.eng.na(Seq::AvsSteps)), int(Na::NoAvs));
    CHECK_EQ(int(r.eng.na(Seq::PpsLow)), int(Na::PpsAbove5V));
    CHECK_EQ(int(r.eng.na(Seq::Epr)), int(Na::NotSelected));
    CHECK_COND(ms < r.o.max_ms);   // inside the run-time cap
    std::printf("      full default run on the RG100 model: %u transitions in %u s simulated\n", unsigned(r.eng.total().transitions), unsigned(ms / 1000));
}

// Nothing outside the advertised list is ever asked: every request the engine made validates against the caps.
void nothing_outside_the_list() {
    Rig r;
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(2000000);
    CHECK_COND(r.sim.reqs.size() > 600);
    for (const Req& q : r.sim.reqs) {
        if (q.c.pos == 0) continue;
        const pd::Plan pl = pd::plan_request(r.sim.caps_, q.c, 36000, false);
        CHECK_EQ(int(pl.err), int(pd::Err::Ok));
        const pd::Pdo& d = r.sim.caps_.pdos[q.c.pos - 1];
        CHECK_COND(pl.mv <= 21000);
        CHECK_COND(pl.ma <= d.max_ma);
        CHECK_COND(!q.c.hunt);
    }
}

void pacing_classes() {
    {   // Ready: the next Request goes 100 ms after PS_RDY (the same-request block)
        Rig r;
        r.o = only(Seq::SameReq, 20);
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.eng.finished());
        int fast = 0;
        for (std::size_t i = 1; i < r.sim.reqs.size() && i < r.sim.rdy_at.size(); ++i) {
            const std::uint32_t gap = r.sim.reqs[i].at - r.sim.rdy_at[i - 1];
            if (gap < 600) { ++fast; CHECK_COND(gap >= kReadyMs); CHECK_COND(gap <= kReadyMs + 5); }
        }
        CHECK_COND(fast >= 10);
    }
    {   // Settled: at least a second of quiet, VBUS in the band
        Rig r;
        r.o = only(Seq::Ladder, 10);
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.eng.finished());
        for (std::size_t i = 1; i < r.sim.reqs.size() && i < r.sim.rdy_at.size(); ++i)
            CHECK_COND(r.sim.reqs[i].at - r.sim.rdy_at[i - 1] >= kSettledMs);
    }
    {   // all-Settled override slows a Ready sequence down
        Rig r;
        r.o = only(Seq::SameReq, 20);
        r.o.pace = 1;
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(900000);
        for (std::size_t i = 1; i < r.sim.reqs.size() && i < r.sim.rdy_at.size(); ++i)
            CHECK_COND(r.sim.reqs[i].at - r.sim.rdy_at[i - 1] >= kSettledMs);
    }
}

void top_down_step_rate() {
    Rig r;
    r.o = only(Seq::Slam, 40);
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    // Consecutive Requests that leave the top level downwards are at least a second apart.
    std::uint32_t last = 0;
    bool have = false;
    unsigned n = 0;
    for (std::size_t i = 1; i < r.sim.reqs.size(); ++i) {
        if (r.sim.reqs[i - 1].c.pos == 4 && r.sim.reqs[i].c.pos < 4) {
            if (have) CHECK_COND(r.sim.reqs[i].at - last >= kTopDownMs);
            last = r.sim.reqs[i].at;
            have = true;
            ++n;
        }
    }
    CHECK_COND(n >= 2);
}

void reject_drops_the_level() {
    Rig r;
    r.o = only(Seq::Ladder, 30);
    r.sim.policy = [](const Req& q, unsigned) { Verdict v; if (q.c.pos == 3) v.v = V::Reject; return v; };
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    unsigned to3 = 0;
    for (const Req& q : r.sim.reqs) if (q.c.pos == 3) ++to3;
    CHECK_EQ(to3, 1);                       // asked once, said no, left out for the rest of the sequence
    CHECK_EQ(r.count(Event::RejectRx), 1);
    CHECK_EQ(r.count(Event::HardResetRx), 0);
    CHECK_EQ(r.count(Event::ContractLost), 0);
    CHECK_EQ(r.eng.total().get(Event::RejectRx), 1);
    // One note about the dropped level, not one per skipped step.
    unsigned dropped_notes = 0;
    for (const auto& n : r.sim.notes) if (n == "level dropped after a reject") ++dropped_notes;
    CHECK_EQ(dropped_notes, 1);
}

void wait_is_retried_then_dropped() {
    {   // the first attempt at 9 V waits out, the second goes through
        Rig r;
        r.o = only(Seq::Ladder, 10);
        unsigned at9 = 0;
        r.sim.policy = [&](const Req& q, unsigned) { Verdict v; if (q.c.pos == 2 && at9++ == 0) v.v = V::Wait; return v; };
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.eng.finished());
        CHECK_EQ(r.count(Event::WaitRx), 3);   // the sink's own three Waits are all counted
        unsigned to2 = 0;
        for (const Req& q : r.sim.reqs) if (q.c.pos == 2) ++to2;
        CHECK_COND(to2 >= 2);
    }
    {   // a level that keeps waiting is dropped after kMaxStepWaits retries
        Rig r;
        r.o = only(Seq::Ladder, 10);
        r.sim.policy = [](const Req& q, unsigned) { Verdict v; if (q.c.pos == 2) v.v = V::Wait; return v; };
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.eng.finished());
        unsigned to2 = 0;
        for (const Req& q : r.sim.reqs) if (q.c.pos == 2) ++to2;
        CHECK_EQ(to2, 2u * (1u + kMaxStepWaits));   // 9 V is visited twice per cycle: up and down
        CHECK_EQ(r.count(Event::WaitRx), 3u * to2);
    }
}

void late_answers_are_counted() {
    {   // an Accept slower than the spec limit
        Rig r;
        r.o = only(Seq::Ladder, 10);
        r.sim.policy = [](const Req& q, unsigned) { Verdict v; if (q.c.pos == 2) v.accept_ms = 120; return v; };
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.count(Event::AcceptLate) >= 1);
        CHECK_EQ(r.count(Event::PsRdyLate), 0);
        const Summary a = summary(r.eng.record().rec(std::size_t(Seq::Ladder)), Metric::Accept);
        CHECK_COND(a.max >= 1200 && a.max <= 1300);   // 0.1 ms units
        CHECK_COND(a.min >= 40 && a.min <= 60);       // 5 ms
        CHECK_COND(score_cp(r.eng.total()) > 0);
    }
    {   // a PS_RDY slower than tPSTransition
        Rig r;
        r.o = only(Seq::Ladder, 10);
        r.sim.policy = [](const Req& q, unsigned) { Verdict v; if (q.c.pos == 3) v.rdy_ms = 700; return v; };
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.count(Event::PsRdyLate) >= 1);
        CHECK_EQ(r.count(Event::AcceptLate), 0);
        const Summary p = summary(r.eng.record().rec(std::size_t(Seq::Ladder)), Metric::PsRdy);
        CHECK_COND(p.max >= 6900 && p.max <= 7100);
    }
}

void vbus_events() {
    {   // PS_RDY while VBUS is still on its way (outside the band)
        Rig r;
        r.o = only(Seq::Ladder, 10);
        r.sim.policy = [](const Req& q, unsigned) { Verdict v; if (q.c.pos == 4) v.late_vbus_ms = 300; return v; };
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.count(Event::PsRdyOutOfBand) >= 1);
        CHECK_EQ(r.count(Event::VbusDropout), 0);
    }
    {   // an overshoot after PS_RDY
        Rig r;
        r.o = only(Seq::Ladder, 10);
        r.sim.policy = [](const Req& q, unsigned) { Verdict v; if (q.c.pos == 3) v.overshoot = 2500; return v; };
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_COND(r.count(Event::VbusBand) >= 1);
        const Summary o = summary(r.eng.record().rec(std::size_t(Seq::Ladder)), Metric::Over);
        CHECK_COND(o.n > 0 && o.max >= 2000);
    }
    {   // the time to band is measured on the transitions that start outside it
        Rig r;
        r.o = only(Seq::Ladder, 10);
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        const Summary b = summary(r.eng.record().rec(std::size_t(Seq::Ladder)), Metric::Band);
        CHECK_COND(b.n > 0);
        CHECK_COND(b.max < 300);
    }
}

void reannounce_replans() {
    Rig r;
    r.o = only(Seq::Ladder, 30);
    // After 12 s the source re-announces without the 15 V level.
    r.sim.at(r.sim.now + 12000, [&r] {
        r.sim.caps_ = from_raw({0x0001912C, 0x0002D12C, 0x0006412C, 0xC1A4323C});
        r.sim.caps_.rx_count = 2;
        ++r.sim.s.caps_unsolicited;
    });
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(r.count(Event::CapsReannounce), 1);
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    // Every request after the re-announce is valid against the new list.
    for (const Req& q : r.sim.reqs) CHECK_COND(q.c.pos <= 4);
}

void get_caps_sequence() {
    Rig r;
    r.o = only(Seq::Reannounce, 50);   // 10 cycles
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(300000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(r.sim.getcaps, 10);
    CHECK_EQ(r.sim.reqs.size(), 0);
    CHECK_EQ(r.total_events(), 0);   // solicited announcements are not "unexpected"
    CHECK_EQ(r.eng.total().transitions, 10);
}

void soft_reset_sequence() {
    Rig r;
    r.o = only(Seq::SoftReset);
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(300000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(r.sim.softresets, 10);
    CHECK_EQ(r.total_events(), 0);
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
}

void mismatch_sequence_sets_the_bit() {
    Rig r;
    r.o = only(Seq::Mismatch);
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(300000);
    CHECK_COND(r.eng.finished());
    unsigned with = 0, without = 0;
    for (const Req& q : r.sim.reqs) (q.c.mismatch ? with : without)++;
    CHECK_EQ(with, 10);
    CHECK_EQ(without, 10);
}

void source_hard_reset_recovers_once() {
    Rig r;
    r.o = only(Seq::Ladder, 30);
    unsigned n = 0;
    r.sim.policy = [&](const Req&, unsigned) { Verdict v; if (++n == 5) v.v = V::HardReset; return v; };
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_EQ(r.count(Event::HardResetRx), 1);
    CHECK_EQ(r.count(Event::VbusDropout), 0);   // the dip belongs to the reset
    CHECK_EQ(r.count(Event::NoRecovery), 0);
    CHECK_COND(r.sim.reqs.size() > 10);         // the run went on after the reset
}

void second_source_hard_reset_stops() {
    Rig r;
    r.o = only(Seq::Ladder, 30);
    unsigned n = 0;
    r.sim.policy = [&](const Req&, unsigned) { Verdict v; if (++n == 4 || n == 8) v.v = V::HardReset; return v; };
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::SourceHardResets));
    CHECK_EQ(r.count(Event::HardResetRx), 2);
    CHECK_EQ(r.sim.released, 1);
}

void source_soft_reset_is_counted() {
    Rig r;
    r.o = only(Seq::Ladder, 20);
    unsigned n = 0;
    r.sim.policy = [&](const Req&, unsigned) { Verdict v; if (++n == 3) v.v = V::SoftReset; return v; };
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_EQ(r.count(Event::SoftResetRx), 1);
    CHECK_EQ(r.count(Event::VbusDropout), 0);
}

void silent_source_is_a_lost_contract() {
    Rig r;
    r.o = only(Seq::Ladder, 20);
    unsigned n = 0;
    r.sim.policy = [&](const Req&, unsigned) { Verdict v; if (++n == 3) v.v = V::Silent; return v; };
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(r.count(Event::ContractLost), 1);
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
}

void vbus_dropout_and_recovery() {
    Rig r;
    r.o = only(Seq::Ladder, 30);
    r.sim.at(r.sim.now + 15000, [&r] { r.sim.drop_vbus(1500); });
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_EQ(r.count(Event::VbusDropout), 1);
    CHECK_EQ(r.count(Event::NoRecovery), 0);
    CHECK_COND(r.sim.reqs.size() > 12);
}

void no_recovery_stops_the_run() {
    Rig r;
    r.o = only(Seq::Ladder, 30);
    r.sim.at(r.sim.now + 15000, [&r] { r.sim.drop_vbus(60000); });
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::NoRecovery));
    CHECK_EQ(r.count(Event::NoRecovery), 1);
    CHECK_EQ(r.count(Event::VbusDropout), 1);
    CHECK_COND(run_stops(r.eng.total()));
    CHECK_COND(score_cp(r.eng.total()) >= 10000 / 4);   // the 100 points over a short run
    CHECK_EQ(r.sim.released, 0);                          // nothing to release on a dead line
}

void cancel_releases() {
    Rig r;
    r.o = only(Seq::Ladder);
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(8000);
    CHECK_COND(r.eng.running());
    r.eng.cancel();
    r.run(20000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Cancelled));
    CHECK_EQ(r.sim.released, 1);
    CHECK_EQ(r.sim.finished, 1);
    CHECK_EQ(r.sim.s.mv, 5000);
    r.eng.cancel();   // idempotent once finished
    CHECK_EQ(int(r.eng.stop()), int(Stop::Cancelled));
}

void hard_caps() {
    {   // run time
        Rig r;
        r.o = only(Seq::Ladder);
        r.o.max_ms = 20000;
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        const std::uint32_t ms = r.run(100000);
        CHECK_EQ(int(r.eng.stop()), int(Stop::Timeout));
        CHECK_COND(ms >= 20000 && ms < 30000);
        CHECK_EQ(r.sim.released, 1);
    }
    {   // transitions
        Rig r;
        r.o = only(Seq::SameReq);
        r.o.max_transitions = 8;
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(300000);
        CHECK_EQ(int(r.eng.stop()), int(Stop::MaxTransitions));
        CHECK_EQ(r.sim.reqs.size(), 8);
    }
}

void overvoltage_aborts() {
    Rig r;
    r.o = only(Seq::Ladder);
    r.sim.at(r.sim.now + 8000, [&r] { r.sim.hold_mv = 40000; r.sim.hold_until = r.sim.now + 5000; });
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(100000);
    CHECK_EQ(int(r.eng.stop()), int(Stop::Overvoltage));
    CHECK_EQ(r.sim.released, 1);
}

void load_aborts() {
    {
        Rig r;
        r.o = only(Seq::Ladder);
        r.sim.at(r.sim.now + 8000, [&r] { r.sim.s.ibus_ma = 400; });
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(100000);
        CHECK_EQ(int(r.eng.stop()), int(Stop::Load));
    }
    {   // a self-powered K2 reads its own supply current: the rule is off
        Rig r;
        r.o = only(Seq::Ladder, 10);
        r.sim.s.self_powered = true;
        r.sim.s.ibus_ma = 400;
        CHECK_EQ(int(r.start()), int(StartResult::Ok));
        r.run(600000);
        CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    }
}

void link_going_inline_stops() {
    Rig r;
    r.o = only(Seq::Ladder);
    r.sim.at(r.sim.now + 8000, [&r] { r.sim.s.inline_link = true; });
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(100000);
    CHECK_EQ(int(r.eng.stop()), int(Stop::Inline));
    CHECK_EQ(r.sim.released, 0);   // nothing is sent on an inline link, not even a release
}

void pps_refused_twice_ends_the_sequence() {
    Rig r;
    r.o = only(Seq::PpsSteps, 20);
    r.sim.policy = [&](const Req& q, unsigned) { Verdict v; if (q.c.pos == 5) v.v = V::Reject; return v; };
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(300000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_COND(r.count(Event::RejectRx) >= 1 && r.count(Event::RejectRx) <= 2);
    unsigned pps = 0;
    for (const Req& q : r.sim.reqs) if (q.c.pos == 5) ++pps;
    CHECK_COND(pps <= 2);
    bool noted = false;
    for (const auto& n : r.sim.notes) if (n.find("needs a load") != std::string::npos) noted = true;
    if (!noted) dump(r);
    CHECK_COND(noted);
}

void epr_without_entry_ends_the_sequence() {
    Rig r;
    r.sim.proof_ = true;
    r.sim.caps_ = from_raw({0x0001912C, 0x0002D12C, 0x0004B12C, 0x0006412C});
    for (int i = r.sim.caps_.num_pdos; i < 7; i++) r.sim.caps_.pdos[i] = pd::Pdo{};
    r.sim.caps_.num_pdos = 8;
    r.sim.caps_.pdos[7] = pd::decode_pdo((560u << 10) | 500u);
    r.o.mask = seq_bit(Seq::Epr) | seq_bit(Seq::Ladder);
    r.o.cycles_pct = 10;
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(300000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_EQ(r.sim.ends.size(), 2);   // EPR ended early, the ladder still ran
    bool noted = false;
    for (const auto& n : r.sim.notes) if (n == "EPR entry failed: sequence ends") noted = true;
    CHECK_COND(noted);
}

void resume_skips_finished_sequences() {
    Rig r;
    r.o.mask = seq_bit(Seq::Ladder) | seq_bit(Seq::Reannounce) | seq_bit(Seq::SoftReset);
    r.o.cycles_pct = 20;
    Resume rs;
    rs.next = 1;
    rs.losses = 1;
    rs.elapsed_ms = 50000;
    CHECK_EQ(int(r.start(&rs)), int(StartResult::Ok));
    r.run(300000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(r.sim.starts.size(), 2);
    CHECK_COND(r.sim.starts[0] == Seq::Reannounce && r.sim.starts[1] == Seq::SoftReset);
    CHECK_EQ(r.eng.reboots(), 1);
    CHECK_COND(r.eng.elapsed_ms(r.sim.now) >= 50000);
}

void resume_gives_up_a_sequence() {
    Rig r;
    r.o.mask = seq_bit(Seq::Ladder) | seq_bit(Seq::Reannounce);
    r.o.cycles_pct = 20;
    Resume rs;
    rs.next = 0;
    rs.given_up[std::size_t(Seq::Ladder)] = 1;
    CHECK_EQ(int(r.start(&rs)), int(StartResult::Ok));
    r.run(300000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(r.sim.starts.size(), 1);
    CHECK_COND(r.sim.starts[0] == Seq::Reannounce);
}

void resume_after_too_many_losses_ends_the_run() {
    // The run's total reboot budget is spent: the engine reports it and sends nothing.
    Rig r;
    r.o.mask = seq_bit(Seq::Ladder) | seq_bit(Seq::Reannounce);
    r.o.cycles_pct = 20;
    Resume rs;
    rs.next = 0;
    rs.losses = kMaxRunLosses;
    CHECK_EQ(int(r.start(&rs)), int(StartResult::Ok));
    r.run(30000);   // the final release to 5 V is confirmed first
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::TooManyLosses));
    CHECK_EQ(r.sim.starts.size(), 0);
    CHECK_EQ(r.eng.transitions(), 0);
    // One reboot fewer still runs.
    Rig q;
    q.o = r.o;
    Resume ok;
    ok.losses = kMaxRunLosses - 1;
    CHECK_EQ(int(q.start(&ok)), int(StartResult::Ok));
    q.run(300000);
    CHECK_COND(q.eng.finished());
    CHECK_EQ(int(q.eng.stop()), int(Stop::Done));
}

void score_of_a_noisy_run() {
    // Rejects and hard resets give the run its score; the per-sequence record carries the raw counters.
    Rig r;
    r.o = only(Seq::Ladder, 30);
    unsigned n = 0;
    r.sim.policy = [&](const Req& q, unsigned) { Verdict v; ++n; if (q.c.pos == 3) v.v = V::Reject; if (n == 6) v.v = V::HardReset; return v; };
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(900000);
    CHECK_COND(r.eng.finished());
    const Counts c = r.eng.seq_counts(Seq::Ladder);
    CHECK_EQ(c.get(Event::HardResetRx), 1);
    CHECK_COND(c.get(Event::RejectRx) >= 1);
    CHECK_EQ(c.transitions, r.eng.total().transitions);
    CHECK_COND(score_cp(c) >= (20 + 2) * 100 * 100 / std::max<std::uint32_t>(c.transitions, kMinTransitions));
}

void ready_pace_survives_a_busy_sink() {
    // A refused Request (the sink's gates) is retried and does not count as a transition until it goes out.
    Rig r;
    r.o = only(Seq::Ladder, 10);
    r.sim.refuse = 40;   // the first forty attempts (a few hundred ms of polls) are refused
    CHECK_EQ(int(r.start()), int(StartResult::Ok));
    r.run(600000);
    CHECK_COND(r.eng.finished());
    CHECK_EQ(int(r.eng.stop()), int(Stop::Done));
    CHECK_EQ(r.sim.refuse, 0);
    CHECK_EQ(r.eng.transitions(), r.sim.reqs.size());
    CHECK_EQ(r.total_events(), 0);
}

}  // namespace

int main() {
    start_refusals();
    clean_run_scores_zero();
    full_default_run_is_clean();
    nothing_outside_the_list();
    pacing_classes();
    top_down_step_rate();
    reject_drops_the_level();
    wait_is_retried_then_dropped();
    late_answers_are_counted();
    vbus_events();
    reannounce_replans();
    get_caps_sequence();
    soft_reset_sequence();
    mismatch_sequence_sets_the_bit();
    source_hard_reset_recovers_once();
    second_source_hard_reset_stops();
    source_soft_reset_is_counted();
    silent_source_is_a_lost_contract();
    vbus_dropout_and_recovery();
    no_recovery_stops_the_run();
    cancel_releases();
    hard_caps();
    overvoltage_aborts();
    load_aborts();
    link_going_inline_stops();
    pps_refused_twice_ends_the_sequence();
    epr_without_entry_ends_the_sequence();
    resume_skips_finished_sequences();
    resume_gives_up_a_sequence();
    resume_after_too_many_losses_ends_the_run();
    score_of_a_noisy_run();
    ready_pace_survives_a_busy_sink();
    return test_report("test_pdstress_engine");
}
