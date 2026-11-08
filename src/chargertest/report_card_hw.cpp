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
Store g_store(fs::instance());
std::uint32_t g_gen = 0;
Err written(Err e) { if (e == Err::Ok) ++g_gen; return e; }
}  // namespace

std::size_t list(std::span<Card> out) { return g_store.list(out); }
std::size_t numbers(std::span<std::uint16_t> out) { return g_store.numbers(out); }
std::uint32_t generation() { return g_gen; }
bool find(std::uint32_t fp, Card& c) { return g_store.find(fp, c); }
bool get(std::uint16_t card_no, Card& c) { return g_store.get(card_no, c); }
Err put(Card& c) {
    const Err e = g_store.put(c);
    K2LOG("[CARD] save #%u fp=%08x runs=%u -> %d\n", unsigned(c.card_no), unsigned(c.id.fp), unsigned(c.runs), int(e));
    return written(e);
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
