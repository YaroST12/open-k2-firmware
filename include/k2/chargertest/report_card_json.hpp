// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// JSON text of one charger report card, for the USB drive (CARDS.JSON) and mirrored by tools/report_card.py
// (`k2_remote.py cards export`). Pure logic, host-tested by tests/test_report_card_json.cpp against
// tests/fixtures/report_card_sample.json, which tests/test_report_card_py.py decodes independently.
//
// Keys: no, name, fp (8 hex digits, upper case), vid, pid, runs, boots, uptime_s, then times, last_boots, last_uptime_s only when
// times > 1 (identical repeats of the stored run, see report_card.hpp), fw (8 hex), state, reason, pdos, spec_rev,
// handover, handover_ms, groups{<group>: {verdict, max_mv, reason, detail}}, ranges[{alg, lo_mv, hi_mv, max_mv,
// fail_mv}]. Groups that were not part of the run are left out; zero max_mv/reason/detail are left out.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/chargertest/report_card.hpp"

namespace k2::report_card {

// Writes the object for `c` into `out` (NUL terminated, cut off when it does not fit); returns the length.
std::size_t render_json(const Card& c, std::span<char> out);

// render_json(c, out) with the card's Deep block (report_card_deep.hpp: u16 bit count, then the bits) as a last key
// "deep": "<base64url, no padding>" (tools/report_card.py decode_deep and docs/site/r/card.js read it). A block that is empty or
// not whole is left out, then this is render_json(c, out). The buffer needs kJsonDeepMax with a worst-case block.
std::size_t render_json(const Card& c, std::span<const std::uint8_t> deep, std::span<char> out);

// The same card as compact Markdown for SUMMARY.MD on the USB drive (the style of tools/charger_summary.py,
// summarize_card): a header, a verdict line, then one line per group that ran, the skipped groups on one line and
// one line per range. NUL terminated, cut off when it does not fit; returns the length.
std::size_t render_summary(const Card& c, std::span<char> out);

inline constexpr std::size_t kSummaryMax = 1536;   // the longest card fits
inline constexpr std::size_t kJsonMax = 2560;   // the longest card (every group failed, four ranges) fits
inline constexpr std::size_t kJsonDeepMax = kJsonMax + 649;   // + ,"deep":"<639 characters>" for the worst 479-byte block, minus the "}" it replaces

}  // namespace k2::report_card
