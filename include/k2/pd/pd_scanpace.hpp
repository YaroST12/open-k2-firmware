// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
// When the unattached CC scan (fusb302.cpp detect_cc_orientation) runs. Pure, host-tested in tests/test_scan_pace.cpp.
//
// The scan used to run on every 25th 20 ms poll (~2 Hz) and nothing else. A source starts its offers ~100-300 ms after it
// sees a sink's Rd (tTypeCSendSourceCap after VBUS), so a scan up to 500 ms after the Rp appeared routed the receiver
// after the Source_Caps, the Request and the Accept had gone by: the first decoded frame was a PS_RDY, the K2 had no caps
// and the Link fell back to "USB DATA" (hardware 2026-11-11, NOVOO -> K2 -> Legion side port, three times;
// runs/2026-11-11-integ-hw/twin/h-side-inline: detach 2539057, VBUS 1.88 V at 2539078, 20 V at 2539388, scan 2539410).
//
// Now a scan is due on the period as before, or soon after a kick:
//   - a no-Rp detach (the partner may already be back, or the line was the wrong one: the live net is scanned at once);
//     every detach first restarts the period (no scan ran while attached), so a VBUS-low or dark detach, whose Rp is
//     still there, gets a steady kScanPeriodMs before the same line is latched again;
//   - VBUS rising through kScanVbusRiseMv (a source turned its output on: its offers follow within ~tTypeCSendSourceCap);
//   - the ADC naming a new line in the Rd window (a Rp over a sink's Rd appeared).
// A kick waits kScanKickMinMs after the last scan, so a level flapping at the window edge costs at most one ~5 ms scan
// per kScanKickMinMs, never one per poll. A kick only moves the scan earlier: it never changes what the scan chooses, and
// the scan's own gates (an Rd hold, the idle wake's reading window) still apply in the caller. Only edges while nothing is
// attached count (attached() drops the rest); of the detaches only the no-Rp one kicks (the caller decides).
#include <cstdint>

namespace k2::pd {

inline constexpr std::uint32_t kScanPeriodMs = 500;     // the plain cadence (was poll_count % 25 at the 20 ms poll)
inline constexpr std::uint32_t kScanKickMinMs = 60;     // a kicked scan, at the soonest, this long after the last one
inline constexpr std::int32_t kScanVbusRiseMv = 3500;   // VBUS crossing up through this kicks (vSafe5V min is 4.75 V)

class ScanPace {
public:
    void kick() { kicked_ = true; }
    // Fresh levels, unattached: VBUS crossing up, or the Rd-window line changing to a line, kicks.
    void feed_vbus(std::int32_t mv) {
        const bool up = mv >= kScanVbusRiseMv;
        if (up && vbus_known_ && !vbus_up_) kicked_ = true;
        vbus_up_ = up;
        vbus_known_ = mv >= 0;
    }
    void feed_window_line(std::uint8_t line) {
        if (line != 0 && line != line_) kicked_ = true;
        line_ = line;
    }
    bool due(std::uint32_t now) const {
        if (!scanned_) return true;
        const std::uint32_t since = now - last_;
        return since >= kScanPeriodMs || (kicked_ && since >= kScanKickMinMs);
    }
    void scanned(std::uint32_t now) {
        scanned_ = true;
        last_ = now;
        kicked_ = false;
    }
    // Called on every poll with a line attached: an edge seen while attached is no reason to rescan after the next detach
    // (a VBUS rise during the contract would otherwise fire the moment a VBUS-low detach takes the line).
    void attached() { kicked_ = false; }
    bool kicked() const { return kicked_; }

private:
    std::uint32_t last_ = 0;
    bool scanned_ = false;
    bool kicked_ = false;
    bool vbus_up_ = false;
    bool vbus_known_ = false;
    std::uint8_t line_ = 0;
};

}  // namespace k2::pd
