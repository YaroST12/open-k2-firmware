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
//   /jr/oem.sec     vendor modes (VOOC, SuperVOOC, vivo) and every 6 A hunt row
//   /jr/ext.sec     UFCS and anything else
//   /jr/ph.rec      Phase   (written BEFORE an above 5 V request and before the handover)
//   /jr/rs.rec      Resume  (a resumed run, counted; the planned re-attach's reboot is not written as a resume)
//   /jr/mt.rec      Meta    (unit order, unattended flag, cable identity of the run)
//   /jr/at.rec      Attempt (which unit lost power how many times)
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
// Format 2 (sections, the order re-derived at replay), format 1 (one file per row, header payload 30) and any other format
// byte are Problem::OldFormat: dropped at boot, never read.
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
#include <cstring>
#include <memory>
#include <span>

#include "k2/core/bbstore.hpp"
#include "k2/chargertest/charger_test.hpp"
#include "k2/core/fs.hpp"

namespace k2::charger_journal {

inline constexpr std::uint8_t kMagic = 0xA5;
inline constexpr const char* kDir = "/jr";
inline constexpr const char* kHeaderPath = "/jr/h.rec";
inline constexpr std::uint8_t kFormat = 3;

// Type::Step is the format 1 record (one row per file); nothing writes it any more.
enum class Type : std::uint8_t { Header = 1, Step = 2, Phase = 3, Finish = 4, Resume = 5, Meta = 6, Attempt = 7 };
using Phase = charger_test::Phase;
enum class Err : std::uint8_t { Ok, TooBig, Flash, Broken };

enum class Section : std::uint8_t { Pd, Pps, Qc, Oem, Ext };
inline constexpr std::size_t kSections = 5;
// Why a replay found no run although a header was there.
enum class Problem : std::uint8_t { None, OldFormat, BadHeader, BadSection };

inline constexpr std::size_t kHeaderBase = 60;   // evidence + per-section counts and hashes, as in format 2
inline constexpr std::size_t kOrderBytes = charger_test::kMaxSteps;   // the plan order rides in the header, one byte per row
inline constexpr std::size_t kHeaderPayload = kHeaderBase + kOrderBytes, kPhasePayload = 10, kFinishPayload = 2, kResumePayload = 2;
inline constexpr std::size_t kMetaPayload = 24, kAttemptPayload = 4;
inline constexpr std::size_t kRecordOverhead = 6;
inline constexpr std::size_t kMaxRecord = kRecordOverhead + kHeaderPayload;
inline constexpr std::size_t kOldHeaderPayload = 30;
inline constexpr std::size_t kSecHeader = 8, kRowBytes = 18;
static_assert(kMetaPayload <= kHeaderPayload && charger_test::kMaxUnits == 16, "the meta record is sized for 16 units");
static_assert(charger_test::kMaxSteps < 255, "a section header counts rows in a byte");
static_assert(kHeaderPayload <= 254 && kHeaderPayload % 2 == 0, "a record payload length is one even byte");
static_assert(kSecHeader + kRowBytes * charger_test::kMaxSteps <= fs::kBlockSize - 8, "a section is at most one block");

inline const char* section_path(Section s) {
    static constexpr const char* const kPaths[kSections] = {"/jr/pd.sec", "/jr/pps.sec", "/jr/qc.sec", "/jr/oem.sec", "/jr/ext.sec"};
    return kPaths[std::size_t(s)];
}
inline const char* section_name(Section s) {
    static constexpr const char* const kNames[kSections] = {"pd", "pps", "qc", "oem", "ext"};
    return kNames[std::size_t(s)];
}

// The unit-to-section map (docs/CHARGER_TEST_FLOW.md): a row belongs to the section of its algorithm; a 6 A hunt row is a vendor
// mode wherever it sits in the plan, so it goes to oem.
inline Section section_of(const charger_test::Step& s) {
    using A = charger_test::Algorithm;
    if (s.hunt) return Section::Oem;
    switch (s.algorithm) {
    case A::Pd2Fixed: case A::Pd3Fixed: case A::Epr: case A::Pd30Revision: case A::Pd31Revision: case A::Pd32Revision:
        return Section::Pd;
    case A::Pps: case A::SprAvs: case A::EprAvs:
        return Section::Pps;
    case A::Qc2: case A::Qc3: case A::Qc3Plus: case A::Afc: case A::Fcp: case A::Scp: case A::Qc2ClassB: case A::Qc3ClassB:
        return Section::Qc;
    case A::Vooc: case A::SuperVooc: case A::SuperVooc65: case A::Vivo:
        return Section::Oem;
    default:
        return Section::Ext;
    }
}

inline std::size_t section_rows(std::span<const charger_test::Step> steps, Section sec) {
    std::size_t n = 0;
    for (const auto& s : steps) n += section_of(s) == sec;
    return n;
}
// Hash of the section's layout: its rows in plan order, without anything a test result changes. FNV-1a, seeded with the section.
inline std::uint32_t layout_sig(Section sec, std::span<const charger_test::Step> steps) {
    std::uint32_t h = (0x811C9DC5u ^ std::uint32_t(sec)) * 0x01000193u;
    auto mix = [&](std::uint32_t b) { h = (h ^ (b & 0xFF)) * 0x01000193u; };
    for (const auto& s : steps) {
        if (section_of(s) != sec) continue;
        mix(std::uint32_t(s.group)); mix(std::uint32_t(s.algorithm)); mix(s.pos);
        mix(s.mv); mix(s.mv >> 8); mix(s.ma); mix(s.ma >> 8);
        mix(std::uint32_t((s.continuous ? 1 : 0) | (s.avs ? 2 : 0) | (s.hunt ? 4 : 0)));
        mix(s.pdo); mix(s.pdo >> 8); mix(s.pdo >> 16); mix(s.pdo >> 24);
    }
    return h;
}

// What a resumed run needs besides its steps: the unit order the user chose, whether anyone is at the K2 (an unattended run
// resumes by itself; the K2 may be powered by the charger under test), and the cable identity the run presented (ctcable
// kind and sequence), so the Ra mode / spoof can be armed again before the first step.
struct RunMeta {
    charger_test::UnitList units{};
    bool unattended = false;
    bool pps_sweep = false;   // the deep sweep was on (meta byte p[1] bit 1; older journals read 0 = off)
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
inline void reset(Replay& r) { std::construct_at(&r); }

namespace detail {
inline void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void put32(std::uint8_t* p, std::uint32_t v) { put16(p, std::uint16_t(v)); put16(p + 2, std::uint16_t(v >> 16)); }
inline std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
inline std::uint32_t get32(const std::uint8_t* p) { return get16(p) | (std::uint32_t(get16(p + 2)) << 16); }
}  // namespace detail

// Encoders write the whole record (with CRC) into `out` and return its length.
inline std::size_t finish_record(std::span<std::uint8_t> out, Type t, std::size_t len) {
    out[0] = kMagic; out[1] = std::uint8_t(t); out[2] = std::uint8_t(len); out[3] = 0;
    const std::uint16_t crc = bbstore::crc16(0xFFFF, {out.data(), 4 + len});
    detail::put16(out.data() + 4 + len, crc);
    return 4 + len + 2;
}
// What the header says about the plan: row count and layout hash of each section file, and the plan order over the
// section-by-section array (order[k] = the index of the plan's k-th row there; 0xFF past the last row).
struct SectionInfo {
    std::array<std::uint16_t, kSections> rows{};
    std::array<std::uint32_t, kSections> sig{};
    std::array<std::uint8_t, kOrderBytes> order{};
};
inline std::size_t encode_header(std::span<std::uint8_t> out, std::uint32_t run_id, const charger_test::Options& o,
                                 const charger_test::Evidence& e, const SectionInfo& si) {
    std::uint8_t* p = out.data() + 4;
    std::size_t total = 0;
    for (const auto r : si.rows) total += r;
    detail::put32(p, run_id); detail::put32(p + 4, o.selected);
    p[8] = kFormat; p[9] = e.rebalanced;   // byte 9 was reserved 0 before: old journals read as "no re-balance"
    detail::put16(p + 10, std::uint16_t(total));
    detail::put32(p + 12, e.session); detail::put16(p + 16, e.monitor_mv); detail::put16(p + 18, e.cable_mv);
    detail::put16(p + 20, e.cable_ma); detail::put16(p + 22, e.raw_header);
    p[24] = e.wire_rev; p[25] = e.flags; p[26] = e.num_pdos;
    p[27] = std::uint8_t((e.reported.rev_major << 4) | (e.reported.rev_minor & 15));
    p[28] = std::uint8_t((e.reported.ver_major << 4) | (e.reported.ver_minor & 15));
    p[29] = e.reported.valid ? 1 : 0;
    for (std::size_t n = 0; n < kSections; ++n) { detail::put16(p + 30 + 6 * n, si.rows[n]); detail::put32(p + 32 + 6 * n, si.sig[n]); }
    for (std::size_t n = 0; n < kOrderBytes; ++n) p[kHeaderBase + n] = si.order[n];
    return finish_record(out, Type::Header, kHeaderPayload);
}
// One plan row and its result, 18 bytes.
inline void encode_row(std::uint8_t* p, const charger_test::Step& s) {
    p[0] = std::uint8_t(s.group); p[1] = std::uint8_t(s.algorithm); p[2] = s.pos; p[3] = std::uint8_t(s.verdict);
    detail::put16(p + 4, s.mv); detail::put16(p + 6, s.ma); detail::put16(p + 8, s.measured_mv);
    p[10] = std::uint8_t(s.reason); p[11] = s.detail;
    p[12] = std::uint8_t((s.continuous ? 1 : 0) | (s.avs ? 2 : 0) | (s.hunt ? 4 : 0)); p[13] = s.tries;   // bit 2: a 6 A hunt row (a resumed one keeps its advertised-only gate)
    detail::put32(p + 14, s.pdo);
}
inline bool decode_row(const std::uint8_t* p, charger_test::Step& s) {
    namespace ct = charger_test;
    if (p[0] >= std::uint8_t(ct::Group::Count) || p[1] >= std::uint8_t(ct::Algorithm::Count) || p[3] > std::uint8_t(ct::Verdict::Skip)) return false;
    s.group = ct::Group(p[0]); s.algorithm = ct::Algorithm(p[1]); s.pos = p[2]; s.verdict = ct::Verdict(p[3]);
    s.mv = detail::get16(p + 4); s.ma = detail::get16(p + 6); s.measured_mv = detail::get16(p + 8);
    s.reason = ct::Reason(p[10]); s.detail = p[11]; s.continuous = p[12] & 1; s.avs = p[12] & 2; s.hunt = p[12] & 4; s.tries = p[13];
    s.pdo = detail::get32(p + 14);
    return true;
}
// Phase marker: which row (section and place in it), checked at replay against what the row is (group, algorithm, position,
// level), so a plan that moved under the marker voids it instead of pointing it at another row. section 0xFF = past the last row.
inline constexpr std::uint8_t kPastEnd = 0xFF;
inline std::size_t encode_phase(std::span<std::uint8_t> out, Phase ph, std::span<const charger_test::Step> steps, std::size_t idx) {
    std::uint8_t* p = out.data() + 4;
    std::memset(p, 0, kPhasePayload);
    p[0] = std::uint8_t(ph);
    if (idx >= steps.size()) p[1] = kPastEnd;
    else {
        const charger_test::Step& s = steps[idx];
        const Section sec = section_of(s);
        std::size_t ord = 0;
        for (std::size_t n = 0; n < idx; ++n) ord += section_of(steps[n]) == sec;
        p[1] = std::uint8_t(sec); detail::put16(p + 2, std::uint16_t(ord));
        p[4] = std::uint8_t(s.group); p[5] = std::uint8_t(s.algorithm); p[6] = s.pos; detail::put16(p + 8, s.mv);
    }
    return finish_record(out, Type::Phase, kPhasePayload);
}
inline std::size_t encode_finish(std::span<std::uint8_t> out, std::uint8_t state, std::uint8_t reason) {
    out[4] = state; out[5] = reason;
    return finish_record(out, Type::Finish, kFinishPayload);
}
inline std::size_t encode_resume(std::span<std::uint8_t> out, std::uint8_t count) {
    out[4] = count; out[5] = 0;
    return finish_record(out, Type::Resume, kResumePayload);
}
inline std::size_t encode_meta(std::span<std::uint8_t> out, const RunMeta& m) {
    std::uint8_t* p = out.data() + 4;
    std::memset(p, 0, kMetaPayload);
    p[0] = m.units.n; p[1] = (m.unattended ? 1 : 0) | (m.pps_sweep ? 2 : 0);   // bit 1 was reserved 0 before: old journals read "no sweep"
    for (std::size_t n = 0; n < m.units.n && n < m.units.u.size(); ++n) p[2 + n] = std::uint8_t(m.units.u[n]);
    p[18] = m.cable_kind; p[19] = m.guard; detail::put16(p + 20, m.cable_seq);
    return finish_record(out, Type::Meta, kMetaPayload);
}
inline std::size_t encode_attempt(std::span<std::uint8_t> out, std::size_t idx, std::uint8_t count) {
    std::uint8_t* p = out.data() + 4;
    detail::put16(p, std::uint16_t(idx)); p[2] = count; p[3] = 0;
    return finish_record(out, Type::Attempt, kAttemptPayload);
}

// Folds one valid small record (resume, meta, attempt, finish) into the replay state. Header, rows and the phase marker need the
// sections and are handled by Journal::replay.
inline void apply(Replay& r, Type t, std::span<const std::uint8_t> p) {
    using namespace charger_test;
    switch (t) {
    case Type::Resume:
        if (r.found && p.size() == kResumePayload) r.resumed = p[0];
        break;
    case Type::Meta: {
        if (!r.found || p.size() != kMetaPayload || p[0] > charger_test::kMaxUnits) return;
        RunMeta m;
        for (std::size_t n = 0; n < p[0]; ++n) if (!m.units.add(Unit(p[2 + n]))) return;
        m.unattended = p[1] & 1; m.pps_sweep = (p[1] >> 1) & 1;
        m.cable_kind = p[18]; m.guard = p[19]; m.cable_seq = detail::get16(p.data() + 20);
        r.meta = m; r.has_meta = true; r.options.units = m.units; r.options.pps_sweep = m.pps_sweep;
        break;
    }
    case Type::Attempt:
        if (!r.found || p.size() != kAttemptPayload) return;
        r.attempt_index = detail::get16(p.data()); r.attempts = p[2];
        break;
    case Type::Finish:
        if (r.found) { r.unfinished = false; r.phase = Phase::None; }
        break;
    default:
        break;
    }
}

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
inline void conclude(Replay& r) {
    if (!r.found) return;
    bool open = r.phase != Phase::None;
    for (std::size_t n = 0; n < r.count; ++n)
        open |= r.steps[n].verdict == charger_test::Verdict::Pending || r.steps[n].verdict == charger_test::Verdict::Running;
    r.unfinished = r.unfinished && open;
}

// A run's rows come off the disk section by section; the Runner wants them in the plan's order - the order of the planner's step
// list (Runner::steps()) the sections were written from, which the header carries (format 3). encode_order is the mechanical
// translation of that list into section-by-section indexing (a stable partition by section, no rule of its own), so the 6 A hunt
// rows land exactly where the planner emitted them, whatever the run did to a row in between. order[k] = the index in `rows`
// of the plan's k-th row; 0xFF past the last row.
namespace detail {
inline void encode_order(std::span<const charger_test::Step> steps, std::uint8_t* order) {
    std::array<std::uint16_t, kSections> base{};
    std::uint16_t at = 0;
    for (std::size_t s = 0; s < kSections; ++s) { base[s] = at; at = std::uint16_t(at + section_rows(steps, Section(s))); }
    std::array<std::uint8_t, kSections> seen{};
    for (std::size_t k = 0; k < steps.size() && k < kOrderBytes; ++k) {
        const std::size_t sec = std::size_t(section_of(steps[k]));
        order[k] = std::uint8_t(base[sec] + seen[sec]++);
    }
    for (std::size_t k = steps.size(); k < kOrderBytes; ++k) order[k] = 0xFF;
}
// The fingerprint of the plan order the header names, so a replan that moved rows (a re-announce re-placing a hunt, a dropped
// row) rewrites the header too even when no section's rows or hash changed.
inline std::uint32_t order_sig(std::span<const charger_test::Step> steps) {
    std::array<std::uint8_t, kOrderBytes> order{};
    encode_order(steps, order.data());
    std::uint32_t h = 2166136261u;
    for (std::size_t k = 0; k < steps.size() && k < kOrderBytes; ++k) h = (h ^ order[k]) * 16777619u;
    return h;
}
// rows[k] = old rows[order[k]], in place (cycle by cycle, one spare row).
inline void apply_order(std::span<charger_test::Step> rows, const std::uint8_t* order) {
    std::array<bool, charger_test::kMaxSteps> done{};
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (done[i]) continue;
        const charger_test::Step first = rows[i];
        std::size_t j = i;
        for (;;) {
            done[j] = true;
            const std::size_t k = order[j];
            if (k == i) { rows[j] = first; break; }
            rows[j] = rows[k];
            j = k;
        }
    }
}
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
    explicit Journal(fs::Fs& f) : fs_(f) { forget(); }

    // Rebuilds the last run from the files.
    void replay(Replay& r) const {
        reset(r);
        std::array<std::uint8_t, kMaxRecord> b{};
        std::size_t n = 0;
        const fs::Err he = fs_.read_file(kHeaderPath, b, n);
        if (he != fs::Err::Ok) {
            if (he == fs::Err::TooBig) r.problem = Problem::OldFormat;   // bigger than any header we write: another layout
            return;
        }
        SectionInfo si;
        const Problem hp = parse_header({b.data(), n}, r, si);
        if (hp != Problem::None) { reset(r); r.problem = hp; return; }
        std::array<std::size_t, kSections> base{};
        std::size_t at = 0;
        for (std::size_t s = 0; s < kSections; ++s) {
            base[s] = at;
            if (si.rows[s] == 0) continue;
            if (!read_section(Section(s), si.rows[s], si.sig[s], &r.steps[at])) {
                reset(r); r.problem = Problem::BadSection; r.bad = Section(s);
                return;
            }
            at += si.rows[s];
        }
        r.count = at;
        read_small("/jr/mt.rec", b, r);   // the unit order (the Runner's own copy; the row order comes from the header now)
        replay_phase(r, base, si);
        // The plan's order as the planner's step list had it, carried by the header: map the phase marker through it, then
        // put the rows back in it.
        if (r.phase != Phase::None && r.phase_index < r.count)
            for (std::size_t k = 0; k < r.count; ++k) if (si.order[k] == r.phase_index) { r.phase_index = k; break; }
        detail::apply_order({r.steps.data(), r.count}, si.order.data());
        read_small("/jr/rs.rec", b, r);
        read_small("/jr/at.rec", b, r);
        read_small("/jr/fin.rec", b, r);
        conclude(r);
    }
    bool broken() const { return broken_; }
    // The filesystem error behind the last Err::Flash (Ok when none since begin): the device log names it.
    fs::Err last_fs() const { return last_fs_; }

    // A run's start. start_arm() only notes the work; each pump() call then does at most ONE write (plus up to kLookups cheap
    // lookups that find nothing to do) and returns to the main loop. Order: header dropped FIRST (a cut during the rest leaves
    // no run at all), stale files swept, every section written, the header LAST.
    static constexpr int kLookups = 24;
    void start_arm() {
        forget();
        broken_ = false;
        last_fs_ = fs::Err::Ok;
        stage_ = Stage::Mkdir;
        cur_ = 0;
    }
    // Drops what is still to be written (the run is being closed).
    void cancel() { stage_ = Stage::Idle; dirty_ = plan_dirty_ = 0; }
    // An arm is in progress (the run is not yet on flash).
    bool working() const { return stage_ != Stage::Idle; }
    // Something is still to be written: an arm in progress, a section to rewrite, or a header that does not name what the
    // section files hold or the order it was written with. The Runner does not start an above-5 V request while this is true.
    bool pending() const { return !broken_ && (stage_ != Stage::Idle || dirty_ != 0 || header_stale()); }
    // The same, for the plan's SHAPE only: an arm, a section whose rows or layout changed, the header naming them, the plan's
    // order moved. What the phase marker's address depends on; a section that is only dirty for a verdict does not hold anything back.
    bool plan_pending() const { return !broken_ && (stage_ != Stage::Idle || plan_dirty_ != 0 || header_stale()); }

    // A row of the plan changed (its result): its section file is rewritten by a later pump().
    void note(std::span<const charger_test::Step> steps, std::size_t idx) {
        if (!live() || idx >= steps.size()) return;
        dirty_ |= bit(section_of(steps[idx]));
    }
    // The plan may have changed shape (rows added, dropped, moved): the sections whose row count or layout hash differ from what
    // is on flash are rewritten, and the header after them - also when only the plan's ORDER moved (a re-announce re-placing a
    // row, a dropped hunt): the header names the order, so it is rewritten too. Costs a hash pass, no flash.
    void note_plan(std::span<const charger_test::Step> steps) {
        if (!live()) return;
        for (std::size_t s = 0; s < kSections; ++s) {
            if (srows_[s] == kUnknown) continue;   // not written yet: an arm in progress writes it from the live rows
            if (srows_[s] != section_rows(steps, Section(s)) || ssig_[s] != layout_sig(Section(s), steps))
                dirty_ |= bit(Section(s)), plan_dirty_ |= bit(Section(s));
        }
        if (hord_known_ && steps.size() <= charger_test::kMaxSteps && detail::order_sig(steps) != hord_sig_)
            dirty_ |= kOrderBit, plan_dirty_ |= kOrderBit;
    }
    // The journal now mirrors a run read back at boot (its sections are on flash as read): later changes are written as changes.
    void adopt(const Replay& r) {
        forget();
        const std::span<const charger_test::Step> all(r.steps.data(), r.count);
        for (std::size_t s = 0; s < kSections; ++s) {
            srows_[s] = hrows_[s] = std::uint16_t(section_rows(all, Section(s)));
            ssig_[s] = hsig_[s] = layout_sig(Section(s), all);
        }
        hord_sig_ = detail::order_sig(all); hord_known_ = true;   // r.steps is in the plan's order: this is the order the header named
        armed_ = true;
    }
    // A journal in the old layout is dropped at boot (the header only; the next arm sweeps the rest).
    void discard_old() { drop(kHeaderPath); }

    Err pump(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
             std::span<const charger_test::Step> steps) {
        if (steps.size() > charger_test::kMaxSteps) { stage_ = Stage::Idle; dirty_ = plan_dirty_ = 0; return Err::TooBig; }
        if (stage_ == Stage::Idle) return flush(run_id, o, e, steps);
        char p[fs::kPathMax];
        for (int budget = kLookups; budget > 0; --budget) {
            bool wrote = false;
            switch (stage_) {
            case Stage::Mkdir:
                if (const fs::Err m = fs_.mkdir(kDir); m != fs::Err::Ok) return stop(fail(m));
                fs_.clean_tmp(kDir);
                stage_ = Stage::DropHeader;
                break;
            case Stage::DropHeader:
                if (!drop(kHeaderPath, &wrote)) return stop(fail());
                stage_ = Stage::Sweep;
                break;
            case Stage::Sweep:
                if (!stale(p)) { stage_ = Stage::Sections; cur_ = 0; break; }
                if (!drop(p, &wrote)) return stop(fail());
                break;
            case Stage::Sections:
                if (cur_ >= kSections) { stage_ = Stage::Header; break; }
                if (const Err x = write_section(Section(cur_), steps, &wrote); x != Err::Ok) return stop(x);
                dirty_ &= std::uint8_t(~bit(Section(cur_)));
                ++cur_;
                break;
            case Stage::Header:
                if (dirty_ & kSectionMask) { stage_ = Stage::Late; break; }   // the plan moved while the sections were written
                if (const Err x = write_header(run_id, o, e, steps); x != Err::Ok) return stop(x);
                stage_ = Stage::Idle; armed_ = true;
                return Err::Ok;
            case Stage::Late: {
                bool did = false;
                if (const Err x = write_dirty(steps, did); x != Err::Ok) return stop(x);
                if (!did) stage_ = Stage::Header;
                wrote = did;
                break;
            }
            case Stage::Idle: return Err::Ok;
            }
            if (wrote) return Err::Ok;   // one write per pump: back to the main loop
        }
        return Err::Ok;
    }
    Err begin(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
              std::span<const charger_test::Step> steps) {
        if (steps.size() > charger_test::kMaxSteps) return Err::TooBig;
        start_arm();
        return settle(run_id, o, e, steps);
    }
    // pump() until nothing is left (or a write failed).
    Err settle(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
               std::span<const charger_test::Step> steps) {
        while (pending())
            if (const Err x = pump(run_id, o, e, steps); x != Err::Ok) return x;
        return Err::Ok;
    }
    // `steps` is the live plan: the marker names row `idx` by section and place in it. idx == steps.size() = past the last row.
    Err phase(Phase ph, std::span<const charger_test::Step> steps, std::size_t idx) {
        std::array<std::uint8_t, kMaxRecord> buf{};
        return put("/jr/ph.rec", {buf.data(), encode_phase(buf, ph, steps, idx)});
    }
    Err resumed(std::uint8_t count) {
        std::array<std::uint8_t, kMaxRecord> buf{};
        return put("/jr/rs.rec", {buf.data(), encode_resume(buf, count)});
    }
    Err meta(const RunMeta& m) {
        std::array<std::uint8_t, kMaxRecord> buf{};
        return put("/jr/mt.rec", {buf.data(), encode_meta(buf, m)});
    }
    // The loss counter of a unit: count interruptions charged to unit `idx` (the third one skips the rest of the unit,
    // charger_test::kRebootBudget).
    Err attempt(std::size_t idx, std::uint8_t count) {
        std::array<std::uint8_t, kMaxRecord> buf{};
        return put("/jr/at.rec", {buf.data(), encode_attempt(buf, idx, count)});
    }
    // Closes the run: the next boot does not resume it, whatever its rows say. Tried even after an earlier write failed (broken_):
    // a lost record must not keep a run that ended open for ever (it would come back on every boot).
    Err finish(std::uint8_t state, std::uint8_t reason) {
        broken_ = false;
        std::array<std::uint8_t, kMaxRecord> buf{};
        return put("/jr/fin.rec", {buf.data(), encode_finish(buf, state, reason)});
    }

private:
    static constexpr std::uint16_t kUnknown = 0xFFFF;
    enum class Stage : std::uint8_t { Idle, Mkdir, DropHeader, Sweep, Sections, Header, Late };
    static constexpr std::uint8_t bit(Section s) { return std::uint8_t(1u << unsigned(s)); }
    static constexpr std::uint8_t kSectionMask = 0x1F, kOrderBit = 0x20;   // dirty_ bits: one per section, plus "the header's plan order is stale"
    bool live() const { return armed_ || stage_ != Stage::Idle; }
    void forget() {
        armed_ = false; dirty_ = plan_dirty_ = 0;
        srows_.fill(kUnknown); hrows_.fill(kUnknown); ssig_.fill(0); hsig_.fill(0);
        hord_sig_ = 0; hord_known_ = false;
    }
    // The header names the section files as they were last written, and the plan order as it was last written.
    bool header_stale() const {
        if (!armed_) return false;
        if (dirty_ & kOrderBit) return true;
        for (std::size_t s = 0; s < kSections; ++s)
            if (srows_[s] != hrows_[s] || ssig_[s] != hsig_[s]) return true;
        return false;
    }

    // One write at rest: the first section to rewrite, else the header that names what the section files hold.
    Err flush(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
              std::span<const charger_test::Step> steps) {
        if (broken_) { dirty_ = plan_dirty_ = 0; return Err::Broken; }
        bool did = false;
        Err x = write_dirty(steps, did);
        if (x == Err::Ok && !did && header_stale()) x = write_header(run_id, o, e, steps);
        if (x != Err::Ok) dirty_ = plan_dirty_ = 0;   // nothing more is tried on a failed volume
        return x;
    }
    // Writes the first section marked dirty (one file); `did` = a section was handled.
    Err write_dirty(std::span<const charger_test::Step> steps, bool& did) {
        did = false;
        for (std::size_t s = 0; s < kSections; ++s) {
            if (!(dirty_ & bit(Section(s)))) continue;
            dirty_ &= std::uint8_t(~bit(Section(s)));
            did = true;
            return write_section(Section(s), steps);
        }
        return Err::Ok;
    }
    // Writes (or, for a section with no rows, removes) the section file and notes what it now holds. `flash` = a file was
    // written or removed (as against a lookup that found nothing to do).
    Err write_section(Section sec, std::span<const charger_test::Step> steps, bool* flash = nullptr) {
        if (flash) *flash = false;
        if (broken_) return Err::Broken;
        const std::size_t rows = section_rows(steps, sec);
        const std::uint32_t sig = layout_sig(sec, steps);
        if (rows == 0) {
            bool removed = false;
            if (!drop(section_path(sec), &removed)) return fail();
            if (flash) *flash = removed;
        } else {
            if (flash) *flash = true;
            if (const fs::Err m = fs_.mkdir(kDir); m != fs::Err::Ok) return fail(m);
            SecCtx c{steps, sec, rows, sig, 0, kNone, 0};
            c.crc = section_crc(c);
            const fs::Err w = fs_.write_chunks(section_path(sec), kSecHeader + kRowBytes * rows, &fill_section, &c);
            if (w != fs::Err::Ok) return fail(w);
        }
        srows_[std::size_t(sec)] = std::uint16_t(rows);
        ssig_[std::size_t(sec)] = sig;
        plan_dirty_ &= std::uint8_t(~bit(sec));
        return Err::Ok;
    }
    Err write_header(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
                     std::span<const charger_test::Step> steps) {
        SectionInfo si;
        for (std::size_t s = 0; s < kSections; ++s) { si.rows[s] = srows_[s] == kUnknown ? 0 : srows_[s]; si.sig[s] = ssig_[s]; }
        detail::encode_order(steps, si.order.data());   // the plan's order as the planner's step list (Runner::steps()) has it
        std::array<std::uint8_t, kMaxRecord> buf{};
        if (const Err x = put(kHeaderPath, {buf.data(), encode_header(buf, run_id, o, e, si)}); x != Err::Ok) return x;
        hrows_ = srows_; hsig_ = ssig_;
        hord_sig_ = detail::order_sig(steps); hord_known_ = true;
        dirty_ &= std::uint8_t(~kOrderBit); plan_dirty_ &= std::uint8_t(~kOrderBit);
        return Err::Ok;
    }

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
    static const charger_test::Step& nth_row(SecCtx& c, std::size_t n) {
        std::size_t pos = 0, k = 0;
        if (c.last_n != kNone && n >= c.last_n) { k = c.last_n; pos = c.last_pos; }
        for (; pos < c.steps.size(); ++pos) {
            if (section_of(c.steps[pos]) != c.sec) continue;
            if (k == n) break;
            ++k;
        }
        c.last_n = n; c.last_pos = pos;
        return c.steps[pos];
    }
    static void put_sec_header(std::uint8_t* h, const SecCtx& c) {
        h[0] = std::uint8_t(c.sec); h[1] = std::uint8_t(c.rows); detail::put32(h + 2, c.sig); detail::put16(h + 6, c.crc);
    }
    static std::uint16_t section_crc(SecCtx& c) {
        std::uint8_t h[kSecHeader];
        put_sec_header(h, c);
        std::uint16_t crc = bbstore::crc16(0xFFFF, {h, 6});
        std::uint8_t row[kRowBytes];
        for (std::size_t n = 0; n < c.rows; ++n) {
            encode_row(row, nth_row(c, n));
            crc = bbstore::crc16(crc, {row, kRowBytes});
        }
        c.last_n = kNone;
        return crc;
    }
    static void fill_section(void* ctx, std::size_t off, std::span<std::uint8_t> out) {
        SecCtx& c = *static_cast<SecCtx*>(ctx);
        std::uint8_t h[kSecHeader], row[kRowBytes];
        put_sec_header(h, c);
        for (std::size_t k = 0; k < out.size();) {
            const std::size_t pos = off + k;
            if (pos < kSecHeader) { out[k++] = h[pos]; continue; }
            const std::size_t rel = pos - kSecHeader, n = rel / kRowBytes, in = rel % kRowBytes;
            encode_row(row, nth_row(c, n));
            const std::size_t take = std::min(kRowBytes - in, out.size() - k);
            std::memcpy(out.data() + k, row + in, take);
            k += take;
        }
    }

    // ---- reading
    // The header record: format and counts. OldFormat for format 1 (payload 30) and any format byte that is not ours.
    static Problem parse_header(std::span<const std::uint8_t> img, Replay& r, SectionInfo& si) {
        using namespace charger_test;
        std::span<const std::uint8_t> pl;
        scan(img, [&](Type t, std::span<const std::uint8_t> p) { if (t == Type::Header && pl.empty()) pl = p; });
        if (pl.empty()) return Problem::None;   // torn or blank: no run
        if (pl.size() == kOldHeaderPayload || pl.size() != kHeaderPayload || pl[8] != kFormat) return Problem::OldFormat;
        const std::uint8_t* p = pl.data();
        r.found = true; r.run_id = detail::get32(p);
        r.options.selected = detail::get32(p + 4);
        Evidence& e = r.evidence;
        e.selected = r.options.selected; e.session = detail::get32(p + 12);
        e.monitor_mv = detail::get16(p + 16); e.cable_mv = detail::get16(p + 18);
        e.cable_ma = detail::get16(p + 20); e.raw_header = detail::get16(p + 22);
        e.wire_rev = p[24]; e.flags = p[25]; e.num_pdos = p[26]; e.rebalanced = p[9];   // reserved 0 on older journals
        e.reported.rev_major = p[27] >> 4; e.reported.rev_minor = p[27] & 15;
        e.reported.ver_major = p[28] >> 4; e.reported.ver_minor = p[28] & 15; e.reported.valid = p[29] != 0;
        std::size_t total = 0;
        for (std::size_t n = 0; n < kSections; ++n) {
            si.rows[n] = detail::get16(p + 30 + 6 * n); si.sig[n] = detail::get32(p + 32 + 6 * n);
            total += si.rows[n];
        }
        if (total > kMaxSteps || total != detail::get16(p + 10)) return Problem::BadHeader;
        for (std::size_t k = 0; k < total; ++k) si.order[k] = p[kHeaderBase + k];
        for (std::size_t k = total; k < kOrderBytes; ++k) si.order[k] = 0xFF;
        {   // the order must be a permutation of the rows: a torn or foreign header is not a run
            std::array<bool, kOrderBytes> seen{};
            for (std::size_t k = 0; k < total; ++k) {
                const std::uint8_t o = si.order[k];
                if (o >= total || seen[o]) return Problem::BadHeader;
                seen[o] = true;
            }
        }
        r.unfinished = true;
        return Problem::None;
    }
    // Reads one section file into out[0..rows) and checks it against the header: size, id, row count, hash, crc.
    bool read_section(Section sec, std::size_t rows, std::uint32_t sig, charger_test::Step* out) const {
        const char* path = section_path(sec);
        std::size_t sz = 0, n = 0;
        if (fs_.size_of(path, sz) != fs::Err::Ok || sz != kSecHeader + kRowBytes * rows) return false;
        std::uint8_t h[kSecHeader];
        if (fs_.read_at(path, 0, h, n) != fs::Err::Ok || n != kSecHeader) return false;
        if (h[0] != std::uint8_t(sec) || h[1] != rows || detail::get32(h + 2) != sig) return false;
        std::uint16_t crc = bbstore::crc16(0xFFFF, {h, 6});
        constexpr std::size_t kBatch = 4;
        std::uint8_t buf[kBatch * kRowBytes];
        for (std::size_t at = 0; at < rows; at += kBatch) {
            const std::size_t b = std::min(kBatch, rows - at);
            if (fs_.read_at(path, std::uint32_t(kSecHeader + kRowBytes * at), {buf, b * kRowBytes}, n) != fs::Err::Ok || n != b * kRowBytes)
                return false;
            crc = bbstore::crc16(crc, {buf, n});
            for (std::size_t k = 0; k < b; ++k) if (!decode_row(buf + kRowBytes * k, out[at + k])) return false;
        }
        return crc == detail::get16(h + 6) && layout_sig(sec, {out, rows}) == sig;
    }
    // The phase marker, mapped to the replayed rows: void when it names a row that is not what it was (the plan moved), or when
    // that row or a later one of its section already has a result.
    void replay_phase(Replay& r, const std::array<std::size_t, kSections>& base, const SectionInfo& si) const {
        std::array<std::uint8_t, kMaxRecord> b{};
        std::size_t n = 0;
        if (fs_.read_file("/jr/ph.rec", b, n) != fs::Err::Ok || n < 4) return;
        scan({b.data(), n}, [&](Type t, std::span<const std::uint8_t> p) {
            if (t != Type::Phase || p.size() != kPhasePayload || p[0] > 2) return;
            r.phase = Phase(p[0]);
            if (r.phase == Phase::None) return;
            if (p[1] == kPastEnd) { r.phase_index = r.count; return; }
            const std::size_t sec = p[1], ord = detail::get16(p.data() + 2);
            bool ok = sec < kSections && ord < si.rows[sec];
            if (ok) {
                const charger_test::Step& s = r.steps[base[sec] + ord];
                ok = p[4] == std::uint8_t(s.group) && p[5] == std::uint8_t(s.algorithm) && p[6] == s.pos &&
                     detail::get16(p.data() + 8) == s.mv && s.verdict < charger_test::Verdict::Pass;
            }
            // Only the row's own result voids the marker: the order the units run in is not the order of the rows in a section, so
            // "a later row is final" proves nothing here.
            if (!ok) { r.phase = Phase::None; return; }
            r.phase_index = base[sec] + ord;
        });
    }
    bool read_small(const char* path, std::array<std::uint8_t, kMaxRecord>& b, Replay& r) const {
        std::size_t n = 0;
        if (fs_.read_file(path, b, n) != fs::Err::Ok || n < 4) return false;
        bool any = false;
        scan({b.data(), n}, [&](Type t, std::span<const std::uint8_t> pl) { apply(r, t, pl); any = true; });
        return any;
    }

    // ---- housekeeping
    static bool is_section_file(const char* name) {
        for (std::size_t s = 0; s < kSections; ++s) if (!std::strcmp(section_path(Section(s)) + 4, name)) return true;
        return false;
    }
    // The path of one file in /jr that is not a section file of this format (format 1 rows, the records of the last run).
    bool stale(char (&path)[fs::kPathMax]) {
        bool found = false;
        fs_.list(kDir, [&](const fs::Info& i) {
            if (is_section_file(i.name) || std::strlen(i.name) + 5 > fs::kPathMax) return true;
            std::memcpy(path, "/jr/", 4);
            std::memcpy(path + 4, i.name, std::strlen(i.name) + 1);
            found = true;
            return false;
        });
        return found;
    }
    Err stop(Err e) { stage_ = Stage::Idle; dirty_ = plan_dirty_ = 0; return e; }
    // `removed` = a file was there and is gone now (a commit), as against nothing to remove (a lookup).
    bool drop(const char* path, bool* removed = nullptr) {
        const fs::Err e = fs_.remove(path);
        if (e != fs::Err::Ok && e != fs::Err::NotFound) last_fs_ = e;
        if (removed) *removed = e == fs::Err::Ok;
        return e == fs::Err::Ok || e == fs::Err::NotFound;
    }
    Err put(const char* path, std::span<const std::uint8_t> rec) {
        if (broken_) return Err::Broken;
        if (const fs::Err m = fs_.mkdir(kDir); m != fs::Err::Ok) return fail(m);
        const fs::Err w = fs_.write_file(path, rec);
        return w == fs::Err::Ok ? Err::Ok : fail(w);
    }
    Err fail() { broken_ = true; return Err::Flash; }
    Err fail(fs::Err why) { last_fs_ = why; return fail(); }

    fs::Fs& fs_;
    bool broken_ = false, armed_ = false;
    fs::Err last_fs_ = fs::Err::Ok;
    Stage stage_ = Stage::Idle;
    std::size_t cur_ = 0;
    std::uint8_t dirty_ = 0;                                // sections whose file is to be rewritten (bit per Section)
    std::uint8_t plan_dirty_ = 0;                           // the subset of dirty_ whose rows or layout changed (not only a verdict)
    std::array<std::uint16_t, kSections> srows_{}, hrows_{};   // row count of each section file / as the header names it
    std::array<std::uint32_t, kSections> ssig_{}, hsig_{};     // layout hash of each section file / as the header names it
    std::uint32_t hord_sig_ = 0;        // fingerprint of the plan order the header names
    bool hord_known_ = false;           // false until a header was written (or a replay adopted)
};

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
    bool stable_ = false;
    std::uint32_t since_ = 0, last_ = 0;
};

}  // namespace k2::charger_journal
