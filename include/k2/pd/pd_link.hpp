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
// or a detach that outlasts kInlineHoldMs. A VBUS cut or a short detach pauses that window (vbus_cut()).
#pragma once
#include <cstddef>
#include <cstdint>

#include "k2/pd/cc_crossed.hpp"
#include "k2/pd/pd_verdict.hpp"
#include "k2/pd/ra_guard.hpp"

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
    // The passive CC levels for the waiting classifier (raguard::RpTrace): a line at/above raguard::kRpMinMv carries
    // an Rp. -1 = no ADC reading. `passive` = the K2 holds nothing on the lines and no sink is inline, so the lines
    // are the far port's own advertisement (the same gate the connection page's topology uses).
    std::int32_t cc1_mv = -1, cc2_mv = -1;
    bool passive = false;
    // The K2 itself holds a line (the Wake sleeping port's Rd hold): the levels are then no longer the far ports' own.
    bool line_held = false;
    // PD-talk inputs for the one verdict per attach (pd_verdict.hpp): any PD message decoded this attach, and
    // whether the sink side answered anything (a GoodCRC / Request / caps / VDM from the far side).
    bool any_pd_msg = false;
    bool sink_answered = false;
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

// A sink's Rd on CC (LinkInput::far_sink_cc) with VBUS never at vSafe5V for this long is not a sink in a contract: a dual-role
// phone alone on the port toggles its Rp through that level (bench 2026-11-10, Pixel alone: inline held for minutes, which
// blocked the idle wake). Longer than the foreign-evidence timer, so a charger that is still ramping up keeps its evidence.
inline constexpr std::uint32_t kDarkSinkMs = 3000;

// A VBUS cut (pattern usb-a-no-load-auto-off: a USB-A port that switches its output off every ~6.5 s while nothing draws, and back
// on by itself): VBUS below vSafe5V min although it was at or above it within kDarkSinkMs. A cut is neither proof that a sink left
// nor evidence that one is there:
//  - the inline quiet window pauses through it (and through any detach inside kInlineHoldMs) instead of starting over at the
//    next attach, so the quiet gathered on both sides of a cut adds up; the cut's own time never counts as quiet;
//  - a sink-band level on CC read during it is void: through an A-to-C cable the Rp is fed from VBUS and sweeps that band while
//    the output collapses. A sink's Rd counts while VBUS is up (or, as before, while a fresh attach waits for VBUS to rise).
// Current and Requests stay evidence everywhere. After a pause the latch clears no sooner than kInlineResumeMs of VBUS back:
// a sink that returns with VBUS (a PD sink Requests within ~300 ms of the caps, a phone draws within ~1 s) shows itself first.
inline constexpr std::uint32_t kInlineResumeMs = 2000;

constexpr bool vbus_cut(bool vbus_known, std::int32_t vbus_mv, bool up_seen, std::uint32_t now_ms, std::uint32_t last_up_ms) {
    return vbus_known && vbus_mv < kVSafe5MinMv && up_seen && now_ms - last_up_ms < kDarkSinkMs;
}

// What latched the inline state (Link::inline_cause()): the [TEST] preflight refusal names it.
enum class InlineCause : std::uint8_t { None, Load, Request, Rise, Vbus, FarRd };
constexpr const char* inline_cause_name(InlineCause c) {
    switch (c) {
        case InlineCause::None: return "none";
        case InlineCause::Load: return "load";
        case InlineCause::Request: return "sink request";
        case InlineCause::Rise: return "vbus rise";
        case InlineCause::Vbus: return "foreign vbus";
        case InlineCause::FarRd: return "far rd";
    }
    return "?";
}

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
    // The one "charger waiting" verdict (the Rp trace over the CC lines, raguard::RpTrace): ChargerWaiting = a
    // charger's Rp steady for >= raguard::RpTrace::kSteadyMs with VBUS down, DualRolePort = an Rp that toggles.
    // The Source tab, the bottom bar and the remote state field read this same value.
    raguard::WaitKind wait_kind() const {
        const bool up = vbus_known_ && vbus_mv_ >= raguard::kVbusAttachedMv;
        const raguard::WaitKind k = wait_trace_.classify(last_now_, up);
        // A slow dual-role toggle under kRpMinMv (a phone's current-source Rp opens to ~1.7 V) reads NoSource
        // on the Rp trace; the level trace sees it fall and rise again. A steady level never does (the CC2
        // bias net's ~1.69 V included), so it cannot turn the verdict.
        if (k == raguard::WaitKind::NoSource && drp_trace_.toggling(last_now_)) return raguard::WaitKind::DualRolePort;
        return k;
    }
    bool charger_waiting() const { return wait_kind() == raguard::WaitKind::ChargerWaiting; }
    // Crossed CC through the K2 (raguard::CrossTrace): a charger's Rp on one line and a phone's toggle on the other with
    // VBUS dark. Fed only while the K2 is passive (no Rd / Ra / Wake hold of ours, no sink inline, no contract) and VBUS is
    // below raguard::kVbusAttachedMv; a verdict needs raguard::CrossTrace::kObserveMs of those lines. The Source tab, the bar,
    // Home and the remote state field read this one value.
    raguard::CrossKind cc_cross() const { return cross_trace_.classify(last_now_); }
    // Latched by a load, a foreign Request or a foreign VBUS rise; held across a short detach (kInlineHoldMs) and left only on proof
    // (kInlineQuietMs of quiet while attached, paused through a VBUS cut or a short detach, or a detach that outlasts the hold).
    bool inline_mode() const { return inline_; }
    InlineCause inline_cause() const { return inline_ ? cause_ : InlineCause::None; }
    std::uint32_t detached_for_s(std::uint32_t now_ms) const { return now_ms > detach_at_ ? (now_ms - detach_at_) / 1000 : 0; }
    // True once per attach edge: the caller drops the previous charger's decoded data when
    // the first new Source_Capabilities arrive.
    bool take_new_source();

    // The one "PD present" verdict per attach (ROADMAP 2026-11-10, Row C; pd_verdict.hpp). Pending until
    // kPdVerdictSettleMs after the attach; the Connection page gates its tabs on it, the remote state
    // exposes it (kLinkNoPd), the binlog logs the edge once.
    std::uint32_t attach_at() const { return attach_at_; }
    std::uint32_t caps_rx_this_attach() const { return last_rx_ - rx_attach_; }
    PdVerdict pd_verdict() const { return verdict_; }
    PdNoneWhy pd_none_why() const { return none_why_; }

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
    InlineCause cause_ = InlineCause::None;
    std::uint32_t requests_at_attach_ = 0, requests_seen_ = 0, quiet_since_ = 0;
    std::uint32_t paused_at_ = 0;   // the last tick the quiet window was paused (a cut or a detach, see vbus_cut())
    bool up_seen_ = false;          // vbus_up_ms_ holds a real sample
    bool vbus_was_low_ = false;
    bool own_seen_ = false;
    bool own_ever_ = false;   // the K2 had a contract of its own at any time since this attach: a high VBUS is then its doing
    bool foreign_timing_ = false;
    std::uint32_t foreign_since_ = 0;
    std::uint32_t own_last_ms_ = 0;
    bool vbus_low_ = false, cc_gone_ = false;
    std::uint32_t vbus_up_ms_ = 0;   // the last sample at/above vSafe5V (see kDarkSinkMs)
    bool low_timing_ = false, gone_timing_ = false;
    std::uint32_t low_since_ = 0, gone_since_ = 0;
    std::uint32_t detach_at_ = 0, attach_at_ = 0;
    std::uint32_t rx_at_detach_ = 0, last_rx_ = 0;
    std::uint32_t rx_attach_ = 0;   // Source_Capabilities counter at the attach edge
    PdVerdict verdict_ = PdVerdict::Pending;
    PdNoneWhy none_why_ = PdNoneWhy::None;
    bool scanned_ = false, cable_dirty_ = false;
    std::uint32_t scan_at_ = 0;
    // The waiting classifier's input: the Rp presence trace over the CC lines (fed only while passive), and the
    // last VBUS reading for its gate.
    raguard::RpTrace wait_trace_;
    raguard::DrpTrace drp_trace_;        // the slow dual-role toggle on the raw levels (same gate; under kRpMinMv)
    raguard::CrossTrace cross_trace_;   // the crossed-CC detector's input (same gate, plus VBUS dark and no inline / contract)
    std::uint32_t last_now_ = 0;
    bool vbus_known_ = false;
    std::int32_t vbus_mv_ = 0;

    void set_attached(bool on, std::uint32_t at_ms);
};

// Active Identify of a device ALONE on the K2 (ROADMAP "Sink Identify inline drops the phone from 9 V to 5 V",
// fix item (2)): the link state decides, never a confirm. alone = not inline, VBUS dark, no source caps this
// attach, and the waiting classifier reads a dual-role port (an Rp that toggles every ~100 ms - a phone alone).
// A steady-Rp charger alone is ChargerWaiting there, so Identify stays NoDevice/refused as before. Inline is
// refused outright (batch 2, unchanged). VBUS unknown (-1 / monitor offline) cannot claim "dark".
constexpr bool identify_alone(bool inline_mode, bool vbus_known, std::int32_t vbus_mv, bool caps_this_attach,
                              raguard::WaitKind wait_kind) {
    return !inline_mode && vbus_known && vbus_mv >= 0 && vbus_mv < kVSafe0Mv && !caps_this_attach &&
           wait_kind == raguard::WaitKind::DualRolePort;
}

// "5s", "1m 23s", "1h 02m", "99h+".
void format_ago(std::uint32_t seconds, char* out, std::size_t n);

}  // namespace k2::pd
