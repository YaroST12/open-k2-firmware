// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Which CC lines the FUSB302 side pulls down with Rd, pure logic (host-tested in tests/test_pd_ccmode.cpp).
//
// A plain boot is fully passive: SWITCHES0 = measure
// only, no Rd ever. Any Rd while a laptop and a dock attach through the K2 costs the link its USB3 (the
// laptop sees Rd/Rd or a doubled Rd and settles at USB2 + Billboard; hardware 2026-11-03). So:
//   - plain boot: no Rd. Attach, orientation and PD are found by listening (comparator + BMC RX).
//   - PLUS / OK held at power-up (bootmode.hpp): ONE Rd pinned for the whole session (sink mode), never Rd on both lines:
//     Rd/Rd is the Debug Accessory signature, a source that sees it on a direct plug turns its Rp off and stays off until a
//     physical detach (OnePlus 120W brick, hardware 2026-11-06).
//   - an explicit active feature (charger test, PD trigger / request, Identify, protocol lab) may present
//     Rd on the ACTIVE line only, for its duration, and never while a sink is downstream (inline).
#pragma once
#include <cstdint>
#include "k2/pd/pd_link.hpp"

namespace k2::pd {

// Rd lines to present: bit0 = CC1, bit1 = CC2. `pinned` = boot-mode Rd bits (0 = plain boot),
// `active_op` = an explicit active feature is running, `cc` = the active line (1/2, 0 = none yet).
constexpr std::uint8_t rd_lines(std::uint8_t pinned, bool active_op, bool inline_sink, std::uint8_t cc) {
    if (inline_sink) return 0;
    // A pinned Rd (PLUS = CC2, OK = CC1) follows the active line once one is known: the key's own Rd is hardware, ours
    // belongs where the charger is, so the charger still holds VBUS after the key is released. Never both lines: a stale
    // pinned 3 collapses to the active line, or to CC2 while none is known.
    if ((pinned & 3) != 0) return cc == 1 ? 1 : cc == 2 ? 2 : (pinned & 3) == 1 ? std::uint8_t(1) : std::uint8_t(2);
    if (active_op && cc != 0) return cc == 1 ? 1 : 2;
    return 0;
}

// The Rd lines a run start asks for (`lines`, start_rd_lines) once a boot-mode pin (`pin`, 0 = plain boot) exists: the start's
// "both while no CC is known" would add a second Rd to the pinned one, which is the Debug Accessory signature. With a pin the
// pair collapses onto the line the pin follows (the active line `cc`, else the pinned one); a single line and a plain boot's
// pair pass through.
constexpr std::uint8_t key_rd_lines(std::uint8_t lines, std::uint8_t pin, std::uint8_t cc) {
    if (lines != 3 || (pin & 3) == 0) return lines;
    return cc == 1 ? 1 : cc == 2 ? 2 : (pin & 3) == 1 ? std::uint8_t(1) : std::uint8_t(2);
}

// A run start (charger test START, PD trigger / request entry) with no sink inline: a charger alone on the
// bottom port never turns VBUS on without Rd, and plain boot has none. Present Rd on the line that sees Rp,
// or on both while none is detected yet. Inline: never (the sink's own Rd must be alone on the line).
constexpr std::uint8_t start_rd_lines(bool inline_sink, std::uint8_t cc) {
    if (inline_sink) return 0;
    return cc == 1 ? 1 : cc == 2 ? 2 : 3;
}

inline constexpr std::int32_t kStartVbusMv = 4500;     // the run preflight wants a live 5 V source
inline constexpr std::uint32_t kStartVbusWaitMs = 5000;  // how long Rd gets to bring VBUS up (the C135 switches it on ~190 ms after Rd; the averaged VBUS reading lagged that by seconds until 1559ae0)
inline constexpr std::uint32_t kStartContractWaitMs = 3000;  // the start's own Request (PDO 1) in flight: Accept + PS_RDY take 100-250 ms (X865, C135); never wait longer than this
inline constexpr std::uint32_t kStartCapsWaitMs = 2000;  // VBUS up, still no Source_Capabilities: PD silence this long means a non-PD source (spec tTypeCSinkWaitCap 310-620 ms; C135 ~670 ms after Rd)

// The start acts as a sink while it waits (a source whose Source_Capabilities get no GoodCRC never settles):
// Auto-CRC is on (pd::CrcDemand::run_start) and PDO 1 (5 V) is requested once, on the attached line, when the caps
// arrive. Never with a sink inline, never twice, never over a negotiation that is already running.
// What stands in the way of that Request, as bits for the black box: inline 1, no CC attached 2, sink engine busy 4,
// already requested 8, legacy-only session 16 (the source must be left without a contract). No VBUS reading is among
// them: caps arriving prove VBUS is up (a slow-ramp charger sends them at 0.6 V), and the source's
// SenderResponseTimer (24-30 ms, the C135 allows ~57) does not wait for a measurement.
constexpr std::uint8_t start_request_blockers(bool inline_sink, std::uint8_t cc, bool sink_idle, bool requested,
                                              bool legacy_only = false) {
    return std::uint8_t((inline_sink ? 1 : 0) | (cc == 0 ? 2 : 0) | (sink_idle ? 0 : 4) | (requested ? 8 : 0) |
                        (legacy_only ? 16 : 0));
}
constexpr bool start_request_wanted(bool rd_start, bool inline_sink, std::uint8_t cc, bool sink_idle, bool requested,
                                    bool legacy_only = false) {
    return rd_start && start_request_blockers(inline_sink, cc, sink_idle, requested, legacy_only) == 0;
}

// Source_Capabilities that went by while Auto-CRC was on were GoodCRC'd by the chip, and that GoodCRC starts the source's
// SenderResponseTimer (24-30 ms): only a Request stops it, silence ends in a Hard Reset (OnePlus 120W, 2026-11-06: a pd-reread's
// first caps were acked, nothing answered, Hard Reset 27 ms later, VBUS to 0.4 V for 650 ms). Whatever turned Auto-CRC on, the K2
// Requests PDO 1 itself unless the answer is somebody else's. Blockers, as bits: caps not acked 1 (the source heard nothing, no
// timer runs), a real sink inline 2 (it owns the answer), no CC attached 4, legacy-only session 8 (PD-silent: the source must be
// left without a contract), sink engine busy 16 (a negotiation is running), prepare running 32 (it re-announces, then Requests),
// requested already in this frame 64 (the run-start / resume branch just asked).
constexpr std::uint8_t acked_caps_blockers(bool acked, bool inline_sink, std::uint8_t cc, bool legacy_only, bool sink_idle,
                                           bool prep_running, bool requested_now) {
    return std::uint8_t((acked ? 0 : 1) | (inline_sink ? 2 : 0) | (cc == 0 ? 4 : 0) | (legacy_only ? 8 : 0) | (sink_idle ? 0 : 16) |
                        (prep_running ? 32 : 0) | (requested_now ? 64 : 0));
}
constexpr bool acked_caps_need_request(bool acked, bool inline_sink, std::uint8_t cc, bool legacy_only, bool sink_idle,
                                       bool prep_running, bool requested_now) {
    return acked_caps_blockers(acked, inline_sink, cc, legacy_only, sink_idle, prep_running, requested_now) == 0;
}

// Self-hold (k2/selfpower-pd-contract). A K2 that lives on the brick it tests (a key-mode boot, or the pin a warm reset
// inherited: `pin` != 0) cannot be PD-silent while attached: its Auto-CRC off meant the brick's Source_Capabilities were never
// GoodCRC'd, the source retried for 7-10 s and then went to PE_SRC_Disabled for the attach (OnePlus 120W, hardware 2026-11-06);
// reviving PD needs a detach or a Hard Reset, either one cuts VBUS and the K2 dies with it. So from the attach on the chip
// GoodCRCs, the caps get a plain 5 V Request (PDO 1) at a modest operating current within tSenderResponse, and the sink stays
// Ready answering re-announces. Never on a plain (HID-powered) boot, never with a sink inline (its answer), never in the
// PD-silent legacy session. A pin of 3 is no pin (Rd on both lines is the Debug Accessory signature).
inline constexpr std::uint16_t kSelfHoldMa = 1000;   // operating current (and max) of that Request: the K2 draws a few mA, the PDO's 3 A stays the source's
constexpr bool self_hold_wanted(std::uint8_t pin, bool inline_sink, bool legacy_only) {
    return ((pin & 3) == 1 || (pin & 3) == 2) && !inline_sink && !legacy_only;
}

// Rd is needed for the start when no sink is inline and VBUS is not up yet (unknown, -1, counts as down).
constexpr bool start_needs_rd(bool inline_sink, std::int32_t vbus_mv) {
    return !inline_sink && vbus_mv < kStartVbusMv;
}

// The START reads the cable's e-marker itself (VCONN from us, SOP' on CC) only on a dead line: VBUS below vSafe0V
// (unknown counts as down) and no source's Rp on the wire. An awake source hears that traffic, and the Lenovo C135
// answered it with a Hard Reset 180 ms after the Rd that followed (hardware 2026-11-03).
constexpr bool start_scan_wanted(std::int32_t vbus_mv, bool rp_on_line) {
    return vbus_mv < kVSafe0Mv && !rp_on_line;
}

// Pace of the START's blocking wait. service() (the PD engine's receive and answer) has to run about as often as the main
// loop does (1 ms + its own register reads): a source's SenderResponseTimer is 24-30 ms (Baseus 140W ~30 ms) and the old
// 20 ms delay + a ~30 ms two-register power read made every pass ~56 ms, so the Request left too late and the source sent
// a Hard Reset (Black Box 2026-11-04). What does not need that pace runs every kStartWaitSlowMs: the attach scan (a
// full CC measurement when nothing is attached yet) and the VBUS read (one register; none once VBUS is up, the wait
// has latched that).
inline constexpr std::uint32_t kStartWaitStepMs = 1;
inline constexpr std::uint32_t kStartWaitSlowMs = 20;
constexpr bool start_wait_slow_due(std::uint32_t now, std::uint32_t last_ms) { return now - last_ms >= kStartWaitSlowMs; }
constexpr bool start_wait_vbus_due(std::uint32_t now, std::uint32_t last_ms, std::int32_t vbus_mv) {
    return vbus_mv < kStartVbusMv && start_wait_slow_due(now, last_ms);
}

inline constexpr std::uint32_t kStartVbusAttachedWaitMs = 10000;  // a charger is attached (Rp seen) but still silent: C135 after a hard reset at the Rd, 2026-11-03

// Wait for the source after Rd went on: VBUS up (kStartVbusMv), then the Source_Capabilities (the run's confirmation
// proposal and its Request need them: caps arriving after the wait re-arm the confirm window, hardware 2026-11-03 run 6).
// Ready when VBUS is up and the caps are valid, or kStartCapsWaitMs after VBUS first came up (a source that does not
// speak PD). TimedOut after kStartVbusWaitMs without VBUS (the existing preflight then refuses with its own reason).
class VbusWait {
public:
    enum class State : std::uint8_t { Idle, Waiting, Ready, TimedOut };
    void begin(std::uint32_t now) { t0_ = now; up_ = false; caps_ = false; st_ = State::Waiting; }
    // `request_pending`: the sink engine is still negotiating the start's own Request. The wait then holds for up to
    // kStartContractWaitMs after the caps, so the run's preview, a refusal's release and the Runner never act on a sink
    // that is mid-exchange (one owner of the Request: hardware 2026-11-04, UGREEN X865, two Requests raced one Accept).
    State update(std::uint32_t now, std::int32_t vbus_mv, bool attached = false, bool caps_valid = false, bool request_pending = false) {
        if (st_ != State::Waiting) return st_;
        if (vbus_mv >= kStartVbusMv && !up_) { up_ = true; t_up_ = now; }
        if (up_ && caps_valid && !caps_) { caps_ = true; t_caps_ = now; }
        if (up_) {
            if (caps_valid) {
                if (!request_pending || static_cast<std::int32_t>(now - t_caps_) >= static_cast<std::int32_t>(kStartContractWaitMs)) st_ = State::Ready;
            } else if (static_cast<std::int32_t>(now - t_up_) >= static_cast<std::int32_t>(kStartCapsWaitMs)) st_ = State::Ready;
        } else if (static_cast<std::int32_t>(now - t0_) >= static_cast<std::int32_t>(attached ? kStartVbusAttachedWaitMs : kStartVbusWaitMs)) st_ = State::TimedOut;
        return st_;
    }
    State state() const { return st_; }
private:
    std::uint32_t t0_ = 0, t_up_ = 0, t_caps_ = 0;
    bool up_ = false, caps_ = false;
    State st_ = State::Idle;
};

}  // namespace k2::pd
