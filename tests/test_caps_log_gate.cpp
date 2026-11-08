// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Source_Capabilities log gate (k2/pd/caps_log_gate.hpp): a source that repeats its caps every few hundred ms floods the
// HID log ring; the first caps of a source, a change and one line per 10 s get through, with the repeat count.
#include "k2/pd/caps_log_gate.hpp"
#include "test_util.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2::pd;

int main() {
    // First caps of a source are logged.
    {
        CapsLogGate g;
        const auto d = g.on_caps(1, 0xAAAA, 1000);
        CHECK(d.log);
        CHECK_EQ(d.repeats, 0u);
    }
    // Identical repeats inside the interval are held back and counted; one line per interval carries the count.
    {
        CapsLogGate g;
        CHECK(g.on_caps(1, 0xAAAA, 1000).log);
        for (unsigned i = 1; i <= 9; i++) CHECK(!g.on_caps(1, 0xAAAA, 1000 + i * 1000).log);     // t = 2..10 s after the first
        const auto d = g.on_caps(1, 0xAAAA, 11000);                                                // exactly 10 s later
        CHECK(d.log);
        CHECK_EQ(d.repeats, 9u);
        CHECK(!g.on_caps(1, 0xAAAA, 11500).log);                                                   // the interval restarts
        const auto e = g.on_caps(1, 0xAAAA, 21000);
        CHECK(e.log);
        CHECK_EQ(e.repeats, 1u);
    }
    // A change of content logs at once and reports the repeats held back before it.
    {
        CapsLogGate g;
        CHECK(g.on_caps(1, 0xAAAA, 0).log);
        CHECK(!g.on_caps(1, 0xAAAA, 300).log);
        CHECK(!g.on_caps(1, 0xAAAA, 600).log);
        const auto d = g.on_caps(1, 0xBBBB, 900);
        CHECK(d.log);
        CHECK_EQ(d.repeats, 2u);
        CHECK(!g.on_caps(1, 0xBBBB, 1200).log);
    }
    // A new attach session is a new source: logged even with identical content, nothing carried over.
    {
        CapsLogGate g;
        CHECK(g.on_caps(1, 0xAAAA, 0).log);
        CHECK(!g.on_caps(1, 0xAAAA, 300).log);
        const auto d = g.on_caps(2, 0xAAAA, 600);
        CHECK(d.log);
        CHECK_EQ(d.repeats, 0u);
    }
    // The millisecond clock wrapping around does not freeze the gate (unsigned difference).
    {
        CapsLogGate g;
        CHECK(g.on_caps(1, 7, 0xFFFFF000u).log);
        CHECK(!g.on_caps(1, 7, 0xFFFFF800u).log);
        CHECK(g.on_caps(1, 7, 0xFFFFF000u + 10000u).log);
    }
    // The content hash separates caps that differ in one PDO, in the count or in the revision, and ignores nothing.
    {
        const std::uint32_t a[] = {0x0001912C, 0x0002D12C};
        const std::uint32_t b[] = {0x0001912C, 0x0002D12D};
        CHECK(caps_content_hash(a, 2, 3, 1) == caps_content_hash(a, 2, 3, 1));
        CHECK(caps_content_hash(a, 2, 3, 1) != caps_content_hash(b, 2, 3, 1));
        CHECK(caps_content_hash(a, 2, 3, 1) != caps_content_hash(a, 1, 3, 1));
        CHECK(caps_content_hash(a, 2, 3, 1) != caps_content_hash(a, 2, 2, 1));
        CHECK(caps_content_hash(a, 2, 3, 1) != caps_content_hash(a, 2, 3, 2));
    }
    return test_report("test_caps_log_gate");
}
