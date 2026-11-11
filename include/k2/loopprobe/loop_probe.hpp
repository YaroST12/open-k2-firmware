// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID-cable loop probe, pure half: the record, its versioned binary layout, and the step runner over an Env seam.
// Research tool (docs/LOOP_PROBE.md): the K2 sits on a charger and a cable runs from its PD-bus port back into its own
// HID port. The probe records raw numbers with timestamps, never a verdict. It runs at 5 V only and never asks for more:
// it refuses to start above kMaxVbusMv on VBUS or kMaxAuxMv on the aux rail, and ends the run when either rises later.
//
// Steps, in execution order (the CC step runs last because it may cut VBUS):
//   Baseline  kBaseSamples readings of VBUS, IBUS, aux, D+, D-, CC1, CC2 (avg/min/max), the HID port's line state and
//             frame counter at the start and the end, the attach state, and BC_LVL on each CC line.
//   Drop      the K2's own draw stepped through kDropPlan (backlight on/off, CPU idle/busy) and back in reverse order:
//             VBUS, aux and IBUS at each level after kDropSettleMs.
//   DpDm      bus-side D+/D- driven through kDpPlan (each state framed by kDischargeMs of 0 V on both lines, held
//             kHoldMs), the HID side's line state read at the end of each hold; then the HID D+ pull-up off and on with
//             the bus lines released, bus-side D+/D- read back.
//   Emarker   the sniffed cable identity as the driver holds it (raw VDOs); the VCONN read is never started here.
//   Cc        the K2's own Rd per kCcPlan, held kCcHoldMs per state, both CC lines and BC_LVL sampled. A record marked
//             "in progress" is saved first, so a reboot in this step is reported by boot_check().
// Every pure part is host-tested (tests/test_loop_probe.cpp); the K2 binding is k2/loopprobe/loop_probe_hw.hpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "k2/drivers/dpdm.hpp"
#include "k2/fastcharge/qc.hpp"
#include "k2/loopprobe/loop_cc.hpp"

namespace k2::loopprobe {

inline constexpr std::uint8_t kVersion = 1;            // record layout version (also the QR schema)
inline constexpr std::int32_t kMaxVbusMv = 5500;       // never above vSafe5V max: refuse / end the run
inline constexpr std::int32_t kMaxAuxMv = 5800;        // the aux rail limit of the same rule
inline constexpr std::int32_t kVbusPresentMv = 4000;   // the Drop step needs VBUS
inline constexpr std::int32_t kCcMinVbusMv = 4500;     // the CC step needs 5 V on VBUS

inline constexpr std::uint8_t kBaseSamples = 16;
inline constexpr std::uint32_t kSampleGapMs = 40;      // the INA226 averages over ~35 ms: one fresh value per sample
inline constexpr std::uint32_t kDropSettleMs = 300;
inline constexpr std::uint8_t kDropSamples = 6;
inline constexpr std::uint32_t kSpinMs = 40;           // a busy level spins this long before each sample
inline constexpr std::uint32_t kDischargeMs = 100;     // 0 V on both lines before every D+/D- state
inline constexpr std::uint32_t kHoldMs = 100;          // one D+/D- state: far below the 1 s an HVDCP handshake needs
inline constexpr std::uint32_t kPullSettleMs = 50;
inline constexpr std::uint16_t kCcHoldUs = 3000;       // one Rd state: below a source's CC debounce

enum class Step : std::uint8_t { Baseline, Drop, DpDm, Emarker, Cc };
inline constexpr std::size_t kSteps = 5;

enum class Status : std::uint8_t { NotRun, Done, Skipped, Aborted, Interrupted };

// Why a step was skipped or the run ended (Record::Step::why).
enum class Why : std::uint8_t {
    None,
    NoMonitor,     // the power monitor gave no reading
    VbusHigh,      // VBUS above kMaxVbusMv
    AuxHigh,       // aux above kMaxAuxMv
    NoVbus,        // VBUS below what the step needs
    HostOnHid,     // a USB host is on the HID port (configured or SOFs seen): no loop, the far end may be a real device
    CcRefused,     // the driver refused the CC hold (Record::cc.refusal says why)
    NoEmarker,     // nothing sniffed
    Cancelled,     // BACK / cancel()
    StoreFailed,   // the in-progress record could not be saved: the CC step does not run without it
    Rebooted,      // boot_check() found the in-progress record
};

// Start refusals (nothing was changed).
enum class Start : std::uint8_t { Ok, NoMonitor, VbusHigh, AuxHigh, Running };

// The K2's own draw in the Drop step. The backlight is a plain GPIO (no PWM), so there is no "mid" level.
enum class Load : std::uint8_t { BlOn, BlOff, BlOffBusy, BlOnBusy };
inline constexpr bool busy(Load l) { return l == Load::BlOffBusy || l == Load::BlOnBusy; }
inline constexpr std::array<Load, 8> kDropPlan = {Load::BlOn,     Load::BlOff,    Load::BlOffBusy, Load::BlOnBusy,
                                                  Load::BlOnBusy, Load::BlOffBusy, Load::BlOff,    Load::BlOn};

// The D+/D- states driven on the bus side, (D+, D-). Each state is held kHoldMs after kDischargeMs of 0/0, which also ends
// any HVDCP mode (D+ below ~0.325 V), so no state lasts the 1 s an HVDCP handshake needs. On top of that the states
// themselves are the safe subset dp_state_safe() admits.
struct DpState {
    dpdm::Drive dp, dm;
};
inline constexpr std::array<DpState, 6> kDpPlan = {{
    {dpdm::Drive::HiZ, dpdm::Drive::HiZ},
    {dpdm::Drive::V0, dpdm::Drive::V0},
    {dpdm::Drive::V0p6, dpdm::Drive::V0},
    {dpdm::Drive::V3p3, dpdm::Drive::V0},
    {dpdm::Drive::V0, dpdm::Drive::V0p6},
    {dpdm::Drive::V0, dpdm::Drive::V3p3},
}};
// A state the probe may drive: only the plain levels (released, 0, 0.6, 3.3 V); none of the QC2 high-voltage codes
// (qc::qc2_levels 9 / 12 / 20 V) and not the QC3 entry (0.6 / 3.3); and a released line only next to a released or 0 V
// line, since a charger's D+/D- short would carry the driven level onto it and form one of those codes.
constexpr bool dp_state_safe(DpState s) {
    using dpdm::Drive;
    const auto plain = [](Drive d) { return d == Drive::HiZ || d == Drive::V0 || d == Drive::V0p6 || d == Drive::V3p3; };
    if (!plain(s.dp) || !plain(s.dm)) return false;
    for (const int mv : {9000, 12000, 20000})
        if (const auto l = qc::qc2_levels(mv); l && l->dp == s.dp && l->dm == s.dm) return false;
    if (s.dp == Drive::V0p6 && s.dm == Drive::V3p3) return false;   // QC3 continuous-mode entry
    const auto quiet = [](Drive d) { return d == Drive::HiZ || d == Drive::V0; };
    if ((s.dp == Drive::HiZ && !quiet(s.dm)) || (s.dm == Drive::HiZ && !quiet(s.dp))) return false;
    return true;
}
constexpr bool plan_safe() {
    for (const DpState& s : kDpPlan)
        if (!dp_state_safe(s)) return false;
    return true;
}
static_assert(plan_safe(), "the loop probe's D+/D- plan holds a state that could ask a charger for more than 5 V");

// The K2's own Rd per CC state: kCcPlan in k2/loopprobe/loop_cc.hpp.

// ---- what the Env reports ------------------------------------------------------------------------------------------------

struct Sample {
    bool pm_ok = false;                  // power monitor answered
    std::int32_t vbus_mv = 0, ibus_ma = 0;
    bool adc_ok = false;
    std::int32_t aux_mv = 0, dp_mv = 0, dm_mv = 0, cc1_mv = 0, cc2_mv = 0;
};

// The HID port as the USB driver sees it. Hid::bits packs the booleans for the record.
struct Hid {
    std::uint8_t bits = 0;               // kHid* below
    std::uint16_t frame = 0;             // the frame counter (11 bits), valid with kHidCore
    std::uint16_t resets = 0;            // bus resets since boot (saturated)
};
inline constexpr std::uint8_t kHidCore = 1 << 0;         // the USB core runs (the link is on in Settings)
inline constexpr std::uint8_t kHidConfigured = 1 << 1;   // a host configured the device
inline constexpr std::uint8_t kHidRxDp = 1 << 2;         // the PHY's D+ receiver (valid with kHidCore)
inline constexpr std::uint8_t kHidRxDm = 1 << 3;         // the PHY's D- receiver
inline constexpr std::uint8_t kHidPinDp = 1 << 4;        // PA12 input level
inline constexpr std::uint8_t kHidPinDm = 1 << 5;        // PA11 input level

// The attach state as the PD side holds it.
struct Link {
    std::uint8_t active_cc = 0;          // 0 none, 1 CC1, 2 CC2
    std::uint8_t flags = 0;              // kLink* below
    std::uint8_t guard = 0;              // link::Reason the inline guard gives now
};
inline constexpr std::uint8_t kLinkInline = 1 << 0, kLinkDownstream = 1 << 1, kLinkCaps = 1 << 2, kLinkContract = 1 << 3,
                              kLinkSinkContract = 1 << 4;

enum class PullMethod : std::uint8_t { None, CorePullup, GpioPull };   // how the HID D+ pull-up was toggled

// The cable identity the driver sniffed.
struct Emk {
    std::uint8_t valid = 0;
    std::uint8_t num_vdos = 0;
    std::array<std::uint32_t, 6> vdos{};   // header, cert stat, product, cable VDO(s) as received
    std::uint16_t seq = 0;                 // fusb302::cable_seq() (low 16 bits)
    std::uint8_t read_mode = 0;            // emk_attached::Mode of the last VCONN read (0 none)
};

// ---- the record ------------------------------------------------------------------------------------------------------------

struct Stat {
    std::int16_t avg = 0, min = 0, max = 0;
};
// Running avg / min / max over int32 samples, saturated to int16 on output.
struct Acc {
    std::int32_t sum = 0, lo = 0, hi = 0;
    std::uint16_t n = 0;
    void add(std::int32_t v);
    Stat stat() const;
};

enum Channel : std::uint8_t { kVbus, kIbus, kAux, kDp, kDm, kCc1, kCc2, kChannels };

struct DropPoint {
    std::uint8_t load = 0;                // Load
    std::uint8_t n = 0;
    Stat vbus, ibus;
    std::int16_t aux = 0;                 // average
};
struct DpRow {
    std::uint8_t drive = 0;               // dp << 4 | dm (dpdm::Drive)
    std::int16_t dp_mv = 0, dm_mv = 0;    // bus side, newest sweeps
    std::uint8_t hid = 0;                 // Hid::bits at the end of the hold
    std::int16_t vbus_max = 0;            // highest VBUS seen during the hold
};
struct PullRow {
    std::uint8_t on = 0;
    std::int16_t dp_mv = 0, dm_mv = 0;
    std::uint8_t hid = 0;
};

inline constexpr std::size_t kFwLen = 30;
inline constexpr std::uint8_t kFlagInProgress = 1 << 0;   // saved before the CC step, cleared when the run ends

struct Record {
    std::uint8_t flags = 0;
    std::array<char, kFwLen> fw{};        // firmware version, NUL padded
    std::uint32_t start_ms = 0;           // uptime at start
    struct StepRec {
        std::uint8_t status = 0;          // Status
        std::uint8_t why = 0;             // Why
        std::uint32_t t_ms = 0;           // start, ms after start_ms
        std::uint16_t dur_ms = 0;
    };
    std::array<StepRec, kSteps> step{};
    // Baseline
    std::uint8_t base_n = 0;
    std::array<Stat, kChannels> base{};
    Hid hid0, hid1;
    Link link;
    std::uint8_t bc = 0;                  // CcRow::bc of the as-found state
    // Drop
    std::uint8_t drop_n = 0;
    std::array<DropPoint, kDropPlan.size()> drop{};
    // DpDm
    std::uint8_t dp_n = 0;
    std::array<DpRow, kDpPlan.size()> dp{};
    std::uint8_t pull_method = 0;         // PullMethod
    std::uint8_t pull_n = 0;
    std::array<PullRow, 2> pull{};
    // Emarker, CC
    Emk emk;
    CcResult cc;

    const StepRec& at(Step s) const { return step[std::size_t(s)]; }
    StepRec& at(Step s) { return step[std::size_t(s)]; }
};

// Layout (little endian), version 1, exactly kRecordBytes:
//   version u8, flags u8, fw[30], start_ms u32
//   5 x step: status u8, why u8, t_ms u32, dur_ms u16                      (Step order: Baseline Drop DpDm Emarker Cc)
//   base_n u8, 7 x (avg, min, max) i16                                      (vbus, ibus, aux, dp, dm, cc1, cc2: mV / mA)
//   hid0, hid1: bits u8, frame u16, resets u16; active_cc u8, link flags u8, guard u8, bc u8
//   drop_n u8, 8 x (load u8, n u8, vbus avg/min/max i16, ibus avg/min/max i16, aux i16)
//   dp_n u8, 6 x (drive u8, dp_mv i16, dm_mv i16, hid u8, vbus_max i16); pull_method u8, pull_n u8,
//   2 x (on u8, dp_mv i16, dm_mv i16, hid u8)
//   emk: valid u8, num_vdos u8, 6 x vdo u32, seq u16, read_mode u8
//   cc: refusal u8, sw0 u8, pc u8, flags u8, n u8, 5 x (cfg u8, cc1_raw u16, cc2_raw u16, bc u8, vbus_mv u16)
inline constexpr std::size_t kRecordBytes = 399;

// 0 when `out` is too small.
std::size_t encode(const Record& r, std::span<std::uint8_t> out);
// nullopt for a wrong length or version.
std::optional<Record> decode(std::span<const std::uint8_t> in);

// The flash file: "LPR" + version, length u16, CRC-16 (0xFFFF seed, over the record), then the record.
inline constexpr std::size_t kFileHeader = 8;
inline constexpr std::size_t kFileBytes = kFileHeader + kRecordBytes;
std::size_t encode_file(const Record& r, std::span<std::uint8_t> out);
std::optional<Record> decode_file(std::span<const std::uint8_t> in);

// ---- the store and the boot check -----------------------------------------------------------------------------------------

class Store {
public:
    virtual bool read(std::span<std::uint8_t> out, std::size_t& n) = 0;
    virtual bool write(std::span<const std::uint8_t> in) = 0;
protected:
    ~Store() = default;
};
// Saves `r` as the last result. False when the write failed.
bool save(Store& s, const Record& r);
// The last result on flash, nullopt when there is none or it does not decode.
std::optional<Record> load(Store& s);
// At boot: a record still marked in progress means the K2 restarted inside the CC step. It is closed (the CC step
// Interrupted / Rebooted, the in-progress flag cleared) and saved. Returns the step that was interrupted, nullopt when
// there was nothing to close.
std::optional<Step> boot_check(Store& s);

// ---- the runner ------------------------------------------------------------------------------------------------------------

class Env {
public:
    virtual Sample sample() = 0;                       // one direct reading of the monitor and the ADC
    virtual std::int32_t vbus_fast_mv() = 0;           // the newest VBUS sample, -1 unknown
    virtual Hid hid() = 0;
    virtual Link link() = 0;
    virtual void load(Load l) = 0;                     // backlight on/off (the busy part is spin())
    virtual void spin(std::uint32_t ms) = 0;           // keep the CPU busy, servicing the PD chip and USB every ms
    virtual void drive(DpState s) = 0;                 // bus-side D+/D-
    virtual bool bus_dpdm(std::int32_t& dp_mv, std::int32_t& dm_mv) = 0;   // newest sweeps, false when the ADC has none
    virtual void dpdm_release() = 0;                   // 0 V hold, then the passive idle (dpdm::start_reset)
    virtual PullMethod pull(bool on) = 0;              // the HID D+ pull-up
    virtual void pull_restore() = 0;
    virtual Emk emarker() = 0;
    virtual void cc_hold(std::span<const std::uint8_t> plan, std::uint16_t hold_us, CcResult& out) = 0;
    virtual bool save(const Record& r) = 0;
protected:
    ~Env() = default;
};

class Runner {
public:
    explicit Runner(Env& e) : env_(e) {}
    // Checks the preconditions with one sample; Ok = running. `fw` is the version string recorded.
    Start start(std::uint32_t now_ms, const char* fw);
    // Advances the run; call every UI tick. Never blocks longer than one spin (kSpinMs) or one CC hold.
    void poll(std::uint32_t now_ms);
    // Ends a running probe now: lines and backlight restored, the step marked Aborted / Cancelled, the record saved.
    void cancel(std::uint32_t now_ms);

    bool running() const { return phase_ != Phase::Idle; }
    bool finished() const { return done_; }
    Step step() const { return step_; }
    const Record& record() const { return rec_; }

private:
    enum class Phase : std::uint8_t { Idle, Baseline, DropSettle, DropSample, DpDischarge, DpHold, PullSettle, Cc };

    void begin(Step s, std::uint32_t now);
    void end(Step s, Status st, Why why, std::uint32_t now);
    void next_after(Step s, std::uint32_t now);
    void abort(Why why, std::uint32_t now);
    void restore();
    bool guard(const Sample& s, std::uint32_t now);   // false: the run ended (VBUS / aux too high, no monitor)
    bool guard_fast(std::uint32_t now);               // the same on the newest VBUS sample alone
    bool host_on_hid() const;
    void start_drop(std::uint32_t now);
    void start_dp(std::uint32_t now);
    void start_emarker(std::uint32_t now);
    void start_cc(std::uint32_t now);
    void finish(std::uint32_t now);

    Env& env_;
    Record rec_{};
    Phase phase_ = Phase::Idle;
    Step step_ = Step::Baseline;
    bool done_ = false;
    bool drove_ = false, pulled_ = false, loaded_ = false;
    std::uint8_t idx_ = 0;                 // baseline sample / drop level / D+/D- row
    std::uint8_t sub_ = 0;                 // drop sample / pull row
    std::uint32_t t_ = 0;                  // phase timer start
    std::uint32_t last_sample_ = 0;
    std::array<Acc, kChannels> acc_{};
    std::int32_t vmax_ = 0;
};

const char* step_name(Step s);              // "Baseline", "Drop", "D+/D-", "E-marker", "CC"
const char* why_name(Why w);

}  // namespace k2::loopprobe
