// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Black Box diagnosis on the K2, see k2/pd/diag_live.hpp. Main loop only (pdtrace writes records there too).
#include "k2/pd/diag_live.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "k2/core/storage.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/twin/twin.hpp"

namespace k2::pd::diag::live {

namespace {

Analyzer g_an;
WindowAcc g_acc;
Report g_report;
std::uint32_t g_eval_ms;
bool g_evaluated;

// The device twin matched to the shown session's sink identity, looked up once per (session, identity).
twin::Twin g_twin;
TwinHint g_hint;
bool g_hint_ok;
std::uint16_t g_twin_key_n = 0xFFFF, g_twin_vid, g_twin_pid;

std::uint32_t g_cursor;
bool g_have_cursor;

// The ring's new records since the last call (8 KB: it never wraps between two main-loop passes). A frozen ring
// takes no records, so the diagnosis pauses with it.
void drain_ring() {
    const auto& ring = pdtrace::live_ring();
    if (!g_have_cursor) { g_cursor = ring.tail(); g_have_cursor = true; }
    std::array<std::uint8_t, 4 * pdtrace::kMaxRecord> buf;
    for (unsigned guard = 0; guard < 64 && g_cursor != ring.head(); guard++) {
        const auto r = ring.read(g_cursor, buf);
        if (r.bytes == 0 && r.next == g_cursor) break;
        g_an.feed({buf.data(), r.bytes});
        g_cursor = r.next;
    }
}

std::uint16_t clamp16(std::int32_t v) { return std::uint16_t(std::clamp<std::int32_t>(v, 0, 0xFFFF)); }
std::int16_t clamp_s16(std::int32_t v) { return std::int16_t(std::clamp<std::int32_t>(v, -32768, 32767)); }

const TwinHint* match_twin(const Session& s) {
    if (s.snk_vid == 0) return nullptr;
    if (g_twin_key_n == g_an.finished() && g_twin_vid == s.snk_vid && g_twin_pid == s.snk_pid)
        return g_hint_ok ? &g_hint : nullptr;
    g_twin_key_n = g_an.finished();
    g_twin_vid = s.snk_vid;
    g_twin_pid = s.snk_pid;
    g_hint_ok = false;
    std::array<std::uint8_t, twin::kCapacity> seqs{};
    const unsigned n = storage::twin_list(seqs);
    for (unsigned i = 0; i < n && !g_hint_ok; i++) {
        if (!storage::twin_get(seqs[i], g_twin)) continue;
        if (g_twin.vid != s.snk_vid || g_twin.pid != s.snk_pid || g_twin.num_pdos == 0) continue;
        g_hint = TwinHint{};
        g_hint.name = g_twin.name.data();
        g_hint.pdos = std::span<const std::uint32_t>(g_twin.pdos.data(), g_twin.num_pdos);
        g_hint.host = (g_twin.id_hi >> 8 & 1) != 0;   // ID Header bit 31: USB communications host
        g_hint.vid = g_twin.vid;
        g_hint.pid = g_twin.pid;
        g_hint_ok = true;
    }
    return g_hint_ok ? &g_hint : nullptr;
}

}  // namespace

void poll(std::uint32_t now_ms) {
    drain_ring();
    const std::uint32_t us = systick::micros();
    if (const auto m = power::read_raw()) {
        Window w;
        if (g_acc.add(now_ms, clamp16(m->vbus_mv), clamp_s16(m->current_ma), w)) g_an.power_window(us, w);
    }
    g_an.advance(us);
}

const Report& report() {
    const std::uint32_t now = systick::millis();
    if (!g_evaluated || now - g_eval_ms >= kRefreshMs) {
        const Session& s = g_an.shown();
        g_report = evaluate(s, match_twin(s), g_an.now_ms());
        g_eval_ms = now;
        g_evaluated = true;
    }
    return g_report;
}

State state() {
    if (g_an.current().open) return State::Open;
    return g_an.has_last() ? State::Last : State::None;
}

std::uint16_t sessions() { return g_an.finished(); }

std::uint32_t session_ms() {
    const Session& s = g_an.shown();
    if (!s.open && !g_an.has_last()) return 0;
    return (s.open ? g_an.now_ms() : s.end_ms) - s.start_ms;
}

void verdict_text(std::span<char> out) {
    if (out.empty()) return;
    if (state() == State::None) {
        constexpr char kNone[] = "No PD session yet";
        const std::size_t n = std::min(out.size() - 1, sizeof kNone - 1);
        std::memcpy(out.data(), kNone, n);
        out[n] = 0;
        return;
    }
    verdict(report(), out);
}

}  // namespace k2::pd::diag::live
