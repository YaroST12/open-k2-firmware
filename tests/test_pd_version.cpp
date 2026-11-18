// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD version of a charger-test run (k2/chargertest/pd_version.hpp): the golden cases in tests/fixtures/pd_versions.txt (shared
// with the Python and JS mirrors), the three Revision-versus-offer cases, and the headline composer ("drop trailing items, never the
// version").
#include "test_util.h"
#include "k2/chargertest/pd_version.hpp"
#define CHECK(c) CHECK_COND(c)
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace pv = k2::pd_version;

static std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
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
static pv::Inputs inputs(const std::vector<std::string>& c) {
    pv::Inputs in;
    in.wire_rev = uint8_t(std::stoul(c[0]));
    if (c[1] != "-") { in.rmdo = true; in.rmdo_major = uint8_t(c[1][0] - '0'); in.rmdo_minor = uint8_t(c[1][2] - '0'); }
    in.pps = c[2].find('p') != std::string::npos;
    in.epr = c[2].find('e') != std::string::npos;
    in.spr_avs = c[2].find('s') != std::string::npos;
    return in;
}

static void test_the_golden_versions() {
    std::ifstream f("tests/fixtures/pd_versions.txt");
    std::string line;
    int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto c = split(line);
        if (c.size() != 5) { std::fprintf(stderr, "bad vector: %s\n", line.c_str()); std::exit(2); }
        const pv::Version v = pv::derive(inputs(c));
        const std::string got = v.valid() ? std::string(pv::label(v).view()) : "-";
        CHECK(got == c[3]);
        if (got != c[3]) std::fprintf(stderr, "  %s -> %s\n", line.c_str(), got.c_str());
        CHECK(v.mark == (c[4] != "-"));
        ++n;
    }
    CHECK(n >= 20);
}
// The user's three cases: an answer under the offer shows the offer's version marked, an equal answer and a higher one show the answer.
static void test_answer_against_the_offer() {
    pv::Inputs in; in.wire_rev = 2; in.spr_avs = true; in.rmdo = true; in.rmdo_major = 3;
    in.rmdo_minor = 1;
    pv::Version v = pv::derive(in);
    CHECK(pv::label(v).view() == "PD3.2*");
    CHECK(v.mark && v.basis == pv::Basis::SprAvs && v.ans_major == 3 && v.ans_minor == 1);
    CHECK(pv::conflict(v).view() == "3.1 < AVS 3.2");
    in.rmdo_minor = 2;   // equal: the answer, no mark
    v = pv::derive(in);
    CHECK(pv::label(v).view() == "PD3.2" && !v.mark && pv::conflict(v).len == 0);
    in.rmdo_major = 4; in.rmdo_minor = 0;   // higher: the answer
    v = pv::derive(in);
    CHECK(pv::label(v).view() == "PD4.0" && !v.mark);
    // Revision is a 3.1 message: an answer below 3.1 is under the proof even with a plain offer
    pv::Inputs low; low.wire_rev = 2; low.rmdo = true; low.rmdo_major = 3; low.rmdo_minor = 0;
    v = pv::derive(low);
    CHECK(pv::label(v).view() == "PD3.1*" && v.basis == pv::Basis::RevisionMsg);
    CHECK(pv::conflict(v).view() == "3.0 < msg 3.1");
    low.epr = true;
    CHECK(pv::conflict(pv::derive(low)).view() == "3.0 < EPR 3.1");
}
static void test_the_conflict_row_fits_the_detail_column() {
    for (unsigned am = 0; am < 16; ++am)
        for (unsigned an = 0; an < 16; ++an) {
            pv::Inputs in; in.wire_rev = 2; in.spr_avs = true; in.rmdo = true; in.rmdo_major = uint8_t(am); in.rmdo_minor = uint8_t(an);
            CHECK(pv::conflict(pv::derive(in)).len <= 16);
            CHECK(pv::label(pv::derive(in)).len <= 8);
        }
}
static std::string head(std::string_view ver, std::vector<std::string_view> items, size_t max) {
    char buf[64];
    pv::headline(buf, sizeof buf, ver, items.data(), items.size(), max);
    return buf;
}
static void test_the_headline_drops_items_not_the_version() {
    CHECK(head("PD3.0", {"PPS 11V 5A", "Mi 55W"}, 26) == "PD3.0  PPS 11V 5A  Mi 55W");
    CHECK(head("PD3.0", {"PPS 11V 5A", "Mi 55W", "QC3 12V"}, 26) == "PD3.0  PPS 11V 5A  Mi 55W");
    CHECK(head("PD3.2*", {"AVS 15-28V 140W", "PPS 3.3-21V 5A"}, 26) == "PD3.2*  AVS 15-28V 140W");
    CHECK(head("PD3.2*", {"AVS 15-28V 140W", "PPS 3.3-21V 5A"}, 26).size() <= 26);
    CHECK(head("PD3.1", {"EPR 28V5A  with more than it fits"}, 26) == "PD3.1");   // the first item too long: the version alone
    CHECK(head("PD2.0", {}, 26) == "PD2.0");
    CHECK(head("", {"PPS 11V 5A"}, 26) == "PPS 11V 5A");   // no PD: no version, no leading gap
    char tiny[4];
    CHECK(pv::headline(tiny, sizeof tiny, "PD3.2*", nullptr, 0, 26) == 3);   // never writes past the buffer
}

int main() {
    test_the_golden_versions();
    test_answer_against_the_offer();
    test_the_conflict_row_fits_the_detail_column();
    test_the_headline_drops_items_not_the_version();
    return test_report("pd_version");
}
