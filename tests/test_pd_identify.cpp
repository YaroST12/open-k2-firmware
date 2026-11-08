// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <vector>

#include "k2/pd/pd_identify.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

struct Rig {
    Identify id;
    std::uint32_t now = 1000;
    std::vector<TxMsg> sent;
    std::vector<std::uint16_t> svids{0x2717, 0xFF01, 0x1234};
    bool idle = true;

    void start(std::uint8_t rev = 2) { id.start(rev, now); }
    // One 10 ms tick; returns true if a message went out (and was GoodCRC'd).
    bool step(bool tx_ok = true) {
        id.tick(now, idle, svids);
        TxMsg m;
        bool out = false;
        if (id.take_tx(m)) { sent.push_back(m); id.on_tx_result(tx_ok, now); out = true; }
        now += 10;
        return out;
    }
    void run(unsigned ms) { for (unsigned t = 0; t < ms; t += 10) step(); }
    // Run until the next message goes out.
    bool next(unsigned limit_ms = 1000) {
        for (unsigned t = 0; t < limit_ms; t += 10) if (step()) return true;
        return false;
    }
    void vdm(std::uint8_t ctype, std::uint8_t cmd, std::uint16_t svid, unsigned extra = 0) {
        std::uint8_t p[4 + 24] = {};
        p[0] = std::uint8_t((ctype << 6) | cmd);
        p[1] = 0xA0;
        p[2] = std::uint8_t(svid);
        p[3] = std::uint8_t(svid >> 8);
        id.on_message(Sop::Sop, make_header(kMsgVendorDefined, 1 + extra, 2), {p, 4 + 4 * extra}, now);
    }
    void ctl(std::uint8_t type) { id.on_message(Sop::Sop, make_header(type, 0, 2), {}, now); }
    std::uint8_t last_type() const { return sent.back().header & 0x1F; }
    std::uint32_t last_word() const {
        const auto& d = sent.back().data;
        return d[0] | (d[1] << 8) | (d[2] << 16) | (std::uint32_t(d[3]) << 24);
    }
};

}  // namespace

int main() {
    CHECK_EQ(vdm_header(0xFF00, 1, 2), 0xFF00A001u);
    CHECK_EQ(vdm_header(0xFF00, 1, 1), 0xFF008001u);
    CHECK_EQ(vdm_header(0xFF00, 2, 2), 0xFF00A002u);
    CHECK_EQ(vdm_header(0x2717, 3, 2), 0x2717A003u);

    // ---- happy path, PD 3.x ----
    {
        Rig r;
        r.start();
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_word(), 0xFF00A001u);
        CHECK_EQ(r.sent.back().header & 0x7000u, 0x1000u);
        r.vdm(1, 1, 0xFF00, 3);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_word(), 0xFF00A002u);
        r.vdm(1, 2, 0xFF00, 1);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_word(), 0x2717A003u);
        r.vdm(1, 3, 0x2717, 1);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_word(), 0xFF01A003u);  // second SVID; the third is never asked
        r.vdm(1, 3, 0xFF01, 1);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(int(r.last_type()), int(kCtlGetSinkCap));
        const std::uint8_t pdo[8] = {0x2C, 0x91, 0x01, 0x36, 0xC8, 0xD0, 0x02, 0x00};
        r.id.on_message(Sop::Sop, make_header(kDataSinkCap, 2, 2), pdo, r.now);
        CHECK_EQ(int(r.id.status().num_sink_pdos), 2);
        CHECK_EQ(r.id.status().sink_pdos[0], 0x3601912Cu);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(int(r.last_type()), int(kCtlGetStatus));
        const std::uint8_t st[8] = {7, 0x80, 0, 0, 0, 0, 0, 0};
        r.id.on_message(Sop::Sop, make_header(kExtStatusMsg, 2, 2, true), st, r.now);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(int(r.last_type()), int(kCtlGetRevision));
        const std::uint8_t rv[4] = {0, 0, 0x20, 0x31};
        r.id.on_message(Sop::Sop, make_header(kDataRevision, 1, 2), rv, r.now);
        r.run(50);
        CHECK_EQ(int(r.id.status().state), int(IdState::Done));
        CHECK_EQ(int(r.id.status().ok_mask), 0x3F);
        CHECK_EQ(int(r.id.status().skipped_mask), 0);
        CHECK_EQ(r.id.status().revision, 0x31200000u);
        CHECK_EQ(r.sent.size(), 7u);
    }

    // ---- PD 2.0 partner: no Status / Revision, VDM version bit clear ----
    {
        Rig r;
        r.start(1);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_word(), 0xFF008001u);
        r.vdm(1, 1, 0xFF00, 3);
        r.next(); r.vdm(1, 2, 0xFF00, 1);
        r.svids.clear();  // none found: Modes skipped
        r.next();
        CHECK_EQ(int(r.last_type()), int(kCtlGetSinkCap));
        r.ctl(kCtlNotSupported);
        r.run(100);
        CHECK_EQ(int(r.id.status().state), int(IdState::Done));
        CHECK_EQ(r.sent.size(), 3u);
        CHECK_EQ(bool(r.id.status().skipped_mask & (1u << unsigned(IdStep::Modes))), true);
        CHECK_EQ(bool(r.id.status().skipped_mask & (1u << unsigned(IdStep::SinkCap))), true);
    }

    // ---- never transmits while the CC line is busy ----
    {
        Rig r;
        r.idle = false;
        r.start();
        r.run(500);
        CHECK_EQ(r.sent.size(), 0u);
        r.idle = true;
        CHECK_EQ(bool(r.next(50)), true);
    }

    // ---- silence: retried with backoff, then the step is skipped and the run continues ----
    {
        Rig r;
        r.start();
        CHECK_EQ(bool(r.next()), true);
        const std::uint32_t t1 = r.now;
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(bool(r.now - t1 >= kIdRespMs + kIdBackoffMs), true);
        CHECK_EQ(r.last_word(), 0xFF00A001u);
        const std::uint32_t t2 = r.now;
        CHECK_EQ(bool(r.next()), true);  // third attempt, backoff 80 ms
        CHECK_EQ(bool(r.now - t2 >= kIdRespMs + 2 * kIdBackoffMs), true);
        CHECK_EQ(r.sent.size(), 3u);
        CHECK_EQ(int(r.id.status().retries), 2);
        CHECK_EQ(bool(r.next(1000)), true);  // identity given up, Discover SVIDs goes out
        CHECK_EQ(r.last_word(), 0xFF00A002u);
        CHECK_EQ(bool(r.id.status().skipped_mask & 1u), true);
    }

    // ---- BUSY then ACK ----
    {
        Rig r;
        r.start();
        r.next();
        r.vdm(3, 1, 0xFF00);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_word(), 0xFF00A001u);
        r.vdm(1, 1, 0xFF00, 3);
        CHECK_EQ(bool(r.next()), true);
        CHECK_EQ(r.last_word(), 0xFF00A002u);
        CHECK_EQ(bool(r.id.status().ok_mask & 1u), true);
        CHECK_EQ(int(r.id.status().retries), 1);
    }

    // ---- NAK skips the step at once ----
    {
        Rig r;
        r.start();
        r.next();
        r.vdm(2, 1, 0xFF00);
        CHECK_EQ(bool(r.next(100)), true);
        CHECK_EQ(r.last_word(), 0xFF00A002u);
        CHECK_EQ(bool(r.id.status().skipped_mask & 1u), true);
    }

    // ---- transmit failure (no GoodCRC) counts as a retry ----
    {
        Rig r;
        r.start();
        r.id.tick(r.now, true, r.svids);
        TxMsg m;
        CHECK_EQ(bool(r.id.take_tx(m)), true);
        r.id.on_tx_result(false, r.now);
        CHECK_EQ(int(r.id.status().retries), 1);
        CHECK_EQ(bool(r.next(200)), true);
        CHECK_EQ(r.last_word(), 0xFF00A001u);
    }

    // ---- unrelated traffic and echoes are ignored; abort stops everything ----
    {
        Rig r;
        r.start();
        r.next();
        r.vdm(0, 1, 0xFF00, 0);  // a REQ, not an answer
        r.ctl(kCtlGoodCrc);
        CHECK_EQ(int(r.id.status().ok_mask), 0);
        r.id.abort();
        r.run(500);
        CHECK_EQ(r.sent.size(), 1u);
        CHECK_EQ(int(r.id.status().state), int(IdState::Aborted));
    }

    // ---- an abort with nothing running leaves the idle state alone (a detach must not leave "Aborted") ----
    {
        Rig r;
        r.id.abort(IdWhy::Detached);
        CHECK_EQ(int(r.id.status().state), int(IdState::Idle));
        CHECK_EQ(int(r.id.status().why), int(IdWhy::None));
        r.start();
        r.id.abort(IdWhy::HardReset);
        CHECK_EQ(int(r.id.status().state), int(IdState::Aborted));
        CHECK_EQ(int(r.id.status().why), int(IdWhy::HardReset));
        r.id.abort(IdWhy::Detached);   // already over: the first reason stays
        CHECK_EQ(int(r.id.status().why), int(IdWhy::HardReset));
    }

    {   // precheck: nothing on CC and VBUS under 3 V = no device to ask; an unknown reading never blocks
        CHECK_EQ(int(identify_no_device(0, 0, 0)), 1);            // the Pixel stopped sourcing
        CHECK_EQ(int(identify_no_device(50, 120, 2900)), 1);
        CHECK_EQ(int(identify_no_device(0, 0, 5000)), 0);         // VBUS is up
        CHECK_EQ(int(identify_no_device(0, 0, 3000)), 0);
        CHECK_EQ(int(identify_no_device(0, 900, 0)), 0);          // Rp on CC2
        CHECK_EQ(int(identify_no_device(200, 0, 0)), 0);          // Rp at the threshold
        CHECK_EQ(int(identify_no_device(-1, -1, 0)), 0);          // no ADC sweep yet
        CHECK_EQ(int(identify_no_device(0, 0, -1)), 0);           // no power monitor
    }
    {   // step names for the "No reply" line
        CHECK_STR(id_step_short(IdStep::Identity), "Identity");
        CHECK_STR(id_step_short(IdStep::Svids), "SVIDs");
        CHECK_STR(id_step_short(IdStep::Modes), "Modes");
        CHECK_STR(id_step_short(IdStep::SinkCap), "Sink caps");
        CHECK_STR(id_step_short(IdStep::Status), "Status");
        CHECK_STR(id_step_short(IdStep::Revision), "Revision");
    }

    return test_report("test_pd_identify");
}
