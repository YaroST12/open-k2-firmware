// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A refused 6 A hunt row freezes a Black Box capture of its own kind (Trigger::Refusal) and leaves a mark in it.
#include <cstdint>

#include "k2/pd/pdtrace.hpp"
#include "test_util.h"

using namespace k2::pdtrace;

namespace {
// The slot holding a Refusal capture, or -1.
int refusal_slot() {
    for (std::size_t k = 0; k < kCapSlots; ++k)
        if (capture_info(k).trigger == Trigger::Refusal) return static_cast<int>(k);
    return -1;
}
// True when the slot's records hold a kMarkRefusal mark with these values.
bool has_mark(std::size_t slot, std::uint16_t a, std::uint16_t b) {
    const auto d = capture_data(slot);
    for (std::size_t i = 0; i + kHeader < d.size() && d[i] >= kHeader; i += d[i]) {
        if (d[i + 1] != std::uint8_t(Type::Mark) || d[i] < kHeader + 5 || d[i + kHeader] != kMarkRefusal) continue;
        const std::uint8_t* p = &d[i + kHeader + 1];
        if ((p[0] | p[1] << 8) == a && (p[2] | p[3] << 8) == b) return true;
    }
    return false;
}
}  // namespace

int main() {
    {   // the trigger is a kind of its own (its cooldown does not swallow the other kinds)
        Ring<600> r;
        Captures<3, 100> c;
        CHECK_EQ(c.start(Trigger::Refusal, 1000, r, 0), 1);
        CHECK_EQ(c.start(Trigger::Refusal, 1500, r, 0), 0);   // same kind inside the cooldown
        CHECK_EQ(c.start(Trigger::Edges, 1500, r, 0), 1);     // Edges (5) and Refusal (6) keep separate cooldowns
        CHECK_EQ(c.start(Trigger::Refusal, 2100, r, 0), 1);
    }
    {   // the public call: a mark in the ring and a frozen capture that holds it
        init();
        CHECK_EQ(refusal_slot(), -1);
        refusal(std::uint16_t(9 | 2 << 8), 5000);
        const int s = refusal_slot();
        CHECK_EQ(s >= 0, 1);
        if (s >= 0) {
            CHECK_EQ(capture_info(static_cast<std::size_t>(s)).filling, 1);
            CHECK_EQ(has_mark(static_cast<std::size_t>(s), std::uint16_t(9 | 2 << 8), 5000), 1);
        }
    }
    return test_report("pdtrace_refusal");
}
