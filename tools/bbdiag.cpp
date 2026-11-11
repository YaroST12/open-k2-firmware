// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host build of the Black Box diagnosis (k2/pd/diag.hpp): the same analyser the K2 runs, fed from a saved capture.
//
// Usage: bbdiag FILE [--json] [--power WINDOWS] [--twin-name NAME --twin-pdos HEX,HEX.. [--twin-vid V --twin-pid P]
//               [--assumed]]
//   FILE: a k2_stream trace.raw or a `pd_trace.py dump --raw` file (pdtrace records back to back).
//   WINDOWS: power-window records (bbdiag_walk.hpp), written by pd_trace.py diagnose from a power.jsonl.
// Prints every finished session, then the open one. pd_trace.py diagnose builds and runs this.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bbdiag_walk.hpp"
#include "k2/pd/diag.hpp"

using namespace k2;
using namespace k2::pd::diag;

namespace {

bool g_json = false;
unsigned g_n = 0;

void json_str(const char* s) {
    std::putchar('"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') std::putchar('\\');
        std::putchar(*s);
    }
    std::putchar('"');
}

const char* sev_name(Sev s) {
    switch (s) {
    case Sev::Fail: return "fail";
    case Sev::Warn: return "warn";
    case Sev::Info: return "info";
    default: return "ok";
    }
}

void print(const Report& r, bool live) {
    g_n++;
    const unsigned n = line_count(r);
    if (g_json) {
        std::printf("%s{\"session\":%u,\"live\":%s,\"start_s\":%.3f,\"dur_ms\":%u,\"findings\":[", g_n > 1 ? ",\n" : "", g_n,
                    live ? "true" : "false", r.start_us / 1e6, r.dur_ms);
        for (unsigned i = 0; i < r.n; i++) {
            Line t, no;
            line(r, 1 + 2 * i, t);
            line(r, 2 + 2 * i, no);
            std::printf("%s{\"code\":\"%s\",\"sev\":\"%s\",\"t_ms\":%u,\"title\":", i ? "," : "", code_name(r.f[i].code),
                        sev_name(r.f[i].sev), r.f[i].t_ms);
            json_str(t.text.data());
            std::printf(",\"note\":");
            json_str(no.text.data());
            std::putchar('}');
        }
        std::printf("],\"lines\":[");
        for (unsigned i = 0; i < n; i++) {
            Line l;
            line(r, i, l);
            if (i) std::putchar(',');
            json_str(l.text.data());
        }
        std::printf("]}");
        return;
    }
    std::printf("== session %u at %.1f s, %u.%u s%s\n", g_n, r.start_us / 1e6, r.dur_ms / 1000, r.dur_ms % 1000 / 100,
                live ? " (still open)" : "");
    for (unsigned i = 0; i < n; i++) {
        Line l;
        if (!line(r, i, l)) continue;
        switch (l.kind) {
        case LineKind::Head: std::printf("%s\n", l.text.data()); break;
        case LineKind::Title: std::printf("%u %-5s %s\n", l.num, sev_name(l.sev), l.text.data()); break;
        case LineKind::Note: std::printf("        %s\n", l.text.data()); break;
        default: std::printf("  %s\n", l.text.data()); break;
        }
    }
    std::printf("\n");
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::vector<std::uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::uint8_t buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    const char* twin_name = nullptr;
    const char* power = nullptr;
    std::vector<std::uint32_t> pdos;
    TwinHint twin;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--json")) g_json = true;
        else if (!std::strcmp(argv[i], "--assumed")) twin.assumed = true;
        else if (!std::strcmp(argv[i], "--twin-host")) twin.host = true;
        else if (!std::strcmp(argv[i], "--twin-name") && i + 1 < argc) twin_name = argv[++i];
        else if (!std::strcmp(argv[i], "--power") && i + 1 < argc) power = argv[++i];
        else if (!std::strcmp(argv[i], "--twin-vid") && i + 1 < argc) twin.vid = std::uint16_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--twin-pid") && i + 1 < argc) twin.pid = std::uint16_t(std::strtoul(argv[++i], nullptr, 0));
        else if (!std::strcmp(argv[i], "--twin-pdos") && i + 1 < argc) {
            for (char* p = argv[++i]; *p;) {
                char* end;
                pdos.push_back(std::uint32_t(std::strtoul(p, &end, 16)));
                p = *end ? end + 1 : end;
            }
        } else if (argv[i][0] != '-') path = argv[i];
        else { std::fprintf(stderr, "bbdiag: unknown option %s\n", argv[i]); return 2; }
    }
    if (!path) {
        std::fprintf(stderr, "usage: bbdiag FILE [--json] [--power WINDOWS] [--twin-name N --twin-pdos HEX,..] [--assumed]\n");
        return 2;
    }
    const auto b = read_file(path);
    if (b.empty()) { std::fprintf(stderr, "bbdiag: cannot read %s\n", path); return 1; }
    twin.name = twin_name ? twin_name : "twin";
    twin.pdos = pdos;
    const TwinHint* hint = pdos.empty() ? nullptr : &twin;

    std::vector<bbdiag::TimedWindow> wins;
    if (power) {
        const auto pb = read_file(power);
        if (pb.empty()) { std::fprintf(stderr, "bbdiag: no power windows in %s\n", power); return 1; }
        wins = bbdiag::parse_windows(pb);
    }

    static Analyzer a;   // large-ish: not on the stack
    if (g_json) std::printf("[");
    const std::size_t skipped =
        bbdiag::walk(a, b, wins, [&] { print(evaluate(a.last(), hint, a.now_ms()), false); });
    if (a.current().open) print(evaluate(a.current(), hint, a.now_ms()), true);
    if (g_json) std::printf("]\n");
    if (g_n == 0 && !g_json) std::printf("no session in %s\n", path);
    if (skipped) std::fprintf(stderr, "bbdiag: %zu bytes skipped to resync\n", skipped);
    return 0;
}
