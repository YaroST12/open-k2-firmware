// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/charger_journal.hpp"

namespace k2::charger_journal {

const char* section_path(Section s) {
    static constexpr const char* const kPaths[kSections] = {"/jr/pd.sec", "/jr/pps.sec", "/jr/qc.sec", "/jr/oem.sec", "/jr/ext.sec"};
    return kPaths[std::size_t(s)];
}

const char* section_name(Section s) {
    static constexpr const char* const kNames[kSections] = {"pd", "pps", "qc", "oem", "ext"};
    return kNames[std::size_t(s)];
}

Section section_of(const charger_test::Step& s) {
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

std::size_t section_rows(std::span<const charger_test::Step> steps, Section sec) {
    std::size_t n = 0;
    for (const auto& s : steps) n += section_of(s) == sec;
    return n;
}

std::uint32_t layout_sig(Section sec, std::span<const charger_test::Step> steps) {
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

void reset(Replay& r) { std::construct_at(&r); }

std::size_t finish_record(std::span<std::uint8_t> out, Type t, std::size_t len) {
    out[0] = kMagic; out[1] = std::uint8_t(t); out[2] = std::uint8_t(len); out[3] = 0;
    const std::uint16_t crc = bbstore::crc16(0xFFFF, {out.data(), 4 + len});
    detail::put16(out.data() + 4 + len, crc);
    return 4 + len + 2;
}

std::size_t encode_header(std::span<std::uint8_t> out, std::uint32_t run_id, const charger_test::Options& o,
                          const charger_test::Evidence& e, const SectionInfo& si) {
    std::uint8_t* p = out.data() + 4;
    std::size_t total = 0;
    for (const auto r : si.rows) total += r;
    detail::put32(p, run_id); detail::put32(p + 4, o.selected);
    p[8] = kFormat; p[9] = e.rebalanced;
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

void encode_row(std::uint8_t* p, const charger_test::Step& s) {
    p[0] = std::uint8_t(s.group); p[1] = std::uint8_t(s.algorithm); p[2] = s.pos; p[3] = std::uint8_t(s.verdict);
    detail::put16(p + 4, s.mv); detail::put16(p + 6, s.ma); detail::put16(p + 8, s.measured_mv);
    p[10] = std::uint8_t(s.reason); p[11] = s.detail;
    p[12] = std::uint8_t((s.continuous ? 1 : 0) | (s.avs ? 2 : 0) | (s.hunt ? 4 : 0)); p[13] = s.tries;
    detail::put32(p + 14, s.pdo);
}

bool decode_row(const std::uint8_t* p, charger_test::Step& s) {
    namespace ct = charger_test;
    if (p[0] >= std::uint8_t(ct::Group::Count) || p[1] >= std::uint8_t(ct::Algorithm::Count) || p[3] > std::uint8_t(ct::Verdict::Skip)) return false;
    s.group = ct::Group(p[0]); s.algorithm = ct::Algorithm(p[1]); s.pos = p[2]; s.verdict = ct::Verdict(p[3]);
    s.mv = detail::get16(p + 4); s.ma = detail::get16(p + 6); s.measured_mv = detail::get16(p + 8);
    s.reason = ct::Reason(p[10]); s.detail = p[11]; s.continuous = p[12] & 1; s.avs = p[12] & 2; s.hunt = p[12] & 4; s.tries = p[13];
    s.pdo = detail::get32(p + 14);
    return true;
}

std::size_t encode_phase(std::span<std::uint8_t> out, Phase ph, std::span<const charger_test::Step> steps, std::size_t idx) {
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

std::size_t encode_finish(std::span<std::uint8_t> out, std::uint8_t state, std::uint8_t reason) {
    out[4] = state; out[5] = reason;
    return finish_record(out, Type::Finish, kFinishPayload);
}

std::size_t encode_resume(std::span<std::uint8_t> out, std::uint8_t count) {
    out[4] = count; out[5] = 0;
    return finish_record(out, Type::Resume, kResumePayload);
}

std::size_t encode_meta(std::span<std::uint8_t> out, const RunMeta& m) {
    std::uint8_t* p = out.data() + 4;
    std::memset(p, 0, kMetaPayload);
    p[0] = m.units.n;
    p[1] = (m.unattended ? 1 : 0) | (m.pps_sweep ? 2 : 0) | (m.avs_sweep ? 4 : 0) |
           std::uint8_t((unsigned(m.level) & 3u) << 3) | std::uint8_t((unsigned(m.sweep_depth) & 3u) << 5);
    for (std::size_t n = 0; n < m.units.n && n < m.units.u.size(); ++n) p[2 + n] = std::uint8_t(m.units.u[n]);
    p[18] = m.cable_kind; p[19] = m.guard; detail::put16(p + 20, m.cable_seq);
    return finish_record(out, Type::Meta, kMetaPayload);
}

std::size_t encode_attempt(std::span<std::uint8_t> out, std::size_t idx, std::uint8_t count) {
    std::uint8_t* p = out.data() + 4;
    detail::put16(p, std::uint16_t(idx)); p[2] = count; p[3] = 0;
    return finish_record(out, Type::Attempt, kAttemptPayload);
}

std::size_t encode_spec(std::span<std::uint8_t> out, const specsec::Result& s) {
    std::array<std::uint8_t, specsec::kWireBytes> w{};
    specsec::to_wire(s, w);
    std::uint8_t* p = out.data() + 4;
    std::memset(p, 0, kSpecPayload);
    std::memcpy(p, w.data(), w.size());
    return finish_record(out, Type::Spec, kSpecPayload);
}

std::size_t encode_spec_detail(std::span<std::uint8_t> out, const specsec::Result& s) {
    std::array<std::uint8_t, specsec::kDetailBytes> w{};
    specsec::to_detail_wire(s, w);
    std::memcpy(out.data() + 4, w.data(), w.size());
    return finish_record(out, Type::SpecDetail, kSpecDetailPayload);
}

std::size_t encode_spec_apdo(std::span<std::uint8_t> out, const specsec::Apdo& a) {
    std::array<std::uint8_t, specsec::kApdoWireBytes> w{};
    specsec::to_apdo_wire(a, w);
    std::memcpy(out.data() + 4, w.data(), w.size());
    return finish_record(out, Type::SpecApdo, kSpecApdoPayload);
}

void apply(Replay& r, Type t, std::span<const std::uint8_t> p) {
    using namespace charger_test;
    switch (t) {
    case Type::Resume:
        if (r.found && p.size() == kResumePayload) r.resumed = p[0];
        break;
    case Type::Meta: {
        if (!r.found || p.size() != kMetaPayload || p[0] > charger_test::kMaxUnits) return;
        RunMeta m;
        for (std::size_t n = 0; n < p[0]; ++n) if (!m.units.add(Unit(p[2 + n]))) return;
        m.unattended = p[1] & 1; m.pps_sweep = (p[1] >> 1) & 1; m.avs_sweep = (p[1] >> 2) & 1;
        if (const unsigned l = (p[1] >> 3) & 3u; l < charger_test::kLevelCount) m.level = charger_test::Level(l);
        m.sweep_depth = ctsweep::Depth((p[1] >> 5) & 3u);
        m.cable_kind = p[18]; m.guard = p[19]; m.cable_seq = detail::get16(p.data() + 20);
        r.meta = m; r.has_meta = true; r.options.units = m.units; r.options.pps_sweep = m.pps_sweep;
        r.options.avs_sweep = m.avs_sweep; r.options.level = m.level; r.options.sweep_depth = m.sweep_depth;
        break;
    }
    case Type::Attempt:
        if (!r.found || p.size() != kAttemptPayload) return;
        r.attempt_index = detail::get16(p.data()); r.attempts = p[2];
        break;
    case Type::Spec:
        if (r.found && p.size() == kSpecPayload && p[specsec::kWireBytes] == 0) specsec::from_wire(p.data(), specsec::kWireBytes, r.evidence.spec);
        break;
    case Type::SpecDetail:
        if (r.found && r.evidence.spec.end != specsec::End::NotRun && p.size() == kSpecDetailPayload) specsec::from_detail_wire(p.data(), p.size(), r.evidence.spec);
        break;
    case Type::SpecApdo:   // read after sp.rec: a range without a spec result is not shown
        if (r.found && r.evidence.spec.end != specsec::End::NotRun && p.size() == kSpecApdoPayload) specsec::from_apdo_wire(p.data(), p.size(), r.spec_apdo);
        break;
    case Type::Finish:
        if (r.found) { r.unfinished = false; r.phase = Phase::None; }
        break;
    default:
        break;
    }
}

void conclude(Replay& r) {
    if (!r.found) return;
    bool open = r.phase != Phase::None;
    for (std::size_t n = 0; n < r.count; ++n)
        open |= r.steps[n].verdict == charger_test::Verdict::Pending || r.steps[n].verdict == charger_test::Verdict::Running;
    r.unfinished = r.unfinished && open;
}

namespace detail {
void encode_order(std::span<const charger_test::Step> steps, std::uint8_t* order) {
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

std::uint32_t order_sig(std::span<const charger_test::Step> steps) {
    std::array<std::uint8_t, kOrderBytes> order{};
    encode_order(steps, order.data());
    std::uint32_t h = 2166136261u;
    for (std::size_t k = 0; k < steps.size() && k < kOrderBytes; ++k) h = (h ^ order[k]) * 16777619u;
    return h;
}

void apply_order(std::span<charger_test::Step> rows, const std::uint8_t* order) {
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

Journal::Journal(fs::Fs& f) : fs_(f) { forget(); }

void Journal::replay(Replay& r) const {
    reset(r);
    std::array<std::uint8_t, kReadRecord> b{};
    std::size_t n = 0;
    const fs::Err he = fs_.read_file(kHeaderPath, b, n);
    if (he != fs::Err::Ok) {
        if (he == fs::Err::TooBig) r.problem = Problem::OldFormat;
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
    read_small("/jr/mt.rec", b, r);
    replay_phase(r, base, si, b);
    if (r.phase != Phase::None && r.phase_index < r.count)
        for (std::size_t k = 0; k < r.count; ++k) if (si.order[k] == r.phase_index) { r.phase_index = k; break; }
    detail::apply_order({r.steps.data(), r.count}, si.order.data());
    static constexpr const char* const kSmallPaths[] = {"/jr/rs.rec", "/jr/at.rec", "/jr/sp.rec", "/jr/spd.rec", "/jr/sa1.rec", "/jr/sa2.rec",
                                                        "/jr/sa3.rec", "/jr/sa4.rec", "/jr/fin.rec"};
    static_assert(specsec::Apdos{}.r.size() == 4, "one range file per Apdos entry");
    for (const char* path : kSmallPaths) read_small(path, b, r);
    conclude(r);
}

void Journal::start_arm() {
    forget();
    broken_ = false;
    last_fs_ = fs::Err::Ok;
    stage_ = Stage::Mkdir;
    cur_ = 0;
}

void Journal::cancel() { stage_ = Stage::Idle; clear_dirty(); }
bool Journal::pending() const { return !broken_ && (stage_ != Stage::Idle || dirty_ != 0 || apdo_dirty_ != 0 || rows_pending() || header_stale()); }
bool Journal::plan_pending() const { return !broken_ && (stage_ != Stage::Idle || plan_dirty_ != 0 || header_stale()); }

void Journal::note(std::span<const charger_test::Step> steps, std::size_t idx) {
    if (!live() || idx >= steps.size()) return;
    dirty_ |= bit(section_of(steps[idx]));
}

void Journal::note_plan(std::span<const charger_test::Step> steps) {
    if (!live()) return;
    for (std::size_t s = 0; s < kSections; ++s) {
        if (srows_[s] == kUnknown) continue;
        if (srows_[s] != section_rows(steps, Section(s)) || ssig_[s] != layout_sig(Section(s), steps))
            dirty_ |= bit(Section(s)), plan_dirty_ |= bit(Section(s));
    }
    if (hord_known_ && steps.size() <= charger_test::kMaxSteps && detail::order_sig(steps) != hord_sig_)
        dirty_ |= kOrderBit, plan_dirty_ |= kOrderBit;
}

void Journal::note_spec(const specsec::Apdos* apdo) {
    if (!live()) return;
    dirty_ |= kSpecBit | kSpecDetailBit;
    apdo_ = apdo;
    apdo_dirty_ = apdo ? static_cast<std::uint8_t>((1u << apdo->n) - 1u) : 0;
#if K2_SPECROWS
    rows_dirty_ = true;
#endif
}

void Journal::adopt(const Replay& r) {
    forget();
    const std::span<const charger_test::Step> all(r.steps.data(), r.count);
    for (std::size_t s = 0; s < kSections; ++s) {
        srows_[s] = hrows_[s] = std::uint16_t(section_rows(all, Section(s)));
        ssig_[s] = hsig_[s] = layout_sig(Section(s), all);
    }
    hord_sig_ = detail::order_sig(all); hord_known_ = true;
    armed_ = true;
}

void Journal::discard_old() { drop(kHeaderPath); }

Err Journal::pump(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
                  std::span<const charger_test::Step> steps
#if K2_SPECROWS
                  , const SpecRowsSource* rows
#endif
                  ) {
    if (steps.size() > charger_test::kMaxSteps) { stage_ = Stage::Idle; clear_dirty(); return Err::TooBig; }
#if K2_SPECROWS
    if (stage_ == Stage::Idle) return flush(run_id, o, e, steps, rows);
#else
    if (stage_ == Stage::Idle) return flush(run_id, o, e, steps);
#endif
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
            if (dirty_ & kSectionMask) { stage_ = Stage::Late; break; }
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
        if (wrote) return Err::Ok;
    }
    return Err::Ok;
}

Err Journal::begin(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
                   std::span<const charger_test::Step> steps) {
    if (steps.size() > charger_test::kMaxSteps) return Err::TooBig;
    start_arm();
    return settle(run_id, o, e, steps);
}

Err Journal::settle(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
                    std::span<const charger_test::Step> steps) {
    while (pending())
        if (const Err x = pump(run_id, o, e, steps); x != Err::Ok) return x;
    return Err::Ok;
}

Err Journal::phase(Phase ph, std::span<const charger_test::Step> steps, std::size_t idx) {
    std::array<std::uint8_t, kMaxRecord> buf;
    return put("/jr/ph.rec", {buf.data(), encode_phase(buf, ph, steps, idx)});
}

Err Journal::resumed(std::uint8_t count) {
    std::array<std::uint8_t, kMaxRecord> buf;
    return put("/jr/rs.rec", {buf.data(), encode_resume(buf, count)});
}

Err Journal::meta(const RunMeta& m) {
    std::array<std::uint8_t, kMaxRecord> buf;
    return put("/jr/mt.rec", {buf.data(), encode_meta(buf, m)});
}

Err Journal::attempt(std::size_t idx, std::uint8_t count) {
    std::array<std::uint8_t, kMaxRecord> buf;
    return put("/jr/at.rec", {buf.data(), encode_attempt(buf, idx, count)});
}

Err Journal::spec(const specsec::Result& s) {
    std::array<std::uint8_t, kMaxRecord> buf;
    return put("/jr/sp.rec", {buf.data(), encode_spec(buf, s)});
}

Err Journal::spec_detail(const specsec::Result& s) {
    std::array<std::uint8_t, kSpecDetailRecord> buf;
    return put("/jr/spd.rec", {buf.data(), encode_spec_detail(buf, s)});
}

Err Journal::spec_apdo(std::size_t idx, const specsec::Apdo& a) {
    std::array<std::uint8_t, kMaxRecord> buf;
    char path[] = "/jr/sa1.rec";
    path[6] = static_cast<char>('1' + idx);
    return put(path, {buf.data(), encode_spec_apdo(buf, a)});
}

#if K2_SPECROWS
Err Journal::spec_rows(const SpecRowsSource* src) {
    if (broken_) return Err::Broken;
    if (!src || !src->size || !src->fill) { bool gone = false; return drop(kSpecRowsPath, &gone) ? Err::Ok : fail(); }
    if (src->size > kSpecRowsMax) return Err::TooBig;
    if (const fs::Err m = fs_.mkdir(kDir); m != fs::Err::Ok) return fail(m);
    const fs::Err w = fs_.write_chunks(kSpecRowsPath, src->size, src->fill, src->ctx);
    return w == fs::Err::Ok ? Err::Ok : fail(w);
}
#endif

Err Journal::finish(std::uint8_t state, std::uint8_t reason) {
    broken_ = false;
    std::array<std::uint8_t, kMaxRecord> buf;
    return put("/jr/fin.rec", {buf.data(), encode_finish(buf, state, reason)});
}

void Journal::forget() {
    armed_ = false; clear_dirty();
    srows_.fill(kUnknown); hrows_.fill(kUnknown); ssig_.fill(0); hsig_.fill(0);
    hord_sig_ = 0; hord_known_ = false;
}

bool Journal::header_stale() const {
    if (!armed_) return false;
    if (dirty_ & kOrderBit) return true;
    for (std::size_t s = 0; s < kSections; ++s)
        if (srows_[s] != hrows_[s] || ssig_[s] != hsig_[s]) return true;
    return false;
}

Err Journal::flush(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
                   std::span<const charger_test::Step> steps
#if K2_SPECROWS
                   , const SpecRowsSource* rows
#endif
                   ) {
    if (broken_) { clear_dirty(); return Err::Broken; }
    bool did = false;
    Err x = write_dirty(steps, did);
    if (x == Err::Ok && !did && header_stale()) x = write_header(run_id, o, e, steps);
    else if (x == Err::Ok && !did && (dirty_ & kSpecBit)) { dirty_ &= std::uint8_t(~kSpecBit); x = spec(e.spec); }
    else if (x == Err::Ok && !did && (dirty_ & kSpecDetailBit)) { dirty_ &= std::uint8_t(~kSpecDetailBit); x = spec_detail(e.spec); }
    else if (x == Err::Ok && !did && apdo_dirty_ && apdo_) {
        const std::size_t i = static_cast<std::size_t>(__builtin_ctz(apdo_dirty_));
        apdo_dirty_ &= static_cast<std::uint8_t>(~(1u << i));
        x = spec_apdo(i, apdo_->r[i]);
    }
#if K2_SPECROWS
    else if (x == Err::Ok && !did && rows_dirty_) { rows_dirty_ = false; x = spec_rows(rows); }
#endif
    if (x != Err::Ok) clear_dirty();   // nothing more is tried on a failed volume
    return x;
}

Err Journal::write_dirty(std::span<const charger_test::Step> steps, bool& did) {
    did = false;
    for (std::size_t s = 0; s < kSections; ++s) {
        if (!(dirty_ & bit(Section(s)))) continue;
        dirty_ &= std::uint8_t(~bit(Section(s)));
        did = true;
        return write_section(Section(s), steps);
    }
    return Err::Ok;
}

Err Journal::write_section(Section sec, std::span<const charger_test::Step> steps, bool* flash) {
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

Err Journal::write_header(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
                          std::span<const charger_test::Step> steps) {
    SectionInfo si;
    for (std::size_t s = 0; s < kSections; ++s) { si.rows[s] = srows_[s] == kUnknown ? 0 : srows_[s]; si.sig[s] = ssig_[s]; }
    detail::encode_order(steps, si.order.data());
    std::array<std::uint8_t, kMaxRecord> buf;
    if (const Err x = put(kHeaderPath, {buf.data(), encode_header(buf, run_id, o, e, si)}); x != Err::Ok) return x;
    hrows_ = srows_; hsig_ = ssig_;
    hord_sig_ = detail::order_sig(steps); hord_known_ = true;
    dirty_ &= std::uint8_t(~kOrderBit); plan_dirty_ &= std::uint8_t(~kOrderBit);
    return Err::Ok;
}

const charger_test::Step& Journal::nth_row(SecCtx& c, std::size_t n) {
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

void Journal::put_sec_header(std::uint8_t* h, const SecCtx& c) {
    h[0] = std::uint8_t(c.sec); h[1] = std::uint8_t(c.rows); detail::put32(h + 2, c.sig); detail::put16(h + 6, c.crc);
}

std::uint16_t Journal::section_crc(SecCtx& c) {
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

void Journal::fill_section(void* ctx, std::size_t off, std::span<std::uint8_t> out) {
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

[[gnu::noinline]] Problem Journal::parse_header(std::span<const std::uint8_t> img, Replay& r, SectionInfo& si) {
    using namespace charger_test;
    std::span<const std::uint8_t> pl;
    scan(img, [&](Type t, std::span<const std::uint8_t> p) { if (t == Type::Header && pl.empty()) pl = p; });
    if (pl.empty()) return Problem::None;
    if (pl.size() == kOldHeaderPayload || pl.size() != kHeaderPayload || pl[8] < kMinFormat || pl[8] > kFormat) return Problem::OldFormat;
    const std::uint8_t* p = pl.data();
    r.found = true; r.run_id = detail::get32(p);
    r.options.selected = detail::get32(p + 4);
    Evidence& e = r.evidence;
    e.selected = r.options.selected; e.session = detail::get32(p + 12);
    e.monitor_mv = detail::get16(p + 16); e.cable_mv = detail::get16(p + 18);
    e.cable_ma = detail::get16(p + 20); e.raw_header = detail::get16(p + 22);
    e.wire_rev = p[24]; e.flags = p[25]; e.num_pdos = p[26]; e.rebalanced = p[9];
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
    {
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

[[gnu::noinline]] bool Journal::read_section(Section sec, std::size_t rows, std::uint32_t sig, charger_test::Step* out) const {
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

[[gnu::noinline]] void Journal::replay_phase(Replay& r, const std::array<std::size_t, kSections>& base, const SectionInfo& si,
                                             std::array<std::uint8_t, kReadRecord>& b) const {
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
        if (!ok) { r.phase = Phase::None; return; }
        r.phase_index = base[sec] + ord;
    });
}

bool Journal::read_small(const char* path, std::array<std::uint8_t, kReadRecord>& b, Replay& r) const {
    std::size_t n = 0;
    if (fs_.read_file(path, b, n) != fs::Err::Ok || n < 4) return false;
    bool any = false;
    scan({b.data(), n}, [&](Type t, std::span<const std::uint8_t> pl) { apply(r, t, pl); any = true; });
    return any;
}

bool Journal::is_section_file(const char* name) {
    for (std::size_t s = 0; s < kSections; ++s) if (!std::strcmp(section_path(Section(s)) + 4, name)) return true;
    return false;
}

bool Journal::stale(char (&path)[fs::kPathMax]) {
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

Err Journal::stop(Err e) { stage_ = Stage::Idle; clear_dirty(); return e; }
void Journal::clear_dirty() {
    dirty_ = plan_dirty_ = apdo_dirty_ = 0;
#if K2_SPECROWS
    rows_dirty_ = false;
#endif
}

bool Journal::drop(const char* path, bool* removed) {
    const fs::Err e = fs_.remove(path);
    if (e != fs::Err::Ok && e != fs::Err::NotFound) last_fs_ = e;
    if (removed) *removed = e == fs::Err::Ok;
    return e == fs::Err::Ok || e == fs::Err::NotFound;
}

Err Journal::put(const char* path, std::span<const std::uint8_t> rec) {
    if (broken_) return Err::Broken;
    if (const fs::Err m = fs_.mkdir(kDir); m != fs::Err::Ok) return fail(m);
    const fs::Err w = fs_.write_file(path, rec);
    return w == fs::Err::Ok ? Err::Ok : fail(w);
}

Err Journal::fail() { broken_ = true; return Err::Flash; }
Err Journal::fail(fs::Err why) { last_fs_ = why; return fail(); }

}  // namespace k2::charger_journal
