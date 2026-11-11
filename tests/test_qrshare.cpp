// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR share framework (k2/ui/qrshare.hpp): CRC-32, base45, the part split, the text of one part, the reader and the assembler,
// and that every part fits one QR code of version <= 12 at level L (read back through the independent decoder).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "k2/core/lz.hpp"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "qr_decode.hpp"
#include "test_util.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
using namespace k2::qrshare;

static std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

static std::vector<std::uint8_t> pattern(std::size_t n, unsigned seed = 7) {
    std::vector<std::uint8_t> v(n);
    unsigned x = seed;
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = std::uint8_t(x >> 16); }
    return v;
}

static std::string part_text(Type t, std::uint8_t schema, const std::vector<std::uint8_t>& rec, unsigned i) {
    char out[kTextBuf];
    const std::size_t n = encode_part(t, schema, rec, i, out);
    return std::string(out, n);
}

static void test_crc_and_base45() {
    const auto d = bytes("123456789");
    CHECK_EQ(crc32(d), 0xCBF43926u);                                   // the CRC-32 check value
    CHECK_EQ(crc32(std::span<const std::uint8_t>{}), 0u);
    CHECK_EQ(crc32(std::span(d).subspan(4), crc32(std::span(d).first(4))), 0xCBF43926u);   // streaming

    // RFC 9285 vectors.
    struct V { const char* plain; const char* enc; };
    for (const V& v : {V{"AB", "BB8"}, V{"Hello!!", "%69 VD92EX0"}, V{"base-45", "UJCLQE7W581"}, V{"ietf!", "QED8WEX0"}}) {
        const auto in = bytes(v.plain);
        char out[64];
        const std::size_t n = base45_encode(in, out);
        CHECK(std::string(out, n) == v.enc);
        CHECK_EQ(n, base45_len(in.size()));
        std::uint8_t back[64];
        std::size_t bn = 0;
        CHECK(base45_decode(v.enc, back, bn));
        CHECK(std::string(reinterpret_cast<char*>(back), bn) == v.plain);
    }
    std::uint8_t back[8];
    std::size_t bn = 0;
    CHECK(!base45_decode("GGW", back, bn));                            // 65536
    CHECK(!base45_decode("lower", back, bn));                          // outside the alphabet
    CHECK(!base45_decode("ABCD", back, bn));                           // length 1 mod 3
    CHECK(!base45_decode("::", back, bn));                             // a single byte above 255
    CHECK(base45_decode("", back, bn) && bn == 0);
    char small[2];
    CHECK_EQ(base45_encode(bytes("AB"), small), 0u);                   // out too small

    // Every length round-trips.
    for (std::size_t len = 0; len < 40; len++) {
        const auto in = pattern(len, unsigned(len));
        char enc[128];
        const std::size_t n = base45_encode(in, enc);
        std::uint8_t dec[64];
        std::size_t dn = 0;
        CHECK(base45_decode(std::string_view(enc, n), dec, dn));
        CHECK_EQ(dn, len);
        CHECK(std::equal(in.begin(), in.end(), dec));
    }
}

static void test_split() {
    CHECK_EQ(part_count(0), 0u);
    CHECK_EQ(part_count(1), 1u);
    CHECK_EQ(part_count(kPartPayload), 1u);
    CHECK_EQ(part_count(kPartPayload + 1), 2u);
    CHECK_EQ(part_count(2 * kPartPayload), 2u);
    CHECK_EQ(part_count(2 * kPartPayload + 1), 3u);
    CHECK_EQ(part_count(kMaxRecord), kMaxParts);
    CHECK_EQ(part_count(kMaxRecord + 1), 0u);
    // Equal slices, the last one the shorter; they add up and none is empty or over the cap.
    for (std::size_t len : {1u, 2u, 330u, 331u, 485u, 660u, 661u, 1000u, 1319u, 1320u}) {
        const unsigned n = part_count(len);
        std::size_t sum = 0;
        for (unsigned i = 0; i < n; i++) {
            const std::size_t s = slice_len(len, i);
            CHECK(s >= 1 && s <= kPartPayload);
            sum += s;
        }
        CHECK_EQ(sum, len);
        CHECK_EQ(slice_len(len, n), 0u);
    }
    CHECK_EQ(slice_len(331, 0), 166u);
    CHECK_EQ(slice_len(331, 1), 165u);
}

static void test_text() {
    // A vector computed with an independent implementation (python zlib + base45).
    CHECK(part_text(Type::BootInfo, 1, bytes("hello"), 0) == "K2:BI1:ZM9W50100KS001HA22+8D VD63EFWGU3");
    CHECK(tag(Type::Emarker) == "EMK" && tag(Type::Card) == "CARD" && tag(Type::PdCaps) == "PDC" && tag(Type::Identity) == "ID");
    CHECK(tag(Type(99)).empty());
    Type t;
    CHECK(type_from_tag("EMK", t) && t == Type::Emarker);
    CHECK(!type_from_tag("EM", t) && !type_from_tag("", t));
    CHECK(tag(Type::LoopProbe) == "LOOP");
    CHECK(type_from_tag("LOOP", t) && t == Type::LoopProbe);

    char out[kTextBuf];
    CHECK_EQ(encode_part(Type::BootInfo, 1, {}, 0, out), 0u);                       // empty record
    CHECK_EQ(encode_part(Type::BootInfo, 1, pattern(10), 1, out), 0u);              // index past the parts
    CHECK_EQ(encode_part(Type::BootInfo, 1, pattern(kMaxRecord + 1), 0, out), 0u);  // too long
    CHECK_EQ(encode_part(Type(99), 1, pattern(10), 0, out), 0u);                    // no tag
    char tiny[10];
    CHECK_EQ(encode_part(Type::BootInfo, 1, pattern(10), 0, tiny), 0u);             // out too small
    CHECK(part_text(Type::Card, 12, pattern(10), 0).starts_with("K2:CARD12:"));     // two-digit schema in the prefix
    CHECK(part_text(Type::Card, 200, pattern(10), 0).starts_with("K2:CARD200:"));

    // Every character is in the QR alphanumeric set, the text never exceeds kMaxText, the worst part stays under 535.
    std::size_t worst = 0;
    for (std::size_t len : {1u, 66u, 184u, 240u, 330u, 331u, 485u, 1320u}) {
        const auto rec = pattern(len, unsigned(len));
        for (unsigned i = 0; i < part_count(len); i++) {
            const std::string s = part_text(Type::Card, 1, rec, i);
            CHECK(!s.empty());
            for (char c : s) CHECK(std::strchr("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:", c) != nullptr);
            CHECK(s.size() <= kMaxText);
            worst = std::max(worst, s.size());
        }
    }
    CHECK(worst <= qr::kMaxAlnum);
}

static void test_parse_and_assemble() {
    const auto rec = pattern(700, 3);                                  // 3 parts
    const unsigned n = part_count(rec.size());
    CHECK_EQ(n, 3u);
    std::array<std::string, 3> text;
    for (unsigned i = 0; i < n; i++) text[i] = part_text(Type::Emarker, 1, rec, i);

    std::uint8_t buf[kMaxFrame];
    Part p;
    CHECK(parse_part(text[1], p, buf));
    CHECK(p.type == Type::Emarker && p.schema == 1 && p.index == 1 && p.count == 3);
    CHECK_EQ(p.record_len, 700u);
    CHECK_EQ(p.record_crc, crc32(rec));
    CHECK_EQ(p.payload.size(), slice_len(700, 1));
    CHECK(std::equal(p.payload.begin(), p.payload.end(), rec.begin() + 234));

    // Any order, with a duplicate in the middle.
    std::uint8_t rbuf[kMaxRecord];
    Assembler a(rbuf);
    CHECK(!a.started());
    CHECK(a.feed(text[2]) == Assembler::Result::Accepted);
    CHECK(a.started() && !a.complete());
    CHECK(a.feed(text[2]) == Assembler::Result::Duplicate);
    CHECK_EQ(a.parts_seen(), 1u);
    CHECK(a.feed(text[0]) == Assembler::Result::Accepted);
    CHECK(a.record().empty());                                         // not before it is complete
    CHECK(a.feed(text[1]) == Assembler::Result::Done);
    CHECK(a.complete());
    CHECK(a.type() == Type::Emarker && a.schema() == 1 && a.count() == 3);
    CHECK(std::equal(a.record().begin(), a.record().end(), rec.begin(), rec.end()));
    CHECK(a.feed(text[0]) == Assembler::Result::Duplicate);

    // A part of another record, a corrupt character, a wrong prefix, a record that does not fit.
    a.reset();
    CHECK(a.feed(text[0]) == Assembler::Result::Accepted);
    const auto other = pattern(700, 4);
    CHECK(a.feed(part_text(Type::Emarker, 1, other, 1)) == Assembler::Result::Mismatch);
    CHECK(a.feed(part_text(Type::Card, 1, rec, 1)) == Assembler::Result::Mismatch);   // same bytes, another record type
    std::string bad = text[1];
    bad[20] = bad[20] == 'A' ? 'B' : 'A';
    CHECK(a.feed(bad) == Assembler::Result::Bad);                                      // frame CRC
    CHECK(a.feed("K2:EMK2:" + text[1].substr(8)) == Assembler::Result::Bad);           // schema in the prefix differs from the frame
    CHECK(a.feed("K3:" + text[1].substr(3)) == Assembler::Result::Bad);
    CHECK(a.feed("") == Assembler::Result::Bad);
    CHECK(a.feed("K2:EMK1:") == Assembler::Result::Bad);
    std::uint8_t tiny[100];
    Assembler small(tiny);
    CHECK(small.feed(text[0]) == Assembler::Result::TooBig);

    // A damaged tail is refused.
    CHECK(!parse_part("K2:BI1:ZM9W50100KS001HA22+8D VD63EFWGU4", p, buf));
}

// The LZ4 flag: bit 7 of the type byte. The frame, the split and the CRCs are over the transmitted (packed) bytes; the
// prefix, the tag and the schema are the same as for a plain record, so a reader that ignores the flag still sees the type.
static void test_packed_flag() {
    std::vector<std::uint8_t> rec(300);
    for (std::size_t i = 0; i < rec.size(); i++) rec[i] = std::uint8_t(i % 23 < 12 ? 0x41 : i % 7);   // compressible
    char a[kTextBuf], b[kTextBuf];
    const std::size_t na = encode_part(Type::Emarker, 1, rec, 0, a);
    const std::size_t nb = encode_part(Type::Emarker, 1, rec, 0, b, true);
    CHECK(na != 0 && nb != 0);
    CHECK(std::string(b, nb).starts_with("K2:EMK1:"));
    CHECK(std::string(a, na) != std::string(b, nb));                    // only the type byte (and so the CRCs) differ

    std::uint8_t buf[kMaxFrame];
    Part p;
    CHECK(parse_part(std::string_view(a, na), p, buf) && !p.packed && p.type == Type::Emarker);
    CHECK(parse_part(std::string_view(b, nb), p, buf) && p.packed && p.type == Type::Emarker && p.schema == 1);

    // One record is all packed or all plain: a part of the other kind does not join.
    std::uint8_t rbuf[kMaxRecord];
    Assembler as(rbuf);
    CHECK(as.feed(std::string_view(b, nb)) == Assembler::Result::Done);   // 300 bytes: one part
    CHECK(as.packed());
    as.reset();
    const auto big = pattern(700, 9);                                   // 3 parts
    char t0[kTextBuf], t1[kTextBuf];
    encode_part(Type::Card, 1, big, 0, t0, true);
    encode_part(Type::Card, 1, big, 1, t1, false);
    CHECK(as.feed(t0) == Assembler::Result::Accepted);
    CHECK(as.feed(t1) == Assembler::Result::Mismatch);

    // pack(): LZ4 when it saves a quarter or more, else 0 (the caller sends the record as it is).
    std::uint8_t out[kMaxRecord + 64];
    const std::size_t pn = pack(rec, out);
    CHECK(pn != 0 && pn * 4 <= rec.size() * 3);
    std::uint8_t back[kMaxRecord];
    CHECK_EQ(lz::decompress({out, pn}, back), rec.size());
    CHECK(std::equal(rec.begin(), rec.end(), back));
    CHECK_EQ(pack(pattern(300, 3), out), 0u);                           // random bytes do not shrink
    CHECK_EQ(pack(std::span<const std::uint8_t>{}, out), 0u);
    CHECK_EQ(pack(rec, std::span<std::uint8_t>(out, 8)), 0u);           // out too small
}

// Capacity: every part fits one QR code of version <= 12 at level L, and reads back through the independent decoder.
static void test_fit() {
    static qr::Workspace w;
    int worst_n = 0;
    for (std::size_t len : {1u, 66u, 184u, 240u, 330u, 331u, 660u, 1320u}) {
        const auto rec = pattern(len, unsigned(len) + 1);
        for (unsigned i = 0; i < part_count(len); i++) {
            const std::string s = part_text(Type::Emarker, 1, rec, i);
            const int n = qr::encode_text(s.c_str(), w);
            CHECK(n >= 21 && n <= 65);
            worst_n = std::max(worst_n, n);
            const auto r = qrdec::decode(n, [&](int x, int y) { return qr::dark(w, x, y); });
            CHECK(r.ok);
            CHECK(r.text == s);
        }
    }
    CHECK_EQ(worst_n, 65);                                             // the full 330-byte part is version 12
    // The boot-info record (240 characters of text) fits version 10, the size the three-line boot screen is laid out for.
    const auto boot = pattern(240, 5);
    const std::string s = part_text(Type::BootInfo, 1, boot, 0);
    CHECK_EQ(qr::encode_text(s.c_str(), w, 10), 57);

    if (std::getenv("K2_DUMP")) {
        std::printf("capacity (level L): version, modules, alnum chars, payload bytes per part\n");
        static const int cap[13] = {0, 25, 47, 77, 114, 154, 195, 224, 279, 335, 395, 468, 535};
        for (int v = 1; v <= 12; v++) {
            const int bytes = std::max(0, (cap[v] - 10) / 3 * 2 - int(kHeaderLen + kCrcLen));
            std::printf("  v%-2d %2d modules  %3d chars  %3d payload bytes\n", v, 4 * v + 17, cap[v], bytes);
        }
    }
}

int main() {
    test_crc_and_base45();
    test_split();
    test_text();
    test_parse_and_assemble();
    test_packed_flag();
    test_fit();
    return test_report("test_qrshare");
}
