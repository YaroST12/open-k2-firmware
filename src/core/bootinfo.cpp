// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure formatter of the boot-info text (see k2/core/bootinfo.hpp).
#include "k2/core/bootinfo.hpp"

#include <algorithm>

#include "k2/pd/boot_ra.hpp"

namespace k2::bootinfo {
namespace {

// Bounded writer: stops at `cap` characters, always leaves room for the NUL.
struct Out {
    std::span<char> buf;
    std::size_t cap;       // characters allowed (excluding the NUL)
    std::size_t n = 0;

    void put(char c) { if (n < cap) buf[n++] = c; }
    void str(std::string_view s) { for (const char c : s) put(c); }
    void dec(std::uint32_t v) {
        char t[10];
        int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    }
    void sdec(std::int32_t v) {
        if (v < 0) { put('-'); dec(std::uint32_t(-(v + 1)) + 1u); } else dec(std::uint32_t(v));
    }
    void hex(std::uint32_t v) {
        char t[8];
        int k = 0;
        do { t[k++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v);
        while (k) put(t[--k]);
    }
};

// Characters one note takes in "n=": <ms><s|c><hex>, plus a comma before every entry but the first.
std::size_t note_len(const Note& n) {
    std::size_t len = 2;                                   // kind letter + at least one hex digit
    for (unsigned v = n.ms; v >= 10; v /= 10) len++;
    len++;                                                 // first ms digit
    for (unsigned v = n.val; v >= 16; v >>= 4) len++;
    return len;
}

const char* mode_text(bootmode::Mode m) {
    switch (m) {
    case bootmode::Mode::OkRdCc1: return "ok";
    case bootmode::Mode::PlusRdCc2: return "plus";
    default: return "plain";
    }
}

}  // namespace

std::size_t format(const Snapshot& s, std::span<char> out) {
    if (out.empty()) return 0;
    Out o{out, std::min(out.size() - 1, kQrMax)};
    o.str("K2B1 fw="); o.str(s.fw);
    o.str(" g="); o.str(s.git);
    o.str(" p="); o.dec(s.proto);
    o.str(" t="); o.dec(s.t_ms);
    o.str(" vb="); o.dec(s.vbus_mv);
    o.str(" c1="); o.dec(s.cc1_mv);
    o.str(" c2="); o.dec(s.cc2_mv);
    o.str(" pl="); o.dec(s.plus_now ? 1 : 0);
    o.str(" m="); o.str(mode_text(s.mode));
    o.str(" k="); o.hex(s.keys);
    o.str(" pa="); o.hex(s.pa);
    o.str(" rf="); o.hex(s.reset_flags);
    o.str(" w="); o.dec(s.warm ? 1 : 0);
    o.str(" pin="); o.hex(s.pin_initial); o.put(','); o.hex(s.pin_now); o.put(','); o.hex(s.pin_boot); o.put(','); o.dec(s.inherited ? 1 : 0);
    o.str(" mk=");
    if (s.mark_read) o.hex(s.mark); else o.put('-');
    o.str(" ra="); o.dec(s.ra_on ? 1 : 0); o.put(':'); o.str(bootra::name(bootra::Verdict(s.ra_why)));
    o.str(" mv=");
    if (s.move.moved) {
        o.dec(s.move.ms); o.put(':'); o.hex(s.move.from); o.put('>'); o.hex(s.move.to); o.put(':');
        o.sdec(s.move.vbus_mv); o.put(':'); o.dec(s.move.cc1_mv); o.put(':'); o.dec(s.move.cc2_mv);
    } else {
        o.put('-');
    }
    o.str(" sw=");
    unsigned readbacks = 0, writes = 0;
    for (unsigned i = 0; i < s.note_n && i < s.notes.size(); i++) {
        const Note& n = s.notes[i];
        if (n.kind == 2) {
            if (readbacks++) o.put(',');
            o.hex(n.val); o.put('@'); o.dec(n.ms);
        } else {
            writes++;
        }
    }
    if (!readbacks) o.put('-');
    o.str(" n=");
    if (!writes) {
        o.put('-');
        out[o.n] = '\0';
        return o.n;
    }
    // As many write notes as fit, keeping room for ",+NN" when some are left out.
    unsigned shown = 0, left = writes;
    for (unsigned i = 0; i < s.note_n && i < s.notes.size(); i++) {
        const Note& n = s.notes[i];
        if (n.kind == 2) continue;
        const std::size_t need = note_len(n) + (shown ? 1 : 0);
        const bool last = left == 1;
        if (o.n + need + (last ? 0 : 4) > o.cap) break;
        if (shown) o.put(',');
        o.dec(n.ms); o.put(n.kind == 0 ? 's' : 'c'); o.hex(n.val);
        shown++; left--;
    }
    if (left) {
        if (shown) o.put(',');
        o.put('+'); o.dec(left);
    }
    out[o.n] = '\0';
    return o.n;
}

std::string_view field(std::string_view text, std::string_view key) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t end = std::min(text.find(' ', pos), text.size());
        const std::string_view tok = text.substr(pos, end - pos);
        if (tok.size() > key.size() && tok.substr(0, key.size()) == key && tok[key.size()] == '=') return tok.substr(key.size() + 1);
        pos = end + 1;
    }
    return {};
}

}  // namespace k2::bootinfo
