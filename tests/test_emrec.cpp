// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the 66-byte e-marker record and the 10-slot library.
#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "k2/pd/emrec.hpp"
#include "test_util.h"

using namespace k2::emrec;

// A built-in profile (name "[inbuilt emark]", PD 3.0, passive cable 5 A / 50 V).
// The checksum 0x3D was computed independently with python3: (sum(bytes[0..0x40]) + 0xA5) & 0xFF.
static const std::array<std::uint32_t, 5> kVdos = {0xFF00A041, 0x18002B01, 0x00000910, 0x40516910, 0x000A2640};

static Record builtin() { return make(kVdos, 2, "[inbuilt emark]"); }

static void test_record_layout() {
    const Record r = builtin();
    CHECK_EQ(r.size(), 66u);
    CHECK_EQ(std::memcmp(r.data(), "[inbuilt emark]", 15), 0);
    for (int i = 15; i < 20; i++) CHECK_EQ(r[i], 0);                  // NUL padded name
    CHECK_EQ(r[0x14], 2);                                             // pd_spec_rev
    CHECK_EQ(r[0x15], 0x41); CHECK_EQ(r[0x16], 0xA0);                 // vdm_header, little endian
    CHECK_EQ(r[0x17], 0x00); CHECK_EQ(r[0x18], 0xFF);
    CHECK_EQ(r[0x19], 0x01); CHECK_EQ(r[0x1C], 0x18);                 // id header
    CHECK_EQ(r[0x25], 0x40); CHECK_EQ(r[0x27], 0x0A);                 // cable vdo 1 = 0x000A2640
    for (int i = 0x29; i <= 0x40; i++) CHECK_EQ(r[i], 0);             // reserved
    CHECK_EQ(r[0x41], 0x3D);                                          // checksum
    CHECK_EQ(checksum(r), 0x3D);
    CHECK_EQ(valid(r), 1);
}

static void test_name_truncated_and_terminated() {
    const Record r = make(kVdos, 1, "a very long name that exceeds twenty bytes");
    CHECK_EQ(r[19], 0);                                               // always NUL terminated
    CHECK_EQ(std::memcmp(r.data(), "a very long name tha", 19), 0);
    CHECK_EQ(valid(r), 1);
    CHECK_STR(reinterpret_cast<const char*>(make(kVdos, 2).data()), "[copied emark]");
}

static void test_valid_rules() {
    Record r = builtin();
    CHECK_EQ(valid(r), 1);
    Record bad = r; bad[0x41] ^= 1;                                   // checksum
    CHECK_EQ(valid(bad), 0);
    bad = r; bad[0x14] = 0; bad[0x41] = checksum(bad);                // spec rev 0 is not allowed
    CHECK_EQ(valid(bad), 0);
    bad[0x14] = 3; bad[0x41] = checksum(bad);
    CHECK_EQ(valid(bad), 0);
    bad = r; bad[0x18] = 0x00; bad[0x41] = checksum(bad);             // SVID != 0xFF00 structured
    CHECK_EQ(valid(bad), 0);
    bad = r; bad[0x16] &= 0x7F; bad[0x41] = checksum(bad);            // structured VDM bit (0x8000) clear
    CHECK_EQ(valid(bad), 0);
    Record blank; blank.fill(0xFF);                                   // erased flash
    CHECK_EQ(valid(blank), 0);
    Record zero{};
    CHECK_EQ(valid(zero), 0);
    CHECK_EQ(valid(std::span<const std::uint8_t>(r.data(), 65)), 0);  // wrong length
}

static void test_spec_rev_mapping() {
    CHECK_EQ(spec_rev_from_pd(0), 0);
    CHECK_EQ(spec_rev_from_pd(1), 1);
    CHECK_EQ(spec_rev_from_pd(2), 2);
    CHECK_EQ(spec_rev_from_pd(3), 0);
    CHECK_EQ(spec_rev_from_pd(255), 0);
}

static void test_describe() {
    char buf[1024];
    const std::size_t n = describe(builtin(), buf);
    const std::string s(buf, n);
    CHECK_EQ(s.find("name: [inbuilt emark]\n") != std::string::npos, 1);
    CHECK_EQ(s.find("vdm header: FF00A041") != std::string::npos, 1);
    CHECK_EQ(s.find("id header: 18002B01") != std::string::npos, 1);
    CHECK_EQ(s.find("passive cable") != std::string::npos, 1);
    CHECK_EQ(s.find("vid: 2B01") != std::string::npos, 1);
    CHECK_EQ(s.find("pid: 4051") != std::string::npos, 1);
    CHECK_EQ(s.find("5 A") != std::string::npos, 1);
    CHECK_EQ(s.find("checksum: 3D ok") != std::string::npos, 1);
    Record bad = builtin(); bad[0x41] = 0;
    const std::size_t m = describe(bad, buf);
    CHECK_EQ(std::string(buf, m).find("checksum: 00 BAD") != std::string::npos, 1);
    char tiny[16];
    CHECK_EQ(describe(builtin(), tiny) < sizeof tiny, 1);             // truncates, never overruns
}

static Record rec_n(int n) {
    std::array<std::uint32_t, 5> v = kVdos;
    v[3] = 0x40510000u + n;
    return make(v, 2, "slot");
}

static void test_library_roundtrip() {
    Library lib;
    CHECK_EQ(lib.count, 0);
    for (int i = 0; i < 10; i++) CHECK_EQ(library_add(lib, rec_n(i)), 1);
    CHECK_EQ(library_add(lib, rec_n(11)), 0);                         // full
    CHECK_EQ(lib.count, 10);

    std::array<std::uint8_t, kLibraryMaxSize> raw{};
    CHECK_EQ(encode_library(lib, raw), 6 + 10 * 66 + 1);
    CHECK_EQ(raw[4], 0);                                              // default slot
    CHECK_EQ(raw[5], 10);                                             // num_records
    CHECK_EQ(std::memcmp(&raw[6 + 3 * 66], lib.rec[3].data(), 66), 0);// 0x080E3806 + i*66
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < 6 + 10 * 66; i++) sum += raw[i];
    CHECK_EQ(raw[6 + 10 * 66], static_cast<std::uint8_t>(sum + 0xA5));

    Library back;
    CHECK_EQ(decode_library(raw, back), 1);
    CHECK_EQ(back.count, 10);
    CHECK_EQ(std::memcmp(back.rec[9].data(), lib.rec[9].data(), 66), 0);

    std::array<std::uint8_t, 100> small{};
    CHECK_EQ(encode_library(lib, small), 0);                          // does not fit: nothing written
}

static void test_library_decode_rejects() {
    Library lib;
    library_add(lib, rec_n(1));
    library_add(lib, rec_n(2));
    std::array<std::uint8_t, kLibraryMaxSize> raw{};
    const std::size_t n = encode_library(lib, raw);
    CHECK_EQ(n, 6 + 2 * 66 + 1);
    Library out;
    CHECK_EQ(decode_library(std::span<const std::uint8_t>(raw.data(), n), out), 1);
    CHECK_EQ(out.count, 2);

    auto bad = raw; bad[6 + 5] ^= 0x10;                               // record byte: table checksum
    CHECK_EQ(decode_library(bad, out), 0);
    bad = raw; bad[5] = 0;                                            // num_records 1..10
    CHECK_EQ(decode_library(bad, out), 0);
    bad = raw; bad[5] = 11;
    CHECK_EQ(decode_library(bad, out), 0);
    bad = raw; bad[4] = 2;                                            // default slot >= count
    CHECK_EQ(decode_library(bad, out), 0);
    bad = raw; bad[0] ^= 1;                                           // header magic
    CHECK_EQ(decode_library(bad, out), 0);
    std::array<std::uint8_t, kLibraryMaxSize> blank; blank.fill(0xFF);  // erased page
    CHECK_EQ(decode_library(blank, out), 0);
    CHECK_EQ(decode_library(std::span<const std::uint8_t>(raw.data(), 10), out), 0);
}

static void test_library_remove() {
    Library lib;
    for (int i = 0; i < 4; i++) library_add(lib, rec_n(i));
    lib.default_slot = 2;
    CHECK_EQ(library_remove(lib, 1), 1);                              // later slots shift down
    CHECK_EQ(lib.count, 3);
    CHECK_EQ(lib.default_slot, 1);                                    // follows its record
    CHECK_EQ(std::memcmp(lib.rec[1].data(), rec_n(2).data(), 66), 0);
    CHECK_EQ(library_remove(lib, 1), 1);                              // remove the default itself
    CHECK_EQ(lib.default_slot, 0);                                    // falls back to the first
    CHECK_EQ(library_remove(lib, 5), 0);                              // out of range
    CHECK_EQ(library_remove(lib, 0), 1);
    CHECK_EQ(library_remove(lib, 0), 1);
    CHECK_EQ(lib.count, 0);
    CHECK_EQ(library_remove(lib, 0), 0);                              // empty
}

// A native-format library page: 4 header bytes, default, count, records at +6, checksum byte after
// them. Built here by hand, not through encode_library().
static std::array<std::uint8_t, 2048> native_page(unsigned n, std::uint8_t h0, bool good_sum) {
    std::array<std::uint8_t, 2048> pg;
    pg.fill(0xFF);
    pg[0] = h0; pg[1] = 0x12; pg[2] = 0x34; pg[3] = 0x56;
    pg[4] = 1; pg[5] = static_cast<std::uint8_t>(n);
    for (unsigned i = 0; i < n; i++) {
        const Record r = rec_n(static_cast<int>(i));
        std::memcpy(&pg[6 + i * 66], r.data(), 66);
    }
    std::uint8_t s = 0xA5;
    for (unsigned i = 0; i < 6 + n * 66; i++) s = static_cast<std::uint8_t>(s + pg[i]);
    pg[6 + n * 66] = good_sum ? s : static_cast<std::uint8_t>(s ^ 0x55);
    return pg;
}

static void test_native_library_parse() {
    NativeLibrary f;
    const auto pg = native_page(3, 0x00, true);               // any header: 00 12 34 56
    CHECK_EQ(parse_native_library(pg, f), 1);
    CHECK_EQ(f.header[0], 0x00); CHECK_EQ(f.header[3], 0x56);
    CHECK_EQ(f.default_slot, 1); CHECK_EQ(f.count, 3);
    CHECK_EQ(f.checksum_ok, 1);
    CHECK_EQ(f.valid_mask, 0x7);
    CHECK_EQ(std::memcmp(f.rec[2].data(), &pg[6 + 2 * 66], 66), 0);
    // The strict K2 decoder refuses the same page (its magic is not "K2EM"): the two stay apart.
    Library lib;
    CHECK_EQ(decode_library(pg, lib), 0);

    auto bad_sum = native_page(2, 0x7E, false);                // checksum wrong: still listed, flagged
    CHECK_EQ(parse_native_library(bad_sum, f), 1);
    CHECK_EQ(f.checksum_ok, 0); CHECK_EQ(f.count, 2);

    auto bad_rec = native_page(2, 0x7E, true);
    bad_rec[6 + 66 + 0x14] = 9;                                 // record 1: spec rev 9
    CHECK_EQ(parse_native_library(bad_rec, f), 1);
    CHECK_EQ(f.valid_mask, 0x1);

    auto full = native_page(10, 0x00, true);
    CHECK_EQ(parse_native_library(full, f), 1);
    CHECK_EQ(f.count, 10); CHECK_EQ(f.valid_mask, 0x3FF); CHECK_EQ(f.checksum_ok, 1);
}

static void test_native_library_rejects() {
    NativeLibrary f;
    f.count = 77;
    std::array<std::uint8_t, 2048> blank;
    blank.fill(0xFF);
    CHECK_EQ(parse_native_library(blank, f), 0);               // count 0xFF
    auto zero = native_page(1, 0, true);
    zero[5] = 0;
    CHECK_EQ(parse_native_library(zero, f), 0);                // no records
    zero[5] = 11;
    CHECK_EQ(parse_native_library(zero, f), 0);                // more than 10
    CHECK_EQ(f.count, 77);                                      // untouched on failure
    const auto pg = native_page(10, 0, true);
    CHECK_EQ(parse_native_library(std::span<const std::uint8_t>(pg.data(), 6 + 10 * 66), f), 0);   // cut short
    CHECK_EQ(parse_native_library(std::span<const std::uint8_t>(pg.data(), 4), f), 0);
}

static void test_native_library_text() {
    NativeLibrary f;
    const auto pg = native_page(2, 0xC3, true);
    CHECK_EQ(parse_native_library(pg, f), 1);
    char buf[512];
    const std::size_t n = describe_native_library(f, buf);
    const std::string s(buf, n);
    CHECK_EQ(s.find("header: C3 12 34 56\n") != std::string::npos, 1);
    CHECK_EQ(s.find("default slot: 1\n") != std::string::npos, 1);
    CHECK_EQ(s.find("records: 2\n") != std::string::npos, 1);
    CHECK_EQ(s.find("table checksum: ") != std::string::npos && s.find(" ok\n") != std::string::npos, 1);
    CHECK_EQ(s.find("0: ") != std::string::npos, 1);
    CHECK_EQ(s.find("1: ") != std::string::npos, 1);
}

// The same listing straight from the raw page (no 700 B NativeLibrary on the stack): byte-identical, 0 when
// the page is not a native-format table.
static void test_native_page_text() {
    for (const auto& pg : {native_page(2, 0xC3, true), native_page(10, 0x11, true), native_page(3, 0x22, false)}) {
        NativeLibrary f;
        CHECK_EQ(parse_native_library(pg, f), 1);
        char want[1024], got[1024];
        const std::size_t nw = describe_native_library(f, want), ng = describe_native_page(pg, got);
        CHECK_EQ(ng, nw);
        CHECK_EQ(std::string(got, ng) == std::string(want, nw), true);
    }
    char buf[64];
    const std::array<std::uint8_t, 8> blank{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(describe_native_page(blank, buf), 0u);
    CHECK_EQ(describe_native_page({}, buf), 0u);
}

static void test_builtin_cable() {
    for (unsigned kind : {0u, 1u}) {
        const Record r = builtin_cable(kind);
        CHECK_EQ(valid(r), true);
        const std::uint32_t id = vdo(r, 1), cable = vdo(r, 4);
        CHECK_EQ((id >> 27) & 7, 3u);                       // passive cable
        CHECK_EQ((cable >> 5) & 3, 2u);                     // 5 A
        CHECK_EQ((cable >> 9) & 3, kind ? 3u : 0u);         // 50 V or 20 V
        CHECK_EQ((cable >> 17) & 1, kind ? 1u : 0u);        // EPR capable only for the 50 V entry
        CHECK_EQ(cable & 0x0198u, 0u);                      // reserved bits 8:7 and 4:3 clear
        CHECK_EQ((cable >> 20) & 1, 0u);                    // reserved bit 20
        CHECK_EQ((cable >> 21) & 7, 0u);                    // VDO version 1.0
        CHECK_EQ((cable >> 18) & 3, 2u);                    // USB Type-C plug on the far end
        CHECK_EQ((cable >> 11) & 3, 0u);                    // VCONN not required
        CHECK_EQ(cable & 7, kind ? 0u : 2u);                // USB 2.0 for the captured identity, Gen2 for the generated one
        const unsigned lat = (cable >> 13) & 15;
        CHECK_EQ(lat >= 1 && lat <= 8, true);               // valid latency code
    }
    CHECK_EQ(std::memcmp(builtin_cable(1).data(), "[EPR 50V 5A]", 12), 0);
}

// The captured identities, as read on the K2 (Discover Identity ACK on SOP'): VDM header, ID header, cert stat, product,
// cable VDO. The literals are copied from the capture files, not computed from the table under test.
struct Captured {
    unsigned id;
    const char* record_name;
    const char* label;
    std::uint32_t vdos[5];
    unsigned vid, pid, amps, volts;
    bool epr, vconn;
};
static const Captured kCaptured[] = {
    {1, "[EPR 50V 5A]", "EPR 50V 5A", {0xFF00A041u, 0x18002B01u, 0x00000910u, 0x40516910u, 0x000A2640u}, 0x2B01, 0x4051, 5, 50, true, false},
    {2, "[CUKTECH 6A]", "CUKTECH 6A", {0xFF00A041u, 0x18602B01u, 0x00000A2Eu, 0x40516910u, 0x000A2640u}, 0x2B01, 0x4051, 5, 50, true, false},
    {4, "[OnePlus 100W]", "OnePlus 100W", {0xFF00A041u, 0x180022D9u, 0x00000000u, 0x61160000u, 0x80082A40u}, 0x22D9, 0x6116, 5, 30, false, true},
};

static void test_builtin_table_round_trips() {
    CHECK_EQ(kBuiltinCount, 5u);                            // id space 0..4: 0 generic, 1 2 4 captured, 3 retired
    for (const Captured& c : kCaptured) {
        const Record r = builtin_cable(c.id);
        CHECK_EQ(valid(r), true);
        CHECK_EQ(r[0x41], checksum(r));
        CHECK_STR(reinterpret_cast<const char*>(r.data()), c.record_name);
        CHECK_EQ(r[0x14], 2);                               // PD 3.x, what these cables answered
        for (unsigned i = 0; i < 5; ++i) CHECK_EQ(vdo(r, i), c.vdos[i]);
        CHECK_EQ(builtin_info(c.id).label == std::string_view(c.label), true);
        CHECK_EQ(builtin_info(c.id).detail.empty(), false);
        CHECK_EQ(builtin_info(c.id).label.size() <= 24, true);   // the remote name field
        // the record survives the library encode / decode like a saved clone
        Library lib; CHECK_EQ(library_add(lib, r), true);
        std::uint8_t page[kLibraryMaxSize];
        const std::size_t n = encode_library(lib, page);
        Library back; CHECK_EQ(decode_library({page, n}, back), true);
        CHECK_EQ(back.rec[0] == r, true);
    }
}

static void test_builtin_table_ratings() {
    for (const Captured& c : kCaptured) {
        const Record r = builtin_cable(c.id);
        const std::uint32_t id = vdo(r, 1), cable = vdo(r, 4);
        CHECK_EQ((id >> 27) & 7, 3u);                       // passive cable
        CHECK_EQ(id & 0xFFFF, c.vid);
        CHECK_EQ(vdo(r, 3) >> 16, c.pid);
        CHECK_EQ(((cable >> 5) & 3) == 2 ? 5u : 0u, c.amps);
        CHECK_EQ(20 + 10 * ((cable >> 9) & 3), c.volts);
        CHECK_EQ(((cable >> 17) & 1) != 0, c.epr);
        CHECK_EQ(((cable >> 11) & 3) != 0, c.vconn);        // VCONN required bit
        CHECK_EQ((cable >> 21) & 7, 0u);                    // VDO version 1.0
        CHECK_EQ((cable >> 13) & 15, 1u);                   // latency <10 ns
    }
    char buf[600];
    const std::size_t n = describe(builtin_cable(2), buf);
    const std::string s(buf, n);
    CHECK_EQ(s.find("current: 5 A\n") != std::string::npos, true);
    CHECK_EQ(s.find("voltage: 50 V\n") != std::string::npos, true);
    CHECK_EQ(s.find("cert stat: 00000A2E\n") != std::string::npos, true);
    const std::size_t m = describe(builtin_cable(4), buf);
    const std::string t(buf, m);
    CHECK_EQ(t.find("voltage: 30 V\n") != std::string::npos, true);
}

static void test_builtin_ids_out_of_range_fall_back_to_generic() {
    CHECK_EQ(builtin_cable(kBuiltinCount) == builtin_cable(0), true);
    CHECK_EQ(builtin_cable(255) == builtin_cable(0), true);
    CHECK_EQ(builtin_info(kBuiltinCount).label == builtin_info(0).label, true);
    for (unsigned id = 0; id < kBuiltinCount; ++id)
        for (unsigned other = id + 1; other < kBuiltinCount; ++other)
            if (id != kRetiredMagId && other != kRetiredMagId) CHECK_EQ(builtin_cable(id) == builtin_cable(other), false);   // every live entry is distinct
}

// Id 3 was "OnePlus 100W (mag)", a duplicate of id 4 (same OPPO VID, 30 V 5 A). The id stays reserved: a stored or remote 3
// reads as OnePlus 100W, never as another cable and never as past the table.
static void test_retired_mag_id_reads_as_oneplus_100w() {
    CHECK_EQ(kRetiredMagId, 3u);
    CHECK_EQ(canonical_builtin_id(3), 4u);
    for (unsigned id : {0u, 1u, 2u, 4u}) CHECK_EQ(canonical_builtin_id(id), id);
    CHECK_EQ(builtin_cable(3) == builtin_cable(4), true);
    CHECK_EQ(builtin_info(3).label == std::string_view("OnePlus 100W"), true);
    CHECK_EQ(vdo(builtin_cable(3), 3) >> 16, 0x6116u);
    for (unsigned id = 0; id < kBuiltinCount; ++id) CHECK_EQ(builtin_info(id).label == std::string_view("OnePlus 100W (mag)"), false);
}
int main() {
    test_builtin_cable();
    test_retired_mag_id_reads_as_oneplus_100w();
    test_builtin_table_round_trips();
    test_builtin_table_ratings();
    test_builtin_ids_out_of_range_fall_back_to_generic();
    test_native_library_parse();
    test_native_library_rejects();
    test_native_library_text();
    test_native_page_text();
    test_record_layout();
    test_name_truncated_and_terminated();
    test_valid_rules();
    test_spec_rev_mapping();
    test_describe();
    test_library_roundtrip();
    test_library_decode_rejects();
    test_library_remove();
    return test_report("test_emrec");
}
