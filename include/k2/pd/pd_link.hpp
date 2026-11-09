// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Attach/detach tracking for the PD pages, pure logic (no I/O, host-tested in
// tests/test_pd_link.cpp). The owner feeds it VBUS (power monitor), the active CC line
// (FUSB302) and the Source_Capabilities counter every UI tick; it says whether the charger
// data on screen is live or the last one seen, and whether the e-marker data may belong to a
// cable that has since been swapped.
//
//  detach  = VBUS below vSafe0V (0.8 V) for more than 300 ms, OR no CC for more than 300 ms
//  attach  = VBUS above vSafe5V min (4.75 V), OR a CC line back
//  charger data is stale while detached and while "waiting" (attached again, no new caps yet); "waiting"
//  also ends after kWaitingCapsMs without new Source_Capabilities, so a non-PD (Type-C only) source does
//  not keep the pages on "waiting for caps" forever
//  cable data is stale after a CC detach (a VBUS loss alone does not touch the cable)
//
// Inline detection: the K2 sits between a charger and a real sink (a laptop). Anything that
// touches Rd, the CC switches or transmits can then break the live contract, so every active
// operation asks inline() first. It latches while attached on: more than 50 mA flowing, a sink's
// Request decoded that the K2 did not send, a VBUS rise above 5.5 V that the K2 did not cause
// (own_contract false, and not within kOwnHoldMs of the K2's own trigger), or the lines alone: VBUS above 5.5 V with no contract of
// the K2's own in this attach, or a sink's Rd on CC with no Rd of the K2 (kInlineForeignMs). "Possibly inline" is the default
// until proven standalone. It clears only on proof: kInlineQuietMs with no load, no Request and no such evidence while attached,
// or a detach that outlasts kInlineHoldMs.
#pragma once
#include <cstddef>
#include <cstdint>

namespace k2::pd {

inline constexpr std::int32_t kVSafe0Mv = 800;
inline constexpr std::int32_t kVSafe5MinMv = 4750;
inline constexpr std::int32_t kVSafe5MaxMv = 5500;
inline constexpr std::uint32_t kDetachDebounceMs = 300;
// An attached source that sends no Source_Capabilities within this long is not PD (a plain Type-C port):
// stop "waiting" and let the callers show the Type-C advertisement. Longer than the ~3 s after which
// fusb302::typec_ma() reports the source's Rp current, so the data is there when waiting ends.
inline constexpr std::uint32_t kWaitingCapsMs = 5000;

constexpr bool is_vsafe5(std::int32_t mv) { return mv >= kVSafe5MinMv && mv <= kVSafe5MaxMv; }

struct LinkInput {
    std::uint32_t now_ms = 0;
    bool vbus_known = false;   // false while the power monitor is offline
    std::int32_t vbus_mv = 0;
    std::uint8_t cc = 0;       // 0 = no CC line, 1 or 2
    std::uint32_t caps_rx = 0; // Source_Capabilities counter (rises with every new message)
    bool current_known = false;
    std::int32_t current_ma = 0;     // signed; only the magnitude is used
    std::uint32_t requests = 0;      // Request messages decoded from a sink (not ours), counter
    bool own_contract = false;       // the K2 itself drives VBUS up: PD trigger or fast-charge trigger active
    // The K2's own draw that may flow through the shunt: with no USB host (no HID power) the K2 runs on the measured VBUS and
    // its ~60-100 mA (LCD, MCU) reads as a load (hardware 2026-11-06, USB-A port, "Load attached" with nothing on the other
    // port). That much is not a sink.
    std::int32_t self_supply_ma = 0;
    // A sink's Rd (about 1.5 V) on a CC line while the K2 presents none itself: something is plugged into the other end and
    // announces itself as a sink. The driver decides it (fusb302::sink_cc_far()); false when unknown.
    bool far_sink_cc = false;
};

// Allowance for the K2's own supply current while it runs on the measured VBUS (no USB host enumerated).
inline constexpr std::int32_t kSelfSupplyAllowanceMa = 150;

inline constexpr std::int32_t kInlineCurrentMa = 50;
inline constexpr std::int32_t kInlineVbusMv = kVSafe5MaxMv;
// A flip back to "standalone" needs proof, never a missed sample: no Request and below 50 mA for this long. A phone that sips
// less than 50 mA for a few seconds is still a phone, and the K2's Rd on top of its own breaks the sink's contract.
inline constexpr std::uint32_t kInlineQuietMs = 10000;
// The inline state outlives a VBUS collapse (a hub or charger re-negotiating, a swap) for this long: "possibly inline" is the safe
// state, the K2 stays passive through the gap. Detached longer than this = unplugged for real, the next attach starts standalone.
inline constexpr std::uint32_t kInlineHoldMs = 15000;
// "Possibly inline" from what the lines show, without any traffic: VBUS above vSafe5V that this K2 never asked for (it has had no
// contract of its own in this attach), or a sink's Rd on CC with no Rd of the K2. A live dock - K2 - laptop contract is silent
// to a K2 that booted into it. Continuous for this long before it latches (the K2's own contract ending, a sample glitch), and it
// holds for as long as the evidence lasts: the quiet window only runs once the lines show nothing.
inline constexpr std::uint32_t kInlineForeignMs = 1500;
// A VBUS rise this soon after the K2's own trigger ended is still the K2's doing: a legacy trigger that gives up (abort,
// reset_bus) releases D+/D- and the charger answers a few ms later, when fctrig is no longer Negotiating or Active.
// The reset hold of the data lines is 500 ms; a real sink behind the K2 still latches by its current or its Request.
inline constexpr std::uint32_t kOwnHoldMs = 2000;

class Link {
public:
    void update(const LinkInput& in);

    // Debug / fake PDOs: hold the state against the real inputs until clear_force().
    void force_attached(std::uint32_t now_ms);
    void force_detached(std::uint32_t now_ms);
    void clear_force() { forced_ = false; }

    bool attached() const { return attached_; }
    bool waiting() const { return waiting_; }  // attached after a detach, no new caps yet
    bool charger_stale() const { return !attached_ || waiting_; }
    // Latched by a load, a foreign Request or a foreign VBUS rise; held across a short detach (kInlineHoldMs) and left only on proof
    // (kInlineQuietMs of quiet while attached, or a detach that outlasts the hold).
    bool inline_mode() const { return inline_; }
    std::uint32_t detached_for_s(std::uint32_t now_ms) const { return now_ms > detach_at_ ? (now_ms - detach_at_) / 1000 : 0; }
    // True once per attach edge: the caller drops the previous charger's decoded data when
    // the first new Source_Capabilities arrive.
    bool take_new_source();

    void emarker_scanned(std::uint32_t now_ms);
    bool cable_scanned() const { return scanned_; }
    bool cable_stale() const { return scanned_ && cable_dirty_; }
    std::uint32_t cable_age_s(std::uint32_t now_ms) const { return now_ms > scan_at_ ? (now_ms - scan_at_) / 1000 : 0; }

private:
    bool attached_ = true;
    bool waiting_ = false;
    bool forced_ = false;
    bool new_source_ = false;
    bool inline_ = false;
    std::uint32_t requests_at_attach_ = 0, requests_seen_ = 0, quiet_since_ = 0;
    bool vbus_was_low_ = false;
    bool own_seen_ = false;
    bool own_ever_ = false;   // the K2 had a contract of its own at any time since this attach: a high VBUS is then its doing
    bool foreign_timing_ = false;
    std::uint32_t foreign_since_ = 0;
    std::uint32_t own_last_ms_ = 0;
    bool vbus_low_ = false, cc_gone_ = false;
    bool low_timing_ = false, gone_timing_ = false;
    std::uint32_t low_since_ = 0, gone_since_ = 0;
    std::uint32_t detach_at_ = 0, attach_at_ = 0;
    std::uint32_t rx_at_detach_ = 0, last_rx_ = 0;
    bool scanned_ = false, cable_dirty_ = false;
    std::uint32_t scan_at_ = 0;

    void set_attached(bool on, std::uint32_t at_ms);
};

// "5s", "1m 23s", "1h 02m", "99h+".
void format_ago(std::uint32_t seconds, char* out, std::size_t n);

}  // namespace k2::pd
