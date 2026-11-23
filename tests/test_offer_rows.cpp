// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Charger tab's offer rows (SPR / EPR / PPS / AVS) and its headline watts: pure, over PDO values.
#include <algorithm>
#include <string>
#include <vector>
#include "test_util.h"
#include "k2/ui/offer_rows.hpp"

using namespace k2;
#define CHECK(c) CHECK_EQ(int(!!(c)), 1)

static uint32_t fixed(uint32_t mv, uint32_t ma) { return (mv / 50) << 10 | ma / 10; }
static uint32_t pps(uint32_t min_mv, uint32_t max_mv, uint32_t ma) {
    return 3u << 30 | (max_mv / 100) << 17 | (min_mv / 100) << 8 | ma / 50;
}
static uint32_t avs_epr(uint32_t min_mv, uint32_t max_mv, uint32_t w) {
    return 3u << 30 | 1u << 28 | (max_mv / 100) << 17 | (min_mv / 100) << 8 | w;
}
static uint32_t avs_spr15(uint32_t ma) { return 3u << 30 | 2u << 28 | (ma / 10) << 10; }

// Source caps from raw words, by position (a 0 word is an unused slot).
static pd::SourceCaps caps_of(std::initializer_list<uint32_t> raws, bool epr = false) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    c.epr = epr;
    std::size_t i = 0;
    for (uint32_t w : raws) c.pdos[i++] = pd::decode_pdo(w);
    c.num_pdos = uint8_t(i);
    return c;
}

// "SPR 5/9V 3A" per row; a dim row gets a leading '~'; the cut and the tail kind are checked apart.
struct Got {
    std::vector<std::string> row;
    std::vector<int> cut;
    std::vector<bool> watts, dim;
    std::size_t widest = 0;
};
static Got rows_of(const pd::SourceCaps &c, std::span<const uint32_t> epr, bool capable, const char *word = "") {
    Got g;
    offers::rows(c, c.num_pdos, epr, capable, word, 19, [&](const offers::Line &l) {
        g.row.push_back(std::string(l.dim ? "~" : "") + l.key + " " + std::string(l.text));
        g.cut.push_back(l.cut);
        g.watts.push_back(l.tail_watts);
        g.dim.push_back(l.dim);
        g.widest = std::max(g.widest, l.text.size());
    });
    return g;
}

static void test_hero_is_pdp_else_best_offer() {
    CHECK_EQ(offers::hero_w(true, 100, 0, 63000), 100);   // the 100 W bank: PDP over its best offer (21 V x 3 A)
    CHECK_EQ(offers::hero_w(true, 100, 140, 100000), 140);   // an EPR charger: the EPR rating
    CHECK_EQ(offers::hero_w(true, 0, 140, 100000), 140);
    CHECK_EQ(offers::hero_w(false, 100, 140, 63000), 63);   // no extended caps read: the best offer
    CHECK_EQ(offers::hero_w(true, 0, 0, 63000), 63);   // a zero rating is no rating
    CHECK_EQ(offers::hero_w(false, 0, 0, 0), 0);
}

static void test_bank_100w() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000),
                                      fixed(20000, 3000), pps(5000, 21000, 3000)});
    const Got g = rows_of(c, {}, false);
    CHECK_EQ(g.row.size(), 4);
    CHECK_STR(g.row[0].c_str(), "SPR 5/9/12/15/20V 3A");
    CHECK_STR(g.row[1].c_str(), "~EPR none");
    CHECK_STR(g.row[2].c_str(), "PPS 5-21V 3A");
    CHECK_STR(g.row[3].c_str(), "~AVS none");
    CHECK_EQ(g.cut[0], 13);   // after "5/9/12/15/20V": the current is the tail
    CHECK_EQ(g.cut[2], 5);
    CHECK_EQ(g.cut[1], 0);
}

static void test_epr_140w_from_the_epr_list() {
    // SPR offers in the live caps; the EPR list (positions 8 on) read by the survey.
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(15000, 3000), fixed(20000, 5000),
                                      pps(3300, 21000, 5000)});
    const uint32_t epr[] = {0, 0, 0, 0, 0, 0, 0, fixed(28000, 5000), avs_epr(15000, 28000, 140)};
    const Got g = rows_of(c, epr, true);
    CHECK_EQ(g.row.size(), 5);
    CHECK_STR(g.row[0].c_str(), "SPR 5/9/15V 3A");
    CHECK_STR(g.row[1].c_str(), "SPR 20V 5A");
    CHECK_STR(g.row[2].c_str(), "EPR 28V 5A");
    CHECK_STR(g.row[3].c_str(), "PPS 3.3-21V 5A");
    CHECK_STR(g.row[4].c_str(), "AVS 15-28V 140W");
    CHECK_EQ(g.watts[4], 1);   // the AVS tail is watts, the others amps
    CHECK_EQ(g.watts[3], 0);
    CHECK_EQ(g.cut[4], 6);
}

static void test_epr_140w_from_the_live_caps() {
    // EPR mode entered: the live caps carry the EPR positions themselves, the SPR slots in between zero.
    pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(15000, 3000), fixed(20000, 5000), 0, 0, 0,
                                fixed(28000, 5000), fixed(36000, 5000), fixed(48000, 5000), avs_epr(15000, 48000, 240)},
                               true);
    const Got g = rows_of(c, {}, true);
    CHECK_EQ(g.row.size(), 5);
    CHECK_STR(g.row[2].c_str(), "EPR 28/36/48V 5A");
    CHECK_STR(g.row[3].c_str(), "~PPS none");
    CHECK_STR(g.row[4].c_str(), "AVS 15-48V 240W");
}

static void test_spr_avs_and_epr_avs_share_the_avs_rows() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(9000, 3000), avs_spr15(3000)});
    const uint32_t epr[] = {0, 0, 0, 0, 0, 0, 0, fixed(28000, 5000), avs_epr(15000, 28000, 140)};
    const Got g = rows_of(c, epr, true);
    CHECK_EQ(g.row.size(), 5);
    CHECK_STR(g.row[3].c_str(), "AVS 9-15V 45W");
    CHECK_STR(g.row[4].c_str(), "AVS 15-28V 140W");
}

static void test_every_pps_is_a_row() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), pps(3300, 11000, 5000), pps(3300, 21000, 3000)});
    const Got g = rows_of(c, {}, false);
    CHECK_EQ(g.row.size(), 5);
    CHECK_STR(g.row[2].c_str(), "PPS 3.3-11V 5A");
    CHECK_STR(g.row[3].c_str(), "PPS 3.3-21V 3A");
}

static void test_nothing_but_5v() {
    const pd::SourceCaps c = caps_of({fixed(5000, 900)});
    const Got g = rows_of(c, {}, false);
    CHECK_EQ(g.row.size(), 4);
    CHECK_STR(g.row[0].c_str(), "SPR 5V 0.9A");
    CHECK_STR(g.row[1].c_str(), "~EPR none");
    CHECK_STR(g.row[2].c_str(), "~PPS none");
    CHECK_STR(g.row[3].c_str(), "~AVS none");
}

static void test_epr_capable_but_not_read() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(20000, 5000)});
    CHECK_STR(rows_of(c, {}, true, "Not asked").row[2].c_str(), "~EPR Not asked");
    CHECK_STR(rows_of(c, {}, true, "Asking...").row[2].c_str(), "~EPR Asking...");
    CHECK_STR(rows_of(c, {}, true, "No answer").row[2].c_str(), "~EPR No answer");
    CHECK_STR(rows_of(c, {}, true, "").row[2].c_str(), "~EPR none");   // read, nothing fixed in it
    CHECK_STR(rows_of(c, {}, false, "No EPR bit").row[2].c_str(), "~EPR none");   // not capable: none, whatever the survey says
}

static void test_mixed_currents_and_long_runs() {
    // Current changes split the run; so does the 19-cell value column ("5/9/12/15/20/28V 3A" is 19, one more voltage is not).
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000),
                                      fixed(20000, 5000)});
    const Got g = rows_of(c, {}, false);
    CHECK_STR(g.row[0].c_str(), "SPR 5/9/12/15V 3A");
    CHECK_STR(g.row[1].c_str(), "SPR 20V 5A");
    const uint32_t epr[] = {0, 0, 0, 0, 0, 0, 0, fixed(28000, 5000), fixed(36000, 5000), fixed(40000, 5000), fixed(48000, 5000)};
    const pd::SourceCaps c2 = caps_of({fixed(5000, 3000)});
    const Got g2 = rows_of(c2, epr, true);
    CHECK_STR(g2.row[1].c_str(), "EPR 28/36/40/48V 5A");
    CHECK(g2.widest <= 19);
}

static void test_every_line_fits_the_value_column() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(9000, 2000), fixed(12000, 1500), fixed(15000, 1000),
                                      fixed(20000, 500), pps(3300, 21000, 3250)});
    const uint32_t epr[] = {0, 0, 0, 0, 0, 0, 0, fixed(28000, 5000), fixed(36000, 4000), fixed(48000, 3000), avs_epr(15000, 48000, 240)};
    const Got g = rows_of(c, epr, true);
    CHECK(g.widest <= 19);
    CHECK(g.row.size() >= 8);
}

static void test_position_decides_spr_or_epr() {
    CHECK_EQ(int(offers::kind_of(pd::decode_pdo(fixed(20000, 5000)), 3)), int(offers::Kind::Spr));
    CHECK_EQ(int(offers::kind_of(pd::decode_pdo(fixed(28000, 5000)), 7)), int(offers::Kind::Epr));
    CHECK_EQ(int(offers::kind_of(pd::decode_pdo(pps(3300, 21000, 3000)), 2)), int(offers::Kind::Pps));
    CHECK_EQ(int(offers::kind_of(pd::decode_pdo(avs_epr(15000, 28000, 140)), 8)), int(offers::Kind::Avs));
    CHECK_EQ(int(offers::kind_of(pd::decode_pdo(0), 4)), int(offers::Kind::None));
}

static void test_summary_flags_for_the_chips() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), pps(3300, 21000, 3000)});
    const uint32_t epr[] = {0, 0, 0, 0, 0, 0, 0, fixed(28000, 5000), avs_epr(15000, 28000, 140)};
    const offers::Summary s = offers::summarize(c, c.num_pdos, epr);
    CHECK_EQ(s.pps, 1);
    CHECK_EQ(s.avs, 1);   // the AVS sits in the EPR list only
    CHECK_EQ(s.epr, 1);
    const offers::Summary none = offers::summarize(caps_of({fixed(5000, 3000)}), 1, {});
    CHECK_EQ(none.pps, 0);
    CHECK_EQ(none.avs, 0);
    CHECK_EQ(none.epr, 0);
}

static void test_epr_capable_bit() {
    pd::SourceCaps c = caps_of({fixed(5000, 3000) | 1u << 23, fixed(20000, 5000)});
    CHECK_EQ(offers::epr_capable(c), 1);   // PDO1 bit 23 on a PD 3.x source
    c.spec_rev = 1;
    CHECK_EQ(offers::epr_capable(c), 0);   // reserved in PD 2.0
    CHECK_EQ(offers::epr_capable(caps_of({fixed(5000, 3000), fixed(20000, 5000)})), 0);
    CHECK_EQ(offers::epr_capable(caps_of({fixed(5000, 3000)}, true)), 1);   // built from EPR chunks
}

int main() {
    test_hero_is_pdp_else_best_offer();
    test_bank_100w();
    test_epr_140w_from_the_epr_list();
    test_epr_140w_from_the_live_caps();
    test_spr_avs_and_epr_avs_share_the_avs_rows();
    test_every_pps_is_a_row();
    test_nothing_but_5v();
    test_epr_capable_but_not_read();
    test_mixed_currents_and_long_runs();
    test_every_line_fits_the_value_column();
    test_position_decides_spr_or_epr();
    test_summary_flags_for_the_chips();
    test_epr_capable_bit();
    return test_report("test_offer_rows");
}
