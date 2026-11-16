// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD spec section's texts (k2/ui/spec_text.hpp): the cell value, the grid cell, the detail rows and the quantized value a cell keeps.
#include <cstdint>
#include <cstring>
#include <string_view>

#include "test_util.h"
#include "k2/chargertest/spec_section.hpp"
#include "k2/ui/spec_text.hpp"

using namespace k2;
namespace sc = pdspec;

static std::string_view value_text(sc::Unit u, std::int32_t v) {
    static StrBuf<24> s;
    s.clear();
    spectext::put_value(s, u, v);
    return s.view();
}

static void test_quantize() {
    using specsec::quantize; using specsec::dequantize; using specsec::kUnseen;
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Us, 1234)), 12);        // 0.1 ms steps
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Us, 1250)), 13);        // half rounds away from zero
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Us, -1250)), -13);
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Mv, 5049)), 505);       // 10 mV steps
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Mv, -5049)), -505);
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Count, 7)), 7);
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Ms, 90)), 90);
    // Never seen: the sentinel, for every unit, and for anything past it.
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Us, sc::kNever)), static_cast<int>(kUnseen));
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Mv, 2147483647)), static_cast<int>(kUnseen));
    CHECK_EQ(dequantize(sc::Unit::Count, kUnseen), static_cast<std::int32_t>(sc::kNever));
    // Saturates, never wraps; the sentinel stays out of reach of a real value.
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Count, 100000)), 32766);
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Count, -100000)), -32766);
    CHECK_EQ(static_cast<int>(quantize(sc::Unit::Us, sc::kNever - 1)), 32766);
    CHECK_EQ(dequantize(sc::Unit::Us, 12), 1200);
    CHECK_EQ(dequantize(sc::Unit::Mv, -505), -5050);
}

static void test_value_text() {
    CHECK_COND(value_text(sc::Unit::Us, 12300) == "12.3ms");
    CHECK_COND(value_text(sc::Unit::Us, -400) == "-0.4ms");
    CHECK_COND(value_text(sc::Unit::Ms, 90) == "90ms");
    CHECK_COND(value_text(sc::Unit::Mv, 5050) == "5.05V");
    CHECK_COND(value_text(sc::Unit::Mv, -120) == "-0.12V");
    CHECK_COND(value_text(sc::Unit::Count, 3) == "3");
    CHECK_COND(value_text(sc::Unit::Count, -2147483647 - 1) == "-2147483648");
    CHECK_COND(value_text(sc::Unit::Us, static_cast<std::int32_t>(sc::kNever)) == "not seen");
}

static const sc::CheckDef& cdef(sc::Check c) { return sc::kDefs[static_cast<std::size_t>(c)]; }

static void test_limit_text() {
    StrBuf<24> s;
    sc::Agg a;
    a.hi = 5000;
    spectext::put_limit(s, cdef(sc::Check::AcceptTime), a);
    CHECK_COND(s.view().substr(0, 4) == "max ");
    s.clear();
    a.lo = 4750; a.hi = 5250;
    spectext::put_limit(s, cdef(sc::Check::Accuracy), a);
    CHECK_COND(s.view() == "4.75V-5.25V");
    s.clear();
    a.lo = -sc::kOpenLimit; a.hi = 500;   // an open low side: only the bound that applies
    spectext::put_limit(s, cdef(sc::Check::Overshoot), a);
    CHECK_COND(s.view() == "max 0.50V");
}

static void test_where_text() {
    StrBuf<20> s;
    CHECK_COND(spectext::put_where(static_cast<int>(sc::Check::PsRdyTime), 2, 3, s));
    CHECK_COND(s.view() == "PDO 2 to 3");
    s.clear();
    // A programmable move: APDO 5, step class 2, down, 10.9 V.
    CHECK_COND(spectext::put_where(static_cast<int>(sc::Check::PpsAccuracy), 0x80 | (2 << 4) | 5, 109, s));
    CHECK_COND(s.view() == "APDO 5 c2 dn 10.9V");
    s.clear();
    CHECK_COND(spectext::put_where(static_cast<int>(sc::Check::AvsBand), 0x03, 250, s));
    CHECK_COND(s.view() == "APDO 3 c0 up 25.0V");
    s.clear();
    CHECK_COND(!spectext::put_where(static_cast<int>(sc::Check::SourceReset), 0, 0, s));   // no object: "the source"
    CHECK_COND(s.view() == "the source");
}

static specsec::Cell cell_of(sc::Verdict v, unsigned fails, std::int16_t value, sc::Skip sk = sc::Skip::None) {
    return specsec::make_cell(v, sk, fails, value);
}

static void test_cell_text() {
    StrBuf<24> s;
    std::string_view shown;
    // Pass: the value alone.
    CHECK_EQ(static_cast<int>(spectext::cell_view(0, cell_of(sc::Verdict::Pass, 0, 123), s, shown)), static_cast<int>(kit::Verdict::Pass));
    CHECK_COND(shown == "12.3ms");   // AcceptTime is in us: 123 x 0.1 ms
    // Fail: the value and the count.
    s.clear();
    CHECK_EQ(static_cast<int>(spectext::cell_view(8, cell_of(sc::Verdict::Fail, 3, 505), s, shown)), static_cast<int>(kit::Verdict::Fail));
    CHECK_COND(shown == "5.05V, 3 fail");
    // Unverified: the number, no pass/fail look.
    s.clear();
    CHECK_EQ(static_cast<int>(spectext::cell_view(11, cell_of(sc::Verdict::Unverified, 0, 40), s, shown)), static_cast<int>(kit::Verdict::None));
    CHECK_COND(!shown.empty());
    // Skip: the reason, struck out by the grid.
    s.clear();
    CHECK_EQ(static_cast<int>(spectext::cell_view(9, cell_of(sc::Verdict::Skip, 0, 0, sc::Skip::NoPps), s, shown)), static_cast<int>(kit::Verdict::Skip));
    CHECK_COND(shown == sc::skip_name(sc::Skip::NoPps));
    // Pending: an empty cell.
    s.clear();
    CHECK_EQ(static_cast<int>(spectext::cell_view(9, cell_of(sc::Verdict::Pending, 0, 0), s, shown)), static_cast<int>(kit::Verdict::Pending));
    CHECK_COND(shown.empty());
    // The same cell in a nutshell: the most extreme value any cell can hold still fits the 24-byte buffer whole.
    s.clear();
    spectext::cell_view(0, cell_of(sc::Verdict::Fail, 255, -32766), s, shown);
    CHECK_COND(shown == "-3276.6ms, 255 fail");
    s.clear();
    spectext::cell_view(8, cell_of(sc::Verdict::Fail, 255, -32766), s, shown);
    CHECK_COND(shown == "-327.66V, 255 fail");
    s.clear();
    spectext::cell_view(8, cell_of(sc::Verdict::Fail, 255, specsec::kUnseen), s, shown);
    CHECK_COND(shown == "not seen, 255 fail");
}

static void test_detail_rows() {
    StrBuf<20> s;
    std::uint16_t col = 0;
    const specsec::Cell pass = cell_of(sc::Verdict::Pass, 0, 123);
    spectext::detail_value(0, 0, pass, nullptr, s, col);
    CHECK_COND(s.view() == "Pass");
    s.clear();
    spectext::detail_value(1, 0, pass, nullptr, s, col);
    CHECK_COND(s.view() == "12.3ms");
    // Without the engine the limit, the count and the place read dim "-".
    for (int row : {2, 3, 5}) {
        s.clear();
        spectext::detail_value(row, 0, pass, nullptr, s, col);
        CHECK_COND(s.view() == "-");
    }
    s.clear();
    spectext::detail_value(4, 0, pass, nullptr, s, col);
    CHECK_COND(s.view() == "0");
    // The penalty is the check's weight times its fails.
    const specsec::Cell bad = cell_of(sc::Verdict::Fail, 2, 505);
    s.clear();
    spectext::detail_value(6, 8, bad, nullptr, s, col);
    CHECK_EQ(static_cast<unsigned>(s.view().empty() ? 0 : s.view()[0] - '0') > 0u, true);
    s.clear();
    spectext::detail_value(0, 8, bad, nullptr, s, col);
    CHECK_COND(s.view() == "Fail");
    // With the engine's aggregate they all read.
    sc::Agg a;
    a.n = 4; a.fails = 2; a.worst = 5050; a.lo = 4750; a.hi = 5250; a.a = 0x02; a.b = 3;
    s.clear();
    spectext::detail_value(2, 8, bad, &a, s, col);
    CHECK_COND(s.view() == "4.75V-5.25V");
    s.clear();
    spectext::detail_value(3, 8, bad, &a, s, col);
    CHECK_COND(s.view() == "4");
    // Skip: the reason in the Worst row, no limit.
    const specsec::Cell skip = cell_of(sc::Verdict::Skip, 0, 0, sc::Skip::NoPps);
    s.clear();
    spectext::detail_value(1, 9, skip, nullptr, s, col);
    CHECK_COND(s.view() == sc::skip_name(sc::Skip::NoPps));
    s.clear();
    spectext::detail_value(0, 9, skip, nullptr, s, col);
    CHECK_COND(s.view() == "Skip");
    // An inferred limit is reported, never judged: Unverified.
    s.clear();
    spectext::detail_value(0, static_cast<int>(sc::Check::PpsSettle), cell_of(sc::Verdict::Unverified, 0, 40), nullptr, s, col);
    CHECK_COND(s.view() == "Unverified");
}

static void test_from_aggs_and_wire() {
    std::array<sc::Agg, sc::kChecks> agg{};
    sc::Agg& acc = agg[static_cast<std::size_t>(sc::Check::AcceptTime)];
    acc.n = 2; acc.worst = 12340; acc.hi = 25000;
    sc::Agg& lvl = agg[static_cast<std::size_t>(sc::Check::Accuracy)];
    lvl.n = 3; lvl.fails = 2; lvl.worst = -5049; lvl.lo = 4750; lvl.hi = 5250;
    sc::Agg& set = agg[static_cast<std::size_t>(sc::Check::PpsSettle)];   // an inferred limit: measured, never judged
    set.n = 1; set.worst = 40;
    sc::Agg& unseen = agg[static_cast<std::size_t>(sc::Check::PpsMonotonic)];
    unseen.n = 1; unseen.worst = static_cast<std::int32_t>(sc::kNever);
    agg[static_cast<std::size_t>(sc::Check::AvsBand)].skip = sc::Skip::NoAvs;   // n == 0: not run
    const specsec::Result r = specsec::from_aggs(agg, specsec::End::Done, 0, true);
    auto cell = [&](sc::Check c) -> const specsec::Cell& {
        for (std::size_t i = 0; i < specsec::kCells; ++i) if (specsec::kOrder[i] == c) return r.cells[i];
        static const specsec::Cell none{};
        return none;
    };
    CHECK_EQ(int(cell(sc::Check::AcceptTime).verdict()), int(sc::Verdict::Pass));
    CHECK_EQ(int(cell(sc::Check::AcceptTime).value), 123);
    CHECK_EQ(int(cell(sc::Check::Accuracy).verdict()), int(sc::Verdict::Fail));
    CHECK_EQ(int(cell(sc::Check::Accuracy).fails), 2);
    CHECK_EQ(int(cell(sc::Check::Accuracy).value), -505);
    CHECK_EQ(int(cell(sc::Check::PpsSettle).verdict()), int(sc::Verdict::Unverified));
    CHECK_EQ(int(cell(sc::Check::PpsMonotonic).value), int(specsec::kUnseen));
    CHECK_EQ(int(cell(sc::Check::AvsBand).verdict()), int(sc::Verdict::Skip));
    CHECK_EQ(int(cell(sc::Check::AvsBand).skip()), int(sc::Skip::NoAvs));
    CHECK_EQ(int(cell(sc::Check::AvsBand).value), 0);
    CHECK_COND(r.unconfirmed);
    CHECK_COND(r.shown());

    std::array<std::uint8_t, specsec::kWireBytes> w{};
    specsec::to_wire(r, w);
    specsec::Result back;
    CHECK_COND(specsec::from_wire(w.data(), w.size(), back));
    CHECK_COND(back.same(r));
    CHECK_EQ(int(back.cells[0].value), int(r.cells[0].value));
    CHECK_EQ(w.size(), static_cast<std::size_t>(3 + 4 * specsec::kCells));
    // A different value is a different result (same() reads the number too).
    specsec::Result other = r;
    other.cells[0].value = static_cast<std::int16_t>(other.cells[0].value + 1);
    CHECK_COND(!other.same(r));
    // Negative values and the sentinel survive the little-endian halves.
    specsec::Result ext;
    ext.end = specsec::End::Done;
    ext.cells[0] = specsec::make_cell(sc::Verdict::Fail, sc::Skip::None, 300, -32766);
    ext.cells[1] = specsec::make_cell(sc::Verdict::Pass, sc::Skip::None, 0, specsec::kUnseen);
    specsec::to_wire(ext, w);
    CHECK_COND(specsec::from_wire(w.data(), w.size(), back));
    CHECK_EQ(int(back.cells[0].value), -32766);
    CHECK_EQ(int(back.cells[0].fails), 255);   // saturated
    CHECK_EQ(int(back.cells[1].value), int(specsec::kUnseen));
    // Damaged or foreign records are refused and leave the result alone.
    specsec::Result keep = r;
    CHECK_COND(!specsec::from_wire(w.data(), w.size() - 1, keep));
    CHECK_COND(keep.same(r));
    w[0] = specsec::kEndMax + 1;
    CHECK_COND(!specsec::from_wire(w.data(), w.size(), keep));
    w[0] = 1; w[3] = 7;   // verdict code out of range
    CHECK_COND(!specsec::from_wire(w.data(), w.size(), keep));
    // The old two-bytes-a-cell record is not one of this version.
    CHECK_COND(!specsec::from_wire(w.data(), 3 + 2 * specsec::kCells, keep));
}

int main() {
    test_from_aggs_and_wire();
    test_quantize();
    test_value_text();
    test_limit_text();
    test_where_text();
    test_cell_text();
    test_detail_rows();
    return test_report("test_spec_text");
}
