// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Crossed CC through the K2 (pure rules, host-tested in tests/test_cc_crossed.cpp). RED STUB: the verdict is always None.
#pragma once
#include <cstddef>
#include <cstdint>

namespace k2::raguard {

enum class CrossKind : std::uint8_t { None = 0, Hint, Crossed };

class CrossTrace {
public:
    void clear() {}
    void add(std::uint32_t, std::int32_t, std::int32_t) {}
    CrossKind classify(std::uint32_t) const { return CrossKind::None; }
};

}  // namespace k2::raguard
