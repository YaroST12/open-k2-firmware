// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Deep extension of the report card (card format v11): what a Deep run knows that the 250-byte card record has no room for,
// carried as one bit-packed blob that travels with the card (the store keeps it in the card's own ring record, the share link
// appends it to the schema v2 stream). Pure: values and buffers, no flash, no UI (host-tested by tests/test_report_card_deep.cpp).
//
// The blob is `u16 nbits` (little endian) and then ceil(nbits / 8) bytes of an LSB-first bit stream:
//   header   has_spec u1 | has_svooc u1 | has_sweep[range 0..3] u1 each                               6 bits
//   svooc    end u3 | why u4 | asks u3 | word u8 (100 mA per count, saturating) | vmax u9 (100 mV) | stop u10 (100 ms)   37 bits
//   spec     end u3 | why u6 | unconfirmed u1 | cells u5 (= specsec::kCells), then per cell in specsec::kOrder:
//              verdict u3 (pdspec::Verdict); a Skip cell adds the skip reason u4; a Pass / Fail / Unverified cell adds the
//              worst value s13 (the cell's quantised value saturated to +-4094, -4096 = event never seen); a Fail cell adds
//              fails u8, then the place bytes a u8 and b u8 (pdspec::Agg a / b: "At" transition of the worst measurement)
//   sweep    per range i set in has_sweep, card order: lo u9 (100 mV, setpoint of point 0) | n u10 (points of the planned span,
//            100 mV apart) | scale u3 | then buckets = min(kBuckets, n) of 16 bits: mean s8 | down u4 | up u4
//
// A sweep bucket covers the setpoint indices [b * n / buckets, (b + 1) * n / buckets) of the planned span. Its mean error is
// the mean of the recorded points in it, in units of E = 2^scale mV (the smallest scale at which no bucket saturates, kMaxScale
// at worst), clamped to +-127 with -128 = no point recorded there (an early stop or a gap). down / up reach the bucket's min and
// max from the mean in units of E, rounded outward (so the band always covers the data), at most 15. A PPS sweep split at 5 V
// is stitched in setpoint order; the viewer draws the mean curve with the min..max band.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/chargertest/report_card.hpp"
#include "k2/chargertest/spec_section.hpp"
#include "k2/chargertest/sweep.hpp"

namespace k2::report_card::deep {

inline constexpr unsigned kBuckets = 32;            // buckets per range graph (the worst Deep card sets the number, see worst_bits())
inline constexpr unsigned kBucketBits = 16;
inline constexpr unsigned kMaxScale = 7;            // E = 2^scale mV: 1 .. 128 mV per unit, +-127 units = +-16 V
inline constexpr std::uint8_t kEmptyMean = 0x80;    // s8 -128: no recorded point in the bucket
inline constexpr unsigned kSpanBits = 10;           // n of a range: the sweep record holds at most ctsweep::kMaxPoints
static_assert(ctsweep::kMaxPoints < (1u << kSpanBits));
static_assert(specsec::kCells == 27, "the spec block carries kCells on the wire: a changed check list needs a reviewed bump");

// Wire widths (see the header comment).
inline constexpr unsigned kHdrBits = 2 + kMaxRanges;
inline constexpr unsigned kSvoocBits = 3 + 4 + 3 + 8 + 9 + 10;
inline constexpr unsigned kSpecHeadBits = 3 + 6 + 1 + 5;
inline constexpr unsigned kCellFailBits = 3 + 13 + 8 + 8 + 8;
inline constexpr unsigned kRangeHeadBits = 9 + kSpanBits + 3;
inline constexpr unsigned kRangeBits = kRangeHeadBits + kBuckets * kBucketBits;
inline constexpr std::int16_t kValueMax = 4094, kValueNever = -4096;   // the s13 value: saturation and the "never seen" mark

consteval unsigned worst_bits() {
    return kHdrBits + kSvoocBits + kSpecHeadBits + unsigned(specsec::kCells) * kCellFailBits + kMaxRanges * kRangeBits;
}
inline constexpr unsigned kMaxBits = worst_bits();                       // every cell failing, four full sweeps: 3273
inline constexpr std::size_t kMaxBlob = 2 + (kMaxBits + 7) / 8;           // the blob with its length: 412 B
static_assert(kMaxBits < (1u << 12), "the link stream carries the block length in 12 bits");

// The blob's bit count (its first two bytes), 0 for anything shorter.
constexpr unsigned blob_bits(std::span<const std::uint8_t> blob) {
    return blob.size() < 2 ? 0u : unsigned(blob[0]) | unsigned(blob[1]) << 8;
}
// Whole and consistent: the length matches the bit count and the count is within the worst case.
constexpr bool blob_valid(std::span<const std::uint8_t> blob) {
    const unsigned n = blob_bits(blob);
    return n != 0 && n <= kMaxBits && blob.size() == 2 + (n + 7) / 8;
}

// ---- the sweep bucket reducer ----

// One recorded part of a sweep: the error of point i is err[i] (mV), its setpoint lo_mv + i * ctsweep::kStepMv.
struct Seg {
    std::span<const std::int16_t> err{};
    unsigned lo_mv = 0;
};
struct Bucket {
    std::uint8_t mean = kEmptyMean;   // s8 in units of E (kEmptyMean = no point)
    std::uint8_t down = 0, up = 0;    // 0..15 units of E below / above the mean
    bool saturated = false;           // the mean or a spread did not fit at this scale
};
constexpr unsigned bucket_count(unsigned n) { return n < kBuckets ? n : kBuckets; }
// Bucket `i` of a span of `n` points starting at `lo_mv`, from the two parts of the record (the low part below 5 V and the part
// from 5 V up, either may be empty), at scale `scale`. Setpoint indices the parts do not cover stay empty.
Bucket bucket(const Seg& a, const Seg& b, unsigned lo_mv, unsigned n, unsigned i, unsigned scale);
// The smallest scale at which no bucket of the span saturates (kMaxScale when none does).
unsigned choose_scale(const Seg& a, const Seg& b, unsigned lo_mv, unsigned n);

// ---- the blob ----

// Builds the Deep blob of a finished Deep run: the spec section, the SVOOC ceiling stage and the error graph of each range of
// `c` that has a sweep, from the runner's evidence and sweep record. Returns the blob's length (0 when `c` is not a Deep card or
// there is nothing to carry; then `out` is untouched). `out` needs kMaxBlob bytes.
std::size_t build(const Card& c, const ct::Evidence& ev, ctsweep::View sweep, std::span<std::uint8_t> out);

}  // namespace k2::report_card::deep
