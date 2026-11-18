// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::eekeep pure part: the spoof slot's payload and which choice the slot should hold.
#include "k2/core/eekeep.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::eekeep;
using ctcable::Choice;
using ctcable::Kind;

static void test_payload_round_trip() {
    const Choice saved{Kind::Saved, 0x1234};
    const auto p = spoof_payload(saved);
    CHECK_EQ(int(p[0]), 2); CHECK_EQ(int(p[1]), 0x34); CHECK_EQ(int(p[2]), 0x12);
    const auto back = spoof_choice(p);
    CHECK_COND(back.has_value()); CHECK_COND(*back == saved);
    const Choice b{Kind::Builtin, std::uint16_t(ctcable::kBuiltinKind)};
    CHECK_COND(*spoof_choice(spoof_payload(b)) == b);
}

static void test_payload_refusals() {
    const std::uint8_t unknown_kind[3] = {3, 1, 0};
    CHECK_COND(!spoof_choice(unknown_kind).has_value());
    const std::uint8_t saved_zero[3] = {2, 0, 0};          // a Saved choice needs a library id
    CHECK_COND(!spoof_choice(saved_zero).has_value());
    const std::uint8_t builtin_bad[3] = {1, 0xEE, 0};      // not a known built-in id
    CHECK_COND(!spoof_choice(builtin_bad).has_value());
    const std::uint8_t two[2] = {2, 1};
    CHECK_COND(!spoof_choice(two).has_value());
    CHECK_COND(!spoof_choice({}).has_value());             // the cleared slot (empty payload) holds no choice
}

static void test_wanted() {
    const Choice u{Kind::Saved, 7};
    CHECK_COND(spoof_wanted(true, 5, 5, u) == u);           // the user's arm still answers
    CHECK_COND(spoof_wanted(false, 5, 5, u) == Choice{});   // disarmed (by the user or a detach of the identity)
    CHECK_COND(spoof_wanted(true, 6, 5, u) == Choice{});    // someone else armed since (a charger test run, the twin)
    CHECK_COND(spoof_wanted(false, 6, 5, u) == Choice{});
}

int main() {
    test_payload_round_trip();
    test_payload_refusals();
    test_wanted();
    return test_report("test_eekeep");
}
