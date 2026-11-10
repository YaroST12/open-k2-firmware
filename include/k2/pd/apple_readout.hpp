// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab-only Apple charger readout (SVID 0x05AC), pure logic (host-tested in tests/test_apple_readout.cpp).
//
// An Apple USB-C charger runs its own VDM discovery about 1.4 s after every Request: Discover Identity /
// Discover SVIDs, and when OUR SVIDs ACK lists 0x05AC it sends Discover Modes 0x05AC, Enter_Mode and the
// unstructured 05AC0005/05AC0015 handshake, then waits while the sink reads its register file with
// Attention + 05ACnn02 + addr, answered by an unstructured 05ACnn02 + addr + nn data words, acked with
// 05ACnn12 (sniffed inline, K2 passive, reverse-engineered protocol; the string addresses come from the
// directory at 0x04..0x24, one word per entry = idx<<24 | len<<16 | addr, never hard-coded).
//
// The op is LAB-ONLY and OFF BY DEFAULT: while unarmed our Discover SVIDs ACK stays exactly [0x2717] (the
// Xiaomi chain on CUKTECH is untouched). Armed by the remote `apple-read` op only, refused inline, without
// a contract, or when the source is not the K2's own partner. The readout never asks for a different
// voltage: arming triggers a re-request of the current contract (the existing Get_Source_Cap path).
//
// Privacy: the serial is a result field only - it never goes into a binlog %s (the cut is 12 bytes
// anyway); the log says serial len=%d.
#pragma once
#include <array>
#include <cstdint>

namespace k2::pd {

// Standards-assigned vendor ID used as the protocol SVID.
inline constexpr std::uint16_t kAppleSvid = 0x05AC;

// The VDM words of the sniffed exchange (as on the wire, little endian per word).
inline constexpr std::uint32_t kAppleDiscoverModesReq = 0x05AC8003;  // structured REQ, cmd 3
inline constexpr std::uint32_t kAppleDiscoverModesAck = 0x05AC8043;  // structured ACK, cmd 3
inline constexpr std::uint32_t kAppleModesVdo        = 0x00000002;   // the one mode we ACK
inline constexpr std::uint32_t kAppleEnterModeReq    = 0x05AC8104;   // structured REQ, cmd 4
inline constexpr std::uint32_t kAppleEnterModeAck    = 0x05AC8144;   // structured ACK, cmd 4
inline constexpr std::uint32_t kAppleAttention       = 0x05AC8106;   // structured REQ, cmd 6 (Attention)
inline constexpr std::uint32_t kAppleHandshakeReq    = 0x05AC0005;   // unstructured, charger -> sink
inline constexpr std::uint32_t kAppleHandshakeAck    = 0x05AC0015;   // unstructured, sink -> charger
// A read of nn words (01..05 as sniffed): the request/answer object 05ACnn02 and the ack 05ACnn12.
constexpr std::uint32_t apple_read_obj(std::uint8_t nn) { return 0x05AC0000u | (std::uint32_t(nn) << 8) | 0x02u; }
constexpr std::uint32_t apple_read_ack(std::uint8_t nn) { return 0x05AC0000u | (std::uint32_t(nn) << 8) | 0x12u; }

// Register file: the directory at 0x04..0x24, one word per entry. Bounds of one run:
inline constexpr std::uint32_t kAppleDirFirst = 0x04, kAppleDirLast = 0x24, kAppleDirStep = 4;
inline constexpr unsigned kAppleDirEntries = unsigned((kAppleDirLast - kAppleDirFirst) / kAppleDirStep) + 1;  // 9
inline constexpr std::uint8_t kAppleMaxReads = 16;        // at most 16 reads per run
inline constexpr std::uint32_t kAppleReadPeriodMs = 20;   // one read per ~20 ms, as sniffed
inline constexpr std::uint32_t kAppleReadTimeoutMs = 50;  // per-read timeout
inline constexpr std::uint8_t kAppleReadRetries = 1;      // one retry per read
inline constexpr std::uint8_t kAppleMaxString = 24;       // strings at most 24 B
inline constexpr std::uint8_t kAppleMaxWordsPerRead = 5;  // nn <= 05, as sniffed
inline constexpr std::uint32_t kAppleRegWindow = 0x100;   // bank 0 window; entries outside are refused
inline constexpr std::uint32_t kAppleArmWaitMs = 3000;     // the source drives discovery ~1.4 s after a Request
inline constexpr std::uint32_t kAppleEnterWaitMs = 1000;   // after Enter_Mode: wait this long for 05AC0005

// Why a run stopped. The names are short tokens (the binlog cuts %s at 12 bytes).
enum class AppleWhy : std::uint8_t { Running, Done, Nak, Timeout, Range, Cap, NoMode };
const char* apple_why_name(AppleWhy w);   // "running" "done" "nak" "timeout" "range" "cap" "nomode"

inline constexpr std::size_t kAppleStr = kAppleMaxString;   // max string bytes, without the NUL
struct AppleInfo {
    bool valid = false;    // a readout ran this attach (even a partial one); cleared on detach
    AppleWhy why = AppleWhy::Running;
    std::uint8_t reads = 0, ok = 0;
    std::array<char, kAppleStr + 1> maker{}, product{}, fw{}, hw{}, field90{}, serial{};
};

// The arm gate for the lab op, one for the remote: refused while a run owns the link, inline (we never
// transmit inline), when the source is not our partner, or without a contract.
enum class AppleRefusal : std::uint8_t { None, Inline, NoContract, NotPartner, Busy };
const char* apple_refusal_name(AppleRefusal r);   // "none" "inline" "no contract" "not partner" "busy"
struct AppleFacts {
    bool charger_test_busy;   // a charger-test run owns the link
    bool inline_mode;         // a sink sits behind the K2 (the inline latch)
    bool trigger_refusal;     // can_trigger() != None (a sink Rd or a foreign Request on the wire)
    bool contract;            // the sink holds a contract (Ready / held)
    bool answers_sop;         // the source is the K2's own partner (the sink answers its SOP messages)
};
constexpr AppleRefusal apple_read_refusal(const AppleFacts& f) {
    if (f.charger_test_busy) return AppleRefusal::Busy;
    if (f.inline_mode || f.trigger_refusal) return AppleRefusal::Inline;
    if (!f.answers_sop) return AppleRefusal::NotPartner;
    if (!f.contract) return AppleRefusal::NoContract;
    return AppleRefusal::None;
}

// One VDM's data objects as words (a PD message carries at most 7).
struct AppleVdm {
    std::array<std::uint32_t, 7> w{};
    std::uint8_t n = 0;
};

class AppleReadout {
public:
    void reset();                 // detach / reset: forget the run, answer nothing, SVIDs ACK back to [0x2717]
    bool armed() const { return state_ != State::Idle; }
    void arm(std::uint32_t now);  // start waiting for the source's 0x05AC discovery
    // The SVID word of our Discover SVIDs ACK: unarmed exactly [0x2717]; armed [0x2717, 0x05AC] packed in one word.
    std::uint32_t svid_ack_word() const { return armed() ? 0x271705ACu : 0x27170000u; }

    // One received SOP VDM (all its data-object words): returns the reply to send now (n = 0: none).
    AppleVdm on_vdm(const AppleVdm& rx, std::uint32_t now);
    // Paced reads / retries / timeouts: returns the next VDM to send (n = 0: nothing due).
    AppleVdm tick(std::uint32_t now);

    const AppleInfo& info() const { return info_; }

private:
    enum class State : std::uint8_t { Idle, Armed, Entered, Reading };
    enum class Phase : std::uint8_t { Dir, Str };
    struct Entry { std::uint8_t idx = 0, len = 0; std::uint16_t addr = 0; };

    void stop(AppleWhy why, std::uint32_t now);
    AppleVdm next_read(std::uint32_t now);   // the Attention VDM of the next read, or n=0 when the run is complete
    void on_answer(const AppleVdm& rx, std::uint32_t now);
    void dir_entry(std::uint32_t w, std::uint32_t now);
    void plan_finish(std::uint32_t now);
    void str_append(const AppleVdm& rx);
    static std::array<char, kAppleStr + 1>& field_of(AppleInfo& i, std::uint8_t idx);

    State state_ = State::Idle;
    Phase phase_ = Phase::Dir;
    AppleInfo info_;
    std::uint32_t armed_at_ = 0, entered_at_ = 0;
    // directory phase
    unsigned dir_i_ = 0;
    std::array<Entry, 6> plan_{};   // the fields we read, in directory order: idx 1, 2, 4, 5, 7, 8
    unsigned plan_n_ = 0;
    // string phase
    unsigned str_i_ = 0;
    std::uint16_t str_addr_ = 0;
    std::uint8_t str_got_ = 0;
    // in-flight read
    bool awaiting_ = false;
    std::uint8_t retries_ = 0;
    std::uint32_t deadline_ = 0, next_at_ = 0;
    std::uint8_t cur_nn_ = 0;
    std::uint32_t cur_addr_ = 0;
};

}  // namespace k2::pd
