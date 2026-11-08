// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The link form of a shared report card (k2/ui/qrshare_link.hpp): base64url, the text, the worst-case length, that the symbol is
// a version 11 code the independent reader returns unchanged, and the golden vectors in tests/fixtures/qrlink.tsv that
// tests/test_qrlink.py and tests/test_cardpage.mjs (the web page's decoder) read back.
//
// The golden file has one row per card: name, packed (0/1), record hex, link text, the card as CARDS.JSON (the firmware's
// render_json). `K2_QRLINK_WRITE=<path> host-build/test_qrshare_link` rewrites it (review the diff before committing).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "k2/chargertest/report_card.hpp"
#include "k2/chargertest/report_card_json.hpp"
#include "k2/core/lz.hpp"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/qrshare_link.hpp"
#include "qr_decode.hpp"
#include "card_fixtures.h"
#include "qrshare_fixtures.h"
#include "test_util.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
namespace qs = k2::qrshare;
namespace lk = k2::qrshare::link;
namespace rc = k2::report_card;
namespace ct = k2::charger_test;
using A = ct::Algorithm;
using V = ct::Verdict;
using Bytes = std::vector<std::uint8_t>;

static std::string b64(const std::string& s) {
    char out[64];
    const std::size_t n = lk::base64url_encode({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()}, out);
    return std::string(out, n);
}

// A small reader of the same alphabet, so the round trip does not lean on the encoder.
static bool b64_decode(const std::string& in, Bytes& out) {
    out.clear();
    unsigned acc = 0, bits = 0;
    for (char c : in) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
              : c == '-' ? 62 : c == '_' ? 63 : -1;
        if (v < 0) return false;
        acc = acc << 6 | unsigned(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(std::uint8_t(acc >> bits)); acc &= (1u << bits) - 1; }
    }
    return true;
}

static void test_base64url() {
    CHECK(b64("") == "");                       // RFC 4648 section 10, without the padding
    CHECK(b64("f") == "Zg");
    CHECK(b64("fo") == "Zm8");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foob") == "Zm9vYg");
    CHECK(b64("fooba") == "Zm9vYmE");
    CHECK(b64("foobar") == "Zm9vYmFy");
    CHECK(b64(std::string("\xfb\xff", 2)) == "-_8");   // the URL safe letters, not '+' '/'
    CHECK(b64(std::string("\xfb\xef\xbe", 3)) == "--" "--");
    CHECK_EQ(lk::b64_len(184), 246u);
    char tiny[3];
    const std::uint8_t three[3] = {1, 2, 3};
    CHECK_EQ(lk::base64url_encode(three, tiny), 0u);   // 4 characters do not fit 3
}

static std::string link_of(qs::Type t, const Bytes& rec, bool packed) {
    char out[qs::kTextBuf];
    const std::size_t n = lk::encode(t, rec, packed, out);
    return std::string(out, n);
}

static Bytes pattern(std::size_t n, unsigned seed = 7) {
    Bytes v(n);
    unsigned x = seed;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = std::uint8_t(x >> 16); }
    return v;
}

static void test_text_and_refusals() {
    const Bytes rec = pattern(40);
    const std::string t = link_of(qs::Type::Card, rec, false);
    CHECK(t.rfind(std::string(lk::kBase) + "1c.", 0) == 0);
    CHECK_EQ(t.size(), lk::kBase.size() + 3 + lk::b64_len(rec.size()));
    Bytes back;
    CHECK(b64_decode(t.substr(lk::kBase.size() + 3), back));
    CHECK(back == rec);
    CHECK(link_of(qs::Type::Card, rec, true).rfind(std::string(lk::kBase) + "1z.", 0) == 0);   // the LZ4 kind
    CHECK(t.find('=') == std::string::npos && t.find('+') == std::string::npos);                // URL safe

    for (qs::Type t : {qs::Type::Card, qs::Type::Emarker, qs::Type::PdCaps, qs::Type::Identity, qs::Type::BootInfo}) {
        CHECK(lk::supported(t));
        CHECK(lk::kind_letter(t, false) != 0 && lk::kind_letter(t, true) != 0);
        CHECK(lk::kind_letter(t, false) != lk::kind_letter(t, true));
    }
    CHECK(!lk::supported(qs::Type::Log));                                  // the log is far too big for one code
    CHECK(link_of(qs::Type::Log, rec, false).empty());
    CHECK(link_of(qs::Type::Card, {}, false).empty());                     // nothing to link
    CHECK(link_of(qs::Type::Card, pattern(lk::kMaxRecord + 1), false).empty());
    char small[40];
    CHECK_EQ(lk::encode(qs::Type::Card, rec, false, small), 0u);           // an output too small is a refusal, not a cut link
}

// The kind letters of docs/QR_SHARE.md: lower case = as built, upper case = the LZ4 block; the card keeps c / z.
static void test_kind_letters() {
    const Bytes rec = pattern(40);
    const struct { qs::Type t; char plain, packed; } k[] = {{qs::Type::Card, 'c', 'z'}, {qs::Type::Emarker, 'e', 'E'},
                                                          {qs::Type::PdCaps, 'p', 'P'}, {qs::Type::Identity, 'i', 'I'},
                                                          {qs::Type::BootInfo, 'b', 'B'}};
    for (const auto& x : k) {
        CHECK_EQ(lk::kind_letter(x.t, false), x.plain);
        CHECK_EQ(lk::kind_letter(x.t, true), x.packed);
        CHECK(link_of(x.t, rec, false).rfind(std::string(lk::kBase) + "1" + x.plain + ".", 0) == 0);
        CHECK(link_of(x.t, rec, true).rfind(std::string(lk::kBase) + "1" + x.packed + ".", 0) == 0);
    }
}

// One code or none: the rule that decides whether the Share view offers a Link at all.
static void test_fits_rule() {
    CHECK_EQ(lk::kMaxRecord, 203u);                                        // 321 - the 50 character head, 4 characters per 3 bytes
    CHECK(lk::fits(qs::Type::Emarker, 91));                                // a plain cable
    CHECK(lk::fits(qs::Type::Emarker, lk::kMaxRecord));
    CHECK(!lk::fits(qs::Type::Emarker, lk::kMaxRecord + 1));
    CHECK(!lk::fits(qs::Type::Emarker, 510));                              // eight quirk rows
    CHECK(!lk::fits(qs::Type::Emarker, 0));
    CHECK(lk::fits(qs::Type::Card, qs::records::kMaxCard));
    CHECK(lk::fits(qs::Type::PdCaps, qs::records::kMaxPdCaps));
    CHECK(lk::fits(qs::Type::Identity, qs::records::kMaxIdentity));
    CHECK(lk::fits(qs::Type::BootInfo, 180));
    CHECK(!lk::fits(qs::Type::Log, 40));
}

// The worst case: an incompressible record of kMaxRecord bytes. At most 321 characters, a version 11 symbol, and the independent
// reader gets the text back from the matrix.
static void test_worst_case_fits_version_11() {
    CHECK_EQ(lk::kMaxText, 321u);
    CHECK(lk::kMaxText <= lk::kQrBytesV11);
    for (unsigned seed : {1u, 2u, 3u}) {
        const Bytes rec = pattern(lk::kMaxRecord, seed);
        char out[qs::kTextBuf];
        const std::size_t n = lk::encode(qs::Type::Emarker, rec, false, out);
        CHECK_EQ(n, lk::kMaxText);
        static qr::Workspace w;
        const int modules = qr::encode_text(out, w, qr::kMaxVersion);
        CHECK_EQ(modules, 61);                                                 // version 11
        const qrdec::Result d = qrdec::decode(modules, [&](int x, int y) { return qr::dark(w, x, y); });
        CHECK(d.ok);
        CHECK_EQ(d.version, 11);
        CHECK(d.text == std::string(out, n));
    }
}

// The worst report card: format 6, the full 196 byte record (four ranges, four UFCS modes, every group failed, every field at its
// limit), plain and incompressible. It is 50 + 262 = 312 characters, inside the 321 of one version 11 code, with seven bytes to spare.
static void test_worst_card_fits_one_code() {
    static_assert(rc::kMaxRecord <= lk::kMaxRecord, "the worst card record fits one code");
    CHECK_EQ(rc::kMaxRecord, 196u);
    CHECK_EQ(lk::b64_len(rc::kMaxRecord), 262u);
    CHECK(lk::fits(qs::Type::Card, rc::kMaxRecord));
    rc::Card c = cardfx::fh22_full();
    c.n_ranges = rc::kMaxRanges;
    for (auto& r : c.range) r = {uint8_t(ct::Algorithm::SprAvs), 255, 511, 511, 511, 511, 6350};
    for (auto& u : c.ufcs) u = {255, 511, 6350};
    for (auto& g : c.group) g = {rc::GV::Fail, 51100, uint8_t(k2::fc::Fail::Inline), uint8_t(ct::Reason::Voltage), 51100};
    c.cable = {0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFF, 0xFF};
    c.id = {0xFFFFFFFF, 0xFFFF, 0xFFFF, 255};
    c.fw = c.boots = c.uptime_s = 0xFFFFFFFF;
    std::strcpy(c.name, "12345678901234567890");
    Bytes rec(rc::kMaxRecord);
    CHECK_EQ(rc::encode_card(rec, c), size_t(rc::kMaxRecord));
    const std::string t = link_of(qs::Type::Card, rec, false);
    CHECK_EQ(t.size(), 312u);
    CHECK(t.size() <= lk::kMaxText);
    static qr::Workspace w;
    const int modules = qr::encode_text(t.c_str(), w, qr::kMaxVersion);
    CHECK(modules >= 21 && modules <= 61);                              // version 11 or less
    const qrdec::Result d = qrdec::decode(modules, [&](int x, int y) { return qr::dark(w, x, y); });
    CHECK(d.ok);
    CHECK(d.text == t);
}

// ---- the golden cards ----------------------------------------------------------------------------

static ct::Step step(A a, ct::Group g, std::uint8_t pos, unsigned mv, V v, unsigned measured = 0, ct::Reason why = ct::Reason::None,
                     std::uint32_t pdo = 0, bool cont = false, bool avs = false, std::uint8_t detail = 0) {
    ct::Step s;
    s.algorithm = a; s.group = g; s.pos = pos; s.mv = std::uint16_t(mv); s.verdict = v; s.measured_mv = std::uint16_t(measured);
    s.reason = why; s.pdo = pdo; s.continuous = cont; s.avs = avs; s.detail = detail;
    return s;
}

static ct::Evidence evidence(std::uint8_t pdos, std::uint8_t rev, std::uint8_t flags) {
    ct::Evidence e;
    e.wire_rev = rev; e.num_pdos = pdos; e.flags = flags;
    return e;
}

// Baseus FH22 100 W (hardware 2026-11-08): fixed PDOs pass, the PPS 5-21 V APDO stops at 19.4 V (Capped), QC does not answer.
static rc::Card card_fh22() {
    constexpr std::uint32_t kP = (3u << 30) | (210u << 17) | (50u << 8) | 100;   // 5-21 V 5 A
    const std::vector<ct::Step> steps = {
        step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5030),
        step(A::Pd3Fixed, ct::Group::Pd, 2, 9000, V::Pass, 9010),
        step(A::Pd3Fixed, ct::Group::Pd, 3, 12000, V::Pass, 12010),
        step(A::Pd3Fixed, ct::Group::Pd, 4, 15000, V::Pass, 15020),
        step(A::Pd3Fixed, ct::Group::Pd, 5, 20000, V::Pass, 19413),
        step(A::Pps, ct::Group::Pd, 6, 5000, V::Pass, 5040, ct::Reason::None, kP, true),
        step(A::Pps, ct::Group::Pd, 6, 13000, V::Pass, 12965, ct::Reason::None, kP, true),
        step(A::Pps, ct::Group::Pd, 6, 21000, V::Fail, 19419, ct::Reason::Capped, kP, true),
        step(A::Qc2, ct::Group::Qc2, 0, 9000, V::Skip, 0, ct::Reason::NoOffer),
        step(A::Afc, ct::Group::Afc, 0, 9000, V::Skip, 0, ct::Reason::NoOffer),
    };
    rc::Identity id{.fp = 0x6B8A1F3C, .vid = 0, .pid = 0, .pdp_w = 100};
    rc::Card c = rc::build(steps, evidence(6, 3, ct::PpsOffer), {}, id, {.fw = 0x0FCB3128, .boots = 41, .uptime_s = 905},
                           ct::State::Done, ct::Reason::None);
    c.card_no = 7; c.runs = 2;
    std::strcpy(c.name, "FH22 100W");
    return c;
}

// Everything offered and everything reached: fixed levels, PPS and AVS in full, the EPR levels, QC and UFCS, a real cable.
static rc::Card card_all_pass() {
    constexpr std::uint32_t kP = (3u << 30) | (210u << 17) | (33u << 8) | 60;   // PPS 3.3-21 V 3 A
    const std::vector<ct::Step> steps = {
        step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5020),
        step(A::Pd3Fixed, ct::Group::Pd, 2, 9000, V::Pass, 9010),
        step(A::Pd3Fixed, ct::Group::Pd, 3, 15000, V::Pass, 15010),
        step(A::Pd3Fixed, ct::Group::Pd, 4, 20000, V::Pass, 19950),
        step(A::Pps, ct::Group::Pd, 5, 3300, V::Pass, 3310, ct::Reason::None, kP, true),
        step(A::Pps, ct::Group::Pd, 5, 21000, V::Pass, 20990, ct::Reason::None, kP, true),
        step(A::Qc2, ct::Group::Qc2, 0, 9000, V::Pass, 8950),
        step(A::Qc3, ct::Group::Qc3, 0, 12000, V::Pass, 11900),
        step(A::Ufcs, ct::Group::Ufcs, 0, 20000, V::Pass, 19900),
    };
    rc::Identity id{.fp = 0x00C0FFEE, .vid = 0x2717, .pid = 0x1234, .pdp_w = 65};
    rc::Cable cable{.seq = 3, .vid = 0x2B1A, .pid = 0x0102, .max_mv = 50000, .max_ma = 5000, .kind = rc::kCableKindReal,
                    .flags = rc::kCableObserved};
    rc::Card c = rc::build(steps, evidence(5, 3, ct::PpsOffer), {}, id, {.fw = 0x0FCB3128, .boots = 12, .uptime_s = 3600},
                           ct::State::Done, ct::Reason::None, cable);
    c.card_no = 12; c.runs = 1;
    c.epr[0] = {280, {rc::GV::Pass, 28000, 0, 0, 0}};
    c.epr[1] = {360, {rc::GV::Pass, 36000, 0, 0, 0}};
    std::strcpy(c.name, "Desk 65W");
    return c;
}

// `json` is the card as render_json writes it for a card row, "-" for the other kinds (tests/test_qrlink.py and
// tests/test_cardpage.mjs decode those with their own decoders and compare to tests/fixtures/qrlink_decoded.json).
struct Golden { std::string name; qs::Type type; Bytes rec; bool packed; std::string json; rc::Card card; Bytes raw; };

// `pack` = what the Share page does (LZ4 when it saves a quarter); false keeps the plain record, the 'c' kind.
static Golden golden_of(const char* name, const rc::Card& c, bool pack = true) {
    Golden g{name, qs::Type::Card, Bytes(rc::kMaxRecord), false, {}, c, {}};
    g.rec.resize(rc::encode_card(g.rec, c));
    Bytes z(lz::bound(g.rec.size()));
    if (const std::size_t n = pack ? qs::pack(g.rec, z) : 0) { z.resize(n); g.rec = z; g.packed = true; }
    char js[rc::kJsonMax];
    g.json.assign(js, rc::render_json(c, js));
    return g;
}

// Any other record: `force` makes the LZ4 block even when it saves nothing, so every upper case kind has a golden link.
static Golden golden_of(const char* name, qs::Type t, const Bytes& rec, bool force = false) {
    Golden g{name, t, rec, false, "-", {}, rec};
    Bytes z(lz::bound(rec.size()));
    const std::size_t n = force ? lz::compress(rec, z) : qs::pack(rec, z);
    if (n != 0) { z.resize(n); g.rec = z; g.packed = true; }
    return g;
}

template <typename F>
static Bytes build(F f, std::size_t cap) {
    Bytes b(cap);
    b.resize(f(std::span<std::uint8_t>(b)));
    return b;
}

// A source with the EPR levels (hardware shape: fixed 5 9 15 20 V, PPS, then 28 V, 36 V and the EPR AVS range).
static pd::SourceCaps caps_epr_real() {
    pd::SourceCaps c;
    c.valid = true;
    c.num_pdos = 10;
    c.active_cc = 2;
    c.spec_rev = 2;
    c.epr = true;
    c.raw_header = 0x5BB1;
    const std::uint32_t w[10] = {0x2A01912Cu | (1u << 23), 0x0002D12Cu, 0x0003C12Cu, 0x0004B1F4u, 3u << 30 | 210u << 17 | 33u << 8 | 100u,
                                 0, 0, 560u << 10 | 500u, 720u << 10 | 400u, 3u << 30 | 1u << 28 | 280u << 17 | 150u << 8 | 140u};
    for (unsigned i = 0; i < 10; i++) c.pdos[i].raw = w[i];
    return c;
}

static Bytes identity_record() {
    return build([](auto o) { return qs::records::identity("1.0", "fcb3128-dirty", 1, o); }, qs::records::kMaxIdentity);
}

// A cable with `rows` response rows, as the Share page packs it.
static Golden cable(const char* name, unsigned rows, bool force = false) {
    const Bytes rec = build([&](auto o) {
        return qs::records::emarker(qsfx::oneplus_mag_clone(), "OnePlus 100W mag", qsfx::quirk_rows(rows), o);
    }, qs::records::kMaxEmarker);
    return golden_of(name, qs::Type::Emarker, rec, force);
}

// fh22 and allpass as the Share page builds them (both LZ4-packed), then both again plain: the page decodes either kind. After the
// cards, one row per other kind that has a link: the plain and the packed letter of each (a forced block where the Share page would
// not pack), a cable with and without quirk rows, the source's PDOs with the EPR levels.
static std::vector<Golden> goldens() {
    namespace r = qs::records;
    using T = qs::Type;
    std::vector<Golden> g = {golden_of("fh22", card_fh22()), golden_of("allpass", card_all_pass()), golden_of("fh22_plain", card_fh22(), false),
            golden_of("allpass_plain", card_all_pass(), false), golden_of("fh22_full", cardfx::fh22_full()),
            golden_of("ufcs_modes", cardfx::ufcs_modes())};
    g.push_back(golden_of("emk_plain", T::Emarker,
                          build([](auto o) { return r::emarker(emrec::builtin_cable(1), "EPR 50V 5A", {}, o); }, r::kMaxEmarker)));
    g.push_back(cable("emk_clone", 0));
    g.push_back(cable("emk_rows2", 2));
    g.push_back(cable("emk_rows4", 4));
    g.push_back(golden_of("pdcaps_spr", T::PdCaps, build([](auto o) { return r::pdcaps(qsfx::caps_spr(), o); }, r::kMaxPdCaps)));
    g.push_back(golden_of("pdcaps_epr", T::PdCaps, build([](auto o) { return r::pdcaps(caps_epr_real(), o); }, r::kMaxPdCaps)));
    g.push_back(golden_of("pdcaps_epr_lz", T::PdCaps, build([](auto o) { return r::pdcaps(caps_epr_real(), o); }, r::kMaxPdCaps), true));
    g.push_back(golden_of("identity", T::Identity, identity_record()));
    g.push_back(golden_of("identity_lz", T::Identity, identity_record(), true));
    g.push_back(golden_of("boot", T::BootInfo, build([](auto o) { return r::boot(qsfx::boot_snapshot(), o); }, 256)));
    g.push_back(golden_of("boot_lz", T::BootInfo, build([](auto o) { return r::boot(qsfx::boot_snapshot(), o); }, 256), true));
    return g;
}

// The cable with all eight quirk rows is 510 bytes, 239 packed: over one code either way, so it has no link (the Share view then
// offers only the K2 data frames). Zero, two and four rows pack into one code.
static void test_big_cable_has_no_link() {
    const Golden eight = cable("rows8", 8);
    CHECK(eight.packed);
    CHECK(eight.rec.size() > lk::kMaxRecord);
    CHECK(!lk::fits(qs::Type::Emarker, eight.rec.size()));
    CHECK(link_of(qs::Type::Emarker, eight.rec, eight.packed).empty());
    for (unsigned rows : {0u, 2u, 4u}) {
        const Golden g = cable("rows", rows);
        CHECK(lk::fits(qs::Type::Emarker, g.rec.size()));
        CHECK(!link_of(qs::Type::Emarker, g.rec, g.packed).empty());
    }
}

static std::string hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

static std::string row_of(const Golden& g) {
    return g.name + "\t" + (g.packed ? "1" : "0") + "\t" + hex(g.rec) + "\t" + link_of(g.type, g.rec, g.packed) + "\t" + g.json;
}

static std::vector<std::vector<std::string>> read_golden(const char* path = "tests/fixtures/qrlink.tsv") {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) f.push_back(cell);
        rows.push_back(f);
    }
    return rows;
}

static void test_cards_are_what_the_ui_tests_say() {
    const rc::Card c = card_fh22();
    CHECK_EQ(c.n_ranges, 1);
    CHECK_EQ(c.range[0].max_dv, 194);                 // the cap: reached 19.4 V
    CHECK_EQ(c.range[0].fail_dv, 210);
    CHECK_EQ(c.group[unsigned(ct::Group::Pd)].reason, int(ct::Reason::Capped));
    const rc::Card a = card_all_pass();
    CHECK_EQ(int(a.group[unsigned(ct::Group::Pd)].verdict), int(rc::GV::Pass));
    CHECK_EQ(a.range[0].max_dv, a.range[0].hi_dv);
    CHECK_EQ(a.range[0].fail_dv, 0);
}

static void test_golden() {
    if (const char* w = std::getenv("K2_QRLINK_WRITE")) {
        std::ofstream o(w);
        o << "# name\tpacked\trecord hex (the bytes the link carries)\tlink text\tcard JSON (render_json), \"-\" for a record that is not a card\n";
        for (const Golden& g : goldens()) o << row_of(g) << "\n";
        std::printf("wrote %s\n", w);
    }
    const auto rows = read_golden();
    const auto gs = goldens();
    CHECK_EQ(rows.size(), gs.size());
    for (std::size_t i = 0; i < gs.size() && i < rows.size(); i++) {
        const auto& r = rows[i];
        CHECK_EQ(r.size(), 5u);
        if (r.size() != 5) continue;
        CHECK(r[0] == gs[i].name);
        CHECK(r[1] == (gs[i].packed ? "1" : "0"));
        CHECK(r[2] == hex(gs[i].rec));
        CHECK(r[3] == link_of(gs[i].type, gs[i].rec, gs[i].packed));
        CHECK(r[4] == gs[i].json);
        CHECK(r[3].size() <= lk::kMaxText);
        // The reader side: strip the address, decode, inflate when marked, and the record is the one that went in.
        const std::string& t = r[3];
        CHECK(t.rfind(std::string(lk::kBase) + "1" + lk::kind_letter(gs[i].type, gs[i].packed) + ".", 0) == 0);
        Bytes rec;
        CHECK(b64_decode(t.substr(lk::kBase.size() + 3), rec));
        if (gs[i].packed) {
            Bytes raw(qs::records::kMaxEmarker);
            const std::size_t n = lz::decompress(rec, raw);
            CHECK(n != lz::kError);
            raw.resize(n == lz::kError ? 0 : n);
            rec = raw;
        }
        if (gs[i].type == qs::Type::Card) {
            CHECK_EQ(rec.size(), rc::kMaxRecord);
            rc::Card d;
            CHECK(rec.size() > 4 && rc::decode_card(std::span(rec).subspan(4, rc::kPayload), d));
            CHECK(d == gs[i].card);
        } else {
            CHECK(rec == gs[i].raw);
        }
        // And it is one QR symbol of version 11 or less.
        static qr::Workspace w;
        const int n = qr::encode_text(t.c_str(), w, qr::kMaxVersion);
        CHECK(n >= 21 && n <= 61);
        const qrdec::Result q = qrdec::decode(n, [&](int x, int y) { return qr::dark(w, x, y); });
        CHECK(q.ok);
        CHECK(q.text == t);
    }
}

// The frozen v5 cards (tests/fixtures/qrlink_v5.tsv, 184 byte records from before the UFCS modes): their links still decode, the
// card reads back with no modes, and its JSON is what v5 firmware wrote (no "ufcs" key). A 184 byte record is under the 203 limit.
static void test_v5_links_still_decode() {
    const auto rows = read_golden("tests/fixtures/qrlink_v5.tsv");
    CHECK_EQ(rows.size(), 5u);
    for (const auto& r : rows) {
        CHECK_EQ(r.size(), 5u);
        if (r.size() != 5) continue;
        Bytes rec;
        CHECK(b64_decode(r[3].substr(lk::kBase.size() + 3), rec));
        if (r[1] == "1") {
            Bytes raw(qs::records::kMaxEmarker);
            const std::size_t n = lz::decompress(rec, raw);
            CHECK(n != lz::kError);
            raw.resize(n == lz::kError ? 0 : n);
            rec = raw;
        }
        CHECK_EQ(rec.size(), size_t(184));
        if (rec.size() != 184) continue;
        std::array<std::uint8_t, rc::kPayload> payload{};   // the store's zero padding to the current payload
        std::memcpy(payload.data(), rec.data() + 4, 178);
        rc::Card d;
        CHECK(rc::decode_card(payload, d));
        CHECK_EQ(d.format, 5u);
        CHECK_EQ(rc::ufcs_modes(d), 0u);
        char js[rc::kJsonMax];
        CHECK(std::string(js, rc::render_json(d, js)) == r[4]);
    }
}

int main() {
    test_base64url();
    test_text_and_refusals();
    test_kind_letters();
    test_fits_rule();
    test_worst_case_fits_version_11();
    test_worst_card_fits_one_code();
    test_big_cable_has_no_link();
    test_cards_are_what_the_ui_tests_say();
    test_golden();
    test_v5_links_still_decode();
    return test_report("qrshare_link");
}
