// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Battery efficiency recorder, pure logic (docs/BATTERY_RECORDER.md): what flows into and out of a power bank over hours,
// phase by phase, from the power monitor's samples. No hardware: the K2 glue is k2/battery/battery_hw.hpp, the host tests drive
// the same classes with made-up samples.
//
// Sign. The samples carry the driver's convention (k2/drivers/power_calc.hpp): positive current = forward = input port to
// output port. A phase maps the two directions to the bank's: "in" = energy into the bank, "out" = energy out of it.
//   same port: the bank stays on the output side, so forward = in and reverse = out in every phase;
//   moved:     the bank sits on the output side while it charges (forward = in) and on the input side, feeding a load on the
//              output, while it discharges (forward = out).
// Integration. Trapezoid over the real time between two samples (mW x ms = uJ, mA x ms = uC); a step whose current changes
// sign is split at the zero crossing, so the two directions never cancel. |I| under kDeadbandMa counts as 0. A step longer
// than kGapMs (a stalled loop, a reset) is not integrated, only counted in the phase's `gaps`.
// Phase end. A phase ends by itself once |I| has reached the end threshold (Config::end_ma) and then stayed under it for
// Config::end_s; the phase time stops where the quiet began. OK / the remote ends it by hand.
// Round trip = Charge, then Discharge. Between the two the recorder waits (nothing counted) until OK, or until the current
// flows out of the bank at the end threshold for kStartHoldMs after the swap was seen (same port: a quiet moment; moved: VBUS
// gone, the bank unplugged); those seconds count into the discharge.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace k2::fs { class Fs; }

namespace k2::battery {

enum class Mode : std::uint8_t { Charge, Discharge, RoundTrip };
enum class Wiring : std::uint8_t { SamePort, Moved };
enum class Kind : std::uint8_t { Charge, Discharge };
// Why a phase ended: Running (it has not), Auto (the current stayed under the threshold), Manual (OK or the remote), Lost (the
// K2 lost power while it measured: its totals were never written, the record says so instead of showing zeros as a result).
enum class End : std::uint8_t { Running, Auto, Manual, Lost };
// Idle: no run yet this boot. Measuring: a phase runs. Between: a round trip waits for its discharge. Done: the run is over
// and its record stays on show until the next start.
enum class State : std::uint8_t { Idle, Measuring, Between, Done };

struct Config {
    std::uint16_t end_ma = 50;   // the phase-end threshold on |I|
    std::uint16_t end_s = 60;    // how long |I| stays under it
};
inline constexpr std::uint16_t kEndMaMin = 20, kEndMaMax = 2000, kEndSMin = 10, kEndSMax = 3600;
constexpr bool valid(const Config &c) {
    return c.end_ma >= kEndMaMin && c.end_ma <= kEndMaMax && c.end_s >= kEndSMin && c.end_s <= kEndSMax;
}
inline constexpr std::int32_t kDeadbandMa = 20;        // idle noise: |I| below this counts as 0
inline constexpr std::uint32_t kGapMs = 5000;          // a step longer than this is a gap, not integrated
inline constexpr std::uint32_t kStartHoldMs = 5000;    // a round trip's discharge starts after this much outflow
inline constexpr std::int32_t kVbusGoneMv = 1000;      // moved wiring: VBUS under this = the bank was unplugged

constexpr std::int32_t deadband(std::int32_t ma) { return ma > -kDeadbandMa && ma < kDeadbandMa ? 0 : ma; }

// Both directions of one stretch of time, in uJ and uC.
struct Flow {
    std::uint64_t uj_fwd = 0, uj_rev = 0, uc_fwd = 0, uc_rev = 0;
};
// One trapezoid step from (mv0, ma0) to (mv1, ma1) over dt_ms, split at the current's zero crossing. Currents as given (the
// caller applies deadband()).
void integrate(Flow &f, std::int32_t mv0, std::int32_t ma0, std::int32_t mv1, std::int32_t ma1, std::uint32_t dt_ms);

// The V / A curve of a run: kPoints averages of equal length, the interval doubling (pairs averaged) whenever the buffer
// fills, so any run length fits. A point is 3 bytes: VBUS in 10 mV (12 bits, to 40.95 V) and the signed current in 4 mA
// (12 bits, +-8.19 A), forward positive. Only measured time feeds it (not the wait between two phases).
struct Curve {
    static constexpr int kPoints = 60;
    static constexpr std::uint16_t kFirstIntervalS = 10;
    std::array<std::uint8_t, kPoints * 3> pts{};
    std::uint8_t n = 0;
    std::uint8_t split = 0xFF;            // the first point of the second phase, 0xFF = none
    std::uint16_t interval_s = kFirstIntervalS;
    std::int64_t acc_mv_ms = 0, acc_ma_ms = 0;
    std::uint32_t acc_ms = 0;

    void add(std::uint32_t dt_ms, std::int32_t mv, std::int32_t ma);   // dt_ms of (mv, ma) on average
    void mark_split() { if (split == 0xFF) split = n; }
    void point(int i, std::int32_t &mv, std::int32_t &ma) const;
    static void pack(std::uint8_t *p, std::int32_t mv, std::int32_t ma);
    static void unpack(const std::uint8_t *p, std::int32_t &mv, std::int32_t &ma);
};

// One phase in RAM, full precision.
struct Phase {
    Kind kind = Kind::Charge;
    End end = End::Running;
    std::uint16_t gaps = 0;
    std::uint32_t ms = 0;                 // measured time (the quiet tail of an auto end taken off)
    std::uint64_t uj_in = 0, uj_out = 0, uc_in = 0, uc_out = 0;
};

// ---- the stored record (flash file, remote read, QR share): 246 bytes, little endian ------------------------------------
//   0  'B' 'R'   2 version   3 mode | wiring << 4   4 state (0 measuring, 1 between, 2 done)
//   5  phases (bits 0..1) | resumes << 2   6 id u16   8 end_ma u16   10 end_s u16   12 interval_s u16   14 points   15 split
//   16 phase 0, 40 phase 1 (24 bytes each): kind, end, gaps u16, seconds u32, uWh in u32, uWh out u32, uAh in u32, uAh out u32
//   64 points (Curve::kPoints x 3)   244 CRC-16/CCITT of bytes 0..243
inline constexpr std::uint8_t kRecordVersion = 1;
inline constexpr std::size_t kPhaseBytes = 24, kHeadBytes = 64;
inline constexpr std::size_t kRecordBytes = kHeadBytes + Curve::kPoints * 3 + 2;
static_assert(kRecordBytes == 246);

enum class RunState : std::uint8_t { Measuring, Between, Done };
struct PhaseRec {
    Kind kind = Kind::Charge;
    End end = End::Running;
    std::uint16_t gaps = 0;
    std::uint32_t secs = 0, uwh_in = 0, uwh_out = 0, uah_in = 0, uah_out = 0;
};
struct Record {
    Mode mode = Mode::Charge;
    Wiring wiring = Wiring::SamePort;
    RunState state = RunState::Done;
    std::uint8_t phases = 0, resumes = 0;
    std::uint16_t id = 0;
    Config cfg{};
    std::uint16_t interval_s = Curve::kFirstIntervalS;
    std::uint8_t points = 0, split = 0xFF;
    std::array<PhaseRec, 2> ph{};
    std::array<std::uint8_t, Curve::kPoints * 3> pts{};
};
std::size_t encode(const Record &r, std::span<std::uint8_t> out);   // kRecordBytes, or 0 when out is short
bool decode(std::span<const std::uint8_t> in, Record &r);            // magic, version, length, CRC and field ranges

// Totals over the phases of a record, and Wh out / Wh in in tenths of a percent (kNoRatio when nothing went in).
struct Sums { std::uint64_t uwh_in = 0, uwh_out = 0, uah_in = 0, uah_out = 0; std::uint32_t secs = 0; };
Sums sums(const Record &r);
inline constexpr std::uint16_t kNoRatio = 0xFFFF;
std::uint16_t ratio_permille(std::uint64_t out, std::uint64_t in);
// A round trip whose two phases both ran: the efficiency is meaningful.
bool has_efficiency(const Record &r);

// ---- the recorder --------------------------------------------------------------------------------------------------------
// One run. Trivially copyable: the K2 keeps a copy in SRAM that a warm reset keeps (the checkpoint).
class Recorder {
public:
    enum class Start : std::uint8_t { Ok, Busy, NoMonitor, BadArg };
    Start start(Mode m, Wiring w, const Config &c, std::uint16_t id);
    // One power-monitor sample (`online` false: the monitor did not answer; nothing is integrated, the next sample primes).
    void sample(std::uint32_t now_ms, std::int32_t mv, std::int32_t ma, bool online);
    bool end_phase();     // OK while measuring: the phase ends by hand (round trip phase 1 -> Between, else Done)
    bool next();          // OK while Between: the discharge starts now
    bool finish();        // ends the run wherever it is (a running phase ends by hand; Between closes with one phase)
    void set_config(const Config &c) { if (valid(c)) cfg_ = c; }   // applies to the run at once (its end rule)

    State state() const { return state_; }
    bool active() const { return state_ == State::Measuring || state_ == State::Between; }
    Mode mode() const { return mode_; }
    Wiring wiring() const { return wiring_; }
    const Config &config() const { return cfg_; }
    std::uint16_t id() const { return id_; }
    int phases() const { return phases_; }                      // phases begun (0..2)
    int current() const { return phases_ ? phases_ - 1 : 0; }   // the phase measuring now, or the last one
    const Phase &phase(int i) const { return ph_[i < 0 ? 0 : i > 1 ? 1 : i]; }
    std::uint8_t resumes() const { return resumes_; }
    const Curve &curve() const { return curve_; }
    // The flash record is out of date (a start, a phase boundary, the end): the glue writes it when it may.
    bool dirty() const { return dirty_; }
    void clean() { dirty_ = false; }

    void to_record(Record &r) const;
    // A run read back from flash at boot. Between resumes as Between (moved wiring: the swap counts as seen, a power cut is
    // what moving the bank usually is); Measuring cannot resume (its totals since the last write are gone): the phase is
    // marked Lost and the run is Done, dirty. Done stays Done. False for a record that is not a run.
    bool restore(const Record &r);
    // A warm reset kept this object in SRAM: the run goes on; the reset gap is not integrated (one gap counted).
    void resumed();

private:
    void begin_phase(Kind k);
    void close_phase(End e, std::uint32_t tail_ms);
    void step(std::uint32_t dt, std::int32_t mv, std::int32_t ma, Phase &p);
    bool in_is_forward(Kind k) const { return !(wiring_ == Wiring::Moved && k == Kind::Discharge); }
    std::int32_t out_ma(std::int32_t ma) const;   // the current out of the bank in the discharge (positive = out)

    Mode mode_ = Mode::Charge;
    Wiring wiring_ = Wiring::SamePort;
    State state_ = State::Idle;
    Config cfg_{};
    std::uint16_t id_ = 0;
    std::uint8_t phases_ = 0, resumes_ = 0;
    bool dirty_ = false;
    bool primed_ = false;
    bool armed_ = false;          // measuring: |I| reached the threshold; between: the swap was seen
    bool quiet_ = false;          // measuring: |I| is under the threshold since quiet_ms_; between: outflow since quiet_ms_
    std::uint32_t last_ms_ = 0, quiet_ms_ = 0;
    std::int32_t last_mv_ = 0, last_ma_ = 0;
    std::array<Phase, 2> ph_{};
    Curve curve_{};
};
static_assert(std::is_trivially_copyable_v<Recorder>);

// ---- the stored runs: /bt<id>.rec, the newest kKeep kept ------------------------------------------------------------------
// Inline files (246 B each) in the root directory: three of them fit the root metadata pair's slack beside the settings files,
// so the runs cost no block of their own; a /bt directory would cost its own pair (2 blocks) and a fourth run splits the
// pair (4 blocks), more than the PD Stress build has left (tests/test_volume_budget.cpp, docs/ARCHITECTURE.md).
namespace store {
inline constexpr const char *kDir = "/";
inline constexpr int kKeep = 3;
using Path = std::array<char, 16>;
Path path_of(std::uint16_t id);
// The ids on the volume, newest (highest) first; returns how many (at most `max`).
int list(fs::Fs &f, std::uint16_t *ids, int max);
bool load(fs::Fs &f, std::uint16_t id, Record &r);
bool save(fs::Fs &f, const Record &r);
void prune(fs::Fs &f, int keep);      // removes the oldest beyond `keep`
std::uint16_t next_id(fs::Fs &f);
}  // namespace store

// ---- the service: the recorder, its record on flash and its SRAM checkpoint ----------------------------------------------
// Flash writes only at a phase boundary (start, end, the discharge's start, the finish) and only when the caller allows them
// (no charger test or PD Stress run, which own the flash while they run): never in the middle of a measuring phase.
class Service {
public:
    // The checkpoint a warm reset keeps (SRAM outside .bss): the recorder with a magic and a CRC-16 of its bytes.
    struct Checkpoint {
        std::uint32_t magic;
        std::uint32_t crc;
        Recorder rec;
    };
    static_assert(std::is_trivially_copyable_v<Checkpoint>);
    static constexpr std::uint32_t kMagic = 0x42415431;   // "1TAB"

    // Boot: the newest run on the volume and, after a warm reset, the checkpoint. Returns what happened (for the log).
    enum class Boot : std::uint8_t { Nothing, ResumedRam, ResumedBetween, Lost, Closed };
    Boot boot(fs::Fs *f, Checkpoint *ckpt, bool warm);
    // Every main-loop pass: the latest sample (online false = no monitor), the deferred write when `may_write`, and the
    // checkpoint once a second.
    void poll(std::uint32_t now_ms, std::int32_t mv, std::int32_t ma, bool online, bool may_write);
    Recorder::Start start(Mode m, Wiring w, bool monitor_online);
    Recorder &rec() { return rec_; }
    const Recorder &rec() const { return rec_; }
    Config &config() { return cfg_; }          // the next run's settings (the running run takes a change at once)
    void set_config(const Config &c) { if (valid(c)) { cfg_ = c; rec_.set_config(c); } }
    Wiring &wiring() { return wiring_; }       // the next run's wiring
    fs::Fs *volume() const { return fs_; }
    bool write_failed() const { return failed_; }

private:
    void checkpoint();
    bool flush();                 // writes the record (and prunes before a new file); false on a failed write
    fs::Fs *fs_ = nullptr;
    Checkpoint *ckpt_ = nullptr;
    Recorder rec_{};
    Config cfg_{};
    Wiring wiring_ = Wiring::SamePort;
    std::uint32_t ckpt_ms_ = 0, retry_ms_ = 0;
    bool failed_ = false;
    bool may_write_ = true;       // the last poll's permission: a start flushes an unwritten earlier run only when allowed
    State ckpt_state_ = State::Idle;   // the run's state the checkpoint holds: a change is checkpointed at the next poll
};

}  // namespace k2::battery
