// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The settings store's two users (k2/drivers/eeprom.hpp: one 16-byte slot each in the 24C16's 0x500..0x7DF). The EEPROM outlives a
// firmware flash, a volume wipe and a power cut, which the littlefs volume does not:
//   - the USB link setting (Settings > USB Link): a copy of /usb.cfg's flags. A boot whose volume has no /usb.cfg (a clean flash)
//     takes the EEPROM copy, so a K2 the user had made reachable stays reachable;
//   - the spoof the user armed (Tools > Spoof, the remote's spoof arm): a boot re-arms it, so a test that cuts VBUS under a
//     self-powered K2 goes on with the same identity. Only those user arms count; a charger test's run identity has its own
//     comeback record (k2/core/rd_comeback.hpp), the twin's inline run is never kept. Disarming, or any other arm or disarm
//     (generation() moves), clears the slot.
// Every write waits for the main loop's quiet point (poll: no port owner, the PD link quiet), never inline with an arm: a page
// write blocks for about 5 ms, inside a charger's cable query that would cost the reply. A slot is written only when it changes.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "k2/chargertest/ct_cable.hpp"

namespace k2::eekeep {

// ---- pure part (tests/test_eekeep.cpp) ----

inline constexpr std::size_t kSpoofLen = 3;   // kind, seq low, seq high
constexpr std::array<std::uint8_t, kSpoofLen> spoof_payload(ctcable::Choice c) {
    return {std::uint8_t(c.kind), std::uint8_t(c.seq & 0xFF), std::uint8_t(c.seq >> 8)};
}
// The slot's choice; nullopt when the payload is not one (wrong length, unknown kind, invalid choice).
constexpr std::optional<ctcable::Choice> spoof_choice(std::span<const std::uint8_t> p) {
    if (p.size() != kSpoofLen || p[0] > std::uint8_t(ctcable::Kind::Saved)) return std::nullopt;
    const ctcable::Choice c{ctcable::Kind(p[0]), std::uint16_t(p[1] | (p[2] << 8))};
    if (!ctcable::valid(c)) return std::nullopt;
    return c;
}
// What the slot should hold: the user's arm while it is still the one answering (nobody armed or disarmed since), else Off.
constexpr ctcable::Choice spoof_wanted(bool active, std::uint32_t gen, std::uint32_t user_gen, ctcable::Choice user) {
    return active && gen == user_gen ? user : ctcable::Choice{};
}

// ---- firmware part (src/core/eekeep.cpp) ----

// Boot, after i2c::init: reads both slots once (a few ms; a missing chip reads as blank).
void init();
// The stored USB link flags (usbcfg's record byte), nullopt when the slot is blank or damaged.
std::optional<std::uint8_t> usb_flags();
// The USB link setting the slot should hold (usbcfg::init with the file's, usbcfg::set with the new one); written by poll().
void usb_want(std::uint8_t flags);
// Boot, after the FUSB302 runs and a comeback armed its run identity: re-arms the stored spoof unless a spoof is armed already.
// A choice that cannot be armed (a deleted clone) is dropped and the slot cleared.
void spoof_restore();
// A user arm took (Tools > Spoof, the remote): remember it with emk_spoof::generation() as it is now.
void spoof_user_armed(ctcable::Choice c);
// The user's arm while it still answers (Off otherwise): the Spoof page adopts a restored or remote arm with it.
ctcable::Choice spoof_user();
// Main loop: writes a slot that changed, only when may_write (no port owner, the PD link quiet). A failed write retries later.
void poll(std::uint32_t now_ms, bool may_write);

}  // namespace k2::eekeep
