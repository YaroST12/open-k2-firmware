// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab-only OPPO / OnePlus (SVID 0x22D9) sink-side probe, pure logic (host-tested in tests/test_oppo_probe.cpp and, driven by the
// sink, tests/test_pd_sink_oppo.cpp).
//
// A OnePlus phone on its brick runs this flow after the contract (GPL kernel, oplus_pps_get_adapter_status): DR_Swap to host,
// AUTH (structured cmd 31 + 4 random words; the brick answers 4 words the phone checks with keys nobody has published), POWER
// (unstructured cmd 1: the brick's current and voltage ceiling), EXAPDO (unstructured cmd 2: "extra PPS current" bits), DR_Swap
// back to device, Get_Source_Cap. The K2 sends the same messages and RECORDS what comes back; it never verifies the AUTH reply.
// The sequencer here asks the sink (pd_sink.hpp, the one owner of the wire) for one message at a time, only when the sink says
// its line is free of any open AMS and quiet, and waits for that message's answer (or its time out) before the next one.
//
// LAB-ONLY: started by the remote `pd oppo-probe` op only (Cmd::OppoProbe), refused while a charger test, PD Stress, PD Spec, a twin
// run, another lab engine or an inline link owns the port, and only on a contract at 5 V. Cold code.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/pd.hpp"

namespace k2::pd::oppo {

inline constexpr std::uint16_t kSvid = 0x22D9;
// The three commands as the phone puts them on the wire (inferred from the GPL kernel's tcpm header macros: the SVDM header gets
// version 2.0 and object position 1 from the tcpm layer; the unstructured header is SVID << 16 | command).
inline constexpr std::uint32_t kAuthHeader = 0x22D9A11F;    // structured REQ, cmd 31, + 4 challenge words (5 objects)
inline constexpr std::uint32_t kPowerHeader = 0x22D90001;   // unstructured cmd 1, + one zero word
inline constexpr std::uint32_t kExapdoHeader = 0x22D90002;  // unstructured cmd 2, + one zero word

// --steps bits (the swaps are --skip-swap's).
inline constexpr std::uint8_t kStepAuth = 1, kStepPower = 2, kStepExapdo = 4, kStepCaps = 8, kStepsAll = 0x0F;

// Timing. The K2's own give-up times, not spec figures: a source owes a VDM answer within tVDMSenderResponse (24-30 ms) and the
// DR_Swap answer within tSenderResponse; the margin covers a slow brick MCU and the driver's poll.
inline constexpr std::uint32_t kGapMs = 60;         // the line is quiet this long (no frame either way) before each probe message
inline constexpr std::uint32_t kVdmReplyMs = 300;   // a VDM step's answer window
inline constexpr std::uint32_t kSwapWaitMs = 100;   // a DR_Swap answered Wait is sent again after this (tDRSwapWait)
inline constexpr std::uint8_t kSwapTries = 3;       // DR_Swap goes out at most this often per swap step; Wait every time = Wait
inline constexpr std::uint32_t kCapsMs = 2000;      // Get_Source_Cap -> the list -> our Request -> PS_RDY
inline constexpr std::uint32_t kTotalMs = 10000;    // the whole probe: a line that never goes idle ends it (End::Stalled)

enum class Step : std::uint8_t { Swap, Auth, Power, Exapdo, SwapBack, Caps, Count };
inline constexpr std::size_t kSteps = std::size_t(Step::Count);
// Append only: k2_remote.py carries the names in this order.
enum class Outcome : std::uint8_t {
    NotRun, Ack, Nak, Busy, Reply, Accept, Reject, Wait, NotSupported, Timeout, TxFail, Skipped, Already, Lost
};
const char* outcome_name(Outcome o);
enum class End : std::uint8_t { None, Done, Stopped, HardReset, Detach, SoftReset, Stalled };
const char* end_name(End e);

// The class of a VDM answer from its header word: structured = ACK / NAK / BUSY by the command type (bits 7..6; a REQ is
// recorded as a plain Reply), unstructured = Reply.
constexpr Outcome classify(std::uint32_t hdr) {
    if (((hdr >> 15) & 1u) == 0) return Outcome::Reply;
    switch ((hdr >> 6) & 3u) {
        case 1: return Outcome::Ack;
        case 2: return Outcome::Nak;
        case 3: return Outcome::Busy;
        default: return Outcome::Reply;
    }
}

// POWER answer, its last word: the brick's current and voltage ceiling.
struct PowerInfo { std::uint32_t imax_ma = 0, vmax_mv = 0; };
constexpr PowerInfo decode_power(std::uint32_t w) { return {(w & 0xFFu) * 50u, ((w >> 16) & 0xFFFFu) * 10u}; }
// EXAPDO answer, its last word: the phone enables the extra PPS current ladder only for num 1, bit 7.
struct ExapdoInfo { std::uint8_t num = 0, bit = 0; bool extra_pps = false; };
constexpr ExapdoInfo decode_exapdo(std::uint32_t w) {
    const auto num = std::uint8_t((w >> 30) & 3u), bit = std::uint8_t((w >> 24) & 0x1Fu);
    return {num, bit, num == 1 && bit == 7};
}

// A VDM's data objects (header first), or a list of PDO words.
struct Words {
    std::array<std::uint32_t, 7> w{};
    std::uint8_t n = 0;
    std::span<const std::uint32_t> view() const { return {w.data(), n}; }
};
Words words_of(std::span<const std::uint32_t> src);   // the first 7

// The caps diff: bit i set when SPR slot i + 1 differs between the lists (a PDO changed, appeared or went away).
std::uint8_t caps_diff(std::span<const std::uint32_t> before, std::span<const std::uint32_t> after);

struct Config {
    std::array<std::uint32_t, 4> challenge{};   // the AUTH words (the host draws them, or --auth-words fixes them)
    std::uint8_t steps = kStepsAll;
    bool skip_swap = false;
};

struct Result {
    std::array<Outcome, kSteps> out{};
    std::array<Words, 3> vdm{};      // the Auth, Power and Exapdo answers as received
    Words before, after;             // SPR caps at the start and after the closing Get_Source_Cap
    std::uint8_t diff = 0;           // caps_diff(before, after), when the after list came
    std::uint8_t swap_waits = 0;     // Wait answers to our DR_Swaps
    std::uint8_t steps = 0;
    bool skip_swap = false;
    bool dfp = false;                // the K2's data role when the probe ended (DFP = host)
    End end = End::None;
};

// The start gate, one for the remote. Append only (k2_remote.py names them).
enum class Refusal : std::uint8_t { None, Busy, Inline, NotPartner, NoContract, NotFiveV, Running, BadArg };
const char* refusal_name(Refusal r);
struct Facts {
    bool ports_busy = false;      // a charger test, PD Stress, PD Spec, a twin run, a loop probe, a listen window, an FC session
    bool engine_busy = false;     // identify, the report-card probe, a re-read or the Apple readout runs
    bool inline_mode = false;     // a sink sits behind the K2
    bool trigger_refusal = false; // a sink Rd or a foreign Request on the wire
    bool answers_sop = true;      // the source is the K2's own partner
    bool contract = false;        // the sink holds a Ready contract
    bool five_v = false;          // ... at vSafe5V, SPR
    bool running = false;         // a probe runs already
};
constexpr Refusal refusal(const Facts& f) {
    if (f.running) return Refusal::Running;
    if (f.ports_busy || f.engine_busy) return Refusal::Busy;
    if (f.inline_mode || f.trigger_refusal) return Refusal::Inline;
    if (!f.answers_sop) return Refusal::NotPartner;
    if (!f.contract) return Refusal::NoContract;
    if (!f.five_v) return Refusal::NotFiveV;
    return Refusal::None;
}

// `pd oppo-power`: the advertised PDO / APDO that serves (mv, ma) in the list in force now. A fixed PDO matches its voltage
// exactly, a PPS / AVS APDO when mv lies in its range; ma must not exceed what the slot offers (0 = the slot's maximum). The
// first match in list order wins. pos 0 = nothing advertised serves the point (the caller refuses; nothing is asked above it).
struct Pick { std::uint8_t pos = 0; std::uint16_t mv = 0, ma = 0; };
Pick pick_power(const SourceCaps& caps, std::uint16_t mv, std::uint16_t ma);

// What the sequencer wants sent next.
enum class Want : std::uint8_t { None, SwapToDfp, SwapToUfp, Vdm, GetCaps };
struct Action {
    Want want = Want::None;
    Words vdm;   // Want::Vdm: the data objects
};

class Probe {
public:
    // A new run: forgets the last result. `caps_before` = the SPR words in force, `dfp` = the data role now.
    void start(const Config& c, std::span<const std::uint32_t> caps_before, bool dfp, std::uint32_t now);
    // Ends a running probe (hard reset, detach, soft reset, the stop op) and records the role it left. No-op when not running.
    void stop(End why, bool dfp);
    void reset() { *this = Probe{}; }   // detach: nothing of the last partner's run stays
    bool running() const { return state_ == State::Running; }
    bool engaged() const { return state_ != State::Idle; }   // ran (or runs) this attach: late 0x22D9 answers are its own
    // An answer is outstanding (a VDM's, the DR_Swap's or the caps step's): the sink starts nothing of its own meanwhile.
    bool awaiting() const { return running() && waiting_; }
    Step step() const { return step_; }
    bool wants_caps() const { return awaiting() && step_ == Step::Caps; }
    // A message is due once the line is idle (the sink asks SinkTxOK only then).
    bool ready_to_send(std::uint32_t now) const { return running() && !waiting_ && std::int32_t(now - next_at_) >= 0; }

    // The next message, when `idle` (the sink's line has no open AMS and was quiet for kGapMs). Want::None = nothing due now;
    // timeouts of the outstanding answer are run here too. Anything else: the sink sends it now and reports its fate below.
    Action next(std::uint32_t now, bool idle);
    void on_tx(bool ok, std::uint32_t now);                           // a VDM's / Get_Source_Cap's GoodCRC (false: none came)
    void on_swap(Outcome o, bool dfp, std::uint32_t now);             // the DR_Swap answer (Accept / Reject / Wait / NotSupported / Timeout / TxFail)
    bool on_vdm(std::span<const std::uint32_t> words, std::uint32_t now);   // a 0x22D9 VDM; true = the awaited answer
    bool on_not_supported(std::uint32_t now);                         // true = the awaited VDM step's answer
    void on_caps(std::span<const std::uint32_t> words);               // Source_Capabilities while the caps step waits
    void on_ready(std::uint32_t now);                                 // a PS_RDY: the caps step's re-request is done

    const Result& result() const { return r_; }

private:
    enum class State : std::uint8_t { Idle, Running, Done };
    void record(Outcome o, std::uint32_t now);   // the step in progress ends with `o`; the next one starts after kGapMs
    void advance();
    void finish(End why);

    State state_ = State::Idle;
    Step step_ = Step::Swap;
    bool waiting_ = false;
    bool swapped_ = false;        // our DR_Swap to DFP was accepted: swap back at the end
    bool caps_in_ = false;        // the caps step got its list
    std::uint8_t swap_tries_ = 0;
    std::uint32_t started_ = 0, deadline_ = 0, next_at_ = 0;
    Config cfg_;
    Result r_;
};

}  // namespace k2::pd::oppo
