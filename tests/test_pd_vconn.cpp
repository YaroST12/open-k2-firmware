// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::pd::Vconn: the sink-side VCONN_Swap engine (pure logic, no hardware).
#include "k2/pd/pd_vconn.hpp"
#include "test_util.h"

using namespace k2::pd;

static int tx_type(Vconn& v) {
    TxMsg m;
    if (!v.take_tx(m, 2)) return -1;
    return m.header & 0x1F;
}

static void test_accept_path() {
    Vconn v;
    CHECK_EQ(int(v.state()), int(VconnState::Off));
    CHECK_EQ(v.request(1000), true);
    CHECK_EQ(v.request(1001), false);                       // already running
    CHECK_EQ(tx_type(v), int(kCtlVconnSwap));
    CHECK_EQ(tx_type(v), -1);                               // one message only
    CHECK_EQ(v.want_on(), false);
    v.on_control(kCtlAccept, 1010);
    CHECK_EQ(int(v.state()), int(VconnState::Settling));
    CHECK_EQ(v.want_on(), true);                            // driver switches VCONN on now
    v.tick(1015);
    CHECK_EQ(int(v.state()), int(VconnState::Settling));    // not settled yet
    CHECK_EQ(tx_type(v), -1);
    v.tick(1010 + kVconnSettleMs);
    CHECK_EQ(int(v.state()), int(VconnState::On));
    CHECK_EQ(tx_type(v), int(kCtlPsRdy));
    CHECK_EQ(v.want_on(), true);
}

static void test_refusals() {
    const struct { std::uint8_t type; VconnWhy why; } cases[] = {
        {kCtlReject, VconnWhy::Rejected}, {kCtlWait, VconnWhy::Wait}, {kCtlNotSupported, VconnWhy::NotSupported}};
    for (const auto& c : cases) {
        Vconn v;
        v.request(0);
        (void)tx_type(v);
        v.on_control(c.type, 5);
        CHECK_EQ(int(v.state()), int(VconnState::Failed));
        CHECK_EQ(int(v.why()), int(c.why));
        CHECK_EQ(v.want_on(), false);
        CHECK_EQ(v.request(10), true);                      // a failed swap may be retried
    }
}

static void test_timeout() {
    Vconn v;
    v.request(100);
    (void)tx_type(v);
    v.tick(100 + kVconnResponseMs - 1);
    CHECK_EQ(int(v.state()), int(VconnState::SwapSent));
    v.tick(100 + kVconnResponseMs);
    CHECK_EQ(int(v.state()), int(VconnState::Failed));
    CHECK_EQ(int(v.why()), int(VconnWhy::Timeout));
    CHECK_EQ(v.want_on(), false);
}

static void test_partner_takes_role_back() {
    Vconn v;
    v.request(0);
    (void)tx_type(v);
    v.on_control(kCtlAccept, 1);
    v.tick(1 + kVconnSettleMs);
    (void)tx_type(v);                                       // PS_RDY
    v.on_control(kCtlVconnSwap, 500);
    CHECK_EQ(int(v.state()), int(VconnState::Releasing));
    CHECK_EQ(tx_type(v), int(kCtlAccept));
    CHECK_EQ(v.want_on(), true);                            // still on until the partner's PS_RDY
    CHECK_EQ(int(v.swaps_rx()), 1);
    v.on_control(kCtlPsRdy, 510);
    CHECK_EQ(int(v.state()), int(VconnState::Off));
    CHECK_EQ(int(v.why()), int(VconnWhy::ReleasedByPartner));
    CHECK_EQ(v.want_on(), false);
}

static void test_partner_ps_rdy_missing() {
    Vconn v;
    v.request(0);
    (void)tx_type(v);
    v.on_control(kCtlAccept, 1);
    v.tick(1 + kVconnSettleMs);
    (void)tx_type(v);
    v.on_control(kCtlVconnSwap, 500);
    (void)tx_type(v);
    v.tick(500 + kVconnReleaseMs);
    CHECK_EQ(int(v.state()), int(VconnState::Off));
    CHECK_EQ(int(v.why()), int(VconnWhy::Timeout));
    CHECK_EQ(v.want_on(), false);
}

static void test_not_source_refuses_partner_swap() {
    Vconn v;
    v.on_control(kCtlVconnSwap, 0);
    CHECK_EQ(tx_type(v), int(kCtlNotSupported));            // we do not take the role unasked
    CHECK_EQ(v.want_on(), false);
    CHECK_EQ(int(v.state()), int(VconnState::Off));
}

static void test_reset() {
    Vconn v;
    v.request(0);
    v.on_control(kCtlAccept, 1);
    CHECK_EQ(v.want_on(), true);
    v.reset();
    CHECK_EQ(v.want_on(), false);
    CHECK_EQ(int(v.state()), int(VconnState::Off));
    CHECK_EQ(tx_type(v), -1);
}

int main() {
    test_accept_path();
    test_refusals();
    test_timeout();
    test_partner_takes_role_back();
    test_partner_ps_rdy_missing();
    test_not_source_refuses_partner_swap();
    test_reset();
    return test_report("test_pd_vconn");
}
