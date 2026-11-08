// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Boot-info text (k2/core/bootinfo.hpp): field order and content, the length cap, the example a PLUS boot produces.
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "k2/pd/boot_ra.hpp"
#include "k2/core/bootinfo.hpp"
#include "test_util.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
using namespace k2::bootinfo;

static Snapshot plus_boot() {
    Snapshot s;
    std::strcpy(s.fw, "1.0");
    std::strcpy(s.git, "a1b2c3d-dirty");
    s.proto = 1;
    s.mode = bootmode::Mode::PlusRdCc2;
    s.keys = bootmode::kPlus;
    s.pa = kPaOk | kPaBack | kPaMinus;       // PLUS (PA13) low
    s.reset_flags = 0x24000000;
    s.warm = false;
    s.pin_initial = 2; s.pin_now = 1; s.pin_boot = 1;
    s.inherited = false;
    s.ra_on = true; s.ra_why = std::uint8_t(bootra::Verdict::Arm);
    s.move = {true, 650, 2, 1, 0, 3300, 150};
    s.notes[0] = {3, 0, 2};
    s.notes[1] = {4, 1, 3};
    s.notes[2] = {652, 0, 1};
    s.notes[3] = {50, 2, 2};     // readbacks
    s.notes[4] = {600, 2, 2};
    s.notes[5] = {3001, 2, 1};
    s.note_n = 6;
    s.t_ms = 4321; s.vbus_mv = 5012; s.cc1_mv = 3290; s.cc2_mv = 0; s.plus_now = true;
    return s;
}

int main() {
    char buf[kQrMax + 1];
    const Snapshot s = plus_boot();
    const std::size_t n = format(s, buf);
    const std::string_view t(buf, n);
    std::printf("%.*s\n", int(n), buf);   // the example the report quotes
    CHECK_EQ(n, std::strlen(buf));
    CHECK(n <= kQrMax);
    // The firmware identity comes first, the same strings PING answers (version, git describe with -dirty, proto).
    CHECK(t.starts_with("K2B1 fw=1.0 g=a1b2c3d-dirty p=1 t=4321 vb=5012 c1=3290 c2=0 pl=1 m=plus "));
    CHECK(field(t, "fw") == "1.0");
    CHECK(field(t, "g") == "a1b2c3d-dirty");
    CHECK(field(t, "p") == "1");
    CHECK(field(t, "m") == "plus");
    CHECK(field(t, "k") == "8");
    CHECK(field(t, "pa") == "D");
    CHECK(field(t, "rf") == "24000000");
    CHECK(field(t, "w") == "0");
    CHECK(field(t, "pin") == "2,1,1,0");
    CHECK(field(t, "mk") == "-");
    CHECK(field(t, "ra") == "1:arm");        // the Ra is on, and the boot armed an identity for it
    CHECK(field(t, "mv") == "650:2>1:0:3300:150");
    CHECK(field(t, "sw") == "2@50,2@600,1@3001");
    CHECK(field(t, "n") == "3s2,4c3,652s1");
    CHECK(field(t, "absent").empty());

    // No move, no readbacks yet, a warm plain boot that consulted the pin mark.
    {
        Snapshot p;
        std::strcpy(p.git, "0000000");
        std::strcpy(p.fw, "dev");
        p.proto = 1;
        p.mode = bootmode::Mode::Plain;
        // clean build: no -dirty suffix
        {
            char c[kQrMax + 1];
            const std::string_view v(c, format(p, c));
            CHECK(v.starts_with("K2B1 fw=dev g=0000000 p=1 t=0 "));
        }
        p.warm = true; p.mark_read = true; p.mark = 0xB502;
        p.pin_initial = 2; p.pin_now = 2; p.pin_boot = 2; p.inherited = true;
        p.vbus_mv = 20000;
        char b[kQrMax + 1];
        const std::size_t len = format(p, b);
        const std::string_view u(b, len);
        CHECK(field(u, "m") == "plain");
        CHECK(field(u, "w") == "1");
        CHECK(field(u, "mk") == "B502");
        CHECK(field(u, "pin") == "2,2,2,1");
        CHECK(field(u, "mv") == "-");
        CHECK(field(u, "sw") == "-");
        CHECK(field(u, "n") == "-");
        CHECK(field(u, "vb") == "20000");
    }
    // A boot that put the Ra on: the CC routing note carries PC13 (bit 2) before any caps note, and ra= says why.
    {
        Snapshot p = plus_boot();
        p.note_n = 0;
        p.notes[p.note_n++] = {3, 0, 2};
        p.notes[p.note_n++] = {20, 1, 6};      // CC routing: CC2 switch closed + PC13 high
        p.ra_why = std::uint8_t(bootra::Verdict::Refused);
        p.ra_on = false;
        char b[kQrMax + 1];
        const std::string_view u(b, format(p, b));
        CHECK(field(u, "n") == "3s2,20c6");
        CHECK(field(u, "ra") == "0:refused");
        Snapshot q;                               // no decision made: "-"
        char c[kQrMax + 1];
        CHECK(field(std::string_view(c, format(q, c)), "ra") == "0:-");
    }
    // Vbus -1 in the move (no power monitor answer) stays readable.
    {
        Snapshot p = plus_boot();
        p.move.vbus_mv = -1;
        char b[kQrMax + 1];
        const std::string_view u(b, format(p, b));
        CHECK(field(u, "mv") == "650:2>1:-1:3300:150");
    }
    // Many notes: the line never passes the cap and says how many did not fit.
    {
        Snapshot p = plus_boot();
        p.note_n = 0;
        for (unsigned i = 0; i < 24; i++) p.notes[p.note_n++] = {std::uint16_t(1000 + i), std::uint8_t(i & 1), std::uint8_t(i & 3)};
        p.notes[p.note_n++] = {50, 2, 2};
        p.notes[p.note_n++] = {600, 2, 2};
        p.notes[p.note_n++] = {3001, 2, 1};
        char b[kQrMax + 1];
        const std::size_t len = format(p, b);
        CHECK(len <= kQrMax);
        const std::string_view u(b, len);
        CHECK(field(u, "sw") == "2@50,2@600,1@3001");       // the readbacks always fit
        const std::string_view nn = field(u, "n");
        CHECK(nn.find('+') != std::string_view::npos);
        CHECK(nn.starts_with("1000s0,1001c1"));
    }
    // A small output buffer cuts cleanly, NUL terminated.
    {
        char tiny[20];
        const std::size_t len = format(s, tiny);
        CHECK(len <= 19);
        CHECK_EQ(tiny[len], 0);
        CHECK(std::string_view(tiny, len).starts_with("K2B1 fw=1.0 g=a1b2"));
    }
    // A long git-describe string (tag + distance + hash + -dirty) is kept whole, the identity never shrinks.
    {
        Snapshot p = plus_boot();
        std::strcpy(p.git, "v1.0-12-g1234567-dirty");
        char b[kQrMax + 1];
        const std::string_view u(b, format(p, b));
        CHECK(field(u, "g") == "v1.0-12-g1234567-dirty");
        CHECK(field(u, "p") == "1");
    }
    return test_report("test_bootinfo");
}
