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
//  - Suspected loop, "the rail follows VBUS": in a loop the HID rail is fed from VBUS, so the two rise together. Aux crossing
//    kAuxPresentMv (4 V) up within kRiseWindowMs (100 ms) of VBUS crossing kVbusPresentMv (4 V) up, with no active host,
//    locks at once (no waiting window: the signature is specific, and waiting would leave a raise open). So does the first
//    sample with both readings known that already shows both up (a loop present at boot, or readings that came up one by one:
//    no order is known, the safe side). The usual way a loop shows: the charger sees the HID port's Rd through the loop
//    cable, so VBUS and the rail come up together and the K2 boots with both up.
//  - Never suspected: a rail that was up before VBUS appeared (a PC plugged in first, a K2 running from a PC and then a charger
//    attached, a charger that cuts VBUS while the rail stays up, e.g. an auto-off USB-A port every ~6.5 s with no load), and
//    VBUS up long before the rail (a charger first, then a PC or a 5 V brick on HID). This holds with the K2's USB link off
//    (usbcfg, the default), where a PC never becomes an active host: an HID-powered K2 runs charger tests unlocked.
//  - Not seen: a loop cable plugged into HID while the K2 already sinks VBUS (Rd presented): the rail rises long after VBUS,
//    as for a PC. A passive K2 presents no Rd, so the charger only switches VBUS on when the loop cable brings the HID port's
//    Rd, and both rise together. The certain rule catches a raise in that case on the first sample above 5.8 V.
//  - "Active host" is configured AND not suspended: TinyUSB has no VBUS sensing here, so after a PC is unplugged the
//    device stays "mounted" (suspended); a stale "configured" must not block the rule or clear a lock.
//  - Clear: proof of a separate supply, aux >= 4 V while VBUS < kVbusOffMv (1 V) for kSeparateMs (60 ms: VBUS cut, the rail
//    stays, which a loop cannot do); aux below kAuxGoneMv (3.5 V, the HID port is empty) for kClearMs (1 s); or an active host
//    for kClearMs with aux at a host's level (<= kHostMaxMv). A host never clears a lock while aux is above what a host gives.
//    Hysteresis on both rails: up at 4 V, down below 3.5 V.
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
inline constexpr std::int32_t kVbusGoneMv = 3500;     // PD-bus VBUS below (once up): the bus is down
inline constexpr std::int32_t kVbusOffMv = 1000;      // PD-bus VBUS below, with the rail up: a separate supply (clears)
inline constexpr std::uint32_t kRiseWindowMs = 100;   // aux and VBUS crossing 4 V up this close together = suspected loop
inline constexpr std::uint32_t kSeparateMs = 60;      // the separate-supply proof must hold this long (a few samples)
inline constexpr std::uint32_t kClearMs = 1000;       // the rail-gone and host clears must hold this long
static_assert(kAuxGoneMv < kAuxPresentMv && kVbusGoneMv < kVbusPresentMv && kHostMaxMv < kCertainMv, "hysteresis bands");

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
enum class Clear : std::uint8_t { None, Separate, Gone, Host };
constexpr const char* clear_name(Clear c) {
    return c == Clear::Separate ? "separate supply" : c == Clear::Gone ? "rail gone" : c == Clear::Host ? "host" : "none";
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
        const bool rose = track(now, s);   // the order of the rises, every sample, locked or not
        if (s.aux_mv > kCertainMv) {
            if (reason_ == Reason::None) { ++events_; cleared_ = Clear::None; }
            reason_ = Reason::Certain;
            sep_ = gone_ = host_run_ = false;
            return !was;
        }
        if (reason_ != Reason::None) {
            const bool known = s.aux_mv >= 0;   // an unknown reading proves nothing: it never clears
            run(sep_, sep_at_, known && s.aux_mv >= kAuxPresentMv && s.vbus_mv >= 0 && s.vbus_mv < kVbusOffMv, now);
            run(gone_, gone_at_, known && s.aux_mv < kAuxGoneMv, now);
            run(host_run_, host_at_, known && s.host && s.aux_mv <= kHostMaxMv, now);
            const Clear c = sep_ && now - sep_at_ >= kSeparateMs ? Clear::Separate
                            : gone_ && now - gone_at_ >= kClearMs ? Clear::Gone
                            : host_run_ && now - host_at_ >= kClearMs ? Clear::Host : Clear::None;
            if (c != Clear::None) {
                reason_ = Reason::None;
                cleared_ = c;
                sep_ = gone_ = host_run_ = false;
            }
            return locked() != was;
        }
        if (!rose || s.host || (gap_ >= 0 && std::uint32_t(gap_) > kRiseWindowMs)) return false;
        reason_ = Reason::Suspected;
        cleared_ = Clear::None;
        ++events_;
        return true;
    }
    bool locked() const { return reason_ != Reason::None; }
    Reason reason() const { return reason_; }
    std::uint16_t events() const { return events_; }   // locks since boot (wraps)
    // Ms between the two 4 V rises at the last time both came up; -1 = no order known (both already up when first read).
    std::int32_t rise_gap_ms() const { return gap_; }
    Clear last_clear() const { return cleared_; }      // what ended the last lock (None while locked or never locked)

private:
    static void run(bool& on, std::uint32_t& at, bool cond, std::uint32_t now) {
        if (!cond) on = false;
        else if (!on) { on = true; at = now; }
    }
    // Per-rail up/down with hysteresis and the time of each rise. True on the sample where both rails become up together,
    // with gap_ set. A sample with an unknown reading is skipped: the order stays as it was.
    bool track(std::uint32_t now, const Sample& s) {
        if (s.aux_mv < 0 || s.vbus_mv < 0) return false;
        const bool a = s.aux_mv >= (aux_up_ ? kAuxGoneMv : kAuxPresentMv);
        const bool v = s.vbus_mv >= (vbus_up_ ? kVbusGoneMv : kVbusPresentMv);
        if (a && !aux_up_) aux_at_ = now;
        if (v && !vbus_up_) vbus_at_ = now;
        aux_up_ = a;
        vbus_up_ = v;
        if (!a || !v) { apart_ = true; both_ = false; return false; }
        if (both_) return false;
        both_ = true;
        gap_ = !apart_ ? -1 : std::int32_t(aux_at_ > vbus_at_ ? aux_at_ - vbus_at_ : vbus_at_ - aux_at_);
        return true;
    }

    Reason reason_ = Reason::None;
    Clear cleared_ = Clear::None;
    bool aux_up_ = false, vbus_up_ = false, both_ = false, apart_ = false;   // apart_: the rails were seen apart once
    bool sep_ = false, gone_ = false, host_run_ = false;
    std::uint32_t aux_at_ = 0, vbus_at_ = 0, sep_at_ = 0, gone_at_ = 0, host_at_ = 0;
    std::int32_t gap_ = -1;
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
std::uint16_t events();

}  // namespace k2::loopguard
