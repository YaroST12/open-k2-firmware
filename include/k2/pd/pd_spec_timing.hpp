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
inline constexpr std::uint32_t kReceiverResponseMs = 15;      // tReceiverResponse: GoodCRC after a message
inline constexpr std::uint32_t kSenderResponseMinMs = 27;     // tSenderResponse: the source's Accept / Reject / Wait after a Request
inline constexpr std::uint32_t kSenderResponseMaxMs = 50;
inline constexpr std::uint32_t kSinkRequestMs = 100;          // tSinkRequest: earliest re-Request after a Wait
// Request -> Accept as the K2 measures it, TX_START to the Accept's RX edge: the limit above plus the Request frame, its GoodCRC and the
// Accept frame on the wire (about 1.5-2 ms at 300 kbit/s, inferred). The scorer applies it, not the bare 50 ms.
inline constexpr std::uint32_t kAcceptWireOverheadMs = 2;
inline constexpr std::uint32_t kAcceptLimitMs = kSenderResponseMaxMs + kAcceptWireOverheadMs;

// Power transition.
inline constexpr std::uint32_t kSrcTransitionMinMs = 25;      // tSrcTransition: Accept to the source starting to move VBUS
inline constexpr std::uint32_t kSrcTransitionMaxMs = 35;
inline constexpr std::uint32_t kPsTransitionMinMs = 450;      // tPSTransition (SPR): the source's PS_RDY after Accept, at the latest
inline constexpr std::uint32_t kPsTransitionMaxMs = 550;
inline constexpr std::uint32_t kPsTransitionEprMaxMs = 920;   // tPSTransition in EPR mode (inferred)
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
inline constexpr std::uint32_t kAvsSmallStepMs = 50;
inline constexpr std::uint32_t kAvsLargeStepMs = 700;

// VBUS band after PS_RDY: vSrcNew is the nominal +-5 %, vSrcValid a further +-0.5 V while settling.
inline constexpr std::uint32_t kVSrcNewPermille = 50;
inline constexpr std::uint32_t kVSrcValidExtraMv = 500;
constexpr std::uint32_t band_mv(std::uint32_t target_mv) { return target_mv * kVSrcNewPermille / 1000; }

// Hard Reset recovery as seen from the sink: tPSHardReset (about 25-35 ms) + the source's 660 ms off time + tSrcRecover + tSrcTurnOn
// up to a valid 5 V: about 1.96 s to a usable port, 2.2 s with the K2's own re-request (derived).
inline constexpr std::uint32_t kHardResetRecoveryMs = 1960;
inline constexpr std::uint32_t kHardResetRecoveryMaxMs = 2200;

static_assert(kAcceptMs > kSenderResponseMaxMs, "the K2's own Accept timeout must be looser than the spec's tSenderResponse");
static_assert(kPsRdyMs >= kPsTransitionMaxMs, "the K2's own PS_RDY timeout must be looser than tPSTransition");
static_assert(kSenderResponseMinMs <= kSenderResponseMaxMs, "tSenderResponse range");

}  // namespace k2::pd::spec
