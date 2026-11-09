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
#include <span>

#include "k2/chargertest/report_card.hpp"

namespace k2::report_card {

// Writes the object for `c` into `out` (NUL terminated, cut off when it does not fit); returns the length.
std::size_t render_json(const Card& c, std::span<char> out);

// The same card as compact Markdown for SUMMARY.MD on the USB drive (the style of tools/charger_summary.py,
// summarize_card): a header, a verdict line, then one line per group that ran, the skipped groups on one line and
// one line per range. NUL terminated, cut off when it does not fit; returns the length.
std::size_t render_summary(const Card& c, std::span<char> out);

inline constexpr std::size_t kSummaryMax = 1536;   // the longest card fits
inline constexpr std::size_t kJsonMax = 2560;   // the longest card (every group failed, four ranges) fits

}  // namespace k2::report_card
