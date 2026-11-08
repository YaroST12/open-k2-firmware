// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <string>

#include "k2/core/logring.hpp"
#include "test_util.h"

using namespace k2;

static std::string snap(const LogRing<16>& r) {
    char b[64];
    return std::string(b, r.snapshot(b));
}

static void test_view() {
    LogRing<16> ring;
    for (const auto text : {"", "ab\n", "cd\n", "0123456789\n", "xyz\n", "0123456789ABCDEFGHIJ"}) {
        ring.push(text);
        for (std::size_t capacity = 0; capacity <= 20; capacity++) {
            const auto parts = ring.view(capacity);
            char copy[20];
            const auto n = ring.snapshot({copy, capacity});
            CHECK_EQ(std::string(parts[0]) + std::string(parts[1]) == std::string(copy, n), true);
            CHECK_EQ(parts[0].size() + parts[1].size() <= capacity, true);
        }
    }
}

int main() {
    test_view();
    LogRing<16> r;
    CHECK_EQ(snap(r).size(), 0u);
    r.push("ab\n");
    r.push("cd\n");
    CHECK_EQ(snap(r) == "ab\ncd\n", 1);
    // wrap: the oldest bytes fall out; the snapshot starts at a line boundary
    r.push("0123456789\n");                        // total 3+3+11 = 17 > 16, 'a' dropped
    CHECK_EQ(snap(r) == "cd\n0123456789\n", 1);
    r.push("xyz\n");
    CHECK_EQ(snap(r) == "0123456789\nxyz\n", 1);
    // a line longer than the ring keeps only its tail, no newline in range: the tail is shown as is
    LogRing<16> s;
    s.push("0123456789ABCDEFGHIJ");
    CHECK_EQ(snap(s) == "456789ABCDEFGHIJ", 1);
    // snapshot into a smaller buffer keeps the newest bytes (from a line boundary when possible)
    LogRing<16> t;
    t.push("aa\nbb\ncc\n");
    char small[6];
    const std::size_t n = t.snapshot(small);
    CHECK_EQ(std::string(small, n) == "cc\n", 1);
    CHECK_EQ(t.size(), 9u);
    return test_report("test_logring");
}
