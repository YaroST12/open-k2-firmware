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
//            0, the payload, CRC-16), 202 bytes.
//   Identity schema 1: ASCII "fw=<version> g=<git, -dirty when dirty> p=<remote proto>", the strings PING answers.
//   BootInfo schema 1: the K2B1 text (k2/core/bootinfo.hpp).
//   Log      schema 1: git_len u8, the firmware git string (git_len bytes, as `git describe --always --dirty`), flags u8 (bit0:
//            older entries were left out), base_ms u32 LE (uptime of the entry before the first one), now_ms u32 LE (uptime when
//            the record was made), then whole K2LOG entries to the end (k2/core/binlog.hpp). The host picks the id -> format map of
//            that git version (tools/logdb.json) and prints t=+<ms> lines.
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

}  // namespace k2::qrshare::records
