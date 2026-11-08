// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emrec.hpp"

#include <algorithm>
#include <cstring>

#include "k2/core/textbuf.hpp"

namespace k2::emrec {

namespace {
constexpr std::size_t kChecksumAt = kRecordSize - 1;   // 0x41
constexpr std::size_t kRevAt = 0x14;
constexpr std::size_t kVdoAt = 0x15;
constexpr std::uint8_t kBias = 0xA5;

void put_le32(std::uint8_t* p, std::uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
std::uint32_t get_le32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint8_t sum_bias(const std::uint8_t* p, std::size_t n) {
    std::uint8_t s = kBias;
    for (std::size_t i = 0; i < n; i++) s = static_cast<std::uint8_t>(s + p[i]);
    return s;
}
}  // namespace

std::uint8_t checksum(const Record& rec) { return sum_bias(rec.data(), kChecksumAt); }

Record make(std::span<const std::uint32_t> vdos, std::uint8_t spec_rev, std::string_view name) {
    Record r{};
    const std::size_t n = std::min(name.size(), kNameLen - 1);
    std::memcpy(r.data(), name.data(), n);
    r[kRevAt] = spec_rev;
    for (unsigned i = 0; i < 5 && i < vdos.size(); i++) put_le32(&r[kVdoAt + 4 * i], vdos[i]);
    r[kChecksumAt] = checksum(r);
    return r;
}

namespace {
// Id 0: ID header = passive cable UFP (type 3), VID 0x1209. Passive Cable VDO (PD R3.2 Table 6.42), reserved bits clear:
// speed 2 (Gen2), 5 A (bits 6:5 = 10b), max VBUS 20 V (bits 10:9 = 00b), termination 0 (no VCONN),
// latency 1 (<10 ns), EPR not capable, plug type 10b (Type-C), VDO version 1.0.
constexpr std::uint32_t kGenericCableVdo = 2u | (2u << 5) | (1u << 13) | (2u << 18);

// Four rows for the five ids 0..4: id 3 (kRetiredMagId) has no row of its own, canonical_builtin_id() sends it to id 4.
constexpr std::size_t kBuiltinRows = kBuiltinCount - 1;
constexpr std::array<BuiltinInfo, kBuiltinRows> kBuiltins{{
    {"[generic 5A 20V]", "generic 20V 5A", "Generic passive 20V 5A",
     {0xFF00A041u, (3u << 27) | 0x1209u, 0, 0x00010001u, kGenericCableVdo}, 2},
    // Captured 2026-11-06 from the user's cables (reverse-engineered), cable VDO 0x000A2640: 5 A, 50 V, EPR, Type-C plug.
    {"[EPR 50V 5A]", "EPR 50V 5A", "Passive cable, EPR 50V 5A",
     {0xFF00A041u, 0x18002B01u, 0x00000910u, 0x40516910u, 0x000A2640u}, 2},
    {"[CUKTECH 6A]", "CUKTECH 6A", "CUKTECH 50V 5A EPR",
     {0xFF00A041u, 0x18602B01u, 0x00000A2Eu, 0x40516910u, 0x000A2640u}, 2},
    // OPPO VID; cable VDO bit 11 = VCONN required, bits 10:9 = 01b = 30 V (the legacy voltage code), no EPR bit.
    {"[OnePlus 100W]", "OnePlus 100W", "OnePlus/OPPO 30V 5A",
     {0xFF00A041u, 0x180022D9u, 0x00000000u, 0x61160000u, 0x80082A40u}, 2},
}};
}  // namespace

unsigned canonical_builtin_id(unsigned id) { return id == kRetiredMagId ? kOnePlusId : id; }

const BuiltinInfo& builtin_info(unsigned id) {
    id = canonical_builtin_id(id);
    return kBuiltins[id == kOnePlusId ? kBuiltinRows - 1 : id < kBuiltinRows - 1 ? id : 0];
}

Record builtin_cable(unsigned id) {
    const BuiltinInfo& b = builtin_info(id);
    return make(b.vdos, b.spec_rev, b.record_name);
}

bool valid(std::span<const std::uint8_t> rec) {
    if (rec.size() != kRecordSize) return false;
    if (rec[kRevAt] != 1 && rec[kRevAt] != 2) return false;
    if ((get_le32(&rec[kVdoAt]) & 0xFF008000u) != 0xFF008000u) return false;
    return rec[kChecksumAt] == sum_bias(rec.data(), kChecksumAt);
}

std::uint8_t spec_rev_from_pd(std::uint8_t wire_rev) { return wire_rev == 1 || wire_rev == 2 ? wire_rev : 0; }

std::uint32_t vdo(const Record& rec, unsigned index) { return get_le32(&rec[kVdoAt + 4 * (index % 5)]); }

std::size_t describe(const Record& rec, std::span<char> out) {
    TextBuf t(out);
    char name[kNameLen + 1];
    std::memcpy(name, rec.data(), kNameLen);
    name[kNameLen] = 0;
    for (char& c : name) if (c != 0 && (c < 0x20 || c > 0x7E)) c = '?';
    const std::uint32_t id = vdo(rec, 1), prod = vdo(rec, 3), cable = vdo(rec, 4);
    const unsigned ptype = (id >> 27) & 7;
    const char* kind = ptype == 3 ? "passive cable" : ptype == 4 ? "active cable" : "other";
    static const char* const kCur[4] = {"?", "3 A", "5 A", "?"};
    static const char* const kVolt[4] = {"20 V", "30 V", "40 V", "50 V"};
    t.put("name: %s\n", name);
    t.put("pd spec rev: %u\n", static_cast<unsigned>(rec[kRevAt]));
    t.put("vdm header: %08X\n", static_cast<unsigned>(vdo(rec, 0)));
    t.put("id header: %08X\n", static_cast<unsigned>(id));
    t.put("cert stat: %08X\n", static_cast<unsigned>(vdo(rec, 2)));
    t.put("product: %08X\n", static_cast<unsigned>(prod));
    t.put("cable vdo: %08X\n", static_cast<unsigned>(cable));
    t.put("type: %s\n", kind);
    t.put("vid: %04X\n", static_cast<unsigned>(id & 0xFFFF));
    t.put("pid: %04X\n", static_cast<unsigned>(prod >> 16));
    t.put("current: %s\n", kCur[(cable >> 5) & 3]);
    t.put("voltage: %s\n", kVolt[(cable >> 9) & 3]);
    t.put("checksum: %02X %s\n", static_cast<unsigned>(rec[kChecksumAt]),
          rec[kChecksumAt] == checksum(rec) ? "ok" : "BAD");
    return t.size();
}

std::size_t encode_library(const Library& lib, std::span<std::uint8_t> out) {
    const std::size_t n = lib.count;
    if (n == 0 || n > kLibrarySlots) return 0;
    const std::size_t len = kLibraryHeader + n * kRecordSize + 1;
    if (out.size() < len) return 0;
    std::memcpy(out.data(), kLibraryMagic.data(), 4);
    out[4] = lib.default_slot;
    out[5] = static_cast<std::uint8_t>(n);
    for (std::size_t i = 0; i < n; i++) std::memcpy(&out[kLibraryHeader + i * kRecordSize], lib.rec[i].data(), kRecordSize);
    out[len - 1] = sum_bias(out.data(), len - 1);
    return len;
}

bool library_valid(std::span<const std::uint8_t> raw) {
    if (raw.size() < kLibraryHeader) return false;
    if (std::memcmp(raw.data(), kLibraryMagic.data(), 4) != 0) return false;
    const std::size_t n = raw[5];
    if (n < 1 || n > kLibrarySlots || raw[4] >= n) return false;
    const std::size_t len = kLibraryHeader + n * kRecordSize + 1;
    if (raw.size() < len) return false;
    return raw[len - 1] == sum_bias(raw.data(), len - 1);
}

unsigned native_library_count(std::span<const std::uint8_t> raw) {
    if (raw.size() < kLibraryHeader) return 0;
    const std::size_t n = raw[5];
    if (n < 1 || n > kLibrarySlots) return 0;
    return raw.size() < kLibraryHeader + n * kRecordSize + 1 ? 0 : static_cast<unsigned>(n);
}

bool decode_library(std::span<const std::uint8_t> raw, Library& out) {
    if (!library_valid(raw)) return false;
    const std::size_t n = raw[5];
    // Everything is validated: fill `out` in place (a local Library here is a 660 B frame on the call chains that load it).
    out.count = static_cast<std::uint8_t>(n);
    out.default_slot = raw[4];
    for (std::size_t i = 0; i < kLibrarySlots; i++) {
        if (i < n) std::memcpy(out.rec[i].data(), &raw[kLibraryHeader + i * kRecordSize], kRecordSize);
        else out.rec[i].fill(0);
    }
    return true;
}

bool parse_native_library(std::span<const std::uint8_t> raw, NativeLibrary& out) {
    if (raw.size() < kLibraryHeader) return false;
    const std::size_t n = raw[5];
    if (n < 1 || n > kLibrarySlots) return false;
    const std::size_t len = kLibraryHeader + n * kRecordSize + 1;
    if (raw.size() < len) return false;
    NativeLibrary& lib = out;     // nothing below can fail: fill `out` in place (a local copy is a ~700 B frame)
    std::memcpy(lib.header.data(), raw.data(), 4);
    lib.default_slot = raw[4];
    lib.count = static_cast<std::uint8_t>(n);
    lib.checksum = raw[len - 1];
    lib.checksum_ok = lib.checksum == sum_bias(raw.data(), len - 1);
    lib.valid_mask = 0;
    for (std::size_t i = 0; i < kLibrarySlots; i++) {
        if (i >= n) { lib.rec[i].fill(0); continue; }
        std::memcpy(lib.rec[i].data(), &raw[kLibraryHeader + i * kRecordSize], kRecordSize);
        if (valid(lib.rec[i])) lib.valid_mask = static_cast<std::uint16_t>(lib.valid_mask | (1u << i));
    }
    return true;
}

std::size_t describe_native_library(const NativeLibrary& lib, std::span<char> out) {
    TextBuf t(out);
    t.put("header: %02X %02X %02X %02X\n", static_cast<unsigned>(lib.header[0]), static_cast<unsigned>(lib.header[1]),
          static_cast<unsigned>(lib.header[2]), static_cast<unsigned>(lib.header[3]));
    t.put("default slot: %u\n", static_cast<unsigned>(lib.default_slot));
    t.put("records: %u\n", static_cast<unsigned>(lib.count));
    t.put("table checksum: %02X %s\n", static_cast<unsigned>(lib.checksum), lib.checksum_ok ? "ok" : "BAD");
    for (unsigned i = 0; i < lib.count; i++) {
        char name[kNameLen + 1];
        std::memcpy(name, lib.rec[i].data(), kNameLen);
        name[kNameLen] = 0;
        for (char& c : name) if (c != 0 && (c < 0x20 || c > 0x7E)) c = '?';
        t.put("%u: %s id=%08X cable=%08X %s\n", i, name, static_cast<unsigned>(vdo(lib.rec[i], 1)),
              static_cast<unsigned>(vdo(lib.rec[i], 4)), (lib.valid_mask >> i) & 1 ? "ok" : "BAD");
    }
    return t.size();
}

std::size_t describe_native_page(std::span<const std::uint8_t> raw, std::span<char> out) {
    const unsigned n = native_library_count(raw);
    if (!n) return 0;
    const std::size_t len = kLibraryHeader + n * kRecordSize + 1;
    TextBuf t(out);
    t.put("header: %02X %02X %02X %02X\n", static_cast<unsigned>(raw[0]), static_cast<unsigned>(raw[1]),
          static_cast<unsigned>(raw[2]), static_cast<unsigned>(raw[3]));
    t.put("default slot: %u\n", static_cast<unsigned>(raw[4]));
    t.put("records: %u\n", n);
    t.put("table checksum: %02X %s\n", static_cast<unsigned>(raw[len - 1]),
          raw[len - 1] == sum_bias(raw.data(), len - 1) ? "ok" : "BAD");
    for (unsigned i = 0; i < n; i++) {
        Record rec;
        std::memcpy(rec.data(), &raw[kLibraryHeader + i * kRecordSize], kRecordSize);
        char name[kNameLen + 1];
        std::memcpy(name, rec.data(), kNameLen);
        name[kNameLen] = 0;
        for (char& c : name) if (c != 0 && (c < 0x20 || c > 0x7E)) c = '?';
        t.put("%u: %s id=%08X cable=%08X %s\n", i, name, static_cast<unsigned>(vdo(rec, 1)),
              static_cast<unsigned>(vdo(rec, 4)), valid(rec) ? "ok" : "BAD");
    }
    return t.size();
}

bool library_add(Library& lib, const Record& rec) {
    if (lib.count >= kLibrarySlots) return false;
    lib.rec[lib.count++] = rec;
    return true;
}

bool library_remove(Library& lib, unsigned slot) {
    if (slot >= lib.count) return false;
    for (unsigned i = slot; i + 1 < lib.count; i++) lib.rec[i] = lib.rec[i + 1];
    lib.rec[--lib.count] = Record{};
    if (lib.count == 0) lib.default_slot = 0;
    else if (lib.default_slot == slot) lib.default_slot = 0;
    else if (lib.default_slot > slot) lib.default_slot--;
    return true;
}

}  // namespace k2::emrec
