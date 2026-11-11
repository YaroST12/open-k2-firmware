// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID-cable loop guard: the detector (aux rail, PD-bus VBUS, USB host) and the 5 V enforcement sequence.
#include "k2/core/loop_guard.hpp"
#include "test_util.h"

using namespace k2::loopguard;

namespace {
// Feed the same sample every 20 ms (the main-loop slot) from `from` up to and including `to`.
void hold(Detector& d, std::uint32_t from, std::uint32_t to, Sample s) {
    for (std::uint32_t t = from; t <= to; t += 20) d.update(t, s);
}
}  // namespace

int main() {
    // ---- The "rail follows VBUS" rule: a loop's HID rail is fed from VBUS, so the two rise (and fall) together ----
    {   // R1. A PC on HID first (USB link off: never an active host), then a charger on the PD bus: never a lock.
        Detector d;
        hold(d, 0, 5000, {5000, 0, false});
        hold(d, 5020, 60000, {5000, 5000, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.events(), 0);
    }
    {   // R2. A charger first (the K2 runs from the PD bus), then a PC (or a 5 V brick) on HID: never a lock.
        Detector d;
        hold(d, 0, 5000, {300, 5000, false});
        hold(d, 5020, 60000, {5000, 5000, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.events(), 0);
    }
    {   // R3. A loop: the rail and VBUS rise together (same sample, or the rail one sample after VBUS): locked at once.
        Detector d;
        hold(d, 0, 1000, {0, 0, false});
        CHECK_EQ(d.update(1020, {5000, 5000, false}), true);
        CHECK_EQ(d.locked(), true);
        CHECK_EQ(int(d.reason()), int(Reason::Suspected));
        CHECK_EQ(d.events(), 1);
        Detector e;
        hold(e, 0, 1000, {0, 0, false});
        e.update(1020, {300, 5000, false});                    // VBUS first
        CHECK_EQ(e.update(1040, {5000, 5000, false}), true);   // the rail one sample later
        CHECK_EQ(e.locked(), true);
    }
    {   // R4. A loop present at boot: the first samples already show both up, no order known: locked.
        Detector d;
        CHECK_EQ(d.update(0, {5000, 5000, false}), true);
        CHECK_EQ(d.locked(), true);
        CHECK_EQ(int(d.reason()), int(Reason::Suspected));
        Detector e;   // the readings come up one by one at boot (no ADC snapshot, no monitor yet): still no order known
        hold(e, 0, 100, {-1, -1, false});
        hold(e, 120, 400, {5000, -1, false});
        CHECK_EQ(e.locked(), false);
        e.update(420, {5000, 5000, false});
        CHECK_EQ(e.locked(), true);
    }
    {   // R5. An auto-off charger (a USB-A port that cuts VBUS every ~6.5 s with no load) with the HID rail up: never locks.
        Detector d;
        hold(d, 0, 2000, {5000, 0, false});
        for (std::uint32_t t = 2020; t < 120000; t += 6500) {
            hold(d, t, t + 6000, {5000, 5000, false});
            hold(d, t + 6020, t + 6480, {5000, 0, false});
        }
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.events(), 0);
    }
    {   // R6. Locked, then VBUS cut while the rail stays up: a separate supply, cannot be a loop: clears.
        Detector d;
        d.update(0, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
        hold(d, 20, 200, {5000, 300, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.events(), 1);
        hold(d, 220, 30000, {5000, 5000, false});   // VBUS back: the rail was up first, no new lock
        CHECK_EQ(d.locked(), false);
    }
    {   // R7. Boot with both up, then an active host for kClearMs: clears.
        Detector d;
        d.update(0, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
        hold(d, 20, 1500, {5000, 5000, false});      // enumeration
        hold(d, 1520, 1520 + kClearMs, {5000, 5000, true});
        CHECK_EQ(d.locked(), false);
    }

    {   // Certain loop: the aux rail above anything a USB 2.0 host gives locks at once, whatever the host state says.
        Detector d;
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.update(0, {kCertainMv + 1, 9000, true}), true);   // the edge is reported
        CHECK_EQ(d.locked(), true);
        CHECK_EQ(int(d.reason()), int(Reason::Certain));
        CHECK_EQ(d.events(), 1);
        CHECK_EQ(d.update(20, {kCertainMv + 1, 9000, true}), false);  // no second edge
        CHECK_EQ(d.events(), 1);
    }
    {   // A PC on the HID port at its 5.25 V ceiling with a charger on the PD bus: never a lock.
        Detector d;
        hold(d, 0, 60000, {5250, 20000, true});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.events(), 0);
    }
    {   // Nothing on the HID port (aux dead, the K2 runs from the PD bus): never a lock.
        Detector d;
        hold(d, 0, 60000, {300, 20000, false});
        CHECK_EQ(d.locked(), false);
    }
    {   // HID-powered with nothing on the PD bus: no VBUS, no loop.
        Detector d;
        hold(d, 0, 60000, {5000, 0, false});
        CHECK_EQ(d.locked(), false);
    }
    {   // The rise window: the rail kRiseWindowMs after VBUS still locks (rise_gap_ms says how far apart), one sample later not.
        Detector d;
        d.update(0, {0, 0, false});
        d.update(20, {0, 5000, false});
        hold(d, 40, 20 + kRiseWindowMs - 20, {3000, 5000, false});
        CHECK_EQ(d.update(20 + kRiseWindowMs, {5000, 5000, false}), true);
        CHECK_EQ(d.rise_gap_ms(), std::int32_t(kRiseWindowMs));
        Detector e;
        e.update(0, {0, 0, false});
        e.update(20, {0, 5000, false});
        hold(e, 40, 20 + kRiseWindowMs, {3000, 5000, false});
        CHECK_EQ(e.update(40 + kRiseWindowMs, {5000, 5000, false}), false);
        CHECK_EQ(e.rise_gap_ms(), std::int32_t(kRiseWindowMs + 20));
        Detector f;   // the rail first, VBUS within the window (sample order of two readings): the same loop
        f.update(0, {0, 0, false});
        f.update(20, {5000, 300, false});
        CHECK_EQ(f.update(40, {5000, 5000, false}), true);
        Detector g;   // the boot rule reports no order known
        g.update(0, {5000, 5000, false});
        CHECK_EQ(g.rise_gap_ms(), -1);
    }
    {   // An active host at the moment both rise blocks the suspicion (a PC that already configured the K2).
        Detector d;
        CHECK_EQ(d.update(0, {5000, 5000, true}), false);
        hold(d, 20, 10000, {5000, 5000, false});   // the host suspends later: no new rise, no lock
        CHECK_EQ(d.locked(), false);
    }
    {   // Rail hysteresis: VBUS dipping into the band (3.5..4 V) is no fall, so its return is no rise.
        Detector d;
        hold(d, 0, 1000, {5000, 0, false});
        hold(d, 1020, 2000, {5000, 5000, false});
        d.update(2020, {300, 3700, false});          // the rail drops; VBUS sags into the band
        CHECK_EQ(d.update(2040, {5000, 5000, false}), false);   // the rail is back alone: VBUS never fell
        CHECK_EQ(d.locked(), false);
    }
    {   // Aux in the hysteresis band (between gone and present) is not up: no lock with VBUS.
        Detector d;
        hold(d, 0, 10000, {kAuxPresentMv - 100, 5000, false});
        CHECK_EQ(d.locked(), false);
    }
    {   // A VBUS cut shorter than kSeparateMs keeps the lock; the clear names its cause.
        Detector d;
        d.update(0, {5000, 5000, false});
        hold(d, 20, 20 + kSeparateMs - 20, {5000, 300, false});
        d.update(20 + kSeparateMs, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
        CHECK_EQ(int(d.last_clear()), int(Clear::None));
        hold(d, 2000, 2000 + kSeparateMs, {5000, 300, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(int(d.last_clear()), int(Clear::Separate));
    }
    {   // VBUS between 1 V and the gone level with the rail up proves nothing: no separate-supply clear.
        Detector d;
        d.update(0, {5000, 5000, false});
        hold(d, 20, 5000, {5000, 2000, false});
        CHECK_EQ(d.locked(), true);
    }
    {   // Unknown readings never clear a lock (no ADC snapshot while another driver owns the pins).
        Detector d;
        d.update(0, {5000, 5000, false});
        hold(d, 20, 10000, {-1, 300, true});
        hold(d, 10020, 20000, {5000, -1, false});   // the rail up, VBUS unknown: no separate-supply proof
        CHECK_EQ(d.locked(), true);
    }
    {   // A loop cable plugged into HID while the K2 already sinks VBUS (Rd presented): the rail rises long after VBUS, as for a
        // PC. Not suspected (documented gap); the certain rule takes the first raise above 5.8 V.
        Detector d;
        hold(d, 0, 5000, {0, 5000, false});
        hold(d, 5020, 10000, {5000, 5000, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.update(10020, {9000, 9000, false}), true);
        CHECK_EQ(int(d.reason()), int(Reason::Certain));
    }
    {   // Clear: aux below kAuxGoneMv for kClearMs (the loop cable pulled). A shorter dip keeps the lock.
        Detector d;
        d.update(0, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
        const std::uint32_t t0 = 20;
        hold(d, t0, t0 + kClearMs - 40, {kAuxGoneMv - 100, 5000, false});
        d.update(t0 + kClearMs - 20, {kAuxPresentMv, 5000, false});   // back up before the clear time: still locked
        CHECK_EQ(d.locked(), true);
        const std::uint32_t t1 = t0 + kClearMs;
        hold(d, t1, t1 + kClearMs - 20, {kAuxGoneMv - 100, 5000, false});
        CHECK_EQ(d.locked(), true);
        CHECK_EQ(d.update(t1 + kClearMs, {kAuxGoneMv - 100, 5000, false}), true);   // the clear edge
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(int(d.reason()), int(Reason::None));
        CHECK_EQ(d.events(), 1);   // events count locks, not clears
    }
    {   // Clear: a host that configures and stays active for kClearMs (a dumb brick swapped for a PC).
        Detector d;
        hold(d, 0, 3000, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
        const std::uint32_t t0 = 3020;
        hold(d, t0, t0 + kClearMs - 20, {5000, 5000, true});
        CHECK_EQ(d.locked(), true);
        d.update(t0 + kClearMs, {5000, 5000, true});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(int(d.last_clear()), int(Clear::Host));
    }
    {   // A certain lock is not cleared by a "host" while aux is still above what a host gives (kHostMaxMv).
        Detector d;
        d.update(0, {9000, 9000, false});
        hold(d, 20, 20000, {9000, 9000, true});
        CHECK_EQ(d.locked(), true);
        hold(d, 20020, 20020 + kClearMs, {kHostMaxMv + 100, 5000, true});
        CHECK_EQ(d.locked(), true);
        hold(d, 30000, 30000 + kClearMs, {5000, 5000, true});   // back at a host's level with a host: clears
        CHECK_EQ(d.locked(), false);
    }
    {   // A suspected lock that then sees aux above kCertainMv becomes certain; no new event.
        Detector d;
        hold(d, 0, 3000, {5000, 5000, false});
        CHECK_EQ(int(d.reason()), int(Reason::Suspected));
        d.update(3020, {9000, 9000, false});
        CHECK_EQ(int(d.reason()), int(Reason::Certain));
        CHECK_EQ(d.events(), 1);
    }
    {   // Unknown readings (no ADC snapshot, no monitor = negative) count as absent: no lock from missing data.
        Detector d;
        hold(d, 0, 20000, {-1, -1, false});
        CHECK_EQ(d.locked(), false);
        hold(d, 20020, 40000, {5000, -1, false});
        CHECK_EQ(d.locked(), false);
    }
    {   // A second lock after a clear counts a second event.
        Detector d;
        d.update(0, {9000, 9000, false});
        hold(d, 20, 20 + kClearMs, {0, 0, false});
        CHECK_EQ(d.locked(), false);
        d.update(5000, {9000, 9000, false});
        CHECK_EQ(d.events(), 2);
    }
    CHECK_STR(reason_name(Reason::None), "none");
    CHECK_STR(reason_name(Reason::Certain), "certain");
    CHECK_STR(reason_name(Reason::Suspected), "suspected");
    CHECK_STR(clear_name(Clear::Separate), "separate supply");
    CHECK_STR(clear_name(Clear::Gone), "rail gone");
    CHECK_STR(clear_name(Clear::Host), "host");

    // ---- Enforcer: the sequence that brings a locked bus back to 5 V ----
    {   // Not locked: nothing to do.
        Enforcer e;
        CHECK_EQ(int(e.update(0, false, 20000)), int(Action::None));
    }
    {   // Lock at 5 V: the 5 V order once (raises off, a held level released), then nothing while VBUS stays safe.
        Enforcer e;
        CHECK_EQ(int(e.update(0, true, 5000)), int(Action::Order5V));
        for (std::uint32_t t = 20; t < 10000; t += 20) CHECK_EQ(int(e.update(t, true, 5000)), int(Action::None));
    }
    {   // Lock at 20 V and the source comes down within kOrderMs: no Hard Reset.
        Enforcer e;
        CHECK_EQ(int(e.update(0, true, 20000)), int(Action::Order5V));
        CHECK_EQ(int(e.update(200, true, 12000)), int(Action::None));
        CHECK_EQ(int(e.update(400, true, 5100)), int(Action::None));
        for (std::uint32_t t = 420; t < 5000; t += 20) CHECK_EQ(int(e.update(t, true, 5100)), int(Action::None));
    }
    {   // VBUS still above kStuckMv kOrderMs after the order: Hard Reset; still up kResetMs after that: detach; then nothing more.
        Enforcer e;
        CHECK_EQ(int(e.update(0, true, 20000)), int(Action::Order5V));
        CHECK_EQ(int(e.update(kOrderMs - 20, true, 20000)), int(Action::None));
        CHECK_EQ(int(e.update(kOrderMs, true, 20000)), int(Action::HardReset));
        CHECK_EQ(int(e.update(kOrderMs + kResetMs - 20, true, 20000)), int(Action::None));
        CHECK_EQ(int(e.update(kOrderMs + kResetMs, true, 20000)), int(Action::Detach));
        for (std::uint32_t t = kOrderMs + kResetMs + 20; t < 20000; t += 20) CHECK_EQ(int(e.update(t, true, 20000)), int(Action::None));
    }
    {   // The Hard Reset brings VBUS down: no detach.
        Enforcer e;
        e.update(0, true, 9000);
        CHECK_EQ(int(e.update(kOrderMs, true, 9000)), int(Action::HardReset));
        CHECK_EQ(int(e.update(kOrderMs + 300, true, 0)), int(Action::None));
        CHECK_EQ(int(e.update(kOrderMs + 900, true, 5000)), int(Action::None));
        for (std::uint32_t t = kOrderMs + 920; t < 10000; t += 20) CHECK_EQ(int(e.update(t, true, 5000)), int(Action::None));
    }
    {   // VBUS back at 5 V, then up again while still locked (a legacy charger that went back up): a new order, a new sequence.
        Enforcer e;
        e.update(0, true, 9000);
        e.update(300, true, 5000);
        CHECK_EQ(int(e.update(2000, true, 9000)), int(Action::Order5V));
        CHECK_EQ(int(e.update(2000 + kOrderMs, true, 9000)), int(Action::HardReset));
    }
    {   // Unlock in the middle of the sequence stops it; the next lock starts a fresh one.
        Enforcer e;
        e.update(0, true, 20000);
        CHECK_EQ(int(e.update(100, false, 20000)), int(Action::None));
        CHECK_EQ(int(e.update(kOrderMs + 100, false, 20000)), int(Action::None));
        CHECK_EQ(int(e.update(5000, true, 20000)), int(Action::Order5V));
    }
    {   // An unknown VBUS (no monitor) never escalates: a Hard Reset needs a reading that proves VBUS stayed up.
        Enforcer e;
        CHECK_EQ(int(e.update(0, true, -1)), int(Action::Order5V));
        for (std::uint32_t t = 20; t < 10000; t += 20) CHECK_EQ(int(e.update(t, true, -1)), int(Action::None));
    }
    return test_report("test_loop_guard");
}
