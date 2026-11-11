// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// "Identify device" sequencer, pure logic (host-tested in tests/test_pd_identify.cpp).
// With the CC line idle it asks the port partner, one message at a time:
//   Discover Identity, Discover SVIDs, Discover Modes (first kIdModeSvids SVIDs), Get_Sink_Cap,
//   Get_Status and Get_Revision (the last two PD 3.x only).
// A step is retried (up to kMaxAttempts, backing off kBackoffMs * attempt) when the partner stays
// silent, answers BUSY or the transmit fails (a collision on the wire); NAK / Not_Supported /
// Reject skip the step. Answers are decoded elsewhere (the engine's VdmInfo); this class only
// tracks the progress and keeps the Sink_Cap and Revision words.
// The driver owns the wire: it feeds frames and tx results in and sends whatever take_tx() returns.
#pragma once
#include <array>
#include <cstdint>
#include <span>

#include "k2/pd/pd_sink.hpp"

namespace k2::pd {

inline constexpr std::uint8_t kIdModeSvids = 2;
inline constexpr std::uint8_t kIdMaxAttempts = 3;
inline constexpr std::uint32_t kIdRespMs = 300, kIdBackoffMs = 40;

enum class IdStep : std::uint8_t { Identity, Svids, Modes, SinkCap, Status, Revision, Done };
enum class IdState : std::uint8_t { Idle, Running, Done, Aborted };
// Why a running identify was cut short: Other = the driver did not say (shown as "link dropped").
enum class IdWhy : std::uint8_t { None, Other, Detached, HardReset, Stopped };

// Structured VDM header: SVID, structured bit 15, VDM version 2.0 (PD 3.x) in bits 14:13, command.
constexpr std::uint32_t vdm_header(std::uint16_t svid, std::uint8_t cmd, std::uint8_t rev) {
    return (std::uint32_t(svid) << 16) | 0x8000u | (rev >= 2 ? 0x2000u : 0u) | cmd;
}

struct IdStatus {
    IdState state = IdState::Idle;
    IdStep step = IdStep::Identity;
    std::uint8_t ok_mask = 0;       // bit per IdStep that was answered
    std::uint8_t skipped_mask = 0;  // bit per IdStep that was refused or gave up
    std::uint8_t retries = 0;       // total retries so far (silence, BUSY, tx failure)
    std::uint8_t num_sink_pdos = 0;
    std::array<std::uint32_t, 7> sink_pdos{};
    std::uint32_t revision = 0;     // Revision message data object (0 = none)
    IdWhy why = IdWhy::None;        // set with state Aborted
    // The first mode VDO of each Discover Modes ACK (at most kIdModeSvids): DisplayPort capabilities, Thunderbolt mode ...
    std::uint8_t num_modes = 0;
    std::array<std::uint16_t, kIdModeSvids> mode_svid{};
    std::array<std::uint32_t, kIdModeSvids> mode_vdo{};
};

// The SVID Discover Modes asks about at position `idx` (0 = first): DisplayPort (0xFF01) and Thunderbolt (0x8087) when the
// partner listed them, then the rest in list order. 0 when there is none at that position.
std::uint16_t id_modes_svid(std::span<const std::uint16_t> svids, unsigned idx);

const char* id_step_name(IdStep s);
const char* id_step_short(IdStep s);   // for the result: "SVIDs", "Sink caps"

// Precheck before transmitting: nothing on the CC lines (no Rp, under kIdRpMinMv) and VBUS under
// kIdVbusMinMv means there is no device to ask, and every try would burn its retries into silence.
// Unknown readings (-1) never block: a missing measurement is not proof.
inline constexpr std::int32_t kIdRpMinMv = 200, kIdVbusMinMv = 3000;
constexpr bool identify_no_device(std::int32_t cc1_mv, std::int32_t cc2_mv, std::int32_t vbus_mv) {
    const bool cc_idle = cc1_mv >= 0 && cc2_mv >= 0 && cc1_mv < kIdRpMinMv && cc2_mv < kIdRpMinMv;
    return cc_idle && vbus_mv >= 0 && vbus_mv < kIdVbusMinMv;
}

class Identify {
public:
    // rev = wire spec revision of the partner (1 = PD 2.0, 2 = PD 3.x).
    void start(std::uint8_t rev, std::uint32_t now);
    // Only a running identify can be aborted: with nothing running (a detach, a hard reset) the state stays Idle / Done.
    void abort(IdWhy why = IdWhy::Other) {
        if (st_.state == IdState::Running) { st_.state = IdState::Aborted; st_.why = why; }
        tx_pending_ = false; awaiting_ = false;
    }
    bool running() const { return st_.state == IdState::Running; }

    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_tx_result(bool ok, std::uint32_t now);
    // `svids` = SVIDs found so far (the engine's VdmInfo); `cc_idle` = nothing on the wire right now.
    void tick(std::uint32_t now, bool cc_idle, std::span<const std::uint16_t> svids);
    bool take_tx(TxMsg& out);
    const IdStatus& status() const { return st_; }

private:
    void advance();
    void attempt_failed(std::uint32_t now);
    void step_ok();
    void step_skipped();
    bool build(std::span<const std::uint16_t> svids);

    IdStatus st_{};
    std::uint8_t rev_ = 2;
    std::uint8_t mode_idx_ = 0;
    std::uint8_t attempts_ = 0;
    std::uint16_t cur_svid_ = 0;
    std::uint32_t deadline_ = 0, next_at_ = 0;
    bool awaiting_ = false, tx_pending_ = false, tx_sent_ = false;
    TxMsg tx_{};
};

}  // namespace k2::pd
