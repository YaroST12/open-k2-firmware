// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The records the QR share tests build, shared by tests/test_qrshare_records.cpp (the frame goldens) and
// tests/test_qrshare_link.cpp (the link goldens): a boot snapshot, a saved cable, quirk rows, the source's PDOs.
#pragma once
#include <cstdint>
#include <cstring>

#include "k2/core/bootinfo.hpp"
#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/pd.hpp"

namespace qsfx {

inline k2::bootinfo::Snapshot boot_snapshot() {
    using namespace k2;
    bootinfo::Snapshot s;
    std::strcpy(s.fw, "1.0");
    std::strcpy(s.git, "fcb3128-dirty");
    s.proto = 1;
    s.mode = bootmode::Mode::PlusRdCc2;
    s.keys = bootmode::kPlus;
    s.pa = bootinfo::kPaOk | bootinfo::kPaBack | bootinfo::kPaMinus;
    s.reset_flags = 0x24000000;
    s.pin_initial = 2; s.pin_now = 1; s.pin_boot = 1;
    s.move = {true, 650, 2, 1, 0, 3300, 150};
    s.notes[0] = {3, 0, 2};
    s.notes[1] = {4, 1, 3};
    s.notes[2] = {652, 0, 1};
    s.notes[3] = {50, 2, 2};
    s.notes[4] = {600, 2, 2};
    s.notes[5] = {3001, 2, 1};
    s.note_n = 6;
    s.t_ms = 12345; s.vbus_mv = 5012; s.cc1_mv = 3290; s.cc2_mv = 0; s.plus_now = true;
    return s;
}

// A saved clone of the magnetic OnePlus cable (the identity the retired built-in id 3 used to carry), kept as a literal so the
// golden fixtures do not depend on the built-in table.
inline k2::emrec::Record oneplus_mag_clone() {
    const std::uint32_t vdos[5] = {0xFF00A041u, 0x180022D9u, 0x00000000u, 0x11960000u, 0x60082A40u};
    return k2::emrec::make(vdos, 2, "[OnePlus mag]");
}

// A clone with `rows` response rows (at most kMaxRows; eight is the biggest record, 510 bytes).
inline k2::emk_quirk::Quirks quirk_rows(unsigned rows) {
    using namespace k2;
    emk_quirk::Quirks q;
    q.flags = emk_quirk::kQSopPP | emk_quirk::kQMsgIdEcho;
    q.unmatched = emk_quirk::Unmatched::Nak;
    for (unsigned i = 0; i < rows && i < emk_quirk::kMaxRows; i++) {
        const std::uint32_t w[3] = {0xAAAA0000u + i, 0xBBBB0000u + i, 0xCCCC0000u + i};
        emk_quirk::add_row(q, emk_quirk::svdm_row(0x22D9, std::uint8_t(16 + i), emk_quirk::Kind::Ack, w,
                                                   i & 1 ? emk_quirk::kSopPP : emk_quirk::kSopP));
    }
    return q;
}

inline k2::pd::SourceCaps caps_spr() {
    k2::pd::SourceCaps c;
    c.valid = true;
    c.num_pdos = 5;
    c.active_cc = 1;
    c.spec_rev = 2;
    c.raw_header = 0x51A1;
    const std::uint32_t w[5] = {0x0801912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0xC1DC0 | (0x3u << 30)};
    for (unsigned i = 0; i < 5; i++) c.pdos[i].raw = w[i];
    return c;
}

inline k2::pd::SourceCaps caps_epr() {
    k2::pd::SourceCaps c = caps_spr();
    c.num_pdos = 10;
    c.epr = true;
    c.epr_partial = true;
    c.raw_header = 0x5BB1;
    c.pdos[5] = {};
    c.pdos[6] = {};
    c.pdos[7].raw = 0x0006F12C;   // EPR positions start at 8; the slot values are only carried, not decoded here
    c.pdos[8].raw = 0xA0A1B0C1;
    c.pdos[9].raw = 0x0F0E0D0C;
    return c;
}

}  // namespace qsfx
