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

// Quirk rows that also constrain the request data objects (nreq 1 and 2, with a header mask that is not the full
// kSvdmMask, so the v2 record carries it): the case the plain quirk_rows() above never covers.
inline k2::emk_quirk::Quirks quirk_rows_req() {
    using namespace k2;
    emk_quirk::Quirks q;
    q.flags = emk_quirk::kQMsgIdEcho;
    q.unmatched = emk_quirk::Unmatched::NotSupported;
    emk_quirk::Row r0;
    r0.sop = emk_quirk::kSopP | emk_quirk::kSopPP;
    r0.kind = emk_quirk::Kind::Ack;
    r0.nreq = 1;
    r0.nrep = 2;
    r0.hdr_val = emk_quirk::svdm_req(0x22D9, 16);
    r0.hdr_mask = 0xFFFF8000u | 0x1Fu;   // not kSvdmMask: the v2 form stores it
    r0.req_val[0] = 0x11223344u;
    r0.req_mask[0] = 0xFF00FF00u;
    r0.rep[0] = 0xDEADBEEFu;
    r0.rep[1] = 0xCAFEBABEu;
    emk_quirk::add_row(q, r0);
    emk_quirk::Row r1;
    r1.sop = emk_quirk::kSopP;
    r1.kind = emk_quirk::Kind::Raw;
    r1.nreq = 2;
    r1.nrep = 3;
    r1.hdr_val = emk_quirk::svdm_req(0xFF00, 31);
    r1.hdr_mask = emk_quirk::kSvdmMask;   // the full mask: the v2 form stores only the flag
    r1.req_val[0] = 0x01020304u;
    r1.req_val[1] = 0x05060708u;
    r1.req_mask[0] = 0xFFFFFFFFu;
    r1.req_mask[1] = 0x0F0F0F0Fu;
    r1.rep[0] = 0xAA55AA55u;
    r1.rep[1] = 0x55AA55AAu;
    r1.rep[2] = 0x12345678u;
    emk_quirk::add_row(q, r1);
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
