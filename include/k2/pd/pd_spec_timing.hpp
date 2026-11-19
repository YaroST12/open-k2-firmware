// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB PD R3.2 timing and voltage-band figures the PD Stress scorer judges a source against. Pure constants. Limits are the specification's
// maximum (or the minimum for a "no earlier than"); the K2's own give-up times (kAcceptMs, kPsRdyMs, ...) are looser by design and live in
// pd_sink.hpp. Figures marked "derived" are the sum of spec figures, not a table row.
#pragma once

#include <cstdint>

#include "k2/pd/pd_sink.hpp"

namespace k2::pd::spec {

// Message handshakes (ms).
// tReceiverResponse: the responder's answer (Accept / Reject / Wait to a Request, PPS_Status to a Get_PPS_Status, ...) starts within this
// after the last bit of the GoodCRC that acknowledged the message (PD R3.3 Table 7.10, CTS COMMON.CHECK.PD.4#1;
// docs/PD_TYPEC_BC12_SPEC_REFERENCE.md B.1). tSenderResponse below is the ASKING side's give-up timer, not the source's limit.
inline constexpr std::uint32_t kReceiverResponseMs = 15;
inline constexpr std::uint32_t kSenderResponseMinMs = 27;     // tSenderResponse: the source's Accept / Reject / Wait after a Request
inline constexpr std::uint32_t kSenderResponseMaxMs = 50;
inline constexpr std::uint32_t kSinkRequestMs = 100;          // tSinkRequest: earliest re-Request after a Wait
// Request -> Accept as the K2 measures it, TX_START to the Accept's RX edge: tReceiverResponse plus what lies on the wire around it, at
// 300 kbit/s (3.33 us a bit, 4b5b): the Request (preamble 64 + SOP 20 + header 20 + one object 40 + CRC 40 + EOP 5 = 189 bits, 0.63 ms),
// the turnaround (tTransmit, under 0.2 ms), its GoodCRC (149 bits, 0.50 ms) and the Accept itself when the edge is its end (0.50 ms):
// 1.8 ms, taken as 2 ms (derived). Conservative: the allowance only ever widens the window by the frames' own length.
inline constexpr std::uint32_t kAcceptWireOverheadMs = 2;
// The PD Spec Check judges the Accept against tReceiverResponse; the charger diagnostics (LateAccept) and PD Stress keep the looser
// tSenderResponse bound (kAcceptLimitMs), a sink's own patience, so their verdicts do not move with this check.
inline constexpr std::uint32_t kAcceptSpecLimitMs = kReceiverResponseMs + kAcceptWireOverheadMs;
inline constexpr std::uint32_t kAcceptLimitMs = kSenderResponseMaxMs + kAcceptWireOverheadMs;
// PS_RDY after a programmable step, measured from the Accept's RX edge to the PS_RDY's RX edge. The K2 stamps a message when it has
// been received and GoodCRC'd, so the PS_RDY reading carries what lies on the wire after the source started it: the PS_RDY frame
// (preamble 64 + SOP 20 + header 20 + CRC 40 + EOP 5 = 149 bits, 0.50 ms at 300 kbit/s), the K2's turnaround (tTransmit, under 0.2 ms)
// and its GoodCRC (149 bits, 0.50 ms): 1.2 ms, taken as the same 2 ms as the Accept (derived). The start edge (the Accept's) is under
// 1 ms before the GoodCRC that starts the source's timer, so the reading is still at least the source's own time. VBUS in the band is
// a sample of the meter, not a message: the band check keeps the bare tPpsSrcTransSmall / tPpsSrcTransLarge (and the AVS ones).
inline constexpr std::uint32_t kPsRdyWireOverheadMs = kAcceptWireOverheadMs;
// Get_PPS_Status -> PPS_Status the same way (a control message, its GoodCRC, then the extended answer: under 2 ms of frames, derived).
inline constexpr std::uint32_t kPpsStatusLimitMs = kReceiverResponseMs + kAcceptWireOverheadMs;

// Power transition.
inline constexpr std::uint32_t kSrcTransitionMinMs = 25;      // tSrcTransition: Accept to the source starting to move VBUS
inline constexpr std::uint32_t kSrcTransitionMaxMs = 35;
inline constexpr std::uint32_t kPsTransitionMinMs = 450;      // tPSTransition (SPR): the source's PS_RDY after Accept, at the latest
inline constexpr std::uint32_t kPsTransitionMaxMs = 550;
inline constexpr std::uint32_t kPsTransitionEprMinMs = 830;   // tPSTransition in EPR mode: 830-1020 ms (the 2026-11-11
inline constexpr std::uint32_t kPsTransitionEprMaxMs = 1020;  // renegotiation research, runs/2026-11-11-renegotiation-research)
inline constexpr std::uint32_t kSrcTransReqSprMs = 325;       // tSrcTransReq: a voltage change settled after the Request, SPR
inline constexpr std::uint32_t kSrcTransReqEprMs = 760;       // EPR
inline constexpr std::uint32_t kSrcSettleSprMs = 275;         // tSrcSettle: VBUS settled after PS_RDY, SPR
inline constexpr std::uint32_t kSrcSettleEprMs = 700;         // EPR

// Programmable steps: the source's transition window depends on the size of the step (small / large boundary below).
inline constexpr std::uint32_t kPpsStepMv = 20;               // PPS output voltage resolution
inline constexpr std::uint32_t kPpsSmallStepMv = 500;         // PPS: a step up to this is "small": kPpsSmallStepMs, bigger kPpsLargeStepMs
inline constexpr std::uint32_t kPpsSmallStepMs = 25;
inline constexpr std::uint32_t kPpsLargeStepMs = 275;
inline constexpr std::uint32_t kAvsStepMv = 100;              // AVS resolution
inline constexpr std::uint32_t kAvsSmallStepMv = 1000;        // AVS: a step up to this is "small": kAvsSmallStepMs, bigger kAvsLargeStepMs
inline constexpr std::uint32_t kAvsSmallStepMs = 50;          // tAvsSrcTransSmall, VBUS in vAvsNew and PS_RDY (PD R3.3 4.5.1.2)
// A large AVS step is an ordinary transition in PD R3.3: tSrcTransition + tSrcReady (35 + 285 ms) for the level, tSrcTransReq (325 ms) for
// PS_RDY; the check takes 325 ms for both (derived, docs/PD_PPS_AVS_SPEC_REFERENCE.md section 4).
inline constexpr std::uint32_t kAvsLargeStepMs = 325;
// The same in EPR mode (an EPR AVS APDO): tSrcTransReq EPR (760 ms) for both, over tSrcTransition + tSrcReady EPR (35 + 720 ms) (derived).
inline constexpr std::uint32_t kAvsLargeStepEprMs = kSrcTransReqEprMs;
// The band beyond vPpsNew / vAvsNew a step may overshoot into, and how long VBUS may stay between the two (no load: tPpsTransient's
// under-60 mA row; tAvsTransient has a single 5 ms row).
inline constexpr std::int32_t kPpsValidExtraMv = 100;        // vPpsValid
inline constexpr std::uint32_t kPpsTransientMs = 150;        // tPpsTransient, load < 60 mA
inline constexpr std::int32_t kAvsValidExtraMv = 500;        // vAvsValid
inline constexpr std::uint32_t kAvsTransientMs = 5;          // tAvsTransient
inline constexpr std::uint32_t kAvsNewPermille = 50;         // vAvsNew, +-5 %
// PPS_Status at no load (PD CTS SRC3.7#2): Output Voltage within +-3 % of the request (rounded to 20 mV) or 0xFFFF.
inline constexpr std::uint32_t kPpsStatusVoltPermille = 30;

// VBUS band after PS_RDY: vSrcNew is the nominal +-5 %, vSrcValid a further +-0.5 V while settling.
inline constexpr std::uint32_t kVSrcNewPermille = 50;
inline constexpr std::uint32_t kVSrcValidExtraMv = 500;
constexpr std::uint32_t band_mv(std::uint32_t target_mv) { return target_mv * kVSrcNewPermille / 1000; }

// Hard Reset recovery as seen from the sink: tPSHardReset (about 25-35 ms) + the source's 660 ms off time + tSrcRecover + tSrcTurnOn
// up to a valid 5 V: about 1.96 s to a usable port, 2.2 s with the K2's own re-request (derived).
// Reference values: nothing reads these two yet (the engines time their own recovery waits); kept as the documented budget a
// recovery timeout is judged against, not removed, so a change in flight that wants one does not break.
inline constexpr std::uint32_t kHardResetRecoveryMs = 1960;
inline constexpr std::uint32_t kHardResetRecoveryMaxMs = 2200;

// ---- PD Spec Check (k2/pdspec, docs/PD_SPEC_CHECK.md) ----------------------------------------------------------------------------
// The figures below come from the 2026-11-11 renegotiation research (the PD R3.2 tables, named by symbol). The reference point of each
// one is stated, because the check measures from the K2's own edges: the Request's TX start, the Accept's and the PS_RDY's RX edge.
//
// PS_RDY after the Accept (from the Accept's GoodCRC): tSrcTransReq is the SOURCE's limit (kSrcTransReqSprMs / kSrcTransReqEprMs above);
// tPSTransition is the SINK's give-up timer and is wider. The check judges PS_RDY against tSrcTransReq.
// VBUS inside vSrcNew after the Accept: the transition may start no earlier than tSrcTransition (kSrcTransitionMinMs) and must be
// settled tSrcSettle after it started, so tSrcTransition max + tSrcSettle after the Accept (derived).
inline constexpr std::uint32_t kInBandSprMs = kSrcTransitionMaxMs + kSrcSettleSprMs;   // 310 ms (derived)
inline constexpr std::uint32_t kInBandEprMs = kSrcTransitionMaxMs + kSrcSettleEprMs;   // 735 ms (derived)
// PPS output: vPpsNew is the requested voltage +-5 % (same permille as vSrcNew). VBUS inside vPpsNew and PS_RDY sent, both within
// tPpsSrcTransSmall / tPpsSrcTransLarge (kPpsSmallStepMs / kPpsLargeStepMs) of the last bit of the GoodCRC that acknowledged the Accept
// (PD R3.3 4.5.2.1, SPT 7.1.4.2.1#11/#12, CTS PS.SRC.2#9/#10). The K2 times from the Accept's RX edge, under 1 ms earlier than that
// GoodCRC's end: the measured time is a little longer than the source's, so the check is conservative.
inline constexpr std::uint32_t kPpsNewPermille = 50;
// Hard Reset, source side: tPSHardReset after the signalling the source starts to remove VBUS, VBUS reaches vSafe0V within tSafe0V,
// stays off for tSrcRecover, is back at vSafe5V within tSrcTurnOn, and the first Source_Capabilities follow within tFirstSourceCap.
inline constexpr std::uint32_t kPsHardResetMaxMs = 35;        // tPSHardReset 25-35 ms
inline constexpr std::uint32_t kSafe0VMs = 650;               // tSafe0V
inline constexpr std::int32_t kVSafe0VMaxMv = 800;            // vSafe0V upper bound
inline constexpr std::int32_t kVSafe5VMinMv = 4750;           // vSafe5V lower bound
inline constexpr std::uint32_t kSrcRecoverMinMs = 660;        // tSrcRecover, SPR
inline constexpr std::uint32_t kSrcRecoverMaxMs = 1000;
inline constexpr std::uint32_t kSrcRecoverEprMinMs = 1085;    // tSrcRecover when the reset leaves EPR mode
inline constexpr std::uint32_t kSrcRecoverEprMaxMs = 1425;
inline constexpr std::uint32_t kSrcTurnOnMs = 275;            // tSrcTurnOn
inline constexpr std::uint32_t kFirstSourceCapMs = 250;       // tFirstSourceCap
// A sink detach (Rd removed): the source sees SRC.Open within tSRCDisconnect (0-20 ms) and VBUS reaches vSafe0V within tVBUSOFF
// (0-650 ms) (Type-C R2.5 Tables 4-34 / 4-32; docs/PD_TYPEC_BC12_SPEC_REFERENCE.md). The check allows both: 670 ms.
inline constexpr std::uint32_t kVbusOffMs = 650;
inline constexpr std::uint32_t kSrcDisconnectMaxMs = 20;
// Rd back: the source debounces it (tCCDebounce 100-200 ms), has its bulk capacitor ready (tBulkDischarge, 275 ms after a level up to
// 21 V, 700 ms above), and VBUS reaches vSafe5V min within tVBUSON (275 ms) of entering Attached.SRC; the first Source_Capabilities
// follow within tFirstSourceCap (above) of vSafe5V (Type-C R2.5 Tables 4-32 / 4-34).
inline constexpr std::uint32_t kCcDebounceMaxMs = 200;
inline constexpr std::uint32_t kBulkDischargeMs = 275;
inline constexpr std::uint32_t kBulkDischargeHvMs = 700;
inline constexpr std::int32_t kBulkDischargeHvAboveMv = 21000;
inline constexpr std::uint32_t kVbusOnMs = 275;
// Keep-alives: in a PPS contract the sink re-requests within tPPSRequest (10 s); the source's timer is tPPSTimeout (12-15 s) and it
// answers the silence with a Hard Reset (that last part is unverified in the research: a source that never resets is reported).
// In EPR mode the sink sends EPR_KeepAlive every tSinkEPRKeepAlive (250-500 ms); the source's timer is tSourceEPRKeepAlive (750-1000 ms).
inline constexpr std::uint32_t kPpsTimeoutMinMs = 12000;
inline constexpr std::uint32_t kPpsTimeoutMaxMs = 15000;
inline constexpr std::uint32_t kSourceEprKeepAliveMinMs = 750;
inline constexpr std::uint32_t kSourceEprKeepAliveMaxMs = 1000;

static_assert(kAcceptMs > kSenderResponseMaxMs, "the K2's own Accept timeout must be looser than the spec's tSenderResponse");
static_assert(kPsRdyMs >= kPsTransitionMaxMs, "the K2's own PS_RDY timeout must be looser than tPSTransition");
static_assert(kSenderResponseMinMs <= kSenderResponseMaxMs, "tSenderResponse range");

}  // namespace k2::pd::spec
