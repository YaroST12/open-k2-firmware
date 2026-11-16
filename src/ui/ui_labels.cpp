// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_labels.hpp"
#include <array>
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

// The pool. Every distinct word is stored once (kWords); an entry is three bytes: short and long word index, and the
// fallback word index with the dim flag in bit 7. The texts come from the same K2_SLOT_POOL rows as the ids.
namespace {
struct Def { const char *s, *l, *a; bool dim; };
constexpr Def kDefs[] = {
#define K2_SLOT_DEF(name, s, l, a, dim) {s, l, a, dim},
    K2_SLOT_POOL(K2_SLOT_DEF)
#undef K2_SLOT_DEF
};
static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == sl::kCount);

constexpr bool same(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

struct WordSet { const char *w[3 * sl::kCount + 1]; unsigned n; };
constexpr unsigned word_index(WordSet &ws, const char *s) {
    for (unsigned i = 0; i < ws.n; i++) if (same(ws.w[i], s)) return i;
    ws.w[ws.n] = s;
    return ws.n++;
}
constexpr WordSet collect() {
    WordSet ws{};
    word_index(ws, "");   // word 0 is the empty text
    for (const Def &d : kDefs) { word_index(ws, d.s); word_index(ws, d.l); word_index(ws, d.a); }
    return ws;
}
constexpr unsigned kWordCount = collect().n;
static_assert(kWordCount < 128);   // bit 7 of the fallback index is the dim flag

constexpr auto kWords = [] {
    std::array<const char *, kWordCount> a{};
    const WordSet ws = collect();
    for (unsigned i = 0; i < kWordCount; i++) a[i] = ws.w[i];
    return a;
}();

struct Packed { uint8_t s, l, a_dim; };
constexpr auto kPacked = [] {
    std::array<Packed, sl::kCount> a{};
    WordSet ws = collect();
    for (unsigned i = 0; i < sl::kCount; i++)
        a[i] = {static_cast<uint8_t>(word_index(ws, kDefs[i].s)), static_cast<uint8_t>(word_index(ws, kDefs[i].l)),
                static_cast<uint8_t>(word_index(ws, kDefs[i].a) | (kDefs[i].dim ? 0x80u : 0u))};
    return a;
}();
}  // namespace

SlotLabel slot_label(sl::Id id) {
    const Packed &p = kPacked[id];
    return {kWords[p.s], kWords[p.l], kWords[p.a_dim & 0x7F], (p.a_dim & 0x80) != 0};
}

PanelLabels resolve(const Strip &s) {
    PanelLabels p;
    for (int i = 0; i < 4; i++) p.b[i] = slot_label(s.b[i]);
    return p;
}

namespace {
constexpr const char *kOkVerbs[] = {
    "Scan", "Listen", "Run", "Force", "Start", "Stop", "Send", "Rescan", "Clear", "Open", "Reset",
    "Next", "Select", "View", "Delete", "Arm", "Disarm", "Toggle", "Cycle", "Apply", "Refresh", "Ask",
    "Confirm", "Zoom",
    // The Meter faces: OK opens the Bench
    "Bench",
    // Device twins: OK on the last row stores the device just identified
    "Learn",
    // Connection page: OK opens the tab's action pop-up
    "Actions",
    // Charger ID / Cable read / Device ID: the tool's own action, or the focused action row
    "Read", "Save", "Share",
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
    if (!one_of(back.shrt, {"Back", "Stop", ""})) return "BACK must read Back";
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
