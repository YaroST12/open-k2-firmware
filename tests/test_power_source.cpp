// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Where the K2 gets its own power from (a charger-test run treats a bus-powered K2 like a self-powered one).
#include "k2/pd/power_source.hpp"
#include "test_util.h"

using k2::power_source::Latch;
using k2::power_source::kDecideMs;

int main() {
    {   // A-port style: no host, VBUS on the measured port: bus-powered once the host window is over, not before.
        Latch l;
        l.update(0, 5000, false); l.update(kDecideMs - 1, 5000, false);
        CHECK_EQ(l.decided(), false); CHECK_EQ(l.bus_powered(), false);
        l.update(kDecideMs, 5000, false);
        CHECK_EQ(l.decided(), true); CHECK_EQ(l.bus_powered(), true);
        l.update(kDecideMs + 10, 0, false);            // a VBUS dip does not undo it ...
        CHECK_EQ(l.bus_powered(), true);
        l.update(kDecideMs + 20, 5000, true);          // ... a host that enumerates does: HID now powers the K2
        CHECK_EQ(l.bus_powered(), false);
    }
    {   // A host that enumerates inside the window: HID-powered, decided at once.
        Latch l;
        l.update(300, 5000, true);
        CHECK_EQ(l.decided(), true); CHECK_EQ(l.bus_powered(), false);
        l.update(kDecideMs + 100, 5000, true);
        CHECK_EQ(l.bus_powered(), false);
    }
    {   // Booted on HID, then HID unplugged with a USB-A port on the measured side (hardware 2026-11-06): bus-powered once the
        // host has been gone kHostGoneMs; a short host gap (a bus reset) does not flip it.
        using k2::power_source::kHostGoneMs;
        Latch l;
        l.update(300, 5000, true);
        l.update(5000, 5000, true);
        l.update(5000 + kHostGoneMs - 1, 5000, false);
        CHECK_EQ(l.bus_powered(), false);
        l.update(5000 + kHostGoneMs, 5000, false);
        CHECK_EQ(l.bus_powered(), true);
        Latch g;                                        // host gone but no VBUS: nothing feeds the K2 from the port
        g.update(300, 5000, true);
        g.update(300 + kHostGoneMs + 10, 100, false);
        CHECK_EQ(g.bus_powered(), false);
    }
    {   // No VBUS (or no monitor reading) at boot: not bus-powered.
        Latch l;
        l.update(kDecideMs, 0, false);
        CHECK_EQ(l.decided(), true); CHECK_EQ(l.bus_powered(), false);
        l.update(kDecideMs + 500, 5000, false);
        CHECK_EQ(l.bus_powered(), false);
        Latch m;
        m.update(kDecideMs, -1, false);
        CHECK_EQ(m.bus_powered(), false);
    }
    return test_report("power_source");
}
