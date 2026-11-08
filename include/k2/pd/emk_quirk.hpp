// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// E-marker response quirks: what a faked cable answers beyond its Discover Identity (pure data and logic).
//
// A fake cable (k2::emk_spoof) is an identity record (k2::emrec, answered to Discover Identity) plus a Quirks
// table: ordered response rows, a default for requests no row matches, and a few behaviour flags. Real cables
// differ in what else they answer (an OPPO/OnePlus brick asks its cable a vendor command and settles on a lower
// power if the cable stays silent), so this is the one place that describes a cable's extra behaviour, in the
// manner of a Linux kernel quirk table: one constexpr row per behaviour, matched in order, first match wins.
//
// A row matches a received message by SOP kind (SOP' and/or SOP''), by the Vendor_Defined_Message header under a
// mask (SVID, structured/unstructured, command type, command) and optionally by the data objects after the header
// under masks. Its reply is built for the request: an ACK, NAK or BUSY keeps the request's SVID, VDM version and
// object position and carries the stored VDOs; a Raw reply sends stored words as they are (header included).
//
// Everything here is verified by host tests (tests/test_emk_quirk.cpp); the wire side is k2::emk_spoof.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::emk_quirk {

inline constexpr unsigned kMaxRows = 8;
inline constexpr unsigned kMaxReqVdos = 2;      // request data objects a row may constrain (after the VDM header)
inline constexpr unsigned kMaxWords = 6;        // reply words per row (VDOs after the header, or header + VDOs for Raw)

// Row SOP mask bits.
inline constexpr std::uint8_t kSopP = 1, kSopPP = 2;

// How a row's reply is built.
enum class Kind : std::uint8_t {
    Ack,    // VDM ACK for the request, then `nrep` stored VDOs
    Nak,    // VDM NAK for the request, no VDOs
    Busy,   // VDM BUSY for the request, no VDOs
    Raw,    // the stored words verbatim: word 0 is the whole VDM header, then `nrep - 1` VDOs
};

// What a structured request that no row (and not Discover Identity) matches gets.
enum class Unmatched : std::uint8_t {
    Ignore,         // silence; the source's GoodCRC was already sent by the chip (the proven default)
    Nak,            // VDM NAK (a Structured-VDM responder that does not know the command)
    NotSupported,   // Not_Supported control message (PD 3.x)
};

// Identity-level behaviour flags.
inline constexpr std::uint8_t kQSopPP = 1;        // the identity is also answered on SOP'' (needs the SOP'' receive path)
inline constexpr std::uint8_t kQMsgIdEcho = 2;    // replies carry the request's MessageID instead of the cable's counter
inline constexpr std::uint8_t kQFlagsKnown = kQSopPP | kQMsgIdEcho;

// VDM header bits that identify a structured request: SVID, structured bit, command type, command.
inline constexpr std::uint32_t kSvdmMask = 0xFFFF8000u | (3u << 6) | 0x1Fu;
constexpr std::uint32_t svdm_req(std::uint16_t svid, std::uint8_t cmd) {
    return (std::uint32_t(svid) << 16) | 0x8000u | (cmd & 0x1Fu);
}

struct Row {
    // 0 = unset (not a valid mask: decode/validate reject it). Every real row sets it (svdm_row defaults to kSopP; decode reads it),
    // and keeping the default all-zero lets a default Quirks live in .bss instead of a flash-backed 420 B .data image.
    std::uint8_t sop = 0;
    Kind kind = Kind::Ack;
    std::uint8_t nreq = 0;                          // request data objects that must be present and match (0..kMaxReqVdos)
    std::uint8_t nrep = 0;                          // stored reply words used
    std::uint32_t hdr_val = 0, hdr_mask = 0;        // (VDM header & hdr_mask) == hdr_val
    std::array<std::uint32_t, kMaxReqVdos> req_val{}, req_mask{};
    std::array<std::uint32_t, kMaxWords> rep{};

    constexpr bool operator==(const Row&) const = default;
};

struct Quirks {
    std::uint8_t flags = 0;
    Unmatched unmatched = Unmatched::Ignore;
    std::uint8_t count = 0;
    std::array<Row, kMaxRows> rows{};

    constexpr bool empty() const { return flags == 0 && unmatched == Unmatched::Ignore && count == 0; }
    constexpr bool operator==(const Quirks& o) const {
        if (flags != o.flags || unmatched != o.unmatched || count != o.count) return false;
        for (unsigned i = 0; i < count && i < kMaxRows; i++)
            if (!(rows[i] == o.rows[i])) return false;
        return true;
    }
};

// A structured-request row: SVID + command under kSvdmMask, answered with kind and VDOs.
constexpr Row svdm_row(std::uint16_t svid, std::uint8_t cmd, Kind kind, std::span<const std::uint32_t> words = {},
                       std::uint8_t sop = kSopP) {
    Row r;
    r.sop = sop;
    r.kind = kind;
    r.hdr_val = svdm_req(svid, cmd);
    r.hdr_mask = kSvdmMask;
    r.nrep = static_cast<std::uint8_t>(words.size() > kMaxWords ? kMaxWords : words.size());
    for (std::size_t i = 0; i < r.nrep; i++) r.rep[i] = words[i];
    return r;
}

// Row appended; false when the table is full.
constexpr bool add_row(Quirks& q, const Row& r) {
    if (q.count >= kMaxRows) return false;
    q.rows[q.count++] = r;
    return true;
}

// ---- storage form (the extension block of a saved clone, k2/pd/emlib.hpp) ----
// +0 flags, +1 unmatched, +2 count, then `count` rows of kRowBytes:
//   sop, kind, nreq, nrep (u8 each), hdr_val, hdr_mask, req_val[2], req_mask[2], rep[6] (u32 little endian).
inline constexpr std::size_t kRowBytes = 4 + 4 * (2 + 2 * kMaxReqVdos + kMaxWords);   // 52
inline constexpr std::size_t kBlockHeader = 3;
inline constexpr std::size_t kMaxBlock = kBlockHeader + kMaxRows * kRowBytes;

constexpr std::size_t block_size(unsigned count) { return kBlockHeader + count * kRowBytes; }

// Serialises into `out`; returns the length, 0 when `out` is too small.
std::size_t encode(const Quirks& q, std::span<std::uint8_t> out);
// Parses and validates a block (exact length); false and `out` untouched on any structural fault.
bool decode(std::span<const std::uint8_t> raw, Quirks& out);

// The extra behaviour of built-in identity `id` (k2/pd/emrec.hpp builtin ids); an id without extras is empty.
const Quirks& builtin_quirks(unsigned id);

// ---- unmatched-request log ----
// The last few SOP'/SOP'' messages a fake cable received and did not answer by a row or its identity, newest
// first, for `emarker unmatched` (the way to learn what a source asks beyond Discover Identity).
struct Seen {
    std::uint8_t sop = 0;                        // kSopP or kSopPP
    std::uint8_t n = 0;                          // data objects
    std::uint16_t header = 0;                    // PD message header
    std::array<std::uint32_t, 7> w{};            // data objects, VDM header first
};

class SeenLog {
public:
    static constexpr unsigned kDepth = 4;

    void clear() { total_ = 0; }
    void push(std::uint8_t sop, std::uint16_t header, std::span<const std::uint8_t> payload) {
        Seen& s = ring_[total_ % kDepth];
        s = {};
        s.sop = sop;
        s.header = header;
        const unsigned n = static_cast<unsigned>(payload.size() / 4 > 7 ? 7 : payload.size() / 4);
        s.n = static_cast<std::uint8_t>(n);
        for (unsigned i = 0; i < n; i++)
            s.w[i] = payload[4 * i] | (payload[4 * i + 1] << 8) | (payload[4 * i + 2] << 16) | (std::uint32_t(payload[4 * i + 3]) << 24);
        total_++;
    }
    std::uint32_t total() const { return total_; }                        // pushed since clear()
    unsigned size() const { return total_ < kDepth ? total_ : kDepth; }   // entries held
    // 0 = newest. False when `i` is past the held entries.
    bool get(unsigned i, Seen& out) const {
        if (i >= size()) return false;
        out = ring_[(total_ - 1 - i) % kDepth];
        return true;
    }

private:
    std::array<Seen, kDepth> ring_{};
    std::uint32_t total_ = 0;
};

// Remote form (Cmd::EmLib unmatched op). `index` kSeenTotals: [err, total u16 (saturated), held]. An entry index
// (0 = newest): the same four bytes, then sop, n, header u16 and the words (u32 little endian, VDM header first).
// An index past the held entries: the one byte [3]. Returns 0 when `out` is too small.
inline constexpr std::uint8_t kSeenTotals = 255;
inline constexpr std::size_t kSeenMax = 8 + 4 * 7;
std::size_t encode_seen(const SeenLog& log, unsigned index, std::span<std::uint8_t> out);

}  // namespace k2::emk_quirk
