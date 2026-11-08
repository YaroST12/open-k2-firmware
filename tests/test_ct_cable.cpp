// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the pure values of the charger-test "Cable identity" choice (k2/chargertest/ct_cable.hpp).
#include "k2/chargertest/ct_cable.hpp"
#include "test_util.h"

using namespace k2;
using ctcable::Choice;
using ctcable::Kind;

static void test_valid_choices() {
    CHECK_EQ(ctcable::valid(Choice{}), true);
    CHECK_EQ(ctcable::valid(Choice{Kind::Builtin, 0}), true);     // 0 = the default built-in (older clients)
    for (unsigned id = 1; id < emrec::kBuiltinCount; ++id) CHECK_EQ(ctcable::valid(Choice{Kind::Builtin, uint16_t(id)}), true);
    CHECK_EQ(ctcable::valid(Choice{Kind::Saved, 4}), true);
    CHECK_EQ(ctcable::valid(Choice{Kind::Off, 1}), false);        // Off carries no library id
    CHECK_EQ(ctcable::valid(Choice{Kind::Builtin, uint16_t(emrec::kBuiltinCount)}), false);   // past the table
    CHECK_EQ(ctcable::valid(Choice{Kind::Builtin, 0x1234}), false);
    CHECK_EQ(ctcable::valid(Choice{Kind::Saved, 0}), false);      // a clone needs its library id
    CHECK_EQ(ctcable::valid(Choice{static_cast<Kind>(3), 0}), false);
}

static void test_facts_of_the_builtin_cable() {
    const auto f = ctcable::facts(emrec::builtin_cable(ctcable::kBuiltinKind));
    CHECK_EQ(f.max_mv, 50000);                                    // the EPR 50 V / 5 A cable
    CHECK_EQ(f.max_ma, 5000);
    const auto generic = ctcable::facts(emrec::builtin_cable(0)); // the generic 5 A / 20 V cable
    CHECK_EQ(generic.max_mv, 20000);
    CHECK_EQ(generic.max_ma, 5000);
    CHECK_EQ(f.vid, 0x2B01);                                      // the dialog's built-in is the captured identity
    CHECK_EQ(generic.vid, 0x1209);
}

static void test_canonical_builtin_ids() {
    const Choice def{Kind::Builtin, ctcable::kBuiltinKind}, two{Kind::Builtin, 2}, four{Kind::Builtin, 4}, saved{Kind::Saved, 7};
    CHECK_EQ(ctcable::canonical(Choice{Kind::Builtin, 0}) == def, true);
    CHECK_EQ(ctcable::canonical(two) == two, true);
    CHECK_EQ(ctcable::canonical(four) == four, true);
    CHECK_EQ(ctcable::canonical(Choice{Kind::Builtin, 3}) == four, true);   // retired id 3 (OnePlus 100W mag) is OnePlus 100W
    CHECK_EQ(ctcable::canonical(saved) == saved, true);
    CHECK_EQ((ctcable::canonical(Choice{Kind::Saved, 3}) == Choice{Kind::Saved, 3}), true);   // a clone's library id 3 is untouched
    CHECK_EQ(ctcable::canonical(Choice{}) == Choice{}, true);
    CHECK_EQ(ctcable::kBuiltinKind, 1u);                          // the default row stays the proven EPR 50 V / 5 A cable
    CHECK_EQ(ctcable::kDialogBuiltins, 3u);                       // rows: ids 1, 2, 4; the generic cable (0) is remote only
    CHECK_EQ(ctcable::dialog_id(0), 1u);
    CHECK_EQ(ctcable::dialog_id(1), 2u);
    CHECK_EQ(ctcable::dialog_id(2), 4u);
    CHECK_EQ(ctcable::dialog_row(1), 0);
    CHECK_EQ(ctcable::dialog_row(2), 1);
    CHECK_EQ(ctcable::dialog_row(3), 2);                          // retired: the OnePlus row
    CHECK_EQ(ctcable::dialog_row(4), 2);
    CHECK_EQ(ctcable::dialog_row(0), 0);                          // the generic cable has no row: the default one
}

static void test_facts_of_every_dialog_builtin() {
    struct Row { unsigned id, vid, pid, mv, ma; };
    const Row rows[] = {{1, 0x2B01, 0x4051, 50000, 5000}, {2, 0x2B01, 0x4051, 50000, 5000},
                        {3, 0x22D9, 0x6116, 30000, 5000}, {4, 0x22D9, 0x6116, 30000, 5000}};   // 3 retired: reads as 4
    for (const Row& r : rows) {
        const auto f = ctcable::facts(emrec::builtin_cable(r.id));
        CHECK_EQ(f.vid, r.vid); CHECK_EQ(f.pid, r.pid); CHECK_EQ(f.max_mv, r.mv); CHECK_EQ(f.max_ma, r.ma);
    }
}

static void test_facts_read_the_vdos() {
    const std::uint32_t vdos[] = {0xFF008041u, 0x18000000u | 0x2B01u, 0x00000000u, 0x1234'0000u, 0x0000'0000u | (1u << 5) | (1u << 9)};
    const auto f = ctcable::facts(emrec::make(vdos, 2, "x"));
    CHECK_EQ(f.vid, 0x2B01);
    CHECK_EQ(f.pid, 0x1234);
    CHECK_EQ(f.max_mv, 30000);                                    // voltage code 1
    CHECK_EQ(f.max_ma, 3000);                                     // current code 1
}

int main() {
    test_valid_choices();
    test_facts_of_the_builtin_cable();
    test_canonical_builtin_ids();
    test_facts_of_every_dialog_builtin();
    test_facts_read_the_vdos();
    return test_report("test_ct_cable");
}
