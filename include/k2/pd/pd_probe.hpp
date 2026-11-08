// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Quick source probe for the charger report card, pure logic (host-tested in tests/test_pd_probe.cpp).
// With the source in a 5 V contract it asks, on SOP and one message at a time: Discover Identity (VDM),
// Get_Manufacturer_Info (extended, PD 3.x only) and Get_Source_Cap_Extended (control, PD 3.x only).
// Unlike pd::Identify it never retries: silence, NAK / BUSY / Reject / Not_Supported and a failed
// transmit all skip the step, each step waits at most kProbeRespMs and the whole probe ends by
// kProbeTotalMs, so a charger that ignores it costs the run under one second. Answers are decoded by the
// engine (VdmInfo, ExtInfo); this class tracks progress, and probe_result() merges what was answered.
#pragma once
#include <array>
#include <cstdint>
#include <span>

#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_sink.hpp"

namespace k2::pd {

inline constexpr std::uint32_t kProbeRespMs = 250, kProbeTotalMs = 900;
inline constexpr std::uint8_t kExtGetManufacturerInfo = 0x06, kCtlGetSourceCapExt = 17;

enum class ProbeStep : std::uint8_t { Identity, MfrInfo, CapsExt, Done };
enum class ProbeState : std::uint8_t { Idle, Running, Done, Aborted };

struct ProbeStatus {
    ProbeState state = ProbeState::Idle;
    ProbeStep step = ProbeStep::Identity;
    std::uint8_t ok_mask = 0;       // bit per ProbeStep that was answered
    std::uint8_t skipped_mask = 0;  // bit per ProbeStep that was refused, ignored or never asked
};
constexpr std::uint8_t probe_bit(ProbeStep s) { return std::uint8_t(1u << unsigned(s)); }

class Probe {
public:
    // rev = wire spec revision of the source (1 = PD 2.0: no extended messages, only Discover Identity).
    void start(std::uint8_t rev, std::uint32_t now);
    void abort() { if (st_.state == ProbeState::Running) st_.state = ProbeState::Aborted; tx_pending_ = awaiting_ = false; }
    bool running() const { return st_.state == ProbeState::Running; }
    bool finished() const { return st_.state == ProbeState::Done || st_.state == ProbeState::Aborted; }

    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_tx_result(bool ok, std::uint32_t now);
    void tick(std::uint32_t now, bool cc_idle);
    bool take_tx(TxMsg& out);
    const ProbeStatus& status() const { return st_; }

private:
    void step_done(bool ok);
    void build();
    ProbeStatus st_{};
    std::uint8_t rev_ = 2;
    std::uint32_t started_ = 0, deadline_ = 0;
    bool awaiting_ = false, tx_pending_ = false, tx_sent_ = false;
    TxMsg tx_{};
};

// What the probe learned about the charger, merged from the engine's decoded answers.
struct ProbeResult {
    std::uint16_t vid = 0, pid = 0;
    std::uint8_t pdp_w = 0;     // Source_Cap_Extended PDP, else the largest PDO power (0 = unknown)
    char mfr[13] = {};          // Manufacturer_Info name, first 12 characters, printable ASCII only
};
// A Discover Identity answer counts whenever it is valid (the probe or a passive one); Manufacturer_Info and
// Source_Cap_Extended only when the probe got their answer. VID/PID priority: Identity, Mfr info, Cap_Ext.
ProbeResult probe_result(const ProbeStatus&, const VdmInfo&, const ExtInfo&, const SourceCaps&);

}  // namespace k2::pd
