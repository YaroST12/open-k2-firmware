// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::cablefact: the one fact a reader of "cable not read" may state about a cable that gave no identity. The cable tab,
// the charger test's START line and the remote all ask here instead of each reading the driver's pieces.
//
//   Read        the cable answered (an e-marker is held)
//   ChipSilent  no answer, but a chip is there: its pull-down sits on a CC line (alone scan, Ra probe) or it acknowledged
//               a query (GoodCRC) without answering. Not "no e-marker": it answers once a source powers it
//               (docs/CHARGER_QUIRKS.md, cable-chip-silent-alone).
//   NoChip      probed alone, no pull-down on either line: a chipless cable (or nothing connected)
//   NotRead     nothing can be said: a source was attached (no probe), the probe was refused or unclear, no read yet
//
// Pure constexpr code over what fusb302::physical_cable(), last_cable_read() and pull_down() return; host-tested in
// tests/test_cable_fact.cpp. It changes no behaviour: a silent chip still counts as an unread cable for the limits.
#pragma once
#include <cstdint>

#include "k2/pd/emk_attached.hpp"
#include "k2/pd/ra_class.hpp"

namespace k2::cablefact {

enum class Fact : std::uint8_t { Read = 0, ChipSilent = 1, NoChip = 2, NotRead = 3 };

constexpr Fact classify(bool read, emk_attached::ReadInfo info, raclass::PullDown pd) {
    if (read) return Fact::Read;
    if (info.acked_no_answer(false)) return Fact::ChipSilent;   // a powered chip that acknowledged: present for sure
    switch (pd) {
    case raclass::PullDown::Cc1:
    case raclass::PullDown::Cc2: return Fact::ChipSilent;
    case raclass::PullDown::None: return Fact::NoChip;
    default: return Fact::NotRead;
    }
}

constexpr const char *name(Fact f) {
    switch (f) {
    case Fact::Read: return "read";
    case Fact::ChipSilent: return "chip silent";
    case Fact::NoChip: return "no chip";
    default: return "not read";
    }
}

}  // namespace k2::cablefact
