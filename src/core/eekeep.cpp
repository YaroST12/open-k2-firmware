// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The settings store's users on the hardware, see k2/core/eekeep.hpp.
#include "k2/core/eekeep.hpp"

#include "k2/core/log.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/drivers/eeprom.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/core/strpack.hpp"

namespace k2::eekeep {
namespace {

constexpr std::uint32_t kRetryMs = 5000;   // a failed write (no chip, a NACK) waits this long before the next try

struct State {
    bool usb_valid = false;          // the slot held a valid record at boot (or since a write)
    std::uint8_t usb_stored = 0;
    std::uint8_t usb_next = 0;
    bool usb_dirty = false;
    ctcable::Choice spoof_stored{};  // Off = blank or damaged
    ctcable::Choice user{};
    std::uint32_t user_gen = 0;
    bool user_set = false;
    std::uint32_t retry_at = 0;
    bool backoff = false;
};
State g;

void service() { fusb302::service(); }

bool put(eeprom::Slot s, std::span<const std::uint8_t> payload) {
    const eeprom::PageErr e = eeprom::store_write(s, payload, &service);
    const bool ok = e == eeprom::PageErr::Ok || e == eeprom::PageErr::Same;
    K2LOG("[EEKEEP] slot %u written: err %u\n", static_cast<unsigned>(s), static_cast<unsigned>(e));
    return ok;
}

}  // namespace

void init() {
    std::uint8_t b[eeprom::kSlotPayload];
    if (eeprom::store_read(eeprom::Slot::UsbLink, b) == 1) { g.usb_valid = true; g.usb_stored = g.usb_next = b[0]; }
    const int n = eeprom::store_read(eeprom::Slot::Spoof, b);
    if (n >= 0) {
        if (const auto c = spoof_choice(std::span<const std::uint8_t>(b, std::size_t(n)))) g.spoof_stored = *c;
    }
    K2LOG("[EEKEEP] usb %s %x, spoof %u/%u\n", g.usb_valid ? "kept" : "none", static_cast<unsigned>(g.usb_stored),
          static_cast<unsigned>(g.spoof_stored.kind), static_cast<unsigned>(g.spoof_stored.seq));
}

std::optional<std::uint8_t> usb_flags() {
    if (!g.usb_valid) return std::nullopt;
    return g.usb_stored;
}

void usb_want(std::uint8_t flags) {
    g.usb_next = flags;
    g.usb_dirty = !g.usb_valid || flags != g.usb_stored;
}

void spoof_restore() {
    const ctcable::Choice c = g.spoof_stored;
    if (c.kind == ctcable::Kind::Off) return;
    if (emk_spoof::active()) {
        K2LOG("[EEKEEP] spoof %u/%u not restored: another identity is armed\n", static_cast<unsigned>(c.kind), static_cast<unsigned>(c.seq));
        return;   // poll() clears the slot: the user's arm is no longer the one answering
    }
    const bool ok = rdcomeback::arm_identity(c);
    K2LOG("[EEKEEP] spoof %u/%u %s\n", static_cast<unsigned>(c.kind), static_cast<unsigned>(c.seq), ok ? K2_P("re-armed") : K2_P("gone, slot cleared"));
    if (ok) spoof_user_armed(c);
}

void spoof_user_armed(ctcable::Choice c) {
    g.user = ctcable::canonical(c);
    g.user_gen = emk_spoof::generation();
    g.user_set = true;
}

ctcable::Choice spoof_user() {
    return g.user_set ? spoof_wanted(emk_spoof::active(), emk_spoof::generation(), g.user_gen, g.user) : ctcable::Choice{};
}

void poll(std::uint32_t now_ms, bool may_write) {
    if (!may_write) return;
    if (g.backoff && static_cast<std::int32_t>(now_ms - g.retry_at) < 0) return;
    g.backoff = false;
    bool ok = true;
    if (g.usb_dirty) {
        const std::uint8_t f = g.usb_next;
        if (put(eeprom::Slot::UsbLink, {&f, 1})) { g.usb_valid = true; g.usb_stored = f; g.usb_dirty = false; }
        else ok = false;
    }
    const ctcable::Choice want = spoof_user();
    if (ok && !(want == g.spoof_stored)) {
        const auto p = spoof_payload(want);
        if (want.kind == ctcable::Kind::Off ? put(eeprom::Slot::Spoof, {}) : put(eeprom::Slot::Spoof, p)) g.spoof_stored = want;
        else ok = false;
    }
    if (!ok) { g.backoff = true; g.retry_at = now_ms + kRetryMs; }
}

}  // namespace k2::eekeep
