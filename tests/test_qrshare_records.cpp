// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR share records (k2/ui/qrshare_records.hpp): the builders for every record type, the golden vectors in
// tests/fixtures/qrshare.tsv (which tests/test_k2_qr.py decodes with the independent Python reader), and that the firmware's
// own parsers read each record back.
//
// The golden file has one row per fixture: name, type, schema, record hex, then the text of each part, tab separated.
// `K2_QRSHARE_WRITE=<path> host-build/test_qrshare_records` rewrites it from the builders (review the diff before committing).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <cstdarg>

#include "k2/core/binlog.hpp"
#include "k2/core/bootinfo.hpp"
#include "k2/core/lz.hpp"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/qrshare_records.hpp"
#include "qr_decode.hpp"
#include "qrshare_fixtures.h"
#include "test_util.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
namespace qs = k2::qrshare;
namespace rc = k2::report_card;
using Bytes = std::vector<std::uint8_t>;

struct Fixture {
    std::string name;
    qs::Type type;
    std::uint8_t schema;
    Bytes rec;               // as sent: the LZ4 block when `packed`
    bool packed = false;
};

static std::string hex(const Bytes& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto v : b) { s += d[v >> 4]; s += d[v & 15]; }
    return s;
}

static Bytes unhex(const std::string& s) {
    Bytes b;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(std::uint8_t(std::stoi(s.substr(i, 2), nullptr, 16)));
    return b;
}

static std::string slurp(const char* path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    std::string t = s.str();
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
    return t;
}

template <typename F>
static Bytes build(F f, std::size_t cap) {
    Bytes b(cap);
    const std::size_t n = f(std::span<std::uint8_t>(b));
    b.resize(n);
    return b;
}

using qsfx::boot_snapshot;
using qsfx::caps_epr;
using qsfx::caps_spr;
using qsfx::oneplus_mag_clone;
// An e-marker clone with eight response rows: the biggest record (510 bytes, two parts).
static emk_quirk::Quirks eight_rows() { return qsfx::quirk_rows(emk_quirk::kMaxRows); }

static rc::Card card_v5() {
    const std::string h = slurp("tests/fixtures/report_card_sample.hex");
    std::array<std::uint8_t, rc::kPayload> raw{};
    for (std::size_t i = 0; i < raw.size() && 2 * i + 1 < h.size(); ++i) raw[i] = std::uint8_t(std::stoi(h.substr(2 * i, 2), nullptr, 16));
    rc::Card c;
    rc::decode_card(raw, c);
    rc::migrate_to_current(c);
    return c;
}

static void bin_push(BinLog<30>& ring, std::uint32_t ms, std::uint16_t id, std::uint32_t sig, ...) {
    std::va_list ap;
    va_start(ap, sig);
    ring.push(ms, id, sig, ap);
    va_end(ap);
}

// A log ring that wrapped: ids from tools/logdb.json ([BOOT] %s, [BBSTORE] entries=%d used=%u free=%u, [BOOT] trace done: %d
// samples, [ADC] raw sweep failed); the oldest entry has been dropped, so base_ms is not 0.
static Bytes log_record(std::size_t* entries_out = nullptr) {
    using logfmt::kSigned;
    using logfmt::kString;
    using logfmt::kUnsigned;
    BinLog<30> ring;   // holds the last four of the five entries below (26 bytes)
    bin_push(ring, 120, 6, kString, "OK");
    bin_push(ring, 125, 5, kSigned | kUnsigned << 2 | kUnsigned << 4, 3, 0x2A0, 0x1500);
    bin_push(ring, 900, 8, kSigned, 1234);
    bin_push(ring, 905, 2, 0);
    bin_push(ring, 1500, 6, kString, "a b%");
    Bytes b(qs::records::kLogHeadMax + 30);
    const BinLogSnapshot s = ring.snapshot(std::span(b).subspan(qs::records::kLogHeadMax));
    if (entries_out) *entries_out = s.len;
    b.resize(qs::records::log("fcb3128-dirty", 4321, s.base_ms, s.older, s.len, b));
    return b;
}

static std::vector<Fixture> fixtures() {
    using T = qs::Type;
    std::vector<Fixture> f;
    namespace r = qs::records;
    f.push_back({"boot", T::BootInfo, qs::kSchemaBootInfo,
                 build([](auto o) { return r::boot(boot_snapshot(), o); }, 256)});
    f.push_back({"identity", T::Identity, qs::kSchemaIdentity,
                 build([](auto o) { return r::identity("1.0", "fcb3128-dirty", 1, o); }, 64)});
    f.push_back({"emk_plain", T::Emarker, qs::kSchemaEmarker,
                 build([](auto o) { return r::emarker(emrec::builtin_cable(1), "EPR 50V 5A", {}, o); }, r::kMaxEmarker)});
    f.push_back({"emk_rows", T::Emarker, qs::kSchemaEmarker,
                 build([](auto o) { return r::emarker(oneplus_mag_clone(), "OnePlus 100W mag", eight_rows(), o); }, r::kMaxEmarker)});
    f.push_back({"pdcaps_spr", T::PdCaps, qs::kSchemaPdCaps, build([](auto o) { return r::pdcaps(caps_spr(), o); }, r::kMaxPdCaps)});
    f.push_back({"pdcaps_epr", T::PdCaps, qs::kSchemaPdCaps, build([](auto o) { return r::pdcaps(caps_epr(), o); }, r::kMaxPdCaps)});
    f.push_back({"card", T::Card, qs::kSchemaCard, build([](auto o) { return r::card(card_v5(), o); }, r::kMaxCard)});
    f.push_back({"log", T::Log, qs::kSchemaLog, log_record()});
    // The records that gain from the LZ4 flag (a quarter or more): the same bytes as above, packed.
    for (const char* base : {"emk_rows", "card"}) {
        for (const Fixture& x : std::vector<Fixture>(f)) {
            if (x.name != base) continue;
            Bytes z(lz::bound(x.rec.size()));
            z.resize(qs::pack(x.rec, z));
            f.push_back({x.name + "_lz", x.type, x.schema, z, true});
        }
    }
    return f;
}

static std::vector<std::string> parts_of(const Fixture& x) {
    std::vector<std::string> out;
    for (unsigned i = 0; i < qs::part_count(x.rec.size()); i++) {
        char t[qs::kTextBuf];
        const std::size_t n = qs::encode_part(x.type, x.schema, x.rec, i, t, x.packed);
        out.emplace_back(t, n);
    }
    return out;
}

static std::string row_of(const Fixture& x) {
    std::string s = x.name + "\t" + std::to_string(int(x.type) | (x.packed ? qs::kPackedFlag : 0)) + "\t" + std::to_string(int(x.schema)) + "\t" + hex(x.rec);
    for (const auto& p : parts_of(x)) s += "\t" + p;
    return s;
}

static std::vector<std::vector<std::string>> read_golden() {
    std::vector<std::vector<std::string>> rows;
    std::ifstream f("tests/fixtures/qrshare.tsv");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t')) cols.push_back(c);
        rows.push_back(cols);
    }
    return rows;
}

static void test_builders() {
    namespace r = qs::records;
    Bytes buf(r::kMaxEmarker);
    CHECK_EQ(r::emarker(emrec::builtin_cable(1), "x", {}, buf), r::kEmarkerHead);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[1], 'x');
    CHECK_EQ(buf[2], 0);                                                          // the name is NUL padded
    Bytes small(10);
    CHECK_EQ(r::emarker(emrec::builtin_cable(1), "x", {}, small), 0u);            // too small: 0
    const emk_quirk::Quirks q8 = eight_rows();
    CHECK_EQ(r::emarker(emrec::builtin_cable(1), "x", q8, buf), r::kMaxEmarker);
    CHECK_EQ(buf[0], r::kEmarkerQuirks);
    CHECK_EQ(qs::part_count(r::kMaxEmarker), 2u);
    CHECK_EQ(r::emarker(emrec::builtin_cable(1), "x", q8, std::span<std::uint8_t>(buf).first(r::kMaxEmarker - 1)), 0u);   // one byte short

    Bytes pb(r::kMaxPdCaps);
    const pd::SourceCaps none;
    CHECK_EQ(r::pdcaps(none, pb), 0u);                                            // no caps seen: nothing to share
    pd::SourceCaps full = caps_spr();
    full.num_pdos = pd::kMaxPdos;
    CHECK_EQ(r::pdcaps(full, pb), r::kMaxPdCaps);
    CHECK_EQ(r::pdcaps(full, small), 0u);

    Bytes cb(r::kMaxCard);
    CHECK_EQ(r::card(card_v5(), cb), r::kMaxCard);
    CHECK_EQ(r::card(card_v5(), small), 0u);

    Bytes ib(r::kMaxIdentity);
    CHECK_EQ(r::identity("1.0", "abc", 1, ib), 16u);
    Bytes tiny(3);
    CHECK_EQ(r::identity("1.0", "abc", 1, tiny), 0u);

    // LOG: the entries sit at kLogHeadMax in the buffer and move down behind the head.
    Bytes lb(r::kLogHeadMax + 6);
    const Bytes entries = {2, 10, 7, 2, 5, 8};
    std::copy(entries.begin(), entries.end(), lb.begin() + r::kLogHeadMax);
    const std::size_t ln = r::log("abc", 0x11223344, 0x01020304, true, entries.size(), lb);
    CHECK_EQ(ln, 1u + 3 + 1 + 4 + 4 + 6);
    CHECK(Bytes(lb.begin(), lb.begin() + ln) == (Bytes{3, 'a', 'b', 'c', 1, 4, 3, 2, 1, 0x44, 0x33, 0x22, 0x11, 2, 10, 7, 2, 5, 8}));
    CHECK_EQ(r::log("abc", 1, 0, false, 0, lb), 0u);                              // no entries: nothing to share
    CHECK_EQ(r::log("abc", 1, 0, false, 7, lb), 0u);                              // more entries than the buffer holds
    const std::string long_git(60, 'g');
    CHECK_EQ(r::log(long_git, 1, 0, false, 6, lb), 1u + 39 + 1 + 4 + 4 + 6);      // the hash is cut to 39 characters
}

static std::vector<std::uint8_t> pattern_bytes(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    unsigned x = 7;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = std::uint8_t(x >> 16); }
    return v;
}

// ---- schema v2: the bit-packed link form of the card and the cable (k2/ui/qrshare_records.hpp) ----
static void test_pack_v2() {
    namespace r = qs::records;
    CHECK(r::has_v2(qs::Type::Card) && r::has_v2(qs::Type::Emarker));
    CHECK(!r::has_v2(qs::Type::PdCaps) && !r::has_v2(qs::Type::Log) && !r::has_v2(qs::Type::Identity) &&
          !r::has_v2(qs::Type::BootInfo));
    CHECK_EQ(r::kMaxCardV2, 187u);
    CHECK_EQ(r::kMaxEmkV2, 459u);

    // A card: valid v1 record in, v2 out; in place gives the same bytes; the v2 record is never longer.
    const Bytes card = build([](auto o) { return r::card(card_v5(), o); }, r::kMaxCard);
    CHECK(r::valid_v1(qs::Type::Card, card));
    Bytes v2(r::kMaxEmarker), inplace = card;
    const std::size_t n = r::pack_v2(qs::Type::Card, card, v2);
    CHECK(n != 0 && n < card.size());
    CHECK_EQ(n, r::pack_v2_in_place(qs::Type::Card, inplace));
    v2.resize(n);
    inplace.resize(n);
    CHECK(v2 == inplace);
    CHECK_EQ(v2[0], r::kSchemaV2);
    // the other kinds and the malformed records refuse
    CHECK(!r::valid_v1(qs::Type::Card, pattern_bytes(202)));
    CHECK_EQ(r::pack_v2(qs::Type::Card, pattern_bytes(202), v2), 0u);
    CHECK_EQ(r::pack_v2(qs::Type::Card, pattern_bytes(184), v2), 0u);   // a v5-length record is not re-packed
    CHECK(!r::valid_v1(qs::Type::PdCaps, card));
    CHECK_EQ(r::pack_v2(qs::Type::PdCaps, card, v2), 0u);
    Bytes small(8);
    CHECK_EQ(r::pack_v2(qs::Type::Card, card, small), 0u);               // out too small
    // a 184 byte record that happens to start with 0xA6 but has no payload length byte of 196
    Bytes fake = pattern_bytes(184);
    fake[0] = 0xA6;
    CHECK(!r::valid_v1(qs::Type::Card, fake));

    // A cable: 91 B plain and 510 B with eight rows; the quirk block must fit its row count.
    const Bytes emk = build([](auto o) { return r::emarker(emrec::builtin_cable(1), "EPR 50V 5A", {}, o); }, r::kMaxEmarker);
    const Bytes emk8 = build([](auto o) { return r::emarker(oneplus_mag_clone(), "OnePlus 100W mag", eight_rows(), o); },
                             r::kMaxEmarker);
    CHECK_EQ(emk8.size(), r::kMaxEmarker);
    CHECK(r::valid_v1(qs::Type::Emarker, emk));
    CHECK(r::valid_v1(qs::Type::Emarker, emk8));
    Bytes e2(r::kMaxEmarker), e8(r::kMaxEmarker);
    const std::size_t n91 = r::pack_v2(qs::Type::Emarker, emk, e2);
    const std::size_t n510 = r::pack_v2(qs::Type::Emarker, emk8, e8);
    CHECK_EQ(n91, 44u);
    CHECK_EQ(n510, 189u);                                                 // the 8-row cable: 189 B -> a 302 character link
    CHECK(n510 <= r::kMaxEmkV2);
    Bytes ip8 = emk8;
    CHECK_EQ(r::pack_v2_in_place(qs::Type::Emarker, ip8), n510);
    e8.resize(n510);
    ip8.resize(n510);
    CHECK(e8 == ip8);
    // malformed: a quirk flag without a block, a row count past the table, a block that does not fit its count
    Bytes bad = emk8;
    bad[0] = 0;
    CHECK(!r::valid_v1(qs::Type::Emarker, bad) && r::pack_v2(qs::Type::Emarker, bad, e2) == 0);
    bad = emk8;
    bad[r::kEmarkerHead + 2] = 9;
    CHECK(!r::valid_v1(qs::Type::Emarker, bad));
    bad = emk8;
    bad[r::kEmarkerHead + emk_quirk::kBlockHeader + 3] = 7;   // nrep past kMaxWords in row 0
    CHECK(!r::valid_v1(qs::Type::Emarker, bad));
    CHECK(!r::valid_v1(qs::Type::Emarker, pattern_bytes(50)));
    CHECK_EQ(r::pack_v2(qs::Type::Emarker, pattern_bytes(300), e2), 0u);
}

// What the firmware's own parsers read back from each record.
static void test_firmware_parsers_read_the_records() {
    for (const Fixture& x : fixtures()) {
        if (x.name == "emk_plain" || x.name == "emk_rows") {
            CHECK(x.rec.size() >= qs::records::kEmarkerHead);
            emrec::Record rec;
            std::memcpy(rec.data(), x.rec.data() + 1 + qs::records::kEmarkerName, rec.size());
            CHECK(emrec::valid(rec));
            const bool has_q = x.rec[0] & qs::records::kEmarkerQuirks;
            CHECK_EQ(has_q, x.name == "emk_rows");
            if (has_q) {
                emk_quirk::Quirks got;
                CHECK(emk_quirk::decode(std::span(x.rec).subspan(qs::records::kEmarkerHead), got));
                CHECK(got == eight_rows());
            } else {
                CHECK_EQ(x.rec.size(), qs::records::kEmarkerHead);
            }
        } else if (x.name == "card") {
            CHECK_EQ(x.rec.size(), rc::kMaxRecord);
            CHECK_EQ(x.rec[0], rc::kMagic);
            rc::Card d;
            CHECK(rc::decode_card(std::span(x.rec).subspan(4, rc::kPayload), d));
            CHECK(d == card_v5());
        } else if (x.name == "pdcaps_spr" || x.name == "pdcaps_epr") {
            const pd::SourceCaps c = x.name == "pdcaps_spr" ? caps_spr() : caps_epr();
            CHECK_EQ(x.rec.size(), 6u + 4u * c.num_pdos);
            CHECK_EQ(x.rec[0], c.num_pdos);
            CHECK_EQ(x.rec[1], (c.epr ? 1 : 0) | (c.epr_partial ? 2 : 0));
            CHECK_EQ(x.rec[2], c.spec_rev);
            CHECK_EQ(x.rec[3], c.active_cc);
            CHECK_EQ(x.rec[4] | x.rec[5] << 8, c.raw_header);
            for (unsigned i = 0; i < c.num_pdos; i++)
                CHECK_EQ(x.rec[6 + 4 * i] | x.rec[7 + 4 * i] << 8 | x.rec[8 + 4 * i] << 16 | std::uint32_t(x.rec[9 + 4 * i]) << 24, c.pdos[i].raw);
        } else if (x.name == "identity") {
            CHECK_EQ(std::string(x.rec.begin(), x.rec.end()) == "fw=1.0 g=fcb3128-dirty p=1", true);
        } else if (x.name == "log") {
            std::size_t entries = 0;
            log_record(&entries);
            CHECK(entries > 0);
            CHECK_EQ(x.rec.size(), 1u + 13 + 1 + 4 + 4 + entries);
            CHECK_EQ(x.rec[0], 13);
            CHECK_EQ(std::string(x.rec.begin() + 1, x.rec.begin() + 14) == "fcb3128-dirty", true);
            CHECK_EQ(x.rec[14], 1);                                                // flags: older entries were dropped
            CHECK_EQ(x.rec[19] | x.rec[20] << 8, 4321);                            // now_ms
            std::size_t at = 23, count = 0;                                        // whole entries to the last byte
            while (at < x.rec.size()) { at += 1 + x.rec[at]; count++; }
            CHECK_EQ(at, x.rec.size());
            CHECK(count >= 2);
        } else if (x.name == "boot") {
            CHECK_EQ(std::string(x.rec.begin(), x.rec.end()).rfind("K2B1 fw=1.0 g=fcb3128-dirty p=1 ", 0), 0u);
        }
    }
}

// The golden file against the builders, and the reader against the golden file.
static void test_golden() {
    if (const char* w = std::getenv("K2_QRSHARE_WRITE")) {
        std::ofstream o(w);
        o << "# name\ttype\tschema\trecord hex\tpart texts (K2:<tag><schema>:<base45 frame>)\n";
        for (const Fixture& x : fixtures()) o << row_of(x) << "\n";
        std::printf("wrote %s\n", w);
    }
    const auto rows = read_golden();
    const auto fx = fixtures();
    CHECK_EQ(rows.size(), fx.size());
    for (std::size_t i = 0; i < fx.size() && i < rows.size(); i++) {
        const auto& r = rows[i];
        CHECK(r.size() >= 5);
        if (r.size() < 5) continue;
        CHECK(r[0] == fx[i].name);
        CHECK_EQ(std::stoi(r[1]), int(fx[i].type) | (fx[i].packed ? qs::kPackedFlag : 0));
        CHECK_EQ(std::stoi(r[2]), int(fx[i].schema));
        CHECK(r[3] == hex(fx[i].rec));              // the builder still produces the golden record
        const auto parts = parts_of(fx[i]);
        CHECK_EQ(r.size(), 4 + parts.size());
        for (std::size_t p = 0; p < parts.size() && 4 + p < r.size(); p++) CHECK(r[4 + p] == parts[p]);

        // The reader (a different code path from the builder) reassembles the golden texts, in reverse order.
        Bytes buf(qs::kMaxRecord);
        qs::Assembler a(buf);
        qs::Assembler::Result res = qs::Assembler::Result::Bad;
        for (std::size_t p = r.size(); p-- > 4;) res = a.feed(r[p]);
        CHECK(res == qs::Assembler::Result::Done);
        CHECK(a.type() == fx[i].type);
        CHECK_EQ(a.packed(), fx[i].packed);
        CHECK(Bytes(a.record().begin(), a.record().end()) == unhex(r[3]));
        if (fx[i].packed) {   // the block inflates to the plain fixture of the same name
            Bytes raw(qs::kMaxRecord);
            const std::size_t rn = lz::decompress(a.record(), raw);
            raw.resize(rn == lz::kError ? 0 : rn);
            for (const Fixture& base : fx)
                if (base.name + "_lz" == fx[i].name) CHECK(raw == base.rec);
        }

        // Every golden part is a QR symbol of version <= 12 that the independent symbol reader returns unchanged.
        for (std::size_t p = 4; p < r.size(); p++) {
            static qr::Workspace w;
            const int n = qr::encode_text(r[p].c_str(), w, qr::kMaxVersion);
            CHECK(n >= 21);
            const qrdec::Result d = qrdec::decode(n, [&](int x, int y) { return qr::dark(w, x, y); });
            CHECK(d.ok);
            CHECK(d.text == r[p]);
        }
    }
}

// Part counts by record: the multipart share the UI shows (the eight-row clone) needs two.
static void test_part_counts() {
    for (const Fixture& x : fixtures()) {
        const unsigned want = x.name == "emk_rows" ? 2u : 1u;   // its packed form and its v2 form are one part
        CHECK_EQ(qs::part_count(x.rec.size()), want);
        if (x.packed) CHECK(x.rec.size() * 4 <= (x.name == "emk_rows_lz" ? 510u : 184u) * 3);
    }
}

int main() {
    test_builders();
    test_pack_v2();
    test_firmware_parsers_read_the_records();
    test_golden();
    test_part_counts();
    return test_report("test_qrshare_records");
}
