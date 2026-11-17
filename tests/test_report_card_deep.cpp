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
struct Decoded {
    unsigned bits = 0, used = 0;
    bool has_spec = false, has_svooc = false;
    unsigned sweep_mask = 0;
    unsigned sv_end = 0, sv_why = 0, sv_asks = 0, sv_word = 0, sv_vmax = 0, sv_stop = 0;
    unsigned sp_end = 0, sp_why = 0, sp_unconf = 0, sp_cells = 0;
    std::vector<DCell> cells;
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
        for (unsigned i = 0; i < d.sp_cells; ++i) {
            DCell c;
            c.verdict = r.get(3);
            if (c.verdict == unsigned(sc::Verdict::Skip)) c.skip = r.get(4);
            else if (c.verdict != unsigned(sc::Verdict::Pending)) {
                c.value = r.gets(13);
                if (c.verdict == unsigned(sc::Verdict::Fail)) { c.fails = r.get(8); c.a = r.get(8); c.b = r.get(8); }
            }
            d.cells.push_back(c);
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
static Bytes run_build(const rc::Card& c, const ct::Evidence& ev, const Sweep& s) {
    Bytes out(dp::kMaxBlob, 0xEE);
    const std::size_t n = dp::build(c, ev, s.view(), out);
    out.resize(n);
    return out;
}
static const ct::Evidence& no_ev() { static const ct::Evidence e{}; return e; }

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
    const Bytes b = run_build(c, ev, s);
    const Decoded d = decode(b);
    CHECK_EQ(d.bits, dp::kMaxBits);
    CHECK_EQ(d.used, dp::kMaxBits);
    CHECK_EQ(b.size(), dp::kMaxBlob);
    CHECK_COND(dp::blob_valid(b));
    CHECK_EQ(d.ranges.size(), 4u);
    CHECK_EQ(d.sv_vmax, 511u); CHECK_EQ(d.sv_stop, 600u); CHECK_EQ(d.sv_word, 255u);
    CHECK_EQ(d.cells[26].fails, 255u);
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
    test_blob_valid_checks();
    return test_report("test_report_card_deep");
}
