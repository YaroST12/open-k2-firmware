// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD spec check's per-transition rows (k2/chargertest/spec_rows.hpp) and their words (k2/ui/spec_text.hpp): the 8-byte row wire
// form, the file header and its refusals, the row texts and the width every text keeps inside its column.
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/chargertest/spec_rows.hpp"
#include "k2/ui/spec_text.hpp"
#include "test_util.h"

using namespace k2;
namespace sc = pdspec;
using specsec::Row;

namespace {

std::string_view name_of(const Row& r) {
    static StrBuf<32> s;
    s.clear();
    spectext::put_row_name(r, s);
    return s.view();
}
std::string_view value_of(const Row& r) {
    static StrBuf<32> s;
    s.clear();
    spectext::put_row_value(r, s);
    return s.view();
}
std::string_view tail_of(const Row& r) {
    static StrBuf<32> s;
    s.clear();
    spectext::put_row_tail(r, s);
    return s.view();
}

Row fixed_row(sc::Check c, unsigned from_mv, unsigned to_mv, sc::Unit u, int value, int margin, bool fail) {
    Row r;
    r.check = static_cast<std::uint8_t>(c);
    r.flags = fail ? specsec::kRowFail : 0;
    r.a = static_cast<std::uint16_t>(from_mv);
    r.b = static_cast<std::uint16_t>(to_mv);
    r.value = specsec::quantize(u, value);
    r.margin = specsec::quantize(u, margin);
    return r;
}

void wire_round_trip() {
    for (const Row r : {Row{9, specsec::kRowFail, 12000, 15000, 1610, 110}, Row{0, 0, 5000, 9000, -3, -32766},
                        Row{27, specsec::kRowProg | (2u << 2), 3300, 11000, specsec::kUnseen, 7}}) {
        std::array<std::uint8_t, specsec::kRowBytes> w{};
        specsec::row_encode(r, w.data());
        Row b;
        specsec::row_decode(w.data(), b);
        CHECK_EQ(int(b.check), int(r.check));
        CHECK_EQ(int(b.flags), int(r.flags));
        CHECK_EQ(int(b.a), int(r.a));
        CHECK_EQ(int(b.b), int(r.b));
        CHECK_EQ(int(b.value), int(r.value));
        CHECK_EQ(int(b.margin), int(r.margin));
    }
    // Little endian, fixed positions: tools and the web page read it by offset.
    std::array<std::uint8_t, specsec::kRowBytes> w{};
    specsec::row_encode(Row{7, 1, 0x1234, 0x5678, -2, 0x0102}, w.data());
    // check, flags, a, b, value, margin (all little endian)
    const std::array<std::uint8_t, 10> want{7, 1, 0x34, 0x12, 0x78, 0x56, 0xFE, 0xFF, 0x02, 0x01};
    for (std::size_t i = 0; i < want.size(); ++i) CHECK_EQ(int(w[i]), int(want[i]));
    CHECK_EQ(specsec::kRowBytes, std::size_t(10));
}

void header_round_trip_and_refusals() {
    std::array<std::uint16_t, specsec::kCells> counts{};
    counts[0] = 12;
    counts[8] = 12;
    counts[11] = 3;
    std::array<std::uint8_t, specsec::kCells> worst{};
    worst[0] = 5;
    worst[11] = 2;
    std::array<std::uint8_t, specsec::kRowsHdr> h{};
    specsec::rows_put_header(h.data(), 0xA1B2C3D4u, counts, worst);
    specsec::RowsIndex ix;
    const std::size_t size = specsec::kRowsHdr + 27 * specsec::kRowBytes;
    CHECK_COND(specsec::rows_parse(h.data(), size, 0xA1B2C3D4u, ix));
    CHECK_EQ(int(ix.count(0)), 12);
    CHECK_EQ(int(ix.count(1)), 0);
    CHECK_EQ(int(ix.first(8)), 12);
    CHECK_EQ(int(ix.first(11)), 24);
    CHECK_EQ(int(ix.count(11)), 3);
    CHECK_EQ(int(ix.total()), 27);
    CHECK_EQ(int(ix.worst(0)), 5);
    CHECK_EQ(int(ix.worst(11)), 2);
    CHECK_EQ(int(ix.offset(8, 2)), int(specsec::kRowsHdr + (12 + 2) * specsec::kRowBytes));
    // Another run, a short or long file, a wrong magic and a row total that does not add up are all refused: the page then falls back to
    // the summary.
    CHECK_COND(!specsec::rows_parse(h.data(), size, 0x11111111u, ix));
    CHECK_COND(!specsec::rows_parse(h.data(), size - 1, 0xA1B2C3D4u, ix));
    CHECK_COND(!specsec::rows_parse(h.data(), size + 10, 0xA1B2C3D4u, ix));
    CHECK_COND(!specsec::rows_parse(h.data(), specsec::kRowsHdr - 1, 0xA1B2C3D4u, ix));
    auto bad = h;
    bad[0] ^= 0xFF;
    CHECK_COND(!specsec::rows_parse(bad.data(), size, 0xA1B2C3D4u, ix));
    // Which cells list at all: the per-transition checks, not the single-measurement ones.
    CHECK_COND(specsec::has_list(0));                 // Accept
    CHECK_COND(specsec::has_list(8));                 // Level
    CHECK_COND(specsec::has_list(9));                 // PPS mono
    CHECK_COND(!specsec::has_list(16));               // PPS_Status: one answer per pass, a summary
    CHECK_COND(!specsec::has_list(18));               // PPS tracking
    CHECK_COND(!specsec::has_list(26));               // Source reset
}

void row_texts() {
    using sc::Check;
    // A fixed-level row: the pair in volts, the value in the check's unit, the margin (positive = outside the limit).
    Row r = fixed_row(Check::Overshoot, 5000, 9000, sc::Unit::Mv, 9210, -290, false);
    CHECK_COND(name_of(r) == "5\xE2\x86\x92" "9V");
    CHECK_COND(value_of(r) == "9.21V");
    CHECK_COND(tail_of(r) == "-0.29V");
    r = fixed_row(Check::Overshoot, 12000, 15000, sc::Unit::Mv, 16100, 1100, true);
    CHECK_COND(name_of(r) == "12\xE2\x86\x92" "15V");
    CHECK_COND(value_of(r) == "16.10V");
    CHECK_COND(tail_of(r) == "+1.10V");
    r = fixed_row(Check::FallTime, 20000, 5000, sc::Unit::Us, 1523000, 1200000, true);   // 1.5 s against 0.3 s
    CHECK_COND(value_of(r) == "1.5s");
    CHECK_COND(tail_of(r) == "+1.2s");
    r = fixed_row(Check::AcceptTime, 5000, 9000, sc::Unit::Us, 12300, -7700, false);
    CHECK_COND(value_of(r) == "12ms");
    CHECK_COND(tail_of(r) == "-7.7ms");
    r = fixed_row(Check::SrcStart, 5000, 9000, sc::Unit::Us, 4500, 1000, true);
    CHECK_COND(value_of(r) == "4.5ms");
    CHECK_COND(tail_of(r) == "+1.0ms");
    r = fixed_row(Check::RiseTime, 5000, 20000, sc::Unit::Us, 169000, -131000, false);
    CHECK_COND(value_of(r) == "169ms");
    CHECK_COND(tail_of(r) == "-131ms");
    // An event that never came: the word, and no margin.
    r = fixed_row(Check::PsRdyTime, 5000, 9000, sc::Unit::Us, sc::kNever, sc::kNever, true);
    CHECK_COND(value_of(r) == "never");
    CHECK_COND(tail_of(r).empty());
    // A fixed level with a fraction (a 9.5 V level) keeps one decimal in the pair.
    r = fixed_row(Check::Accuracy, 5000, 9500, sc::Unit::Mv, 9500, 0, false);
    CHECK_COND(name_of(r) == "5\xE2\x86\x92" "9.5V");
    // A programmable pass: the window and the step class; the value is the pass's worst, the tail its failing steps.
    Row p;
    p.check = static_cast<std::uint8_t>(Check::PpsBand);
    p.flags = specsec::kRowProg | (1u << 2);
    p.a = 3300; p.b = 11000; p.value = 123; p.margin = 0;
    CHECK_COND(name_of(p) == "3.3-11V 100mV");
    CHECK_COND(value_of(p) == "12ms");
    CHECK_COND(tail_of(p) == "ok");
    p.flags |= specsec::kRowFail; p.margin = 3;
    CHECK_COND(tail_of(p) == "3 bad");
    p.margin = 250;
    CHECK_COND(tail_of(p) == ">99");
    p.check = static_cast<std::uint8_t>(Check::AvsBand);
    p.flags = specsec::kRowProg | (2u << 2);
    p.a = 15000; p.b = 28000;
    CHECK_COND(name_of(p) == "15-28V 1V");
    p.flags = specsec::kRowProg | (3u << 2);
    CHECK_COND(name_of(p) == "15-28V slam");
    p.check = static_cast<std::uint8_t>(Check::PpsSettle);
    p.flags = specsec::kRowProg;
    CHECK_COND(name_of(p) == "15-28V 20mV");
    // Signed millivolts (accuracy, monotonic, overshoot): sign and unit, volts from 1 V.
    p.check = static_cast<std::uint8_t>(Check::PpsAccuracy);
    p.value = 4;      // +40 mV
    CHECK_COND(value_of(p) == "+40mV");
    p.value = -12;
    CHECK_COND(value_of(p) == "-120mV");
    p.value = 125;
    CHECK_COND(value_of(p) == "+1.25V");
    p.value = 1234;
    CHECK_COND(value_of(p) == "+12.3V");
    p.check = static_cast<std::uint8_t>(Check::PpsPsRdy);
    p.value = specsec::kUnseen;
    CHECK_COND(value_of(p) == "never");
}

// Every text stays inside its column for any value: the pair (x28..x104: 9 characters), the programmable name (to the tail: 16), the
// value and the tail (6 each). A cut would fail the truncation gate on the device, so the fuzz walks the whole range.
void widths_fuzz() {
    std::uint32_t seed = 12345;
    const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int i = 0; i < 20000; ++i) {
        Row r;
        r.check = static_cast<std::uint8_t>(next() % sc::kChecks);
        const bool prog = (next() & 1) != 0;
        r.flags = static_cast<std::uint8_t>((prog ? specsec::kRowProg : 0) | ((next() & 3) << 2) | (next() & 1));
        r.a = static_cast<std::uint16_t>(next());
        r.b = static_cast<std::uint16_t>(next());
        r.value = static_cast<std::int16_t>(next());
        r.margin = static_cast<std::int16_t>(next());
        if (i % 7 == 0) r.value = specsec::kUnseen;
        if (i % 11 == 0) r.margin = specsec::kUnseen;
        const int nm = text_cells(name_of(r));
        CHECK_COND(nm <= (prog ? 16 : 9));
        CHECK_COND(text_cells(value_of(r)) <= 6);
        CHECK_COND(text_cells(tail_of(r)) <= 6);
        if (nm > (prog ? 16 : 9)) { std::printf("  name '%.*s'\n", int(name_of(r).size()), name_of(r).data()); break; }
    }
}

}  // namespace

int main() {
    wire_round_trip();
    header_round_trip_and_refusals();
    row_texts();
    widths_fuzz();
    return test_report("test_spec_rows");
}
