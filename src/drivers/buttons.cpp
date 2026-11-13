// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/buttons.hpp"

#include "k2/core/hot.h"

namespace k2 {

K2_HOT ButtonEvent ButtonDetector::update(bool down, uint32_t now) {
    if (down != raw_) {
        raw_ = down;
        raw_since_ = now;
    }
    if (raw_ != stable_ && now - raw_since_ >= kDebounceMs) {
        stable_ = raw_;
        if (stable_) {
            press_t_ = raw_since_;
            long_fired_ = false;
            hold_fired_ = false;
        } else if (!long_fired_) {
            return ButtonEvent::Short;
        }
    }
    // raw_ too: a release inside the debounce window must not complete a hold.
    if (stable_ && raw_ && long_fired_ && !hold_fired_ && now - press_t_ >= kHoldMs) {
        hold_fired_ = true;
        return ButtonEvent::Hold;
    }
    if (stable_ && !long_fired_ && now - press_t_ >= kLongMs) {
        long_fired_ = true;
        return ButtonEvent::Long;
    }
    return ButtonEvent::None;
}

}  // namespace k2
