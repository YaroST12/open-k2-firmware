// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD limiter (k2/pd/pd_limiter.hpp): the forged Source_Capabilities, the request power, and the trigger state machine over
// a replayed wire (charger = source role bits, sink = sink role bits, every frame 1 ms apart unless a test moves the clock).
#include <vector>

#include "k2/pd/pd_limiter.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

constexpr std::uint16_t kSrc = 0x0120, kSnk = 0x0000;
constexpr std::uint16_t hdr(std::uint8_t type, std::uint8_t ndo, std::uint8_t rev, std::uint16_t roles, std::uint8_t id,
                            bool ext = false) {
    return std::uint16_t(type | (rev << 6) | roles | ((id & 7) << 9) | (ndo << 12) | (ext ? 0x8000u : 0u));
}
constexpr std::uint32_t fixed(unsigned mv, unsigned ma, std::uint32_t flags = 0) { return flags | (mv / 50) << 10 | ma / 10; }
constexpr std::uint32_t pps(unsigned max_mv, unsigned min_mv, unsigned ma) {
    return 0xC0000000u | (max_mv / 100) << 17 | (min_mv / 100) << 8 | ma / 50;
}
constexpr std::uint32_t kFlags1 = 0x0A000000u;   // unconstrained power, USB communications capable
constexpr std::uint32_t kEprBit = 1u << 23;
// A 100 W charger: 5 / 9 / 12 / 15 V at 3 A, 20 V 5 A, a 3.3-11 V 5 A PPS.
const std::uint32_t kCaps[6] = {fixed(5000, 3000, kFlags1 | kEprBit), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000),
                                fixed(20000, 5000), pps(11000, 3300, 5000)};
constexpr std::uint32_t rq_fixed(unsigned pos, unsigned op_ma, unsigned max_ma) { return pos << 28 | op_ma / 10 << 10 | max_ma / 10; }
constexpr std::uint32_t rq_pps(unsigned pos, unsigned mv, unsigned ma) { return pos << 28 | mv / 20 << 9 | ma / 50; }

void put32(std::vector<std::uint8_t>& b, std::uint32_t w) {
    for (int i = 0; i < 4; i++) b.push_back(std::uint8_t(w >> (8 * i)));
}
std::uint32_t word(const TxMsg& m, unsigned i) {
    return m.data[4 * i] | (m.data[4 * i + 1] << 8) | (m.data[4 * i + 2] << 16) | (std::uint32_t(m.data[4 * i + 3]) << 24);
}

struct Rig {
    Limiter lim;
    std::uint32_t now = 10000;
    std::uint8_t last_id = 0;   // the charger's newest MessageID

    void rx(std::uint16_t header, std::vector<std::uint8_t> payload = {}) {
        lim.on_message(Sop::Sop, header, payload, now);
        now += 1;
    }
    void caps(std::uint8_t id, std::uint8_t rev = 2) {
        std::vector<std::uint8_t> p;
        for (std::uint32_t w : kCaps) put32(p, w);
        rx(hdr(1, 6, rev, kSrc, id), p);
        rx(hdr(1, 0, 1, kSnk, id));
        last_id = id;
    }
    void request(std::uint32_t rdo) {
        std::vector<std::uint8_t> p;
        put32(p, rdo);
        rx(hdr(2, 1, 2, kSnk, 0), p);
        rx(hdr(1, 0, 1, kSrc, 0));
    }
    void accept_ready(std::uint8_t id) {   // Accept id, PS_RDY id + 1
        rx(hdr(3, 0, 2, kSrc, id));
        rx(hdr(1, 0, 1, kSnk, id));
        rx(hdr(6, 0, 2, kSrc, std::uint8_t(id + 1)));
        rx(hdr(1, 0, 1, kSnk, std::uint8_t(id + 1)));
        last_id = std::uint8_t((id + 1) & 7);
    }
    // caps -> Request -> Accept -> PS_RDY, ids from `id0`
    void negotiate(std::uint8_t id0, std::uint32_t rdo) {
        caps(id0);
        request(rdo);
        accept_ready(std::uint8_t((id0 + 1) & 7));
    }
    // A quiet line, then ask for the frame.
    bool tx(TxMsg& m, std::uint32_t quiet_ms = 20) {
        now += quiet_ms;
        lim.tick(now);
        return lim.take_tx(m, now);
    }
};

}  // namespace

int main() {
    // ---- request_mw: what a Request asks, by PDO kind ----
    {
        CHECK_EQ(request_mw(rq_fixed(5, 1500, 1500), kCaps), 30000);   // 20 V x 1.5 A
        CHECK_EQ(request_mw(rq_fixed(1, 3000, 3000), kCaps), 15000);
        CHECK_EQ(request_mw(rq_pps(6, 9000, 3000), kCaps), 27000);     // PPS: the requested voltage x the current
        CHECK_EQ(request_mw(rq_fixed(7, 1000, 1000), kCaps), 0);       // beyond the list: unknown, never "over"
        CHECK_EQ(request_mw(rq_fixed(0, 1000, 1000), kCaps), 0);
    }

    // ---- forge_caps: the same count and positions, every PDO clamped to P ----
    {
        std::uint32_t out[7]{};
        CHECK_EQ(int(forge_caps(kCaps, 30000, out)), 6);
        CHECK_EQ(out[0], fixed(5000, 3000, kFlags1));   // 15 W: unchanged; only the EPR-capable bit goes
        CHECK_EQ(out[1], fixed(9000, 3000));            // 27 W: unchanged
        CHECK_EQ(out[2], fixed(12000, 2500));           // 30 W / 12 V
        CHECK_EQ(out[3], fixed(15000, 2000));
        CHECK_EQ(out[4], fixed(20000, 1500));
        CHECK_EQ(out[5], pps(11000, 3300, 2700));       // 30 W / 11 V = 2.72 A, rounded down to 50 mA
        // a limit above everything changes nothing but the EPR bit
        CHECK_EQ(int(forge_caps(kCaps, 100000, out)), 6);
        CHECK_EQ(out[4], kCaps[4]);
        CHECK_EQ(out[5], kCaps[5]);
        // the other kinds: battery (250 mW units), variable (10 mA), SPR AVS (two current fields)
        const std::uint32_t real[3] = {
            0x40000000u | (20000 / 50) << 20 | (5000 / 50) << 10 | (60000 / 250),   // battery 5-20 V 60 W
            0x80000000u | (20000 / 50) << 20 | (5000 / 50) << 10 | 300,             // variable 5-20 V 3 A
            0xC0000000u | (2u << 28) | (300u << 10) | 500u};                         // SPR AVS 3 A (9-15 V), 5 A (15-20 V)
        std::uint32_t f[3]{};
        CHECK_EQ(int(forge_caps(real, 30000, f)), 3);
        CHECK_EQ(f[0] & 0x3FF, 30000 / 250);
        CHECK_EQ(f[0] >> 10, real[0] >> 10);
        CHECK_EQ(f[1] & 0x3FF, 150);                    // 30 W / 20 V
        CHECK_EQ((f[2] >> 10) & 0x3FF, 200);            // 30 W / 15 V = 2 A
        CHECK_EQ(f[2] & 0x3FF, 150);                    // 30 W / 20 V
        CHECK_EQ(f[2] >> 20, real[2] >> 20);
        // an unused slot stays zero (the count is the list as given)
        const std::uint32_t with_hole[2] = {kCaps[0], 0};
        std::uint32_t h2[2]{};
        CHECK_EQ(int(forge_caps(with_hole, 30000, h2)), 2);
        CHECK_EQ(h2[1], 0);
    }

    // ---- arming: the limit is a plain SPR figure ----
    {
        Limiter l;
        CHECK_EQ(int(l.status().state), int(LimState::Off));
        CHECK_COND(!l.arm(4999, 0));
        CHECK_COND(!l.arm(100001, 0));
        CHECK_COND(!l.armed());
        CHECK_COND(l.arm(30000, 0));
        CHECK_COND(l.armed());
        CHECK_EQ(int(l.status().state), int(LimState::Armed));
        CHECK_EQ(l.status().limit_mw, 30000);
        l.disarm();
        CHECK_COND(!l.armed());
        CHECK_EQ(int(l.status().why), int(LimWhy::User));
    }

    // ---- a first contract that asks for too much: wait for PS_RDY and a quiet line, then forge ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.caps(3);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Negotiating));   // a real list while armed: a negotiation is in flight
        r.request(rq_fixed(5, 5000, 5000));                              // 100 W
        TxMsg m;
        CHECK_COND(!r.tx(m));                                              // no PS_RDY yet: never on top of the negotiation
        r.accept_ready(4);                                                 // Accept id 4, PS_RDY id 5
        CHECK_EQ(int(r.lim.status().state), int(LimState::Pending));
        r.now += 5;
        r.lim.tick(r.now);
        CHECK_COND(!r.lim.take_tx(m, r.now));                              // the line was quiet only 5 ms
        CHECK_COND(r.tx(m));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Injected));
        CHECK_COND(m.inline_tx);
        CHECK_COND(!m.hard_reset);
        CHECK_EQ(int(m.sop), int(Sop::Sop));
        CHECK_EQ(m.len, 24);
        CHECK_EQ(m.header & 0x1F, 1);                                      // Source_Capabilities
        CHECK_EQ((m.header >> 12) & 7, 6);
        CHECK_EQ(m.header & 0x0120, 0x0120);                               // the source's power and data role bits
        CHECK_EQ((m.header >> 6) & 3, 2);                                  // the charger's revision
        CHECK_EQ((m.header >> 9) & 7, (5 + 2) & 7);                        // two past the charger's newest: its Accept (6) is never ours
        CHECK_EQ(word(m, 0), fixed(5000, 3000, kFlags1));
        CHECK_EQ(word(m, 4), fixed(20000, 1500));
        CHECK_EQ(word(m, 5), pps(11000, 3300, 2700));
        CHECK_EQ(r.lim.status().injections, 1);
        CHECK_EQ(r.lim.status().forged_n, 6);
        CHECK_EQ(r.lim.status().forged[4], fixed(20000, 1500));
        // the sink now asks within the limit: verified
        r.request(rq_fixed(5, 1500, 1500));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Limited));
        CHECK_EQ(r.lim.status().req_pos, 5);
        CHECK_EQ(r.lim.status().req_mw, 30000);
        CHECK_COND(!r.tx(m));   // nothing more to send
    }

    // ---- a real list while armed is forged even when the first Request is already within the limit ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(2, 1000, 1000));   // 9 V 1 A
        TxMsg m;
        CHECK_COND(r.tx(m));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Injected));
        r.request(rq_fixed(2, 1000, 1000));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Limited));
        // the charger lists again (a replug, a Soft_Reset path): forged again
        r.now += 2000;
        r.negotiate(0, rq_fixed(2, 1000, 1000));
        CHECK_COND(r.tx(m));
        CHECK_EQ(r.lim.status().injections, 2);
    }

    // ---- armed mid-contract: no real list was seen, nothing to forge from ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.request(rq_fixed(5, 5000, 5000));   // the PPS-style keep-alive of a contract the limiter never saw the list of
        r.accept_ready(1);
        TxMsg m;
        CHECK_COND(!r.tx(m));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Armed));
    }

    // ---- limited, then the sink asks for more again: forged again ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.request(rq_fixed(5, 1500, 1500));
        r.accept_ready(1);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Limited));
        r.now += 1500;
        r.lim.tick(r.now);
        r.request(rq_pps(6, 11000, 5000));   // 55 W
        CHECK_EQ(int(r.lim.status().state), int(LimState::Negotiating));
        r.accept_ready(4);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Pending));
        CHECK_COND(r.tx(m));
        CHECK_EQ(r.lim.status().injections, 2);
    }

    // ---- the rate: at least kLimGapMs between two forged lists, kLimMaxInjections per arm ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 1500, 1500));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.request(rq_fixed(5, 1500, 1500));
        r.negotiate(0, rq_fixed(5, 1500, 1500));
        CHECK_COND(!r.tx(m));                      // 200 ms after the last one: too soon
        r.now += kLimGapMs;
        CHECK_COND(r.tx(m));
        CHECK_EQ(r.lim.status().injections, 2);
        for (int i = 0; i < 40 && r.lim.armed(); i++) {
            r.request(rq_fixed(5, 1500, 1500));
            r.now += 1200;
            r.lim.tick(r.now);
            r.negotiate(0, rq_fixed(5, 1500, 1500));
            r.tx(m);
        }
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::Rate));
        CHECK_EQ(r.lim.status().injections, kLimMaxInjections);
    }

    // ---- a Hard Reset within kLimBlameMs of the forged list is a rejection; two end it ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.request(rq_fixed(5, 1500, 1500));        // the sink answered, then the charger resets
        r.now += 40;
        r.lim.on_hard_reset(r.now);
        CHECK_EQ(r.lim.status().rejects, 1);
        CHECK_COND(r.lim.armed());
        CHECK_EQ(int(r.lim.status().state), int(LimState::Armed));
        r.now += 1500;
        r.negotiate(0, rq_fixed(5, 5000, 5000));   // the charger lists again after its reset
        CHECK_COND(r.tx(m));
        r.now += 30;                                // no Request came: the sink side gave up
        r.lim.on_hard_reset(r.now);
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::SinkResets));
        CHECK_EQ(r.lim.status().rejects, 2);
    }
    // a Hard Reset long after it is just a new negotiation, no rejection
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.request(rq_fixed(5, 1500, 1500));
        r.now += kLimBlameMs + 50;
        r.lim.tick(r.now);
        r.lim.on_hard_reset(r.now);
        CHECK_EQ(r.lim.status().rejects, 0);
        CHECK_COND(r.lim.armed());
        CHECK_EQ(int(r.lim.status().state), int(LimState::Armed));
    }
    // Soft_Reset by role: the charger's is ChargerResets, the sink's SinkResets
    {
        Rig r;
        r.lim.arm(30000, r.now);
        TxMsg m;
        for (int k = 0; k < 2; k++) {
            r.negotiate(0, rq_fixed(5, 5000, 5000));
            CHECK_COND(r.tx(m));
            r.now += 10;
            r.rx(hdr(13, 0, 2, kSrc, 7));   // Soft_Reset from the source
            r.now += 1500;
        }
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::ChargerResets));
        Rig s;
        s.lim.arm(30000, s.now);
        for (int k = 0; k < 2; k++) {
            s.negotiate(0, rq_fixed(5, 5000, 5000));
            CHECK_COND(s.tx(m));
            s.now += 10;
            s.rx(hdr(13, 0, 2, kSnk, 7));   // Soft_Reset from the sink
            s.now += 1500;
        }
        CHECK_COND(!s.lim.armed());
        CHECK_EQ(int(s.lim.status().why), int(LimWhy::SinkResets));
    }
    // a real list right after the forged one is the charger starting over: a rejection too
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.now += 20;
        r.caps(6);
        CHECK_EQ(r.lim.status().rejects, 1);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Negotiating));
    }
    // a stable limited link clears the count
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.now += 30;
        r.lim.on_hard_reset(r.now);
        CHECK_EQ(r.lim.status().rejects, 1);
        r.now += 1500;
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        CHECK_COND(r.tx(m));
        r.request(rq_fixed(5, 1500, 1500));
        r.now += kLimBlameMs + 20;
        r.lim.tick(r.now);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Limited));
        CHECK_EQ(r.lim.status().rejects, 0);
    }

    // ---- the sink never answers, or keeps asking too much: counted, then given up ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        TxMsg m;
        for (int k = 0; k < 2; k++) {
            r.negotiate(0, rq_fixed(5, 5000, 5000));
            CHECK_COND(r.tx(m));
            r.now += kLimVerifyMs + 10;
            r.lim.tick(r.now);
            r.now += 1500;
        }
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::SinkIgnores));
        Rig s;
        s.lim.arm(30000, s.now);
        for (int k = 0; k < 2; k++) {
            s.negotiate(0, rq_fixed(5, 5000, 5000));
            CHECK_COND(s.tx(m));
            s.request(rq_fixed(5, 5000, 5000));   // still 100 W
            s.accept_ready(1);
            s.now += 1500;
        }
        CHECK_COND(!s.lim.armed());
        CHECK_EQ(int(s.lim.status().why), int(LimWhy::SinkIgnores));
    }

    // ---- a negotiation that never reaches PS_RDY, or a line that is never quiet, is dropped ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.caps(0);
        r.now += kLimNegMs + 10;
        r.lim.tick(r.now);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Armed));
        CHECK_EQ(r.lim.status().rejects, 0);
        Rig b;
        b.lim.arm(30000, b.now);
        b.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        for (int i = 0; i < 700; i++) {   // chatter every 8 ms: never 10 ms quiet
            b.now += 8;
            b.rx(hdr(1, 0, 1, kSnk, 0));
            b.lim.tick(b.now);
            if (b.lim.take_tx(m, b.now)) break;
        }
        CHECK_EQ(int(b.lim.status().state), int(LimState::Armed));
        CHECK_EQ(b.lim.status().injections, 0);
    }

    // ---- EPR is not ours: any sign of it stops the limiter ----
    {
        Rig a;
        a.lim.arm(30000, a.now);
        a.rx(hdr(0x11, 1, 2, kSrc, 0, true), {0x00, 0x80, 0, 0});   // EPR_Source_Capabilities (extended)
        CHECK_COND(!a.lim.armed());
        CHECK_EQ(int(a.lim.status().why), int(LimWhy::Epr));
        Rig b;
        b.lim.arm(30000, b.now);
        b.rx(hdr(10, 1, 2, kSrc, 0), {0, 0, 0, 0x03});               // EPR_Mode: Enter Succeeded
        CHECK_COND(!b.lim.armed());
        CHECK_EQ(int(b.lim.status().why), int(LimWhy::Epr));
        Rig c;
        c.lim.arm(30000, c.now);
        c.caps(0);
        c.request(rq_fixed(8, 1000, 1000));                          // an EPR object position
        CHECK_COND(!c.lim.armed());
        CHECK_EQ(int(c.lim.status().why), int(LimWhy::Epr));
        Rig d;
        d.lim.arm(30000, d.now);
        d.rx(hdr(10, 1, 2, kSnk, 0), {0, 0, 0, 0x01});                // Enter asked only: not yet a contract
        CHECK_COND(d.lim.armed());
    }

    // ---- detach ends it; disarmed, nothing is sent and nothing is tracked ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        r.lim.on_detach();
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::Detached));
        TxMsg m;
        CHECK_COND(!r.tx(m));
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        CHECK_COND(!r.tx(m));
        CHECK_EQ(r.lim.status().injections, 0);
        // arming again starts clean
        CHECK_COND(r.lim.arm(20000, r.now));
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::None));
        CHECK_EQ(r.lim.status().limit_mw, 20000);
        CHECK_EQ(r.lim.status().injections, 0);
    }

    // ---- SOP' / SOP'' traffic is not the contract ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.lim.on_message(Sop::SopP, hdr(1, 6, 2, kSrc, 0), std::vector<std::uint8_t>(24, 0), r.now);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Armed));
    }

    // ---- the arming gate ----
    {
        const LimFacts ok{true, true, false};
        CHECK_EQ(int(lim_gate(ok, 30000)), int(LimRefusal::None));
        CHECK_EQ(int(lim_gate(ok, kLimMinMw - 1)), int(LimRefusal::BadLimit));
        CHECK_EQ(int(lim_gate(ok, kLimMaxMw + 1)), int(LimRefusal::BadLimit));
        CHECK_EQ(int(lim_gate({false, true, false}, 30000)), int(LimRefusal::NoCc));
        CHECK_EQ(int(lim_gate({true, false, false}, 30000)), int(LimRefusal::NotInline));
        CHECK_EQ(int(lim_gate({true, true, true}, 30000)), int(LimRefusal::Busy));
        CHECK_STR(lim_refusal_name(LimRefusal::NotInline), "not inline");
    }

    // ---- the names (the shell and the wire carry them) ----
    {
        CHECK_STR(lim_state_name(LimState::Limited), "limited");
        CHECK_STR(lim_why_name(LimWhy::ChargerResets), "the charger resets");
        CHECK_STR(lim_why_name(LimWhy::SinkResets), "the sink resets");
    }
    return test_report("test_pd_limiter");
}
