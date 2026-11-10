// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The phase-1 lab hand-back (ROADMAP "An inline latch leaves the far sink on Type-C current: hand the contract
// back with a Hard Reset", phase 1 lab only). The pure rules are here so host tests cover the decision truth
// table (pd::handback_verdict, in k2/pd/pd_passive.hpp) and the sequence's step ORDER; src/core/handback.cpp binds
// the sequence to the volume (/rd.rec), the FUSB302 and the main loop.
//
// The sequence, in order (the order is the whole point): (1) wait until the comeback record /rd.rec is removed
// from the volume (the follower scrubs it on the inline latch), (2) drop the Rd, (3) send ONE Hard Reset through
// the e12dcee driver path (fusb302::sink_hard_reset(lab_inline=true), which returns whether it went out). A reset
// before the record is gone brings a self-powered K2 back with Rd from the first write and it fights the far sink
// (X1 in docs/CONNECTION_MATRIX.md). 60 s after the reset one [LINK] handback result line: the far sink's highest
// Request voltage seen, or "no Request: Type-C current".
#pragma once
#include <cstdint>

namespace k2::handback {

inline constexpr std::uint32_t kRecordWaitMs = 10000;   // the follower scrubs /rd.rec on the inline latch; past this the sequence gives up WITHOUT a reset
inline constexpr std::uint32_t kResultMs = 60000;       // the [LINK] handback result line comes this long after the reset

enum class Step : std::uint8_t { Idle, WaitRecord, Watching };
enum class Ev : std::uint8_t { None, RecordGone, ResetRefused, RecordTimeout, Result };

// The one hand-back sequence, as a pure step machine over its inputs (host-tested order). The hw calls run only in
// the step that owns them, in order: drop_rd() then send_reset(), and only once the record is confirmed gone.
struct Seq {
    Step step = Step::Idle;
    bool done = false;             // a hand-back was armed this attach (one per attach; the binder clears it on a new attach)
    std::uint32_t t0 = 0, reset_at = 0;
    std::uint16_t far_max_mv = 0;  // the far sink's highest Request voltage seen since arming
    std::uint32_t req_seen = 0;

    void arm(std::uint32_t now, std::uint32_t far_requests) {
        step = Step::WaitRecord; done = true; t0 = now; reset_at = 0; far_max_mv = 0; req_seen = far_requests;
    }
    // One poll. `record_present` = /rd.rec still on the volume; `far_requests` / `far_req_mv` = the engine's decoded
    // foreign-Request counter and the latest Request's voltage this poll.
    Ev poll(std::uint32_t now, bool record_present, std::uint32_t far_requests, std::uint16_t far_req_mv,
            void (*drop_rd)(), bool (*send_reset)()) {
        if (step == Step::WaitRecord) {
            if (record_present) {
                if (now - t0 > kRecordWaitMs) { step = Step::Idle; return Ev::RecordTimeout; }
                return Ev::None;
            }
            drop_rd();                                                  // (2) drop the Rd - the record is already gone (1)
            if (!send_reset()) { step = Step::Idle; return Ev::ResetRefused; }   // (3) the one Hard Reset
            step = Step::Watching; reset_at = now;
            return Ev::RecordGone;
        }
        if (step == Step::Watching) {
            if (far_requests != req_seen) { req_seen = far_requests; if (far_req_mv > far_max_mv) far_max_mv = far_req_mv; }
            if (now - reset_at >= kResultMs) { step = Step::Idle; return Ev::Result; }
        }
        return Ev::None;
    }
};

// Firmware binder (src/core/handback.cpp), polled from the main loop. Armed only by the remote
// `pd handback --confirm` (Cmd::PdHandback) after pd::handback_verdict allowed it.
void arm(std::uint32_t now);
void poll(std::uint32_t now);
bool done();   // a hand-back was already done this attach (a HandbackFacts input)

}  // namespace k2::handback
