// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The record layouts the QR share system carries (k2/ui/qrshare.hpp is the frame around them; docs/QR_SHARE.md lists them).
// Each builder serialises one thing the firmware already holds into `out` and returns the length, 0 when `out` is too small
// or there is nothing to share. Pure code, host-tested (tests/test_qrshare_records.cpp); tools/k2_qr.py reads them back.
//
// The payloads reuse the existing serialisers, so a later import on the K2 calls the parsers that are already there:
//   Emarker  schema 1: flags u8 (bit0: a quirk block follows), name[24] (NUL padded, the user's name), the 66-byte
//            record (k2/pd/emrec.hpp), then the emk_quirk block (k2/pd/emk_quirk.hpp encode()) when flag bit0 is set.
//   PdCaps   schema 1: num_pdos u8, flags u8 (bit0 EPR caps, bit1 EPR chunks incomplete), spec_rev u8 (wire header: 1 PD 2.0,
//            2 PD 3.x), active_cc u8 (0 none, 1 CC1, 2 CC2), raw message header u16 LE, then num_pdos raw PDO words u32 LE by
//            position (an unused slot is 0).
//   Card     schema 1: the report card record as the store keeps it (k2/chargertest/report_card.hpp encode_card(): 0xA6, type, payload length,
//            0, the payload, CRC-16), 204 bytes (v8; 202 with v7).
//   Identity schema 1: ASCII "fw=<version> g=<git, -dirty when dirty> p=<remote proto>", the strings PING answers.
//   BootInfo schema 1: the K2B1 text (k2/core/bootinfo.hpp).
//   Log      schema 1: git_len u8, the firmware git string (git_len bytes, as `git describe --always --dirty`), flags u8 (bit0:
//            older entries were left out), base_ms u32 LE (uptime of the entry before the first one), now_ms u32 LE (uptime when
//            the record was made), then whole K2LOG entries to the end (k2/core/binlog.hpp). The host picks the id -> format map of
//            that git version (tools/logdb.json) and prints t=+<ms> lines.
//
// Schema 2 (the link form of the two big kinds, k2/ui/qrshare_link.hpp): a bit-packed re-encoding of the card and the e-marker
// records, LSB-first, varint = 7 value bits + 1 continuation bit. It drops what the v1 layout repeats or reserves (the record
// header, the unused range slots, the emrec's 24 reserved zero bytes) and keeps the v1 card's CRC-16 at the end, so the host
// integrity check is unchanged. Every field of a v2 record is at most as wide as in v1, so packing in place is always safe.
//   Card v2:  schema u8 = 2; fp u32, vid u16, pid u16; varint card_no, runs, boots, uptime_s; fw u32; varint handover_ds;
//             spec_rev u2, num_pdos u4, handover u3, state u4, reason u6; n_ranges u3, ev_flags u8; name_len u5 (<= 20), the
//             name as u7 chars; pdp_w u8, name_auto u1, pd_ma50 u7; 12 groups of (verdict u2, reason u6, detail u6, has_max u1
//             [+ max_dv u9], has_fail u1 [+ fail_dv u9]); n_ranges ranges of (alg u5, pos u8, lo_dv u9, hi_dv u9, max_dv u9,
//             fail_dv u9, ma50 u7); 3 EPR slots of (dv code u2: 0 none, 1 = 280, 2 = 360, 3 = 480, then a group result);
//             cable_present u1 [+ varint seq, vid u16, pid u16, max_mv u16, max_ma u16, kind code u3 (0 = 0, 1 = 1, 2 = 2,
//             3 = 4, 4 = 0xFF), flags u8, resumed u8, src_flags u8]; n_ufcs u3, that many modes of (lo_dv u8, hi_dv u9, ma50 u7);
//             varint times [+ last_boots u16, last_uptime_s u24 when times > 1]; varint rebalanced (v8: the source's mid-run
//             re-balance count, 0 on a v7 record); crc16 u16 of the v1 record.
//   Emarker v2: schema u8 = 2; name_len u5 (<= 24), the name as u7 chars; flags u8; the emrec: name_len u5 (<= 20), the name as
//             u7 chars, spec_rev u2, five VDOs u32, checksum u8; has_quirks u1 [+ flags u2, unmatched u2, count u4, then count
//             rows of (sop u2, kind u2, nreq u2, nrep u3, mask_is_kSvdmMask u1, hdr_val u32, hdr_mask u32 when the mask is not
//             kSvdmMask, nreq x (req_val u32, req_mask u32), nrep x rep u32)].
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/core/bootinfo.hpp"
#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/pd.hpp"
#include "k2/chargertest/report_card.hpp"
#include "k2/ui/qrshare.hpp"

namespace k2::qrshare::records {

inline constexpr std::size_t kEmarkerName = 24;                                    // the saved cable's name (emlib::kNameMax)
inline constexpr std::size_t kEmarkerHead = 1 + kEmarkerName + emrec::kRecordSize; // 91
inline constexpr std::size_t kMaxEmarker = kEmarkerHead + emk_quirk::kMaxBlock;    // 510: two parts
inline constexpr std::uint8_t kEmarkerQuirks = 1;                                  // flags bit0

inline constexpr std::size_t kPdCapsHead = 6;
inline constexpr std::size_t kMaxPdCaps = kPdCapsHead + 4 * pd::kMaxPdos;          // 50
inline constexpr std::uint8_t kPdCapsEpr = 1, kPdCapsEprPartial = 2;               // flags

inline constexpr std::size_t kMaxCard = report_card::kMaxRecord;                   // 202
inline constexpr std::size_t kMaxIdentity = 64;

inline constexpr std::size_t kLogGit = 39;
inline constexpr std::size_t kLogHeadMax = 1 + kLogGit + 1 + 4 + 4;                 // 49
inline constexpr std::uint8_t kLogOlder = 1;                                        // flags bit0

std::size_t emarker(const emrec::Record& rec, std::string_view name, const emk_quirk::Quirks& quirks, std::span<std::uint8_t> out);
std::size_t pdcaps(const pd::SourceCaps& caps, std::span<std::uint8_t> out);        // 0 when caps.valid is false
std::size_t card(const report_card::Card& c, std::span<std::uint8_t> out);
std::size_t identity(std::string_view fw, std::string_view git, unsigned proto, std::span<std::uint8_t> out);
std::size_t boot(const bootinfo::Snapshot& s, std::span<std::uint8_t> out);
// The caller puts `entries` bytes of K2LOG entries (usb::log_bin) at out[kLogHeadMax...); this writes the head in front and
// moves the entries down behind it, so the record needs no second buffer. 0 when there are no entries or they do not fit.
std::size_t log(std::string_view git, std::uint32_t now_ms, std::uint32_t base_ms, bool older, std::size_t entries, std::span<std::uint8_t> out);

// ---- schema v2 (the bit-packed form of the card and the e-marker record, carried by version '2' links) ----

inline constexpr std::uint8_t kSchemaV2 = 2;

// The types that have a schema v2: the report card and the e-marker (cable) record.
constexpr bool has_v2(Type type) { return type == Type::Card || type == Type::Emarker; }

// True when `rec` is a well-formed schema 1 record of a v2 kind (a 202-byte format-7 card record, or an e-marker record whose
// quirk block fits its row count): only such records are re-packed to v2.
bool valid_v1(Type type, std::span<const std::uint8_t> rec);

// Packs the v1 record `in` into the v2 record in `out` (LSB-first bit stream, see the header). Returns the v2 length, or 0 when
// `in` is not a valid v1 record of a v2 kind or `out` is too small. `out` may alias `in` (in-place packing; a local copy makes
// that safe), and the v2 record is never longer than the v1 one.
std::size_t pack_v2(Type type, std::span<const std::uint8_t> in, std::span<std::uint8_t> out);
// The same, packing `rec` into its own front: the share record buffer holds the v2 record afterwards. Returns the new length.
std::size_t pack_v2_in_place(Type type, std::span<std::uint8_t> rec);

// Worst-case v2 sizes (every field at its widest), for the one-code asserts in qrshare_link.hpp.
consteval std::size_t card_v2_worst_bits() {
    std::size_t b = 8 + 32 + 16 + 16;                       // schema, fp, vid, pid
    b += 24 + 24 + 40 + 40 + 32 + 24;                       // varint card_no, runs, boots, uptime_s, fw, handover_ds
    b += 2 + 4 + 3 + 4 + 6 + 3 + 8;                         // spec_rev, num_pdos, handover, state, reason, n_ranges, ev_flags
    b += 5 + 20 * 7 + 8 + 1 + 7;                            // name, pdp_w, name_auto, pd_ma50
    b += 12 * (2 + 6 + 6 + 1 + 9 + 1 + 9);                  // 12 groups, both dv present
    b += report_card::kMaxRanges * (5 + 8 + 9 + 9 + 9 + 9 + 7);          // ranges
    b += 3 * (2 + 2 + 6 + 6 + 1 + 9 + 1 + 9);               // EPR slots
    b += 1 + 40 + 4 * 16 + 3 + 3 * 8;                       // cable (seq varint worst, kind code, flags/resumed/src_flags)
    b += 3 + 4 * (8 + 9 + 7);                               // n_ufcs + modes
    b += 16 + 16 + 24 + 16;                                 // times varint, last_boots, last_uptime_s, crc16
    b += 16;                                                // the v8 rebalanced varint (worst case 2 bytes)
    return b;                                               // 1506 bits
}
consteval std::size_t emk_v2_worst_bits() {
    std::size_t b = 8 + 5 + 24 * 7 + 8;                     // schema, name, flags
    b += 5 + 20 * 7 + 2 + 5 * 32 + 8;                       // the emrec (no reserved bytes)
    b += 1 + 2 + 2 + 4;                                     // has_quirks, flags, unmatched, count
    b += emk_quirk::kMaxRows * (2 + 2 + 2 + 3 + 1 + 32 + 32 + 2 * 64 + 6 * 32);   // rows, full mask
    return b;                                               // 3665 bits
}
inline constexpr std::size_t kMaxCardV2 = (card_v2_worst_bits() + 7) / 8;   // 189 B
inline constexpr std::size_t kMaxEmkV2 = (emk_v2_worst_bits() + 7) / 8;     // 459 B
static_assert(kMaxCardV2 == 189 && kMaxEmkV2 == 459);

}  // namespace k2::qrshare::records
