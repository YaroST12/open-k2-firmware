// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test step labels (k2/chargertest/step_label.hpp): one wording for the Running / Steps rows, the Detail heading, the Results chips
// and summary, and the report card text. The golden vectors in tests/fixtures/step_labels.txt are shared with the Python mirror
// (tools/step_label.py, tests/test_step_label.py).
#include "test_util.h"
#include "k2/chargertest/step_label.hpp"
#include "k2/pd/pd.hpp"
#include "k2/ui/kit.hpp"
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace k2;
namespace ct = k2::charger_test;
namespace sl = k2::step_label;
using A = ct::Algorithm;

static std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}
static std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    size_t from = 0;
    for (;;) {
        const size_t bar = line.find('|', from);
        out.push_back(trim(line.substr(from, bar == std::string::npos ? std::string::npos : bar - from)));
        if (bar == std::string::npos) return out;
        from = bar + 1;
    }
}
static A algorithm(const std::string& n) {
    static const std::map<std::string, A> names = {
        {"pd2", A::Pd2Fixed}, {"pd3", A::Pd3Fixed}, {"epr", A::Epr}, {"spr-avs", A::SprAvs}, {"pps", A::Pps},
        {"epr-avs", A::EprAvs}, {"pd30rev", A::Pd30Revision}, {"pd31rev", A::Pd31Revision}, {"pd32rev", A::Pd32Revision},
        {"qc2", A::Qc2}, {"qc3", A::Qc3}, {"qc3+", A::Qc3Plus}, {"afc", A::Afc}, {"fcp", A::Fcp}, {"scp", A::Scp},
        {"vooc", A::Vooc}, {"svooc", A::SuperVooc}, {"svooc65", A::SuperVooc65}, {"ufcs", A::Ufcs}, {"vivo", A::Vivo},
        {"qc2b", A::Qc2ClassB}, {"qc3b", A::Qc3ClassB}};
    const auto it = names.find(n);
    if (it == names.end()) { std::fprintf(stderr, "unknown algorithm %s\n", n.c_str()); std::exit(2); }
    return it->second;
}
struct Vector { ct::Step step, prev; bool has_prev = false; std::string label; };
static std::vector<Vector> vectors() {
    std::vector<Vector> out;
    std::ifstream f("tests/fixtures/step_labels.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto c = split(line);
        if (c.size() != 8) { std::fprintf(stderr, "bad vector: %s\n", line.c_str()); std::exit(2); }
        Vector v;
        ct::Step& s = v.step;
        s.algorithm = algorithm(c[0]); s.group = ct::group(s.algorithm);
        s.pos = uint8_t(std::stoul(c[1])); s.mv = uint16_t(std::stoul(c[2])); s.ma = uint16_t(std::stoul(c[3]));
        s.pdo = uint32_t(std::stoul(c[4], nullptr, 16));
        s.hunt = c[5].find('h') != std::string::npos; s.avs = c[5].find('a') != std::string::npos;
        s.continuous = c[5].find('c') != std::string::npos;
        if (const unsigned pmv = unsigned(std::stoul(c[6]))) {
            v.has_prev = true; v.prev = s; v.prev.mv = uint16_t(pmv); v.prev.continuous = false;
        }
        v.label = c[7];
        out.push_back(v);
    }
    return out;
}
// Glyph cells of a UTF-8 string (the panel fonts are monospace: one cell per code point).
static unsigned cells(const std::string& s) {
    unsigned n = 0;
    for (const unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

static void test_the_golden_labels() {
    const auto all = vectors();
    CHECK_EQ(all.size() > 40, true);
    for (const auto& v : all) {
        const auto got = sl::label(v.step, v.has_prev ? &v.prev : nullptr);
        if (v.label != got.c_str()) std::fprintf(stderr, "label: want '%s' got '%s'\n", v.label.c_str(), got.c_str());
        CHECK_STR(got.c_str(), v.label.c_str());
        // The Results chip is the same words without the protocol: protocol + ' ' + params is the label.
        const auto proto = sl::protocol(v.step);
        const auto par = sl::params(v.step, v.has_prev ? &v.prev : nullptr);
        const std::string joined = par.len ? std::string(proto.c_str()) + " " + par.c_str() : proto.c_str();
        CHECK_STR(joined.c_str(), v.label.c_str());
    }
}
static void test_every_algorithm_has_a_label_without_the_internal_name() {
    for (unsigned a = 0; a < unsigned(A::Count); ++a) {
        ct::Step s; s.algorithm = A(a); s.group = ct::group(A(a)); s.mv = 9000;
        const auto t = sl::label(s);
        CHECK_EQ(t.len > 1, true);
        CHECK_EQ(std::string(t.c_str()).find('?') == std::string::npos, true);
        CHECK_EQ(std::string(t.c_str()).find('#') == std::string::npos, true);   // the PDO position lives in the detail page
    }
}
static void test_amps_only_when_the_step_has_a_current() {
    ct::Step qc; qc.algorithm = A::Qc3; qc.group = ct::Group::Qc3; qc.mv = 12000; qc.ma = 0;
    CHECK_EQ(sl::amps_ma(qc), 0u);
    qc.ma = 1000;   // a stray request current on a D+/D- row is never shown: only SCP carries a limit
    CHECK_EQ(sl::amps_ma(qc), 0u);
    ct::Step scp; scp.algorithm = A::Scp; scp.group = ct::Group::Scp; scp.mv = 10000; scp.ma = 2250;
    CHECK_EQ(sl::amps_ma(scp), 2250u);
    ct::Step fixed; fixed.algorithm = A::Pd3Fixed; fixed.mv = 20000; fixed.ma = 1000; fixed.pdo = 409935u;   // 20 V 3.35 A offer
    CHECK_EQ(sl::amps_ma(fixed), 3350u);   // the offer, not the 1 A the row asks for
    fixed.pdo = 0;
    CHECK_EQ(sl::amps_ma(fixed), 0u);      // no offer in the evidence: nothing to show, nothing invented
    ct::Step avs; avs.algorithm = A::SprAvs; avs.avs = true; avs.mv = 15000; avs.ma = 1000; avs.pdo = 0xE004B000u;
    CHECK_EQ(sl::amps_ma(avs), 0u);
    ct::Step hunt; hunt.algorithm = A::Pd3Fixed; hunt.hunt = true; hunt.mv = 20000; hunt.ma = 6000; hunt.pdo = 409935u;
    CHECK_EQ(sl::amps_ma(hunt), 6000u);
}
static void test_a_ramp_needs_the_previous_row_of_the_same_offer() {
    ct::Step prev; prev.algorithm = A::Pps; prev.pos = 6; prev.mv = 11000; prev.pdo = 0xC1A42164u;
    ct::Step s = prev; s.mv = 8000; s.continuous = true;
    CHECK_STR(sl::label(s, &prev).c_str(), "PPS 11→8V");
    CHECK_STR(sl::label(s, nullptr).c_str(), "PPS 8V 5A");           // no previous row known
    ct::Step other = prev; other.pos = 7;
    CHECK_STR(sl::label(s, &other).c_str(), "PPS 8V 5A");            // another APDO's row is not where this one ramps from
    ct::Step hunt = prev; hunt.hunt = true;
    CHECK_STR(sl::label(s, &hunt).c_str(), "PPS 8V 5A");             // nor is a 6 A probe
    s.continuous = false;
    CHECK_STR(sl::label(s, &prev).c_str(), "PPS 8V 5A");             // a row that starts its own sequence
    ct::Step none = prev; none.mv = 0; s.continuous = true;
    CHECK_STR(sl::label(s, &none).c_str(), "PPS 8V 5A");
}
static void test_the_range_chip() {
    CHECK_STR(sl::range(0xC1A42164u).c_str(), "3.3-21V 5A");
    CHECK_STR(sl::range(0xC0DC213Cu).c_str(), "3.3-11V 3A");
    CHECK_STR(sl::range(0xD230968Cu).c_str(), "15-28V 5A");   // an EPR AVS offer is a power: 140 W at 28 V is 5 A
    CHECK_STR(sl::range(0xE004B000u).c_str(), "9-15V 3A");      // PD 3.2 SPR AVS: 9 V up, the 15 V limit's current in 19:10
    CHECK_STR(sl::range(0).c_str(), "");
    // From numbers (the report card keeps 100 mV units and a current).
    CHECK_STR(sl::range(3300, 11000, 3000).c_str(), "3.3-11V 3A");
    CHECK_STR(sl::range(15000, 28000, 0).c_str(), "15-28V");
    CHECK_STR(sl::range(9000, 20000, 0, 60000).c_str(), "9-20V 60W");
}
// A UFCS step planned from the charger's Output_Capabilities carries its mode in `pdo` (charger_test.hpp pack_ufcs_mode): the
// chip adds the mode's top current, the Detail page the whole mode. The label words themselves never change (the golden vectors).
static ct::Step ufcs_step(unsigned mv, unsigned lo_mv, unsigned hi_mv, unsigned lo_ma, unsigned hi_ma) {
    ct::Step s; s.algorithm = A::Ufcs; s.group = ct::group(A::Ufcs); s.mv = uint16_t(mv);
    s.pdo = ct::pack_ufcs_mode(int(lo_mv), int(hi_mv), int(lo_ma), int(hi_ma));
    return s;
}
static void test_the_ufcs_mode_words() {
    const ct::Step s = ufcs_step(9000, 5000, 20000, 500, 3000);
    CHECK_EQ(sl::ufcs_amps_ma(s), 3000u);
    CHECK_STR(sl::ufcs_range(s).c_str(), "5-20V 0.5-3A");
    CHECK_STR(sl::label(s).c_str(), "UFCS 9V");   // the Steps / Running words stay volts only
    ct::UfcsMode m; CHECK_EQ(ct::ufcs_mode_of(s, m), true);
    CHECK_EQ(m.min_mv, 5000u); CHECK_EQ(m.max_mv, 20000u); CHECK_EQ(m.min_ma, 500u); CHECK_EQ(m.max_ma, 3000u);
    // A mode with one current reads it once; 100 mV and 50 mA are the stored resolution.
    CHECK_STR(sl::ufcs_range(ufcs_step(5000, 3400, 11000, 3000, 3000)).c_str(), "3.4-11V 3A");
    CHECK_STR(sl::ufcs_range(ufcs_step(5000, 5500, 11000, 1000, 4000)).c_str(), "5.5-11V 1-4A");
    CHECK_STR(sl::ufcs_range(ufcs_step(5000, 5000, 21000, 1000, 3250)).c_str(), "5-21V 1-3.25A");
    CHECK_EQ(sl::ufcs_amps_ma(ufcs_step(5000, 3400, 11000, 1000, 3250)), 3250u);
    // No mode (a plan row made before the charger's list was read, or any other protocol): nothing.
    ct::Step plain; plain.algorithm = A::Ufcs; plain.group = ct::group(A::Ufcs); plain.mv = 9000;
    CHECK_EQ(ct::ufcs_mode_of(plain, m), false);
    CHECK_EQ(sl::ufcs_amps_ma(plain), 0u);
    CHECK_STR(sl::ufcs_range(plain).c_str(), "");
    ct::Step pd; pd.algorithm = A::Pd3Fixed; pd.group = ct::group(A::Pd3Fixed); pd.pdo = 0x0001912Cu;
    CHECK_EQ(sl::ufcs_amps_ma(pd), 0u);
    CHECK_STR(sl::ufcs_range(pd).c_str(), "");
}
// The Detail page's value column is 16 cells of the 16 px font: every mode a charger can report reads within it.
static void test_every_ufcs_mode_range_fits_the_detail_value() {
    unsigned longest = 0;
    for (unsigned lo_mv = 3000; lo_mv <= 21000; lo_mv += 100)
        for (unsigned hi_mv = lo_mv; hi_mv <= 36000; hi_mv += 100)
            for (const unsigned lo_ma : {0u, 500u, 1000u, 1250u, 2250u, 3250u})
                for (const unsigned hi_ma : {500u, 1000u, 2250u, 3000u, 3250u, 4450u, 5000u, 6350u}) {
                    if (hi_ma < lo_ma) continue;
                    const auto t = sl::ufcs_range(ufcs_step(5000, lo_mv, hi_mv, lo_ma, hi_ma));
                    longest = std::max<unsigned>(longest, unsigned(t.len));
                }
    CHECK_EQ(longest <= 16u, true);
}
// The Steps list is a kit::RowList of 16 px rows (8 px cells): the label starts at kTextX and ends a gap before the measured-voltage
// column, which is right-aligned to kTailX and at most "36.0V" (5 cells, the monitor's ceiling) wide. kRowCells is that room.
static constexpr int kTailCells = 5;
static constexpr int kRoomCells = (kit::kTailX - kit::kTailGap - kTailCells * int(kSmallAdvPx) - kit::kTextX) / int(kSmallAdvPx);
static_assert(kRoomCells == 17, "the Steps row's text room changed: re-check step_label.hpp kRowCells and the labels");

// Every label a plan can produce fits the row, swept over every algorithm, flag, current and voltage a step can carry (10 mV
// steps cover every digit pattern the label prints) and over every ramp pair of the programmable offers.
static void test_the_longest_labels_fit_the_row() {
    CHECK_EQ(int(sl::kRowCells), kRoomCells);   // the documented room is the real one
    unsigned longest = 0;
    std::string worst;
    const auto note = [&](const sl::Text& t) {
        CHECK_EQ(t.len < sl::kMax, true);   // the byte buffer holds the arrow's three bytes
        const unsigned n = cells(t.c_str());
        if (n > longest) { longest = n; worst = t.c_str(); }
    };
    for (unsigned a = 0; a < unsigned(A::Count); ++a)
        for (const bool hunt : {false, true})
            for (const bool avs : {false, true})
                for (const unsigned ma : {0u, 1000u, 2250u, 3000u, 5000u, 6000u})
                    for (const uint32_t pdo : {0u, 0x0001912Cu, 0xC1A42164u, 0xD230968Cu})
                        for (unsigned mv = 0; mv <= 50000; mv += 10) {
                            ct::Step s;
                            s.algorithm = A(a); s.group = ct::group(A(a)); s.hunt = hunt; s.avs = avs;
                            s.ma = uint16_t(ma); s.pdo = pdo; s.mv = uint16_t(mv);
                            note(sl::label(s));
                        }
    for (const A a : {A::Pps, A::SprAvs, A::EprAvs, A::Epr}) {
        ct::Step prev; prev.algorithm = a; prev.avs = a == A::Epr; prev.pos = 1; prev.pdo = 0xC1A42164u;
        ct::Step s = prev; s.continuous = true;
        for (unsigned from = 3000; from <= 48000; from += 70)
            for (unsigned to = 3000; to <= 48000; to += 70) { prev.mv = uint16_t(from); s.mv = uint16_t(to); note(sl::label(s, &prev)); }
    }
    for (const auto& v : vectors()) note(sl::label(v.step, v.has_prev ? &v.prev : nullptr));
    if (longest > unsigned(kRoomCells)) printf("longest step label, %u cells: %s\n", longest, worst.c_str());
    CHECK_EQ(longest <= unsigned(kRoomCells), true);
}

int main() {
    test_the_golden_labels();
    test_every_algorithm_has_a_label_without_the_internal_name();
    test_amps_only_when_the_step_has_a_current();
    test_a_ramp_needs_the_previous_row_of_the_same_offer();
    test_the_range_chip();
    test_the_ufcs_mode_words();
    test_every_ufcs_mode_range_fits_the_detail_value();
    test_the_longest_labels_fit_the_row();
    return test_report("test_step_label");
}
