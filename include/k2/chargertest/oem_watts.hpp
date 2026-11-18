// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// OEM power levels: what the charger-test results call a vendor protocol's level, by its watts ("SVOOC 120W", "SCP 22.5W",
// "Mi 55W") instead of a protocol version ("SVOOC65"). Pure code, host-tested (tests/test_oem_watts.cpp); the golden vectors in
// tests/fixtures/oem_watts.txt are shared with tools/oem_watts.py, so the K2, the host tools and the web pages agree.
//
// The K2 never draws current in a charger test, so no watts figure here is a measured power. A level's watts are its voltage x the
// current the protocol itself names: the limit the K2 writes (SCP), the current the charger advertises for the mode (UFCS), or the
// ask the adapter went on under (the SuperVOOC ceiling stage). Every sentence that carries such a figure says "claimed"; a cell has
// no room for the word and wears the claimed look (result_layout::Tone::Noted) instead. A protocol whose exchange names no current
// (AFC, FCP, VIVO, QC) keeps its volts: no watts are invented for it.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "k2/chargertest/charger_test.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/chargertest/svooc_ceiling.hpp"

namespace k2::oem_watts {

using Text = step_label::Text;

// The families with a level table. Append only: a Results row stores a level by its index into kLevels.
enum class Family : std::uint8_t { Vooc, Count };

// One power level of a family: the word its row and its label carry, the rated voltage / current pair, the watts the cell shows,
// and whether the SuperVOOC ceiling stage can claim it (it asks currents at the adapter's raised level, 10-11 V, never at 5 V).
struct Level {
    Family family;
    const char* word;
    std::uint16_t mv, ma;
    std::uint16_t w;
    bool ceiling;
};

// The level table, one row per level in ascending watts within a family. Sources (provenance tags only, no vendor documents):
//   VOOC   20W  5 V 4 A    low-voltage high-current VOOC (docs/CHARGING_PROTOCOL_MAP.md section 2; inferred)
//   SVOOC  50W  10 V 5 A   SuperVOOC (same; inferred)
//   SVOOC  65W  10 V 6.5 A SuperVOOC 2.0 (same; inferred)
//   SVOOC 100W  10 V 10 A  (same; inferred)
//   SVOOC 120W  11 V 11 A  the OnePlus 120 W adapter's own rating (docs/CHARGER_QUIRKS.md; factory); the ceiling stage claimed it on
//                          hardware at 11.02 V x 10.9 A (2026-11-15, docs/FASTCHARGE_PROTOCOLS.md section 3)
// Left out: VOOC 30W (5 V 6 A, never asked: it would only ever read "not tested"), an 80W level (no voltage / current pair in
// the tree's notes), 150W and 240W (20 V levels, above the ceiling stage's 12 V guard: never askable).
inline constexpr std::array<Level, 5> kLevels{{
    {Family::Vooc, "VOOC", 5000, 4000, 20, false},
    {Family::Vooc, "SVOOC", 10000, 5000, 50, true},
    {Family::Vooc, "SVOOC", 10000, 6500, 65, true},
    {Family::Vooc, "SVOOC", 10000, 10000, 100, true},
    {Family::Vooc, "SVOOC", 11000, 11000, 120, true},
}};

// A family's slice of kLevels.
struct Span { std::uint8_t first = 0, count = 0; };
constexpr Span span(Family f) {
    Span s{};
    bool seen = false;
    for (std::size_t i = 0; i < kLevels.size(); ++i) {
        if (kLevels[i].family != f) continue;
        if (!seen) { s.first = static_cast<std::uint8_t>(i); seen = true; }
        ++s.count;
    }
    return s;
}
inline constexpr std::size_t kMaxFamilyLevels = 8;   // two bits per level in a uint16_t
static_assert(span(Family::Vooc).count <= kMaxFamilyLevels);

// Does a charger-test unit belong to a family? The units whose steps a family's level rows own (one shared detection).
constexpr bool in_family(Family f, charger_test::Algorithm a) {
    using A = charger_test::Algorithm;
    switch (f) {
    case Family::Vooc: return a == A::Vooc || a == A::SuperVooc || a == A::SuperVooc65;
    default: return false;
    }
}
// The same by report-card group (one group per unit), and the one word a family reads as wherever its groups fold into one
// (the card's cells, its summary text): never a protocol version such as "SVOOC65".
constexpr bool in_family(Family f, charger_test::Group g) {
    using G = charger_test::Group;
    switch (f) {
    case Family::Vooc: return g == G::Vooc || g == G::SuperVooc || g == G::SuperVooc65;
    default: return false;
    }
}
constexpr const char* family_word(Family f) { return f == Family::Vooc ? "VOOC" : "?"; }

// What the run proves of one level. NotTested = 0, so an all-zero word is "nothing tested".
enum class Claim : std::uint8_t { NotTested = 0, NotReached, Claimed };

// The ceiling stage's word on one level: Claimed when the top VBUS x the ask it went on under (svooc_ceiling::claim_w) reaches the
// level's watts; above that, NotReached when the stage ran its course (judged, stopped, time cap), NotTested when it did not run,
// was refused, found no adapter, was cut by a guard or cancelled. A level the stage cannot ask (Level::ceiling false) is NotTested.
constexpr Claim claim(const Level& l, const svooc_ceiling::Outcome& o) {
    using E = svooc_ceiling::End;
    if (!l.ceiling) return Claim::NotTested;
    if (o.word && o.vmax_mv && svooc_ceiling::claim_w(o.vmax_mv, o.word) >= l.w) return Claim::Claimed;
    return o.end == E::Judged || o.end == E::Stopped || o.end == E::Cap ? Claim::NotReached : Claim::NotTested;
}
// Every level of a family, packed two bits each (bit 2i: the family's i-th level).
constexpr std::uint16_t claims(Family f, const svooc_ceiling::Outcome& o) {
    const Span s = span(f);
    std::uint16_t p = 0;
    for (unsigned i = 0; i < s.count; ++i) p = static_cast<std::uint16_t>(p | unsigned(claim(kLevels[s.first + i], o)) << (2 * i));
    return p;
}
constexpr Claim unpack(std::uint16_t packed, unsigned i) { return static_cast<Claim>((packed >> (2 * i)) & 3u); }

// The watts of a level as a cell reads them: "120W", "22.5W" (rounded to 0.1 W, the tenth only when it is not 0; whole watts from
// 100 W, "110W", so the text never passes 5 characters, the width of the "3.25A" a grid bracket already carries); "" for 0 mW.
Text watts(std::uint32_t mw);
// The label of a level: "SVOOC 120W", "SCP 22.5W", "Mi 55W"; the bare word for 0 mW. The one wording of every OEM level on the K2,
// in the host tools and on the web pages (the Xiaomi rows use it too).
Text label(std::string_view word, std::uint32_t mw);
// A charger-test step's own watts, from the current its protocol names: SCP the limit the K2 writes, UFCS the current the charger
// advertises for the step's mode. 0 for every other step (and for an SCP / UFCS step that names none).
std::uint32_t step_mw(const charger_test::Step&);
// The Results chip of such a step: its volts, then its watts ("10V 22.5W", "9V 27W"); "" when step_mw() is 0.
Text chip(const charger_test::Step&);

}  // namespace k2::oem_watts
