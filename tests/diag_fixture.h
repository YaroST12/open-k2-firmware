// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A Black Box diagnosis report from a saved capture (tests/fixtures/bbdiag/*.raw), through the real analyser, for the
// UI tests and mockups that show it (g_stub.diag_report). Case 1 is the Legion rear port on a 60 W hub.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

#include "k2/pd/diag.hpp"

namespace diag_fixture {

inline std::vector<std::uint8_t> load(const char* path) {
    std::vector<std::uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::uint8_t buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    return v;
}

inline constexpr std::uint32_t kLegionPdos[] = {0x3601912C, 0x9902D145};   // fixed 5 V 3 A, variable 9-20 V 3.25 A (65 W)

// The report of the session the capture shows; empty when the file is missing.
inline k2::pd::diag::Report report(const char* path, const k2::pd::diag::TwinHint* twin = nullptr) {
    static k2::pd::diag::Analyzer a;
    a.reset();
    const auto raw = load(path);
    if (raw.empty()) return {};
    a.feed(raw);
    return k2::pd::diag::evaluate(a.shown(), twin, a.now_ms());
}

// Case 1: the Legion's rear port behind a 60 W hub (the twin assumed).
inline k2::pd::diag::Report case1() {
    const k2::pd::diag::TwinHint twin{"Legion rear port", kLegionPdos, true, true};
    return report("tests/fixtures/bbdiag/hub_rear_60w.raw", &twin);
}

}  // namespace diag_fixture
