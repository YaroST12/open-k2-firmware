// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/resume_text.hpp"
#include <cstdio>
#include <string_view>
using namespace k2::resumetext;
static int g_n = 0, g_bad = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static In combo(unsigned m) {
    In in;
    in.self_now = m & 1; in.gate_ready = m & 2; in.rd_on = m & 4; in.wait_over = m & 8; in.behind_cable = m & 16; in.held = m & 32;
    return in;
}

int main() {
    // Every state fits its field (status 20, row 23, note 31, footer 27): the screenshot of 2026-11-07 cut
    // "Self-powered, VBUS lost" (23) to "Self-powered, VBUS l..." and the footer (37) at the edge.
    for (unsigned m = 0; m < 64; ++m) {
        const In in = combo(m);
        CHECK(!status(in).empty() && status(in).size() <= kStatusMax);
        CHECK(!row(in).empty() && row(in).size() <= kRowMax);
        CHECK(!note(in).empty() && note(in).size() <= kNoteMax);
        CHECK(!footer(in).empty() && footer(in).size() <= kFooterMax);
    }
    // "Self-powered" only when the K2 is self-supplied now.
    for (unsigned m = 0; m < 64; ++m) {
        const In in = combo(m);
        CHECK((status(in).find("Self-powered") != std::string_view::npos) == in.self_now);
        for (const auto s : {row(in), note(in), footer(in)}) CHECK(s.find("Self-powered") == std::string_view::npos);
    }
    CHECK(status(In{}) == "Rebooted mid-run");
    // The row says what the resume waits for.
    In in;
    CHECK(row(in) == "Waiting for stable 5V");
    in.rd_on = true;
    CHECK(row(in) == "Rd on, awaiting charger");
    in.wait_over = true;
    CHECK(row(in) == "Waiting for stable 5V");
    in.behind_cable = true;
    CHECK(row(in) == "Needs a direct plug");
    in.held = true;
    CHECK(row(in) == "Press OK to try again");
    in.gate_ready = true;
    CHECK(row(in) == "VBUS 5V stable: ready");
    std::printf("%d checks, %d failed\n", g_n, g_bad);
    return g_bad != 0;
}
