// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host side of the Black Box diagnosis: walks a saved capture (pdtrace records back to back, resyncing over damage)
// merged by time with a power-window file, and reports each session as it finishes. Shared by tools/bbdiag.cpp and the
// fixture tests, so both see the sessions the same way.
//
// Power-window file: 20-byte little-endian records, one per power.jsonl row (pd_trace.py diagnose writes it):
//   t_ms u32 (K2 uptime at the window's last sample, the trace clock in ms), span_ms u16, n u16, vbus_mv u16,
//   ibus_ma i16, vbus_min u16, vbus_max u16, ibus_min i16, ibus_max i16.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "k2/pd/diag.hpp"
#include "k2/pd/pdtrace.hpp"

namespace k2::bbdiag {

struct TimedWindow {
    std::uint32_t ts_us = 0;   // the trace clock (wraps like it)
    pd::diag::Window w;
};

inline constexpr std::size_t kWindowRecord = 20;

inline std::vector<TimedWindow> parse_windows(std::span<const std::uint8_t> b) {
    const auto u16 = [&b](std::size_t i) { return std::uint16_t(b[i] | b[i + 1] << 8); };
    std::vector<TimedWindow> out;
    for (std::size_t i = 0; i + kWindowRecord <= b.size(); i += kWindowRecord) {
        TimedWindow t;
        const std::uint32_t ms = b[i] | b[i + 1] << 8 | b[i + 2] << 16 | std::uint32_t(b[i + 3]) << 24;
        t.ts_us = ms * 1000u;
        t.w.span_ms = u16(i + 4);
        t.w.n = u16(i + 6);
        t.w.mv = u16(i + 8);
        t.w.ma = std::int16_t(u16(i + 10));
        t.w.vmin = u16(i + 12);
        t.w.vmax = u16(i + 14);
        t.w.imin = std::int16_t(u16(i + 16));
        t.w.imax = std::int16_t(u16(i + 18));
        out.push_back(t);
    }
    return out;
}

// Feeds `a` with every record and window in time order; `on_finish()` runs after each finished session.
template <class F>
std::size_t walk(pd::diag::Analyzer& a, std::span<const std::uint8_t> b, std::span<const TimedWindow> wins, F on_finish) {
    std::uint16_t seen = 0;
    std::size_t skipped = 0, wi = 0;
    const auto check = [&] {
        if (a.finished() != seen) { seen = a.finished(); on_finish(); }
    };
    for (std::size_t i = 0; i + pdtrace::kHeader <= b.size();) {
        const std::uint8_t len = b[i];
        if (len < pdtrace::kHeader || len > pdtrace::kMaxRecord || i + len > b.size()) { i++; skipped++; continue; }
        const std::uint32_t ts = b[i + 2] | b[i + 3] << 8 | b[i + 4] << 16 | std::uint32_t(b[i + 5]) << 24;
        // windows that ended before this record (wrap-safe: the two streams run side by side)
        for (; wi < wins.size() && std::int32_t(wins[wi].ts_us - ts) <= 0; wi++) {
            a.power_window(wins[wi].ts_us, wins[wi].w);
            check();
        }
        a.feed_record(b[i + 1], ts, {b.data() + i + pdtrace::kHeader, std::size_t(len - pdtrace::kHeader)});
        i += len;
        check();
    }
    // windows after the last record: only those still inside the capture's session matter, the rest are skipped
    // (a power log usually runs on after the trace was saved)
    for (; wi < wins.size() && a.current().open; wi++) {
        a.power_window(wins[wi].ts_us, wins[wi].w);
        check();
    }
    return skipped;
}

}  // namespace k2::bbdiag
