// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Where the K2's own supply comes from. A K2 with no USB host that has VBUS on its measured port at boot runs on that port
// ("bus-powered", e.g. from a charger's USB-A port through an A-to-C cable). A charger-test run treats it exactly like a
// charger-powered K2 with a pinned Rd (fusb302::self_powered()): no detach, no OK press on the resume. After the boot window the
// answer follows the host: a host that enumerates means host-powered, a host gone for kHostGoneMs with VBUS present means
// bus-powered (hardware 2026-11-06: a K2 booted on HID, then unplugged and started on a USB-A port, kept "host" from the boot and
// closed its run on the first power cut). A VBUS dip alone never flips it; a run latches the answer at START and at each resume.
#pragma once
#include <cstdint>
#include "k2/core/bootmode.hpp"

namespace k2::power_source {

inline constexpr std::uint32_t kDecideMs = 2000;   // a host enumerates well inside this; after it, no host = the port feeds the K2
inline constexpr std::uint32_t kHostGoneMs = 1000; // a host that stopped answering this long is unplugged (a bus reset is far shorter)

// Pure decision (host-tested).
class Latch {
public:
    void update(std::uint32_t uptime_ms, std::int32_t vbus_mv, bool usb_configured) {
        if (usb_configured) { decided_ = true; bus_ = false; host_seen_ms_ = uptime_ms; have_host_ = true; return; }
        if (!decided_) {
            if (uptime_ms < kDecideMs) return;
            decided_ = true; bus_ = vbus_mv >= bootmode::kVbusPresentMv;
            return;
        }
        // Decided and no host now: the host left (HID unplugged) once it has been gone long enough and VBUS feeds the K2.
        if (!bus_ && have_host_ && uptime_ms - host_seen_ms_ >= kHostGoneMs && vbus_mv >= bootmode::kVbusPresentMv) bus_ = true;
    }
    bool decided() const { return decided_; }
    bool bus_powered() const { return bus_; }
private:
    bool decided_ = false, bus_ = false, have_host_ = false;
    std::uint32_t host_seen_ms_ = 0;
};

void poll(std::uint32_t now_ms);   // main loop: feeds the latch with the USB state and the fast VBUS reading
bool decided();                    // false until the host window is over; a resume waits for it
bool bus_powered();

}  // namespace k2::power_source
