// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Debounce and short/long press detection. Pure: pin level and time in, event out.
#pragma once
#include <cstdint>

namespace k2 {

enum class ButtonEvent : uint8_t { None, Short, Long, Hold };

class ButtonDetector {
public:
    static constexpr uint32_t kDebounceMs = 20;
    static constexpr uint32_t kLongMs = 500;
    static constexpr uint32_t kHoldMs = 5000;   // the reboot gesture: far from any accidental press

    // `down` = button currently pressed (already active-low corrected). Call at least every few ms.
    // Short fires on the debounced release; Long fires once when the hold reaches kLongMs,
    // counted from the first edge, and suppresses the Short on release. Hold fires once more when
    // the same press reaches kHoldMs (Long has fired before it).
    ButtonEvent update(bool down, uint32_t now_ms);
    bool pressed() const { return stable_; }   // debounced state, for highlighting

private:
    bool raw_ = false;
    bool stable_ = false;
    bool long_fired_ = false;
    bool hold_fired_ = false;
    uint32_t raw_since_ = 0;
    uint32_t press_t_ = 0;
};

}  // namespace k2
