// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card delta (k2/chargertest/report_card_delta.hpp): what changed between two runs of the same charger.
#include <cstring>

#include "k2/chargertest/report_card_delta.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;
namespace ct = k2::charger_test;

static rc::Card base() {
    rc::Card c;
    c.group[unsigned(ct::Group::Pd)] = {rc::GV::Fail, 20000, 7, uint8_t(ct::Reason::Voltage)};
    c.group[unsigned(ct::Group::Qc3)] = {rc::GV::Pass, 9000, 0, 0};
    c.group[unsigned(ct::Group::Afc)] = {rc::GV::Skip, 0, 0, uint8_t(ct::Reason::NoOffer)};
    c.n_ranges = 1;
    c.range[0] = {uint8_t(ct::Algorithm::Pps), 4, 33, 210, 210, 130};
    return c;
}

static void test_no_change() {
    rc::DeltaLine d[8];
    CHECK_EQ(rc::delta(base(), base(), d), 0u);
}

static void test_changes() {
    rc::Card was = base(), now = base();
    now.range[0].fail_dv = 0;                                            // the 13 V step passes now
    now.group[unsigned(ct::Group::Pd)] = {rc::GV::Pass, 20000, 0, 0};
    now.group[unsigned(ct::Group::Qc3)] = {rc::GV::Fail, 0, 2, uint8_t(ct::Reason::Timeout)};
    now.group[unsigned(ct::Group::Afc)] = {rc::GV::None, 0, 0, 0};       // not part of this run: not a change
    rc::DeltaLine d[8];
    const size_t n = rc::delta(was, now, d);
    CHECK_EQ(n, 3u);
    char s[40];
    bool pd = false, qc3 = false, pps = false;
    for (size_t i = 0; i < n; ++i) {
        rc::format_delta(d[i], s, sizeof s);
        if (!std::strcmp(s, "PD: was FAIL, now PASS")) pd = true;
        if (!std::strcmp(s, "QC3: was PASS, now FAIL")) qc3 = true;
        if (!std::strcmp(s, "PPS 13V: was FAIL, now PASS")) pps = true;
    }
    CHECK_EQ(pd, 1); CHECK_EQ(qc3, 1); CHECK_EQ(pps, 1);
}

static void test_range_change_and_bounds() {
    rc::Card was = base(), now = base();
    now.range[0].fail_dv = 90;                                           // fails earlier now
    rc::DeltaLine d[1];
    CHECK_EQ(rc::delta(was, now, d), 1u);                                // out holds one line, never overruns
    char s[40];
    rc::format_delta(d[0], s, sizeof s);
    CHECK_STR(s, "PPS: fail 13V > 9V");
    rc::Card other = base();
    other.n_ranges = 0;                                                  // a range that vanished is not a line
    rc::DeltaLine e[4];
    CHECK_EQ(rc::delta(was, other, e), 0u);
}

int main() {
    test_no_change();
    test_changes();
    test_range_change_and_bounds();
    return test_report("test_report_card_delta");
}
