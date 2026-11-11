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
        CHECK_EQ(d.pending(), false);
        CHECK_EQ(d.events(), 0);
    }
    {   // Nothing on the HID port (aux dead, the K2 runs from the PD bus): never a lock.
        Detector d;
        hold(d, 0, 60000, {300, 20000, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.pending(), false);
    }
    {   // HID-powered with nothing on the PD bus: no VBUS, no loop.
        Detector d;
        hold(d, 0, 60000, {5000, 0, false});
        CHECK_EQ(d.locked(), false);
    }
    {   // Suspected loop: aux and VBUS up, no host, for kSuspectMs. Pending before, locked at the end of the window.
        Detector d;
        hold(d, 0, kSuspectMs - 20, {5000, 5000, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.pending(), true);
        CHECK_EQ(d.update(kSuspectMs, {5000, 5000, false}), true);
        CHECK_EQ(d.locked(), true);
        CHECK_EQ(d.pending(), false);
        CHECK_EQ(int(d.reason()), int(Reason::Suspected));
        CHECK_EQ(d.events(), 1);
    }
    {   // A PC that configures inside the window (boot enumeration) leaves no lock and ends the pending state.
        Detector d;
        hold(d, 0, 1500, {5000, 5000, false});
        CHECK_EQ(d.pending(), true);
        hold(d, 1520, 20000, {5000, 5000, true});
        CHECK_EQ(d.pending(), false);
        CHECK_EQ(d.locked(), false);
    }
    {   // The window restarts when the condition breaks (aux below the present level, VBUS gone).
        Detector d;
        hold(d, 0, kSuspectMs - 100, {5000, 5000, false});
        d.update(kSuspectMs - 80, {5000, 0, false});
        hold(d, kSuspectMs - 60, 2 * kSuspectMs - 200, {5000, 5000, false});
        CHECK_EQ(d.locked(), false);
        hold(d, 2 * kSuspectMs - 180, 2 * kSuspectMs + 100, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
    }
    {   // Aux in the hysteresis band (between gone and present) does not start the window.
        Detector d;
        hold(d, 0, 10000, {kAuxPresentMv - 100, 5000, false});
        CHECK_EQ(d.locked(), false);
        CHECK_EQ(d.pending(), false);
    }
    {   // Clear: aux below kAuxGoneMv for kClearMs (the loop cable pulled). A shorter dip keeps the lock.
        Detector d;
        hold(d, 0, kSuspectMs, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
        const std::uint32_t t0 = kSuspectMs + 20;
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
        hold(d, 0, kSuspectMs, {5000, 5000, false});
        CHECK_EQ(d.locked(), true);
        const std::uint32_t t0 = kSuspectMs + 20;
        hold(d, t0, t0 + kClearMs - 20, {5000, 5000, true});
        CHECK_EQ(d.locked(), true);
        d.update(t0 + kClearMs, {5000, 5000, true});
        CHECK_EQ(d.locked(), false);
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
        hold(d, 0, kSuspectMs, {5000, 5000, false});
        CHECK_EQ(int(d.reason()), int(Reason::Suspected));
        d.update(kSuspectMs + 20, {9000, 9000, false});
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
