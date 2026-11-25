// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test reboot-resume journal. A handover or PD reset can cut VBUS and reboot a K2 that is
// powered from VBUS; the run is therefore kept on the littlefs volume (k2/core/fs.hpp) and rebuilt at the next boot.
// Pure logic over k2::fs::Fs, host-tested by tests/test_charger_journal.cpp on a RAM disk that can lose power.
//
// Format 3. The plan is five independent section files, so a plan change rewrites only the section it touched:
//   /jr/h.rec       Header  (run id, selection, evidence, per section: row count + layout hash, and the plan order); written LAST by an arm
//   /jr/pd.sec      PD fixed, EPR fixed, the EPR levels, PD revision rows
//   /jr/pps.sec     PPS, SPR AVS, EPR AVS
//   /jr/qc.sec      QC2, QC3, QC3+, AFC, FCP, SCP (and their Class B rows)
//   /jr/oem.sec     vendor modes (VOOC, SuperVOOC, vivo) and every Xiaomi row (the 0x2717 chain and its modes)
//   /jr/ext.sec     UFCS and anything else
//   /jr/ph.rec      Phase   (written BEFORE an above 5 V request and before the handover)
//   /jr/rs.rec      Resume  (a resumed run, counted; the planned re-attach's reboot is not written as a resume)
//   /jr/mt.rec      Meta    (unit order, unattended flag, cable identity of the run)
//   /jr/at.rec      Attempt (which unit lost power how many times)
//   /jr/sp.rec      Spec    (the PD spec stage's result: end, why, the cells; written once, when the stage ends)
//   /jr/spd.rec     SpecDetail (the cells' limit, count and place; written right behind sp.rec. A journal without it still loads:
//                   those cells read n = 0 and the detail page shows "-". The record is additive and sp.rec is unchanged)
//   /jr/spt.rec     SpecRows (K2_SPECROWS builds only: the per-transition rows of the spec checks: "SPT1", run id, counts, worst, then the
//                   10 byte rows; written right behind spd.rec, only read back. No file = the page shows the summary cell)
//   /jr/sa1.rec ..  SpecApdo (format 6: one PPS / AVS range's own spec cells, specsec::to_apdo_wire, one record per range in the order
//   /jr/sa4.rec     the run met them; written right behind spd.rec. None = the grid shows the merged section, as before)
//   /jr/fin.rec     Finish  (the run ended: Done, Stopped or abandoned)
// A section file: 8 bytes (id, row count, layout hash u32, crc16 over the whole file but these two bytes) then 18 bytes per
// row (the plan row and its result). Rows are addressed by (section, ordinal): the n-th row of the plan that belongs to the
// section. The layout hash covers what makes a row that row (group, algorithm, position, level, current, flags, PDO), never
// its result, so a verdict rewrites one file and a replan rewrites the sections whose hash moved plus the header.
// The header's last kMaxSteps payload bytes are the plan order the arm/replan read from the planner's step list
// (Runner::steps(), the one structure the grid also uses since 98e2f39): order[k] = the index of the plan's k-th row in the
// section-by-section array the sections are read back into. Replay applies it verbatim, so a resumed run executes exactly
// in the order it was planned in - no second copy of the order (where the 6 A hunt rows sit) is re-derived from the rows.
// Other records: 0 u8 0xA5  1 u8 type  2 u8 payload length (even)  3 u8 0  4.. payload  crc16 (CCITT, bytes 0..3+len).
// littlefs commits a file atomically on close, so a power cut leaves the old file or the new one.
//
// Resume policy: every section named by the header is read and must match it (size, crc, id, row count, layout hash), and the
// order must be a permutation of the rows. Anything that does not makes the run NOT resumable (Problem::BadSection, `bad`
// names the section; a broken order is Problem::BadHeader): a plan that is only partly known would put the pending rows at
// the wrong addresses, and a "last good copy" would need a second copy of every section (flash and a second consistency
// story). The one window is the gap between a section file replaced and the header that names its new hash and order (one
// flash write, tens of ms); it is the only way a clean power cut loses a run, and it is reported, never mis-read.
// Format 6 = format 5 plus the optional /jr/sa<n>.rec (each range's spec cells); a format 3..5 journal loads without them.
// Format 5 = format 4 plus the optional /jr/spt.rec (the spec's per-transition rows, 10 bytes each, read by the detail page and the
// remote, never replayed; a build without K2_SPECROWS neither writes nor reads it, and the format byte is 5 either way).
// Format 4 = format 3 plus the optional /jr/spd.rec (spec cell limit, count, place); the header layout is the same, so a format 3 or 4 journal is read as it is (no rows: the detail page shows its summary). Format 2 (sections, the order re-derived at replay), format 1 (one file per row, header
// payload 30) and any other format byte are Problem::OldFormat: dropped at boot, never read.
//
// Order of an arm: header dropped FIRST (a cut during the rest leaves no run at all), stale files swept, every section
// written, the header LAST. Replay order: header, sections, phase (void unless it still names a pending row), resume, meta,
// attempt, finish.
// The sections hold the rows section by section; replay puts them back in the plan's order with the order the header carries
// (written from the planner's step list), so a resumed run executes in the order it was planned in.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "k2/core/bss_zero.hpp"
#include <cstring>
#include <memory>
#include <span>

#include "k2/core/bbstore.hpp"
#include "k2/core/build_config.h"
#include "k2/chargertest/charger_test.hpp"
#include "k2/chargertest/spec_section.hpp"
#include "k2/core/fs.hpp"

namespace k2::charger_journal {

inline constexpr std::uint8_t kMagic = 0xA5;
inline constexpr const char* kDir = "/jr";
inline constexpr const char* kHeaderPath = "/jr/h.rec"; // k2-plain: fs path
#if K2_SPECROWS
inline constexpr const char* kSpecRowsPath = "/jr/spt.rec"; // k2-plain: fs path
inline constexpr std::size_t kSpecRowsMax = 4096;   // the rows file's ceiling (specsec::kRowsMaxFile is 4010; ui_charger_test.cpp static_asserts it fits)
#endif
inline constexpr std::uint8_t kFormat = 6;       // 6: each range's spec cells in /jr/sa<n>.rec (5: per-transition rows in /jr/spt.rec, 4: cell limit, count, place in /jr/spd.rec)
inline constexpr std::uint8_t kMinFormat = 3;    // a format 3 to 5 journal (no sa, spt or spd records) still loads: its detail shows the summary

// Type::Step is the format 1 record (one row per file); nothing writes it any more.
enum class Type : std::uint8_t { Header = 1, Step = 2, Phase = 3, Finish = 4, Resume = 5, Meta = 6, Attempt = 7, Spec = 8, SpecDetail = 9, SpecApdo = 10 };
using Phase = charger_test::Phase;
enum class Err : std::uint8_t { Ok, TooBig, Flash, Broken };

#if K2_SPECROWS
// The bytes of the spec rows file (specsec::rows_fill), handed to the pump by whoever still holds the engine: the journal stores them
// and never interprets them. size 0 = no rows (the file is removed).
struct SpecRowsSource { std::size_t size = 0; fs::Fs::Fill fill = nullptr; void* ctx = nullptr; };
#endif

enum class Section : std::uint8_t { Pd, Pps, Qc, Oem, Ext };
inline constexpr std::size_t kSections = 5;
// Why a replay found no run although a header was there.
enum class Problem : std::uint8_t { None, OldFormat, BadHeader, BadSection };

inline constexpr std::size_t kHeaderBase = 60;   // evidence + per-section counts and hashes, as in format 2
inline constexpr std::size_t kOrderBytes = charger_test::kMaxSteps;   // the plan order rides in the header, one byte per row
inline constexpr std::size_t kHeaderPayload = kHeaderBase + kOrderBytes, kPhasePayload = 10, kFinishPayload = 2, kResumePayload = 2;
inline constexpr std::size_t kMetaPayload = 24, kAttemptPayload = 4;
inline constexpr std::size_t kSpecPayload = (specsec::kWireBytes + 1) & ~std::size_t(1);   // the wire form, padded to an even length
inline constexpr std::size_t kSpecDetailPayload = specsec::kDetailBytes;                      // 8 bytes per cell, already even
inline constexpr std::size_t kSpecApdoPayload = specsec::kApdoWireBytes;                     // one range, already even
inline constexpr std::size_t kRecordOverhead = 6;
inline constexpr std::size_t kMaxRecord = kRecordOverhead + kHeaderPayload;
inline constexpr std::size_t kSpecDetailRecord = kRecordOverhead + kSpecDetailPayload;   // the spec detail is the one record larger than the header
inline constexpr std::size_t kReadRecord = kSpecDetailRecord > kMaxRecord ? kSpecDetailRecord : kMaxRecord;   // what a read buffer holds
inline constexpr std::size_t kOldHeaderPayload = 30;
inline constexpr std::size_t kSecHeader = 8, kRowBytes = 18;
static_assert(kMetaPayload <= kHeaderPayload && charger_test::kMaxUnits == 16, "the meta record is sized for 16 units");
static_assert(charger_test::kMaxSteps < 255, "a section header counts rows in a byte");
static_assert(kSpecPayload <= kHeaderPayload && kSpecPayload <= 254, "the spec record fits a header-sized buffer");
static_assert(kSpecDetailPayload <= 254 && kSpecDetailPayload % 2 == 0, "the spec detail record is one even length byte; it is the largest record, so a read takes kReadRecord");
static_assert(kHeaderPayload <= 254 && kHeaderPayload % 2 == 0, "a record payload length is one even byte");
static_assert(kSpecApdoPayload <= kHeaderPayload, "a range record fits a header-sized buffer");
static_assert(kSecHeader + kRowBytes * charger_test::kMaxSteps <= fs::kBlockSize - 8, "a section is at most one block");

const char* section_path(Section s);
const char* section_name(Section s);

// The unit-to-section map (docs/CHARGER_TEST_FLOW.md): a row belongs to the section of its algorithm; a Xiaomi row (Step::hunt:
// the 0x2717 chain row and its mode rows) is a vendor mode wherever it sits in the plan, so it goes to oem.
Section section_of(const charger_test::Step& s);

std::size_t section_rows(std::span<const charger_test::Step> steps, Section sec);
// Hash of the section's layout: its rows in plan order, without anything a test result changes. FNV-1a, seeded with the section.
std::uint32_t layout_sig(Section sec, std::span<const charger_test::Step> steps);

// What a resumed run needs besides its steps: the unit order the user chose, whether anyone is at the K2 (an unattended run
// resumes by itself; the K2 may be powered by the charger under test), and the cable identity the run presented (ctcable
// kind and sequence), so the Ra mode / spoof can be armed again before the first step.
struct RunMeta {
    charger_test::UnitList units{};
    bool unattended = false;
    bool pps_sweep = false;   // the PPS deep sweep was on (meta byte p[1] bit 1; older journals read 0 = off)
    bool avs_sweep = false;   // the AVS deep sweep was on (meta byte p[1] bit 2; older journals read 0 = off)
    charger_test::Level level = charger_test::Level::Medium;   // the run's level (meta byte p[1] bits 3..4; older journals read 0 = Medium)
    ctsweep::Depth sweep_depth = ctsweep::Depth::Step100;      // the deep sweeps' step (p[1] bits 5..6; older journals read 0 = 100 mV, what they ran)
    std::uint8_t cable_kind = 0;
    std::uint16_t cable_seq = 0;
    // What the Ra guard read when an identity was started: 0 = not judged (no identity, or a journal written before this
    // byte existed), else raguard::Topology + 1; bit 7 = the user started anyway against a refusing reading.
    std::uint8_t guard = 0;
};

// What the records say about the last run. `unfinished` = it neither ended nor has all steps final.
struct Replay {
    bool found = false, unfinished = false;
    Problem problem = Problem::None;    // set when found is false although a header was there
    Section bad = Section::Pd;          // BadSection: the section that did not match
    std::uint32_t run_id = 0;
    charger_test::Options options{};
    charger_test::Evidence evidence{};
    specsec::Apdos spec_apdo{};         // each range's spec cells (format 6; none with no spec result)
    std::array<charger_test::Step, charger_test::kMaxSteps> steps{};   // the plan's order (the order the header carries, from the planner's step list)
    std::size_t count = 0;
    std::uint8_t resumed = 0;
    Phase phase = Phase::None;          // marker with no final result after it
    std::size_t phase_index = 0;        // == count: the marker was written past the last row
    bool has_meta = false;
    RunMeta meta{};
    std::size_t attempt_index = 0;      // the unit (charger_test::Unit number) whose interruptions are counted ...
    std::uint8_t attempts = 0;          // ... and how many times power went while one of its steps was in flight
};

// Clears a Replay in place. `r = Replay{}` builds a ~2.6 KB temporary on the stack first (it was the largest
// frame of the whole firmware); construct_at value-initialises the object where it lies.
void reset(Replay& r);

namespace detail {
inline void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void put32(std::uint8_t* p, std::uint32_t v) { put16(p, std::uint16_t(v)); put16(p + 2, std::uint16_t(v >> 16)); }
inline std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
inline std::uint32_t get32(const std::uint8_t* p) { return get16(p) | (std::uint32_t(get16(p + 2)) << 16); }
}  // namespace detail

// Encoders write the whole record (with CRC) into `out` and return its length.
std::size_t finish_record(std::span<std::uint8_t> out, Type t, std::size_t len);
// What the header says about the plan: row count and layout hash of each section file, and the plan order over the
// section-by-section array (order[k] = the index of the plan's k-th row there; 0xFF past the last row).
struct SectionInfo {
    std::array<std::uint16_t, kSections> rows{};
    std::array<std::uint32_t, kSections> sig{};
    std::array<std::uint8_t, kOrderBytes> order{};
};
std::size_t encode_header(std::span<std::uint8_t> out, std::uint32_t run_id, const charger_test::Options& o,
                          const charger_test::Evidence& e, const SectionInfo& si);
// One plan row and its result, 18 bytes.
void encode_row(std::uint8_t* p, const charger_test::Step& s);
bool decode_row(const std::uint8_t* p, charger_test::Step& s);
// Phase marker: which row (section and place in it), checked at replay against what the row is (group, algorithm, position,
// level), so a plan that moved under the marker voids it instead of pointing it at another row. section 0xFF = past the last row.
inline constexpr std::uint8_t kPastEnd = 0xFF;
std::size_t encode_phase(std::span<std::uint8_t> out, Phase ph, std::span<const charger_test::Step> steps, std::size_t idx);
std::size_t encode_finish(std::span<std::uint8_t> out, std::uint8_t state, std::uint8_t reason);
std::size_t encode_resume(std::span<std::uint8_t> out, std::uint8_t count);
std::size_t encode_meta(std::span<std::uint8_t> out, const RunMeta& m);
std::size_t encode_attempt(std::span<std::uint8_t> out, std::size_t idx, std::uint8_t count);
std::size_t encode_spec(std::span<std::uint8_t> out, const specsec::Result& s);
std::size_t encode_spec_detail(std::span<std::uint8_t> out, const specsec::Result& s);
std::size_t encode_spec_apdo(std::span<std::uint8_t> out, const specsec::Apdo& a);

// Folds one valid small record (resume, meta, attempt, finish, spec) into the replay state. Header, rows and the phase marker need the
// sections and are handled by Journal::replay.
void apply(Replay& r, Type t, std::span<const std::uint8_t> p);

// Scans a region image; calls `fn(type, payload)` for every record whose CRC holds. Bytes that do not
// start a valid record (blank, torn, stale) are stepped over half-word by half-word. Returns the offset
// just past the last non-blank half-word (the append position).
template <class Fn>
std::size_t scan(std::span<const std::uint8_t> img, Fn&& fn) {
    std::size_t pos = 0, end = 0;
    while (pos + 2 <= img.size()) {
        if (img[pos] == 0xFF && img[pos + 1] == 0xFF) { pos += 2; continue; }
        end = pos + 2;
        if (img[pos] == kMagic && pos + 4 <= img.size()) {
            const std::size_t len = img[pos + 2], total = 4 + len + 2;
            if (len % 2 == 0 && pos + total <= img.size() &&
                bbstore::crc16(0xFFFF, {img.data() + pos, 4 + len}) == detail::get16(img.data() + pos + 4 + len)) {
                fn(Type(img[pos + 1]), std::span<const std::uint8_t>(img.data() + pos + 4, len));
                pos += total; end = pos;
                continue;
            }
        }
        pos += 2;
    }
    return end;
}

// Decides whether the replay still needs the user: a run without Finish and with work left.
void conclude(Replay& r);

// A run's rows come off the disk section by section; the Runner wants them in the plan's order - the order of the planner's step
// list (Runner::steps()) the sections were written from, which the header carries (format 3). encode_order is the mechanical
// translation of that list into section-by-section indexing (a stable partition by section, no rule of its own), so the 6 A hunt
// rows land exactly where the planner emitted them, whatever the run did to a row in between. order[k] = the index in `rows`
// of the plan's k-th row; 0xFF past the last row.
namespace detail {
void encode_order(std::span<const charger_test::Step> steps, std::uint8_t* order);
// The fingerprint of the plan order the header names, so a replan that moved rows (a re-announce re-placing a hunt, a dropped
// row) rewrites the header too even when no section's rows or hash changed.
std::uint32_t order_sig(std::span<const charger_test::Step> steps);
// rows[k] = old rows[order[k]], in place (cycle by cycle, one spare row).
void apply_order(std::span<charger_test::Step> rows, const std::uint8_t* order);
}  // namespace detail

// The writer / reader over a volume. All calls belong to the main loop.
//
// What a call holds the main loop (and with it the PD engine) for is the flash it writes: a small record is one inline directory
// commit (12 to 17 ms, now and then a directory compaction of 130 to 210 ms); a section file over 256 bytes is a block erase
// (35 ms) plus 0.2 ms per half-word, about 40 ms for the 11 rows of the FH22 PPS section and about 240 ms for a section of the
// largest plan there is. pump() does at most ONE such write per call and returns to the main loop between two, so the longest
// tick the journal causes is one section file. Writes are noted (note / note_plan) and flushed by pump(): nothing in a
// plan change or a verdict blocks. settle() and begin() are the same work run to the end in one call, for tests and callers with
// no link to serve.
class Journal {
public:
    explicit Journal(fs::Fs& f);

    // Rebuilds the last run from the files.
    void replay(Replay& r) const;
    bool broken() const { return broken_; }
    // The filesystem error behind the last Err::Flash (Ok when none since begin): the device log names it.
    fs::Err last_fs() const { return last_fs_; }

    // A run's start. start_arm() only notes the work; each pump() call then does at most ONE write (plus up to kLookups cheap
    // lookups that find nothing to do) and returns to the main loop. Order: header dropped FIRST (a cut during the rest leaves
    // no run at all), stale files swept, every section written, the header LAST.
    static constexpr int kLookups = 24;
    void start_arm();
    // Drops what is still to be written (the run is being closed).
    void cancel();
    // An arm is in progress (the run is not yet on flash).
    bool working() const { return stage_ != Stage::Idle; }
    // Something is still to be written: an arm in progress, a section to rewrite, or a header that does not name what the
    // section files hold or the order it was written with. The Runner does not start an above-5 V request while this is true.
    bool pending() const;
    // The same, for the plan's SHAPE only: an arm, a section whose rows or layout changed, the header naming them, the plan's
    // order moved. What the phase marker's address depends on; a section that is only dirty for a verdict does not hold anything back.
    bool plan_pending() const;

    // A row of the plan changed (its result): its section file is rewritten by a later pump().
    void note(std::span<const charger_test::Step> steps, std::size_t idx);
    // The plan may have changed shape (rows added, dropped, moved): the sections whose row count or layout hash differ from what
    // is on flash are rewritten, and the header after them - also when only the plan's ORDER moved (a re-announce re-placing a
    // row, a dropped hunt): the header names the order, so it is rewritten too. Costs a hash pass, no flash.
    void note_plan(std::span<const charger_test::Step> steps);
    // The PD spec stage ended: Evidence::spec is written by a later pump() (one small record, behind any section or header write),
    // then each range of `apdo` (it must outlive the writes: the Runner's own).
    void note_spec(const specsec::Apdos* apdo = nullptr);
    // The journal now mirrors a run read back at boot (its sections are on flash as read): later changes are written as changes.
    void adopt(const Replay& r);
    // A journal in the old layout is dropped at boot (the header only; the next arm sweeps the rest).
    void discard_old();

    Err pump(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
             std::span<const charger_test::Step> steps
#if K2_SPECROWS
             , const SpecRowsSource* rows = nullptr
#endif
             );
    Err begin(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
              std::span<const charger_test::Step> steps);
    // pump() until nothing is left (or a write failed).
    Err settle(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
               std::span<const charger_test::Step> steps);
    // `steps` is the live plan: the marker names row `idx` by section and place in it. idx == steps.size() = past the last row.
    Err phase(Phase ph, std::span<const charger_test::Step> steps, std::size_t idx);
    Err resumed(std::uint8_t count);
    Err meta(const RunMeta& m);
    // The loss counter of a unit: count interruptions charged to unit `idx` (the third one skips the rest of the unit,
    // charger_test::kRebootBudget).
    Err attempt(std::size_t idx, std::uint8_t count);
    // The PD spec stage's result (it is final when the stage ends; a resumed run shows it and never repeats the stage).
    Err spec(const specsec::Result& s);
    // The detail of the same result (limit, count and place per cell): its own file, written by the pump right behind sp.rec.
    Err spec_detail(const specsec::Result& s);
    // One range's spec cells (idx = its place in Apdos, the file /jr/sa<idx + 1>.rec).
    Err spec_apdo(std::size_t idx, const specsec::Apdo& a);
#if K2_SPECROWS
    // The per-transition rows of the spec result (specsec::rows_fill): their own file, written by the pump right behind spd.rec from the
    // source the caller hands the pump. No source or no rows (size 0) writes nothing and removes a file left by an earlier run. The file
    // is read back by the detail page, never replayed (the format stays 5 for a journal without it: the page shows the summary).
    Err spec_rows(const SpecRowsSource* src);
    // The size of the rows file on flash (0 = none) and a piece of it.
    std::size_t spec_rows_size() const { std::size_t n = 0; return fs_.size_of(kSpecRowsPath, n) == fs::Err::Ok ? n : 0; }
    fs::Err spec_rows_read(std::uint32_t off, std::span<std::uint8_t> out, std::size_t& n) const { return fs_.read_at(kSpecRowsPath, off, out, n); }
#endif
    // Closes the run: the next boot does not resume it, whatever its rows say. Tried even after an earlier write failed (broken_):
    // a lost record must not keep a run that ended open for ever (it would come back on every boot).
    Err finish(std::uint8_t state, std::uint8_t reason);

private:
    static constexpr std::uint16_t kUnknown = 0xFFFF;
    enum class Stage : std::uint8_t { Idle, Mkdir, DropHeader, Sweep, Sections, Header, Late };
    static constexpr std::uint8_t bit(Section s) { return std::uint8_t(1u << unsigned(s)); }
    static constexpr std::uint8_t kSectionMask = 0x1F, kOrderBit = 0x20, kSpecBit = 0x40, kSpecDetailBit = 0x80;   // dirty_ bits: one per section, "the header's plan order is stale", "the spec result is to be written", "and its detail"
    bool live() const { return armed_ || stage_ != Stage::Idle; }
    void forget();
    // The header names the section files as they were last written, and the plan order as it was last written.
    bool header_stale() const;

    // One write at rest: the first section to rewrite, else the header that names what the section files hold.
    Err flush(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
              std::span<const charger_test::Step> steps
#if K2_SPECROWS
              , const SpecRowsSource* rows
#endif
              );
    void clear_dirty();
    // Writes the first section marked dirty (one file); `did` = a section was handled.
    Err write_dirty(std::span<const charger_test::Step> steps, bool& did);
    // Writes (or, for a section with no rows, removes) the section file and notes what it now holds. `flash` = a file was
    // written or removed (as against a lookup that found nothing to do).
    Err write_section(Section sec, std::span<const charger_test::Step> steps, bool* flash = nullptr);
    Err write_header(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
                     std::span<const charger_test::Step> steps);

    // The section file is generated chunk by chunk (fs::Fs::write_chunks): only one row at a time is ever encoded.
    static constexpr std::size_t kNone = ~std::size_t(0);
    struct SecCtx {
        std::span<const charger_test::Step> steps;
        Section sec;
        std::size_t rows;
        std::uint32_t sig;
        std::uint16_t crc;
        std::size_t last_n;     // cursor: the row ordinal last asked for and where it sits in `steps`
        std::size_t last_pos;
    };
    static const charger_test::Step& nth_row(SecCtx& c, std::size_t n);
    static void put_sec_header(std::uint8_t* h, const SecCtx& c);
    static std::uint16_t section_crc(SecCtx& c);
    static void fill_section(void* ctx, std::size_t off, std::span<std::uint8_t> out);

    // ---- reading
    // The header record: format and counts. OldFormat for format 1 (payload 30) and any format byte outside kMinFormat..kFormat.
    [[gnu::noinline]] static Problem parse_header(std::span<const std::uint8_t> img, Replay& r, SectionInfo& si);
    // Reads one section file into out[0..rows) and checks it against the header: size, id, row count, hash, crc.
    [[gnu::noinline]] bool read_section(Section sec, std::size_t rows, std::uint32_t sig, charger_test::Step* out) const;
    // The phase marker, mapped to the replayed rows: void when it names a row that is not what it was (the plan moved), or when
    // that row or a later one of its section already has a result.
    [[gnu::noinline]] void replay_phase(Replay& r, const std::array<std::size_t, kSections>& base, const SectionInfo& si,
                                        std::array<std::uint8_t, kReadRecord>& b) const;
    bool read_small(const char* path, std::array<std::uint8_t, kReadRecord>& b, Replay& r) const;

    // ---- housekeeping
    static bool is_section_file(const char* name);
    // The path of one file in /jr that is not a section file of this format (format 1 rows, the records of the last run).
    bool stale(char (&path)[fs::kPathMax]);
    Err stop(Err e);
    // `removed` = a file was there and is gone now (a commit), as against nothing to remove (a lookup).
    bool drop(const char* path, bool* removed = nullptr);
    Err put(const char* path, std::span<const std::uint8_t> rec);
    Err fail();
    Err fail(fs::Err why);

    fs::Fs& fs_;
    bool broken_ K2_BSS_INIT(false), armed_ K2_BSS_INIT(false);
    fs::Err last_fs_ K2_BSS_INIT(fs::Err::Ok);
    Stage stage_ K2_BSS_INIT(Stage::Idle);
    std::size_t cur_ K2_BSS_INIT(0);
    std::uint8_t dirty_ K2_BSS_INIT(0);                       // sections whose file is to be rewritten (bit per Section)
    std::uint8_t apdo_dirty_ K2_BSS_INIT(0);                  // a bit per range of *apdo_ still to write
    const specsec::Apdos* apdo_ K2_BSS_INIT(nullptr);
#if K2_SPECROWS
    bool rows_dirty_ K2_BSS_INIT(false);                      // the spec rows (/jr/spt.rec) are to be written, from the source the pump is given
    bool rows_pending() const { return rows_dirty_; }
#else
    static constexpr bool rows_pending() { return false; }
#endif
    std::uint8_t plan_dirty_ K2_BSS_INIT(0);                  // the subset of dirty_ whose rows or layout changed (not only a verdict)
    std::array<std::uint16_t, kSections> srows_ K2_BSS_ZERO, hrows_ K2_BSS_ZERO;   // row count of each section file / as the header names it
    std::array<std::uint32_t, kSections> ssig_ K2_BSS_ZERO, hsig_ K2_BSS_ZERO;     // layout hash of each section file / as the header names it
    std::uint32_t hord_sig_ K2_BSS_INIT(0);                   // fingerprint of the plan order the header names
    bool hord_known_ K2_BSS_INIT(false);                      // false until a header was written (or a replay adopted)
};

// Write gate. A journal write holds the main loop, and with it every PD reply, for 12 to 250 ms (the flash erases and programs
// with interrupts masked; the app spans both banks, so no read-while-write). A source's own requests (Discover Identity,
// Get_Sink_Cap, Get_Sink_Cap_Extended, VDMs) follow a new contract: 104 on record (2026-11) came a median 208 ms and 95 % within
// 510 ms after PS_RDY, so a quiet link alone (kWriteQuietMs) lets a write start right inside that window
// (runs/ugreen-qa/r2-stream: Get_Sink_Cap mid-write, answered 14.8 ms after the frame). A write starts once the link was quiet
// for kWriteQuietMs AND the last Accept / PS_RDY is kAfterContractMs old, or after `max_wait_ms` of waiting whatever the link
// does (a source that never goes quiet must not keep the records out for ever). A request a source makes later than that, or
// during a forced write, still waits for the write: no time gate can bound it (docs/PD_SINK_REPLIES.md).
inline constexpr std::uint32_t kWriteQuietMs = 80;
inline constexpr std::uint32_t kAfterContractMs = 600;
constexpr bool write_ok(std::uint32_t since_activity_ms, std::uint32_t contract_age_ms, std::uint32_t waited_ms,
                        std::uint32_t max_wait_ms) {
    if (waited_ms >= max_wait_ms) return true;
    return since_activity_ms >= kWriteQuietMs && contract_age_ms >= kAfterContractMs;
}
// The age of the newest contract event: the later of the sink's Accept and PS_RDY stamps (millis; 0 = none, an inferred PS_RDY
// leaves ps_rdy_at 0). No event at all reads as old.
constexpr std::uint32_t contract_age_ms(std::uint32_t now, std::uint32_t accept_at, std::uint32_t ps_rdy_at) {
    if (accept_at == 0 && ps_rdy_at == 0) return 0xFFFFFFFFu;
    const std::uint32_t a = accept_at ? now - accept_at : 0xFFFFFFFFu, p = ps_rdy_at ? now - ps_rdy_at : 0xFFFFFFFFu;
    return a < p ? a : p;
}

// Resume gate: the OK press is accepted only after VBUS sat in vSafe5V (4.75..5.50 V) for kStableMs.
inline constexpr std::uint32_t kResumeStableMs = 500;
class ResumeGate {
public:
    void update(std::uint32_t now, int vbus_mv) {
        if (vbus_mv < 4750 || vbus_mv > 5500) { stable_ = false; return; }
        if (!stable_ || now - last_ > 250) since_ = now;   // a gap in the samples restarts the window
        stable_ = true; last_ = now;
    }
    bool ready(std::uint32_t now) const { return stable_ && now - since_ >= kResumeStableMs; }
    void reset() { stable_ = false; }
private:
    bool stable_ K2_BSS_INIT(false);
    std::uint32_t since_ K2_BSS_INIT(0), last_ K2_BSS_INIT(0);
};

}  // namespace k2::charger_journal
