// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB-PD sink (trigger) engine, pure logic (no I/O, host-tested in tests/test_pd_sink.cpp).
// It builds the messages a sink sends (Request fixed / PPS / EPR AVS, EPR_Mode, chunk requests,
// keep-alives, Get_Source_Cap, hard reset) and runs the Request -> Accept -> PS_RDY state
// machine. The FUSB302 driver owns the wire: it feeds received frames and the VBUS reading in,
// stamps the MessageID and transmits whatever take_tx() returns.
//
// Safety rules enforced here, not left to the caller:
//  - a request above the voltage cap is refused (Err::OverCap), never clamped silently for a fixed PDO;
//    PPS / AVS voltages are clamped to min(PDO max, cap);
//  - a measured VBUS above the cap, or above the target + 10 % once the contract settled, drops
//    the sink back to 5 V (Request for PDO 1); if that fails it sends a hard reset.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_engine.hpp"

namespace k2::pd {

inline constexpr std::uint32_t kVsafe5vMv = 5000;

// Control / data / extended message types a sink uses (USB PD 3.1 table 6.5, 6.6, 6.53).
inline constexpr std::uint8_t kCtlGoodCrc = 1, kCtlAccept = 3, kCtlReject = 4, kCtlPsRdy = 6,
                              kCtlGetSourceCap = 7, kCtlGetSinkCap = 8, kCtlDrSwap = 9, kCtlPrSwap = 10, kCtlWait = 12,
                              kCtlSoftReset = 13, kCtlNotSupported = 16, kCtlGetStatus = 18, kCtlFrSwap = 19,
                              kCtlGetSinkCapExt = 22, kCtlGetRevision = 24;
// Role-swap requests a partner can send (control messages, no data objects; type 9 with objects is EPR_Request).
enum class SwapKind : std::uint8_t { Dr, Pr, Fr };
inline constexpr std::uint8_t kDataSourceCap = 1, kDataRequest = 2, kDataSinkCap = 4,
                              kDataEprRequest = 9, kDataEprMode = 10;  // kDataRevision: pd_ext.hpp
inline constexpr std::uint8_t kExtStatusMsg = 2, kExtSinkCapsExt = 15, kExtControl = 0x10;  // kExtEprSourceCaps: pd_engine.hpp
inline constexpr std::uint8_t kEprEnter = 1, kEprAck = 2, kEprSucceeded = 3, kEprFailed = 4, kEprExit = 5;
inline constexpr std::uint8_t kEprGetSourceCap = 1, kEprKeepAlive = 3;
// EPR_Mode Enter, Data field (bits 23..16) = "EPR Sink Operational PDP" in watts (USB PD 3.2 table 6.28). The sink does not know the
// source's rated EPR PDP before it has entered (Source_Capabilities_Extended is optional and is not read first), and the K2 is a
// tester that takes whatever the source offers, so it states 140 W (28 V at 5 A), the 5 A step of the EPR range.
inline constexpr std::uint8_t kEprSinkPdpW = 140;
// Name of the Enter_Failed Data field (table 6.28); "invalid reason" for values the spec does not define.
const char* epr_fail_name(std::uint8_t code);

// The monitor ceiling (`cap_mv`, per device: INA226 36 V, INA228/237 48 V) bounds what the K2 measures. A level is allowed when its
// nominal voltage is at or under the ceiling (36 V on a 36 V monitor runs; 48 V is skipped there and runs on a 48 V monitor). The
// source's tolerance above the nominal is not budgeted: the runner's overvoltage abort (ceiling + margin) stays the guard.
inline constexpr bool fits_monitor(std::uint32_t mv, std::uint32_t cap_mv) { return mv <= cap_mv; }

// One message to transmit. `data` = the bytes after the header (data objects little endian; for
// an extended message the 2-byte extended header first). The driver fills the MessageID.
struct TxMsg {
    Sop sop = Sop::Sop;
    std::uint16_t header = 0;  // MessageID bits 11:9 are zero
    std::array<std::uint8_t, 28> data{};
    std::uint8_t len = 0;      // bytes in `data`, a multiple of 4 for non-extended messages
    bool hard_reset = false;   // send a hard reset ordered set instead of a message
    bool restart_id = false;   // set the driver's MessageID counter to 0 before stamping (after a Soft_Reset)
};

// Header of a sink-to-source message: port power role sink, data role UFP, MessageID 0.
// `rev` is the 2-bit wire value (1 = PD 2.0, 2 = PD 3.x).
constexpr std::uint16_t make_header(std::uint8_t type, std::uint8_t num_do, std::uint8_t rev, bool ext = false) {
    return std::uint16_t(type | (rev << 6) | (num_do << 12) | (ext ? 1u << 15 : 0u));
}
constexpr std::uint16_t with_msg_id(std::uint16_t header, std::uint8_t id) {
    return std::uint16_t((header & ~(7u << 9)) | ((id & 7u) << 9));
}

// Request data objects. `pos` is 1-based. Fixed: currents in 10 mA units (10 bits); PPS: voltage
// in 20 mV units (bits 20:9), operating current in 50 mA units (bits 6:0); AVS: voltage in 25 mV
// units rounded down to 100 mV, current as PPS. Bit 22 = EPR mode capable, bit 24 = no USB suspend.
std::uint32_t rdo_fixed(std::uint8_t pos, std::uint16_t op_ma, std::uint16_t max_ma, bool epr);
std::uint32_t rdo_pps(std::uint8_t pos, std::uint16_t mv, std::uint16_t ma, bool epr);
std::uint32_t rdo_avs(std::uint8_t pos, std::uint16_t mv, std::uint16_t ma, bool epr = true);

enum class Err : std::uint8_t { Ok, NoCaps, BadPos, OverCap, BadVoltage, Inline, Busy, BadChoice };
const char* err_name(Err e);

struct Choice {
    std::uint8_t pos = 0;      // 1-based PDO position; 0 = only enter EPR mode and read the list
    std::uint16_t mv = 0;      // PPS / AVS target (0 = the PDO maximum below the cap)
    std::uint16_t ma = 0;      // operating current (0 = the PDO maximum)
    bool hunt = false;         // ma is an OEM-mode probe: send it as asked, above the advertised
                               // PDO/APDO limit (the RDO field width still masks it); pass is the
                               // source's Accept + PS_RDY at the target voltage, not the granted amps
};

struct Plan {
    Err err = Err::NoCaps;
    std::uint32_t rdo = 0;
    std::uint8_t pos = 0;
    std::uint32_t pdo = 0;     // the PDO the RDO refers to (EPR_Request carries a copy)
    std::uint16_t mv = 0, ma = 0;  // effective values after clamping
    PdoType type = PdoType::Unknown;
    bool needs_epr = false;    // position >= 8 or an AVS PDO
};

// Validate `c` against `caps` and build the RDO. `cap_mv` = highest voltage allowed (0 or below
// 5 V is treated as 5 V). `epr_mode` selects the EPR flag in the RDO.
Plan plan_request(const SourceCaps& caps, Choice c, std::uint32_t cap_mv, bool epr_mode);

// Voltage cap from the power monitor: the lower of the two limits; 5 V when either is unknown (0).
std::uint32_t request_cap_mv(std::int32_t max_request_mv, std::int32_t max_vbus_mv);

enum class SinkCapExtMode : std::uint8_t { Silent, Skedb, NotSupported };

// The K2's Sink_Capabilities_Extended data block (SKEDB, USB PD 3.2 table 6.61, 24 bytes): VID/PID of the HID logger,
// no XID, FW/HW 1, SKEDB version 1, Sink Modes 0x23 = AVS | VBUS powered | PPS charging, SPR PDP min/op/max 1/15/33 W
// (the 5 V 100 mA mode, a typical load, 11 V * 3 A of the PPS word), EPR PDPs 0 (not EPR capable).
std::array<std::uint8_t, 24> make_skedb();

enum class SinkState : std::uint8_t { Idle, EprEntering, EprWaitCaps, Requesting, Transition, Ready, Aborting, Failed };
enum class SinkResult : std::uint8_t {
    None, Ok, Rejected, WaitExhausted, Timeout, TxFailed, HardReset, SoftReset, Aborted, OverCap, EprFailed, BadChoice
};
const char* state_name(SinkState s);
const char* result_name(SinkResult r);

inline constexpr std::uint32_t kAcceptMs = 300;       // Request -> Accept (spec tSenderResponse 30 ms, margin for the poll)
inline constexpr std::uint32_t kPsRdyMs = 1500;       // Accept -> PS_RDY (spec tPSTransition max 1020 ms in EPR, table 7.1.8)
inline constexpr std::uint32_t kPsRdyDownMs = 4000;   // Accept -> PS_RDY when the voltage steps down: a no-load source drains ~2 V/s
inline constexpr std::uint32_t kWaitRetryMs = 100;    // after Wait (spec tSinkRequest)
inline constexpr std::uint8_t kMaxWaitRetries = 3;
inline constexpr std::uint32_t kPpsKeepAliveMs = 5000;  // spec: re-request at least every 10 s
inline constexpr std::uint32_t kEprKeepAliveMs = 400;   // spec tSinkEPRKeepAlive 250..500 ms
inline constexpr std::uint32_t kEprEnterMs = 800;       // EPR_Mode Enter -> Succeeded (spec tEnterEPR 500 ms)
inline constexpr std::uint32_t kEprCapsMs = 1500;       // Succeeded -> all EPR caps chunks
inline constexpr std::uint32_t kSettleMs = 1500;        // after PS_RDY before the VBUS check applies
// Descent window: after a large step-down the unloaded source's bulk caps discharge slowly, so VBUS can
// still be above target + 10 % when kSettleMs ends. While the samples keep falling (>= 50 mV progress
// every kStallMs) the guard waits, for at most kDescentMs more; a rise > 150 mV or a stall aborts.
inline constexpr std::uint32_t kDescentMs = 3000;
inline constexpr std::uint32_t kStallMs = 600;
inline constexpr std::int32_t kFallStepMv = 50, kRiseMv = 150;
// An unloaded programmable source cannot pull its output down by itself: the Baseus 35 W stops at 3.73 V after
// a PPS 3.3 V request (measured, 5.12 V start, 0.57 V/s fall, flat afterwards). For PPS and AVS the VBUS
// guard and the charger test therefore accept target + max(10 %, kUnloadedFloorMv).
inline constexpr std::int32_t kUnloadedFloorMv = 600;
constexpr std::int32_t programmable_ceiling_mv(std::int32_t target_mv) {
    return target_mv + (target_mv / 10 > kUnloadedFloorMv ? target_mv / 10 : kUnloadedFloorMv);
}
inline constexpr std::uint8_t kMaxTxRetries = 2;

// Release verification (pattern budget-reannounce-mid-run): the PS_RDY of a 5 V release is not proof that VBUS came down (an
// Anker splitter kept 15 V under a "5 V" contract, hardware 2026-11-09). With a live VBUS feed the sink waits kSettleMs after that
// PS_RDY (longer while the samples keep falling, kDescentMs at most, same rule as the hold guard) for VBUS <= vSafe5V max; then it
// asks for PDO 1 again, at most kVerifyRetries times, then a Hard Reset (never on the wire for a self-powered K2: the state is
// Failed and the charger test reports the port as unsafe). A sample older than kVerifyFreshMs is no feed: nothing is verified.
inline constexpr std::uint8_t kVerifyRetries = 2;
inline constexpr std::uint32_t kVerifyFreshMs = 1000;

// Xiaomi chain guard (hardware 2026-11-06, CUKTECH 120 W C1): a sink-initiated Request in the middle of the 0x2717 VDM
// exchange makes the charger drop the chain (no 0x2717020A, no Get_Source_Cap, no 6 A caps for the session). While the
// chain runs the sink holds back its own Requests / EPR entry / Get_Source_Cap; answers to the source's messages go out.
// The chain is over when no 0x2717 exchange happened for this long. The CUKTECH C1 chain pauses up to ~336 ms between two of
// its own steps (hardware 2026-11-06, runs/cuktech6a-scheme1/bb.txt: RX 0x2717 335.9 ms after our TX), so 300 ms let a Request
// slip into the gap; 600 ms covers it with margin and kXiaomiChainBoundMs still caps a stalled chain.
inline constexpr std::uint32_t kXiaomiChainQuietMs = 600;
inline constexpr std::uint32_t kXiaomiChainBoundMs = 2500;   // absolute bound from the SVID 0x2717 listing: a stalled chain never holds longer
inline constexpr std::uint32_t kXiaomiCapsWaitMs = 1000;     // after 0x2717020A: wait this long for the re-announced Source_Capabilities

// EPR_Get_Source_Cap asked in SPR mode (the list is informational; no EPR entry): None = not asked this attach,
// Pending = waiting for the answer, Got = the whole list is in the bound info caps, Refused = Not_Supported / Reject
// (a source that cannot be an EPR source), Timeout = no answer.
enum class EprInfo : std::uint8_t { None, Pending, Got, Refused, Timeout };
inline constexpr std::uint32_t kEprInfoMs = 1500;   // EPR_Get_Source_Cap -> the whole list (or a refusal)
// request_source_caps(): how long the sink counts a Source_Capabilities as the answer to its Get_Source_Cap. A source answers within
// tSenderResponse (30 ms at most); the Connection page's Refresh gives up at 150 ms (k2/core/rescan_bridge.hpp), this is a little more.
inline constexpr std::uint32_t kGetCapAnswerMs = 200;

struct SinkStatus {
    SinkState state = SinkState::Idle;
    EprInfo epr_info = EprInfo::None;
    SinkResult result = SinkResult::None;
    bool held = false;        // a trigger is active (keep-alive running)
    // A release's PS_RDY is in: the sink is checking that VBUS really came down to vSafe5V (and re-asks, then Hard Resets, when it
    // does not). The charger test keeps its release window open while this is set. Never set without a live VBUS feed.
    bool verifying = false;
    bool epr_mode = false;
    std::uint8_t pos = 0;     // contract position when Ready
    std::uint16_t mv = 0, ma = 0;
    std::uint32_t hard_resets = 0;         // Hard Resets the K2 SENT (do_hard_reset on the wire): the K2-must-not-send checks and the UI read this
    std::uint32_t source_hard_resets = 0;  // Hard Resets the SOURCE sent (on_hard_reset_rx); the charger test counts both as "the link was reset"
    std::uint8_t epr_fail = 0;  // Data field of the last EPR_Mode Enter_Failed (epr_fail_name); 0 after a new Enter
    bool xiaomi_svid = false;    // the source listed SVID 0x2717 this session (the auth chain is expected, ~0.7 s after the contract)
    bool xiaomi_authed = false;  // the SVID 0x2717 auth chain completed this session
    bool oppo_svid = false;      // the source listed SVID 0x22D9 (OPPO) in its Discover SVIDs ACK this session: a note only, no VDM is sent
};

// What the sink saw of the Xiaomi (SVID 0x2717) chain and what its last Request met, kept for the charger test's "hunt evidence"
// (docs/CHARGER_TEST_FLOW.md): a refused 6 A request is explained from it, not guessed. Times are the sink's `now` stamps; 0 =
// did not happen. The Request half records EVERY Request that went on the wire (not only a Xiaomi source's); the chain half stays
// empty on a source that never listed the SVID. Cleared on a Hard Reset and a detach, with the rest of the Xiaomi state.
enum class ReqAnswer : std::uint8_t { None, Accept, Reject, Wait };
struct XiaomiTrace {
    std::uint32_t authed_at = 0;      // 0x2717020A seen (the auth chain finished)
    std::uint32_t reannounce_at = 0;  // the first Source_Capabilities after authed_at
    std::uint32_t req_at = 0;         // when the last Request was queued (not when a held one was asked for)
    std::uint32_t answer_at = 0;      // when that Request was answered (Accept / Reject / Wait)
    std::uint32_t req_pdo = 0;        // the PDO word the last Request referred to (the list in force at the time)
    std::uint32_t req_rdo = 0;        // the last Request word as sent
    std::uint16_t req_mv = 0, req_ma = 0;   // its effective voltage and operating current (a hunt keeps the asked mA)
    std::uint8_t last_step = 0;       // the last 0x2717020x REQ the source sent (1..8, 0x0A); 0 = none
    std::uint8_t req_pos = 0;         // object position of the last Request
    std::uint8_t req_caps = 0;        // PDOs in the list in force when it was sent
    std::uint8_t req_n = 0;           // Requests sent since the clear (wraps): "a new one went out" is a change of this number
    PdoType req_type = PdoType::Unknown;
    ReqAnswer answer = ReqAnswer::None;   // the source's answer to the last Request
    bool opener_sent = false;         // our 0x27170101 opener went out
    bool authed = false;              // mirrors authed_at != 0 (a stamp of 0 is a valid time)
    bool reannounced = false;         // a Source_Capabilities arrived after the auth
    bool req_in_chain = false;        // the last Request left while the chain guard said the chain was running
    bool req_held = false;            // a Request is waiting behind the chain guard (never sent yet)
};

class Sink {
public:
    // The source capabilities the requests are validated against (the engine's `caps`).
    void bind(const SourceCaps* caps) { caps_ = caps; }
    // Where an informational EPR_Source_Capabilities list is decoded (the engine's `epr_info`): never the live `caps`.
    void bind_epr_info(const SourceCaps* info) { info_ = info; }
    // SPR mode, Ready, PDO 1 carries "EPR Mode Capable": ask EPR_Get_Source_Cap (once per attach). The answer is informational
    // (USB PD R3.2 6.5.17.1): the sink never answers it with an EPR_Request. Ok = queued; BadChoice = PDO 1 has no EPR bit;
    // Busy = not Ready, in EPR mode, already asked, or the line is occupied; NoCaps = nothing bound. `force` skips the EPR-bit
    // gate (the remote debug ask, to record how a charger without the bit answers).
    Err request_epr_info(std::uint32_t now, bool force = false);
    // The Connection page's Refresh on a live contract: ask the source for its capabilities (Get_Source_Cap) and refresh the list
    // without touching CC. The only sender of this message besides the post-Soft_Reset and Xiaomi paths, and like them it goes through
    // the sink's one gate: Ok = queued (the answer refreshes the engine's caps and is answered with a Request, the same RDO when the list is
    // unchanged: a source that sent Source_Capabilities Hard Resets when none comes);
    // Busy = not Ready, in EPR mode (an unsolicited Source_Capabilities there is a hard reset), or a Request / message / release /
    // Xiaomi chain is outstanding, or the last ask is still inside kGetCapAnswerMs; NoCaps = nothing bound.
    Err request_source_caps(std::uint32_t now);
    void set_cap_mv(std::uint32_t mv) { cap_mv_ = mv < kVsafe5vMv ? kVsafe5vMv : mv; }
    std::uint32_t cap_mv() const { return cap_mv_; }

    // Start a trigger. Err::Ok = queued; anything else = nothing was sent.
    Err trigger(Choice c, std::uint32_t now);
    // Back to 5 V: Request for PDO 1 (EPR_Request in EPR mode); hard reset when impossible.
    void release(std::uint32_t now);
    void hard_reset(std::uint32_t now);  // user asked: send a hard reset and drop everything (always, whatever the next call says)
    // A self-powered K2 (VBUS comes from the source it negotiates with): the sink's own timeouts, aborts and over-cap guards
    // end the attempt without a Hard Reset on the wire. The sink state and result are the same as with one (Failed, Timeout).
    void set_never_hard_reset(bool on) { never_hard_reset_ = on; }
    bool never_hard_reset() const { return never_hard_reset_; }

    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_tx_result(bool ok, std::uint32_t now);  // GoodCRC seen (true) or retries exhausted (false)
    void on_hard_reset_rx(std::uint32_t now);       // the source sent a hard reset
    void detach();                                  // the source went away: forget everything, send nothing
    // Drop the contract with a Soft_Reset instead of a detach (a self-powered K2 must keep VBUS up). The message restarts
    // our MessageID, the sink goes Idle with no held request, and the source answers with Accept and fresh
    // Source_Capabilities, which the driver requests PDO 1 from. A source that stays silent is asked with one
    // Get_Source_Cap after 400 ms. Never a Hard Reset: a missing GoodCRC or answer just leaves the sink Idle.
    void soft_reset(std::uint32_t now);
    void on_vbus(std::int32_t mv, std::uint32_t now);
    void tick(std::uint32_t now);

    // Answer Get_Sink_Cap with Sink_Capabilities (spec PE_SNK_Give_Sink_Cap, two words). Off for the
    // PD-silent legacy session and while the K2 sits inline: there it must not transmit at all.
    void set_gives_sink_cap(bool on) { gives_sink_cap_ = on; if (!on) { sink_cap_pending_ = false; sink_cap_ext_pending_ = false; swap_reply_ = 0; } }
    // Answer what the partner sends on SOP: Discover Identity / Discover SVIDs / Xiaomi VDMs and Soft_Reset (the Accept). Off
    // while another sink may sit behind the K2 (inline, or a Request from a sink seen on the wire): those messages are the
    // far sink's conversation with the source, and any SOP frame of ours in it ends in Hard Resets (hardware 2026-11-07, three
    // recordings). The Soft_Reset still resets our own state; only the transmission is dropped. Default on.
    void set_answers_sop(bool on) { answers_sop_ = on; }
    bool answers_sop() const { return answers_sop_; }
    // Answer to Get_Sink_Cap_Extended: the K2's Sink_Capabilities_Extended (make_skedb, the default: the UGREEN X865 lists its
    // SPR AVS APDO only after it, hardware 2026-11-04), Not_Supported (leaves the X865 at 6 PDOs) or nothing (as the sink
    // did before). Subject to set_gives_sink_cap like the Sink_Capabilities answer.
    void set_sink_cap_ext_mode(SinkCapExtMode m) { sink_cap_ext_mode_ = m; if (m == SinkCapExtMode::Silent) sink_cap_ext_pending_ = false; }
    SinkCapExtMode sink_cap_ext_mode() const { return sink_cap_ext_mode_; }
    // Proactive Discover SVIDs once the contract settles. Default OFF: hardware 2026-11-04
    // (CUKTECH 10, C1) shows the charger runs its own VDM sequence (identity REQ, then
    // Discover SVIDs REQ) and an early sink-side SVIDs REQ desyncs it - the auth chain never
    // starts. The charger-driven flow answers its REQs and sends none first.
    void set_proactive_discover(bool on) { proactive_discover_ = on; }
    bool proactive_discover() const { return proactive_discover_; }

    // True while a Xiaomi auth chain is in progress: the source listed SVID 0x2717 this contract, it is not authed yet and
    // the last 0x2717 exchange is younger than kXiaomiChainQuietMs (and the chain is younger than kXiaomiChainBoundMs);
    // after 0x2717020A until the re-announced caps arrive (kXiaomiCapsWaitMs at most). The sink holds its own Request /
    // EPR entry back meanwhile, and the driver holds its own Get_Source_Cap: any of them kills the chain.
    bool xiaomi_chain_active(std::uint32_t now) const;

    bool take_tx(TxMsg& out);  // next message to send, if any
    const SinkStatus& status() const { return st_; }
    const Plan& plan() const { return plan_; }  // the request in flight / the contract (effective mV, mA, RDO)
    const XiaomiTrace& xiaomi_trace() const { return xm_; }
    std::uint8_t rev() const { return rev_; }  // wire revision to use in headers
    // Role-swap requests received and handled (saturating). The K2 is a UFP-only, VBUS-sink-only port: DR_Swap and PR_Swap
    // are answered with Reject, FR_Swap (PD 3 only) with Not_Supported, never Accept (Accept on PR_Swap makes the partner
    // expect us to drive VBUS and cut its own). Gated like Get_Sink_Cap: nothing is sent or counted inline / PD-silent.
    std::uint8_t swaps_rx(SwapKind k) const { return swaps_rx_[std::size_t(k)]; }
    // The answer a swap request gets at wire revision `rev` (1 = PD 2.0, 2 = PD 3): 0 = no answer.
    static constexpr std::uint8_t swap_reply(SwapKind k, std::uint8_t rev) {
        return k == SwapKind::Fr ? (rev >= 2 ? kCtlNotSupported : std::uint8_t(0)) : kCtlReject;
    }
    // True once after a source hard reset, when the next Source_Capabilities arrive: the caller re-requests
    // PDO 1 (5 V) so the contract returns without anyone asking.
    bool take_resume() { const bool r = resume_; resume_ = false; return r; }

private:
    // `may_defer`: a sink-initiated Request waits while a Xiaomi chain runs (tick sends it when the chain ends). Answers to the
    // source (re-announced caps) and the safety abort pass false and always go out.
    void send_request(std::uint32_t now, bool may_defer = false);
    void send_epr_enter(std::uint32_t now, bool may_defer = false);
    void note_xiaomi_svid(std::uint32_t now);
    void note_xiaomi_exchange(std::uint32_t now) { if (source_has_xiaomi_svid_) chain_at_ = now; }
    void queue(const TxMsg& m);
    void fail(SinkResult r, SinkState to);
    void do_hard_reset(SinkResult r, SinkState to, bool forced = false);
    void proceed_after_epr_caps(std::uint32_t now);
    void queue_epr_control(std::uint8_t kind);
    void begin_abort(SinkResult why, std::uint32_t now);
    void restart_abort(std::uint32_t now);                  // (re)send the Request for PDO 1 of the abort in progress
    void settle_release(std::uint32_t now);                 // the Request a release waited behind is over: send the release now
    void fail_keep_release(SinkResult r, SinkState to, std::uint32_t now);
    void arm_verify(std::uint32_t now);
    void verify_vbus(std::uint32_t now);
    void after_ps_rdy(std::uint32_t now);
    void request_chunk(std::uint16_t header, std::span<const std::uint8_t> payload);
    void update_rev();
    void on_vdm(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void send_xiaomi_vdm_reply(std::uint8_t cmd, std::span<const std::uint32_t> vdos, std::uint32_t now);

    bool never_hard_reset_ = false;
    const SourceCaps* caps_ = nullptr;
    const SourceCaps* info_ = nullptr;      // informational EPR list target (never the live caps)
    std::uint32_t info_deadline_ = 0;
    std::uint32_t cap_mv_ = kVsafe5vMv;
    SinkStatus st_;
    Choice want_;               // the held choice
    Plan plan_;                 // the request in flight / the contract
    std::uint8_t rev_ = 2;
    std::uint32_t deadline_ = 0, next_keepalive_ = 0, ready_at_ = 0, retry_at_ = 0, abort_at_ = 0;
    std::uint8_t wait_retries_ = 0, tx_retries_ = 0;
    bool retry_pending_ = false;
    bool epr_get_sent_ = false;
    bool resume_ = false;       // a source hard reset dropped a contract; re-request on the next caps
    bool get_cap_sent_ = false; // our Get_Source_Cap is out: the next Source_Capabilities answers it (not unsolicited in EPR mode)
    bool refresh_pending_ = false;   // request_source_caps(): its answer window is open until refresh_deadline_ (get_cap_sent_ is set with it)
    std::uint32_t refresh_deadline_ = 0;
    bool contract_ = false;     // a PS_RDY was seen for plan_
    bool sr_wait_ = false;      // a Soft_Reset is out: ask for the caps at sr_at_ if the source has not sent them
    std::uint32_t sr_at_ = 0;
    SinkResult abort_result_ = SinkResult::Ok;
    bool accepted_ = false;      // Accept seen for the abort request
    // A release arrived while another Request was in flight: one Request at a time (USB PD forbids a second before the
    // PS_RDY / Reject), so the release waits and goes out when that one is over (settle_release).
    bool release_pending_ = false;
    bool verify_ = false;                  // a release's PS_RDY is in and VBUS is still to be confirmed (status().verifying)
    std::uint8_t verify_tries_ = 0;        // 5 V Requests repeated so far because VBUS stayed up
    std::uint32_t verify_at_ = 0;          // the PS_RDY the window counts from
    std::int32_t vref_mv_ = 0;             // lowest sample of the verify descent
    std::uint32_t vref_at_ = 0;            // when vref_mv_ last improved by kFallStepMv
    bool vref_set_ = false;
    std::int32_t vbus_mv_ = 0;             // latest sample from on_vbus
    std::uint32_t vbus_at_ = 0;
    bool vbus_seen_ = false;
    std::int32_t ref_mv_ = 0;    // lowest VBUS sample of the current descent
    std::uint32_t ref_at_ = 0;   // when ref_mv_ last improved by kFallStepMv
    bool tracking_ = false;
    bool fresh_ = false;         // a new target: the settle timer restarts at PS_RDY
    bool epr_caps_seen_ = false;
    std::uint32_t next_epr_ka_ = 0, get_cap_at_ = 0, retx_at_ = 0;
    bool retx_pending_ = false;
    TxMsg last_tx_;
    TxMsg tx_;
    bool tx_pending_ = false;
    bool gives_sink_cap_ = true;
    bool answers_sop_ = true;
    bool sink_cap_pending_ = false;   // a Get_Sink_Cap answer waits; it has its own slot so it never displaces a Request
    SinkCapExtMode sink_cap_ext_mode_ = SinkCapExtMode::Skedb;
    bool sink_cap_ext_pending_ = false;   // a Get_Sink_Cap_Extended answer waits (own slot, as above)
    std::uint8_t swap_reply_ = 0;         // a role-swap answer waits (Reject / Not_Supported type; own slot, as above)
    std::array<std::uint8_t, 3> swaps_rx_{};

    // Xiaomi Mi Turbo Charge authentication (SVID 0x2717) state
    bool xiaomi_authed_ = false;      // auth completed, 20V+ PPS should be available
    bool source_has_xiaomi_svid_ = false;  // the SOURCE is Xiaomi-family: its SVIDs ACK listed 0x2717 or it sent a 0x2717 VDM (never its Discover SVIDs REQUEST)
    std::uint8_t xiaomi_cmd_ = 0;     // current Xiaomi command being processed
    bool discover_svids_sent_ = false;    // we have sent Discover SVIDs REQ in this session
    // send Discover SVIDs once after the contract settles; default ON since the sink-initiated
    // Xiaomi opener (2026-11-04, CUKTECH C1) broke the old deadlock - the SVIDs ACK now arms
    // the auth chain instead of leaving both sides waiting
    bool proactive_discover_ = true;
    // Sink-initiated chain opener: 1-DO 0x27170101,
    // queued on the SVIDs exchange that showed 0x2717, paced, sent on SOP once per contract.
    bool xiaomi_opener_pending_ = false;  // waiting for the pacing delay
    bool xiaomi_opener_sent_ = false;     // the opener went out this contract
    std::uint32_t xiaomi_opener_at_ = 0;
    std::uint32_t chain_start_ = 0;   // when the source listed 0x2717 (the bound counts from here)
    std::uint32_t chain_at_ = 0;      // the last 0x2717-session VDM exchange (either direction)
    XiaomiTrace xm_;                  // diagnostics only: nothing in the sink reads it back
    // A sink-initiated message held back by the chain guard: the Request / EPR entry goes out when the chain ends.
    enum class Deferred : std::uint8_t { None, Request, EprEnter } deferred_ = Deferred::None;
    bool discover_svids_pending_ = false;  // proactive SVIDs REQ waiting for its pacing delay
    std::uint32_t discover_svids_at_ = 0;
};

// What this sink advertises: fixed 5 V 100 mA and PPS 3.3-11 V 3 A. No AVS, no EPR.
inline constexpr std::uint32_t kSinkCapPdos[2] = {0x0001900A, 0xC0DC213C};

// Xiaomi Mi Turbo Charge (SVID 0x2717).
// Reply to the charger's first chain message RX 0x27170201. Hardware-verified on the
// CUKTECH 10 C1 (2026-11-04): 0x27170102 runs the full 9-step chain incl. the keyA/keyB
// exchange and the charger re-announces PPS APDOs at 6 A (120 W); 0x27170106
// skips to step 06 and does NOT unlock.
inline constexpr std::uint32_t kXiaomiStep1ReplyVdo = 0x27170102;
// Discover Identity ACK data objects:
// [VDM header 0xFF00A041 (bit 13 set), ID header VDO 0x05C02717, Cert Stat 0, product VDO].
inline constexpr std::uint32_t kXiaomiIdentityAckHeader = 0xFF00A041;
inline constexpr std::uint32_t kXiaomiIdHeaderVdo = 0x05C02717;
inline constexpr std::uint32_t kXiaomiProductVdo = 0x0000A486;

}  // namespace k2::pd
