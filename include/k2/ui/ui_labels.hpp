// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Panel label model: what each of the four buttons does right now, as short words that fit a
// slot. Pure: strings in, strings out. The page supplies the Ok/list parts, nav_strip() the
// paging parts that are the same on every page.
#pragma once
#include <cstdint>
#include "k2/ui/font_aa.hpp"
#include "k2/ui/ui_layout.hpp"

namespace k2 {

// Slot lengths along the panel, in on-screen order; the four tile the 240 px edge. The labels are at
// most 7 characters ("Up/Prev", "Dn/Next", "Rescan", "Reset"; BACK is "Back"), 42 px at the 12 px font,
// so every slot is wider than text + divider + padding at both ends. The outer pair and the inner
// pair mirror each other, so the length can be looked up per button in every orientation. These are
// tunable: the real button centres are not measured yet (see kSlotCenters in ui_style.hpp).
constexpr unsigned kSlotLenPx[4] = {66, 54, 54, 66};
constexpr unsigned kLabelAdv = 6;   // advance of the 12 px panel font (kLabelFont, Spleen 6x12; tests check it)
constexpr int kSlotPadPx = 4;       // clear space between text and either end of a slot
constexpr int kDividerPx = 2;       // divider drawn on the leading edge of every slot but the first

// Where the label text may sit inside a slot of `slot_len` pixels: after the divider (slots 1..3)
// and the padding, up to the end padding. The text is centred in this zone.
struct LabelZone { int start; int len; unsigned max_chars; };

constexpr LabelZone label_zone(int slot_len, unsigned slot_index) {
    const int start = (slot_index > 0 ? kDividerPx : 0) + kSlotPadPx;
    const int len = slot_len - start - kSlotPadPx;
    return {start, len > 0 ? len : 0, len > 0 ? static_cast<unsigned>(len) / kLabelAdv : 0u};
}

// Offset along the slot of the first glyph of a `chars` long label, centred in the zone.
constexpr int label_origin(const LabelZone &z, unsigned chars) {
    return z.start + (z.len - static_cast<int>(chars * kLabelAdv)) / 2;
}

// Characters that fit the slot of one button, assuming the slot has a divider (the narrowest case).
constexpr unsigned label_max_chars(Button b) {
    return label_zone(static_cast<int>(b == Button::Back || b == Button::Plus ? kSlotLenPx[0] : kSlotLenPx[1]), 1).max_chars;
}

// One slot: the short-press action, the long-press action and an optional shorter spelling of
// the long one for when the full word does not fit. Pages use full words; the fallback stays
// available for tight slots. Empty = nothing.
struct SlotLabel {
    const char *shrt = "";
    const char *lng = "";
    const char *lng_alt = "";
    bool dim = false;   // shown but unavailable (e.g. no page to go to)
};

// In Button order: Back, Minus, Ok, Plus.
struct PanelLabels { SlotLabel b[4]; };

// "short/long" when it fits max_chars, else "short/alt", else the short word alone, else (no short
// word) the long word or its alternative. Writes ASCII; returns the length, 0 when nothing fits.
unsigned label_text(char (&out)[24], const SlotLabel &l, unsigned max_chars);

// The strip pool: every distinct slot a page can show, once, in flash. A page does not build SlotLabels; it names the pool
// entries (sl::Open, sl::BackDel, ...) and the panel resolves them when it paints. Entry order is free; ids are not stored
// anywhere (no persistence), so adding one is only an append here. Columns: name, short, long, long fallback, dim.
namespace sl {
#define K2_SLOT_POOL(X)                                                                                                \
    X(None, "", "", "", false)                                                                                         \
    X(Dim, "", "", "", true)                                                                                           \
    /* navigation */                                                                                                   \
    X(Back, "Back", "", "", false)                                                                                     \
    X(Stop, "Stop", "", "", false)                                                                                     \
    X(Up, "Up", "", "", false)                                                                                         \
    X(Dn, "Dn", "", "", false)                                                                                         \
    X(UpDim, "Up", "", "", true)                                                                                       \
    X(DnDim, "Dn", "", "", true)                                                                                       \
    X(Inc, "+", "", "", false)                                                                                         \
    X(Dec, "-", "", "", false)                                                                                         \
    X(IncBig, "+", "Big*", "", false)                                                                                  \
    X(DecBig, "-", "Big*", "", false)                                                                                  \
    X(Inc1V, "+", "1V", "", false)                                                                                     \
    X(Dec1V, "-", "1V", "", false)                                                                                     \
    X(Inc05A, "+", "0.5A", "", false)                                                                                  \
    X(Dec05A, "-", "0.5A", "", false)                                                                                  \
    X(TabPrev, "Tab-", "", "", false)                                                                                  \
    X(TabNext, "Tab+", "", "", false)                                                                                  \
    X(Mode, "Mode", "", "", false)                                                                                     \
    X(Prev, "Prev", "", "", false)                                                                                     \
    X(Hero, "", "Hero", "", false)                                                                                     \
    X(BackDel, "Back", "Delete", "Del", false)                                                                         \
    /* OK verbs (kOkVerbs) */                                                                                          \
    X(Open, "Open", "", "", false)                                                                                     \
    X(Toggle, "Toggle", "", "", false)                                                                                 \
    X(Start, "Start", "", "", false)                                                                                   \
    X(Next, "Next", "", "", false)                                                                                     \
    X(Bench, "Bench", "", "", false)                                                                                   \
    X(Reset, "Reset", "", "", false)                                                                                   \
    X(Learn, "Learn", "", "", false)                                                                                   \
    X(Apply, "Apply", "", "", false)                                                                                   \
    X(Hold, "Hold", "", "", false)                                                                                     \
    X(Auto, "Auto", "", "", false)                                                                                     \
    X(Confirm, "Confirm", "", "", false)                                                                               \
    X(Zoom, "Zoom", "", "", false)                                                                                     \
    X(Send, "Send", "", "", false)                                                                                     \
    X(Run, "Run", "", "", false)                                                                                       \
    X(Read, "Read", "", "", false)                                                                                     \
    X(Select, "Select", "", "", false)                                                                                 \
    X(Disarm, "Disarm", "", "", false)                                                                                 \
    X(Cycle, "Cycle", "", "", false)                                                                                   \
    X(Ask, "Ask", "", "", false)                                                                                       \
    X(Arm, "Arm", "", "", false)                                                                                       \
    X(View, "View", "", "", false)                                                                                     \
    X(Save, "Save", "", "", false)                                                                                     \
    X(Refresh, "Refresh", "", "", false)                                                                               \
    X(Delete, "Delete", "", "", false)                                                                                 \
    X(Link, "Link", "", "", false)                                                                                     \
    X(K2Data, "K2 data", "", "", false)                                                                                \
    X(Actions, "Actions", "", "", false)                                                                               \
    X(Steps, "Steps", "", "", false)                                                                                   \
    X(Share, "Share", "", "", false)                                                                                   \
    X(Pick, "Pick", "", "", false)                                                                                     \
    X(Set, "Set", "", "", false)                                                                                       \
    X(Resume, "Resume", "", "", false)                                                                                 \
    X(Diff, "Diff", "", "", false)                                                                                     \
    X(Results, "Results", "", "", false)                                                                               \
    X(Change, "Change", "", "", false)                                                                                 \
    X(Off, "Off", "", "", false)                                                                                       \
    X(Cancel, "Cancel", "", "", false)                                                                                 \
    X(Host, "Host", "", "", false)                                                                                     \
    X(NextDim, "Next", "", "", true)                                                                                   \
    X(SendDim, "Send", "", "", true)                                                                                   \
    /* OK verb with a long press */                                                                                    \
    X(ShareLong, "", "Share", "", false)                                                                               \
    X(BenchReset, "Bench", "Reset", "", false)                                                                         \
    X(SetGrp, "Set", "Grp", "", false)                                                                                 \
    X(ToggleOpen, "Toggle", "Open", "", false)                                                                         \
    X(OpenShare, "Open", "Share", "", false)                                                                           \
    X(StartShare, "Start", "Share", "", false)                                                                         \
    X(DeleteShare, "Delete", "Share", "", false)                                                                       \
    X(StepsShare, "Steps", "Share", "", false)                                                                         \
    X(DiffShare, "Diff", "Share", "", false)                                                                           \
    X(ResultsShare, "Results", "Share", "", false)                                                                     \
    X(ActionsShare, "Actions", "Share", "", false)                                                                     \
    X(ConfirmShare, "Confirm", "Share", "", false)                                                                     \
    X(RefreshShare, "Refresh", "Share", "", false)                                                                     \
    X(CancelShare, "Cancel", "Share", "", false)                                                                       \
    X(ViewShare, "View", "Share", "", false)                                                                           \
    X(ShareShare, "Share", "Share", "", false)                                                                         \
    X(ReadShare, "Read", "Share", "", false)                                                                           \
    X(SaveShare, "Save", "Share", "", false)                                                                           \
    X(AskShare, "Ask", "Share", "", false)

enum Id : uint8_t {
#define K2_SLOT_ENUM(name, ...) name,
    K2_SLOT_POOL(K2_SLOT_ENUM)
#undef K2_SLOT_ENUM
    kCount
};
}  // namespace sl

// What a page returns for its four buttons (Button order Back, Minus, Ok, Plus): four pool ids, 4 bytes.
struct Strip {
    sl::Id b[4];
    sl::Id &operator[](Button x) { return b[static_cast<int>(x)]; }
    sl::Id operator[](Button x) const { return b[static_cast<int>(x)]; }
};

// The pool entry as the slot painter and the tests read it.
SlotLabel slot_label(sl::Id id);
PanelLabels resolve(const Strip &s);

// The labels every page shares: BACK "Back", [-] / [+] "Up" / "Dn" (a list's focus, the Meter's face; Fieldbook has no
// paging press). OK is left empty for the page to fill in.
constexpr Strip nav_strip() { return {{sl::Back, sl::Up, sl::None, sl::Dn}}; }

// The strip vocabulary (docs/UI_DESIGN.md 6, "Strip vocabulary"):
//  * BACK is "Back" everywhere ("Stop" only while a fast-charge run is live: there it stops the output); Flow, with no
//    level above it, leaves the short press blank and shows its long "Hero".
//  * [-] / [+] move through a list as "Up" / "Dn", and change a value as "-" / "+".
//    "Tab-" / "Tab+" switch the PD tabs, "Mode" toggles the Cable R method.
//  * OK is a verb of the focused item, one of kOkVerbs (its long press is free text: Reset, Menu, Grp, Charger).
//  * The footer never repeats the strip: no "OK", "BACK" or "+/-" instructions in footer text.
inline constexpr const char *kUp = "Up", *kDn = "Dn";

// [-]/[+] as a list pair on top of `base`: "Up"/"Dn" (no pool entry carries a long press on those two).
constexpr Strip list_strip(Strip base) {
    base[Button::Minus] = sl::Up;
    base[Button::Plus] = sl::Dn;
    return base;
}

// First rule the panel breaks, or nullptr. Used by the table test over every page and submenu.
const char *label_violation(const PanelLabels &p);

// True when `s` is a footer-safe text: no button instruction ("OK ...", "BACK ...", "+/- ...").
bool footer_is_clean(const char *s);

}  // namespace k2
