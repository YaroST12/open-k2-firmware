// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store bound to the real flash, see k2/chargertest/report_card_hw.hpp.
#include "k2/core/log.hpp"
#include "k2/chargertest/report_card_hw.hpp"

#include "k2/drivers/fs_flash.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/core/version.hpp"

namespace k2::report_card::hw {
namespace {
Store g_store(fs::card_ring());
std::uint32_t g_gen = 0;
std::uint32_t g_erase_ms = 0;
bool g_erased = false;
constexpr std::uint32_t kEraseGapMs = 1000;   // between two maintenance erases (each stops the main loop ~35 ms)
Err written(Err e) {
    if (e == Err::Ok) ++g_gen;
    else if (e == Err::Full || e == Err::Flash) K2LOG("[CARD] write refused: %s (blank=%u dirty=%u)\n", e == Err::Full ? "ring full, waits for maintenance" : "no ring",
               fs::card_ring().blank(), fs::card_ring().dirty());
    return e;
}
}  // namespace

void poll(bool may_erase) {
    if (!may_erase || !fs::rings_ready() || !g_store.needs_maintenance()) return;
    const std::uint32_t now = systick::millis();
    if (g_erased && now - g_erase_ms < kEraseGapMs) return;
    g_erase_ms = now;
    g_erased = true;
    const bool ok = g_store.maintain(true);
    K2LOG("[CARD] ring maintenance: %s blank=%u dirty=%u live=%u\n", ok ? "erased" : "nothing erased", fs::card_ring().blank(),
          fs::card_ring().dirty(), fs::card_ring().live());
}

std::size_t list(std::span<Card> out) { return g_store.list(out); }
std::size_t numbers(std::span<std::uint16_t> out) { return g_store.numbers(out); }
std::uint32_t generation() { return g_gen; }
bool find(std::uint32_t fp, Card& c) { return g_store.find(fp, c); }
bool get(std::uint16_t card_no, Card& c) { return g_store.get(card_no, c); }
[[gnu::noinline]] Err put(Card& c, Card& prev, bool& had) {   // the caller owns the previous Card: no second one on this stack
    const Err e = g_store.put(c, prev, had);
    K2LOG("[CARD] save #%u fp=%08x runs=%u times=%u -> %d\n", unsigned(c.card_no), unsigned(c.id.fp), unsigned(c.runs), unsigned(c.times), int(e));
    return written(e);   // Full: the run's card is lost (the ring's spare is kept blank while idle, so only a ring that never idled)
}
Err put(Card& c) {
    Card prev;
    bool had = false;
    return put(c, prev, had);
}
Err rename(std::uint16_t card_no, std::string_view name) { return written(g_store.rename(card_no, name)); }
Err remove(std::uint16_t card_no) { return written(g_store.remove(card_no)); }
Context context() {
    Context c;
    c.fw = fw_hash(kGitHash);
    c.boots = session::boots();
    c.uptime_s = systick::millis() / 1000;
    return c;
}

}  // namespace k2::report_card::hw
