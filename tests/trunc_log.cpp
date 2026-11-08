// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host-only recorder of text that was cut or clipped on its way to the screen (include/k2/ui/trunc.hpp).
// Linked into every host test binary; test_report() (test_util.h) asks k2_trunc_failures() and fails the test
// when any copy was truncated while it drove the UI.
#include "k2/ui/trunc.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace k2::trunc {
namespace {

struct Hit {
    Kind kind;
    std::string text, where, ctx;
    int max_px;
    int count;
};

std::vector<Hit> g_hits;
std::string g_ctx;
int g_pause = 0;

const char *basename_of(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/') base = p + 1;
    return base;
}

}  // namespace

Pause::Pause() { ++g_pause; }
Pause::~Pause() { --g_pause; }

void context(const char *label) { g_ctx = label ? label : ""; }

void note(Kind kind, std::string_view text, int max_px, const char *file, unsigned line) {
    if (g_pause > 0) return;
    std::string where = file ? std::string(basename_of(file)) + ":" + std::to_string(line) : std::string("?");
    for (Hit &h : g_hits)
        if (h.kind == kind && h.max_px == max_px && h.text == text && h.where == where && h.ctx == g_ctx) {
            ++h.count;
            return;
        }
    g_hits.push_back({kind, std::string(text), where, g_ctx, max_px, 1});
}

static int count_of(bool data) {
    int n = 0;
    for (const Hit &h : g_hits) n += (h.kind == Kind::Data) == data;
    return n;
}

int failures() { return count_of(false); }
int data_hits() { return count_of(true); }
void clear() { g_hits.clear(); }

void print_report(bool with_data) {
    for (const Hit &h : g_hits) {
        if (h.kind == Kind::Data && !with_data) continue;
        printf("TRUNCATED %s \"%s\" (%d chars, %d px box) at %s%s%s\n", h.kind == Kind::Clip ? "clip" : h.kind == Kind::Data ? "data" : "cut",
               h.text.c_str(), text_cells(h.text), h.max_px, h.where.c_str(), h.ctx.empty() ? "" : " in ",
               h.ctx.c_str());
    }
}

}  // namespace k2::trunc

// test_report() in test_util.h calls this when it is linked (weak reference there): prints every truncated copy
// string and returns how many distinct ones there were, so the test fails and names them.
extern "C" int k2_trunc_failures() {
    const int n = k2::trunc::failures();
    if (n) {
        k2::trunc::print_report();
        printf("%d truncated copy string%s: reword to fit (docs/UI_DESIGN.md section 6), never cut with \"...\"\n", n,
               n == 1 ? "" : "s");
    }
    return n;
}
