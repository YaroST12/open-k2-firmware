// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// JSON text of a report card (k2/chargertest/report_card_json.hpp) against the hand-written fixture that the Python
// decoder (tools/report_card.py, tests/test_report_card_py.py) is also checked against.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "k2/chargertest/report_card_json.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;
namespace ct = k2::charger_test;

static std::string slurp(const char* path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    std::string t = s.str();
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
    return t;
}

static rc::Card sample() {
    rc::Card c;
    c.id = {0x1A2B3C4D, 0x05AC, 0x1234};
    c.card_no = 3; c.runs = 2; c.boots = 7; c.uptime_s = 123; c.fw = 0xABCDEF12;
    c.num_pdos = 5; c.spec_rev = 3;
    c.handover = ct::HandoverOutcome::FellBack; c.handover_ds = 15;
    c.state = ct::State::Done; c.reason = ct::Reason::None;
    rc::set_name(c, "Desk 65W");
    c.group[unsigned(ct::Group::Pd)] = {rc::GV::Pass, 20000, 0, 0};
    c.group[unsigned(ct::Group::Qc2)] = {rc::GV::Skip, 0, 0, uint8_t(ct::Reason::NoOffer)};
    c.group[unsigned(ct::Group::Afc)] = {rc::GV::Fail, 9000, uint8_t(k2::fc::Fail::NoResponse), uint8_t(ct::Reason::Request), 12000};
    c.group[unsigned(ct::Group::Scp)] = {rc::GV::Fail, 0, 3, uint8_t(ct::Reason::Voltage)};
    c.n_ranges = 2;
    c.range[0] = {uint8_t(ct::Algorithm::Pps), 0, 33, 110, 110, 0};
    c.range[1] = {uint8_t(ct::Algorithm::Pps), 0, 33, 210, 130, 150};
    return c;
}

static void test_probe_identity_fields_render() {
    rc::Card c = sample();
    c.id.pdp_w = 65; c.name_auto = true;
    std::array<char, rc::kJsonMax> buf;
    size_t n = rc::render_json(c, buf);
    CHECK_EQ(std::string(buf.data(), n).find("\"pid\":4660,\"brand\":\"Apple\",\"pdp_w\":65,\"name_auto\":true,\"runs\":2") != std::string::npos, true);
    c.id.vid = 0x1111; c.id.pdp_w = 0; c.name_auto = false;   // unknown brand, nothing extra
    n = rc::render_json(c, buf);
    CHECK_EQ(std::string(buf.data(), n).find("\"pid\":4660,\"runs\":2") != std::string::npos, true);
    // The passive QC4+/QC5 label (Qualcomm VID plus a PPS range; 100 W or more is QC5) after the identity extras.
    // The JSON carries the full justification text. sample() ranges have ma = 0 (unknown current):
    // QC4+ shows the span-only "via PPS ..." form; QC5 falls back to the VID + span form because pd_ma is 0.
    c.id.vid = 0x05C6; c.id.pdp_w = 140; c.name_auto = true;
    n = rc::render_json(c, buf);
    CHECK_EQ(std::string(buf.data(), n).find("\"pid\":4660,\"pdp_w\":140,\"name_auto\":true,\"qc\":\"QC5\",\"runs\":2") != std::string::npos, true);
    c.id.pdp_w = 45;
    n = rc::render_json(c, buf);
    // sample() ranges have ma = 0: span-only justification, no current shown
    CHECK_EQ(std::string(buf.data(), n).find("\"qc\":\"QC4+\"") != std::string::npos, true);
}

// The shared fixture (also decoded by the Python tools) is a v4 card: no cable object in its JSON.
static void test_sample_matches_fixture() {
    rc::Card c = sample();
    c.format = 4;
    std::array<char, rc::kJsonMax> buf;
    const size_t n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(), slurp("tests/fixtures/report_card_sample.json").c_str());
    CHECK_EQ(buf[n], 0);
}

// A v5 card (the format of a card built by this firmware, or an old one after migrate_to_current) is the v4 JSON
// plus the trailing cable object.
static void test_v5_sample_adds_the_cable_object() {
    rc::Card c = sample();
    CHECK_EQ(c.format, rc::kFormat);
    std::array<char, rc::kJsonMax> buf;
    size_t n = rc::render_json(c, buf);
    std::string v4 = slurp("tests/fixtures/report_card_sample.json");
    v4.pop_back();   // the closing brace of the object
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"vid\":0,\"pid\":0,\"max_mv\":0,\"max_ma\":0,\"kind\":0,\"observed\":0,\"spoofed\":0}}").c_str());
    c.cable = {42, 0x2B01, 0x0001, 50000, 5000, 2, rc::kCableObserved | rc::kCableSpoofed};
    n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"seq\":42,\"vid\":11009,\"pid\":1,\"max_mv\":50000,\"max_ma\":5000,\"kind\":2,\"observed\":1,\"spoofed\":1}}").c_str());
    // A run the charger itself powered carries one more key, only then: every other card renders as before.
    c.cable.flags |= rc::kCableSelfPowered;
    n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"seq\":42,\"vid\":11009,\"pid\":1,\"max_mv\":50000,\"max_ma\":5000,\"kind\":2,\"observed\":1,\"spoofed\":1,\"self_powered\":1}}").c_str());
    // The Ra guard's reading (Topology + 1 in bits 3..5) and "started anyway" (bit 6): keys only when set.
    c.cable.flags = rc::kCableObserved | rc::kCableSpoofed | (3 << rc::kCableGuardShift);   // Topology::CableSingleCc
    n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"seq\":42,\"vid\":11009,\"pid\":1,\"max_mv\":50000,\"max_ma\":5000,\"kind\":2,\"observed\":1,\"spoofed\":1,\"guard\":\"cable\"}}").c_str());
    c.cable.flags |= rc::kCableGuardBypassed;
    n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"seq\":42,\"vid\":11009,\"pid\":1,\"max_mv\":50000,\"max_ma\":5000,\"kind\":2,\"observed\":1,\"spoofed\":1,\"guard\":\"cable\",\"guard_bypassed\":1}}").c_str());
    // The OPPO SVID note is a top-level key after the cable, only when the source listed it.
    c.src_flags = rc::kSrcOppoSvid;
    n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"seq\":42,\"vid\":11009,\"pid\":1,\"max_mv\":50000,\"max_ma\":5000,\"kind\":2,\"observed\":1,\"spoofed\":1,\"guard\":\"cable\",\"guard_bypassed\":1},\"oppo_svid\":1}").c_str());
    // The adapter's SCP current limit (mA) follows it, only when read.
    c.src_flags |= 120 << 1;
    n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"seq\":42,\"vid\":11009,\"pid\":1,\"max_mv\":50000,\"max_ma\":5000,\"kind\":2,\"observed\":1,\"spoofed\":1,\"guard\":\"cable\",\"guard_bypassed\":1},\"oppo_svid\":1,\"scp_ma\":6000}").c_str());
}

// V6: a "ufcs" list closes the object, after the cable keys: one entry per mode with its voltage range and, when known, its
// current. A card with no modes (a plain run, or any card from before v6) renders exactly as before.
static void test_v6_ufcs_modes_render() {
    rc::Card c = sample();
    std::array<char, rc::kJsonMax> buf;
    size_t n = rc::render_json(c, buf);
    const std::string plain(buf.data(), n);
    CHECK_EQ(plain.find("\"ufcs\"") == std::string::npos, true);
    c.ufcs[0] = {34, 55, 3000};
    c.ufcs[1] = {50, 200, 5000};
    c.ufcs[2] = {33, 110, 0};      // a mode whose current is not known has none
    n = rc::render_json(c, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (plain.substr(0, plain.size() - 1) +
               ",\"ufcs\":[{\"lo_mv\":3400,\"hi_mv\":5500,\"max_ma\":3000},{\"lo_mv\":5000,\"hi_mv\":20000,\"max_ma\":5000},{\"lo_mv\":3300,\"hi_mv\":11000}]}").c_str());
    // A card decoded from v5 never lists modes, even if its slots held something.
    c.format = 5;
    n = rc::render_json(c, buf);
    CHECK_EQ(std::string(buf.data(), n).find("\"ufcs\"") == std::string::npos, true);
}

// A stored v4 card migrates on read: decode (zero-padded to kPayload, as the store does) then migrate_to_current
// renders the v4 JSON with an empty cable object.
static void test_v4_hex_migrates_to_v5_json() {
    const std::string hex = slurp("tests/fixtures/report_card_sample.hex");
    std::array<uint8_t, rc::kPayload> raw{};
    for (size_t i = 0; i < hex.size() / 2; ++i) raw[i] = uint8_t(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
    rc::Card d;
    CHECK_EQ(rc::decode_card(raw, d), true);
    CHECK_EQ(d.format, 4);
    rc::migrate_to_current(d);
    CHECK_EQ(d.format, rc::kFormat);
    std::array<char, rc::kJsonMax> buf;
    const size_t n = rc::render_json(d, buf);
    std::string v4 = slurp("tests/fixtures/report_card_sample.json");
    v4.pop_back();
    CHECK_STR(std::string(buf.data(), n).c_str(),
              (v4 + ",\"cable\":{\"vid\":0,\"pid\":0,\"max_mv\":0,\"max_ma\":0,\"kind\":0,\"observed\":0,\"spoofed\":0}}").c_str());
}

static void check_hex_fixture(const char* hex_path, const char* json_path) {
    // The hex is the 162 byte v4 payload of the sample card as the flash store kept it before v5; the store
    // zero-pads such payloads to kPayload before decoding, and so does this test.
    const std::string hex = slurp(hex_path);
    CHECK_EQ(hex.size(), size_t(2 * 162));
    std::array<uint8_t, rc::kPayload> raw{};
    for (size_t i = 0; i < raw.size() && 2 * i + 1 < hex.size(); ++i)
        raw[i] = uint8_t(std::stoi(hex.substr(2 * i, 2), nullptr, 16));
    rc::Card d;
    CHECK_EQ(rc::decode_card(raw, d), true);
    std::array<char, rc::kJsonMax> buf;
    const size_t n = rc::render_json(d, buf);
    CHECK_STR(std::string(buf.data(), n).c_str(), slurp(json_path).c_str());
}

static void test_fixture_hex_decodes_to_the_same_json() {
    check_hex_fixture("tests/fixtures/report_card_sample.hex", "tests/fixtures/report_card_sample.json");
}

// A card written by the first firmware (payload byte 33 = 0) still decodes and renders, with the new "why" name.
static void test_legacy_fixture_hex_decodes() {
    check_hex_fixture("tests/fixtures/report_card_sample_v0.hex", "tests/fixtures/report_card_sample_v0.json");
}

static void test_worst_case_fits() {
    rc::Card c = sample();
    rc::set_name(c, "12345678901234567890");
    c.handover = ct::HandoverOutcome::StayedPd; c.handover_ds = 65535;
    c.uptime_s = 0xFFFFFFFF; c.boots = 0xFFFFFFFF; c.fw = 0xFFFFFFFF; c.id = {0xFFFFFFFF, 0xFFFF, 0xFFFF};
    c.times = 255; c.last_boots = 0xFFFF; c.last_uptime_s = 0xFFFFFF;
    for (auto& g : c.group) g = {rc::GV::Fail, 65535, uint8_t(k2::fc::Fail::Inline), uint8_t(ct::Reason::Voltage), 65535};
    c.n_ranges = 4;
    for (auto& r : c.range) r = {uint8_t(ct::Algorithm::SprAvs), 255, uint16_t(65535), uint16_t(65535), uint16_t(65535), uint16_t(65535)};
    for (auto& u : c.ufcs) u = {255, 511, 6350};
    std::array<char, rc::kJsonMax> buf;
    const size_t n = rc::render_json(c, buf);
    printf("worst case card JSON: %zu of %zu bytes\n", n, rc::kJsonMax);
    CHECK_EQ(n < rc::kJsonMax - 8, true);
    CHECK_EQ(buf[n - 1], '}');
    std::array<char, rc::kSummaryMax> sb;
    const size_t m = rc::render_summary(c, std::span<char>(sb.data(), sb.size() - 1));
    printf("worst case card summary: %zu of %zu bytes\n", m, rc::kSummaryMax);
    CHECK_EQ(m < rc::kSummaryMax - 8, true);
    CHECK_EQ(sb[m - 1], '\n');
}

static void test_summary_text() {
    rc::Card c = sample();
    std::array<char, rc::kSummaryMax> sb;
    const size_t m = rc::render_summary(c, std::span<char>(sb.data(), sb.size() - 1));
    const std::string t(sb.data(), m);
    puts(t.c_str());
    CHECK_EQ(t.find("Card ") == 0, true);
    CHECK_EQ(t.find("pass") != std::string::npos, true);
    CHECK_EQ(t.find("Identity ") != std::string::npos, true);
    // A range reads as its own verdict: whole range passed, or where it stopped.
    CHECK_EQ(t.find("- PPS 3.3-11V: \xE2\x9C\x93 whole range\n") != std::string::npos, true);
    CHECK_EQ(t.find("- PPS 3.3-21V: \xE2\x9C\x97 at 15 V, reached 13 V\n") != std::string::npos, true);
    c.pd_ma = 3000; c.range[0].ma = 3000;
    {
        std::array<char, rc::kJsonMax> jb;
        const std::string j(jb.data(), rc::render_json(c, jb));
        CHECK_EQ(j.find("\"PD\":{\"verdict\":\"pass\",\"max_ma\":3000,\"max_mv\":20000") != std::string::npos, true);
        CHECK_EQ(j.find("\"fail_mv\":0,\"max_ma\":3000}") != std::string::npos, true);
        const size_t k = rc::render_summary(c, std::span<char>(sb.data(), sb.size() - 1));
        const std::string t2(sb.data(), k);
        CHECK_EQ(t2.find("- PD: \xE2\x9C\x93 up to 20 V, 3 A\n") != std::string::npos, true);
        CHECK_EQ(t2.find("- PPS 3.3-11V 3A: \xE2\x9C\x93 whole range\n") != std::string::npos, true);
    }
    c.n_ranges = 3;
    c.range[2] = {uint8_t(ct::Algorithm::SprAvs), 0, 150, 280, 200, 0};   // stopped early, nothing failed
    const size_t m2 = rc::render_summary(c, std::span<char>(sb.data(), sb.size() - 1));
    CHECK_EQ(std::string(sb.data(), m2).find("- AVS 15-28V: reached 20 V\n") != std::string::npos, true);
}

// A card seen again with the same result carries the repeat count and the time of the latest repeat; a card seen once has
// none of those keys and no line in the summary.
static void test_repeat_renders() {
    rc::Card c = sample();
    std::array<char, rc::kJsonMax> jb;
    std::array<char, rc::kSummaryMax> sb;
    std::string j(jb.data(), rc::render_json(c, jb));
    CHECK_EQ(j.find("\"times\"") == std::string::npos, true);
    CHECK_EQ(j.find("last_boots") == std::string::npos, true);
    size_t m = rc::render_summary(c, std::span<char>(sb.data(), sb.size() - 1));
    CHECK_EQ(std::string(sb.data(), m).find("same result") == std::string::npos, true);

    c.times = 3; c.last_boots = 9; c.last_uptime_s = 4567;
    j.assign(jb.data(), rc::render_json(c, jb));
    CHECK_EQ(j.find("\"uptime_s\":123,\"times\":3,\"last_boots\":9,\"last_uptime_s\":4567,") != std::string::npos, true);
    m = rc::render_summary(c, std::span<char>(sb.data(), sb.size() - 1));
    CHECK_EQ(std::string(sb.data(), m).find("same result x3, last boot 9 uptime 4567 s") != std::string::npos, true);
}

static void test_in_contract_handover_renders() {
    rc::Card c = sample();
    c.handover = ct::HandoverOutcome::InContract;
    std::array<char, rc::kJsonMax> buf;
    const size_t n = rc::render_json(c, buf);
    CHECK_EQ(std::string(buf.data(), n).find("\"handover\":\"in PD contract\"") != std::string::npos, true);
}

// A QC3+ group skipped "no QC3+" (the 5 V baseline passed, the engine got no QC3+ step) reads "no QC3+" on its own line,
// not folded into the "skipped" list; a group skipped for another reason still folds. The JSON "why" says it too.
static void test_qc3plus_skip_reads_no_qc3plus() {
    rc::Card c = sample();
    c.group[unsigned(ct::Group::Qc3Plus)] = {rc::GV::Skip, 0, uint8_t(k2::fc::Fail::NoResponse), uint8_t(ct::Reason::NoQc3Plus)};
    std::array<char, rc::kSummaryMax> sb;
    const size_t m = rc::render_summary(c, std::span<char>(sb.data(), sb.size() - 1));
    const std::string t(sb.data(), m);
    CHECK_EQ(t.find("- QC3+: no QC3+\n") != std::string::npos, true);
    CHECK_EQ(t.find("- QC2: skipped\n") != std::string::npos, true);   // QC2 (not advertised) is the only folded skip
    CHECK_EQ(t.find("QC3+: skipped") == std::string::npos, true);
    std::array<char, rc::kJsonMax> jb;
    const std::string j(jb.data(), rc::render_json(c, jb));
    CHECK_EQ(j.find("\"QC3+\":{\"verdict\":\"skip\"") != std::string::npos, true);
    CHECK_EQ(j.find("\"why\":\"no QC3+\"") != std::string::npos, true);
}

int main() {
    test_repeat_renders();
    test_qc3plus_skip_reads_no_qc3plus();
    test_in_contract_handover_renders();
    test_probe_identity_fields_render();
    test_sample_matches_fixture();
    test_v5_sample_adds_the_cable_object();
    test_v6_ufcs_modes_render();
    test_v4_hex_migrates_to_v5_json();
    test_fixture_hex_decodes_to_the_same_json();
    test_legacy_fixture_hex_decodes();
    test_worst_case_fits();
    test_summary_text();
    return test_report(__FILE__);
}
