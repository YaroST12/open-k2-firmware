// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The e-marker scan with a source attached. A cable chip that lives on the charger's supply (the Baseus 100 W
// C-to-C cable: silent to a K2 that reads it alone, answers the charger) starves when the scan drops the K2's Rd:
// the charger sees a detach and takes VBUS and VCONN away mid-scan. So with a source attached the scan keeps the Rd on
// the live CC line, tries only the orientation whose BMC line is that live line, and never puts VCONN on it. With the
// cable alone (no source) the scan is the two-orientation one without Rd, unchanged.
// Pure constexpr code, host-tested in tests/test_emk_attached.cpp; the register work is in src/drivers/fusb302.cpp.
#pragma once
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/pd/pd_phy.hpp"

namespace k2::emk_attached {

// A source is attached when the K2 presents Rd on the line it talks on and either the chip's VBUS comparator sees
// VBUS (vSafe5V and up) or a contract is up. `active_cc` is the line the driver locked (0 = none), `sw0` the
// SWITCHES0 value before the scan.
constexpr bool attached(std::uint8_t active_cc, std::uint8_t sw0, bool vbus_ok, bool contract) {
    if (active_cc != 1 && active_cc != 2) return false;
    const std::uint8_t pdwn = active_cc == 1 ? pd::phy::kPdwn1 : pd::phy::kPdwn2;
    return (sw0 & pdwn) != 0 && (vbus_ok || contract);
}

// One orientation of the scan: A (BMC on CC1, VCONN on CC2) or B (BMC on CC2, VCONN on CC1).
struct Step {
    std::uint8_t orientation = 0;   // 1 = A, 2 = B (the emk_variant::Answered numbering)
    std::uint8_t sw0 = 0;
    std::uint8_t sw1 = 0;
};

struct Plan {
    bool attached = false;
    std::uint8_t count = 0;         // steps to run
    std::array<Step, 2> steps{};
    bool pc13 = true;               // drive PC13 high during the scan
};

// PC13 high is an Ra (~1 kohm to ground) on the CC1 net (src/pd/emk_ra.hpp): with a source on CC1 it would sit beside
// the K2's Rd on the live wire. Attached on CC1 the scan leaves PC13 low; on CC2 the Ra lands on the idle CC1.
constexpr Plan plan(bool is_attached, std::uint8_t active_cc) {
    namespace phy = pd::phy;
    constexpr std::uint8_t kSw1Cc1 = phy::sw1(1, true), kSw1Cc2 = phy::sw1(2, true);   // BMC on the line, Auto-CRC, SPECREV1
    Plan p;
    p.attached = is_attached;
    if (!is_attached) {
        p.count = 2;
        p.steps[0] = {1, std::uint8_t(phy::kVconnCc2 | phy::kMeasCc1), kSw1Cc1};
        p.steps[1] = {2, std::uint8_t(phy::kVconnCc1 | phy::kMeasCc2), kSw1Cc2};
        return p;
    }
    p.count = 1;
    if (active_cc == 1) {
        p.steps[0] = {1, std::uint8_t(phy::kPdwn1 | phy::kMeasCc1 | phy::kVconnCc2), kSw1Cc1};
        p.pc13 = false;
    } else {
        p.steps[0] = {2, std::uint8_t(phy::kPdwn2 | phy::kMeasCc2 | phy::kVconnCc1), kSw1Cc2};
    }
    return p;
}

// What the last read did, for the readers that must tell "the cable was silent alone" from "silent with the source
// attached" (the held 5 A finding's "cable not read" facts) and for the learn summary.
enum class Mode : std::uint8_t { None = 0, Alone = 1, Attached = 2 };

struct ReadInfo {
    Mode mode = Mode::None;         // None = no read since the attach
    std::uint8_t acks = 0;          // query variants the cable acknowledged (GoodCRC) in the last read, saturating
    // The chip acknowledged but never answered: only meaningful when the read found no cable.
    constexpr bool acked_no_answer(bool answered) const { return !answered && acks != 0; }
    constexpr bool operator==(const ReadInfo&) const = default;
};

constexpr std::string_view mode_text(Mode m) {
    switch (m) {
        case Mode::Alone: return "alone";
        case Mode::Attached: return "attached";
        default: return "none";
    }
}

}  // namespace k2::emk_attached
