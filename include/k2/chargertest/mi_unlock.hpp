// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Xiaomi (SVID 0x2717) unit of the charger test, its pure half: what the auth chain unlocked and how the chain ended.
//
// The sink runs the 0x2717 auth chain on any source that lists the SVID (detected by the SVID, never by vendor) and keeps the
// list in force at the moment the chain finished (pd::XiaomiTrace::pre). The source then re-announces. diff() compares the two
// lists PDO by PDO: a PDO that is new, or whose current, power or voltage range went up, is an unlocked mode. The charger test
// plans one Mi row per mode and asks it at its new level: a fixed PDO at its voltage and new current, a PPS / AVS APDO at its
// top voltage with the current it offers there. Only what the source now advertises, never above it.
//
// Matching is by kind and voltage, not by slot (a source can insert an APDO ahead of ours after the chain): a fixed PDO pairs with
// the fixed PDO of the same voltage; a PPS / AVS APDO with the one of the same type and range, else the one that shares its minimum
// or its maximum (a range that grew). Only the SPR part of each list is compared (positions 1..7, zero words skipped): an EPR list
// heard after the chain is not a set of new modes. A PDO whose numbers went down, or only its flag bits changed, is no unlock.
// Pure: values only, host-tested by tests/test_mi_unlock.cpp.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// charger_test.hpp carries a mode table (Evidence::mi), so this header must not pull charger_test.hpp in through step_label.hpp:
// the two text functions return a step_label::Text, declared here and defined there (callers include step_label.hpp).
namespace k2::step_label { struct Text; }

namespace k2::mi_unlock {

inline constexpr std::size_t kSprSlots = 7;   // the SPR positions of a Source_Capabilities list
inline constexpr std::size_t kMaxModes = 4;   // a mode table never holds more (the rows it plans, the card's Xiaomi block)

// One unlocked mode: the PDO before the chain (0 = the list did not carry it: a new PDO) and after it, at its 1-based slot after.
struct Mode {
    std::uint32_t before = 0, after = 0;
    std::uint8_t pos = 0;
    bool operator==(const Mode&) const = default;
};
struct Diff {
    std::array<Mode, kMaxModes> modes{};
    std::uint8_t n = 0;
    std::span<const Mode> view() const { return {modes.data(), n}; }
};

// True when `after` offers more than `before` of the same kind: more current, more power, or a higher top voltage.
bool raised(std::uint32_t before, std::uint32_t after);
// The unlocked modes, in the order of the after list (the first kMaxModes).
Diff diff(std::span<const std::uint32_t> before, std::span<const std::uint32_t> after);

// The level a Mi row asks for a mode: fixed = its voltage and current; PPS / AVS = the top voltage and the current offered there.
struct Point { std::uint16_t mv = 0, ma = 0; };
Point test_point(std::uint32_t after);
// The watts of that level, rounded down (0 = no offer).
unsigned point_w(std::uint32_t after);

// "PPS 5-11V 3A→5A", "PD 20V 3A→5A", "AVS 15-20V 60W→100W", "PPS 5-11V→5-20V 3A" (range grew), "PPS 5-20V 6A new";
// the arrow is U+2192 (one glyph cell). The after mode alone ("PPS 5-11V 5A") when before is 0 and `fresh` is false: a mode
// whose before was not kept (a run resumed from the journal). Wider than `max_cells` (0 = a Steps row, step_label::kRowCells; the
// Detail page's value column is 16), a range is named by its top ("PPS 21V 6.25A").
step_label::Text mode_text(const Mode&, bool fresh = true, unsigned max_cells = 0);

// How the chain ended, as the chain row's Step::detail (fits the card's five detail bits): no SVID, the SVID was listed but no
// 0x2717 step came, failed at step N (the last 0x2717020x the source sent), or done: verified at 0x27170208 (payload 1) or
// finished through our 0x2717010A and the source's 0x2717020A.
enum class Chain : std::uint8_t { None, NoSvid, NoAnswer, Stalled, Verified, Finished };
inline constexpr std::uint8_t kCodeNoSvid = 1, kCodeNoAnswer = 2, kCodeStalled = 3, kCodeVerified = 20, kCodeFinished = 21;
// last_step: 1..8 or 0x0A; finish: the step that finished the chain (8 or 0x0A, 0 = not finished).
std::uint8_t chain_code(bool svid, bool authed, std::uint8_t last_step, std::uint8_t finish);
Chain chain_of(std::uint8_t code);
std::uint8_t chain_step(std::uint8_t code);   // the step a Stalled chain stopped at (0 otherwise)
constexpr bool chain_done(std::uint8_t code) { return code == kCodeVerified || code == kCodeFinished; }
// The outcome in words, at most 16 cells: "done, verified", "done via 020A", "failed at step 5", "no answer", "no 0x2717 SVID".
step_label::Text chain_text(std::uint8_t code);

}  // namespace k2::mi_unlock
