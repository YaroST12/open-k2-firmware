// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// OEM power levels (k2/chargertest/oem_watts.hpp): the level table, the watts wording of a level ("SVOOC 120W", "SCP 22.5W") and
// what the SuperVOOC ceiling stage claims of each level. The golden vectors in tests/fixtures/oem_watts.txt are shared with the
// Python mirror (tools/oem_watts.py, tests/test_oem_watts.py).
#include "test_util.h"
#include "k2/chargertest/oem_watts.hpp"
#include "k2/chargertest/step_label.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace k2;
namespace ct = k2::charger_test;
namespace ow = k2::oem_watts;
namespace sc = k2::svooc_ceiling;
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
static std::vector<std::vector<std::string>> rows(const char* kind) {
    std::vector<std::vector<std::string>> out;
    std::ifstream f("tests/fixtures/oem_watts.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto c = split(line);
        if (c[0] == kind) out.push_back(c);
    }
    return out;
}
static sc::End end_of(const std::string& n) {
    for (unsigned e = 0; e < sc::kEndCount; ++e)
        if (n == (e == 0 ? "none" : sc::name(sc::End(e)))) return sc::End(e);
    std::fprintf(stderr, "unknown end %s\n", n.c_str());
    std::exit(2);
}
static const char* state_name(ow::Claim c) {
    return c == ow::Claim::Claimed ? "claimed" : c == ow::Claim::NotReached ? "not reached" : "not tested";
}

static void test_the_golden_labels() {
    const auto v = rows("label");
    CHECK_EQ(v.size() >= 10u, true);
    for (const auto& c : v) CHECK_STR(ow::label(c[1], uint32_t(std::stoul(c[2]))).c_str(), c[3].c_str());
}
static void test_the_golden_claims() {
    const auto v = rows("claim");
    CHECK_EQ(v.size() >= 12u, true);
    for (const auto& c : v) {
        const sc::Outcome o{uint16_t(std::stoul(c[3], nullptr, 16)), end_of(c[1]), uint16_t(std::stoul(c[2])), 0, 0};
        const ow::Level lv{ow::Family::Vooc, "X", 0, 0, uint16_t(std::stoul(c[4])), c[5] == "y"};
        CHECK_STR(state_name(ow::claim(lv, o)), c[6].c_str());
    }
}
// The table: every level's watts is its pair's (within the 1 W a marketed figure rounds to), in ascending order per family, and
// each family's words fit the Results grid (label strip 8 characters, a cell "120W").
static void test_the_level_table() {
    const ow::Span s = ow::span(ow::Family::Vooc);
    CHECK_EQ(int(s.count), 5);
    for (unsigned i = s.first; i < s.first + s.count; ++i) {
        const auto& l = ow::kLevels[i];
        const uint32_t pair_w = (uint32_t(l.mv) * l.ma + 500000u) / 1000000u;
        CHECK_EQ(pair_w + 1 >= l.w && pair_w <= l.w + 1u, true);
        if (i > s.first) CHECK_EQ(ow::kLevels[i - 1].w < l.w, true);
        CHECK_EQ(std::strlen(l.word) <= 8u, true);
        CHECK_EQ(ow::watts(uint32_t(l.w) * 1000u).len <= 4u, true);
    }
    CHECK_STR(ow::kLevels[s.first].word, "VOOC");
    CHECK_EQ(int(ow::kLevels[s.first].w), 20);
    CHECK_EQ(ow::kLevels[s.first].ceiling, false);           // the 5 V level: the ceiling stage never asks there
    CHECK_STR(ow::kLevels[s.first + s.count - 1].word, "SVOOC");
    CHECK_EQ(int(ow::kLevels[s.first + s.count - 1].w), 120);
}
// One claim per level of the family, packed two bits each; the hardware vector claims every SuperVOOC level and tests no VOOC one.
static void test_family_claims_pack() {
    const sc::Outcome hw{0x06D, sc::End::Stopped, 11020, 3300, 0};
    const uint16_t p = ow::claims(ow::Family::Vooc, hw);
    const ow::Span s = ow::span(ow::Family::Vooc);
    CHECK_EQ(int(ow::unpack(p, 0)), int(ow::Claim::NotTested));   // VOOC 20W
    for (unsigned i = 1; i < s.count; ++i) CHECK_EQ(int(ow::unpack(p, i)), int(ow::Claim::Claimed));
    CHECK_EQ(int(ow::claims(ow::Family::Vooc, {})), 0);              // a Medium run: nothing tested
    const uint16_t q = ow::claims(ow::Family::Vooc, {0x03F, sc::End::Judged, 11000, 0, 0});   // 69 W
    CHECK_EQ(int(ow::unpack(q, 2)), int(ow::Claim::Claimed));      // 65W
    CHECK_EQ(int(ow::unpack(q, 3)), int(ow::Claim::NotReached));   // 100W
}
// A step's own watts: SCP the limit the K2 writes, UFCS the current its mode advertises; no current named, no watts.
static void test_step_watts() {
    ct::Step scp; scp.algorithm = A::Scp; scp.group = ct::Group::Scp; scp.mv = 10000; scp.ma = 2250;
    CHECK_EQ(ow::step_mw(scp), 22500u);
    CHECK_STR(ow::chip(scp).c_str(), "10V 22.5W");
    scp.mv = 4500; scp.ma = 5000;
    CHECK_STR(ow::chip(scp).c_str(), "4.5V 22.5W");
    scp.mv = 5000; scp.ma = 0;                                     // the handshake row: no limit written
    CHECK_EQ(ow::step_mw(scp), 0u);
    CHECK_STR(ow::chip(scp).c_str(), "");
    ct::Step ufcs; ufcs.algorithm = A::Ufcs; ufcs.group = ct::Group::Ufcs; ufcs.mv = 9000;
    ufcs.pdo = ct::pack_ufcs_mode(5000, 11000, 500, 3000);
    CHECK_EQ(ow::step_mw(ufcs), 27000u);
    CHECK_STR(ow::chip(ufcs).c_str(), "9V 27W");
    ufcs.pdo = 0;                                                  // planned before the modes were read: volts only
    CHECK_EQ(ow::step_mw(ufcs), 0u);
    ct::Step afc; afc.algorithm = A::Afc; afc.group = ct::Group::Afc; afc.mv = 9000; afc.ma = 1670;
    CHECK_EQ(ow::step_mw(afc), 0u);                                // nothing in the AFC exchange names a current
    ct::Step qc; qc.algorithm = A::Qc3; qc.group = ct::Group::Qc3; qc.mv = 12000; qc.ma = 1000;
    CHECK_EQ(ow::step_mw(qc), 0u);
    ct::Step pd; pd.algorithm = A::Pd3Fixed; pd.group = ct::Group::Pd; pd.mv = 20000; pd.pdo = 409935u;
    CHECK_EQ(ow::step_mw(pd), 0u);                                 // PD keeps its volts and amps
}

int main() {
    test_the_golden_labels();
    test_the_golden_claims();
    test_the_level_table();
    test_family_claims_pack();
    test_step_watts();
    return test_report("test_oem_watts");
}
