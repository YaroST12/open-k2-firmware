// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The one "PD present" verdict per attach (ROADMAP 2026-11-10, Row C): pure logic, no I/O, host-tested in
// tests/test_pd_verdict.cpp. pd::Link feeds it every UI tick; the Connection page gates its tabs on it and
// the remote state exposes it (kLinkNoPd).
//
// Inputs (all per attach, from the existing readers - nothing here drives CC):
//   - any PD message decoded this attach (Source_Caps, Request, GoodCRC, VDMs, control messages);
//   - the Source_Capabilities count of this attach;
//   - whether the sink side ever answered: "caps went unanswered" = Source_Capabilities repeated with no
//     GoodCRC and no Request (the sink stayed silent while the source re-sent its offers).
// No verdict before kPdVerdictSettleMs after the attach, so a normal negotiation never flashes a message.
#pragma once
#include <cstdint>

namespace k2::pd {

// No verdict before ~1.5 s after the attach.
inline constexpr std::uint32_t kPdVerdictSettleMs = 1500;
// "Repeated" Source_Capabilities with no answer: at least this many this attach means the source is
// shouting into the void (measured 44 on a replug), not announcing once at attach.
inline constexpr unsigned kPdUnansweredCapsMin = 3;

enum class PdVerdict : std::uint8_t {
    Pending,   // no attach, or the settle time has not passed yet
    Present,   // PD is talking on the link this attach
    None,      // settled and silent: no PD conversation this attach (see PdNoneWhy)
};
enum class PdNoneWhy : std::uint8_t {
    None,
    SinkSilentSourceOffered,   // caps repeated, the sink never answered (no GoodCRC, no Request)
    SourceNeverOffered,        // no Source_Capabilities at all: a plain Type-C source (Rp only)
};

struct PdVerdictIn {
    std::uint32_t now_ms = 0;
    std::uint32_t attach_at_ms = 0;
    bool attached = false;
    bool any_msg = false;         // any PD message decoded this attach (fusb302::pd_seen_attach)
    std::uint32_t caps_rx = 0;    // Source_Capabilities decoded this attach
    bool sink_answered = false;   // the sink role sent anything this attach (GoodCRC / Request / caps / VDMs)
};

struct PdVerdictOut {
    PdVerdict verdict = PdVerdict::Pending;
    PdNoneWhy why = PdNoneWhy::None;
};

// The rules the matrix in tests/test_pd_verdict.cpp drives:
//   - not attached, or inside the settle window: Pending (a normal negotiation never flashes a message);
//   - the sink answered anything this attach (GoodCRC / Request / its caps / VDMs): Present;
//   - Source_Capabilities repeated kPdUnansweredCapsMin times or more with no answer: None, the offers
//     went unanswered (the sink stayed silent while the source re-sent);
//   - any other PD message decoded this attach (a VDM, a control message, one or two caps): Present;
//   - otherwise the source never offered PD (a plain Type-C Rp-only port): None / SourceNeverOffered.
constexpr PdVerdictOut pd_verdict(const PdVerdictIn &in) {
    PdVerdictOut out;
    if (!in.attached || in.now_ms - in.attach_at_ms < kPdVerdictSettleMs) return out;   // Pending
    if (in.sink_answered) { out.verdict = PdVerdict::Present; return out; }
    if (in.caps_rx >= kPdUnansweredCapsMin) {
        out.verdict = PdVerdict::None;
        out.why = PdNoneWhy::SinkSilentSourceOffered;
        return out;
    }
    if (in.any_msg) { out.verdict = PdVerdict::Present; return out; }
    out.verdict = PdVerdict::None;
    out.why = PdNoneWhy::SourceNeverOffered;
    return out;
}

// Short token for the binlog (%s is cut at 12 bytes) and the remote: "pending" / "present" /
// "none-offered" (sink silent, source offered) / "none-never" (source never offered).
constexpr const char *pd_verdict_token(PdVerdict v, PdNoneWhy w) {
    if (v == PdVerdict::Present) return "present";
    if (v == PdVerdict::None) return w == PdNoneWhy::SinkSilentSourceOffered ? "none-offered" : "none-never";
    return "pending";
}

}  // namespace k2::pd
