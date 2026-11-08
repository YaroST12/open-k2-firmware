// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Boot mode chosen by the buttons held at reset: PLUS = mode 3 (FUSB302 SWITCHES0 0x02, Rd on CC2),
// OK = mode 2 (0x01, Rd on CC1), otherwise mode 1. MINUS is the bootloader's DFU key (it is acted
// on before the app runs); BACK selects no mode.
#pragma once
#include <cstdint>

namespace k2::bootmode {

inline constexpr std::uint8_t kBack = 1, kMinus = 2, kOk = 4, kPlus = 8;   // button mask bits

enum class Mode : std::uint8_t { Plain, OkRdCc1, PlusRdCc2 };

constexpr Mode decide(std::uint8_t held) {
    if (held & kPlus) return Mode::PlusRdCc2;
    if (held & kOk) return Mode::OkRdCc1;
    return Mode::Plain;
}

// FUSB302 SWITCHES0 value of the mode: bit0 PDWN1 (Rd on CC1), bit1 PDWN2 (Rd on CC2).
constexpr std::uint8_t sw0(Mode m) {
    return m == Mode::PlusRdCc2 ? 0x02 : m == Mode::OkRdCc1 ? 0x01 : 0x00;
}

// CC analog switch routing: bit0 = PC14 (CC1), bit1 = PC15 (CC2); PC13 is low in every mode. The K2 has no hardware Rd to fall back on
// once the key is released: the Rd it pins has to reach the connector, so every mode closes both switches.
constexpr std::uint8_t cc_mask(Mode) { return 3; }

// The pin mark: a 16-bit word in a backup register (src/drivers/pin_mark.cpp) that says "this firmware held an Rd pin when it reset".
// Magic in the high byte, the pin bits (SWITCHES0 bit0 / bit1) in the low byte; 0 = no pin. It is written wherever the boot pin
// changes and survives SYSRESETREQ, IWDG and NRST (not a power loss: no battery). The FUSB302's own SWITCHES0 is NOT a source
// for this decision: after a warm reset it holds the chip's power-on 0x03 (a flash resets the app, not the chip), a
// pd-reread's or a run's Rd, none of which a K2 powered over HID (its VBUS never hangs on its Rd) may keep.
// One line only: Rd on both is the Type-C Debug Accessory signature, which latches a source's Rp off until a physical detach
// (OnePlus 120W brick, hardware 2026-11-06), so a K2 never holds that pin and a mark that says both (an older build's widened
// pin) is no mark.
inline constexpr std::uint16_t kPinMarkMagic = 0xB500;
constexpr std::uint16_t pin_mark(std::uint8_t pin) {
    return (pin & 0x03) == 1 || (pin & 0x03) == 2 ? std::uint16_t(kPinMarkMagic | (pin & 0x03)) : std::uint16_t(0);
}
constexpr std::uint8_t pin_from_mark(std::uint16_t raw) {
    return (raw & 0xFFFC) == kPinMarkMagic && ((raw & 0x03) == 1 || (raw & 0x03) == 2) ? std::uint8_t(raw & 0x03) : std::uint8_t(0);
}

// Rd bits (SWITCHES0 bit0 / bit1) the driver owns from the first write after reset. A key mode pins its own line. A plain
// boot after a warm reset (`warm`: the reset was not a power-on) keeps the pin the mark says this firmware held, so a
// charger-powered K2 that resets (watchdog, crash, resume) never drops the Rd its VBUS hangs on. Everything else is passive:
// a cold start, and a warm reset without a mark (HID-powered, a reflash, `reboot`). `mark` = the backup register's raw word.
constexpr std::uint8_t initial_rd(Mode m, bool warm, std::uint16_t mark) {
    if (m != Mode::Plain) return sw0(m);
    return warm ? pin_from_mark(mark) : std::uint8_t(0);
}

// A key mode asks for VBUS with Rd on ONE line (PLUS = CC2, OK = CC1) and never on both. The charger can
// sit on either orientation of the plug, so when no VBUS shows after kMoveAfterMs, the pinned line sees no Rp and the OTHER line
// does, the single Rd moves to the other line (the caller moves once per boot). A direct plug (Rp on the pinned line, over our
// Rd) stays; no Rp anywhere (a source that already latched off) stays too, there is nothing to move towards.
//   kMoveAfterMs: a source needs tCCDebounce (up to 200 ms) to see our Rd and tVBUSON (up to 275 ms) to raise VBUS: 475 ms
//   at worst, plus a sampling margin (the ADC is read every 50 ms). A source that is going to answer has answered by then.
//   kRpSeenMv: the lowest level a source's Rp makes on a line (over a sink's Rd), an open Rp reads ~3.3 V, a line nothing
//   drives ~0 V.
//   vbus_mv < 0 = power monitor not answering (counts as no VBUS).
inline constexpr std::uint32_t kMoveAfterMs = 600;
inline constexpr std::int32_t kVbusPresentMv = 4000;
inline constexpr std::int32_t kRpSeenMv = 200;
constexpr std::uint8_t moved_pin(std::uint8_t pin, std::uint32_t ms_since_start, std::int32_t vbus_mv, std::int32_t cc1_mv,
                                 std::int32_t cc2_mv) {
    if (pin != 1 && pin != 2) return pin;
    if (ms_since_start < kMoveAfterMs || vbus_mv >= kVbusPresentMv) return pin;
    const std::int32_t on_pin = pin == 1 ? cc1_mv : cc2_mv;
    const std::int32_t on_other = pin == 1 ? cc2_mv : cc1_mv;
    if (on_pin >= kRpSeenMv || on_other < kRpSeenMv) return pin;
    return pin == 1 ? std::uint8_t(2) : std::uint8_t(1);
}

constexpr const char* name(Mode m) {
    return m == Mode::PlusRdCc2 ? "plus-rd-cc2" : m == Mode::OkRdCc1 ? "ok-rd-cc1" : "plain";
}

constexpr bool self_powered(Mode m) { return m != Mode::Plain; }

}  // namespace k2::bootmode
