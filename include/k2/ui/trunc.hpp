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

#include "k2/core/strpack.hpp"

namespace k2 {

struct Font;   // font_aa.hpp (the overlap recorder below only keeps the pointer)

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

// The same for a packed table (char[N][M]: no pointer per entry, for the flash budget).
template <std::size_t N, std::size_t M>
constexpr bool all_fit(const char (&table)[N][M], unsigned adv, int max_px) {
    for (const auto& s : table)
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

// Compile-time width checks for BPE-packed (K2_P) or plain ASCII string tables.
constexpr bool fits_px_p(unsigned adv, const char *s, int max_px) {
    return strpack::cells_packed(s) * static_cast<int>(adv) <= max_px;
}

template <std::size_t N>
constexpr bool all_fit_p(const char *const (&table)[N], unsigned adv, int max_px) {
    for (const char *s : table)
        if (!fits_px_p(adv, s, max_px)) return false;
    return true;
}

namespace trunc {

// What happened to the text.
enum class Kind : uint8_t {
    Cut,    // our copy was shortened (fit_text, a Field, the OVR placeholder): a gate failure, reword it
    Clip,   // our copy ran off its surface and gfx_text clipped it: a gate failure
    Data,   // external or user data (saved names, vendor strings, card titles): shortened by design, only counted
    Overlap,   // two text draws that both still show ink on the final frame overlap (tests/overlap_log.cpp): a gate failure
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
// Overlap guard (opt in: tests/overlap_log.cpp, overlap_enable()). gfx_text / gfx_text_rot report every draw's native box;
// when the test calls settled() (the screen has stopped changing: the ticks after a press ran) the recorder replays which draw
// owns each inked pixel on the panel and fails a draw whose ink was partly overpainted by another text draw. A draw fully repainted by a later one is dead and ignored,
// as is a draw of the same field (same font and origin: a Field redrawing changed cells).
void note_text(const Font *f, std::string_view text, int key_x, int key_y, int nx, int ny, int w, int h, int dir);
// field: non-zero inside a Field redraw (FieldScope); draws of one field never damage each other (it pads and diffs its own cells).
void set_text_hook(void (*hook)(const Font *, std::string_view, int, int, int, int, int, int, int, bool allowed, unsigned field));
void set_frame_hook(void (*hook)());
void settled();   // the screen has settled: check the frame (test_report does it for the last one)
// Deliberate composites (a toast over the page, text over a sweep trend): draws made while one is alive never count.
struct OverlapOk {
    OverlapOk();
    ~OverlapOk();
    OverlapOk(const OverlapOk &) = delete;
    OverlapOk &operator=(const OverlapOk &) = delete;
};
struct FieldScope {
    explicit FieldScope(unsigned key);
    ~FieldScope();
    FieldScope(const FieldScope &) = delete;
    FieldScope &operator=(const FieldScope &) = delete;
};
int failures();     // distinct Cut / Clip / Overlap hits so far
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
#define K2_OVERLAP_OK ::k2::trunc::OverlapOk k2_overlap_ok_
#define K2_FIELD_SCOPE(key) ::k2::trunc::FieldScope k2_field_scope_(key)
#define K2_TEXT_DRAWN(f, text, kx, ky, nx, ny, w, h, dir) ::k2::trunc::note_text(&(f), text, kx, ky, nx, ny, w, h, dir)
#else
#define K2_TRUNC_DECL
#define K2_TRUNC_DEF
#define K2_TRUNC_ARG
#define K2_TRUNC_NOTE(kind, text, px) ((void)0)
#define K2_TRUNC_NOTE_HERE(kind, text, px) ((void)0)
#define K2_TRUNC_PAUSE ((void)0)
#define K2_OVERLAP_OK ((void)0)
#define K2_FIELD_SCOPE(key) ((void)0)
#define K2_TEXT_DRAWN(f, text, kx, ky, nx, ny, w, h, dir) ((void)0)
#endif
