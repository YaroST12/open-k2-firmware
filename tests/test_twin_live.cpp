// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the device twin's live proof (k2::twin::live::Runner): a simulated charger behind a fake Env, one case per
// verdict, the order of the calls (impersonate before detach, release before the next twin, the K2's own caps back at the end),
// cancel from every phase, and the refusals that touch nothing.
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "k2/twin/live.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::twin;
using namespace k2::twin::live;

namespace {

constexpr std::uint32_t fx(unsigned mv, unsigned ma) { return (mv / 50u) << 10 | ma / 10u; }

pd::SourceCaps caps_of(std::initializer_list<std::uint32_t> words) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    for (std::uint32_t w : words) c.pdos[c.num_pdos++] = pd::decode_pdo(w);
    return c;
}

Twin twin_of(std::initializer_list<std::uint32_t> sink, const char* name) {
    Twin t;
    for (std::size_t i = 0; name[i] && i < kNameMax; ++i) t.name[i] = name[i];
    for (std::uint32_t w : sink) t.pdos[t.num_pdos++] = w;
    return t;
}

const Twin kPixel = twin_of({0x3601912C, 0x0002D12C, 0xC0DC323C}, "Pixel");
const Twin kLaptop = twin_of({0x3601912C}, "Laptop");
const Twin kOp15 = twin_of({0x2601912C, 0x0002D12C, 0x0003C12C, 0xC0DC3264}, "OP15");
// C1 with the plain cable: fixed 5/9/12/15/20 V 3 A, PPS 3.3-11 V 3 A (pos 6)
const pd::SourceCaps kC1 = caps_of({0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0006412C, 0xC8DC323C, 0xC990323C});

enum class Mode { Grant, Reject, NoPsRdy, Timeout, OutOfBand, Over, Reset };
enum class Ev { None, Dark, Attach, Reply, Release };

// The charger behind the K2, the Env the runner talks to, and the clock: step() advances 10 ms at a time.
struct Sim : Env {
    std::map<std::uint8_t, Twin> twins;
    std::vector<std::string> log;
    std::map<std::uint8_t, Slot> recorded;
    pd::SourceCaps caps = kC1, caps_after = kC1;
    bool swap_caps = false, no_caps = false, never_dark = false, self_powered = false, busy = false, inline_link = false;
    pd::Err req_err = pd::Err::Ok;
    Mode mode = Mode::Grant;
    std::uint32_t now = 1000, seq = 1, cap_mv = 36000;
    int vbus = 5000;
    pd::SinkStatus sink;
    Ev ev = Ev::None;
    std::uint32_t ev_at = 0;
    pd::Choice last{};
    bool finished_called = false;
    Reason fin = Reason::Done;
    Runner r;

    Sim() {
        sink.state = pd::SinkState::Ready;
        sink.pos = 1;
        twins[1] = kPixel;
        twins[2] = kLaptop;
        twins[3] = kOp15;
    }

    bool load(std::uint8_t seq_no, Twin& out) override {
        auto it = twins.find(seq_no);
        if (it == twins.end()) return false;
        out = it->second;
        return true;
    }
    void impersonate(const Twin* t) override { log.push_back(t ? std::string("impersonate ") + t->name.data() : "impersonate none"); }
    void detach() override {
        log.push_back("detach");
        sink.state = pd::SinkState::Idle;
        ev = Ev::Dark; ev_at = now + 100;
    }
    void attach() override {
        log.push_back("attach");
        ev = Ev::Attach; ev_at = now + 200;
    }
    void hold() override {
        log.push_back("hold");
        ev = Ev::Attach; ev_at = now + 200;
    }
    pd::Err request(pd::Choice c) override {
        log.push_back("request pos " + std::to_string(c.pos) + " mv " + std::to_string(c.mv) + " ma " + std::to_string(c.ma));
        last = c;
        if (req_err != pd::Err::Ok) return req_err;
        sink.state = pd::SinkState::Requesting;
        sink.result = pd::SinkResult::None;
        ev = Ev::Reply; ev_at = now + 100;
        return pd::Err::Ok;
    }
    void release() override {
        log.push_back("release");
        sink.state = pd::SinkState::Transition;
        ev = Ev::Release; ev_at = now + 100;
    }
    void record(std::uint8_t seq_no, const Slot& s) override { recorded[seq_no] = s; log.push_back(std::string("record ") + std::to_string(seq_no) + " " + name(s.verdict)); }
    void finished(Reason why) override { finished_called = true; fin = why; log.push_back(std::string("finished ") + name(why)); }

    int level_of(const pd::Choice& c) const { return c.mv != 0 ? int(c.mv) : int(caps.pdos[c.pos - 1].max_mv); }

    void fire() {
        if (ev == Ev::None || now < ev_at) return;
        const Ev e = ev;
        ev = Ev::None;
        switch (e) {
            case Ev::Dark: vbus = never_dark ? 5000 : 0; break;
            case Ev::Attach:
                if (swap_caps) caps = caps_after;
                if (!no_caps) { ++seq; sink.state = pd::SinkState::Ready; sink.pos = 1; sink.result = pd::SinkResult::None; vbus = 5000; }
                break;
            case Ev::Reply:
                switch (mode) {
                    case Mode::Grant: sink.state = pd::SinkState::Ready; sink.pos = last.pos; vbus = level_of(last); break;
                    case Mode::Reject: sink.state = pd::SinkState::Ready; sink.pos = 1; sink.result = pd::SinkResult::Rejected; break;
                    case Mode::NoPsRdy: break;
                    case Mode::Timeout: sink.state = pd::SinkState::Failed; sink.result = pd::SinkResult::Timeout; break;
                    case Mode::OutOfBand: sink.state = pd::SinkState::Ready; sink.pos = last.pos; break;
                    case Mode::Over: sink.state = pd::SinkState::Ready; sink.pos = last.pos; vbus = 40000; break;
                    case Mode::Reset: sink.state = pd::SinkState::Idle; sink.result = pd::SinkResult::HardReset; break;
                }
                break;
            case Ev::Release:
                sink.state = pd::SinkState::Ready; sink.pos = 1; sink.result = pd::SinkResult::None; vbus = 5000;
                break;
            case Ev::None: break;
        }
    }

    Input input() const {
        Input in;
        in.now = now;
        in.vbus_mv = vbus;
        in.caps = &caps;
        in.caps_seq = seq;
        in.sink = sink;
        in.cap_mv = cap_mv;
        in.self_powered = self_powered;
        in.busy = busy;
        in.inline_link = inline_link;
        return in;
    }

    Refusal start(std::initializer_list<std::uint8_t> seqs, bool confirmed = true) {
        std::vector<std::uint8_t> v(seqs);
        return r.start(*this, v, input(), confirmed);
    }
    // Runs until the runner stops or `ms` pass.
    void run(std::uint32_t ms = 120000) {
        for (std::uint32_t t = 0; t < ms && r.running(); t += 10) step();
    }
    void step() {
        now += 10;
        fire();
        r.poll(input());
    }
    int index_of(const std::string& prefix, std::size_t from = 0) const {
        for (std::size_t i = from; i < log.size(); ++i)
            if (log[i].compare(0, prefix.size(), prefix) == 0) return int(i);
        return -1;
    }
    int count_of(const std::string& prefix) const {
        int n = 0;
        for (const auto& l : log) if (l.compare(0, prefix.size(), prefix) == 0) ++n;
        return n;
    }
};

}  // namespace

static void test_refusals_touch_nothing() {
    {
        Sim s;
        CHECK_EQ(int(s.start({1}, false)), int(Refusal::NeedConfirm));
        CHECK_COND(!s.r.running());
        CHECK_COND(s.log.empty());
    }
    {
        Sim s;
        s.busy = true;
        CHECK_EQ(int(s.start({1})), int(Refusal::Busy));
        CHECK_COND(s.log.empty());
    }
    {
        Sim s;
        s.inline_link = true;
        CHECK_EQ(int(s.start({1})), int(Refusal::Inline));
        CHECK_COND(s.log.empty());
    }
    {
        Sim s;
        s.cap_mv = 0;
        CHECK_EQ(int(s.start({1})), int(Refusal::NoMonitor));
        s.cap_mv = 36000;
        s.vbus = -1;
        CHECK_EQ(int(s.start({1})), int(Refusal::NoMonitor));
    }
    {
        Sim s;
        s.vbus = 0;
        CHECK_EQ(int(s.start({1})), int(Refusal::NoSource));
        s.vbus = 5000;
        s.caps.valid = false;
        CHECK_EQ(int(s.start({1})), int(Refusal::NoSource));
        CHECK_COND(s.log.empty());
    }
    {
        Sim s;
        CHECK_EQ(int(s.start({})), int(Refusal::NoTwins));
        CHECK_EQ(int(s.start({1})), int(Refusal::None));
        CHECK_EQ(int(s.start({2})), int(Refusal::Busy));   // a run owns the link
    }
}

static void test_granted_pps() {
    Sim s;
    CHECK_EQ(int(s.start({1})), int(Refusal::None));
    CHECK_COND(s.r.running());
    s.run();
    CHECK_COND(!s.r.running());
    CHECK_COND(s.finished_called);
    CHECK_EQ(int(s.fin), int(Reason::Done));
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Granted));
    CHECK_COND(s.r.slot(1).measured_mv == 11000);
    // The request is what predict chose for the Pixel on this charger: PPS position 6, 11.00 V, 3.00 A
    CHECK_EQ(s.last.pos, 6u);
    CHECK_EQ(s.last.mv, 11000u);
    CHECK_EQ(s.last.ma, 3000u);
    // Order: impersonate, detach, attach, request, release, then the K2's own words back with a last attach cycle
    const int imp = s.index_of("impersonate Pixel"), det = s.index_of("detach"), att = s.index_of("attach"), req = s.index_of("request");
    const int rel = s.index_of("release"), none = s.index_of("impersonate none");
    CHECK_COND(imp == 0 && det > imp && att > det && req > att && rel > req && none > rel);
    CHECK_COND(s.index_of("detach", none) > none && s.index_of("attach", none) > none);
    CHECK_EQ(s.log.back() == "finished done", true);
    CHECK_EQ(s.count_of("request"), 1);
}

static void test_five_volts_needs_no_request() {
    Sim s;
    s.start({2});
    s.run();
    CHECK_EQ(int(s.r.slot(2).verdict), int(Verdict::Granted));
    CHECK_EQ(s.count_of("request"), 0);
    CHECK_EQ(s.count_of("release"), 0);
    CHECK_COND(s.r.slot(2).measured_mv >= 4750 && s.r.slot(2).measured_mv <= 5250);
}

static void test_skipped_touches_nothing() {
    // the monitor ceiling is 9 V: the Pixel's 11 V level is above it, so nothing is sent for it
    Sim s;
    s.cap_mv = 9000;
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Skipped));
    CHECK_EQ(int(s.r.slot(1).skip), int(Skip::AboveCeiling));
    CHECK_EQ(s.count_of("detach"), 0);
    CHECK_EQ(s.count_of("attach"), 0);
    CHECK_EQ(s.count_of("impersonate"), 0);
    CHECK_EQ(int(s.fin), int(Reason::Done));
}

static void test_skipped_no_twin_and_no_prediction() {
    Sim s;
    s.twins[4] = twin_of({fx(12000, 3000)}, "Odd");   // PDO 1 is not 5 V: nothing usable
    s.start({9, 4, 2});
    s.run();
    CHECK_EQ(int(s.r.slot(9).skip), int(Skip::NoTwin));
    CHECK_EQ(int(s.r.slot(4).verdict), int(Verdict::Skipped));
    CHECK_EQ(int(s.r.slot(4).skip), int(Skip::NoPrediction));
    CHECK_EQ(int(s.r.slot(2).verdict), int(Verdict::Granted));
}

static void test_refused() {
    Sim s;
    s.mode = Mode::Reject;
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Refused));
    CHECK_EQ(s.last.pos, 6u);
}

static void test_no_ps_rdy() {
    Sim s;
    s.mode = Mode::NoPsRdy;   // never answers: the backstop decides
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::NoPsRdy));
    Sim t;
    t.mode = Mode::Timeout;   // the engine says so itself
    t.start({1});
    t.run();
    CHECK_EQ(int(t.r.slot(1).verdict), int(Verdict::NoPsRdy));
}

static void test_out_of_band() {
    Sim s;
    s.mode = Mode::OutOfBand;   // contract Ready at the PPS level but VBUS stays at 5 V
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::OutOfBand));
    CHECK_COND(s.r.slot(1).measured_mv >= 4900 && s.r.slot(1).measured_mv <= 5100);
    CHECK_EQ(s.count_of("release"), 1);
}

static void test_unsafe_overvoltage_releases_at_once() {
    Sim s;
    s.mode = Mode::Over;   // 40 V on a 36 V monitor
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Unsafe));
    CHECK_COND(s.r.slot(1).measured_mv >= 40000);
    CHECK_EQ(s.count_of("release"), 1);
    CHECK_EQ(int(s.fin), int(Reason::Done));
}

static void test_reset() {
    Sim s;
    s.mode = Mode::Reset;
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Reset));
}

static void test_no_caps() {
    Sim s;
    s.no_caps = true;   // the charger never lists after the attach
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::NoCaps));
    CHECK_EQ(s.count_of("request"), 0);
    CHECK_EQ(int(s.fin), int(Reason::Done));
}

static void test_replan_on_the_fresh_list() {
    // The charger shows another list to the twin than to the K2: 5/9 V only. The Pixel's 9 V fixed level is what is requested.
    Sim s;
    s.caps_after = caps_of({fx(5000, 3000), fx(9000, 3000)});
    s.swap_caps = true;
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Granted));
    CHECK_EQ(s.last.pos, 2u);
    CHECK_EQ(s.last.mv, 0u);
    CHECK_COND(s.r.slot(1).measured_mv == 9000);
}

static void test_replan_skips_when_the_list_has_nothing() {
    Sim s;
    s.caps_after = caps_of({fx(5000, 1500)});   // 5 V only: the Pixel falls to FiveV, which is a grant at 5 V without a request
    s.swap_caps = true;
    s.start({1});
    s.run();
    CHECK_EQ(s.count_of("request"), 0);
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Granted));
}

static void test_request_errors_skip() {
    {
        Sim s;
        s.req_err = pd::Err::OverCap;
        s.start({1});
        s.run();
        CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Skipped));
        CHECK_EQ(int(s.r.slot(1).skip), int(Skip::AboveCeiling));
    }
    {
        Sim s;
        s.req_err = pd::Err::BadChoice;
        s.start({1});
        s.run();
        CHECK_EQ(int(s.r.slot(1).skip), int(Skip::RequestRefused));
    }
    {
        Sim s;
        s.req_err = pd::Err::Inline;   // a sink appeared behind the K2: the run stops, it is not a verdict
        s.start({1});
        s.run();
        CHECK_EQ(int(s.fin), int(Reason::Busy));
        CHECK_COND(s.finished_called);
        CHECK_EQ(s.log.back() == "finished ports taken", true);
    }
}

static void test_several_twins_release_between() {
    Sim s;
    s.start({1, 3, 2});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Granted));
    CHECK_EQ(int(s.r.slot(3).verdict), int(Verdict::Granted));
    CHECK_EQ(int(s.r.slot(2).verdict), int(Verdict::Granted));
    CHECK_EQ(s.count_of("impersonate Pixel"), 1);
    CHECK_EQ(s.count_of("impersonate OP15"), 1);
    CHECK_EQ(s.count_of("impersonate Laptop"), 1);
    // the first twin is released before the second one's impersonation and detach
    const int rel1 = s.index_of("release");
    const int imp2 = s.index_of("impersonate OP15");
    CHECK_COND(rel1 >= 0 && imp2 > rel1);
    CHECK_EQ(s.r.done_count(), 3u);
    CHECK_EQ(s.r.count(), 3u);
    CHECK_EQ(s.count_of("impersonate none"), 1);   // the K2's own words back once, at the end
}

static void test_never_dark_detach_is_short() {
    Sim s;
    s.never_dark = true;   // a source that never drops VBUS (the detach cannot be seen)
    s.start({2, 2});
    const std::uint32_t t0 = s.now;
    s.run();
    CHECK_EQ(int(s.r.slot(2).verdict), int(Verdict::Granted));
    // first detach waits kDarkMs, the second only kDetachQuickMs, the restore cycle the same: well under 3 x kDarkMs
    CHECK_COND(s.now - t0 < kDarkMs + 4 * kDetachQuickMs + 4 * kCapsMs);
}

static void test_self_powered_holds_instead_of_detaching() {
    Sim s;
    s.self_powered = true;
    s.start({1});
    s.run();
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Granted));
    CHECK_EQ(s.count_of("detach"), 0);
    CHECK_COND(s.count_of("hold") >= 1);
}

static void test_cancel_in_hold() {
    Sim s;
    s.start({1, 3});
    // run until the request is out and VBUS is at 11 V, then cancel
    for (int i = 0; i < 6000 && s.sink.pos != 6; ++i) s.step();
    CHECK_EQ(s.sink.pos, 6u);
    s.r.cancel();
    CHECK_COND(!s.r.running());
    CHECK_EQ(int(s.fin), int(Reason::Cancelled));
    CHECK_EQ(int(s.r.slot(1).verdict), int(Verdict::Cancelled));
    CHECK_EQ(s.count_of("release"), 1);
    CHECK_COND(s.index_of("impersonate none") > s.index_of("release"));
    CHECK_EQ(s.count_of("impersonate OP15"), 0);   // the second twin never started
    // cancel on an idle runner does nothing
    const std::size_t n = s.log.size();
    s.r.cancel();
    CHECK_EQ(s.log.size(), n);
}

static void test_cancel_in_dark_reattaches() {
    Sim s;
    s.start({1});
    s.step();
    CHECK_COND(s.index_of("detach") >= 0);
    CHECK_EQ(s.count_of("attach"), 0);
    s.r.cancel();
    CHECK_COND(s.index_of("attach") > s.index_of("detach"));
    CHECK_EQ(s.count_of("release"), 0);
    CHECK_EQ(int(s.fin), int(Reason::Cancelled));
    CHECK_COND(s.index_of("impersonate none") >= 0);
}

static void test_busy_mid_run_cancels() {
    Sim s;
    s.start({1});
    for (int i = 0; i < 100; ++i) s.step();
    s.busy = true;
    s.step();
    CHECK_COND(!s.r.running());
    CHECK_EQ(int(s.fin), int(Reason::Busy));
}

static void test_names() {
    CHECK_STR(name(Verdict::Granted), "granted");
    CHECK_STR(name(Verdict::NoPsRdy), "no PS_RDY");
    CHECK_STR(name(Skip::AboveCeiling), "above the monitor ceiling");
    CHECK_STR(name(Refusal::NeedConfirm), "confirm needed");
    CHECK_EQ(sizeof(Slot), 4u);
}

int main() {
    test_refusals_touch_nothing();
    test_granted_pps();
    test_five_volts_needs_no_request();
    test_skipped_touches_nothing();
    test_skipped_no_twin_and_no_prediction();
    test_refused();
    test_no_ps_rdy();
    test_out_of_band();
    test_unsafe_overvoltage_releases_at_once();
    test_reset();
    test_no_caps();
    test_replan_on_the_fresh_list();
    test_replan_skips_when_the_list_has_nothing();
    test_request_errors_skip();
    test_several_twins_release_between();
    test_never_dark_detach_is_short();
    test_self_powered_holds_instead_of_detaching();
    test_cancel_in_hold();
    test_cancel_in_dark_reattaches();
    test_busy_mid_run_cancels();
    test_names();
    return test_report("test_twin_live");
}
