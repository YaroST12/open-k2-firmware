// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the device twin's one-line texts (k2/twin/present.hpp): the outcome in the Home protocol-row style,
// the limiter note, the live verdict line. The screen and the shell print exactly these strings.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "k2/twin/present.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::twin;

namespace {

// The texts are terminated, so the buffer itself is the string; the static buffer outlives the check macro.
const char* sum(const Outcome& o) {
    static std::array<char, 24> b;
    present::summary(o, b);
    return b.data();
}

Outcome pps(std::uint16_t mv, std::uint16_t ma, std::uint32_t want_w, Limiter l) {
    Outcome o;
    o.kind = Kind::Pps;
    o.pos = 6;
    o.mv = mv;
    o.ma = ma;
    o.mw = std::uint32_t(mv) * ma / 1000u;
    o.want_mw = want_w * 1000u;
    o.limiter = l;
    return o;
}

void test_summary() {
    CHECK_STR(sum(pps(9000, 3000, 27, Limiter::None)), "27W PPS");
    // Not a whole number of watts: rounded to the nearest.
    CHECK_STR(sum(pps(11000, 3000, 33, Limiter::Device)), "33W PPS");
    CHECK_STR(sum(pps(5000, 3000, 33, Limiter::Charger)), "15W of 33W");
    Outcome f;
    f.kind = Kind::Fixed;
    f.pos = 3;
    f.mv = 12000;
    f.ma = 3000;
    f.mw = 36000;
    f.want_mw = 55000;
    f.limiter = Limiter::Cable;
    CHECK_STR(sum(f), "36W of 55W");
    f.want_mw = 36000;
    f.limiter = Limiter::None;
    CHECK_STR(sum(f), "36W 12V");
    Outcome v;
    v.kind = Kind::FiveV;
    v.mv = 5000;
    v.ma = 3000;
    v.mw = 15000;
    v.want_mw = 15000;
    CHECK_STR(sum(v), "5V only");
    CHECK_STR(sum(Outcome{}), "no contract");
}

void test_summary_fits() {
    // The widest outcome: three digits of watts on both sides.
    Outcome o = pps(21000, 5000, 240, Limiter::Charger);
    o.mw = 105000;
    o.want_mw = 240000;
    std::array<char, 24> b{};
    const int n = present::summary(o, b);
    CHECK_STR(b.data(), "105W of 240W");
    CHECK_COND(n <= 12);
    // A buffer that is too small is cut, never overrun, and stays terminated.
    std::array<char, 6> s{};
    const int m = present::summary(o, s);
    CHECK_COND(m < 6);
    CHECK_EQ(s[std::size_t(m)], '\0');
}

void test_note() {
    CHECK_STR(present::note(Limiter::None), "full");
    CHECK_STR(present::note(Limiter::Cable), "limited by the cable");
    CHECK_STR(present::note(Limiter::Charger), "limited by this charger");
    CHECK_STR(present::note(Limiter::Device), "the device takes no more");
}

void test_verdict_line() {
    using live::Verdict;
    std::array<char, 24> b{};
    Outcome o = pps(9000, 3000, 27, Limiter::None);
    present::proof(Verdict::Granted, 9012, o, b);
    CHECK_STR(b.data(), "27W PPS ok 9.0V");
    present::proof(Verdict::Refused, 0, o, b);
    CHECK_STR(b.data(), "refused");
    present::proof(Verdict::OutOfBand, 7400, o, b);
    CHECK_STR(b.data(), "out of band 7.4V");
    present::proof(Verdict::None, 0, o, b);
    CHECK_STR(b.data(), "not run");
}

}  // namespace

int main() {
    test_summary();
    test_summary_fits();
    test_note();
    test_verdict_line();
    return test_report("test_twin_present");
}
