// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The one-call "stop": everything a host tool (or a lost host) can have left holding the output above 5 V, undone in one
// fixed order. One implementation serves the HID `stop` command, the dead-man lease expiry and the CDC shell `stop` line.
//
// The Env is the seam (the real one is in src/core/remote.cpp, a fake in tests/test_stop.cpp):
//   bool test_active() / cancel_test()      a charger test owns the link
//   bool fc_active()   / stop_fc()          an FC trigger or scan is negotiating or holding; stop_fc also starts the measured 5 V release
//   bool pd_held()     / release_pd()       a PD trigger contract is held (the release is a no-op at 5 V, so it always runs)
//   bool spoof_active() / disarm_spoof()    the e-marker spoof answers on the top port
//   int32_t vbus_mv()                       the latest VBUS sample (power::vbus_fast_mv)
//   bool fc_release_busy()                  the measured FC release window is still open
//
// Order: cancel the test first (it would re-trigger), stop FC next (its release window measures the 5 V the PD release must
// reach), release PD, disarm the spoof last (it holds no voltage). The measurements come after the actions.
#pragma once

#include <cstdint>

namespace k2::stop {

inline constexpr std::uint8_t kTest = 1;    // a charger test was running
inline constexpr std::uint8_t kFc = 2;      // an FC trigger or scan was active
inline constexpr std::uint8_t kPd = 4;      // a PD trigger contract was held
inline constexpr std::uint8_t kSpoof = 8;   // a spoof was armed

struct Report {
    std::uint8_t active = 0;          // kTest | kFc | kPd | kSpoof: what was active BEFORE the call
    std::int32_t vbus_mv = 0;         // the latest VBUS sample after the actions (not yet settled if the FC release is busy)
    bool fc_release_busy = false;     // the measured FC release is still running
};

template <class Env>
Report stop_all(Env& e) {
    Report r;
    const bool test = e.test_active(), fc = e.fc_active(), pd = e.pd_held(), spoof = e.spoof_active();
    if (test) { r.active |= kTest; e.cancel_test(); }
    if (fc) { r.active |= kFc; e.stop_fc(); }
    if (pd) r.active |= kPd;
    e.release_pd();
    if (spoof) { r.active |= kSpoof; e.disarm_spoof(); }
    r.vbus_mv = e.vbus_mv();
    r.fc_release_busy = e.fc_release_busy();
    return r;
}

}  // namespace k2::stop
