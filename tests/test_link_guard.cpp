// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/link_guard.hpp"
#include "test_util.h"

using namespace k2::link;

static Probe probe(int vbus, int ma, bool pd = false) {
    Probe p;
    p.vbus_known = true;
    p.vbus_mv = vbus;
    p.current_known = true;
    p.current_ma = ma;
    p.pd_active = pd;
    return p;
}

static Probe cc(int vbus, int ma, bool cc_on = true) {
    Probe p = probe(vbus, ma);
    p.cc_active = cc_on;
    return p;
}

static void test_pd_likely() {
    CHECK_EQ(pd_likely(cc(8898, 1351)), true);                 // Pixel after a reboot: contract, caps missed
    CHECK_EQ(pd_likely(cc(5000, 1351)), false);                // 5 V is no proof of a contract
    CHECK_EQ(pd_likely(cc(5500, 1351)), false);                // strictly above 5.5 V
    CHECK_EQ(pd_likely(cc(5501, 1351)), true);
    CHECK_EQ(pd_likely(cc(8898, 50)), false);                  // no load
    CHECK_EQ(pd_likely(cc(8898, -400)), true);
    CHECK_EQ(pd_likely(cc(8898, 1351, false)), false);         // no CC line: legacy source
    Probe d = cc(8898, 1351);
    d.pd_active = true;
    CHECK_EQ(pd_likely(d), false);                             // already a known contract
    Probe blind;
    blind.cc_active = true;
    CHECK_EQ(pd_likely(blind), false);
}

static void test_reason() {
    CHECK_EQ(inline_reason(probe(5000, 0)), Reason::None);
    CHECK_EQ(inline_reason(probe(5000, 13)), Reason::None);          // an idle laptop draws ~13 mA
    CHECK_EQ(inline_reason(probe(5000, 50)), Reason::None);          // the limit itself is allowed
    CHECK_EQ(inline_reason(probe(5000, 51)), Reason::DataDevice);
    CHECK_EQ(inline_reason(probe(5000, -400)), Reason::DataDevice);  // only the magnitude counts
    CHECK_EQ(inline_reason(probe(8900, 1350, true)), Reason::PdContract);   // a contract outranks the load
    CHECK_EQ(inline_reason(probe(5000, 0, true)), Reason::PdContract);
    CHECK_EQ(inline_reason(probe(0, 900)), Reason::None);            // no VBUS: nothing attached
    CHECK_EQ(inline_reason(probe(3900, 900)), Reason::None);
    CHECK_EQ(inline_reason(probe(4000, 900)), Reason::DataDevice);
    Probe blind;                                                      // power monitor offline
    CHECK_EQ(inline_reason(blind), Reason::None);
    blind.pd_active = true;
    CHECK_EQ(inline_reason(blind), Reason::PdContract);
    Probe nocur = probe(5000, 900);
    nocur.current_known = false;
    CHECK_EQ(inline_reason(nocur), Reason::None);
}

static void test_sticky_inline() {
    // pd::Link owns the verdict: a published sticky flag wins over an idle 13 mA reading.
    Probe idle = probe(5000, 13);
    CHECK_EQ(inline_reason(idle), Reason::None);
    idle.sticky_inline = true;
    CHECK_EQ(inline_reason(idle), Reason::DataDevice);
    Probe blind;
    blind.sticky_inline = true;                       // monitor offline: the flag still holds
    CHECK_EQ(inline_reason(blind), Reason::DataDevice);
    blind.pd_active = true;
    CHECK_EQ(inline_reason(blind), Reason::PdContract);
    CHECK_EQ(kInlineCurrentMa, k2::pd::kInlineCurrentMa); // one threshold, not two copies
}

// The inline VOOC monitor's gate: a far sink draws through the K2, with or without a PD contract (the USB-C phone holds a
// 5 V contract and runs SuperVOOC on D+/D-). inline_reason() says PdContract first, so it cannot answer this.
static void test_far_sink_draws() {
    CHECK_EQ(far_sink_draws(probe(9130, 2800, true)), true);          // PD contract + load
    CHECK_EQ(far_sink_draws(probe(9130, 2800, false)), true);         // load, no contract (USB-A port)
    CHECK_EQ(far_sink_draws(probe(5000, 13, true)), false);           // a contract, nothing drawing: the K2 itself or an idle link
    CHECK_EQ(far_sink_draws(probe(5000, 13, false)), false);
    Probe sticky = probe(5000, 13, true);
    sticky.sticky_inline = true;                                      // pd::Link latched a far sink
    CHECK_EQ(far_sink_draws(sticky), true);
    Probe own = probe(5100, 95, true);
    own.self_supply_ma = k2::pd::kSelfSupplyAllowanceMa;              // the K2's own supply current is no sink
    CHECK_EQ(far_sink_draws(own), false);
    CHECK_EQ(far_sink_draws(Probe{}), false);                         // monitor offline, nothing latched
}

static void test_names() {
    CHECK_STR(name(Reason::None), "none");
    CHECK_STR(name(Reason::PdContract), "PD contract");
    CHECK_STR(name(Reason::DataDevice), "load attached");
}

// No USB host: the K2 runs on this VBUS and its own draw crosses the shunt (USB-A port, hardware 2026-11-06: every legacy
// row refused "PD or load active" at ~95 mA with nothing else attached).
static void test_self_supply() {
    Probe p = probe(5100, 95);
    CHECK_EQ(inline_reason(p), Reason::DataDevice);                   // with a USB host: a load
    p.self_supply_ma = k2::pd::kSelfSupplyAllowanceMa;
    CHECK_EQ(inline_reason(p), Reason::None);                         // without: the K2 itself
    p.current_ma = kInlineCurrentMa + k2::pd::kSelfSupplyAllowanceMa + 1;
    CHECK_EQ(inline_reason(p), Reason::DataDevice);                   // a real sink on top still counts
}

int main() {
    test_reason();
    test_self_supply();
    test_names();
    test_sticky_inline();
    test_far_sink_draws();
    test_pd_likely();
    return test_report("test_link_guard");
}
