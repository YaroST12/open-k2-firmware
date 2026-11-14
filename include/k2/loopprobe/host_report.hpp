// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Loop probe "Host report", pure half: the PD interrogation of a USB-C host port, its record, the record's versioned layout
// and journal, and the start refusal. Host-tested in tests/test_host_report.cpp; the K2 binding is
// k2/loopprobe/host_report_hw.hpp (built with K2_HOST_REPORT=1 only). Prototype: docs/LOOP_PROBE.md, "Host report".
//
// Setup: host USB-C port -> the K2's measured path -> a cable -> the K2's own HID (device) port. The HID port has a fixed Rd and
// no PD controller (only D+/D- reach the MCU), so the host sees one sink, settles as source + DFP, powers the K2 and enumerates
// its HID device. The K2's FUSB302 on the measured CC line is then the only PD speaker: it GoodCRCs the host's caps, the sink
// engine takes a 5 V contract (PDO 1, never more), and this sequencer asks its questions one at a time.
//
// Never drops VBUS: the Env has no Hard Reset (the driver also runs the sink with never_hard_reset), no Request is built here
// (the sink's PDO 1 is the only one), no PR_Swap, no VCONN_Swap. A transmit that keeps failing (no GoodCRC) is a protocol error:
// one Soft_Reset (at most kMaxSoftResets per run), then the step is asked again on the new contract.
//
// Order (the risky swap last, so everything before it is in the journal when it goes out):
//   IdUfp      Discover Identity as UFP (PD 3.x only: a PD 2.0 UFP may not start a VDM)
//   SrcCapExt  Get_Source_Cap_Extended   (PD 3.x only, as are the next three; a refusal such as Not_Supported or
//   Revision   Get_Revision                Reject is recorded as data)
//   MfrInfo    Get_Manufacturer_Info
//   Status     Get_Status
//   DrSwap     DR_Swap (VBUS untouched), only when IdUfp got no identity (PD 2.0 or no answer); the journal is saved first
//   IdDfp      Discover Identity as DFP (only when the swap made the K2 DFP and no identity came as UFP)
//   Svids      Discover SVIDs (DFP only)
//   Modes1/2   Discover Modes for the first two SVIDs worth it (pd::id_modes_svid order: DisplayPort, Thunderbolt, rest)
//   DrBack     DR_Swap back, so the host is DFP (USB host) again
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "k2/loopprobe/usb_fp.hpp"
#include "k2/pd/pd_sink.hpp"

namespace k2::loopprobe::host {

inline constexpr std::uint8_t kVersion = 1;            // record layout version
inline constexpr std::uint32_t kSettleMs = 200;        // Ready contract settled this long before the first question (alone-Identify)
inline constexpr std::uint32_t kRespMs = 60;           // answer window: 2 x tSenderResponse max (30 ms), for the main loop's RX latency
inline constexpr std::uint32_t kBackoffMs = 50;        // retry n waits n x this (tVDMBusy is 50 ms minimum)
inline constexpr std::uint32_t kWaitRetryMs = 100;     // a Wait answer to DR_Swap: ask again after this
inline constexpr std::uint8_t kMaxAttempts = 3;
inline constexpr std::uint8_t kMaxSoftResets = 2;
inline constexpr std::uint32_t kContractWaitMs = 10000;   // no Ready contract within this (from start or a reset): the run ends
inline constexpr std::uint32_t kUsbWaitMs = 4000;      // the DR_Swap waits this long after Ready at most for the USB enumeration
inline constexpr std::uint8_t kMaxBoots = 3;           // journal: a run that rebooted the K2 this often is closed

enum class Q : std::uint8_t { IdUfp, SrcCapExt, Revision, MfrInfo, Status, DrSwap, IdDfp, Svids, Modes1, Modes2, DrBack };
inline constexpr std::size_t kQueries = 11;

// The outcome of one question. A refusal is data too.
enum class Ans : std::uint8_t { NotRun, Ack, Nak, Busy, NotSupported, Reject, Wait, Timeout, TxFail, Skipped, Interrupted };
// Why a question was skipped (Ans::Skipped) or interrupted.
enum class Skip : std::uint8_t { None, Pd2, NoDfp, HaveId, NoSvid, NotSwapped, HardReset, Rebooted, Ended };
// Why the run ended.
enum class End : std::uint8_t { None, Done, NoContract, Cancelled, ForeignSink, Detached, Rebooted, TooManyBoots, VbusHigh, NoLoop };

struct QRec {
    std::uint8_t ans = 0;        // Ans
    std::uint8_t attempts = 0;   // messages sent for it (retries included)
    std::uint8_t skip = 0;       // Skip
    std::uint16_t t_ms = 0;      // when the outcome came, ms after the contract went Ready (saturated)
};

// What the PD conversation collected.
struct PdFacts {
    std::uint8_t rev = 0;                       // the host's spec revision (Source_Capabilities header: 1 PD 2.0, 2 PD 3.x)
    std::uint8_t n_pdos = 0;
    std::array<std::uint32_t, 7> pdos{};        // Source_Capabilities as advertised (PPS / EPR bits as sent, never requested)
    std::uint8_t caps_seen = 0;                 // Source_Capabilities messages (saturated)
    std::uint16_t ready_ms = 0;                 // first Ready contract, ms after start (0 = none)
    std::array<QRec, kQueries> q{};
    std::uint8_t n_id = 0;
    std::array<std::uint32_t, 6> id{};          // Discover Identity ACK VDOs after the VDM header (ID header, cert, product, ...)
    std::uint8_t n_svids = 0;
    std::array<std::uint16_t, 8> svids{};
    std::array<std::uint16_t, 2> mode_svid{};
    std::array<std::uint8_t, 2> mode_n{};       // modes the ACK listed
    std::array<std::uint32_t, 2> mode_vdo{};    // the first mode VDO
    std::uint8_t n_scx = 0;
    std::array<std::uint8_t, 25> scx{};         // Source_Capabilities_Extended data block
    std::uint32_t revision = 0;                 // Revision data object
    std::uint8_t n_mfr = 0;
    std::array<std::uint8_t, 26> mfr{};         // Manufacturer_Info data block (VID, PID, name)
    std::uint8_t n_status = 0;
    std::array<std::uint8_t, 7> status{};       // Status data block
    std::uint8_t dfp = 0;                       // 1 = the K2 is DFP now
    std::uint8_t hard_resets = 0, soft_resets = 0, tx_fails = 0;   // hard resets the HOST sent; soft resets the K2 sent

    const QRec& at(Q x) const { return q[std::size_t(x)]; }
    QRec& at(Q x) { return q[std::size_t(x)]; }
};

// ---- the sequencer --------------------------------------------------------------------------------------------------------

// The wire, as the FUSB302 driver gives it. No Hard Reset, no Request, no other swap: this interface cannot send them.
class Env {
public:
    virtual bool send(const pd::TxMsg& m) = 0;   // one SOP message; true = the host's GoodCRC came back
    virtual void set_dfp(bool dfp) = 0;          // the data role the K2's headers and its GoodCRC carry from now on
    virtual bool soft_reset() = 0;               // the sink's Soft_Reset (contract drops; the sink asks PDO 1 on the new caps)
protected:
    ~Env() = default;
};

enum class Phase : std::uint8_t { Idle, WaitContract, Settle, Asking, Done };

class Seq {
public:
    explicit Seq(Env& e) : env_(e) {}
    // Starts waiting for the contract. `resume`: the facts of a run the K2 rebooted in (journal): what was answered stays, the
    // question that was due when the journal was saved is not asked again (Interrupted / Rebooted), the role is UFP again.
    void start(std::uint32_t now, const PdFacts* resume = nullptr);
    void stop(End why, std::uint32_t now);
    // The driver's feeds. on_message gets every SOP message from the host (header, data objects; extended: ext header first).
    void on_message(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_contract(bool ready, std::uint32_t now);   // the sink holds a Ready contract (PDO 1) or not
    void on_hard_reset(std::uint32_t now);            // the host sent a Hard Reset (roles back to UFP)
    // `idle`: the line is quiet and the sink has nothing in flight; `usb_settled`: the host's enumeration is over.
    void tick(std::uint32_t now, bool idle, bool usb_settled);
    // True once, right before the DR_Swap goes out: save the journal now (the swap goes on the next tick).
    bool take_checkpoint();

    bool running() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    Phase phase() const { return phase_; }
    Q current() const { return Q(q_); }
    End end() const { return end_; }
    const PdFacts& facts() const { return f_; }

private:
    void advance(std::uint32_t now);
    void complete(Ans a, std::uint32_t now, Skip why = Skip::None);
    void retry_or(Ans a, std::uint32_t now, std::uint32_t wait_ms);
    void tx_failed(std::uint32_t now);
    bool askable(Q x, Skip& why) const;
    pd::TxMsg build(Q x) const;
    std::uint8_t rev() const { return f_.rev >= 2 ? 2 : 1; }
    bool matches_vdm(std::uint8_t cmd) const;
    std::uint16_t mode_svid(unsigned idx) const;

    Env& env_;
    PdFacts f_{};
    Phase phase_ = Phase::Idle;
    End end_ = End::None;
    std::uint8_t q_ = 0;
    bool awaiting_ = false, cp_due_ = false, cp_done_ = false;
    std::uint8_t tries_ = 0;            // attempts of the current question since the last contract
    std::uint32_t start_ = 0, wait_from_ = 0, ready_at_ = 0, deadline_ = 0, next_at_ = 0;
};

const char* q_name(Q x);
const char* ans_name(Ans a);
const char* end_name(End e);

// ---- the start refusal: the loop case, explicitly ---------------------------------------------------------------------------
// The general rule stays: an inline K2 never transmits. This session is the one exception, and only on proof that the far
// sink is the K2's own HID port: the USB link is on and a host enumerated the HID device (or sends SOFs), VBUS is at 5 V,
// the current through the meter is no more than the K2's own draw, no other sink spoke PD, and the K2 holds no Rd, no
// contract and no other PD tool. A Request from another sink later ends the run (End::ForeignSink).

inline constexpr std::int32_t kMinVbusMv = 4000;
inline constexpr std::int32_t kMaxVbusMv = 5500;   // never above vSafe5V max
inline constexpr std::int32_t kLoadMaxMa = 400;    // the K2's own draw with the backlight on is far below; a phone is not (inferred)

struct LoopFacts {
    bool usb_core = false;       // the USB link is on (Tools > USB link)
    bool hid_host = false;       // a host configured the HID device or sends SOFs
    std::int32_t vbus_mv = -1;   // newest sample, -1 unknown
    std::int32_t ibus_ma = 0;    // through the meter, signed
    std::uint8_t active_cc = 0;  // 0 = no CC line attached yet
    bool far_request = false;    // a Request from a sink other than the K2 in this attach
    bool own_rd = false;         // the K2 presents an Rd itself (key-mode pin, a run, the Ra mode)
    bool busy = false;           // another PD tool owns the line (Identify, probe, re-read, charger test, PD Stress, ...)
    bool contract = false;       // the K2's sink holds a contract already
};
enum class Refuse : std::uint8_t { None, UsbOff, NoHost, NoCc, NoVbus, VbusHigh, Load, FarSink, OwnRd, Busy, Contract };
Refuse refusal(const LoopFacts& f);
// The ones the start waits out (the host is still enumerating, the CC scan has not attached, VBUS is coming up).
constexpr bool transient(Refuse r) { return r == Refuse::NoHost || r == Refuse::NoCc || r == Refuse::NoVbus; }
const char* refuse_name(Refuse r);

// ---- the record and its journal --------------------------------------------------------------------------------------------

enum class JState : std::uint8_t { None, Armed, Running, Done };

inline constexpr std::size_t kFwLen = 30;

struct Report {
    std::uint8_t state = 0;       // JState
    std::uint8_t boots = 0;       // boots that started or resumed this run
    std::uint8_t end = 0;         // End
    std::uint8_t refuse = 0;      // Refuse: why the latest start did not happen
    std::uint8_t os = 0;          // usbfp::Os, the classifier's guess
    std::array<char, kFwLen> fw{};
    std::uint32_t start_ms = 0;   // uptime when the latest session started
    std::uint8_t cc = 0;          // active CC line at the start
    std::uint8_t rp = 0;          // the host's Rp as BC_LVL on that line: 1 default USB, 2 1.5 A, 3 3 A (0 unknown)
    std::int16_t vbus_mv = 0;     // at the start
    std::int16_t dp_mv = -1, dm_mv = -1;   // D+ / D- at the start (passive only: the BC1.2 hint, see docs)
    PdFacts pd;
    usbfp::Trace usb;
};

// Layout (little endian), version 1, exactly kReportBytes:
//   version u8, state u8, boots u8, end u8, refuse u8, os u8, fw[30], start_ms u32, cc u8, rp u8, vbus i16, dp i16, dm i16
//   pd: rev u8, n_pdos u8, 7 x pdo u32, caps_seen u8, ready_ms u16,
//       11 x (ans u8, attempts u8, skip u8, t_ms u16)                        (Q order)
//       n_id u8, 6 x u32, n_svids u8, 8 x u16, 2 x mode svid u16, 2 x mode n u8, 2 x mode vdo u32,
//       n_scx u8, scx[25], revision u32, n_mfr u8, mfr[26], n_status u8, status[7], dfp u8, hard u8, soft u8, tx_fails u8
//   usb: n u8, total u16, flags u8, 24 x (t_ms u16, kind u8, req_type u8, req u8, value u16, index u16, length u16)
inline constexpr std::size_t kReportBytes = 529;

std::size_t encode(const Report& r, std::span<std::uint8_t> out);   // 0 when `out` is too small
std::optional<Report> decode(std::span<const std::uint8_t> in);     // nullopt for a wrong length or version
bool decode(std::span<const std::uint8_t> in, Report& out);          // the same into out (no record on the stack)

// The flash file /lp/host.rec: "LPH" + version, length u16, CRC-16 (0xFFFF seed, over the record), the record.
inline constexpr std::size_t kFileHeader = 8;
inline constexpr std::size_t kFileBytes = kFileHeader + kReportBytes;
std::size_t encode_file(const Report& r, std::span<std::uint8_t> out);
std::optional<Report> decode_file(std::span<const std::uint8_t> in);
bool decode_file(std::span<const std::uint8_t> in, Report& out);

// What a boot does with the journal. Armed (the user armed a run, then replugged the host): Start, the state goes Running.
// Running (the K2 lost power inside a run: the host cut VBUS, a Hard Reset, an unplug): Resume, boots counted; past
// kMaxBoots the run is closed (End::TooManyBoots). Anything else: Nothing. `r` is updated; the caller saves it when the
// answer is not Nothing.
enum class Boot : std::uint8_t { Nothing, Start, Resume, Closed };
Boot boot_decide(Report& r);

// ---- the Loop probe page's Host view: five key / value rows ------------------------------------------------------------------
// Host (journal state, end or refusal), PD (revision, PDO count, Rp), Ident (VID, else the answer), DR swap (the answer),
// USB (the OS guess and the event count). Values are at most kRowChars characters, NUL-terminated in `out`.
inline constexpr std::size_t kRows = 5;
inline constexpr std::size_t kRowChars = 17;
const char* row_key(std::size_t row);
void row_text(const Report& r, std::size_t row, std::span<char> out);

}  // namespace k2::loopprobe::host
