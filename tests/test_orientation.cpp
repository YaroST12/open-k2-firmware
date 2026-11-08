// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "test_util.h"
#include "k2/ui/orientation.hpp"

using k2::orientation::Gravity;
using k2::orientation::Tracker;

static void test_four_way() {
    Tracker t;
    const Gravity g[] = {{0, 1000, 0}, {-1000, 0, 0}, {0, -1000, 0}, {1000, 0, 0}};
    for (unsigned r = 0; r < 4; ++r) {
        const uint32_t start = r * 1000;
        CHECK_EQ(t.update(g[r], start).has_value(), false);
        for (uint32_t ms = 50; ms < 300; ms += 50)
            CHECK_EQ(t.update(g[r], start + ms).has_value(), false);
        const auto change = t.update(g[r], start + 300);
        CHECK_EQ(change.has_value(), r != 0);
        CHECK_EQ(t.rotation(), r);
    }
}

static void test_rejection_and_debounce() {
    Tracker t;
    CHECK_EQ(t.update(Gravity{1000, 0, 0}, 0).has_value(), false);
    CHECK_EQ(t.update(std::nullopt, 50).has_value(), false);
    CHECK_EQ(t.update(Gravity{1000, 0, 0}, 100).has_value(), false);
    CHECK_EQ(t.update(Gravity{1000, 0, 0}, 400).has_value(), false); // stale samples do not debounce
    CHECK_EQ(t.rotation(), 0);
    for (Gravity g : {Gravity{0, 0, 1000}, Gravity{700, 700, 0}, Gravity{0, 0, 0},
                     Gravity{2000, 0, 0}, Gravity{-32768, 0, 0}, Gravity{900, 0, 700},
                     Gravity{650, 0, 0}, Gravity{1200, 700, 0}, Gravity{-32768, -32768, -32768}}) {
        CHECK_EQ(k2::orientation::classify(g).has_value(), false);
    }
    for (uint32_t ms = 450; ms <= 700; ms += 50) t.update(Gravity{1000, 0, 0}, ms);
    CHECK_EQ(t.rotation(), 3);
    const uint32_t wrap_start = 0xFFFFFF80u;
    t.update(Gravity{0, -1000, 0}, wrap_start);
    for (uint32_t elapsed = 50; elapsed <= 300; elapsed += 50)
        t.update(Gravity{0, -1000, 0}, wrap_start + elapsed);
    CHECK_EQ(t.rotation(), 2);
}

int main() {
    test_four_way();
    test_rejection_and_debounce();
    return test_report("test_orientation");
}
