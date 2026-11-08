// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One-click auto-detect: tries each protocol in a fixed order, reports whether the
// charger accepted it and the voltage reached, and returns the output to 5 V after every step.
// Pure state machine over the Engine; it never asks for more than 9 V (SCP and VOOC stay at 5 V,
// QC3/QC3+ stop at 6 V). The PD step belongs to the PD reader and is not part of this scan.
#pragma once
#include <array>
#include <cstdint>
#include <span>

#include "k2/fastcharge/fc_engine.hpp"

namespace k2::fc {

enum class Outcome : std::uint8_t { None, Accepted, Rejected, Skipped };
const char* name(Outcome o);

struct ScanResult {
    Proto proto;
    Outcome outcome;
    int mv;           // VBUS reached while the protocol was active (0 when not accepted)
    Fail reason = Fail::None; // refusal/failure, not evidence that a protocol is unsupported
};

inline constexpr std::size_t kScanSteps = 7;
inline constexpr std::array<Proto, kScanSteps> kScanOrder = {Proto::Fcp, Proto::Scp, Proto::Afc,
                                                              Proto::Qc2, Proto::Qc3, Proto::Vooc, Proto::Qc3Plus};
inline constexpr int kScanMaxMv = 9000;      // highest voltage any step asks for
inline constexpr int kScanQc3Mv = 6000;      // proves the continuous QC3 mode without going to 9 V
inline constexpr int kScanMinSourceMv = 4000;

int scan_target_mv(Proto p);

class Scanner {
public:
    Scanner(Engine& e, Env& env) : eng_(e), env_(env) {}

    // Needs `confirmed` (the steps reach 9 V), a VBUS ceiling and >= 4 V on VBUS.
    // `allow_inline` overrides the PD-contract / load guard once for the whole scan.
    Fail start(bool confirmed, bool allow_inline = false);
    void abort();      // stop now: reset to 5 V
    void poll();       // after Engine::poll()

    bool running() const { return phase_ != Phase::Off; }
    bool done() const { return done_; }
    Fail fail() const { return fail_; }
    std::size_t step() const { return step_; }   // index into kScanOrder of the running / next step
    std::span<const ScanResult> results() const { return {res_.data(), done_n_}; }

private:
    enum class Phase : std::uint8_t { Off, Running, Hold, Return };
    void begin_step();
    void finish_step(Outcome o, int mv, Fail reason = Fail::None);

    Engine& eng_;
    Env& env_;
    Phase phase_ = Phase::Off;
    bool done_ = false;
    bool allow_inline_ = false;
    Fail fail_ = Fail::None;
    std::size_t step_ = 0, done_n_ = 0;
    std::array<ScanResult, kScanSteps> res_{};
    std::uint32_t t_ms_ = 0, step_ms_ = 0;
};

}  // namespace k2::fc
