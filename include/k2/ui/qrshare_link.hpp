// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The link form of a shared record: a web address a phone camera opens, with the record after the "#". The page there
// (docs/site/r/) decodes it in the browser; the fragment never reaches a server. Pure code over spans, no display, no heap.
// The format is documented in docs/QR_SHARE.md.
//
//   <kBase><version><kind>.<base64url of the record bytes, no padding>
//
//   version  '1': the record as the K2 data frame carries it (schema 1). '2': a schema v2 record (k2/ui/qrshare_records.hpp),
//                the bit-packed form of the report card and the e-marker (cable) record, uncompressed; the same kind letter as
//                version '1'. A page that does not know the digit says so instead of guessing. Version '1' links stay valid.
//   kind     one letter per record type; the record is the same bytes the K2 data frame carries (qrshare_records.hpp):
//              c / z  report card         (c = as the store keeps it or its v2 record, z = the LZ4 block of it; shipped first,
//                                        so the pair is kept)
//              e / E  e-marker (cable)    (lower case = the record as built or its v2 record, upper case = its LZ4 block,
//                                        qrshare::pack)
//              p / P  source capabilities (PDOs)
//              i / I  firmware identity   (text "fw=.. g=.. p=..")
//              b / B  boot info           (the K2B1 text)
//              t      a plain ASCII text record (NUL padded), version '1' only, never packed
// base45 cannot be used here (its space and "%" and "$" are not URL safe); base64url costs 1.33 characters per byte.
// The text is byte-mode QR (the lower case letters of the address rule out alphanumeric mode) and must stay ONE code within the
// share view's look policy (k2/ui/qrview.hpp look_for): version 21 at level L (101 modules, 2 px each on the panel, read by a
// phone's main camera), kMaxRecord bytes at most, whatever the type. Every record the share view can hold fits (the largest is
// the 510 byte cable record); only a type without a link form (the log, several KB) has none, and the Share view then offers
// only the K2 data frames.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/ui/qr.hpp"
#include "k2/ui/qrshare.hpp"
#include "k2/ui/qrshare_records.hpp"

namespace k2::qrshare::link {

// The published viewer: GitHub Pages of the repository, folder r/. The one place the address lives.
inline constexpr std::string_view kBase = "https://yarost12.github.io/open-k2-firmware/r/#";
inline constexpr char kVersion = '1';
inline constexpr char kVersionV2 = '2';                                    // a schema v2 record (card, cable), uncompressed
inline constexpr char kKindCard = 'c', kKindCardPacked = 'z';
inline constexpr char kKindText = 't';                                     // a plain ASCII text record, version '1', never packed

constexpr std::size_t b64_len(std::size_t bytes) { return (bytes * 4 + 2) / 3; }

inline constexpr std::size_t kHead = kBase.size() + 3;                             // base, the version, the kind, '.'
inline constexpr int kMaxVersion = 21;                                             // the largest QR version a link may need (look policy)
inline constexpr std::size_t kQrBytes = 929;                                       // version 21, level L, byte mode
constexpr std::size_t max_record_for(std::size_t qr_bytes) {                       // the most record bytes whose link fits `qr_bytes`
    std::size_t n = 0;
    while (kHead + b64_len(n + 1) <= qr_bytes) n++;
    return n;
}
inline constexpr std::size_t kMaxRecord = max_record_for(kQrBytes);                // the largest record one link code carries (659)
inline constexpr std::size_t kMaxText = kHead + b64_len(kMaxRecord);               // its link: at most kQrBytes characters
// Every (version, kind) a link can carry stays ONE code within the look policy (version 21 or less): the record behind each form
// is capped at kMaxRecord (version '1', every kind, and version '2' for the cable, checked before a '2' link is emitted), and
// every record the share view can hold is under that cap, the largest being the 510 byte cable record.
static_assert(kMaxText <= kQrBytes, "the worst-case link must stay a version 21 code (101 modules, 2 px each on the panel)");
static_assert(qr::buf_len_for(kMaxVersion) >= kQrBytes, "the version 21 buffer holds a link of kQrBytes characters");
static_assert(kHead + b64_len(records::kMaxCardV2) <= kQrBytes, "a version '2' card link stays one code");
static_assert(records::kMaxCardV2 <= kMaxRecord, "the v2 card record never grows past one code's record cap");
static_assert(kHead + b64_len(kMaxRecord) <= kQrBytes, "a version '2' cable link and a kind 't' link stay one code");
static_assert(records::kMaxCard <= kMaxRecord && records::kMaxPdCaps <= kMaxRecord && records::kMaxIdentity <= kMaxRecord &&
                  records::kMaxEmarker <= kMaxRecord,
              "a card, the cable (as held, v1 with every quirk row), the source caps and the identity always have a link form");

// True for the record types that have a link form: the card, the cable, the source caps, the identity, the boot info.
bool supported(Type type);
// True for every kind letter a link can carry, 't' included.
bool supported(char kind);

// The kind letter of `type` (see the table above; upper case = `packed`), 0 for a type without a link form.
char kind_letter(Type type, bool packed);

// True when a record of `len` bytes (as the Share view holds it: packed or not, or already a v2 record) has a link form: its
// type is supported and it fits one code. A cable whose quirk block does not re-pack small enough has none.
bool fits(Type type, std::size_t len);
// The v2-aware check for a record as built: a v1 card/cable record is judged by its v2 size (packed into a local), so the
// 8-row-quirk cable (510 B raw, 189 B v2) has a link form; anything else by the length rule above.
bool fits(Type type, std::span<const std::uint8_t> record, bool packed);
// The one-code rule for a kind letter ('t' is never packed): the record fits one code (kMaxRecord).
bool fits(char kind, std::size_t len);

// base64url (RFC 4648 section 5, no padding): 'A'..'Z' 'a'..'z' '0'..'9' '-' '_'. Writes b64_len(in.size()) characters, no NUL;
// returns the count, or 0 when `out` is too small.
std::size_t base64url_encode(std::span<const std::uint8_t> in, std::span<char> out);

// The link text of `record` into `out`, NUL terminated. `packed` marks an LZ4 block made by qrshare::pack(). For the kinds with a
// schema v2 (the card, the cable) a v1 record is re-packed to v2 when that fits one code — the smaller code wins (`allow_v2`);
// a v2 record is emitted as version '2'. Returns the length, or 0 when no form fits one code or `out` is too small (kMaxText + 1
// characters always suffice).
std::size_t encode(Type type, std::span<const std::uint8_t> record, bool packed, std::span<char> out, bool allow_v2 = true);
// The link of an already v2-packed record (records::pack_v2): version '2', the plain kind letter, the record as it is.
std::size_t encode_v2(Type type, std::span<const std::uint8_t> v2record, std::span<char> out);
// The link of a kind letter directly: 't' is the plain ASCII text record at version '1' (never packed); the other letters name a
// frame type (upper case = its LZ4 block) and go through encode().
std::size_t encode_kind(char kind, std::span<const std::uint8_t> record, std::span<char> out);

}  // namespace k2::qrshare::link
