// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// E-marker records and the library page, the flash format this firmware stores and reads, pure logic.
//
// A record is 66 bytes: name[20], pd_spec_rev, five VDOs (VDM header, ID header, cert stat,
// product, cable VDO 1; little endian), 24 reserved zero bytes, checksum (sum of bytes 0..0x40
// plus 0xA5). The library is one flash page: 4 header bytes, default slot, num_records (1..10),
// the records back to back, and a table checksum (sum of everything before it plus 0xA5).
// This firmware writes kLibraryMagic in the header bytes. A page of the same layout with any
// other header bytes (the native library page) is read with parse_native_library().
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace k2::emrec {

inline constexpr std::size_t kRecordSize = 66;
inline constexpr std::size_t kNameLen = 20;
inline constexpr std::size_t kLibrarySlots = 10;
inline constexpr std::size_t kLibraryHeader = 6;                       // magic(4), default, num_records
inline constexpr std::size_t kLibraryMaxSize = kLibraryHeader + kLibrarySlots * kRecordSize + 1;
inline constexpr std::array<std::uint8_t, 4> kLibraryMagic{'K', '2', 'E', 'M'};
inline constexpr std::string_view kCopiedName = "[copied emark]";

using Record = std::array<std::uint8_t, kRecordSize>;

// (sum of bytes 0..0x40 + 0xA5) & 0xFF
std::uint8_t checksum(const Record& rec);

// Build a record from the first five VDOs (fewer than five are zero filled). The name is cut to
// 19 characters so it stays NUL terminated.
Record make(std::span<const std::uint32_t> vdos, std::uint8_t spec_rev, std::string_view name = kCopiedName);

// Built-in passive-cable identities for the e-marker spoof, always available whatever is saved. The table is the one
// place that lists them (src/pd/emrec.cpp). Id 0 is a generic 5 A / 20 V USB 3.2 Gen2 cable; ids 1, 2 and 4 are identities read
// from real cables (Discover Identity ACK on SOP', PD 3.x, VDO v0), so every word is byte exact:
//   1 EPR 50V 5A        0x2B01:0x4051  the hardware-proven set (EPR 28 V on the Baseus 140 W); a generated EPR identity
//                       made that charger hard-reset in EPR
//   2 CUKTECH 6A        0x2B01:0x4051  the cable bundled with the CUKTECH 10 / 120 W charger, 50 V 5 A EPR, USB 2.0
//   4 OnePlus 100W      0x22D9:0x6116  C-to-C, 30 V 5 A, VCONN required, no certification id
// Id 3 was "OnePlus 100W (mag)" (0x22D9:0x1196), a duplicate of id 4 (same VID, same ratings) and is retired. Ids are on the
// wire and in stored data (remote spoof / cable choice, report card kind, /cable.cfg), so the numbering stays: id 3 is a
// reserved alias that reads as id 4 (canonical_builtin_id()), and no table row or UI row carries it any more.
// Always valid().
inline constexpr unsigned kBuiltinCount = 5;        // the id space 0..4 (id 3 is the retired alias)
inline constexpr unsigned kRetiredMagId = 3;
inline constexpr unsigned kOnePlusId = 4;

// The id a stored or remote `id` stands for: the retired id 3 is OnePlus 100W (4), every other id is itself (an id past the
// table is the caller's to refuse, builtin_info() reads it as the generic cable).
unsigned canonical_builtin_id(unsigned id);

struct BuiltinInfo {
    std::string_view record_name;   // the record's name field, "[...]" marks a built-in (at most 19 characters)
    std::string_view label;         // user-facing name, at most 24 characters (the remote name field)
    std::string_view detail;        // one line for the dialog's detail row, at most 26 characters
    std::array<std::uint32_t, 5> vdos;   // VDM header, ID header, cert stat, product, cable VDO
    std::uint8_t spec_rev;
};

// The table row of `id`; an id past the table is the generic cable (row 0).
const BuiltinInfo& builtin_info(unsigned id);

Record builtin_cable(unsigned id);

// The record validator: pd_spec_rev 1 or 2, (vdm_header & 0xFF008000) == 0xFF008000, checksum.
bool valid(std::span<const std::uint8_t> rec);

// Known wire revision 1 (PD2.0) or 2 (PD3.x); unknown/reserved returns invalid 0.
// Saving a cable must use the cable answer's revision, never the source header.
std::uint8_t spec_rev_from_pd(std::uint8_t wire_rev);

std::uint32_t vdo(const Record& rec, unsigned index);                  // index 0..4 (0x15 + 4 * index)

// Human-readable decode, one "key: value" per line. Returns the length; cut off when `out` is small.
std::size_t describe(const Record& rec, std::span<char> out);

struct Library {
    std::uint8_t default_slot = 0;
    std::uint8_t count = 0;                  // records in use; slots 0..count-1 (the format has no gaps)
    std::array<Record, kLibrarySlots> rec{};
};

// Serialise into `out` (kLibraryMaxSize bytes are always enough); returns the length, 0 if it does
// not fit or the library is empty (an empty library is an erased page, not a table).
std::size_t encode_library(const Library& lib, std::span<std::uint8_t> out);

// Parse and validate a library page; false (and `out` untouched) for an erased or damaged table.
bool decode_library(std::span<const std::uint8_t> raw, Library& out);
// The same verdict as decode_library() without a Library to fill (callers that only need to know).
bool library_valid(std::span<const std::uint8_t> raw);

// The native library page, read leniently and never
// written: any 4 header bytes, default slot at +4, num_records at +5 (1..10), records from +6,
// table checksum after the last record. Only the structure is required (count in range and the
// table fitting the page); the checksum and each record are reported, not enforced.
struct NativeLibrary {
    std::array<std::uint8_t, 4> header{};
    std::uint8_t default_slot = 0;
    std::uint8_t count = 0;
    std::uint8_t checksum = 0;               // the byte stored after the last record
    bool checksum_ok = false;                // equals (sum of everything before it + 0xA5)
    std::uint16_t valid_mask = 0;            // bit i set when record i passes valid()
    std::array<Record, kLibrarySlots> rec{};
};

bool parse_native_library(std::span<const std::uint8_t> raw, NativeLibrary& out);
// Record count when parse_native_library() would accept `raw`, else 0 (no NativeLibrary to fill).
unsigned native_library_count(std::span<const std::uint8_t> raw);

// Text listing: header bytes, default, count, checksum verdict, then one line per record.
std::size_t describe_native_library(const NativeLibrary& lib, std::span<char> out);
// The same text straight from the raw page, one record at a time: no NativeLibrary (~700 B) on the stack.
// Returns 0 when parse_native_library() would refuse the page.
std::size_t describe_native_page(std::span<const std::uint8_t> raw, std::span<char> out);

bool library_add(Library& lib, const Record& rec);                    // next free slot; false when full
bool library_remove(Library& lib, unsigned slot);                     // later slots shift down

}  // namespace k2::emrec
