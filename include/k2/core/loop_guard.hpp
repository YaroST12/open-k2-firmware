// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID-cable loop guard: the 5 V lock.
//
// The topology: the K2 on a charger, and a cable from its PD-bus port back into its own left (HID) port. The HID port is
// USB 2.0, 5 V only, and its VBUS then follows the PD bus: any contract or trigger above 5 V puts 9-36 V into it. The guard
// sees the loop from three signals the K2 already has, and while it holds the lock nothing may raise VBUS above 5 V.
//
//  - aux rail (PA0 behind the 11:1 divider, `adc::Snapshot::aux_mv`): the HID port's VBUS. A USB host gives 4.75-5.25 V.
//  - PD-bus VBUS (the power monitor's fast reading).
//  - the USB device state on the HID port: a host that configured the K2 and keeps the bus awake (not suspended).
//
// Rules (Detector, pure, host-tested in tests/test_loop_guard.cpp):
//  - Certain loop: aux above kCertainMv (5.8 V). No USB 2.0 host gives that; it locks on the first sample.
//  - Suspected loop: aux >= kAuxPresentMv (4 V) and VBUS >= kVbusPresentMv (4 V) with no active host for kSuspectMs
//    (3 s). In a loop no host ever configures the K2. A PC enumerates the K2 in well under 1.5 s (power_source::kDecideMs
//    budgets 2 s for the same question); 3 s leaves room for a slow hub and is still short against any user action. A
//    dumb 5 V brick on the HID port looks the same and is an accepted false positive (it only refuses raises). So is a PC
//    while the K2's USB link is off (usbcfg, the default): the K2 never enumerates, a PC and a loop look alike.
//  - "Active host" is configured AND not suspended: TinyUSB has no VBUS sensing here, so after a PC is unplugged the
//    device stays "mounted" (suspended). Without the suspend term, a PC swapped for the loop cable while the K2 runs from
//    the charger would leave a stale "configured" and the suspected rule would never fire.
//  - Clear: aux below kAuxGoneMv (3.5 V, the HID port is empty) for kClearMs (1 s), or an active host for kClearMs with aux at
//    a host's level (<= kHostMaxMv). A host never clears a lock while aux is above what a host gives. Hysteresis: the window
//    starts at 4 V, the clear needs 3.5 V.
//  - While the suspicion window runs the guard is `pending()`: shown and logged, but not a lock. A raise in that window
//    puts the bus voltage on aux, which the certain rule catches on the next 20 ms sample.
//
// Enforcement (Enforcer, pure): on the lock edge the 5 V order (every raise path refuses, a PD contract above 5 V is released
// to PDO 1, a legacy session goes back to 0 V on D+/D-, which every legacy protocol answers with 5 V). VBUS still above
// kStuckMv kOrderMs after the order (the SPR source transition is 550 ms at most): Hard Reset. Still above kResetMs after it
// (tSafe0V is 650 ms): CC detach. Then nothing more is tried; a VBUS that came back to vSafe5V and rises again re-arms it.
//
// Boot note: rdintent::host_powered(aux) reads "aux up at boot" as a PC on the HID port. Through a loop the aux rail is the
// PD bus, so a comeback boot in a loop is judged host-powered and stays passive (no resumed session). That is the safe side:
// the guard does not rely on it and does not change it; aux early in boot is no proof of a PC.
#pragma once
#include <cstdint>

#include "k2/core/rd_intent.hpp"

namespace k2::loopguard {

inline constexpr std::int32_t kCertainMv = 5800;      // aux above this: no USB 2.0 host (5.25 V max + divider tolerance)
inline constexpr std::int32_t kHostMaxMv = 5500;      // aux at or below this can be a host (clears with an active host)
inline constexpr std::int32_t kAuxPresentMv = 4000;   // aux at or above: something powers the HID port
inline constexpr std::int32_t kAuxGoneMv = rdintent::kHostAuxMv;   // aux below: the HID port is empty (3.5 V)
inline constexpr std::int32_t kVbusPresentMv = 4000;  // PD-bus VBUS at or above: a source feeds the bus
inline constexpr std::uint32_t kSuspectMs = 3000;     // aux + VBUS up with no active host this long = suspected loop
inline constexpr std::uint32_t kClearMs = 1000;       // the clear condition must hold this long
static_assert(kAuxGoneMv < kAuxPresentMv && kHostMaxMv < kCertainMv, "hysteresis bands");

inline constexpr std::int32_t kStuckMv = 6000;        // VBUS above this after the 5 V order: escalate
inline constexpr std::int32_t kSafeMv = 5500;         // VBUS at or below this: back at vSafe5V, the sequence re-arms
inline constexpr std::uint32_t kOrderMs = 600;        // 5 V order -> Hard Reset (tPSTransition 550 ms + a sample)
inline constexpr std::uint32_t kResetMs = 1000;       // Hard Reset -> CC detach (tSafe0V 650 ms + margin)

// The one line the lock shows: the Caution footer on every page while locked, PD Stress's refusal, the shell's lab refusal. The
// charger test's Reason::Loop name is the same words (its pure table cannot include this header).
inline constexpr const char* kLockText = "HID-cable loop: 5V lock";

enum class Reason : std::uint8_t { None, Certain, Suspected };
constexpr const char* reason_name(Reason r) {
    return r == Reason::Certain ? "certain" : r == Reason::Suspected ? "suspected" : "none";
}

// One sample. A negative reading = unknown (no ADC snapshot, no monitor): counted as absent, never as a loop.
struct Sample {
    std::int32_t aux_mv = -1;
    std::int32_t vbus_mv = -1;
    bool host = false;   // a USB host configured the K2 and keeps the bus awake (not suspended)
};

class Detector {
public:
    // Returns true when locked() changed with this sample.
    bool update(std::uint32_t now, const Sample& s) {
        const bool was = locked();
        if (s.aux_mv > kCertainMv) {
            if (reason_ == Reason::None) ++events_;
            reason_ = Reason::Certain;
            pending_ = gone_ = host_run_ = false;
            return !was;
        }
        if (reason_ != Reason::None) {
            if (s.aux_mv < kAuxGoneMv) { if (!gone_) { gone_ = true; gone_at_ = now; } }
            else gone_ = false;
            if (s.host && s.aux_mv <= kHostMaxMv) { if (!host_run_) { host_run_ = true; host_at_ = now; } }
            else host_run_ = false;
            if ((gone_ && now - gone_at_ >= kClearMs) || (host_run_ && now - host_at_ >= kClearMs)) {
                reason_ = Reason::None;
                gone_ = host_run_ = false;
            }
            return locked() != was;
        }
        const bool suspect = s.aux_mv >= kAuxPresentMv && s.vbus_mv >= kVbusPresentMv && !s.host;
        if (!suspect) { pending_ = false; return false; }
        if (!pending_) { pending_ = true; pend_at_ = now; }
        if (now - pend_at_ < kSuspectMs) return false;
        pending_ = false;
        reason_ = Reason::Suspected;
        ++events_;
        return true;
    }
    bool locked() const { return reason_ != Reason::None; }
    Reason reason() const { return reason_; }
    bool pending() const { return pending_; }       // the suspicion window runs (not a lock)
    std::uint16_t events() const { return events_; }   // locks since boot (wraps)

private:
    Reason reason_ = Reason::None;
    bool pending_ = false, gone_ = false, host_run_ = false;
    std::uint32_t pend_at_ = 0, gone_at_ = 0, host_at_ = 0;
    std::uint16_t events_ = 0;
};

enum class Action : std::uint8_t { None, Order5V, HardReset, Detach };
constexpr const char* action_name(Action a) {
    return a == Action::Order5V ? "5V order" : a == Action::HardReset ? "hard reset" : a == Action::Detach ? "detach" : "none";
}

// The escalation once locked. update() every main-loop slot; the caller carries out the returned action.
class Enforcer {
public:
    Action update(std::uint32_t now, bool locked, std::int32_t vbus_mv) {
        if (!locked) { stage_ = Stage::Idle; return Action::None; }
        const bool safe = vbus_mv >= 0 && vbus_mv <= kSafeMv;
        const bool stuck = vbus_mv > kStuckMv;
        switch (stage_) {
            case Stage::Idle:
                stage_ = Stage::Ordered; at_ = now;
                return Action::Order5V;
            case Stage::Safe:
                if (!stuck) return Action::None;
                stage_ = Stage::Ordered; at_ = now;   // up again while locked: a fresh order and sequence
                return Action::Order5V;
            case Stage::Ordered:
                if (safe) { stage_ = Stage::Safe; return Action::None; }
                if (stuck && now - at_ >= kOrderMs) { stage_ = Stage::Reset; at_ = now; return Action::HardReset; }
                return Action::None;
            case Stage::Reset:
                if (safe) { stage_ = Stage::Safe; return Action::None; }
                if (stuck && now - at_ >= kResetMs) { stage_ = Stage::Detached; return Action::Detach; }
                return Action::None;
            case Stage::Detached:
                if (safe) stage_ = Stage::Safe;
                return Action::None;   // nothing left to try
        }
        return Action::None;
    }

private:
    enum class Stage : std::uint8_t { Idle, Ordered, Reset, Detached, Safe };
    Stage stage_ = Stage::Idle;
    std::uint32_t at_ = 0;
};

// ---- Hardware binding (src/core/loop_guard.cpp) ----
// Main loop, every slot: samples aux, VBUS and the USB host state, updates the detector, pushes the lock to the PD sink and the
// legacy engine, runs the enforcer, logs [LOOP] on every transition.
void poll(std::uint32_t now_ms);
bool locked();
Reason reason();
bool pending();
std::uint16_t events();

}  // namespace k2::loopguard
