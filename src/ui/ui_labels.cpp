// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_labels.hpp"
#include <initializer_list>

namespace k2 {

namespace {
unsigned len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

unsigned put(char (&out)[24], unsigned n, const char *s) {
    for (; *s && n < 23; s++) out[n++] = *s;
    out[n] = 0;
    return n;
}
}  // namespace

unsigned label_text(char (&out)[24], const SlotLabel &l, unsigned max_chars) {
    out[0] = 0;
    const unsigned ns = len(l.shrt), nl = len(l.lng), na = len(l.lng_alt);
    if (ns == 0) {
        // A long-press-only action carries a trailing '*' (design 6.2) when there is room for it.
        if (nl && nl + 1 <= max_chars) return put(out, put(out, 0, l.lng), "*");
        if (na && na + 1 <= max_chars) return put(out, put(out, 0, l.lng_alt), "*");
        if (nl && nl <= max_chars) return put(out, 0, l.lng);
        if (na && na <= max_chars) return put(out, 0, l.lng_alt);
        return 0;
    }
    if (ns > max_chars) return 0;
    if (nl && ns + 1 + nl <= max_chars) { put(out, put(out, put(out, 0, l.shrt), "/") , l.lng); return ns + 1 + nl; }
    if (na && ns + 1 + na <= max_chars) { put(out, put(out, put(out, 0, l.shrt), "/"), l.lng_alt); return ns + 1 + na; }
    put(out, 0, l.shrt);
    return ns;
}

PanelLabels nav_labels(unsigned page, unsigned page_count) {
    PanelLabels p;
    // Reboot is OK + Back held together for 3 s (on every page and menu) and is not advertised on the panel.
    p.b[static_cast<int>(Button::Back)] = {"Back", "", "", false};
    p.b[static_cast<int>(Button::Minus)] = {"-", "Prev", "", page == 0};
    p.b[static_cast<int>(Button::Plus)] = {"+", "Next", "", page + 1 >= page_count};
    return p;
}

PanelLabels list_labels(PanelLabels base) {
    base.b[static_cast<int>(Button::Minus)].shrt = kUp;
    // A tight slot may shorten a real long press ("Prev" -> "Prv"); a menu has no long press, so no "Up/Prv" there.
    if (*base.b[static_cast<int>(Button::Minus)].lng) base.b[static_cast<int>(Button::Minus)].lng_alt = "Prv";
    base.b[static_cast<int>(Button::Plus)].shrt = kDn;
    return base;
}

namespace {
constexpr const char *kOkVerbs[] = {
    "Scan", "Listen", "Run", "Force", "Start", "Stop", "Send", "Rescan", "Clear", "Open", "Reset",
    "Next", "Select", "View", "Delete", "Arm", "Disarm", "Toggle", "Cycle", "Apply", "Refresh", "Ask",
    "Confirm", "Zoom",
    // Device twins: OK on the last row stores the device just identified
    "Learn",
    // Connection page: OK opens the tab's action pop-up
    "Actions",
    // Charger-flow setup pages (their wording is owned by the charger test)
    "Go", "Set", "Pick", "Resume", "Diff", "Steps", "All", "Off", "Cancel",
};

bool eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

bool one_of(const char *s, std::initializer_list<const char *> set) {
    for (const char *w : set) if (eq(s, w)) return true;
    return false;
}

bool word_at(const char *s, const char *w) {   // `w` as a whole word at s[0]
    unsigned n = 0;
    while (w[n] && s[n] == w[n]) n++;
    return !w[n] && (s[n] == 0 || s[n] == ' ' || s[n] == ':' || s[n] == ',' || s[n] == '.' || s[n] == ';' || s[n] == ')');
}
}  // namespace

const char *label_violation(const PanelLabels &p) {
    const SlotLabel &back = p.b[static_cast<int>(Button::Back)], &minus = p.b[static_cast<int>(Button::Minus)],
                    &ok = p.b[static_cast<int>(Button::Ok)], &plus = p.b[static_cast<int>(Button::Plus)];
    if (!one_of(back.shrt, {"Back", "Stop"})) return "BACK must read Back";
    if (!one_of(minus.shrt, {"", "-", kUp, "Tab-"})) return "[-] must be -, Up or Tab-";
    if (!one_of(plus.shrt, {"", "+", kDn, "Tab+", "Mode"})) return "[+] must be +, Dn, Tab+ or Mode";
    if ((eq(minus.shrt, kUp) && eq(plus.shrt, "+")) || (eq(minus.shrt, "-") && eq(plus.shrt, kDn))) return "list and value pair mixed";
    if (ok.shrt[0]) {
        bool verb = false;
        for (const char *v : kOkVerbs) verb = verb || eq(ok.shrt, v);
        if (!verb) return "OK must be a verb";
    }
    return nullptr;
}

bool footer_is_clean(const char *s) {
    for (unsigned i = 0; s[i]; i++) {
        const bool word_start = i == 0 || s[i - 1] == ' ' || s[i - 1] == '(';
        if (word_start && (word_at(s + i, "OK") || word_at(s + i, "BACK"))) return false;
        if (s[i] == '+' && s[i + 1] == '/' && s[i + 2] == '-') return false;
    }
    return true;
}

}  // namespace k2
