// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The guarded SuperVOOC trigger's guards and session (k2/fastcharge/svooc_run.hpp): every refusal at the start, every
// abort while it runs (with the 12.0 V and 50 mA boundaries), the wake / listen / rest steps, the exit check that VBUS
// came back to 5 V, and the report.
#include <string_view>

#include "k2/fastcharge/svooc_run.hpp"
#include "test_util.h"

using namespace k2;
using svooc::Abort;
using svooc::Refusal;
using svooc::Stage;
using svooc::Next;

#define CHECK(c) CHECK_EQ(!!(c), 1)

namespace {

svooc::StartFacts ok_start() {
    svooc::StartFacts f;
    f.confirmed = true;
    f.host_ok = true;
    f.vbus_known = f.current_known = true;
    f.vbus_mv = 5100;
    f.ibus_ma = 3;
    f.ram_free = true;
    return f;
}

svooc::Sample ok_sample(std::uint32_t now) {
    svooc::Sample s;
    s.now_ms = now;
    s.host_ok = true;
    s.vbus_known = s.current_known = true;
    s.vbus_mv = 5100;
    s.ibus_ma = 2;
    return s;
}

void test_start_refusals() {
    CHECK_EQ(svooc::refusal(ok_start()), Refusal::None);
    struct Case { void (*edit)(svooc::StartFacts&); Refusal want; };
    const Case cases[] = {
        {[](svooc::StartFacts& f) { f.confirmed = false; }, Refusal::NotConfirmed},
        {[](svooc::StartFacts& f) { f.ports_busy = true; }, Refusal::Busy},
        {[](svooc::StartFacts& f) { f.host_ok = false; }, Refusal::NoHost},
        {[](svooc::StartFacts& f) { f.vbus_known = false; }, Refusal::NoMonitor},
        {[](svooc::StartFacts& f) { f.current_known = false; }, Refusal::NoMonitor},
        {[](svooc::StartFacts& f) { f.far_sink = true; }, Refusal::Sink},
        {[](svooc::StartFacts& f) { f.inline_mode = true; }, Refusal::Inline},
        {[](svooc::StartFacts& f) { f.contract = svooc::Contract::Other; }, Refusal::PdContract},
        {[](svooc::StartFacts& f) { f.ibus_ma = 51; }, Refusal::Current},
        {[](svooc::StartFacts& f) { f.ibus_ma = -51; }, Refusal::Current},
        {[](svooc::StartFacts& f) { f.vbus_mv = 4499; }, Refusal::NotFiveVolts},
        {[](svooc::StartFacts& f) { f.vbus_mv = 5501; }, Refusal::NotFiveVolts},
        {[](svooc::StartFacts& f) { f.ram_free = false; }, Refusal::NoRam},
    };
    for (const Case& c : cases) {
        svooc::StartFacts f = ok_start();
        c.edit(f);
        CHECK_EQ(svooc::refusal(f), c.want);
    }
    // the boundaries that still start
    for (int ma : {50, -50, 0}) { svooc::StartFacts f = ok_start(); f.ibus_ma = ma; CHECK_EQ(svooc::refusal(f), Refusal::None); }
    for (int mv : {4500, 5500}) { svooc::StartFacts f = ok_start(); f.vbus_mv = mv; CHECK_EQ(svooc::refusal(f), Refusal::None); }
    // a refused start leaves the run idle with the reason in the report
    svooc::Run r;
    svooc::StartFacts f = ok_start();
    f.ram_free = false;
    CHECK_EQ(r.start(f, 100), Refusal::NoRam);
    CHECK_EQ(r.stage(), Stage::Idle);
    CHECK_EQ(r.report().refusal, Refusal::NoRam);
}

// Started: wake at once, listen kWakeListenMs, rest kWakeRestMs, up to kWakeTries wakes; no command at all ends the run
// as NoAdapter.
void test_wake_listen_rest_and_no_adapter() {
    svooc::Run r;
    CHECK_EQ(r.start(ok_start(), 1000), Refusal::None);
    std::uint32_t t = 1000;
    CHECK_EQ(r.poll(ok_sample(t)), Next::Wake);
    CHECK_EQ(r.stage(), Stage::Listen);
    CHECK_EQ(r.poll(ok_sample(t + svooc::kWakeListenMs - 1)), Next::None);
    CHECK_EQ(r.poll(ok_sample(t + svooc::kWakeListenMs)), Next::Rest);
    t += svooc::kWakeListenMs;
    CHECK_EQ(r.poll(ok_sample(t + svooc::kWakeRestMs - 1)), Next::None);
    CHECK_EQ(r.poll(ok_sample(t + svooc::kWakeRestMs)), Next::Wake);
    t += svooc::kWakeRestMs;
    CHECK_EQ(r.poll(ok_sample(t + svooc::kWakeListenMs)), Next::Rest);
    t += svooc::kWakeListenMs + svooc::kWakeRestMs;
    CHECK_EQ(r.poll(ok_sample(t)), Next::Wake);
    CHECK_EQ(r.report().tries, svooc::kWakeTries);
    CHECK_EQ(r.poll(ok_sample(t + svooc::kWakeListenMs)), Next::Release);
    CHECK_EQ(r.report().abort, Abort::NoAdapter);
    CHECK_EQ(r.stage(), Stage::Exit);
}

// A command heard while listening: the run answers (the wire does the work) until the time cap.
void test_answer_until_the_cap() {
    svooc::Run r;
    r.start(ok_start(), 0);
    CHECK_EQ(r.poll(ok_sample(0)), Next::Wake);
    svooc::Sample s = ok_sample(600);
    s.commands = 1;
    CHECK_EQ(r.poll(s), Next::None);
    CHECK_EQ(r.stage(), Stage::Answer);
    s.now_ms = svooc::kCapMs - 1;
    s.accepted = true;
    CHECK_EQ(r.poll(s), Next::None);
    s.now_ms = svooc::kCapMs;
    CHECK_EQ(r.poll(s), Next::Release);
    CHECK_EQ(r.report().abort, Abort::TimeCap);
    CHECK(r.report().accepted);
    // a shorter cap from the caller, never a longer one
    svooc::Run q;
    q.start(ok_start(), 0, 5000);
    q.poll(ok_sample(0));
    CHECK_EQ(q.poll(ok_sample(4999)), Next::Rest);
    svooc::Run w;
    w.start(ok_start(), 0, 60000);
    CHECK_EQ(w.report().cap_ms, svooc::kCapMs);
}

// Running and answering: each guard ends the run with its reason and one Release.
Abort abort_after(void (*edit)(svooc::Sample&)) {
    svooc::Run r;
    r.start(ok_start(), 0);
    r.poll(ok_sample(0));
    svooc::Sample s = ok_sample(500);
    s.commands = 3;
    r.poll(s);
    s.now_ms = 700;
    edit(s);
    const Next st = r.poll(s);
    if (st != Next::Release) return Abort::None;
    s.now_ms = 720;
    if (r.poll(s) == Next::Release) return Abort::None;   // only one Release
    return r.report().abort;
}

void test_every_abort() {
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.vbus_mv = 12001; }), Abort::Overvoltage);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.vbus_mv = 12000; }), Abort::None);   // 12.0 V itself still runs
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.ibus_ma = 51; }), Abort::Current);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.ibus_ma = -51; }), Abort::Current);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.ibus_ma = 50; }), Abort::None);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.ibus_ma = -50; }), Abort::None);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.off_script = true; }), Abort::OffScript);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.host_ok = false; }), Abort::HostGone);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.vbus_known = false; }), Abort::NoMonitor);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.current_known = false; }), Abort::NoMonitor);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.far_sink = true; }), Abort::Sink);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.inline_mode = true; }), Abort::Sink);
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.contract = svooc::Contract::Other; }), Abort::PdContract);   // appeared
    CHECK_EQ(abort_after([](svooc::Sample& s) { s.storm = true; }), Abort::Storm);
    // the guards hold in every stage, also while listening after a wake
    svooc::Run r;
    r.start(ok_start(), 0);
    r.poll(ok_sample(0));
    svooc::Sample s = ok_sample(10);
    s.vbus_mv = 12500;
    CHECK_EQ(r.poll(s), Next::Release);
    CHECK_EQ(r.report().abort, Abort::Overvoltage);
}

// stop() (the lease, `stop`, BACK): Release once, reason Stopped; a stop of an idle run does nothing.
void test_stop() {
    svooc::Run idle;
    CHECK_EQ(idle.stop(Abort::Stopped, 5), Next::None);
    CHECK_EQ(idle.stage(), Stage::Idle);
    svooc::Run r;
    r.start(ok_start(), 0);
    r.poll(ok_sample(0));
    CHECK_EQ(r.stop(Abort::Stopped, 100), Next::Release);
    CHECK_EQ(r.stop(Abort::Stopped, 101), Next::None);
    CHECK_EQ(r.report().abort, Abort::Stopped);
    CHECK_EQ(r.stage(), Stage::Exit);
}

// After the release: VBUS back at or under 5.5 V ends the run with exit_ok; still high after kExitWatchMs ends it with
// exit_ok false and the reading, for the report.
void test_exit_check() {
    svooc::Run r;
    r.start(ok_start(), 0);
    r.poll(ok_sample(0));
    svooc::Sample s = ok_sample(500);
    s.commands = 30;
    s.accepted = true;
    s.vbus_mv = 8900;
    r.poll(s);
    s.now_ms = 600;
    s.vbus_mv = 12001;
    CHECK_EQ(r.poll(s), Next::Release);
    s.now_ms = 1500;
    s.vbus_mv = 9000;
    CHECK_EQ(r.poll(s), Next::None);
    CHECK_EQ(r.stage(), Stage::Exit);
    s.now_ms = 2600;
    s.vbus_mv = 5500;
    r.poll(s);
    CHECK_EQ(r.stage(), Stage::Done);
    const svooc::Report rep = r.report();
    CHECK(rep.exit_checked);
    CHECK(rep.exit_ok);
    CHECK_EQ(rep.vbus_exit_mv, 5500);
    CHECK_EQ(rep.vbus_max_mv, 12001);
    CHECK_EQ(rep.vbus_start_mv, 5100);
    CHECK_EQ(rep.abort, Abort::Overvoltage);
    CHECK_EQ(rep.elapsed_ms, 600u);   // start to release

    svooc::Run h;
    h.start(ok_start(), 0);
    h.poll(ok_sample(0));
    CHECK_EQ(h.stop(Abort::Stopped, 100), Next::Release);
    svooc::Sample hs = ok_sample(100 + svooc::kExitWatchMs - 1);
    hs.vbus_mv = 9100;
    h.poll(hs);
    CHECK_EQ(h.stage(), Stage::Exit);
    hs.now_ms = 100 + svooc::kExitWatchMs;
    h.poll(hs);
    CHECK_EQ(h.stage(), Stage::Done);
    CHECK(h.report().exit_checked);
    CHECK(!h.report().exit_ok);
    CHECK_EQ(h.report().vbus_exit_mv, 9100);
    CHECK(!h.running());
    CHECK(h.report().ibus_max_ma >= 2);
}


// The K2's own 5 V contract (a USB-C charger that wakes on Rd): allowed at the start and while it runs; any other contract
// is refused, one that changes or goes away aborts, and the report carries the contract at the start and at the end.
svooc::ContractFacts own5v() {
    svooc::ContractFacts c;
    c.pd_active = c.held = c.ready = true;
    c.pos = 1;
    c.mv = 5000;
    return c;
}

void test_contract_classification() {
    using svooc::Contract;
    CHECK_EQ(svooc::classify(svooc::ContractFacts{}), Contract::None);
    CHECK_EQ(svooc::classify(own5v()), Contract::Own5V);
    svooc::ContractFacts c = own5v();
    c.mv = 4900;                       // vSafe5V band: a fixed 5 V PDO reads at or under 5000
    CHECK_EQ(svooc::classify(c), Contract::Own5V);
    c = own5v(); c.mv = 9000; c.pos = 2;
    CHECK_EQ(svooc::classify(c), Contract::Other);        // above 5 V
    c = own5v(); c.mv = 5001;
    CHECK_EQ(svooc::classify(c), Contract::Other);
    c = own5v(); c.pos = 3;                               // a PPS / AVS object, even asked at 5 V
    CHECK_EQ(svooc::classify(c), Contract::Other);
    c = own5v(); c.epr_mode = true;
    CHECK_EQ(svooc::classify(c), Contract::Other);
    c = own5v(); c.inline_mode = true;                    // a charger behind the K2 / a far sink holds it
    CHECK_EQ(svooc::classify(c), Contract::Other);
    c = own5v(); c.far_sink = true;
    CHECK_EQ(svooc::classify(c), Contract::Other);
    c = own5v(); c.held = false;                          // decoded passively, not the K2's request
    CHECK_EQ(svooc::classify(c), Contract::Other);
    c = own5v(); c.ready = false;                         // mid-renegotiation
    CHECK_EQ(svooc::classify(c), Contract::Other);
    c = svooc::ContractFacts{}; c.pd_active = true;       // PD seen, nothing of ours
    CHECK_EQ(svooc::classify(c), Contract::Other);
}

void test_own_contract_start() {
    svooc::StartFacts f = ok_start();
    f.contract = svooc::Contract::Own5V;
    CHECK_EQ(svooc::refusal(f), Refusal::None);
    f.contract = svooc::Contract::Other;                  // 9 V, PPS, EPR, a far holder
    CHECK_EQ(svooc::refusal(f), Refusal::PdContract);
    f.contract = svooc::Contract::Own5V;
    f.inline_mode = true;                                 // inline is refused whatever the contract
    CHECK_EQ(svooc::refusal(f), Refusal::Inline);
    f.inline_mode = false; f.far_sink = true;
    CHECK_EQ(svooc::refusal(f), Refusal::Sink);
}

svooc::Run own_run(std::uint32_t& t) {
    svooc::Run r;
    svooc::StartFacts f = ok_start();
    f.contract = svooc::Contract::Own5V;
    r.start(f, 0);
    t = 0;
    return r;
}

svooc::Sample own_sample(std::uint32_t now) {
    svooc::Sample s = ok_sample(now);
    s.contract = svooc::Contract::Own5V;
    return s;
}

void test_own_contract_runs_and_aborts() {
    using svooc::Contract;
    std::uint32_t t;
    svooc::Run r = own_run(t);
    CHECK_EQ(r.poll(own_sample(0)), Next::Wake);
    svooc::Sample s = own_sample(500);
    s.commands = 3;
    CHECK_EQ(r.poll(s), Next::None);
    s.now_ms = 900;
    s.vbus_mv = 9000;                                     // the adapter raises VBUS; the 5 V contract stays
    CHECK_EQ(r.poll(s), Next::None);
    CHECK_EQ(r.stage(), Stage::Answer);
    CHECK_EQ(r.report().contract_start, Contract::Own5V);
    CHECK_EQ(r.report().contract_end, Contract::Own5V);

    // renegotiation to anything but the same 5 V fixed PDO aborts
    svooc::Run c = own_run(t);
    c.poll(own_sample(0));
    s = own_sample(300);
    s.contract = Contract::Other;
    CHECK_EQ(c.poll(s), Next::Release);
    CHECK_EQ(c.report().abort, Abort::ContractChanged);
    CHECK_EQ(c.report().contract_start, Contract::Own5V);
    CHECK_EQ(c.report().contract_end, Contract::Other);
    CHECK(std::string_view(svooc::name(Abort::ContractChanged)) == "PD contract changed");

    // lost (hard reset, detach)
    svooc::Run l = own_run(t);
    l.poll(own_sample(0));
    s = own_sample(300);
    s.contract = Contract::None;
    CHECK_EQ(l.poll(s), Next::Release);
    CHECK_EQ(l.report().abort, Abort::ContractLost);
    CHECK_EQ(l.report().contract_end, Contract::None);
    CHECK(std::string_view(svooc::name(Abort::ContractLost)) == "PD contract lost");
    s.now_ms = 320;
    CHECK_EQ(l.poll(s), Next::None);                      // one Release only

    // no contract at the start: any contract appearing aborts, as before; an own one counts too
    svooc::Run a;
    a.start(ok_start(), 0);
    a.poll(ok_sample(0));
    s = own_sample(300);
    CHECK_EQ(a.poll(s), Next::Release);
    CHECK_EQ(a.report().abort, Abort::PdContract);
    CHECK_EQ(a.report().contract_start, Contract::None);
    CHECK_EQ(a.report().contract_end, Contract::Own5V);

    // the other guards keep running under an own contract
    svooc::Run o = own_run(t);
    o.poll(own_sample(0));
    s = own_sample(300);
    s.vbus_mv = 12001;
    CHECK_EQ(o.poll(s), Next::Release);
    CHECK_EQ(o.report().abort, Abort::Overvoltage);
}

// Every reason has its own name (the log and tools/k2_remote.py print them).
void test_names() {
    for (int i = 0; i <= int(Refusal::NoRam); ++i) CHECK(svooc::name(Refusal(i))[0] != '?');
    for (int i = 0; i <= int(Abort::ContractChanged); ++i) CHECK(svooc::name(Abort(i))[0] != '?');
    CHECK(std::string_view(svooc::name(Refusal::NoRam)) == "no RAM: USB drive enabled");
}

}  // namespace

int main() {
    test_start_refusals();
    test_wake_listen_rest_and_no_adapter();
    test_answer_until_the_cap();
    test_every_abort();
    test_stop();
    test_exit_check();
    test_contract_classification();
    test_own_contract_start();
    test_own_contract_runs_and_aborts();
    test_names();
    return test_report("test_svooc_run");
}
