// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box diagnosis: "why is PD failing" for one attach session, as plain ranked findings with their evidence.
//
// Pure logic, no I/O, no heap (host-tested in tests/test_pd_diag.cpp). One analyser, two homes:
//  - on the K2, pdtrace feeds it every record it writes (the same post-Collapse bytes the ring gets) plus a 20 ms
//    advance() tick, so it keeps counting after the 8 KB ring has wrapped; the last finished session survives unplug;
//  - on the host, tools/bbdiag.cpp feeds it a trace.raw (k2_stream) or a `pd_trace.py dump --raw` file.
//
// PASSIVE: the analyser only reads records; nothing here transmits or touches CC, VBUS or D+/D-.
//
// What it may say (the rules, each one host-tested with a fixture):
//  - a cause only when the data proves it: "draws 0 W" is an observation; it becomes "power short" only with a known need
//    above the best offer. Need sources in trust order: the sink's own Sink_Capabilities on the wire, its
//    Sink_Capabilities_Extended PDP, then a device twin (matched by VID/PID, or assumed by the user, and labelled so);
//    Sink_Capabilities sent by the source role (its own sink side, asked with Get_Sink_Cap) are never the device's need;
//  - the K2 sees one port: findings keep to the observed leg, anything else is "not observed here";
//  - a draw drop with the contract and VBUS unchanged and no power message around it is the device choosing to draw less;
//  - draw above the contract (over_limit_ma: 5 % plus the monitor offset) is a Warn when sustained for a share of the
//    contract time or in bursts of kOverBurstMs or longer; a Fail only when VBUS then falls or the source resets;
//  - VBUS gone with a CC detach right after it is an unplug, not a source cut-out; VBUS falling through the band ahead
//    of a detach is the unplug ramp, not a dip; VBUS between a Request and its PS_RDY is a transition, not a breach;
//  - IBUS is not trusted below kLostMv (the monitor's averaging lags the falling VBUS);
//  - the K2's own sink releasing its contract (a re-read) drops VBUS by design; its own Requests are not the device's;
//  - Soft_Reset on SOP' (the cable plug, normal after a VCONN swap) is not contract instability, and a cable plug NAKing
//    its own Enter_Mode is Info (the port's mode is on SOP);
//  - Source_Capabilities left without GoodCRC before the sink listens are the source's normal repeats, not a link fault;
//  - the data lanes pass through the K2 and are not observed: only what the two ends agreed over PD is reported.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/pdtrace_link.hpp"

namespace k2::pd::diag {

enum class Sev : std::uint8_t { Fail = 0, Warn = 1, Info = 2, Ok = 3 };
enum class Group : std::uint8_t { Power = 0, Contract = 1, Quality = 2, Link = 3, Roles = 4 };

enum class Code : std::uint8_t {
    // power shortfall
    PowerShort, MismatchFlag, NoMismatchFlag, OverDraw, Pinned, Draw, DrawReduced, FiveVoltOnly, ContractUnseen, NoContract,
    ActiveSink,
    // contract instability
    HardReset, SoftReset, Reattach, Rejected, WaitLoop, ManyRequests, NoAccept, LateAccept, NoPsRdy, LatePsRdy, CapsUnanswered,
    // power quality
    VbusLow, VbusHigh, VbusLost, InputCuts, OverSustained,
    // link quality
    NoGoodCrc, Retries, TxFail, RxBad,
    // roles and modes
    SwapRefused, ModeNak, EnterUsbRefused, EprFailed, CableCap, VdmLoop, CableSilent,
};

// Thresholds (named, so the tests and the docs quote the same numbers).
inline constexpr std::uint32_t kReattachMs = 3000;      // an attach this soon after a detach, with VBUS kept up
                                                        // through the gap, continues the session (a CC bounce)
inline constexpr std::uint32_t kUnplugMs = 1500;        // VBUS gone, then a CC detach within this: an unplug
inline constexpr std::uint32_t kSettleMs = 500;         // after PS_RDY, before VBUS and draw count against the contract
inline constexpr std::uint32_t kDrawSettleMs = 10000;   // the draw verdict needs this much contract time
inline constexpr unsigned kCutMin = 3;                  // input cut-outs (draw from kDropFromMa to under kDrawMa inside a
                                                        // power window, VBUS staying in band) before they are reported
inline constexpr std::uint32_t kQuietMs = 2000;        // no power message this long before a draw drop: the device chose it
inline constexpr std::uint32_t kRetryMs = 20;           // the same frame (header and CRC) again within this: a retry
inline constexpr std::uint32_t kAnswerSlackMs = 20;     // record-time slack on the spec response limits
inline constexpr std::uint32_t kMissingMs = 1000;       // no Accept / swap answer this long: missing (later = late)
inline constexpr std::uint32_t kPsRdyMissingMs = 2000;  // no PS_RDY this long after Accept: missing
inline constexpr unsigned kVdmLoopMin = 1;              // the same VDM request asked again this many times, each ...
inline constexpr std::uint32_t kVdmRepeatMs = 1000;     // ... within this of the previous one: a discovery loop
inline constexpr unsigned kManyRequests = 6;            // fixed / variable Requests in one session (PPS keep-alives aside)
inline constexpr std::uint16_t kLostMv = 3500;          // VBUS below this with a contract up: lost
inline constexpr std::uint16_t kUpMv = 5500;            // VBUS above this: a contract above 5 V is up
inline constexpr std::uint16_t kDrawMa = 50;            // below this the device is not drawing
inline constexpr std::uint16_t kDropFromMa = 500;       // a draw drop counts from a level at least this high
inline constexpr unsigned kPinPermille = 950;           // draw at or above this share of the contract current: pinned
inline constexpr unsigned kPinSharePct = 50;            // ... for at least this share of the contract time
// Draw above the contract: IBUS over the RDO current by more than 5 % plus the monitor's offset (the INA226 shunt
// offset and the IBUS sampling noise stay under ~50 mA at no load on hardware, inferred), sustained for a share of the
// settled contract time. A VBUS drop or a hard reset after the first over-draw sample raises it from Warn to Fail.
inline constexpr unsigned kOverPermille = 1050;
inline constexpr std::uint16_t kOverOffsetMa = 50;
inline constexpr unsigned kOverSharePct = 20;           // over the limit at least this share of the contract time ...
inline constexpr std::uint32_t kOverMinMs = 2000;       // ... and at least this long in total; or
inline constexpr std::uint32_t kOverBurstMs = 500;      // a burst: over the limit at least this long without a break ...
inline constexpr std::uint32_t kOverGapMs = 300;        // ... longer than this (one low sample does not end it)
// Sustained draw over the contract: settled contract time in blocks of kSustainMs; a block counts when the draw is
// above over_limit_ma() for kSustainPct of it. Input cut-outs (0 mA samples) inside a block break bursts and pull a
// plain mean down, so the share is the measure; the level is the mean while above. Short bursts in a block that is
// mostly inside the contract do not count here (OverDraw covers them).
inline constexpr std::uint32_t kSustainMs = 10000;
inline constexpr unsigned kSustainPct = 50;
constexpr std::uint32_t over_limit_ma(std::uint32_t contract_ma) { return contract_ma * kOverPermille / 1000 + kOverOffsetMa; }

// One tracked VDM request (SVID << 8 | command) with its count and the last answer.
struct VdmKey {
    std::uint32_t key = 0;
    std::uint16_t count = 0;
    std::uint8_t answer = 0;   // 0 none, 1 ACK, 2 NAK, 3 BUSY, 4 Not_Supported
    std::uint8_t fast = 0;     // repeats within kVdmRepeatMs of the previous one
    std::uint32_t last_ms = 0;
};
inline constexpr std::size_t kVdmKeys = 3;

// The evidence of one attach session (what evaluate() reads). Times are ms on the analyser's unwrapped clock.
// Fields are grouped by width (32, 16, then 8 bit) so the struct carries no padding: two of them live in RAM.
struct Session {
    // ---- 32 bit ----
    std::uint32_t start_ms = 0, end_ms = 0, detach_ms = 0;
    std::uint32_t start_us = 0;            // trace time of the first record (device uptime on the host tool)
    std::uint32_t reattach_ms = 0;
    // the offer (the last Source_Capabilities, summarised: the PDOs themselves are live state)
    std::uint32_t offer_mw = 0;            // the best PDO's power
    // the contract
    std::uint32_t rdo = 0;
    std::uint32_t contract_ms = 0, rdo_ms = 0;
    // the sink's need, from its Sink_Capabilities on the wire (summarised)
    std::uint32_t snk_mw = 0;              // the best sink PDO's power
    // identities (Discover Identity ACK on SOP)
    std::uint32_t snk_idh = 0, src_idh = 0;
    // instability
    std::uint32_t hard_ms = 0, soft_ms = 0, reject_ms = 0, miss_ms = 0;
    // roles and modes
    std::uint32_t refused_ms = 0;
    std::uint32_t mode_nak_ms = 0, cable_nak_ms = 0;
    std::array<VdmKey, kVdmKeys> vdm{};
    // power (sample-and-hold, time-weighted)
    std::uint32_t obs_ms = 0, cobs_ms = 0, pin_ms = 0, over_ms = 0;
    std::uint32_t over_first_ms = 0;       // the first sample above over_limit_ma(c_ma)
    std::uint32_t mj = 0, mv_s = 0, cmj = 0;   // energy (mJ) and mV x s: means over obs_ms / cobs_ms
    std::uint32_t low_ms = 0, high_ms = 0;
    std::uint32_t lost_ms = 0;
    std::uint32_t drop_ms = 0;
    std::uint32_t cut_ms = 0;              // the first input cut-out (draw to 0 with VBUS up, from power windows)
    std::uint32_t sus_first_ms = 0;        // the first kSustainMs block mostly over the contract
    std::uint32_t sus_ms = 0, sus_over_ms = 0;   // the time in such blocks, and the time above the limit in them
    std::uint32_t sus_mas = 0;             // mA x s above the limit in them (the mean while above: sus_mas / sus_over_ms)

    // ---- 16 bit ----
    std::uint16_t msgs = 0;                // PD messages (not GoodCRC) on SOP
    std::uint16_t offer_ma = 0;            // the best PDO's current
    std::uint16_t offer_top_mv = 0;        // the highest voltage offered
    std::uint16_t caps = 0;
    std::uint16_t c_mv = 0, c_ma = 0;
    std::uint16_t snk_mv = 0, snk_ma = 0;  // the best sink PDO's voltage and current
    std::uint16_t snk_top_mv = 0;          // the highest voltage the sink declares
    std::uint16_t snk_vid = 0, snk_pid = 0;
    std::uint16_t late_accept_ms = 0, late_psrdy_ms = 0;
    std::uint16_t refused_mv = 0;          // VBUS (the contract voltage) when the PR_Swap was refused
    std::uint16_t up_mv = 0;               // the first VBUS above kUpMv this session
    std::uint16_t over_peak_ma = 0, over_c_ma = 0;
    std::uint16_t over_off_ma = 0;         // the contracted PDO's offered current at the over-draw peak
    std::uint16_t c_pdo_ma = 0;            // the contracted PDO's offered current (its max)
    std::uint16_t win_n = 0, cut_n = 0;    // power windows on a settled contract; cut-outs among them
    std::uint16_t sus_c_ma = 0, sus_off_ma = 0;   // the contract current and the offer's in the last such block
    std::uint16_t burst_min_ms = 0, burst_max_ms = 0;   // the over-draw bursts' shortest and longest
    std::uint16_t peak_ma = 0;
    std::uint16_t low_mv = 0, low_c = 0, low_ma = 0, high_mv = 0, high_c = 0;   // the worst band breaches
    std::uint16_t lost_ma = 0, lost_c_ma = 0;
    std::uint16_t drop_from_ma = 0, drop_min_ma = 0;

    // ---- 8 bit ----
    bool open = false, attached = false, cc_seen = false, ended = false, active = false;
    std::uint8_t reattaches = 0;
    std::uint8_t n_src = 0;
    bool sink_answered = false;
    bool contract = false, contract_seen = false, epr = false, mismatch = false, pps = false;
    std::uint8_t req_fixed = 0;
    std::uint8_t n_snk = 0;
    bool snk_higher = false;               // sink PDO1 "higher capability": it wants more than 5 V
    std::uint8_t skedb_op_w = 0, skedb_max_w = 0;
    bool snk_host = false;                 // the sink is the USB data host (DFP data role on the wire)
    bool src_host = false;                 // ... the source is
    std::uint8_t hard = 0, soft = 0, soft_k2 = 0, rejects = 0, waits = 0, no_accept = 0, no_psrdy = 0;
    std::uint8_t retries = 0, no_goodcrc = 0, tx_fail = 0, rx_bad = 0;   // link quality; counters saturate (255)
    std::uint8_t refused = 0;              // bit per swap kind refused: 1 PR, 2 DR, 4 VCONN, 8 FR
    std::uint8_t refused_by_src = 0;       // ... the refuser held the source role
    std::uint8_t refused_ans = 0;          // the PR refusal's answer (4 Reject, 12 Wait, 16 Not_Supported, 0 none)
    std::uint8_t mode_nak = 0;             // bit0 DP, bit1 TBT, bit2 other (SOP); bits 4-6 the same, NAKed by the cable (SOP')
    bool eusb_refused = false, epr_failed = false;
    std::uint8_t epr_reason = 0;
    std::uint8_t cable_frames = 0, cable_tries = 0;
    bool cable_answered = false;
    bool have_v = false;
    std::uint8_t over_fault = 0;           // bit0 VBUS fell out of band or was lost after the first over-draw, bit1 hard
                                           // reset after it, bit2 a peak above the offer's own maximum current
    std::uint8_t lost = 0;
    std::uint8_t drops = 0;
    bool drops_quiet = true;
    std::uint8_t bursts = 0;               // over-draw bursts (kOverBurstMs or longer)

    pdtrace::view::LinkAcc link;
};

// A device twin the caller matched (by the session's sink VID/PID) or the user assumed for this session.
struct TwinHint {
    const char* name = nullptr;
    std::span<const std::uint32_t> pdos;
    bool assumed = false;     // the user named it; nothing on the wire matched it
    bool host = false;        // its ID header says USB host
    std::uint16_t vid = 0, pid = 0;   // the twin's identity, when known: a session whose sink identified as another
                                      // device ignores the hint (the twin is not the sink there)
};

struct Finding {
    Sev sev = Sev::Info;
    Group group = Group::Power;
    Code code = Code::Draw;
    std::uint32_t t_ms = 0;   // offset into the session
    std::int32_t a = 0, b = 0, c = 0, d = 0;
};

enum class NeedFrom : std::uint8_t { None, SinkCaps, Skedb, Twin, AssumedTwin };

inline constexpr std::size_t kMaxFindings = 16;
struct Report {
    bool empty = true;        // no session yet
    bool live = false;        // the session is still running
    bool no_pd = false;       // no PD message at all
    bool active = false;      // the K2 was the sink (it sent messages)
    std::uint8_t link = 0;    // 0 healthy, 1 degraded, 2 unstable
    std::uint8_t power = 0;   // see diag.cpp kPower*
    std::uint32_t dur_ms = 0, start_us = 0;
    NeedFrom need_from = NeedFrom::None;
    std::uint32_t need_mw = 0, offer_mw = 0;
    std::uint16_t need_mv = 0, need_ma = 0, vbus_mv = 0, peak_ma = 0;
    const char* twin = nullptr;
    const char* snk_noun = "Device";
    const char* src_noun = "source";
    std::uint8_t n = 0;
    std::array<Finding, kMaxFindings> f{};
    bool quiet_contract = true, quiet_quality = true, quiet_link = true, quiet_roles = true;
    bool band_checked = false;
    pdtrace::view::Link lanes;
};

// One power-monitor window: every IBUS / VBUS sample over `span_ms`, as mean and extremes. The Black Box ring's VBUS
// records are rate and deadband limited and average away what matters (a 0 mA cut-out inside a second of 2 A, a 5.6 A
// burst), so draw and VBUS quality are judged on these windows whenever a session has them.
struct Window {
    std::uint16_t span_ms = 0, n = 0;
    std::uint16_t mv = 0;      // mean VBUS
    std::int16_t ma = 0;       // mean IBUS
    std::uint16_t vmin = 0, vmax = 0;
    std::int16_t imin = 0, imax = 0;
};

// Builds windows from single power samples (the K2 feeds every fresh monitor reading): one window per kWindowMs.
inline constexpr std::uint32_t kWindowMs = 100;
class WindowAcc {
public:
    // Adds a sample; returns true with `out` filled when the window that started kWindowMs ago is complete.
    bool add(std::uint32_t now_ms, std::uint16_t mv, std::int16_t ma, Window& out);

private:
    std::uint32_t t0_ = 0;
    std::int32_t sum_mv_ = 0, sum_ma_ = 0;
    Window w_{};
};

class Analyzer {
public:
    // Whole records (len, type, ts_us, payload) back to back; a short or bad length ends the walk.
    void feed(std::span<const std::uint8_t> records);
    void feed_record(std::uint8_t type, std::uint32_t ts_us, std::span<const std::uint8_t> payload);
    // A power window ending at ts_us (the record clock). Once a session has windows, its draw and VBUS-band evidence
    // come from them; the ring's VBUS records then only witness VBUS loss and the unplug.
    void power_window(std::uint32_t ts_us, const Window& w);
    // Time passes with no record (the K2's 20 ms tick): the held VBUS sample keeps counting, a detach ends the session.
    void advance(std::uint32_t ts_us);
    void reset() { *this = Analyzer(); }

    const Session& current() const { return cur_; }
    const Session& last() const { return last_; }
    bool has_last() const { return have_last_; }
    std::uint16_t finished() const { return finished_; }   // sessions finished so far (the host tool reports each)
    // The session worth showing: the running one when there is one, else the last finished one.
    const Session& shown() const { return cur_.open || !have_last_ ? cur_ : last_; }
    std::uint32_t now_ms() const { return now_ms_; }

private:
    void tick(std::uint32_t ts_us);
    void timers();
    void begin(bool by_attach);
    void finish();
    void hold_to(std::uint32_t ms);
    void sample(std::uint16_t mv, std::int16_t ma);
    void observe(const Window& w);   // a ring record is a window of one sample and no span
    void band(std::uint16_t vmin, std::uint16_t vmax, std::uint16_t ma);
    bool settled() const;
    void message(bool rx, std::uint8_t token, std::uint16_t hdr, std::span<const std::uint8_t> objs, std::uint32_t crc);
    void contract_from_rdo();
    void close_burst();
    bool first_vbus_opens(std::uint16_t mv) const;   // VBUS up on a trace that started mid-session
    void sustain(std::uint32_t dt, std::uint32_t ma);   // the kSustainMs blocks against the contract

    // Live tracking: not part of the evidence, so not kept with the last session. Grouped by width (no padding).
    struct Frame {
        std::uint32_t crc = 0;
        std::uint16_t hdr = 0, ms16 = 0;
    };
    Session cur_, last_;
    std::array<Frame, 3> frame_{};         // SOP, SOP', SOP''
    std::array<std::uint32_t, 7> src_pdos_{};   // the offer, for the contract at PS_RDY
    std::uint32_t pend_ms_ = 0, accept_ms_ = 0, v_ms_ = 0;
    std::uint32_t plow_ms_ = 0;            // a VBUS reading under the band, waiting kUnplugMs for a detach (plow_)
    std::uint32_t lost_pend_ms_ = 0, last_pwr_ms_ = 0;
    std::uint32_t over_on_ms_ = 0, over_last_ms_ = 0;   // the open over-draw burst: first and last sample above
    std::uint32_t blk_ms_ = 0, blk_over_ms_ = 0, blk_over_mas_ = 0;   // the open kSustainMs block: settled time, the
                                                                       // time above the limit, mA x ms above it
    std::uint32_t last_us_ = 0, now_ms_ = 0, frac_us_ = 0;
    std::uint16_t v_mv_ = 0;
    std::int16_t i_ma_ = 0;
    std::uint16_t plow_mv_ = 0, plow_c_ = 0, plow_ma_ = 0;
    std::int16_t lost_pend_ma_ = 0;
    std::uint16_t hi_ma_ = 0;
    std::uint16_t mj_frac_ = 0, mv_frac_ = 0, cmj_frac_ = 0;   // remainders of the mJ / mV x s sums (below 1000)
    std::uint16_t finished_ = 0;
    std::uint16_t cable_mode_svid_ = 0;    // the SVID of the last Enter_Mode sent to the cable plug
    std::int16_t prev_imax_ = 0;           // the previous power window's IBUS maximum (a cut-out across windows)
    std::uint16_t pre_mv_ = 0;            // VBUS before any session opened (seeds the next one)
    std::int16_t pre_ma_ = 0;
    std::uint8_t frame_open_ = 0;          // bit per SOP type: a frame waits for its GoodCRC
    std::uint8_t pend_ = 0, pend_from_src_ = 0;
    bool wait_psrdy_ = false;
    bool lost_pend_ = false, low_ = false;
    bool unplug_ = false;                  // VBUS went away around the detach: a new attach is a new session
    bool plow_ = false;
    bool over_on_ = false;
    bool win_ = false;                     // this session has power windows
    bool cc_any_ = false;                  // a CC record was seen (else the trace started mid-session)
    bool have_last_ = false, have_clock_ = false, pre_have_ = false;
};

// The findings of one session. `twin` is optional (matched or assumed); `now_ms` closes a live session's clock.
Report evaluate(const Session& s, const TwinHint* twin, std::uint32_t now_ms);

// Text: line 0 the verdict, then per finding its title and its note, then the healthy lines, then the data-lane line
// and its note. Each line fits kLineMax - 1 characters.
inline constexpr std::size_t kLineMax = 80;
enum class LineKind : std::uint8_t { Head = 0, Title = 1, Note = 2, Healthy = 3, Lanes = 4 };
struct Line {
    LineKind kind = LineKind::Head;
    Sev sev = Sev::Ok;
    std::uint8_t num = 0;      // finding number (1-based) for Title / Note; 0 otherwise
    std::array<char, kLineMax> text{};
};
unsigned line_count(const Report& r);
bool line(const Report& r, unsigned idx, Line& out);
// A one-line verdict for the Connection page ("PD link healthy, power short").
void verdict(const Report& r, std::span<char> out);
const char* code_name(Code c);   // stable lowercase token for --json ("power_short")

}  // namespace k2::pd::diag
