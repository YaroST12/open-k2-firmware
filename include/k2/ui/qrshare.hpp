// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR share framework: one way to put any K2 record (boot info, an e-marker clone, source capabilities, a report card,
// the identity) into one or more QR codes, and to read the parts back (host tools today, an import on the K2 later).
// Pure code over spans; no display, no heap. The format is documented in docs/QR_SHARE.md and decoded by tools/k2_qr.py.
//
// A record is a byte string owned by its type (the schema number says which layout): the payload reuses the serialisers the
// firmware already has (emrec / emk_quirk blocks, the report card record, raw PDO words), so a later import calls the
// same parsers. A record longer than one QR holds is cut into n parts of equal size (kPartPayload at most); every part is
// self-describing, so the parts may be scanned in any order.
//
// Binary frame of one part (little endian):
//   +0  'K' '2'          magic
//   +2  type             Type
//   +3  schema           record layout version of that type
//   +4  index            part number, 0..count-1
//   +5  count            parts of this record, 1..kMaxParts
//   +6  record_len u16   bytes of the whole record
//   +8  record_crc u32   CRC-32 (zlib) of the whole record
//   +12 payload          this part's slice of the record: slice i starts at i * ceil(record_len / count)
//   +n  frame_crc u32    CRC-32 of everything before it (header + payload)
// The type byte's bit 7 (kPackedFlag) says the record is an LZ4 block (k2/core/lz.hpp, the standard block format): the record_len,
// record_crc and the split are over the bytes sent, so the K2 side needs no decompressor; the reader joins the parts and
// then decompresses. A plain record never sets it, so every older frame stays valid. pack() decides when it pays.
// Text of one part, which is what the QR carries: "K2:" + tag + schema + ":" + base45(frame), e.g. "K2:EMK1:...". Every
// character is in the QR alphanumeric set (0-9 A-Z space $ % * + - . / :), so the symbol is an alphanumeric segment and a
// phone shows the prefix. base45 costs 1.5 characters per byte at 5.5 bits each: 3 % more bits than byte mode, not fewer,
// but it is plain text a phone can copy and a decoder can take from any scanner.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace k2::qrshare {

enum class Type : std::uint8_t {
    BootInfo = 1,    // the K2B1 text line (k2/core/bootinfo.hpp)
    Emarker = 2,     // flags byte, emrec record (66 B), optional emk_quirk block
    PdCaps = 3,      // 6-byte header, then the raw 32-bit PDO words
    Card = 4,        // the report card record as stored (k2/chargertest/report_card.hpp encode_card)
    Identity = 5,    // text "fw=<version> g=<git> p=<proto>"
    Log = 6,         // the newest binary log entries (k2/core/binlog.hpp), see qrshare_records.hpp
    LoopProbe = 7,   // the HID-cable loop probe result (k2/loopprobe/loop_probe.hpp encode(), 399 bytes)
    Battery = 8,     // one battery recorder run (k2/battery/battery.hpp encode(), 246 bytes)
    // 9..127 are free; bit 7 of the byte is kPackedFlag
};

inline constexpr std::uint8_t kSchemaBootInfo = 1, kSchemaEmarker = 1, kSchemaPdCaps = 1, kSchemaCard = 1, kSchemaIdentity = 1,
                              kSchemaLog = 1, kSchemaLoop = 1, kSchemaBattery = 1;
inline constexpr std::uint8_t kPackedFlag = 0x80;

std::string_view tag(Type t);                          // "BI", "EMK", "PDC", "CARD", "ID", "LOG", "LOOP", "BATT"; "" for a type this build does not know
bool type_from_tag(std::string_view tag, Type& out);

inline constexpr std::size_t kHeaderLen = 12;
inline constexpr std::size_t kCrcLen = 4;
inline constexpr std::size_t kPartPayload = 330;       // the largest slice: the frame then fits version 12 at level L (535 characters)
inline constexpr unsigned kMaxParts = 4;
inline constexpr std::size_t kMaxRecord = kPartPayload * kMaxParts;
inline constexpr std::size_t kMaxFrame = kHeaderLen + kPartPayload + kCrcLen;   // 346
inline constexpr std::size_t kMaxTextPrefix = 3 + 4 + 3 + 1;                   // "K2:" tag schema ":"
inline constexpr std::size_t kMaxText = kMaxTextPrefix + (kMaxFrame / 2) * 3 + 2;   // 529 characters
inline constexpr std::size_t kTextBuf = kMaxText + 1;                          // with the NUL

// CRC-32 (zlib / IEEE 802.3): crc32(a + b) == crc32(b, crc32(a)).
std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc = 0);

// base45 (RFC 9285 alphabet, which is the QR alphanumeric set). Encode: two bytes -> three characters, a last single byte ->
// two; no NUL written. Returns the characters, or 0 when `out` is too small. Decode: false on a character outside the
// alphabet, a bad length, a group above 0xFFFF or `out` too small.
std::size_t base45_len(std::size_t bytes);
std::size_t base45_encode(std::span<const std::uint8_t> in, std::span<char> out);
bool base45_decode(std::string_view in, std::span<std::uint8_t> out, std::size_t& n);

// Parts a record of `record_len` bytes needs: 0 for an empty record or one over kMaxRecord.
unsigned part_count(std::size_t record_len);
// Bytes of slice `index` of such a record (0 when out of range).
std::size_t slice_len(std::size_t record_len, unsigned index);

// The text of part `index` of `record` into `out` (kTextBuf is always enough), NUL terminated. Returns its length, 0 for an
// empty or too long record, an index past the parts, a type without a tag, or an `out` that is too small. `packed` sets
// kPackedFlag: `record` is then an LZ4 block made by pack().
std::size_t encode_part(Type type, std::uint8_t schema, std::span<const std::uint8_t> record, unsigned index,
                        std::span<char> out, bool packed = false);

// LZ4-compresses `record` into `out` (lz::bound(record.size()) bytes always suffice) when that takes at most three
// quarters of it; the length, or 0 when it does not pay, the record is empty or `out` is too small. The 25 % rule keeps
// the flag for records that gain a smaller code or one part fewer; a boot line or a PDO list stays plain.
std::size_t pack(std::span<const std::uint8_t> record, std::span<std::uint8_t> out);

struct Part {
    Type type = Type::BootInfo;
    bool packed = false;                        // kPackedFlag was set
    std::uint8_t schema = 0;
    std::uint8_t index = 0, count = 0;
    std::uint16_t record_len = 0;
    std::uint32_t record_crc = 0;
    std::span<const std::uint8_t> payload;      // points into the buffer given to parse_part()
};
// Reads one part's text. `buf` (kMaxFrame bytes) holds the decoded frame and `out.payload` points into it. Checks the
// prefix against the frame (tag, schema), the magic, both CRCs, and that the slice has the length the header implies.
bool parse_part(std::string_view text, Part& out, std::span<std::uint8_t> buf);

// Collects the parts of one record, in any order, into the caller's buffer.
class Assembler {
public:
    enum class Result : std::uint8_t {
        Accepted,    // a new part stored, more to come
        Done,        // the record is complete and its CRC matches
        Duplicate,   // this part was already stored
        Bad,         // not a part (prefix, base45, magic, CRC, slice length), or the finished record failed its CRC
        Mismatch,    // a part of a different record (type, schema, count, length or CRC differ from the first)
        TooBig,      // the record does not fit the buffer
    };
    explicit Assembler(std::span<std::uint8_t> record_buf) : buf_(record_buf) {}

    Result feed(std::string_view text);
    void reset() { seen_ = 0; have_ = false; }
    bool started() const { return have_; }
    bool complete() const { return have_ && seen_ == full_mask(); }
    unsigned parts_seen() const;
    unsigned count() const { return count_; }
    Type type() const { return type_; }
    bool packed() const { return packed_; }      // record() is an LZ4 block
    std::uint8_t schema() const { return schema_; }
    std::span<const std::uint8_t> record() const { return complete() ? buf_.first(len_) : std::span<const std::uint8_t>{}; }

private:
    unsigned full_mask() const { return (1u << count_) - 1u; }
    std::span<std::uint8_t> buf_;
    std::uint8_t frame_[kMaxFrame] = {};
    unsigned seen_ = 0;
    bool have_ = false, packed_ = false;
    Type type_ = Type::BootInfo;
    std::uint8_t schema_ = 0, count_ = 0;
    std::uint16_t len_ = 0;
    std::uint32_t crc_ = 0;
};

}  // namespace k2::qrshare
