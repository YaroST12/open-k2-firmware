// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emk_ra.hpp"
#include <cstdio>
#include <string>
using namespace k2::emkra;
static int g_n = 0, g_bad = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    // PC13 is the Ra on CC1: high whenever the mode holds it or the (experimental) VCONN rail wants it
    CHECK(!pc13_high(false, false));
    CHECK(pc13_high(true, false) && pc13_high(false, true) && pc13_high(true, true));
    CHECK(kLine == 2);   // the scan may take only CC2 while the Ra is on CC1

    // enabling: nothing inline, no contract or tool running, no pinned Rd, no VCONN role
    CHECK(check({}) == Refuse::None);
    CHECK(check({.inline_sink = true}) == Refuse::Inline);
    CHECK(check({.contract = true}) == Refuse::Contract);
    CHECK(check({.busy = true}) == Refuse::Busy);
    // A boot pin (bit0 = Rd on CC1, bit1 = Rd on CC2): the Ra sits on CC1, so only a pin that reaches CC1 collides with it.
    // PLUS pins CC2 alone, which is exactly the Rd line the mode wants: a charger-powered (PLUS) run may spoof.
    CHECK(check({.pinned_rd = 0}) == Refuse::None);
    CHECK(check({.pinned_rd = 2}) == Refuse::None);
    CHECK(check({.pinned_rd = 1}) == Refuse::PinnedRd);    // OK key: Rd on CC1
    CHECK(check({.pinned_rd = 3}) == Refuse::PinnedRd);    // both lines (no longer reached, still refused)
    CHECK(check({.inline_sink = true, .pinned_rd = 2}) == Refuse::Inline);   // the other reasons still hold with the PLUS pin
    CHECK(check({.contract = true, .pinned_rd = 2}) == Refuse::Contract);
    CHECK(check({.pinned_rd = 2, .vconn = true}) == Refuse::Vconn);
    CHECK(check({.vconn = true}) == Refuse::Vconn);
    CHECK(check({.inline_sink = true, .contract = true}) == Refuse::Inline);   // the first reason wins
    CHECK(std::string(refuse_name(Refuse::None)) == "ok" && std::string(refuse_name(Refuse::Contract)) == "contract");

    // Rd while the mode is on: CC2 alone (never CC1, where the Ra sits), none with a sink inline
    CHECK(run_rd_lines(true, false, 0) == 2 && run_rd_lines(true, false, 2) == 2 && run_rd_lines(true, false, 1) == 2);
    CHECK(run_rd_lines(true, true, 0) == 0);
    // the mode off: the run start's rule (both lines while none is detected, else the Rp line)
    CHECK(run_rd_lines(false, false, 0) == 3 && run_rd_lines(false, false, 1) == 1 && run_rd_lines(false, false, 2) == 2);
    CHECK(run_rd_lines(false, true, 0) == 0);

    // detach ends the mode, except inside a charger-test run that owns it (its units detach and re-attach)
    CHECK(!survives_detach(false) && survives_detach(true));
    // VCONN switches never close while the mode is on (the chip's supply on CC1 would fight the Ra)
    CHECK(vconn_switch_allowed(false) && !vconn_switch_allowed(true));

    std::printf("%d checks, %d failed\n", g_n, g_bad);
    return g_bad != 0;
}
