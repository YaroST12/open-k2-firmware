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

// The Device twins page's footer states a fact in at most 27 cells; the count of "full" twins excludes a shortfall and a
// capability mismatch, and counts a device limit as full (the charger gave all it needs).
void test_is_full() {
    CHECK_EQ(present::is_full(pps(11000, 3000, 33, Limiter::None)), true);
    CHECK_EQ(present::is_full(pps(11000, 3000, 33, Limiter::Device)), true);
    CHECK_EQ(present::is_full(pps(5000, 3000, 33, Limiter::Charger)), false);
    CHECK_EQ(present::is_full(pps(5000, 3000, 33, Limiter::Cable)), false);
    Outcome mm = pps(5000, 3000, 15, Limiter::Device);
    mm.mismatch = true;
    CHECK_EQ(present::is_full(mm), false);
    CHECK_EQ(present::is_full(Outcome{}), false);
}

void test_footer() {
    std::array<char, 28> b{};
    present::footer(0, 0, true, b);
    CHECK_STR(b.data(), "No twin yet. Learn one");
    present::footer(3, 0, false, b);
    CHECK_STR(b.data(), "No charger: no predictions");
    present::footer(5, 3, true, b);
    CHECK_STR(b.data(), "3 of 5 full on charger");
    present::footer(1, 1, true, b);
    CHECK_STR(b.data(), "1 of 1 full on charger");
    // The widest (12 of 12) and every other pair stay within the 27-cell footer.
    int worst = 0;
    for (unsigned n = 0; n <= 12; n++)
        for (unsigned k = 0; k <= n; k++)
            for (int caps = 0; caps < 2; caps++) {
                const int len = present::footer(n, k, caps != 0, b);
                if (len > worst) worst = len;
                CHECK_EQ(int(std::string_view(b.data()).size()), len);
            }
    CHECK_EQ(worst <= 27, true);
}

}  // namespace

int main() {
    test_summary();
    test_summary_fits();
    test_note();
    test_verdict_line();
    test_is_full();
    test_footer();
    return test_report("test_twin_present");
}
