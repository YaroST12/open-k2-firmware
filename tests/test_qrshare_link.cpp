// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The link form of a shared record (k2/ui/qrshare_link.hpp): base64url, the text, the version digit ('1' and the schema v2 '2'
// for the card and the cable), the worst-case length, that the symbol is a version 11 code the independent reader returns
// unchanged, and the golden vectors in tests/fixtures/qrlink.tsv that tests/test_qrlink.py and tests/test_cardpage.mjs (the web
// page's decoder) read back.
//
// The golden file has one row per record: name, packed (0/1), record hex (the bytes the link carries: the v2 record for the
// version '2' rows), link text, the card as CARDS.JSON (the firmware's render_json). `K2_QRLINK_WRITE=<path>
// host-build/test_qrshare_link` rewrites it (review the diff before committing). `K2_QRCORPUS_DUMP=<path>` writes the corpus
// rows (name, type, v1 record hex, v2 hex, v2 link) that tools/qr_corpus.py --v2-check compares against its Python prototype.
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

// `json` is the card as render_json writes it for a card row, "-" for the other kinds (tests/test_qrlink.py and
// tests/test_cardpage.mjs decode those with their own decoders and compare to tests/fixtures/qrlink_decoded.json).
// `v2` rows carry the schema v2 record and the version '2' link, the form the Share view now shows; `packed` rows carry the LZ4
// block and the upper case letter (version '1'); plain rows pin the version '1' plain form.
struct Golden { std::string name; qs::Type type; Bytes rec; bool packed; std::string json; rc::Card card; Bytes raw; bool v2 = false; };
static rc::Card card_fh22();
static Bytes card_record(const rc::Card& c);
static Bytes cable_record(unsigned rows);
static std::string link_of(const Golden& g);

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

// The link text of a record, in the form encode() picks (v2 first for a v1 card/cable record). `v1` forces the version '1'
// form, which stays valid and decodable.
static std::string link_of(qs::Type t, const Bytes& rec, bool packed, bool v1 = false) {
    char out[qs::kTextBuf];
    const std::size_t n = lk::encode(t, rec, packed, out, !v1);
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
    CHECK(t.rfind(std::string(lk::kBase) + "1c.", 0) == 0);          // 40 random bytes are not a card record: v1 stays
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

// The kind letters of docs/QR_SHARE.md: lower case = as built (or the v2 record), upper case = the LZ4 block; the card keeps c / z.
static void test_kind_letters() {
    const Bytes rec = pattern(40);
    const struct { qs::Type t; char plain, packed; } k[] = {{qs::Type::Card, 'c', 'z'}, {qs::Type::Emarker, 'e', 'E'},
                                                          {qs::Type::PdCaps, 'p', 'P'}, {qs::Type::Identity, 'i', 'I'},
                                                          {qs::Type::BootInfo, 'b', 'B'}};
    for (const auto& x : k) {
        CHECK_EQ(lk::kind_letter(x.t, false), x.plain);
        CHECK_EQ(lk::kind_letter(x.t, true), x.packed);
        CHECK(link_of(x.t, rec, false, true).rfind(std::string(lk::kBase) + "1" + x.plain + ".", 0) == 0);
        CHECK(link_of(x.t, rec, true).rfind(std::string(lk::kBase) + "1" + x.packed + ".", 0) == 0);
    }
    // Kind 't': a plain ASCII text record at version '1', never packed.
    CHECK(lk::supported(lk::kKindText));
    CHECK(lk::supported('t'));
    CHECK(!lk::supported('x') && !lk::supported('Z'));
    CHECK(lk::fits('t', 1) && lk::fits('t', lk::kMaxRecord) && !lk::fits('t', 0) && !lk::fits('t', lk::kMaxRecord + 1));
    const Bytes text = [] {
        Bytes b(64, 0);   // "QR scan test v11 3px", NUL padded
        const char* s = "QR scan test v11 3px";
        std::memcpy(b.data(), s, std::strlen(s));
        return b;
    }();
    char out[qs::kTextBuf];
    const std::size_t n = lk::encode_kind('t', text, out);
    CHECK(n != 0);
    const std::string t(out, n);
    CHECK(t.rfind(std::string(lk::kBase) + "1t.", 0) == 0);
    Bytes back;
    CHECK(b64_decode(t.substr(lk::kBase.size() + 3), back));
    CHECK(back == text);
    CHECK_EQ(lk::encode_kind('t', {}, out), 0u);                          // empty text: no link
    CHECK_EQ(lk::encode_kind('t', pattern(lk::kMaxRecord + 1), out), 0u); // too long for one code
    CHECK_EQ(lk::encode_kind('x', text, out), 0u);                         // an unknown kind
    CHECK(std::string(out, lk::encode_kind('c', rec, out)) == link_of(qs::Type::Card, rec, false, true));   // the letters go through encode()
    CHECK(std::string(out, lk::encode_kind('E', rec, out)) == link_of(qs::Type::Emarker, rec, true));
}

// One code or none: the rule that decides whether the Share view offers a Link at all. A v1 card/cable record is judged by its
// v2 size, so the 8-row-quirk cable (510 B raw, 189 B v2) has a link form even though the raw record is far over the cap.
static void test_fits_rule() {
    CHECK_EQ(lk::kMaxRecord, 203u);                                        // 321 - the 50 character head, 4 characters per 3 bytes
    CHECK(lk::fits(qs::Type::Emarker, 91));                                // a plain cable record as held
    CHECK(lk::fits(qs::Type::Emarker, lk::kMaxRecord));
    CHECK(!lk::fits(qs::Type::Emarker, lk::kMaxRecord + 1));
    CHECK(!lk::fits(qs::Type::Emarker, 510));                              // as held: over the cap...
    CHECK(!lk::fits(qs::Type::Emarker, 0));
    CHECK(lk::fits(qs::Type::Card, qs::records::kMaxCard));
    CHECK(lk::fits(qs::Type::PdCaps, qs::records::kMaxPdCaps));
    CHECK(lk::fits(qs::Type::Identity, qs::records::kMaxIdentity));
    CHECK(lk::fits(qs::Type::BootInfo, 180));
    CHECK(!lk::fits(qs::Type::Log, 40));

    const Bytes eight = cable_record(8);
    CHECK_EQ(eight.size(), qs::records::kMaxEmarker);
    CHECK(lk::fits(qs::Type::Emarker, {eight.data(), eight.size()}, false));   // ...but via v2 it fits: 189 B -> 302 chars
    CHECK(!lk::fits(qs::Type::Emarker, {eight.data(), eight.size()}, true));
    CHECK(lk::fits(qs::Type::Card, {pattern(40).data(), 40}, false));        // not a card record: the plain length rule, no v2
    const Bytes card = card_record(card_fh22());
    CHECK(lk::fits(qs::Type::Card, {card.data(), card.size()}, false));
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

// The worst report card as the store keeps it: format 7, the full 202 byte record, plain and incompressible, 320 characters of
// the 321 of one version 11 code. Its v2 form is 83 B for a real card and at most 187 B with every field at its limit (300
// characters), still one code.
static void test_worst_card_fits_one_code() {
    static_assert(rc::kMaxRecord <= lk::kMaxRecord, "the worst card record fits one code");
    static_assert(qs::records::kMaxCardV2 <= lk::kMaxRecord, "the worst v2 card record fits one code");
    CHECK_EQ(rc::kMaxRecord, 202u);
    CHECK_EQ(lk::b64_len(rc::kMaxRecord), 270u);
    CHECK(lk::fits(qs::Type::Card, rc::kMaxRecord));
    rc::Card c = cardfx::fh22_full();
    c.n_ranges = rc::kMaxRanges;
    for (auto& r : c.range) r = {uint8_t(ct::Algorithm::SprAvs), 255, 511, 511, 511, 511, 6350};
    for (auto& u : c.ufcs) u = {255, 511, 6350};
    for (auto& g : c.group) g = {rc::GV::Fail, 51100, uint8_t(159), uint8_t(ct::Reason::Voltage), 51100};   // detail 159 packs to 6 ones
    for (auto& e : c.epr) e = {480, {rc::GV::Fail, 51100, uint8_t(159), uint8_t(63), 51100}};   // a full EPR slot
    c.cable = {0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFF, 0xFF};
    c.id = {0xFFFFFFFF, 0xFFFF, 0xFFFF, 255};
    c.card_no = 0xFFFF; c.runs = 0xFFFF; c.handover_ds = 0xFFFF;
    c.fw = c.boots = c.uptime_s = 0xFFFFFFFF;
    c.times = 255; c.last_boots = 0xFFFF; c.last_uptime_s = 0xFFFFFF;
    std::strcpy(c.name, "12345678901234567890");
    const Bytes rec = card_record(c);
    CHECK_EQ(rec.size(), rc::kMaxRecord);
    const std::string t = link_of(qs::Type::Card, rec, false, true);
    CHECK_EQ(t.size(), 320u);
    CHECK(t.size() <= lk::kMaxText);
    // ... and its v2 form: 187 B worst case, 300 characters, still a version 11 code
    Bytes v2(qs::records::kMaxEmarker);
    const std::size_t vn = qs::records::pack_v2(qs::Type::Card, rec, v2);
    CHECK_EQ(vn, qs::records::kMaxCardV2);
    v2.resize(vn);
    char out[qs::kTextBuf];
    const std::size_t n2 = lk::encode_v2(qs::Type::Card, v2, out);
    CHECK(n2 != 0);
    CHECK(std::string(out, n2).rfind(std::string(lk::kBase) + "2c.", 0) == 0);
    CHECK_EQ(n2, lk::kHead + lk::b64_len(vn));
    CHECK_EQ(n2, 300u);
    CHECK(n2 <= lk::kQrBytesV11);
    static qr::Workspace w;
    const int modules = qr::encode_text(out, w, qr::kMaxVersion);
    CHECK(modules >= 21 && modules <= 61);
    const qrdec::Result d = qrdec::decode(modules, [&](int x, int y) { return qr::dark(w, x, y); });
    CHECK(d.ok);
    CHECK(d.text == std::string(out, n2));
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

// The FH22 card re-tested three more times with the same results (format 7 repeat block): 4 runs of one result, the latest at boot 44
// after 1 h 2 m of uptime. The links and the decoders must show the repeat count.
static rc::Card card_fh22_repeat() {
    rc::Card c = card_fh22();
    c.runs = 5; c.times = 4; c.last_boots = 44; c.last_uptime_s = 3725;
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

static Bytes card_record(const rc::Card& c) {
    Bytes rec(rc::kMaxRecord);
    rec.resize(rc::encode_card(rec, c));
    return rec;
}

// The golden rows: every card as its v2 link (the form the Share view shows) plus two version '1' card vectors (plain and
// packed) so the old links stay pinned; every cable as v2, the 8-row one included (510 B raw -> 189 B v2 -> a 302 character link),
// plus one packed v1 cable; the other kinds plain and packed; and one kind 't' text record.
static Golden golden_v2(const char* name, qs::Type t, const Bytes& raw) {
    Golden g{name, t, Bytes(qs::records::kMaxEmarker), false, "-", {}, raw, true};
    g.rec.resize(qs::records::pack_v2(t, raw, g.rec));
    return g;
}

static Golden golden_card_v2(const char* name, const rc::Card& c) {
    Golden g = golden_v2(name, qs::Type::Card, card_record(c));
    g.card = c;
    char js[rc::kJsonMax];
    g.json.assign(js, rc::render_json(c, js));
    return g;
}

// The version '1' forms of a card: plain (the record as the store keeps it) and packed (its LZ4 block when that pays).
static Golden golden_card_v1(const char* name, const rc::Card& c, bool pack) {
    Golden g{name, qs::Type::Card, card_record(c), false, {}, c, card_record(c), false};
    if (pack) {
        Bytes z(lz::bound(g.rec.size()));
        if (const std::size_t n = qs::pack(g.rec, z)) { z.resize(n); g.rec = z; g.packed = true; }
    }
    char js[rc::kJsonMax];
    g.json.assign(js, rc::render_json(c, js));
    return g;
}

// Any other record: `force` makes the LZ4 block even when it saves nothing, so every upper case kind has a golden link.
static Golden golden_of(const char* name, qs::Type t, const Bytes& rec, bool force = false) {
    Golden g{name, t, rec, false, "-", {}, rec, false};
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

// The e-marker record of a cable with `rows` response rows (as the Share page builds it, before the v2 re-pack).
static Bytes cable_record(unsigned rows) {
    return build([&](auto o) {
        return qs::records::emarker(qsfx::oneplus_mag_clone(), "OnePlus 100W mag", qsfx::quirk_rows(rows), o);
    }, qs::records::kMaxEmarker);
}

// The golden rows: every card as its v2 link (the form the Share view shows) plus two version '1' card vectors (plain and
// packed) so the old links stay pinned; every cable as v2, the 8-row one included (510 B raw -> 189 B v2 -> a 302 character link),
// plus one packed v1 cable; the other kinds plain and packed; and one kind 't' text record.
static std::vector<Golden> goldens() {
    namespace r = qs::records;
    using T = qs::Type;
    std::vector<Golden> g = {golden_card_v2("fh22", card_fh22()), golden_card_v2("allpass", card_all_pass()),
                             golden_card_v2("fh22_full", cardfx::fh22_full()), golden_card_v2("ufcs_modes", cardfx::ufcs_modes()),
                             golden_card_v2("fh22_repeat", card_fh22_repeat())};
    g.push_back(golden_card_v1("fh22_plain", card_fh22(), false));
    g.push_back(golden_card_v1("fh22_lz", card_fh22(), true));
    g.push_back(golden_v2("emk_plain", T::Emarker,
                          build([](auto o) { return r::emarker(emrec::builtin_cable(1), "EPR 50V 5A", {}, o); }, r::kMaxEmarker)));
    g.push_back(golden_v2("emk_clone", T::Emarker, cable_record(0)));
    g.push_back(golden_v2("emk_rows2", T::Emarker, cable_record(2)));
    g.push_back(golden_v2("emk_rows4", T::Emarker, cable_record(4)));
    g.push_back(golden_v2("emk_rows8", T::Emarker, cable_record(8)));   // 510 B raw: no v1 link, a v2 one now
    g.push_back(golden_v2("emk_req", T::Emarker,
                          build([](auto o) {
                              return r::emarker(qsfx::oneplus_mag_clone(), "OnePlus 100W mag", qsfx::quirk_rows_req(), o);
                          }, r::kMaxEmarker)));   // rows with request constraints (nreq 1 and 2, a partial header mask)
    g.push_back(golden_of("emk_rows4_lz", T::Emarker, cable_record(4)));
    g.push_back(golden_of("pdcaps_spr", T::PdCaps, build([](auto o) { return r::pdcaps(qsfx::caps_spr(), o); }, r::kMaxPdCaps)));
    g.push_back(golden_of("pdcaps_epr", T::PdCaps, build([](auto o) { return r::pdcaps(caps_epr_real(), o); }, r::kMaxPdCaps)));
    g.push_back(golden_of("pdcaps_epr_lz", T::PdCaps, build([](auto o) { return r::pdcaps(caps_epr_real(), o); }, r::kMaxPdCaps), true));
    g.push_back(golden_of("identity", T::Identity, identity_record()));
    g.push_back(golden_of("identity_lz", T::Identity, identity_record(), true));
    g.push_back(golden_of("boot", T::BootInfo, build([](auto o) { return r::boot(qsfx::boot_snapshot(), o); }, 256)));
    g.push_back(golden_of("boot_lz", T::BootInfo, build([](auto o) { return r::boot(qsfx::boot_snapshot(), o); }, 256), true));
    // The kind 't' text record of the qrtest scan-test links: ASCII, NUL padded.
    {
        Golden t{"qrtest", T::BootInfo, Bytes(64, 0), false, "-", {}, {}, false};
        const char* s = "QR scan test v11 3px";
        std::memcpy(t.rec.data(), s, std::strlen(s));
        t.raw = t.rec;
        g.push_back(t);
    }
    return g;
}

static std::string link_of(const Golden& g) {
    char out[qs::kTextBuf];
    std::size_t n = 0;
    if (g.name == "qrtest") n = lk::encode_kind('t', g.rec, out);
    else if (g.v2) n = lk::encode_v2(g.type, g.rec, out);
    else n = lk::encode(g.type, g.rec, g.packed, out, false);   // the pinned version '1' form of the row
    return std::string(out, n);
}

// The 8-row quirk cable: 510 B raw, 239 B packed — over one code either way in v1, so it used to have no link form. Its v2 record
// is 189 B, a 302 character link: now it fits and opens as a link. Zero, two and four rows fit v1 packed as well.
static void test_big_cable_has_a_v2_link() {
    const Golden eight = golden_v2("rows8", qs::Type::Emarker, cable_record(8));
    CHECK_EQ(eight.raw.size(), qs::records::kMaxEmarker);
    CHECK_EQ(eight.rec.size(), 189u);
    CHECK(!lk::fits(qs::Type::Emarker, eight.raw.size()));               // as held in v1: no link
    CHECK(lk::fits(qs::Type::Emarker, {eight.raw.data(), eight.raw.size()}, false));
    const std::string t = link_of(eight);
    CHECK(t.rfind(std::string(lk::kBase) + "2e.", 0) == 0);
    CHECK_EQ(t.size(), 302u);
    CHECK(t.size() <= lk::kMaxText);
    static qr::Workspace w;
    const int n = qr::encode_text(t.c_str(), w, qr::kMaxVersion);
    CHECK(n >= 21 && n <= 61);                                           // one version 11 code
    const qrdec::Result d = qrdec::decode(n, [&](int x, int y) { return qr::dark(w, x, y); });
    CHECK(d.ok);
    CHECK(d.text == t);
    for (unsigned rows : {0u, 2u, 4u}) {
        const Golden g = golden_v2("rows", qs::Type::Emarker, cable_record(rows));
        CHECK(lk::fits(qs::Type::Emarker, {g.raw.data(), g.raw.size()}, false));
        CHECK(!link_of(g).empty());
    }
}

// A v1 card or cable record is re-packed to v2 by encode() when that fits one code (the smaller code wins); the v1 forms stay
// available and decodable.
static void test_encode_picks_v2() {
    const Bytes card = card_record(card_fh22());
    Bytes v2(qs::records::kMaxEmarker);
    const std::size_t vn = qs::records::pack_v2(qs::Type::Card, card, v2);
    CHECK(vn != 0 && vn < card.size());
    v2.resize(vn);
    const std::string auto_link = link_of(qs::Type::Card, card, false);
    CHECK(auto_link.rfind(std::string(lk::kBase) + "2c.", 0) == 0);     // v2 preferred
    CHECK(auto_link.size() < link_of(qs::Type::Card, card, false, true).size());
    CHECK(link_of(qs::Type::Card, card, false, true).rfind(std::string(lk::kBase) + "1c.", 0) == 0);   // forced v1
    Bytes z(lz::bound(card.size()));
    const std::size_t zn = qs::pack(card, z);
    CHECK(zn != 0);
    z.resize(zn);
    CHECK(link_of(qs::Type::Card, z, true).rfind(std::string(lk::kBase) + "1z.", 0) == 0);              // a block stays v1 packed
    CHECK(link_of(qs::Type::Card, z, true, true).size() == link_of(qs::Type::Card, z, true).size());
    // encode_v2 emits an already packed v2 record; garbage is refused
    char out[qs::kTextBuf];
    CHECK_EQ(lk::encode_v2(qs::Type::Card, v2, out), lk::kHead + lk::b64_len(vn));
    CHECK_EQ(lk::encode_v2(qs::Type::Card, card, out), 0u);             // not a v2 record
    CHECK_EQ(lk::encode_v2(qs::Type::PdCaps, v2, out), 0u);             // only the card and the cable have v2
    // the v2 payload of the picked link is the packed record
    Bytes back;
    CHECK(b64_decode(auto_link.substr(lk::kBase.size() + 3), back));
    CHECK(back == v2);
    // a cable record: same rule
    const Bytes emk = cable_record(2);
    CHECK(link_of(qs::Type::Emarker, emk, false).rfind(std::string(lk::kBase) + "2e.", 0) == 0);
    CHECK(link_of(qs::Type::Emarker, emk, false, true).rfind(std::string(lk::kBase) + "1e.", 0) == 0);
}

static std::string hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto x : b) { s += d[x >> 4]; s += d[x & 15]; }
    return s;
}

static std::string row_of(const Golden& g) {
    return g.name + "\t" + (g.packed ? "1" : "0") + "\t" + hex(g.rec) + "\t" + link_of(g) + "\t" + g.json;
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

// The corpus rows for tools/qr_corpus.py --v2-check: name, type, the v1 record, the firmware's v2 record, its v2 link.
static void dump_corpus(const char* path) {
    std::ofstream o(path);
    o << "# name\ttype\tv1 hex\tv2 hex\tv2 link\n";
    for (const Golden& g : goldens()) {
        if (!qs::records::has_v2(g.type)) continue;
        Bytes v2(qs::records::kMaxEmarker);
        const std::size_t n = qs::records::pack_v2(g.type, g.raw, v2);
        if (n == 0) continue;
        v2.resize(n);
        char out[qs::kTextBuf];
        const std::size_t ln = lk::encode_v2(g.type, v2, out);
        o << g.name << "\t" << unsigned(g.type) << "\t" << hex(g.raw) << "\t" << hex(v2) << "\t" << std::string(out, ln) << "\n";
    }
    // the synthetic all-limits card as well (the worst case for the v2 packer)
    rc::Card c = cardfx::fh22_full();
    c.n_ranges = rc::kMaxRanges;
    for (auto& r : c.range) r = {uint8_t(ct::Algorithm::SprAvs), 255, 511, 511, 511, 511, 6350};
    for (auto& u : c.ufcs) u = {255, 511, 6350};
    for (auto& g : c.group) g = {rc::GV::Fail, 51100, uint8_t(159), uint8_t(ct::Reason::Voltage), 51100};
    for (auto& e : c.epr) e = {480, {rc::GV::Fail, 51100, uint8_t(159), uint8_t(63), 51100}};   // a full EPR slot
    c.cable = {0xFFFFFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFF, 0xFF};
    c.id = {0xFFFFFFFF, 0xFFFF, 0xFFFF, 255};
    c.card_no = 0xFFFF; c.runs = 0xFFFF; c.handover_ds = 0xFFFF;
    c.fw = c.boots = c.uptime_s = 0xFFFFFFFF;
    c.times = 255; c.last_boots = 0xFFFF; c.last_uptime_s = 0xFFFFFF;
    std::strcpy(c.name, "12345678901234567890");
    const Bytes rec = card_record(c);
    Bytes v2(qs::records::kMaxEmarker);
    const std::size_t n = qs::records::pack_v2(qs::Type::Card, rec, v2);
    v2.resize(n);
    char out[qs::kTextBuf];
    const std::size_t ln = lk::encode_v2(qs::Type::Card, v2, out);
    o << "card_worst\t4\t" << hex(rec) << "\t" << hex(v2) << "\t" << std::string(out, ln) << "\n";
    std::printf("wrote %s\n", path);
}

static void test_golden() {
    if (const char* w = std::getenv("K2_QRLINK_WRITE")) {
        std::ofstream o(w);
        o << "# name\tpacked\trecord hex (the bytes the link carries: the v2 record on a version '2' row)\tlink text\tcard JSON (render_json), \"-\" for a record that is not a card\n";
        for (const Golden& g : goldens()) o << row_of(g) << "\n";
        std::printf("wrote %s\n", w);
    }
    if (const char* d = std::getenv("K2_QRCORPUS_DUMP")) dump_corpus(d);
    const auto rows = read_golden();
    const auto gs = goldens();
    CHECK_EQ(rows.size(), gs.size());
    std::size_t worst_card_link = 0;
    for (std::size_t i = 0; i < gs.size() && i < rows.size(); i++) {
        const auto& r = rows[i];
        CHECK_EQ(r.size(), 5u);
        if (r.size() != 5) continue;
        CHECK(r[0] == gs[i].name);
        CHECK(r[1] == (gs[i].packed ? "1" : "0"));
        CHECK(r[2] == hex(gs[i].rec));
        CHECK(r[3] == link_of(gs[i]));
        CHECK(r[4] == gs[i].json);
        CHECK(r[3].size() <= lk::kMaxText);
        if (gs[i].v2 && gs[i].type == qs::Type::Card) worst_card_link = std::max(worst_card_link, r[3].size());
        // The reader side: strip the address, decode, inflate when marked, and the record is the one that went in.
        const std::string& t = r[3];
        const char version = t[lk::kBase.size()];
        const char kind = t[lk::kBase.size() + 1];
        CHECK(t[lk::kBase.size() + 2] == '.');
        if (gs[i].v2) {
            CHECK_EQ(version, lk::kVersionV2);
            CHECK(kind == lk::kind_letter(gs[i].type, false));
            CHECK(!gs[i].packed);
            Bytes rec;
            CHECK(b64_decode(t.substr(lk::kBase.size() + 3), rec));
            CHECK(rec == gs[i].rec);                       // the v2 record the firmware packed
            Bytes again(qs::records::kMaxEmarker);
            CHECK_EQ(qs::records::pack_v2(gs[i].type, gs[i].raw, again), rec.size());
            again.resize(rec.size());
            CHECK(again == rec);
        } else if (gs[i].name == "qrtest") {
            CHECK_EQ(version, lk::kVersion);
            CHECK_EQ(kind, 't');
            Bytes rec;
            CHECK(b64_decode(t.substr(lk::kBase.size() + 3), rec));
            CHECK(rec == gs[i].rec);
        } else {
            CHECK_EQ(version, lk::kVersion);
            CHECK(kind == lk::kind_letter(gs[i].type, gs[i].packed));
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
        }
        // And it is one QR symbol of version 11 or less.
        static qr::Workspace w;
        const int n = qr::encode_text(t.c_str(), w, qr::kMaxVersion);
        CHECK(n >= 21 && n <= 61);
        const qrdec::Result q = qrdec::decode(n, [&](int x, int y) { return qr::dark(w, x, y); });
        CHECK(q.ok);
        CHECK(q.text == t);
    }
    // The real cards' v2 links stay well under the cap: at most 85 % of the 321 characters of a version 11 code.
    CHECK(worst_card_link != 0);
    CHECK(worst_card_link * 100 <= 85 * lk::kQrBytesV11);
}

// The frozen v5 cards (tests/fixtures/qrlink_v5.tsv, 184 byte records from before the UFCS modes): their version '1' links still
// decode, the card reads back with no modes, and its JSON is what v5 firmware wrote (no "ufcs" key). A 184 byte record is under
// the 203 limit.
static void test_v5_links_still_decode() {
    const auto rows = read_golden("tests/fixtures/qrlink_v5.tsv");
    CHECK_EQ(rows.size(), 5u);
    for (const auto& r : rows) {
        CHECK_EQ(r.size(), 5u);
        if (r.size() != 5) continue;
        CHECK_EQ(r[3][lk::kBase.size()], '1');
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
    test_encode_picks_v2();
    test_big_cable_has_a_v2_link();
    test_cards_are_what_the_ui_tests_say();
    test_golden();
    test_v5_links_still_decode();
    return test_report("qrshare_link");
}
