// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The RX edge stamp: the FUSB302 INT_N interrupt stores one microsecond timestamp (edge()), the main loop's service() takes it when it
// finds a frame in the RX FIFO (take()). That is the frame's arrival time to within the ISR latency, instead of "when the 1 ms poll got
// round to reading the FIFO" (up to a millisecond late, more when the loop was busy). Used for the Black Box ring timestamps and the PD
// Stress latencies; NEVER for a protocol decision (the sink's timers keep running on millis()).
//
// Latest edge wins: a Request's own TXSENT edge sits in front of the Accept's, and the Accept's edge must replace it. A stamp older than
// kMaxAgeUs is not this frame's (the loop ran late, or the edge was for something that left no frame, e.g. a GoodCRC the chip consumed).
// The 32-bit microsecond counter wraps every ~71 min; the age is a difference, so the wrap is harmless.
#pragma once

#include <cstdint>

namespace k2::pd {

class RxStamp {
public:
    static constexpr std::uint32_t kMaxAgeUs = 20000;

    // ISR side: one store pair, nothing else.
    void edge(std::uint32_t us) { us_ = us; pending_ = true; }

    // Main loop: the latest edge when it is fresh enough; consumed either way.
    bool take(std::uint32_t now_us, std::uint32_t& out) {
        if (!pending_) return false;
        pending_ = false;
        const std::uint32_t us = us_;
        if (now_us - us > kMaxAgeUs) return false;
        out = us;
        return true;
    }
    void clear() { pending_ = false; }

private:
    volatile std::uint32_t us_ = 0;
    volatile bool pending_ = false;
};

}  // namespace k2::pd
