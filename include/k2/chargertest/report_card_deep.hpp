// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The extension block of the report card (card format v11: the Deep block; v12 adds the Xiaomi section): what a run knows that the
// 250-byte card record has no room for, carried as one bit-packed blob that travels with the card (the store keeps it in the card's
// own ring record, the share link appends it to the schema v2 stream). A Deep run fills the spec, SVOOC and sweep parts; any run whose
// Xiaomi chain row ran on a source listing the 0x2717 SVID adds the Xiaomi section, so a Medium or Light card can carry a block too (header bits all 0, then the section).
// Pure: values and buffers, no flash, no UI (host-tested by tests/test_report_card_deep.cpp).
//
// The blob is `u16 nbits` (little endian) and then ceil(nbits / 8) bytes of an LSB-first bit stream:
//   header   has_spec u1 | has_svooc u1 | has_sweep[range 0..3] u1 each                               6 bits
//   svooc    end u3 | why u4 | asks u3 | word u8 (100 mA per count, saturating) | vmax u9 (100 mV) | stop u10 (100 ms)   37 bits
//   spec     end u3 | why u6 | unconfirmed u1 | form u5 (specsec::kCells = the cells only; kCells + 1 = the cells, then the ranges:
//              v13), then per cell in specsec::kOrder:
//              verdict u3 (pdspec::Verdict); a Skip cell adds the skip reason u4; a Pass / Fail / Unverified cell adds the
//              worst value s13 (the cell's quantised value saturated to +-4094, -4096 = event never seen); a Fail cell adds
//              fails u8, then the place bytes a u8 and b u8 (pdspec::Agg a / b: "At" transition of the worst measurement)
//              ranges (form kCells + 1, v13): n u3, then per PPS / AVS range in run order: pos u4 | avs u1 | tone u2
//              (specsec::RangeTone) | group u2 (specsec::Group) | per slot (specsec::kApdoPps / kApdoAvs) verdict u3
//   sweep    per range i set in has_sweep, card order: lo u9 (100 mV, setpoint of point 0) | n u10 (points of the planned span,
//            100 mV apart) | scale u3 | then buckets = min(kBuckets, n) of 16 bits: mean s8 | down u4 | up u4
//   xiaomi   (v12) present when bits are left after the sweeps (a v11 block ends there): chain u5 (mi_unlock chain code: 1 no SVID,
//            2 no answer, 4..13 stalled, 20 / 21 done) | n u3 (modes, at most mi_unlock::kMaxModes) | per mode in plan order:
//            before u32 (the PDO before the chain; 0 = a new PDO, kBeforeLost = a resumed run lost it) | after u32 (the PDO the chain
//            unlocked) | mv u9 (100 mV) | ma u7 (50 mA): the level the row asked | verdict u3 (ct::Verdict) | reason u6 (ct::Reason)
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

#include "k2/chargertest/mi_unlock.hpp"
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
inline constexpr unsigned kSpecRangesForm = unsigned(specsec::kCells) + 1;   // the spec part's form when the ranges follow (v13)
inline constexpr unsigned kSpecRangeBits = 4 + 1 + 2 + 2 + 3 * unsigned(specsec::kApdoSlots);
// The count, then the two kinds' split (specsec::split: their merged rows give way to the range lines), then each range.
inline constexpr unsigned kSpecRangesBits = 3 + 2 + unsigned(pdspec::kMaxProg) * kSpecRangeBits;
static_assert(kSpecRangesForm < 32 && pdspec::kMaxProg < 8);
inline constexpr unsigned kMiHeadBits = 5 + 3;
inline constexpr unsigned kMiModeBits = 32 + 32 + 9 + 7 + 3 + 6;
inline constexpr std::uint32_t kBeforeLost = 0xFFFFFFFFu;   // a mode's PDO before the chain is not known (the run resumed; never a PDO)

consteval unsigned worst_bits() {
    return kHdrBits + kSvoocBits + kSpecHeadBits + unsigned(specsec::kCells) * kCellFailBits + kSpecRangesBits + kMaxRanges * kRangeBits +
           kMiHeadBits + unsigned(mi_unlock::kMaxModes) * kMiModeBits;
}
inline constexpr unsigned kMaxBits = worst_bits();                       // every cell failing, four ranges, four full sweeps, four Xiaomi modes: 3811 (3638 before the v13 ranges)
inline constexpr std::size_t kMaxBlob = 2 + (kMaxBits + 7) / 8;           // the blob with its length: 479 B
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

// ---- the Xiaomi section ----

struct MiMode {
    std::uint32_t before = 0, after = 0;   // the PDO before the chain (0 = new, kBeforeLost = not known) and the one it unlocked
    std::uint16_t mv = 0, ma = 0;          // the level the row asked, in the block's steps (100 mV, 50 mA)
    std::uint8_t verdict = 0, reason = 0;  // ct::Verdict, ct::Reason of the row
    bool operator==(const MiMode&) const = default;
};
struct MiSection {
    std::uint8_t chain = 0;   // mi_unlock chain code; 0 = no section (the chain row never ran)
    std::uint8_t n = 0;
    std::array<MiMode, mi_unlock::kMaxModes> mode{};
    bool operator==(const MiSection&) const = default;
};
// The section of a run: the chain row's outcome (its detail) and each mode row in plan order with its verdict; the PDO before the chain
// comes from the evidence (Evidence::mi, kBeforeLost when a resumed run no longer holds it). chain 0 when the chain row has not run
// or the source never listed the SVID (kCodeNoSvid: no section).
MiSection mi_of(std::span<const ct::Step> steps, const ct::Evidence& ev);
// The section a block carries (chain 0 for a block without one, a v11 block, or one that does not parse).
MiSection read_mi(std::span<const std::uint8_t> blob);
// The watts a mode is named by ("Mi 55W": step_label::mi_watts of the level it asked).
unsigned mi_watts(const MiMode& m);

// ---- the blob ----

// Builds the block of a finished run: for a Deep card the spec section, the SVOOC ceiling stage and the error graph of each range of
// `c` that has a sweep, from the runner's evidence and sweep record; for any card the Xiaomi section when the run's chain row ran.
// Returns the blob's length (0 when there is nothing to carry; then `out` is untouched). `out` needs kMaxBlob bytes.
// `apdo`: each PPS / AVS range's spec cells (the Runner's spec_apdo; null or empty = the spec part without ranges, form kCells).
std::size_t build(const Card& c, std::span<const ct::Step> steps, const ct::Evidence& ev, ctsweep::View sweep, std::span<std::uint8_t> out,
                  const specsec::Apdos* apdo = nullptr);

}  // namespace k2::report_card::deep
