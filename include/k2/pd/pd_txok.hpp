// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// SinkTxOK: in a PD 3.x contract a source that wants to keep its own message sequence clear of a sink's advertises "SinkTxOK" as Rp 3 A on CC
// and "SinkTxNG" as Rp 1.5 A; the sink starts a message sequence of its own (Request, Get_Source_Cap, ...) only while it sees SinkTxOK.
// Answers to the source's messages are not sink-initiated and are never held. Pure logic over (level, time) samples; the FUSB302 driver
// feeds the BC_LVL reading it already makes every poll, so the rule costs no extra bus traffic.
//
// Fail open, always: the wait is bounded (kTxOkWaitMs), a reading older than kTxOkFreshMs is ignored, and a source that has never been seen at
// 3 A since attach (a plain 1.5 A Rp, not a SinkTxOK signaller) is never waited on. Safety paths (release / abort, Hard Reset) do not ask the
// gate at all; the EPR keep-alive and Soft_Reset do not either (inferred: a tight timer and error recovery, both allowed at any time).
#pragma once

#include <cstdint>

namespace k2::pd {

inline constexpr std::uint8_t kRpDefault = 1;   // BC_LVL: default USB current
inline constexpr std::uint8_t kRpTxNg = 2;      // BC_LVL: 1.5 A, SinkTxNG
inline constexpr std::uint8_t kRpTxOk = 3;      // BC_LVL: 3 A, SinkTxOK
inline constexpr std::uint32_t kTxOkFreshMs = 60;   // the driver samples every 20 ms; an older reading is no reading
inline constexpr std::uint32_t kTxOkWaitMs = 50;    // longest a sink-initiated message waits for SinkTxOK before going anyway

class TxOkGate {
public:
    // The source's Rp level (STATUS0 BC_LVL, 0..3) read at `now`.
    void feed(std::uint8_t level, std::uint32_t now) {
        level_ = level;
        at_ = now;
        have_ = true;
        if (level == kRpTxOk) ok_seen_ = true;
        if (level != kRpTxNg) end_episode();
    }

    // true = hold the sink-initiated message back (the caller asks again on its next tick); false = send it. `rev` is the wire revision
    // in use (2 = PD 3.x). Asking again keeps the same episode; the bound runs from the first ask.
    bool hold(std::uint8_t rev, std::uint32_t now) {
        if (rev < 2) return false;
        if (!have_ || now - at_ > kTxOkFreshMs || level_ != kRpTxNg || !ok_seen_) { end_episode(); return false; }
        if (holding_ && now - last_ask_ > 2 * kTxOkWaitMs) end_episode();   // nobody asked for a while: this is a new message
        last_ask_ = now;
        if (!holding_) { holding_ = true; overridden_ = false; hold_at_ = now; ++waits_; }
        if (overridden_) return false;
        if (now - hold_at_ >= kTxOkWaitMs) { overridden_ = true; ++overrides_; return false; }
        return true;
    }

    void sent() { end_episode(); }   // the held message went out: the next one starts a new episode
    void reset() { *this = TxOkGate{}; }   // detach: forget the reading and that 3 A was ever seen

    std::uint32_t waits() const { return waits_; }          // episodes that held a message back
    std::uint32_t overrides() const { return overrides_; }  // of those, ones that hit the bound and went anyway
    std::uint8_t level() const { return level_; }
    bool seen_ok() const { return ok_seen_; }

private:
    void end_episode() { holding_ = false; overridden_ = false; }

    std::uint32_t at_ = 0, hold_at_ = 0, last_ask_ = 0;
    std::uint32_t waits_ = 0, overrides_ = 0;
    std::uint8_t level_ = 0;
    bool have_ = false, ok_seen_ = false, holding_ = false, overridden_ = false;
};

}  // namespace k2::pd
