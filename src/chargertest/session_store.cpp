// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// See k2/chargertest/session_store.hpp. BPR at 0x40006C00: DR1..DR10 at 0x04 + 4 * (n - 1), DR11..DR42 at
// 0x40 + 4 * (n - 11), 16 bits each. This store uses DR22..DR37 (0x6C..0xA8).
#include "k2/core/log.hpp"
#include "k2/chargertest/session_store.hpp"

#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::session {
namespace {

constexpr std::uint32_t kBpr = 0x40006C00;
constexpr std::uint32_t kPwr = 0x40007000;
constexpr std::uint32_t kFirstOffset = 0x6C;          // DR22
constexpr std::uint32_t kApb1enPwr = 1u << 28, kApb1enBpr = 1u << 27;
constexpr std::uint32_t kPwrDbp = 1u << 8;            // disable backup-domain write protection

volatile std::uint32_t& bpr(std::size_t i) { return *reinterpret_cast<volatile std::uint32_t*>(kBpr + kFirstOffset + 4 * i); }

std::optional<Record> g_prev;
Record g_last;            // what the backup registers hold now
bool g_ready;
std::uint32_t g_last_write_ms;

std::array<std::uint16_t, kWords> load() {
    std::array<std::uint16_t, kWords> w{};
    for (std::size_t i = 0; i < kWords; i++) w[i] = std::uint16_t(bpr(i));
    return w;
}

void store(const Record& r) {
    const auto w = encode(r);
    for (std::size_t i = 0; i < kWords; i++) bpr(i) = w[i];
    g_last = r;
}

Record sample() {
    Record r;
    const auto& caps = fusb302::caps();
    const auto& c = fusb302::contract();
    const auto& id = fusb302::identify_status();
    r.cc = fusb302::active_cc();
    r.caps_valid = caps.valid;
    if (caps.valid) {
        r.num_pdos = caps.num_pdos;
        r.spec_rev = caps.spec_rev;
        for (std::size_t i = 0; i < kPdos && i < caps.pdos.size(); i++) r.pdo[i] = caps.pdos[i].raw;
    }
    r.contract_valid = c.valid;
    r.contract_ready = c.ready;
    if (c.valid) {
        r.pos = c.pos;
        r.mv = c.mv;
        r.op_ma = c.op_ma;
        r.max_ma = c.max_ma;
    }
    r.id_state = static_cast<std::uint8_t>(id.state);
    r.id_step = static_cast<std::uint8_t>(id.step);
    r.id_ok_mask = id.ok_mask;
    r.identify_running = id.state == pd::IdState::Running;
    return r;
}

}  // namespace

void init() {
    reg::crm().APB1EN |= kApb1enPwr | kApb1enBpr;
    *reinterpret_cast<volatile std::uint32_t*>(kPwr) |= kPwrDbp;
    g_prev = decode(load());
    if (g_prev) {
        char line[96];
        summarize(*g_prev, line, sizeof line);
        K2LOG("[SESS] previous session #%u: %s\n", static_cast<unsigned>(g_prev->boots), line);
        for (std::size_t i = 0; i < kPdos; i++)
            if (g_prev->pdo[i]) K2LOG("[SESS]   pdo%u=%08X\n", static_cast<unsigned>(i + 1), static_cast<unsigned>(g_prev->pdo[i]));
        g_last = *g_prev;
    } else {
        K2LOG("[SESS] no previous session record\n");
        g_last = Record{};
    }
    Record fresh;
    fresh.boots = std::uint16_t(g_last.boots + 1);
    g_last = fresh;
    store(fresh);
    g_ready = true;
    g_last_write_ms = systick::millis();
}

void poll() {
    if (!g_ready) return;
    const std::uint32_t now = systick::millis();
    if (now - g_last_write_ms < kMinGapMs) return;
    Record r = sample();
    r.boots = g_last.boots;
    if (r == g_last) return;
    store(r);
    g_last_write_ms = now;
}

void wipe() {
    g_ready = false;                                   // poll() must not write the record back
    for (std::size_t i = 0; i < kWords; i++) bpr(i) = 0;
    g_last = Record{};
}

const std::optional<Record>& previous() { return g_prev; }
std::uint16_t boots() { return g_last.boots; }

}  // namespace k2::session
