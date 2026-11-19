// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Deep extension blob of the report card (k2/chargertest/report_card_deep.hpp): the sweep bucket reducer and the blob builder.
// The blob is read back by a reader written here from the wire description in the header (not the firmware's own code path), so
// a mistake in the layout shows as a mismatch, not as a symmetric round trip.
#include <cstdint>
#include <vector>

#include "k2/chargertest/report_card_deep.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;
namespace dp = k2::report_card::deep;
namespace ct = k2::charger_test;
namespace sc = k2::pdspec;
using Bytes = std::vector<std::uint8_t>;

// ---- an independent LSB-first reader ----
struct Rd {
    const std::uint8_t* p;
    unsigned pos = 0;
    std::uint32_t get(unsigned n) {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++pos) v |= std::uint32_t((p[pos >> 3] >> (pos & 7)) & 1u) << i;
        return v;
    }
    std::int32_t gets(unsigned n) {
        const std::uint32_t v = get(n);
        return (v & (1u << (n - 1))) ? std::int32_t(v) - std::int32_t(1u << n) : std::int32_t(v);
    }
};

struct DBucket { int mean; unsigned down, up; };
struct DRange { unsigned lo_dv, n, scale; std::vector<DBucket> b; };
struct DCell { unsigned verdict = 0, skip = 0; int value = 0; unsigned fails = 0, a = 0, b = 0; };
struct DMode { std::uint32_t before = 0, after = 0; unsigned mv = 0, ma = 0, verdict = 0, reason = 0; };
struct DSpecRange { unsigned pos = 0, avs = 0, tone = 0, group = 0; std::vector<unsigned> verdicts; };
struct Decoded {
    unsigned bits = 0, used = 0;
    bool has_mi = false;
    unsigned mi_chain = 0;
    std::vector<DMode> modes;
    bool has_spec = false, has_svooc = false;
    unsigned sweep_mask = 0;
    unsigned sv_end = 0, sv_why = 0, sv_asks = 0, sv_word = 0, sv_vmax = 0, sv_stop = 0;
    unsigned sp_end = 0, sp_why = 0, sp_unconf = 0, sp_cells = 0;
    std::vector<DCell> cells;
    std::vector<DSpecRange> spec_ranges;   // v13: the spec part's form 28 (27 cells, then the ranges)
    unsigned split_pps = 0, split_avs = 0;   // ... and which kinds' merged rows the range lines replace (specsec::split)
    std::vector<DRange> ranges;   // in range-index order of the set bits
};

static Decoded decode(const Bytes& blob) {
    Decoded d;
    d.bits = blob[0] | blob[1] << 8;
    Rd r{blob.data() + 2};
    d.has_spec = r.get(1); d.has_svooc = r.get(1); d.sweep_mask = r.get(4);
    if (d.has_svooc) {
        d.sv_end = r.get(3); d.sv_why = r.get(4); d.sv_asks = r.get(3); d.sv_word = r.get(8); d.sv_vmax = r.get(9); d.sv_stop = r.get(10);
    }
    if (d.has_spec) {
        d.sp_end = r.get(3); d.sp_why = r.get(6); d.sp_unconf = r.get(1); d.sp_cells = r.get(5);
        const unsigned ncells = d.sp_cells == 28 ? 27 : d.sp_cells;
        for (unsigned i = 0; i < ncells; ++i) {
            DCell c;
            c.verdict = r.get(3);
            if (c.verdict == unsigned(sc::Verdict::Skip)) c.skip = r.get(4);
            else if (c.verdict != unsigned(sc::Verdict::Pending)) {
                c.value = r.gets(13);
                if (c.verdict == unsigned(sc::Verdict::Fail)) { c.fails = r.get(8); c.a = r.get(8); c.b = r.get(8); }
            }
            d.cells.push_back(c);
        }
        if (d.sp_cells == 28) {
            const unsigned n = r.get(3);
            d.split_pps = r.get(1); d.split_avs = r.get(1);
            for (unsigned k = 0; k < n; ++k) {
                DSpecRange g;
                g.pos = r.get(4); g.avs = r.get(1); g.tone = r.get(2); g.group = r.get(2);
                for (unsigned s = 0; s < 11; ++s) g.verdicts.push_back(r.get(3));
                d.spec_ranges.push_back(g);
            }
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!(d.sweep_mask >> i & 1)) continue;
        DRange g;
        g.lo_dv = r.get(9); g.n = r.get(10); g.scale = r.get(3);
        const unsigned nb = g.n < 32 ? g.n : 32;
        for (unsigned k = 0; k < nb; ++k) {
            DBucket b;
            b.mean = r.gets(8); b.down = r.get(4); b.up = r.get(4);
            g.b.push_back(b);
        }
        d.ranges.push_back(g);
    }
    if (d.bits - r.pos >= 8) {   // v12: the Xiaomi section after the sweeps
        d.has_mi = true;
        d.mi_chain = r.get(5);
        const unsigned n = r.get(3);
        for (unsigned k = 0; k < n; ++k) {
            DMode m;
            m.before = r.get(16); m.before |= r.get(16) << 16;
            m.after = r.get(16); m.after |= r.get(16) << 16;
            m.mv = r.get(9); m.ma = r.get(7); m.verdict = r.get(3); m.reason = r.get(6);
            d.modes.push_back(m);
        }
    }
    d.used = r.pos;
    return d;
}

// ---- fixtures ----
struct Sweep {
    std::vector<std::int16_t> pts;
    std::vector<ctsweep::RowView> rows;
    // Adds a part: n recorded points starting at lo_mv with the planned last setpoint hi_mv; err(i) is point i's error.
    template <class F> void row(unsigned pos, unsigned lo_mv, unsigned hi_mv, unsigned n, F err) {
        ctsweep::RowView rv{};
        rv.pos = std::uint8_t(pos); rv.alg = std::uint8_t(ct::Algorithm::Pps);
        rv.lo_mv = std::uint16_t(lo_mv); rv.hi_mv = std::uint16_t(hi_mv);
        rv.first = std::uint16_t(pts.size()); rv.n = std::uint16_t(n);
        for (unsigned i = 0; i < n; ++i) pts.push_back(std::int16_t(err(i)));
        rows.push_back(rv);
    }
    ctsweep::View view() const { return {{rows.data(), rows.size()}, {pts.data(), pts.size()}}; }
};
static rc::Card deep_card(unsigned ranges) {
    rc::Card c;
    c.level = ct::Level::Deep;
    c.n_ranges = std::uint8_t(ranges);
    for (unsigned i = 0; i < ranges; ++i) c.range[i] = {std::uint8_t(ct::Algorithm::Pps), std::uint8_t(i + 1), 33, 210, 210, 0, 100};
    return c;
}
static Bytes run_build(const rc::Card& c, const ct::Evidence& ev, const Sweep& s, std::span<const ct::Step> steps = {},
                       const specsec::Apdos* apdo = nullptr) {
    Bytes out(dp::kMaxBlob, 0xEE);
    const std::size_t n = dp::build(c, steps, ev, s.view(), out, apdo);
    out.resize(n);
    return out;
}
static const ct::Evidence& no_ev() { static const ct::Evidence e{}; return e; }
static ct::Step mi_chain_row(std::uint8_t code) {
    ct::Step s; s.group = ct::Group::Pd; s.algorithm = ct::Algorithm::Pd3Fixed; s.hunt = true; s.pos = 0;
    s.verdict = k2::mi_unlock::chain_done(code) ? ct::Verdict::Pass : ct::Verdict::Skip;
    s.reason = k2::mi_unlock::chain_done(code) ? ct::Reason::None : ct::Reason::MiChain;
    s.detail = code;
    return s;
}
static ct::Step mi_mode_row(std::uint8_t pos, std::uint32_t after, std::uint16_t mv, std::uint16_t ma, ct::Verdict v) {
    ct::Step s; s.group = ct::Group::Pd; s.algorithm = ct::Algorithm::Pps; s.hunt = true; s.pos = pos; s.pdo = after;
    s.mv = mv; s.ma = ma; s.verdict = v;
    return s;
}

// ---- the reducer ----

static void test_bucket_count() {
    CHECK_EQ(dp::bucket_count(0), 0u);
    CHECK_EQ(dp::bucket_count(5), 5u);
    CHECK_EQ(dp::bucket_count(32), 32u);
    CHECK_EQ(dp::bucket_count(640), 32u);
}

static void test_flat_zero_stitched_split() {
    // 3.3 V..4.9 V recorded below 5 V and 5.0 V..21.0 V above (the PPS split), error 0: one 178-point span, 32 flat buckets
    Sweep s;
    s.row(1, 3300, 4900, 17, [](unsigned) { return 0; });
    s.row(1, 5000, 21000, 161, [](unsigned) { return 0; });
    const Bytes b = run_build(deep_card(1), no_ev(), s);
    CHECK_COND(!b.empty());
    const Decoded d = decode(b);
    CHECK_EQ(d.bits, d.used);
    CHECK_EQ(b.size(), 2u + (d.bits + 7) / 8);
    CHECK_COND(dp::blob_valid(b));
    CHECK_EQ(d.sweep_mask, 1u);
    CHECK_EQ(d.ranges.size(), 1u);
    CHECK_EQ(d.ranges[0].lo_dv, 33u);
    CHECK_EQ(d.ranges[0].n, 178u);                 // 3300..21000 step 100
    CHECK_EQ(d.ranges[0].scale, 0u);
    CHECK_EQ(d.ranges[0].b.size(), 32u);
    for (const auto& k : d.ranges[0].b) { CHECK_EQ(k.mean, 0); CHECK_EQ(k.down, 0u); CHECK_EQ(k.up, 0u); }
}

static void test_stitch_order_and_adaptive_scale() {
    // low part -50 mV, high part +50 mV, 178 points: the bucket straddling the split mixes both
    Sweep s;
    s.row(1, 3300, 4900, 17, [](unsigned) { return -50; });
    s.row(1, 5000, 21000, 161, [](unsigned) { return 50; });
    const Bytes b = run_build(deep_card(1), no_ev(), s);
    const Decoded d = decode(b);
    // bucket i covers indices [i*178/32, (i+1)*178/32); the split is at index 17: bucket 3 = [16,22): one low + five high points,
    // mean 33.3 mV, min -50, max +50: E = 8 mV is the first scale whose band fits 15 units (E = 4 needs 21 down)
    CHECK_EQ(d.ranges[0].scale, 3u);
    CHECK_EQ(d.ranges[0].b[0].mean, -6);           // -50 / 8 = -6.25 -> -6
    CHECK_EQ(d.ranges[0].b[31].mean, 6);           // +50 / 8 = 6.25 -> 6
    const DBucket& m = d.ranges[0].b[3];
    CHECK_EQ(m.mean, 4);                           // 33.3 / 8 = 4.2 -> 4 (32 mV)
    CHECK_EQ(m.down, 11u);                         // ceil((32 + 50) / 8) = 11
    CHECK_EQ(m.up, 3u);                            // ceil((50 - 32) / 8) = 3
    // the band always covers the data it summarises
    CHECK_COND(int(m.mean) * 8 - int(m.down) * 8 <= -50);
    CHECK_COND(int(m.mean) * 8 + int(m.up) * 8 >= 50);
}

static void test_early_stop_leaves_empty_buckets() {
    // the high part stopped after 10 points of its planned 161: the rest of the span has no data
    Sweep s;
    s.row(1, 3300, 4900, 17, [](unsigned) { return -20; });
    s.row(1, 5000, 21000, 10, [](unsigned) { return -20; });
    const Bytes b = run_build(deep_card(1), no_ev(), s);
    const Decoded d = decode(b);
    CHECK_EQ(d.ranges[0].n, 178u);
    CHECK_EQ(d.ranges[0].b.size(), 32u);
    CHECK_EQ(d.ranges[0].b[0].mean, -20);          // scale 0: no spread, the mean fits
    CHECK_EQ(d.ranges[0].b[31].mean, -128);        // nothing recorded up there
    CHECK_EQ(d.ranges[0].b[31].down, 0u);
    CHECK_EQ(d.ranges[0].b[31].up, 0u);
    CHECK_EQ(d.ranges[0].b[2].mean, -20);
}

static void test_constant_offset_picks_the_scale() {
    // a flat +300 mV error has no spread: only the mean limits the scale (300 / E <= 127: E = 4)
    Sweep s;
    s.row(1, 10000, 25900, 160, [](unsigned) { return 300; });
    const Decoded d = decode(run_build(deep_card(1), no_ev(), s));
    CHECK_EQ(d.ranges[0].scale, 2u);
    CHECK_EQ(d.ranges[0].b[7].mean, 75);
    CHECK_EQ(d.ranges[0].b[7].down, 0u);
    CHECK_EQ(d.ranges[0].b[7].up, 0u);
}

static void test_alternating_errors_pick_the_scale_from_the_spread() {
    // +-300 mV points inside every bucket: mean ~0, the band must reach 300 mV in 15 units: E = 32 mV
    Sweep s;
    s.row(1, 10000, 22700, 128, [](unsigned i) { return i % 2 ? 300 : -300; });
    const Decoded d = decode(run_build(deep_card(1), no_ev(), s));
    CHECK_EQ(d.ranges[0].scale, 5u);
    CHECK_EQ(d.ranges[0].b[3].mean, 0);
    CHECK_EQ(d.ranges[0].b[3].down, 10u);          // ceil(300 / 32)
    CHECK_EQ(d.ranges[0].b[3].up, 10u);
}

static void test_saturation_ends_at_the_top_scale() {
    // 20 V of error does not fit even at E = 128 mV: the mean clamps to +127 at scale 7
    Sweep s;
    s.row(1, 10000, 25900, 160, [](unsigned) { return 20000; });
    const Decoded d = decode(run_build(deep_card(1), no_ev(), s));
    CHECK_EQ(d.ranges[0].scale, 7u);
    CHECK_EQ(d.ranges[0].b[0].mean, 127);
    // the reducer itself reports the saturation
    const dp::Seg a{{s.pts.data(), s.pts.size()}, 10000};
    CHECK_EQ(dp::choose_scale(a, {}, 10000, 160), 7u);
    CHECK_COND(dp::bucket(a, {}, 10000, 160, 0, 7).saturated);
    CHECK_COND(dp::bucket(a, {}, 10000, 160, 0, 0).saturated);              // at E = 1 mV the same data saturates too
}

static void test_short_sweep_has_one_bucket_per_point() {
    Sweep s;
    s.row(1, 9000, 9400, 5, [](unsigned i) { return int(i) * 10 - 20; });
    const Decoded d = decode(run_build(deep_card(1), no_ev(), s));
    CHECK_EQ(d.ranges[0].n, 5u);
    CHECK_EQ(d.ranges[0].b.size(), 5u);
    for (unsigned i = 0; i < 5; ++i) { CHECK_EQ(d.ranges[0].b[i].mean, int(i) * 10 - 20); CHECK_EQ(d.ranges[0].b[i].down, 0u); }
}

static void test_ranges_map_by_algorithm_and_slot() {
    // range 0 has no sweep (the run stopped first), range 1 does: mask 0b0010, one graph
    rc::Card c = deep_card(2);
    Sweep s;
    s.row(2, 9000, 9400, 5, [](unsigned) { return 0; });
    const Decoded d = decode(run_build(c, no_ev(), s));
    CHECK_EQ(d.sweep_mask, 2u);
    CHECK_EQ(d.ranges.size(), 1u);
    CHECK_EQ(d.ranges[0].lo_dv, 90u);
}

// ---- spec and svooc ----

static void test_spec_and_svooc_blocks() {
    ct::Evidence ev{};
    ev.spec.end = specsec::End::Done;
    ev.spec.why = 0;
    ev.spec.unconfirmed = true;
    ev.spec.cells[0] = specsec::make_cell(sc::Verdict::Pass, sc::Skip::None, 0, 1234);
    ev.spec.cells[1] = specsec::make_cell(sc::Verdict::Fail, sc::Skip::None, 7, -321);
    ev.spec.cells[1].a = 5; ev.spec.cells[1].b = 9;
    ev.spec.cells[2] = specsec::make_cell(sc::Verdict::Skip, sc::Skip::NoAvs, 0);
    ev.spec.cells[3] = specsec::make_cell(sc::Verdict::Unverified, sc::Skip::None, 0, specsec::kUnseen);
    ev.spec.cells[4] = specsec::make_cell(sc::Verdict::Pass, sc::Skip::None, 0, 20000);   // saturates to +4094
    // cell 5.. stay Pending
    ev.svooc_end = 7; ev.svooc_why = 3; ev.svooc_asks = 2; ev.svooc_word = 55; ev.svooc_vmax_mv = 11250; ev.svooc_stop_ms = 3300;
    const Sweep s;
    const Decoded d = decode(run_build(deep_card(0), ev, s));
    CHECK_EQ(d.bits, d.used);
    CHECK_COND(d.has_spec);
    CHECK_COND(d.has_svooc);
    CHECK_EQ(d.sweep_mask, 0u);
    CHECK_EQ(d.sv_end, 7u); CHECK_EQ(d.sv_why, 3u); CHECK_EQ(d.sv_asks, 2u); CHECK_EQ(d.sv_word, 55u);
    CHECK_EQ(d.sv_vmax, 112u); CHECK_EQ(d.sv_stop, 33u);
    CHECK_EQ(d.sp_end, unsigned(specsec::End::Done)); CHECK_EQ(d.sp_unconf, 1u);
    CHECK_EQ(d.sp_cells, unsigned(specsec::kCells));
    CHECK_EQ(d.cells.size(), specsec::kCells);
    CHECK_EQ(d.cells[0].verdict, unsigned(sc::Verdict::Pass)); CHECK_EQ(d.cells[0].value, 1234);
    CHECK_EQ(d.cells[1].verdict, unsigned(sc::Verdict::Fail)); CHECK_EQ(d.cells[1].value, -321);
    CHECK_EQ(d.cells[1].fails, 7u); CHECK_EQ(d.cells[1].a, 5u); CHECK_EQ(d.cells[1].b, 9u);
    CHECK_EQ(d.cells[2].verdict, unsigned(sc::Verdict::Skip)); CHECK_EQ(d.cells[2].skip, unsigned(sc::Skip::NoAvs));
    CHECK_EQ(d.cells[3].verdict, unsigned(sc::Verdict::Unverified)); CHECK_EQ(d.cells[3].value, -4096);
    CHECK_EQ(d.cells[4].value, 4094);
    CHECK_EQ(d.cells[5].verdict, unsigned(sc::Verdict::Pending));
}

static void test_nothing_to_carry() {
    const Sweep none;
    rc::Card c = deep_card(1);
    CHECK_EQ(run_build(c, no_ev(), none).size(), 0u);              // Deep, but no spec, no ceiling, no sweep
    ct::Evidence ev{};
    ev.svooc_end = 3;
    c.level = ct::Level::Medium;
    CHECK_EQ(run_build(c, ev, none).size(), 0u);                   // not a Deep card: nothing, whatever the evidence says
    c.level = ct::Level::Light;
    CHECK_EQ(run_build(c, ev, none).size(), 0u);
    c.level = ct::Level::Deep;
    CHECK_COND(run_build(c, ev, none).size() > 0);
}

static void test_worst_case_is_the_budget() {
    // four full ranges, every spec cell failing, the ceiling stage: exactly kMaxBits
    rc::Card c = deep_card(4);
    Sweep s;
    for (unsigned p = 1; p <= 4; ++p) s.row(p, 10000, 25900, 160, [](unsigned i) { return int(i % 7) * 11 - 30; });
    ct::Evidence ev{};
    ev.spec.end = specsec::End::Done;
    for (auto& cell : ev.spec.cells) { cell = specsec::make_cell(sc::Verdict::Fail, sc::Skip::None, 255, -4000); cell.a = 255; cell.b = 255; }
    ev.svooc_end = 7; ev.svooc_word = 255; ev.svooc_vmax_mv = 51100; ev.svooc_stop_ms = 60000;
    // and the Xiaomi section at its worst: four modes
    std::vector<ct::Step> steps{mi_chain_row(k2::mi_unlock::kCodeFinished)};
    for (unsigned k = 0; k < k2::mi_unlock::kMaxModes; ++k) steps.push_back(mi_mode_row(std::uint8_t(6 + k), 0xC8DC3278u, 21000, 6000, ct::Verdict::Pass));
    // and four PPS / AVS ranges with their own spec cells (v13)
    specsec::Apdos ap;
    for (std::uint8_t p = 4; p <= 7; ++p) ap.add(sc::Check::PpsBand, {300000, 0, 0, 275000, std::uint8_t(p | 1 << 4), 50});
    const Bytes b = run_build(c, ev, s, steps, &ap);
    const Decoded d = decode(b);
    CHECK_EQ(d.modes.size(), std::size_t(k2::mi_unlock::kMaxModes));
    CHECK_EQ(d.spec_ranges.size(), 4u);
    CHECK_EQ(dp::kMaxBits, 3638u + 3u + 2u + 4u * 42u);
    CHECK_EQ(dp::kMaxBlob, std::size_t(479));
    CHECK_EQ(d.bits, dp::kMaxBits);
    CHECK_EQ(d.used, dp::kMaxBits);
    CHECK_EQ(b.size(), dp::kMaxBlob);
    CHECK_COND(dp::blob_valid(b));
    CHECK_EQ(d.ranges.size(), 4u);
    CHECK_EQ(d.sv_vmax, 511u); CHECK_EQ(d.sv_stop, 600u); CHECK_EQ(d.sv_word, 255u);
    CHECK_EQ(d.cells[26].fails, 255u);
}

// v13: each PPS / AVS range's spec word and slot verdicts ride in the spec part (form 28 = the 27 cells, then the ranges); without
// ranges the part keeps form 27, so a v12 reader still reads it. read_mi skips the ranges to the Xiaomi tail.
static void test_spec_ranges() {
    const Sweep none;
    ct::Evidence ev{};
    ev.spec.end = specsec::End::Done;
    specsec::Apdos ap;
    ap.add(sc::Check::PpsBand, {100000, 0, 0, 275000, std::uint8_t(5 | 1 << 4), 50});
    ap.add(sc::Check::PpsOvershoot, {9700, 0, -sc::kOpenLimit, 9500, std::uint8_t(5 | 2 << 4), 95});
    ap.add(sc::Check::AvsAccuracy, {15200, 0, 14700, 15300, std::uint8_t(7 | 2 << 4), 150});
    std::vector<ct::Step> steps{mi_chain_row(k2::mi_unlock::kCodeVerified), mi_mode_row(6, 0xC0DC3264u, 11000, 5000, ct::Verdict::Pass)};
    const Bytes b = run_build(deep_card(0), ev, none, steps, &ap);
    const Decoded d = decode(b);
    CHECK_EQ(d.used, d.bits);
    CHECK_EQ(d.sp_cells, 28u);
    CHECK_EQ(d.cells.size(), specsec::kCells);
    CHECK_EQ(d.spec_ranges.size(), 2u);
    CHECK_EQ(d.spec_ranges[0].pos, 5u); CHECK_EQ(d.spec_ranges[0].avs, 0u);
    CHECK_EQ(d.spec_ranges[0].tone, unsigned(specsec::RangeTone::Fail));
    CHECK_EQ(d.spec_ranges[0].group, unsigned(specsec::Group::Shape));
    CHECK_EQ(d.spec_ranges[0].verdicts[specsec::apdo_slot(sc::Check::PpsBand)], unsigned(sc::Verdict::Pass));
    CHECK_EQ(d.spec_ranges[0].verdicts[specsec::apdo_slot(sc::Check::PpsOvershoot)], unsigned(sc::Verdict::Fail));
    CHECK_EQ(d.spec_ranges[0].verdicts[specsec::apdo_slot(sc::Check::PpsStatus)], unsigned(sc::Verdict::Pending));
    CHECK_EQ(d.spec_ranges[1].pos, 7u); CHECK_EQ(d.spec_ranges[1].avs, 1u);
    CHECK_EQ(d.spec_ranges[1].tone, unsigned(specsec::RangeTone::Pass));
    CHECK_EQ(d.modes.size(), 1u);
    CHECK_EQ(dp::read_mi(b).n, 1u);
    CHECK_EQ(d.split_pps, 0u); CHECK_EQ(d.split_avs, 0u);   // no range line on the grid: the merged rows stay
    // The PPS range has its line (its range step), the AVS one has none: only PPS splits, as the grid draws it.
    ct::Step pps{};
    pps.algorithm = ct::Algorithm::Pps; pps.group = ct::group(pps.algorithm); pps.pos = 5; pps.pdo = 0xC8D23264u; pps.mv = 5000;
    pps.verdict = ct::Verdict::Pass;
    steps.push_back(pps);
    const Decoded s = decode(run_build(deep_card(0), ev, none, steps, &ap));
    CHECK_EQ(s.used, s.bits);
    CHECK_EQ(s.split_pps, 1u); CHECK_EQ(s.split_avs, 0u);
    CHECK_EQ(dp::read_mi(run_build(deep_card(0), ev, none, steps, &ap)).n, 1u);
    // No range measured: form 27, the v12 layout.
    const Decoded e = decode(run_build(deep_card(0), ev, none, {}, nullptr));
    CHECK_EQ(e.sp_cells, 27u); CHECK_EQ(e.used, e.bits);
    const specsec::Apdos empty;
    CHECK_EQ(decode(run_build(deep_card(0), ev, none, {}, &empty)).sp_cells, 27u);
}

// The Xiaomi section (v12): the chain row's outcome and each mode row, read back by the independent reader and by read_mi.
static void test_xiaomi_section() {
    const Sweep none;
    rc::Card c = deep_card(0);
    c.level = ct::Level::Medium;
    // a) the bank: PPS 5-11V 3A -> 5A, tested at 11 V 5 A
    ct::Evidence ev{};
    ev.mi_chain = k2::mi_unlock::kCodeVerified; ev.mi_n = 1; ev.mi[0] = {0xC0DC323Cu, 0xC0DC3264u, 6};
    const std::vector<ct::Step> bank{mi_chain_row(k2::mi_unlock::kCodeVerified), mi_mode_row(6, 0xC0DC3264u, 11000, 5000, ct::Verdict::Pass)};
    const Bytes b = run_build(c, ev, none, bank);
    CHECK_COND(b.size() > 0);                                      // a Medium card carries the block for its Xiaomi section
    const Decoded d = decode(b);
    CHECK_EQ(d.has_spec || d.has_svooc || d.sweep_mask, false);
    CHECK_EQ(d.used, d.bits);
    CHECK_EQ(d.mi_chain, unsigned(k2::mi_unlock::kCodeVerified));
    CHECK_EQ(d.modes.size(), 1u);
    CHECK_EQ(d.modes[0].before, 0xC0DC323Cu); CHECK_EQ(d.modes[0].after, 0xC0DC3264u);
    CHECK_EQ(d.modes[0].mv, 110u); CHECK_EQ(d.modes[0].ma, 100u);
    CHECK_EQ(d.modes[0].verdict, unsigned(ct::Verdict::Pass));
    const dp::MiSection m = dp::read_mi(b);
    CHECK_EQ(m == dp::mi_of(bank, ev), true);
    CHECK_EQ(dp::mi_watts(m.mode[0]), 55u);
    // b) FH22: done, nothing raised
    const std::vector<ct::Step> fh22{mi_chain_row(k2::mi_unlock::kCodeVerified)};
    const Decoded e = decode(run_build(c, no_ev(), none, fh22));
    CHECK_EQ(e.mi_chain, unsigned(k2::mi_unlock::kCodeVerified)); CHECK_EQ(e.modes.size(), 0u);
    // no SVID: a source that never listed 0x2717 is no Xiaomi charger; its card carries no section (no block on a Medium card)
    ct::Step skip = mi_chain_row(k2::mi_unlock::kCodeNoSvid); skip.verdict = ct::Verdict::Skip;
    const std::vector<ct::Step> nosvid{skip};
    CHECK_EQ(run_build(c, no_ev(), none, nosvid).size(), 0u);
    CHECK_EQ(dp::mi_of(nosvid, no_ev()).chain, 0u);
    // no answer: the source listed the SVID and the chain got nothing back; carried, the source is a Xiaomi one
    ct::Step mute = mi_chain_row(k2::mi_unlock::kCodeNoAnswer); mute.verdict = ct::Verdict::Skip;
    const std::vector<ct::Step> noanswer{mute};
    CHECK_EQ(decode(run_build(c, no_ev(), none, noanswer)).mi_chain, unsigned(k2::mi_unlock::kCodeNoAnswer));
    // the chain row has not run: nothing to carry
    ct::Step pending = mi_chain_row(0); pending.verdict = ct::Verdict::Pending;
    const std::vector<ct::Step> early{pending};
    CHECK_EQ(run_build(c, no_ev(), none, early).size(), 0u);
    // c) a resumed run lost the PDO before the chain (Evidence::mi is not journaled): kBeforeLost, never "new"
    const std::vector<ct::Step> cuk{mi_chain_row(k2::mi_unlock::kCodeFinished), mi_mode_row(6, 0xC8DC3278u, 11000, 6000, ct::Verdict::Pass)};
    const dp::MiSection lost = dp::read_mi(run_build(c, no_ev(), none, cuk));
    CHECK_EQ(lost.n, 1u); CHECK_EQ(lost.mode[0].before, dp::kBeforeLost); CHECK_EQ(dp::mi_watts(lost.mode[0]), 66u);
    // behind the Deep parts: read_mi walks the spec cells and the sweeps to find it
    rc::Card dc = deep_card(1);
    Sweep s;
    s.row(1, 3300, 21000, 60, [](unsigned i) { return int(i % 5) - 2; });
    ct::Evidence dev = ev;
    dev.svooc_end = 2;
    dev.spec.end = specsec::End::Done;
    for (auto& cell : dev.spec.cells) cell = specsec::make_cell(sc::Verdict::Fail, sc::Skip::None, 3, -40);
    const Bytes db = run_build(dc, dev, s, bank);
    const Decoded dd = decode(db);
    CHECK_EQ(dd.has_spec && dd.has_svooc && dd.sweep_mask == 1u && dd.has_mi, true);
    CHECK_EQ(dd.used, dd.bits);
    CHECK_EQ(dp::read_mi(db) == m, true);
    // a v11 block (no section) and a broken one read as none
    CHECK_EQ(dp::read_mi(run_build(dc, dev, s)).chain, 0u);
    Bytes cut = db; cut.pop_back();
    CHECK_EQ(dp::read_mi(cut).chain, 0u);
}

static void test_blob_valid_checks() {
    CHECK_COND(!dp::blob_valid({}));
    const std::uint8_t two[2] = {0, 0};
    CHECK_COND(!dp::blob_valid(two));                              // zero bits is no blob
    const std::uint8_t cut[3] = {20, 0, 1};                        // 20 bits need 3 data bytes
    CHECK_COND(!dp::blob_valid(cut));
    const std::uint8_t ok[5] = {20, 0, 1, 2, 3};
    CHECK_COND(dp::blob_valid(ok));
}

int main() {
    test_bucket_count();
    test_flat_zero_stitched_split();
    test_stitch_order_and_adaptive_scale();
    test_early_stop_leaves_empty_buckets();
    test_constant_offset_picks_the_scale();
    test_alternating_errors_pick_the_scale_from_the_spread();
    test_saturation_ends_at_the_top_scale();
    test_short_sweep_has_one_bucket_per_point();
    test_ranges_map_by_algorithm_and_slot();
    test_spec_and_svooc_blocks();
    test_nothing_to_carry();
    test_worst_case_is_the_budget();
    test_xiaomi_section();
    test_spec_ranges();
    test_blob_valid_checks();
    return test_report("test_report_card_deep");
}
