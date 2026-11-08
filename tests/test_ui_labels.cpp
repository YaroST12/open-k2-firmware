// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the panel label model: wording, dimming and fit in a slot.
#include "test_util.h"
#include "k2/ui/ui_labels.hpp"
#include "k2/ui/font_aa.hpp"
#include "k2/ui/ui_style.hpp"
#include <initializer_list>
#include <string.h>

using namespace k2;

static const char *text_of(const SlotLabel &l, unsigned max, unsigned *n = nullptr) {
    static char buf[24];
    unsigned c = label_text(buf, l, max);
    if (n) *n = c;
    return buf;
}

static void test_label_text() {
    unsigned n;
    CHECK_STR(text_of({"Back", "", ""}, 8, &n), "Back");                   CHECK_EQ(n, 4);
    CHECK_STR(text_of({"+", "Next", ""}, 6, &n), "+/Next");                CHECK_EQ(n, 6);
    // Not enough room for the long word: an alternative spelling is used if the label has one,
    // then the short press alone.
    CHECK_STR(text_of({"Back", "Reboot", "Rbt"}, 8, &n), "Back/Rbt");      CHECK_EQ(n, 8);
    CHECK_STR(text_of({"Back", "Reboot", ""}, 8, &n), "Back");             CHECK_EQ(n, 4);
    CHECK_STR(text_of({"", "Reboot", "Rbt"}, 4, &n), "Rbt*");              CHECK_EQ(n, 4);
    CHECK_STR(text_of({"", "Reset", ""}, 8, &n), "Reset*");                CHECK_EQ(n, 6);
    CHECK_STR(text_of({"", "", ""}, 8, &n), "");                           CHECK_EQ(n, 0);
    CHECK_STR(text_of({"Rescan", "", ""}, 3, &n), "");                     CHECK_EQ(n, 0);   // does not fit at all
}

static void test_nav_labels() {
    const unsigned plus_chars = label_max_chars(Button::Plus), minus_chars = label_max_chars(Button::Minus);
    // [+] and [-] read "+/Next" and "-/Prev" on every page; dim when that paging press is a no-op.
    for (unsigned page = 0; page < 4; page++) {
        PanelLabels l = nav_labels(page, 4);
        CHECK_STR(text_of(l.b[static_cast<int>(Button::Plus)], plus_chars), "+/Next");
        CHECK_STR(text_of(l.b[static_cast<int>(Button::Minus)], minus_chars), "-/Prev");
        CHECK_EQ(l.b[static_cast<int>(Button::Minus)].dim, page == 0);
        CHECK_EQ(l.b[static_cast<int>(Button::Plus)].dim, page == 3);
        // BACK reads just "Back" on every page, Home included; there is no Reboot label anywhere.
        CHECK_STR(text_of(l.b[0], label_max_chars(Button::Back)), "Back");
        CHECK_EQ(l.b[0].dim, false);
        CHECK_STR(l.b[0].lng, "");
    }
    // OK carries nothing by default; a page fills it in.
    CHECK_STR(text_of(nav_labels(1, 4).b[static_cast<int>(Button::Ok)], minus_chars), "");
}

static void test_label_zone() {
    // Text zone of a slot: 2 px divider on the leading edge (all but the first slot), then 4 px pad
    // at both ends; the characters that fit follow from the zone.
    // The advance constants match the fonts (Spleen 6x12 and 8x16).
    CHECK_EQ(static_cast<int>(kLabelAdv), static_cast<int>(kLabelFont.adv));
    CHECK_EQ(static_cast<int>(kSmallAdvPx), static_cast<int>(kSmall.adv));
    const LabelZone inner = label_zone(54, 1);
    CHECK_EQ(inner.start, 6);  CHECK_EQ(inner.len, 44);  CHECK_EQ(inner.max_chars, 44 / kLabelAdv);
    const LabelZone first = label_zone(66, 0);                       // no divider on slot 0
    CHECK_EQ(first.start, 4);  CHECK_EQ(first.len, 58);  CHECK_EQ(first.max_chars, 58 / kLabelAdv);
    // Centred: the gap on each side differs by at most one pixel, and clears divider + pad.
    for (unsigned chars = 1; chars <= inner.max_chars; chars++) {
        const int o = label_origin(inner, chars);
        const int right = 54 - (o + static_cast<int>(chars * kLabelAdv));
        CHECK_EQ(o >= kDividerPx + kSlotPadPx, 1);
        CHECK_EQ(right >= kSlotPadPx, 1);
        CHECK_EQ(o - kDividerPx - right <= 1 && right - (o - kDividerPx) <= 1, 1);
    }
    // A slot too small for any text gives no characters rather than a negative width.
    CHECK_EQ(label_zone(6, 1).max_chars, 0);
}

static void test_fit_in_slot() {
    // Slots tile the edge, and every label of every state fits its zone at the panel font's advance
    // with the full words (no abbreviation fallback is needed). Outer slots mirror each other so the
    // table is valid in every orientation.
    CHECK_EQ(kSlotLenPx[0] + kSlotLenPx[1] + kSlotLenPx[2] + kSlotLenPx[3], 240);
    CHECK_EQ(kSlotLenPx[0], kSlotLenPx[3]);
    CHECK_EQ(kSlotLenPx[1], kSlotLenPx[2]);
    CHECK_EQ(label_max_chars(Button::Minus), 7);
    CHECK_EQ(label_max_chars(Button::Back) >= 4, 1);
    struct Sample { SlotLabel l; Button b; };
    const Sample samples[] = {{{"Back", "", ""}, Button::Back},
                              {{"+", "Next", ""}, Button::Plus}, {{"-", "Prev", ""}, Button::Minus},
                              {{"Rescan", "", ""}, Button::Ok}, {{"Scan", "", ""}, Button::Ok},
                              {{"", "Reset", ""}, Button::Ok}};
    char buf[24];
    for (const Sample &s : samples) {
        const unsigned n = label_text(buf, s.l, label_max_chars(s.b));
        CHECK_EQ(n > 0, 1);
        CHECK_STR(buf, text_of(s.l, 24));   // the full text, nothing dropped
        const int slot_len = static_cast<int>(s.b == Button::Back || s.b == Button::Plus ? kSlotLenPx[0] : kSlotLenPx[1]);
        // Fits with divider + 4 px padding at each end in the worst case (a slot that has a divider).
        CHECK_EQ(static_cast<int>(n * kLabelFont.adv) + kDividerPx + 2 * kSlotPadPx <= slot_len, 1);
        CHECK_EQ(strlen(buf), n);
    }
}

// The strip vocabulary: Up/Dn (long Prev/Next) on lists, -/+ on values, OK a verb, BACK always Back.
static void test_vocabulary() {
    auto strip = [](SlotLabel back, SlotLabel minus, SlotLabel ok, SlotLabel plus) {
        PanelLabels p;
        p.b[static_cast<int>(Button::Back)] = back;   p.b[static_cast<int>(Button::Minus)] = minus;
        p.b[static_cast<int>(Button::Ok)] = ok;       p.b[static_cast<int>(Button::Plus)] = plus;
        return p;
    };
    // The shared paging labels are a value pair; list_labels turns them into Up/Dn and keeps Prev / Next.
    CHECK_EQ(label_violation(nav_labels(1, 4)) == nullptr, 1);
    PanelLabels lst = list_labels(nav_labels(1, 4));
    CHECK_STR(text_of(lst.b[static_cast<int>(Button::Minus)], label_max_chars(Button::Minus)), "Up/Prev");
    CHECK_STR(text_of(lst.b[static_cast<int>(Button::Plus)], label_max_chars(Button::Plus)), "Dn/Next");
    CHECK_EQ(label_violation(lst) == nullptr, 1);
    // Every OK verb in the table is a valid OK label; nouns and answers are not.
    for (const char *v : {"Scan", "Listen", "Run", "Force", "Start", "Stop", "Send", "Rescan", "Clear", "Open", "Reset",
                          "Next", "Select", "View", "Delete", "Arm", "Disarm", "Toggle", "Cycle", "Apply", "Refresh",
                          "Ask", "Confirm", "Zoom"})
        CHECK_EQ(label_violation(strip({"Back"}, {}, {v}, {})) == nullptr, 1);
    for (const char *v : {"Span", "Yes", "Again", "Menu", "Mode", "Back"})
        CHECK_EQ(label_violation(strip({"Back"}, {}, {v}, {})) != nullptr, 1);
    // BACK: only "Back", or "Stop" while a run is live.
    CHECK_EQ(label_violation(strip({"Stop"}, {}, {}, {})) == nullptr, 1);
    for (const char *v : {"Cancel", "List", "Prev", "Esc"})
        CHECK_EQ(label_violation(strip({v}, {}, {}, {})) != nullptr, 1);
    // [-]/[+]: Up/Dn, -/+, Tab-/Tab+ and Mode only; "Down", "V+" and a list/value mix are refused.
    CHECK_EQ(label_violation(strip({"Back"}, {"Up"}, {}, {"Dn"})) == nullptr, 1);
    CHECK_EQ(label_violation(strip({"Back"}, {"-", "Big*"}, {}, {"+", "Big*"})) == nullptr, 1);
    CHECK_EQ(label_violation(strip({"Back"}, {"Tab-", "Prev"}, {}, {"Tab+", "Next"})) == nullptr, 1);
    CHECK_EQ(label_violation(strip({"Back"}, {}, {}, {"Mode"})) == nullptr, 1);
    CHECK_EQ(label_violation(strip({"Back"}, {"Up"}, {}, {"Down"})) != nullptr, 1);
    CHECK_EQ(label_violation(strip({"Back"}, {"V-"}, {}, {"V+"})) != nullptr, 1);
    CHECK_EQ(label_violation(strip({"Back"}, {"Up"}, {}, {"+"})) != nullptr, 1);
    CHECK_EQ(label_violation(strip({"Back"}, {"-"}, {}, {"Dn"})) != nullptr, 1);
    // A footer never repeats the strip.
    for (const char *f : {"This boot only", "Replug to apply", "Back to 5V", "Above 5V: send twice", "Okay", "1/3 cables"})
        CHECK_EQ(footer_is_clean(f), 1);
    for (const char *f : {"OK toggles; remembered", "+/- pick, OK next", "BACK = release", "Live sink. OK again forces",
                          "OK: ask the device", "OK or BACK cancels", "x (OK)"})
        CHECK_EQ(footer_is_clean(f), 0);
}

int main() {
    test_vocabulary();
    test_label_text();
    test_nav_labels();
    test_label_zone();
    test_fit_in_slot();
    return test_report("test_ui_labels");
}
