// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the charger-card source probe (k2/pd/pd_probe.hpp): the message order, answers decoded by the real
// Engine, and the paths where the charger NAKs or stays silent (never retried, under one second in total).
#include <cstring>
#include <vector>

#include "k2/pd/pd_probe.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

struct Rig {
    Probe p;
    Engine eng;
    std::uint32_t now = 5000;
    std::vector<TxMsg> sent;
    void start(std::uint8_t rev = 2) { p.start(rev, now); }
    // One 10 ms tick; true when a message went out (and was GoodCRC'd).
    bool step(bool tx_ok = true) {
        p.tick(now, true);
        TxMsg m;
        bool out = false;
        if (p.take_tx(m)) { sent.push_back(m); p.on_tx_result(tx_ok, now); out = true; }
        now += 10;
        return out;
    }
    bool next(unsigned limit_ms = 1000) {
        for (unsigned t = 0; t < limit_ms; t += 10) if (step()) return true;
        return false;
    }
    void deliver(Sop sop, std::uint16_t header, const std::uint8_t* d, std::size_t n) {
        eng.on_message(sop, header, {d, n}, 1);
        p.on_message(sop, header, {d, n}, now);
    }
    // Discover Identity response: ctype 1 = ACK (ID header with VID, cert, product VDO with PID), 2 = NAK.
    void identity(std::uint8_t ctype, std::uint16_t vid, std::uint16_t pid) {
        std::uint8_t d[16] = {std::uint8_t(0x01 | (ctype << 6)), 0xA0, 0x00, 0xFF};
        const unsigned n = ctype == 1 ? 4 : 1;
        if (ctype == 1) {
            d[4] = std::uint8_t(vid); d[5] = std::uint8_t(vid >> 8); d[7] = 0x40;   // ID header: UFP, VID
            d[12] = 0; d[13] = 0; d[14] = std::uint8_t(pid); d[15] = std::uint8_t(pid >> 8);   // product VDO
        }
        deliver(Sop::Sop, make_header(kMsgVendorDefined, ctype == 1 ? 4 : 1, 2), d, 4 * n);
    }
    void ext(std::uint8_t type, const std::uint8_t* data, std::size_t size) {
        std::uint8_t d[32] = {};
        d[0] = std::uint8_t(size); d[1] = 0x80;   // chunked, chunk 0
        std::memcpy(d + 2, data, size);
        const std::size_t len = (2 + size + 3) & ~std::size_t(3);
        deliver(Sop::Sop, std::uint16_t(0x8000 | ((len / 4) << 12) | (2u << 6) | type), d, len);
    }
    void control(std::uint8_t type) { deliver(Sop::Sop, make_header(type, 0, 2), nullptr, 0); }
};

// MIDB: VID 0x291A, PID 0x0102, "Anker Innovations" (17 chars, as a first chunk carries up to 22).
const std::uint8_t kMidb[4 + 17] = {0x1A, 0x29, 0x02, 0x01, 'A', 'n', 'k', 'e', 'r', ' ', 'I', 'n', 'n', 'o', 'v', 'a', 't', 'i', 'o', 'n', 's'};
// SCEDB: VID 0x2717, PID 0x4321, PDP 67 W.
const std::uint8_t kScedb[24] = {0x17, 0x27, 0x21, 0x43, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 67};

void test_messages_in_order() {
    Rig r; r.start();
    CHECK_EQ(r.next(), true);
    CHECK_EQ(r.sent[0].header & 0x1F, kMsgVendorDefined);
    CHECK_EQ(r.sent[0].len, 4);
    CHECK_EQ(r.sent[0].data[0], 0x01);          // structured Discover Identity request
    CHECK_EQ(r.sent[0].data[1], 0xA0);          // structured, VDM 2.0
    CHECK_EQ(r.sent[0].data[2], 0x00);          // PD SID 0xFF00
    CHECK_EQ(r.sent[0].data[3], 0xFF);
    r.identity(1, 0x291A, 0x0102);
    CHECK_EQ(r.next(), true);
    CHECK_EQ(r.sent[1].header & 0x1F, kExtGetManufacturerInfo);
    CHECK_EQ((r.sent[1].header >> 15) & 1, 1);  // extended
    CHECK_EQ((r.sent[1].header >> 12) & 7, 1);  // one data object
    CHECK_EQ(r.sent[1].data[0], 2);             // ext header: unchunked, 2 data bytes
    CHECK_EQ(r.sent[1].data[1], 0);
    CHECK_EQ(r.sent[1].data[2], 0);             // target: the port
    CHECK_EQ(r.sent[1].data[3], 0);
    r.ext(kExtManufacturerInfo, kMidb, sizeof kMidb);
    CHECK_EQ(r.next(), true);
    CHECK_EQ(r.sent[2].header & 0x1F, kCtlGetSourceCapExt);
    CHECK_EQ(r.sent[2].len, 0);
    r.ext(kExtSourceCapsExt, kScedb, sizeof kScedb);
    CHECK_EQ(r.p.status().state == ProbeState::Done, true);
    CHECK_EQ(r.p.status().ok_mask, 7);
    CHECK_EQ(r.p.status().skipped_mask, 0);
}

void test_decode_answers() {
    Rig r; r.start();
    r.next(); r.identity(1, 0x291A, 0x0102);
    r.next(); r.ext(kExtManufacturerInfo, kMidb, sizeof kMidb);
    r.next(); r.ext(kExtSourceCapsExt, kScedb, sizeof kScedb);
    SourceCaps caps;
    const auto res = probe_result(r.p.status(), r.eng.vdm, r.eng.ext, caps);
    CHECK_EQ(res.vid, 0x291A);                   // Discover Identity wins over Mfr info and Cap_Ext
    CHECK_EQ(res.pid, 0x0102);
    CHECK_EQ(res.pdp_w, 67);                     // Source_Cap_Extended PDP
    CHECK_STR(res.mfr, "Anker Innova");          // first 12 characters
}

void test_vid_falls_back_to_mfr_then_caps_ext() {
    Rig r; r.start();
    r.next(); r.identity(2, 0, 0);               // NAK
    r.next(); r.ext(kExtManufacturerInfo, kMidb, sizeof kMidb);
    r.next(); r.ext(kExtSourceCapsExt, kScedb, sizeof kScedb);
    SourceCaps caps;
    auto res = probe_result(r.p.status(), r.eng.vdm, r.eng.ext, caps);
    CHECK_EQ(res.vid, 0x291A); CHECK_EQ(res.pid, 0x0102);
    Rig q; q.start();
    q.next(); q.identity(2, 0, 0);
    q.next(); q.control(kCtlNotSupported);
    q.next(); q.ext(kExtSourceCapsExt, kScedb, sizeof kScedb);
    res = probe_result(q.p.status(), q.eng.vdm, q.eng.ext, caps);
    CHECK_EQ(res.vid, 0x2717); CHECK_EQ(res.pid, 0x4321);
    CHECK_STR(res.mfr, "");
}

void test_placeholder_vids_are_unknown() {
    Rig r; r.start();
    r.next(); r.identity(1, 0xFFFF, 0xFFFF);
    r.next(); r.ext(kExtManufacturerInfo, kMidb, sizeof kMidb);
    r.next(); r.ext(kExtSourceCapsExt, kScedb, sizeof kScedb);
    SourceCaps caps;
    const auto res = probe_result(r.p.status(), r.eng.vdm, r.eng.ext, caps);
    CHECK_EQ(res.vid, 0x291A);                   // FFFF skipped, next source used
    Rig q; q.start();
    q.next(); q.identity(1, 0xFFFF, 0xFFFF);
    q.next(); q.control(kCtlNotSupported);
    q.next(); q.control(kCtlNotSupported);
    const auto none = probe_result(q.p.status(), q.eng.vdm, q.eng.ext, caps);
    CHECK_EQ(none.vid, 0); CHECK_EQ(none.pid, 0);
}

void test_pdp_falls_back_to_largest_pdo() {
    Rig r; r.start();
    SourceCaps caps;
    caps.valid = true; caps.num_pdos = 3;
    caps.pdos[0] = decode_pdo(((100u) << 10) | 300);          // 5 V 3 A
    caps.pdos[1] = decode_pdo(((400u) << 10) | 325);          // 20 V 3.25 A = 65 W
    caps.pdos[2] = decode_pdo(0xc0000000 | (110u << 17) | (50u << 8) | 64);   // PPS 11 V 3.2 A = 35 W
    ProbeStatus none;
    CHECK_EQ(probe_result(none, r.eng.vdm, r.eng.ext, caps).pdp_w, 65);
    caps.valid = false;
    CHECK_EQ(probe_result(none, r.eng.vdm, r.eng.ext, caps).pdp_w, 0);
}

void test_nak_skips_without_retry() {
    Rig r; r.start();
    r.next(); r.identity(2, 0, 0);               // NAK: skipped, the next message goes out at once
    CHECK_EQ(r.p.status().skipped_mask, 1);
    CHECK_EQ(r.next(20), true);
    CHECK_EQ(r.sent.size(), 2u);
    r.control(kCtlNotSupported);                 // Get_Manufacturer_Info refused
    CHECK_EQ(r.next(20), true);
    CHECK_EQ(r.sent[2].header & 0x1F, kCtlGetSourceCapExt);
    r.control(kCtlReject);
    CHECK_EQ(r.p.status().state == ProbeState::Done, true);
    CHECK_EQ(r.p.status().ok_mask, 0);
    CHECK_EQ(r.sent.size(), 3u);                 // exactly one try per step
}

void test_silent_source_finishes_under_a_second() {
    Rig r; r.start();
    const std::uint32_t t0 = r.now;
    while (r.p.running() && r.now - t0 < 5000) r.step();
    CHECK_EQ(r.p.status().state == ProbeState::Done, true);
    CHECK_EQ(r.now - t0 <= 1000, true);
    CHECK_EQ(r.sent.size(), 3u);                 // asked each once, never retried
    CHECK_EQ(r.p.status().ok_mask, 0);
    CHECK_EQ(r.p.status().skipped_mask, 7);
}

void test_tx_failure_skips_the_step() {
    Rig r; r.start();
    r.step(false);                               // Discover Identity: no GoodCRC
    CHECK_EQ(r.p.status().skipped_mask, 1);
    CHECK_EQ(r.p.status().step == ProbeStep::MfrInfo, true);
}

void test_pd2_source_only_gets_identity() {
    Rig r; r.start(1);
    CHECK_EQ(r.next(), true);
    CHECK_EQ(r.sent[0].header & 0x1F, kMsgVendorDefined);
    CHECK_EQ(r.sent[0].data[1], 0x80);           // VDM 1.0 header for PD 2.0
    r.identity(1, 0x05AC, 0x7108);
    CHECK_EQ(r.p.status().state == ProbeState::Done, true);
    CHECK_EQ(r.p.status().skipped_mask, probe_bit(ProbeStep::MfrInfo) | probe_bit(ProbeStep::CapsExt));
    CHECK_EQ(r.next(100), false);
}

void test_abort_and_foreign_traffic() {
    Rig r; r.start();
    r.next();
    r.deliver(Sop::SopP, make_header(kMsgVendorDefined, 1, 2), (const std::uint8_t*)"\x41\xA0\x00\xFF", 4);   // cable, not us
    CHECK_EQ(r.p.status().ok_mask, 0);
    r.p.abort();
    CHECK_EQ(r.p.running(), false);
    CHECK_EQ(r.next(100), false);
}

}  // namespace

int main() {
    test_messages_in_order();
    test_decode_answers();
    test_vid_falls_back_to_mfr_then_caps_ext(); test_placeholder_vids_are_unknown();
    test_pdp_falls_back_to_largest_pdo();
    test_nak_skips_without_retry();
    test_silent_source_finishes_under_a_second();
    test_tx_failure_skips_the_step();
    test_pd2_source_only_gets_identity();
    test_abort_and_foreign_traffic();
    return test_report("test_pd_probe");
}
