// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// No-truncation gate, two layers (docs/UI_DESIGN.md section 6, rule "Copy must fit").
//
// Layer 1, compile time: `fits_px` and `all_fit` check a constant string table against the box it is drawn into,
// so a table entry that is too long is a compile error (static_assert next to the table).
//
// Layer 2, run time, host builds only (-DK2_TRUNC_LOG=1): every place that cuts or clips text on its way to the
// screen (fit_text, gfx_text's surface clip, Field::draw, the "OVR" placeholder) reports to `trunc::note`, and each
// host test binary ends in `test_report`, which fails when copy was truncated anywhere the test drove the UI.
// The device build defines no K2_TRUNC_LOG: the macros below vanish, the signatures and the code are unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace k2 {

// Code points of a UTF-8 string (every font here is monospaced, so width = cells x advance).
constexpr int text_cells(std::string_view s) {
    int n = 0;
    for (const char c : s) n += (static_cast<uint8_t>(c) & 0xC0) != 0x80;
    return n;
}

// Does `s` fit `max_px` in a monospaced font of advance `adv` px (6 / 8 / 12 / 24 for the 12 / 16 / 24 / 48 px fonts)?
constexpr bool fits_px(unsigned adv, std::string_view s, int max_px) {
    return text_cells(s) * static_cast<int>(adv) <= max_px;
}

// Every entry of a table of C strings fits.
template <std::size_t N>
constexpr bool all_fit(const char *const (&table)[N], unsigned adv, int max_px) {
    for (const char *s : table)
        if (!fits_px(adv, s, max_px)) return false;
    return true;
}

// Longest entry of such a table, in cells (for a failing static_assert to be easy to read in the build log).
template <std::size_t N>
constexpr int longest_cells(const char *const (&table)[N]) {
    int most = 0;
    for (const char *s : table) most = text_cells(s) > most ? text_cells(s) : most;
    return most;
}

namespace trunc {

// What happened to the text.
enum class Kind : uint8_t {
    Cut,    // our copy was shortened (fit_text, a Field, the OVR placeholder): a gate failure, reword it
    Clip,   // our copy ran off its surface and gfx_text clipped it: a gate failure
    Data,   // external or user data (saved names, vendor strings, card titles): shortened by design, only counted
};

#ifdef K2_TRUNC_LOG
void note(Kind kind, std::string_view text, int max_px, const char *file = nullptr, unsigned line = 0);
// A label the report prints with each hit ("page tools rot 1"): set by the walking test, "" otherwise.
void context(const char *label);
// Drops notes while alive: a unit test of the fitting itself, or a clip that is the point (the progress bar's fill).
struct Pause {
    Pause();
    ~Pause();
    Pause(const Pause &) = delete;
    Pause &operator=(const Pause &) = delete;
};
int failures();     // distinct Cut / Clip hits so far
int data_hits();    // distinct Data hits so far
void clear();
void print_report(bool with_data = false);   // with_data: the Data hits too (a test that wants to see what was cut by design)
#endif

}  // namespace trunc
}  // namespace k2

#ifdef K2_TRUNC_LOG
#include <source_location>
// Call-site capture: the declaration gains a defaulted source_location, the definition takes it, calls forward it.
#define K2_TRUNC_DECL , const std::source_location &site = std::source_location::current()
#define K2_TRUNC_DEF  , const std::source_location &site
#define K2_TRUNC_ARG  , site
#define K2_TRUNC_NOTE(kind, text, px) ::k2::trunc::note(kind, text, px, site.file_name(), site.line())
#define K2_TRUNC_NOTE_HERE(kind, text, px) ::k2::trunc::note(kind, text, px)
#define K2_TRUNC_PAUSE ::k2::trunc::Pause k2_trunc_pause_
#else
#define K2_TRUNC_DECL
#define K2_TRUNC_DEF
#define K2_TRUNC_ARG
#define K2_TRUNC_NOTE(kind, text, px) ((void)0)
#define K2_TRUNC_NOTE_HERE(kind, text, px) ((void)0)
#define K2_TRUNC_PAUSE ((void)0)
#endif
