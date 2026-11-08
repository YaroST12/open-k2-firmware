// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fake source-capability generator and override used by `k2_remote.py debug pdos --fake N`.
#include "test_util.h"
#include "ui_stubs.h"
#include "k2/pd/pd_source.hpp"

using namespace k2;

static void test_generator() {
    pd::SourceCaps c{};
    pd::fake_fill(c, 7);
    CHECK_EQ(c.valid, 1);
    CHECK_EQ(c.num_pdos, 7);
    CHECK_EQ(c.spec_rev, 2);
    CHECK_EQ(c.pdos[0].type, pd::PdoType::Fixed);
    CHECK_EQ(c.pdos[0].max_mv, 5000);
    int fix = 0, pps = 0, avs = 0;
    for (int i = 0; i < 7; i++) {
        fix += c.pdos[i].type == pd::PdoType::Fixed;
        pps += c.pdos[i].type == pd::PdoType::Pps;
        avs += c.pdos[i].type == pd::PdoType::Avs;
        CHECK_EQ(c.pdos[i].max_mw > 0, 1);
        CHECK_EQ(c.pdos[i].max_mv >= c.pdos[i].min_mv, 1);
    }
    CHECK_EQ(fix > 0 && pps > 0 && avs > 0, 1);       // the mix covers every row layout
    pd::fake_fill(c, 11);                              // EPR positions 8 to 11
    CHECK_EQ(c.num_pdos, 11);
    CHECK_EQ(c.pdos[9].max_mv, 48000);
    CHECK_EQ(c.pdos[10].type, pd::PdoType::Avs);
    pd::fake_fill(c, 2);
    CHECK_EQ(c.num_pdos, 2);
    pd::fake_fill(c, 99);                              // clamped to pd::kMaxPdos
    CHECK_EQ(c.num_pdos, pd::kMaxPdos);
}

static void test_override() {
    CHECK_EQ(pd::is_fake(), 0);
    CHECK_EQ(&pd::active_caps(), &fusb302::caps());
    pd::set_fake(4);
    CHECK_EQ(pd::is_fake(), 1);
    CHECK_EQ(pd::active_caps().num_pdos, 4);
    pd::set_fake(0);
    CHECK_EQ(pd::is_fake(), 0);
    CHECK_EQ(&pd::active_caps(), &fusb302::caps());
}

int main() {
    test_generator();
    test_override();
    return test_report("test_pd_source");
}
