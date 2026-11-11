// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One-click auto-detect scanner against fake chargers: order, per-protocol outcome, the 9 V stop,
// and every abort path.
#include <cstdio>

#include "fc_fake.hpp"
#include "k2/fastcharge/fc_scan.hpp"

using namespace k2::fc;

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void run(FakeCharger& c, Engine& e, Scanner& s, std::uint32_t ms) {
    for (std::uint32_t i = 0; i < ms && s.running(); i++) {
        c.now++;
        c.update_open();
        e.poll();
        s.poll();
    }
}

static const ScanResult* find(const Scanner& s, Proto p) {
    for (const auto& r : s.results()) if (r.proto == p) return &r;
    return nullptr;
}

static void order_and_refusals() {
    FakeCharger c;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(false) == Fail::NeedConfirm);   // the scan reaches 9 V: it needs the confirmation
    CHECK(c.set_calls == 0 && c.resets == 0);
    c.ceiling = 0;
    CHECK(s.start(true) == Fail::NoMonitor);
    c.ceiling = 36000;
    c.vbus = c.vbus_pending = 0;                  // nothing on VBUS
    CHECK(s.start(true) == Fail::SourceLost);
    CHECK(c.set_calls == 0);
    const auto order = kScanOrder;
    CHECK(order[0] == Proto::Fcp && order[1] == Proto::Scp && order[2] == Proto::Afc);
    CHECK(order[3] == Proto::Qc2 && order[4] == Proto::Qc3 && order[5] == Proto::Vooc);
}

static void inline_guard() {
    using k2::link::Reason;
    FakeCharger c;
    Engine e(c);
    Scanner s(e, c);
    c.inline_now = Reason::PdContract;
    CHECK(s.start(true) == Fail::Inline);          // confirmed for the voltage, still refused
    CHECK(c.set_calls == 0 && c.resets == 0 && !s.running());
    c.inline_now = Reason::DataDevice;
    CHECK(s.start(false, true) == Fail::NeedConfirm);
    CHECK(s.start(true, true) == Fail::None);      // explicit override: the steps do not re-ask
    run(c, e, s, 1000);
    CHECK(s.running());
    CHECK(e.fail() != Fail::Inline);
}

static void qc_only_charger() {
    FakeCharger c;
    c.afc_ok = false;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    run(c, e, s, 400000);
    CHECK(s.done());
    CHECK(find(s, Proto::Qc2)->outcome == Outcome::Accepted);
    CHECK(find(s, Proto::Qc2)->mv >= 8850 && find(s, Proto::Qc2)->mv <= 9900);
    CHECK(find(s, Proto::Qc3)->outcome == Outcome::Accepted);
    CHECK(find(s, Proto::Qc3)->mv <= 6600);
    CHECK(find(s, Proto::Qc3Plus) != nullptr);
    if (find(s, Proto::Qc3Plus)) CHECK(find(s, Proto::Qc3Plus)->outcome == Outcome::Accepted);
    CHECK(find(s, Proto::Fcp)->outcome == Outcome::Rejected);
    CHECK(find(s, Proto::Scp)->outcome == Outcome::Rejected);
    CHECK(find(s, Proto::Afc)->outcome == Outcome::Rejected);
    CHECK(find(s, Proto::Vooc)->outcome == Outcome::Rejected);
    CHECK(c.vbus_mv() <= 5500);                  // the scan leaves the output at 5 V
    CHECK(e.state() == State::Idle);
    CHECK(c.resets >= 6);                        // a reset after every step
}

static void never_above_9v() {
    FakeCharger c;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    int peak = 0;
    for (std::uint32_t i = 0; i < 400000 && s.running(); i++) {
        c.now++;
        c.update_open();
        e.poll();
        s.poll();
        peak = std::max(peak, c.vbus);
    }
    CHECK(peak <= 9000);
    CHECK(s.done());
}

static void ceiling_skips_9v() {
    FakeCharger c;
    c.ceiling = 8000;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    run(c, e, s, 400000);
    CHECK(find(s, Proto::Qc2)->outcome == Outcome::Skipped);   // 9 V is above the ceiling
    CHECK(find(s, Proto::Afc)->outcome == Outcome::Skipped);
    CHECK(find(s, Proto::Fcp)->outcome == Outcome::Skipped);
}

static void abort_returns_to_5v() {
    FakeCharger c;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    run(c, e, s, 200);
    s.abort();
    CHECK(!s.running());
    CHECK(e.state() == State::Idle);
    CHECK(c.resets >= 1);
}

static void overvoltage_stops_scan() {
    FakeCharger c;
    c.accp_charger = true;             // an FCP charger: the first step is answered before the source misbehaves
    c.regs[k2::accp::kRegFcpCaps] = 1;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    for (int i = 0; i < 20000 && c.reads.empty(); i++) run(c, e, s, 1);
    CHECK(!c.reads.empty());
    c.vbus = c.vbus_pending = 12000;   // the source jumps up on its own
    run(c, e, s, 100);
    CHECK(!s.running());
    CHECK(s.fail() == Fail::Overvoltage);
}

// A QC-only bank that moves VBUS on the FCP handshake's levels without ever answering ACCP: the FCP step is "rejected" (the
// engine says unsupported) and the scan goes on to the next protocol; it is not a source misbehaving after an answer.
static void unanswered_rise_does_not_stop_scan() {
    FakeCharger c;
    c.dpdm_rise_mv = 6700;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    for (int i = 0; i < 60000 && s.running() && !find(s, Proto::Fcp); i++) run(c, e, s, 1);
    CHECK(find(s, Proto::Fcp) && find(s, Proto::Fcp)->outcome == Outcome::Rejected);
    CHECK(s.running() && s.fail() == Fail::None);
    s.abort();
}

static void newly_attached_load_stops_scan() {
    FakeCharger c;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    run(c, e, s, 50);
    c.inline_now = k2::link::Reason::DataDevice;
    run(c, e, s, 1);
    CHECK(!s.running());
    CHECK(s.fail() == Fail::Inline);
    CHECK(c.resets > 0);
}

static void overvoltage_during_result_hold_stops_scan() {
    FakeCharger c;
    Engine e(c);
    Scanner s(e, c);
    CHECK(s.start(true) == Fail::None);
    for (int i = 0; i < 100000 && e.state() != State::Active; i++) run(c, e, s, 1);
    CHECK(e.state() == State::Active);
    run(c, e, s, 1); // scanner is now holding the candidate result
    c.vbus = c.vbus_pending = 12000;
    run(c, e, s, 1);
    CHECK(!s.running());
    CHECK(s.fail() == Fail::Overvoltage);
}

// The HID-cable loop 5 V lock: the scan reaches 9 V, so it does not start; a scan that is running stops at the lock.
static void hv_lock_guard() {
    FakeCharger c;
    Engine e(c);
    Scanner s(e, c);
    c.hv_lock = true;
    CHECK(s.start(true) == Fail::OverLimit);
    CHECK(c.set_calls == 0 && c.resets == 0 && !s.running());
    c.hv_lock = false;
    CHECK(s.start(true) == Fail::None);
    run(c, e, s, 300);
    CHECK(s.running());
    c.hv_lock = true;
    run(c, e, s, 5);
    CHECK(!s.running());
    CHECK(s.fail() == Fail::OverLimit);
    CHECK(e.state() != State::Negotiating && e.state() != State::Active);
}

int main() {
    hv_lock_guard();
    order_and_refusals();
    inline_guard();
    qc_only_charger();
    never_above_9v();
    ceiling_skips_9v();
    abort_returns_to_5v();
    overvoltage_stops_scan();
    unanswered_rise_does_not_stop_scan();
    newly_attached_load_stops_scan();
    overvoltage_during_result_hold_stops_scan();
    std::printf("test_fc_scan: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
