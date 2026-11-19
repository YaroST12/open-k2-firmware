// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD spec section's per-APDO results (include/k2/chargertest/spec_section.hpp Apdos): the measurements of each PPS / AVS APDO folded
// into its own cells the way the merged aggregate folds them, and the one word the range's Results cell shows.
#include "k2/chargertest/spec_section.hpp"
#include "test_util.h"

using namespace k2;
namespace sc = k2::pdspec;
using C = sc::Check;

namespace {

sc::Meas meas(std::int32_t v, std::int32_t lo, std::int32_t hi, std::uint8_t pos, std::uint8_t cls = 1, std::uint8_t b = 50) {
    return {v, 0, lo, hi, std::uint8_t(pos | cls << 4), b};
}

// Each APDO keeps its own cells; a cell agrees with the merged aggregate over the same measurements (verdict, count, fails, the worst
// value and its limit, quantised as the section's cells are).
void cells_follow_the_aggregate_per_apdo() {
    specsec::Apdos a;
    sc::Agg merged5;
    const sc::Meas m5[] = {meas(120000, 0, 275000, 5), meas(300000, 0, 275000, 5), meas(90000, 0, 275000, 5)};
    for (const auto& x : m5) { a.add(C::PpsBand, x); merged5.add(sc::Kind::Max, x); }
    a.add(C::PpsBand, meas(100000, 0, 275000, 6));
    a.add(C::AvsOvershoot, meas(20900, -sc::kOpenLimit, 20500, 7));
    CHECK_EQ(int(a.n), 3);
    const specsec::Apdo* p5 = a.find(5);
    CHECK_COND(p5 != nullptr);
    CHECK_EQ(int(p5->avs), 0);
    const specsec::Cell& band = p5->cells[specsec::apdo_slot(C::PpsBand)];
    const specsec::Cell ref = specsec::cell_of(C::PpsBand, merged5);
    CHECK_COND(band.verdict() == sc::Verdict::Fail);
    CHECK_EQ(int(band.n), 3);
    CHECK_EQ(int(band.fails), 1);
    CHECK_EQ(int(band.value), int(ref.value));
    CHECK_EQ(int(band.hi), int(ref.hi));
    CHECK_EQ(int(band.a), 5 | 1 << 4);
    CHECK_EQ(int(a.find(6)->cells[specsec::apdo_slot(C::PpsBand)].n), 1);
    CHECK_COND(a.find(6)->cells[specsec::apdo_slot(C::PpsBand)].verdict() == sc::Verdict::Pass);
    CHECK_EQ(int(a.find(7)->avs), 1);
    CHECK_COND(a.find(7)->cells[specsec::apdo_slot(C::AvsOvershoot)].verdict() == sc::Verdict::Fail);
    CHECK_COND(a.find(4) == nullptr);
    // The worst of passing measurements is the one nearest its limit, after a failure the failure stays the worst.
    specsec::Apdos b;
    b.add(C::PpsAccuracy, meas(9000, 8800, 9200, 3));
    b.add(C::PpsAccuracy, meas(9180, 8800, 9200, 3));
    b.add(C::PpsAccuracy, meas(9100, 8800, 9200, 3));
    CHECK_EQ(int(b.find(3)->cells[specsec::apdo_slot(C::PpsAccuracy)].value), 918);
    // A pass on its bracket (an ms-fallback Accept) is kept at its limit, as the merged aggregate keeps it.
    sc::Agg acc;
    for (const sc::Meas x : {sc::Meas{12000, 0, 0, 17000, 9, 150}, sc::Meas{20000, 5000, 0, 17000, 9, 157}}) {
        b.add(C::AcceptTime, x);
        acc.add(sc::Kind::Max, x);
    }
    const specsec::Cell& ac = b.find(9)->cells[specsec::apdo_slot(C::AcceptTime)];
    CHECK_COND(ac.verdict() == sc::Verdict::Pass);
    CHECK_EQ(int(ac.value), int(specsec::cell_of(C::AcceptTime, acc).value));
    CHECK_EQ(int(ac.b), 157);
    CHECK_EQ(acc.worst, 17000);
    // An inferred limit (PPS settle) reads unverified, measured or over.
    b.add(C::PpsSettle, meas(40000, 0, 25000, 3, 0));
    CHECK_COND(b.find(3)->cells[specsec::apdo_slot(C::PpsSettle)].verdict() == sc::Verdict::Unverified);
    // A fifth APDO has no room: dropped, the four kept.
    specsec::Apdos full;
    for (std::uint8_t pos = 2; pos <= 6; ++pos) full.add(C::PpsBand, meas(100000, 0, 275000, pos));
    CHECK_EQ(int(full.n), int(sc::kMaxProg));
    CHECK_COND(full.find(6) == nullptr);
}

// The slots: the PPS and AVS counterparts share one, a fixed-walk check has none; the Accept of a move is the Timing group's.
void slots_and_groups() {
    CHECK_EQ(specsec::apdo_slot(C::PpsBand), specsec::apdo_slot(C::AvsBand));
    CHECK_EQ(specsec::apdo_slot(C::PpsTrack), specsec::apdo_slot(C::AvsTrack));
    CHECK_EQ(int(specsec::apdo_slot(C::RiseTime)), int(specsec::kApdoSlots));
    CHECK_COND(specsec::apdo_check(true, specsec::apdo_slot(C::PpsSettle)) == C::Count);   // AVS has no settle class
    CHECK_COND(specsec::apdo_check(false, specsec::apdo_slot(C::AcceptTime)) == C::AcceptTime);
    CHECK_COND(specsec::slot_group(specsec::apdo_slot(C::PpsAccuracy)) == specsec::Group::Level);
    CHECK_COND(specsec::slot_group(specsec::apdo_slot(C::PpsStatus)) == specsec::Group::Level);
    CHECK_COND(specsec::slot_group(specsec::apdo_slot(C::PpsPsRdy)) == specsec::Group::Timing);
    CHECK_COND(specsec::slot_group(specsec::apdo_slot(C::AcceptTime)) == specsec::Group::Timing);
    CHECK_COND(specsec::slot_group(specsec::apdo_slot(C::PpsTransient)) == specsec::Group::Shape);
}

// The range's word: "spec" when every judged check passed, else the family of the failing checks (the one with the larger penalty);
// over an unverified limit only is a caution, nothing measured reads None.
void the_range_word() {
    specsec::Apdos a;
    CHECK_COND(specsec::range_spec(a.r[0]).tone == specsec::RangeTone::None);
    a.add(C::PpsBand, meas(100000, 0, 275000, 5));
    a.add(C::PpsAccuracy, meas(9000, 8800, 9200, 5));
    auto rs = specsec::range_spec(*a.find(5));
    CHECK_COND(rs.tone == specsec::RangeTone::Pass);
    CHECK_STR(specsec::range_word(rs), "spec");
    a.add(C::PpsSettle, meas(40000, 0, 25000, 5, 0));
    rs = specsec::range_spec(*a.find(5));
    CHECK_COND(rs.tone == specsec::RangeTone::Caution);
    CHECK_STR(specsec::range_word(rs), "timing");
    a.add(C::PpsOvershoot, meas(9700, -sc::kOpenLimit, 9500, 5));   // weight 3, one fail: shape 3
    rs = specsec::range_spec(*a.find(5));
    CHECK_COND(rs.tone == specsec::RangeTone::Fail);
    CHECK_STR(specsec::range_word(rs), "shape");
    a.add(C::PpsAccuracy, meas(9300, 8800, 9200, 5));   // weight 2
    a.add(C::PpsAccuracy, meas(9400, 8800, 9200, 5));   // level 4 > shape 3
    CHECK_STR(specsec::range_word(specsec::range_spec(*a.find(5))), "level");
}

// The wire form of one range (the journal record /jr/sa<n>.rec, the remote read 0x0B [9, off]): pos, avs, then per slot the cell's
// code, fails, value, lo, hi, n (little endian), a, b. The stream the remote reads is the count, then each range's form.
void wire_round_trip() {
    specsec::Apdos a;
    a.add(C::PpsBand, meas(300000, 0, 275000, 5));
    a.add(C::PpsAccuracy, meas(9180, 8800, 9200, 5));
    a.add(C::AvsOvershoot, meas(20900, -sc::kOpenLimit, 20500, 7, 2, 205));
    std::array<std::uint8_t, specsec::kApdoWireBytes> w{};
    specsec::to_apdo_wire(a.r[0], w);
    CHECK_EQ(int(specsec::kApdoWireBytes), 2 + 12 * 11);
    CHECK_EQ(int(w[0]), 5);
    CHECK_EQ(int(w[1]), 0 | 3 << 1 | 1 << 3);   // PPS, the range's word: Fail, timing (the band)
    const std::size_t band = 2 + 12 * specsec::apdo_slot(C::PpsBand);
    CHECK_EQ(int(w[band + 1]), 1);                             // fails
    CHECK_EQ(int(w[band + 2] | w[band + 3] << 8), 3000);       // 300 ms in 0.1 ms
    specsec::Apdos b;
    CHECK_COND(specsec::from_apdo_wire(w.data(), w.size(), b));
    specsec::to_apdo_wire(a.r[1], w);
    CHECK_COND(specsec::from_apdo_wire(w.data(), w.size(), b));
    CHECK_EQ(int(b.n), 2);
    CHECK_COND(b.r[0].cells == a.r[0].cells && b.r[1].cells == a.r[1].cells && b.r[1].avs && b.r[1].pos == 7);
    // The same range again replaces its entry; a damaged form changes nothing.
    CHECK_COND(specsec::from_apdo_wire(w.data(), w.size(), b));
    CHECK_EQ(int(b.n), 2);
    w[1] = 0x20;
    CHECK_COND(!specsec::from_apdo_wire(w.data(), w.size(), b));
    w[1] = 1; w[2 + 12 * 4] = 7;                               // verdict 7
    CHECK_COND(!specsec::from_apdo_wire(w.data(), w.size(), b));
    w[2 + 12 * 4] = 0; w[0] = 0;
    CHECK_COND(!specsec::from_apdo_wire(w.data(), w.size(), b));
    CHECK_COND(!specsec::from_apdo_wire(w.data(), w.size() - 1, b));
    CHECK_COND(b.r[1].cells == a.r[1].cells);
    // The stream: count, then each range.
    CHECK_EQ(int(specsec::apdo_stream_size(a)), 1 + 2 * int(specsec::kApdoWireBytes));
    CHECK_EQ(int(specsec::apdo_stream_byte(a, 0)), 2);
    CHECK_EQ(int(specsec::apdo_stream_byte(a, 0, specsec::split_bits({true, true}))), 2 | 1 << 4 | 1 << 5);   // the split rides on the count
    specsec::to_apdo_wire(a.r[1], w);
    bool same = true;
    for (std::size_t i = 0; i < w.size(); ++i) same = same && specsec::apdo_stream_byte(a, 1 + specsec::kApdoWireBytes + i) == w[i];
    CHECK_COND(same);
    CHECK_EQ(int(specsec::apdo_stream_size(specsec::Apdos{})), 0);   // nothing to read: no range measured
}

}  // namespace

// Layout B (user decision 2026-11-19): a kind's range lines replace its merged PPS / AVS rows when the stage ran, measured a range of
// that kind and every such range has a line on the grid; otherwise that kind's merged rows stay (nothing is hidden). Transitions and
// the observer always stay.
void the_split_rule() {
    specsec::Result r;
    r.end = specsec::End::Done;
    specsec::Apdos a;
    a.add(C::PpsBand, meas(100000, 0, 275000, 5));
    a.add(C::PpsBand, meas(100000, 0, 275000, 6));
    a.add(C::AvsBand, meas(30000, 0, 50000, 10));
    const std::uint16_t all = 1u << 5 | 1u << 6 | 1u << 10;
    specsec::Split s = specsec::split(r, a, all);
    CHECK_COND(s.pps && s.avs);
    CHECK_EQ(int(specsec::shown_n(s)), 10);
    CHECK_EQ(int(specsec::shown_cell(s, 0)), 0);
    CHECK_EQ(int(specsec::shown_cell(s, 8)), 8);
    CHECK_COND(specsec::kOrder[specsec::shown_cell(s, 9)] == C::SourceReset);
    CHECK_EQ(int(specsec::shown_cell(s, 10)), int(specsec::kCells));
    CHECK_COND(specsec::kOrder[specsec::kPpsFirst] == C::PpsMonotonic && specsec::kOrder[specsec::kAvsFirst] == C::AvsMonotonic);
    // PPS 6 without a line (its unit deselected, its chip folded): PPS keeps its merged rows, AVS still splits.
    s = specsec::split(r, a, all & ~(1u << 6));
    CHECK_COND(!s.pps && s.avs);
    CHECK_EQ(int(specsec::shown_n(s)), 9 + 10 + 1);
    CHECK_COND(specsec::kOrder[specsec::shown_cell(s, 9)] == C::PpsMonotonic);
    CHECK_COND(specsec::kOrder[specsec::shown_cell(s, 19)] == C::SourceReset);
    // No range of a kind (no AVS measured), no stage, no ranges kept (an old journal): nothing splits there.
    specsec::Apdos pps_only;
    pps_only.add(C::PpsBand, meas(100000, 0, 275000, 5));
    s = specsec::split(r, pps_only, all);
    CHECK_COND(s.pps && !s.avs);
    CHECK_EQ(int(specsec::shown_n(s)), 9 + 7 + 1);
    s = specsec::split(r, specsec::Apdos{}, all);
    CHECK_COND(!s.pps && !s.avs);
    CHECK_EQ(int(specsec::shown_n(s)), int(specsec::kCells));
    specsec::Result none;
    s = specsec::split(none, a, all);
    CHECK_COND(!s.pps && !s.avs);
}

// A range's Detail lists the slots its kind has (PPS 11, AVS 8); a slot the range never measured shows the merged cell's skip reason, or
// reads pending.
void the_range_slots() {
    CHECK_EQ(int(specsec::slot_n(false)), 11);
    CHECK_EQ(int(specsec::slot_n(true)), 8);
    for (std::size_t k = 0; k < specsec::slot_n(true); ++k) CHECK_COND(specsec::apdo_check(true, specsec::slot_at(true, k)) != C::Count);
    CHECK_EQ(int(specsec::slot_at(true, 8)), int(specsec::kApdoSlots));
    specsec::Result r;
    r.end = specsec::End::Done;
    for (std::size_t i = 0; i < specsec::kCells; ++i) r.cells[i] = specsec::make_cell(sc::Verdict::Pass, sc::Skip::None, 0);
    const std::size_t st = specsec::cell_index(C::PpsStatus);
    CHECK_COND(specsec::kOrder[st] == C::PpsStatus);
    r.cells[st] = specsec::make_cell(sc::Verdict::Skip, sc::Skip::NoData, 0);
    specsec::Apdos a;
    a.add(C::PpsBand, meas(100000, 0, 275000, 5));
    const specsec::Apdo& p = *a.find(5);
    CHECK_EQ(int(specsec::slot_cell(r, p, specsec::apdo_slot(C::PpsBand)).n), 1);
    const specsec::Cell skip = specsec::slot_cell(r, p, specsec::apdo_slot(C::PpsStatus));
    CHECK_COND(skip.verdict() == sc::Verdict::Skip && skip.skip() == sc::Skip::NoData);
    CHECK_COND(specsec::slot_cell(r, p, specsec::apdo_slot(C::PpsAccuracy)).verdict() == sc::Verdict::Pending);
}

// The range word names its worst slot too (the footer of the range line).
void the_worst_slot() {
    specsec::Apdos a;
    a.add(C::PpsBand, meas(100000, 0, 275000, 5));
    CHECK_EQ(int(specsec::range_spec(*a.find(5)).slot), int(specsec::apdo_slot(C::PpsBand)));
    a.add(C::PpsOvershoot, meas(9900, 8000, 9655, 5));
    a.add(C::PpsOvershoot, meas(9900, 8000, 9655, 5));
    a.add(C::PpsBand, meas(300000, 0, 275000, 5));
    const specsec::RangeSpec rs = specsec::range_spec(*a.find(5));
    CHECK_COND(rs.tone == specsec::RangeTone::Fail);
    CHECK_EQ(int(rs.slot), int(specsec::apdo_slot(rs.group == specsec::Group::Shape ? C::PpsOvershoot : C::PpsBand)));
}

int main() {
    cells_follow_the_aggregate_per_apdo();
    the_split_rule();
    the_range_slots();
    the_worst_slot();
    slots_and_groups();
    the_range_word();
    wire_round_trip();
    return test_report("test_spec_apdo");
}
