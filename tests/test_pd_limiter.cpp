// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD limiter (k2/pd/pd_limiter.hpp) and comeback store (k2/core/lim_comeback.hpp): per-tech Fixed/PPS/SPR-AVS filtering,
// 0.25 A picker rows, PPS range reduction, Request compliance, state-machine recovery, and wire-replayed trigger behavior.
#include <vector>

#include "k2/core/lim_comeback.hpp"
#include "k2/pd/pd_limiter.hpp"
#include "test_util.h"

using namespace k2::pd;
using k2::bootmode::Mode;

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
constexpr std::uint32_t spr_avs(unsigned ma15, unsigned ma20) {
    return 0xE0000000u | ((ma15 / 10u) << 10) | (ma20 / 10u);
}
constexpr std::uint32_t kFlags1 = 0x0A000000u;   // unconstrained power, USB communications capable
constexpr std::uint32_t kEprBit = 1u << 23;
// A 100 W charger: 5 / 9 / 12 / 15 V at 3 A, 20 V 5 A, a 3.3-11 V 5 A PPS.
const std::uint32_t kCaps[6] = {fixed(5000, 3000, kFlags1 | kEprBit), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000),
                                fixed(20000, 5000), pps(11000, 3300, 5000)};
constexpr std::uint32_t rq_fixed(unsigned pos, unsigned op_ma, unsigned max_ma, std::uint32_t flags = 0) {
    return flags | (pos << 28) | ((op_ma / 10) << 10) | (max_ma / 10);
}
constexpr std::uint32_t rq_pps(unsigned pos, unsigned mv, unsigned ma) { return pos << 28 | mv / 20 << 9 | ma / 50; }
constexpr std::uint32_t rq_avs(unsigned pos, unsigned mv, unsigned ma) { return pos << 28 | mv / 25 << 9 | ma / 50; }

void put32(std::vector<std::uint8_t>& b, std::uint32_t w) {
    for (int i = 0; i < 4; i++) b.push_back(std::uint8_t(w >> (8 * i)));
}
std::uint32_t word(const TxMsg& m, unsigned i) {
    return m.data[4 * i] | (m.data[4 * i + 1] << 8) | (m.data[4 * i + 2] << 16) | (std::uint32_t(m.data[4 * i + 3]) << 24);
}

struct Rig {
    Limiter lim;
    IqWatch watch;              // the driver's always-fed sniffer: what an arm mid-contract is seeded from
    std::uint32_t now = 10000;
    std::uint8_t last_id = 0;   // the charger's newest MessageID

    void rx(std::uint16_t header, std::vector<std::uint8_t> payload = {}) {
        watch.on_message(Sop::Sop, header, payload, now);
        lim.on_message(Sop::Sop, header, payload, now);
        now += 1;
    }
    void caps(std::uint8_t id, std::span<const std::uint32_t> list = kCaps, std::uint8_t rev = 2) {
        std::vector<std::uint8_t> p;
        for (std::uint32_t w : list) put32(p, w);
        rx(hdr(1, std::uint8_t(list.size()), rev, kSrc, id), p);
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
    void negotiate(std::uint8_t id0, std::uint32_t rdo, std::span<const std::uint32_t> list = kCaps) {
        caps(id0, list);
        request(rdo);
        accept_ready(std::uint8_t((id0 + 1) & 7));
    }
    // A quiet line, then ask for the frame.
    bool tx(TxMsg& m, std::uint32_t quiet_ms = kLimQuietMs) {
        now += quiet_ms;
        lim.tick(now);
        return lim.take_tx(m, now);
    }
};

}  // namespace

int main() {
    // ---- request_mw and request_complies ----
    {
        CHECK_EQ(request_mw(rq_fixed(5, 1500, 1500), kCaps), 30000);
        CHECK_EQ(request_mw(rq_fixed(1, 3000, 3000), kCaps), 15000);
        CHECK_EQ(request_mw(rq_pps(6, 9000, 3000), kCaps), 27000);
        CHECK_EQ(request_mw(rq_fixed(7, 1000, 1000), kCaps), 0);
        CHECK_EQ(request_mw(rq_fixed(0, 1000, 1000), kCaps), 0);

        std::uint32_t forged[7]{};
        const LimConfig w30 = lim_config_watts(30000);
        const std::size_t fn = forge_caps(kCaps, w30, forged);
        CHECK_EQ(int(fn), 2);
        CHECK_COND(request_complies(rq_fixed(2, 3000, 3000), kCaps, {forged, fn}, w30));
        // Low-current request on a truncated higher-voltage position (20V 0.5A = 10W) must NOT comply.
        CHECK_COND(!request_complies(rq_fixed(5, 500, 500), kCaps, {forged, fn}, w30));
        // max_ma exceeding forged PDO without GiveBack flag must fail compliance.
        CHECK_COND(!request_complies(rq_fixed(2, 2000, 3500), kCaps, {forged, fn}, w30));
        CHECK_COND(request_complies(rq_fixed(2, 2000, 3500, 1u << 27), kCaps, {forged, fn}, w30));
    }

    // ---- forge_caps: watt-cap removes higher tiers (no tiny high-V clamping) and keeps mandatory PDO 1 ----
    {
        std::uint32_t out[7]{};
        CHECK_EQ(int(forge_caps(kCaps, 30000, out)), 2);   // 5V 3A (15W) + 9V 3A (27W); 12/15/20V/PPS removed
        CHECK_EQ(out[0], fixed(5000, 3000, kFlags1));      // EPR-capable bit cleared
        CHECK_EQ(out[1], fixed(9000, 3000));

        // Non-trailing gap in watt-cap keeps position alignment via 0 mA placeholder.
        CHECK_EQ(int(forge_caps(kCaps, 60000, out)), 6);   // 20V 5A (100W) neutered in-place so PPS (55W) stays at pos 6
        CHECK_EQ(out[4], fixed(20000, 0));
        CHECK_EQ(out[5], kCaps[5]);

        // 10 W clamps mandatory PDO 1 to 2 A and removes everything higher.
        CHECK_EQ(int(forge_caps(kCaps, 10000, out)), 1);
        CHECK_EQ(out[0], fixed(5000, 2000, kFlags1));

        // 100 W changes nothing except PDO 1's EPR-capable bit.
        CHECK_EQ(int(forge_caps(kCaps, 100000, out)), 6);
        CHECK_EQ(out[4], kCaps[4]);
        CHECK_EQ(out[5], kCaps[5]);
    }

    // ---- per-tech Fixed / PPS / SPR-AVS filtering, 100 mA steps, and PPS range clamping ----
    {
        // Charger with Fixed 5..20V, 5-21V 5A PPS, and 15V/20V SPR AVS
        const std::uint32_t charger[7] = {
            fixed(5000, 3000, kFlags1 | kEprBit), fixed(9000, 3000), fixed(12000, 3000), fixed(15000, 3000),
            fixed(20000, 5000), pps(21000, 5000, 5000), spr_avs(3000, 5000)
        };
        std::uint32_t out[7]{};

        // 1. Fixed capped to 9V 2.5A, PPS/AVS off -> clean tail truncation to 2 PDOs.
        LimConfig fx9{};
        fx9.fixed = {true, 9000, 2500};
        fx9.pps.enabled = false;
        fx9.avs.enabled = false;
        CHECK_EQ(int(forge_caps(charger, fx9, out)), 2);
        CHECK_EQ(out[0], fixed(5000, 2500, kFlags1));
        CHECK_EQ(out[1], fixed(9000, 2500));

        // 2. PPS 5-21V -> 5-11V at 3.0A, Fixed & AVS off -> Fixed PDO1 clamped to 100 mA, PDO2..5 neutered at 0 mA, PPS at pos 6.
        LimConfig pps11{};
        pps11.fixed.enabled = false;
        pps11.pps = {true, 11000, 3000};
        pps11.avs.enabled = false;
        CHECK_EQ(int(forge_caps(charger, pps11, out)), 6);
        CHECK_EQ(out[0], fixed(5000, 100, kFlags1));
        CHECK_EQ(out[1], fixed(9000, 0));
        CHECK_EQ(out[4], fixed(20000, 0));
        CHECK_EQ(out[5], pps(11000, 5000, 3000));
        CHECK_COND(request_complies(rq_pps(6, 11000, 3000), charger, {out, 6}, pps11));
        CHECK_COND(!request_complies(rq_pps(6, 12000, 3000), charger, {out, 6}, pps11));
        CHECK_COND(!request_complies(rq_fixed(5, 1000, 1000), charger, {out, 6}, pps11));
        CHECK_COND(!request_complies(rq_fixed(1, 500, 500), charger, {out, 6}, pps11));

        // 3. PPS max_mv below PPS vmin -> drops/neuters PPS cleanly.
        LimConfig pps_drop{};
        pps_drop.fixed.max_mv = 9000;
        pps_drop.pps = {true, 4000, 0};   // < 5000 mV vmin -> dropped
        pps_drop.avs.enabled = false;
        CHECK_EQ(int(forge_caps(charger, pps_drop, out)), 2);

        // 4. SPR AVS capped to 15V 2.5A (both 20V field zeroed and 15V field clamped); 20V @ 3.2A keeps 15V<=3.0A, 20V<=3.2A.
        LimConfig avs15{};
        avs15.avs = {true, 15000, 2500};
        CHECK_EQ(int(forge_caps(charger, avs15, out)), 7);
        CHECK_EQ(out[6], spr_avs(2500, 0));
        CHECK_COND(request_complies(rq_avs(7, 15000, 2500), charger, {out, 7}, avs15));
        CHECK_COND(!request_complies(rq_avs(7, 20000, 2000), charger, {out, 7}, avs15));

        LimConfig avs20{};
        avs20.avs = {true, 20000, 3200};
        CHECK_EQ(int(forge_caps(charger, avs20, out)), 7);
        CHECK_EQ(out[6], spr_avs(3000, 3200));
        CHECK_COND(request_complies(rq_avs(7, 20000, 3200), charger, {out, 7}, avs20));
        CHECK_COND(!request_complies(rq_avs(7, 20000, 3300), charger, {out, 7}, avs20));
    }

    // ---- LimConfig validation and labels ----
    {
        LimConfig c{};
        CHECK_COND(!lim_config_valid(c));   // inactive
        c.fixed.enabled = false; c.pps.enabled = false; c.avs.enabled = false;
        CHECK_COND(!lim_config_valid(c));   // all off
        c = {}; c.fixed.max_ma = 255;       // not a 10 mA multiple
        CHECK_COND(!lim_config_valid(c));
        c = {}; c.fixed.max_ma = 50;        // under 100 mA
        CHECK_COND(!lim_config_valid(c));
        c = {}; c.fixed.max_ma = 5010;      // over 5 A
        CHECK_COND(!lim_config_valid(c));
        c = {}; c.fixed.max_ma = 3250;      // a 0.25 A step
        CHECK_COND(lim_config_valid(c));
        c = {}; c.pps.max_ma = 4800;        // an offer's exact maximum
        CHECK_COND(lim_config_valid(c));
        c = {}; c.fixed.max_ma = 2500;
        CHECK_COND(lim_config_valid(c));
        c = {}; c.avs.max_mv = 18000;       // only 15V/20V valid
        CHECK_COND(!lim_config_valid(c));
        c = {}; c.avs.max_mv = 15000;
        CHECK_COND(lim_config_valid(c));

        char buf[8]{};
        std::size_t n = lim_label(lim_config_watts(27000), buf);
        CHECK_STR(std::string(buf, n).c_str(), "27W");
        LimConfig l1{}; l1.fixed = {true, 9000, 2500}; l1.pps.enabled = false; l1.avs.enabled = false;
        n = lim_label(l1, buf);
        CHECK_EQ(n <= kLimLabelMax, true);
        LimConfig l2{}; l2.fixed.enabled = false; l2.pps = {true, 11000, 0}; l2.avs.enabled = false;
        n = lim_label(l2, buf);
        CHECK_STR(std::string(buf, n).c_str(), "PPS11");
        LimConfig l3{}; l3.avs = {true, 15000, 0};
        n = lim_label(l3, buf);
        CHECK_STR(std::string(buf, n).c_str(), "AVS15");
        // Fixed alone: volts and amps, the 'A' dropped when one over, the volts alone when longer.
        const auto fixed_label = [&](std::uint16_t mv, std::uint16_t ma) {
            LimConfig c{};
            c.fixed = {true, mv, ma};
            return std::string(buf, lim_label(c, buf));
        };
        CHECK_STR(fixed_label(9000, 3000).c_str(), "9V3A");
        CHECK_STR(fixed_label(9000, 1500).c_str(), "9V1.5");
        CHECK_STR(fixed_label(20000, 1500).c_str(), "20V");
        CHECK_STR(fixed_label(9000, 3250).c_str(), "9V");      // 3.25 A does not fit beside the volts and is never rounded up
        CHECK_STR(fixed_label(0, 3250).c_str(), "3.25A");
        CHECK_STR(fixed_label(0, 4800).c_str(), "4.8A");
        LimConfig l4{}; l4.fixed.enabled = false;
        CHECK_STR(std::string(buf, lim_label(l4, buf)).c_str(), "FIX");   // Fixed off: the one constrained
        l4.avs.enabled = false;
        CHECK_STR(std::string(buf, lim_label(l4, buf)).c_str(), "PPS");   // only PPS on
        LimConfig l5{}; l5.fixed.max_mv = 9000; l5.pps.max_ma = 1500;
        CHECK_STR(std::string(buf, lim_label(l5, buf)).c_str(), "9V");    // two constrained, no watts: the Fixed volts
        l5.limit_mw = 27000;
        CHECK_STR(std::string(buf, lim_label(l5, buf)).c_str(), "27W");
        LimConfig l6{}; l6.pps.max_ma = 1500;
        CHECK_STR(std::string(buf, lim_label(l6, buf)).c_str(), "PPS2");
    }

    // ---- the shared 17-byte form (Cmd::PdLimit op 3 and /lim.rec) ----
    {
        LimConfig c{};
        c.fixed = {true, 9000, 2500}; c.pps = {false, 0, 0}; c.avs = {true, 15000, 3000}; c.limit_mw = 45000;
        std::array<std::uint8_t, kLimPackBytes> b{};
        lim_pack(c, b);
        const std::array<std::uint8_t, kLimPackBytes> want{0x05, 0x28, 0x23, 0xC4, 0x09, 0, 0, 0, 0,
                                                           0x98, 0x3A, 0xB8, 0x0B, 0xC8, 0xAF, 0x00, 0x00};
        CHECK_COND(b == want);
        LimConfig d{};
        CHECK_COND(lim_unpack(b, d));
        CHECK_COND(d == c);
        b[0] |= 0x08;
        CHECK_COND(!lim_unpack(b, d));   // an unknown flag bit
        b[0] = 0;
        CHECK_COND(!lim_unpack(b, d));   // every tech off
    }

    // ---- lim_comeback pure encode/decode/restore ----
    {
        LimConfig cfg{};
        cfg.fixed = {true, 9000, 2500};
        cfg.pps = {true, 11000, 3000};
        cfg.avs = {false, 0, 0};
        const auto enc = k2::limcomeback::encode(cfg);
        const auto dec = k2::limcomeback::decode(enc);
        CHECK_COND(dec.has_value());
        CHECK_COND(*dec == cfg);
        auto bad = enc; bad[9] ^= 1;
        CHECK_COND(!k2::limcomeback::decode(bad).has_value());
        CHECK_COND(k2::limcomeback::should_restore(Mode::Plain, false, true, true));
        CHECK_COND(!k2::limcomeback::should_restore(Mode::Plain, false, true, false));   // stale canary
        CHECK_COND(!k2::limcomeback::should_restore(Mode::Plain, true, true, true));     // warm reset
        CHECK_COND(!k2::limcomeback::should_restore(Mode::OkRdCc1, false, true, true));  // key held
    }

    // ---- arming: the limit is a plain SPR config ----
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

    // ---- a first contract that asks for too much: wait for PS_RDY and quiet line, then forge ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.caps(3);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Negotiating));
        r.request(rq_fixed(5, 5000, 5000));                              // 100 W
        TxMsg m;
        CHECK_COND(!r.tx(m));                                            // no PS_RDY yet
        r.accept_ready(4);                                               // Accept id 4, PS_RDY id 5
        CHECK_EQ(int(r.lim.status().state), int(LimState::Pending));
        r.now += 20;
        r.lim.tick(r.now);
        CHECK_COND(!r.lim.take_tx(m, r.now));                            // quiet time is 50 ms
        CHECK_COND(r.tx(m));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Injected));
        CHECK_EQ(m.len, 8);                                              // 5V3A, 9V3A (higher levels removed)
        CHECK_EQ((m.header >> 9) & 7, (5 + 2) & 7);
        CHECK_EQ(word(m, 0), fixed(5000, 3000, kFlags1));
        CHECK_EQ(word(m, 1), fixed(9000, 3000));
        CHECK_EQ(r.lim.status().forged_n, 2);
        // the sink obeys at pos 2 (9V 3A = 27W): verified
        r.request(rq_fixed(2, 3000, 3000));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Limited));
        CHECK_COND(!r.tx(m));
    }

    // ---- no-op real list (already equals forged): skips injection straight to Limited ----
    {
        const std::uint32_t small_caps[2] = {fixed(5000, 3000, kFlags1), fixed(9000, 3000)};
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(2, 3000, 3000), small_caps);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Limited));
        TxMsg m;
        CHECK_COND(!r.tx(m));
        CHECK_EQ(r.lim.status().injections, 0);
    }

    // ---- seeded mid-contract: differs -> Pending (forged immediately); identical -> Limited ----
    {
        Rig r;
        r.negotiate(2, rq_fixed(4, 3000, 3000));   // 15 V 3 A
        r.now += 60;
        CHECK_COND(r.lim.arm(10000, r.now, &r.watch));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Pending));
        TxMsg m;
        CHECK_COND(r.tx(m));
        CHECK_EQ(m.len, 4);
        CHECK_EQ(word(m, 0), fixed(5000, 2000, kFlags1));
        r.request(rq_fixed(1, 2000, 2000));
        CHECK_EQ(int(r.lim.status().state), int(LimState::Limited));

        // Even if current draw is 9V 1A (<10W), charger's cached caps differ (contain 20V 5A), so seed still forges!
        Rig idle_sink;
        idle_sink.negotiate(0, rq_fixed(2, 1000, 1000));
        idle_sink.lim.arm(10000, idle_sink.now, &idle_sink.watch);
        CHECK_EQ(int(idle_sink.lim.status().state), int(LimState::Pending));

        // Charger caps with nothing differing -> Limited immediately.
        const std::uint32_t small_caps[1] = {fixed(5000, 2000, kFlags1)};
        Rig w;
        w.negotiate(0, rq_fixed(1, 2000, 2000), small_caps);
        w.lim.arm(10000, w.now, &w.watch);
        CHECK_EQ(int(w.lim.status().state), int(LimState::Limited));
    }

    // ---- Injected timeout retries with fresh advancing MessageID instead of deadlocking in Armed ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));   // last charger ID = 2
        TxMsg m1, m2;
        CHECK_COND(r.tx(m1));
        CHECK_EQ((m1.header >> 9) & 7, (2 + 2) & 7);   // skip = 1 -> ID = 4
        r.now += kLimVerifyMs + 5;
        r.lim.tick(r.now);
        CHECK_EQ(r.lim.status().ignores, 1);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Pending));   // NOT Armed deadlock!
        r.now += kLimGapMs;
        CHECK_COND(r.tx(m2));
        CHECK_EQ((m2.header >> 9) & 7, (2 + 3) & 7);   // skip = 2 -> fresh ID = 5 (never drops as duplicate)
        r.now += kLimVerifyMs + 5;
        r.lim.tick(r.now);
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::SinkIgnores));
    }

    // ---- Sink Get_Source_Cap followed by charger caps does NOT falsely blame ChargerResets ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.rx(hdr(kCtlGetSourceCap, 0, 2, kSnk, 3));
        r.now += 20;
        r.caps(6);   // charger answers Get_Source_Cap within kLimBlameMs
        CHECK_EQ(r.lim.status().rejects, 0);
        CHECK_EQ(int(r.lim.status().state), int(LimState::Negotiating));
    }

    // ---- Hard Reset and Soft Reset rejection attribution ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        TxMsg m;
        CHECK_COND(r.tx(m));
        r.request(rq_fixed(2, 3000, 3000));
        r.now += 40;
        r.lim.on_hard_reset(r.now);
        CHECK_EQ(r.lim.status().rejects, 1);
        r.now += 1500;
        r.negotiate(0, rq_fixed(5, 5000, 5000));
        CHECK_COND(r.tx(m));
        r.now += 30;
        r.lim.on_hard_reset(r.now);
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::SinkResets));

        Rig sc;
        sc.lim.arm(30000, sc.now);
        for (int k = 0; k < 2; k++) {
            sc.negotiate(0, rq_fixed(5, 5000, 5000));
            CHECK_COND(sc.tx(m));
            sc.now += 10;
            sc.rx(hdr(13, 0, 2, kSrc, 7));
            sc.now += 1500;
        }
        CHECK_COND(!sc.lim.armed());
        CHECK_EQ(int(sc.lim.status().why), int(LimWhy::ChargerResets));
    }

    // ---- rate cap, EPR exit, detach, gate, picks ----
    {
        Rig r;
        r.lim.arm(30000, r.now);
        TxMsg m;
        for (int i = 0; i < 40 && r.lim.armed(); i++) {
            r.negotiate(0, rq_fixed(5, 5000, 5000));
            r.tx(m);
            r.request(rq_fixed(2, 3000, 3000));
            r.now += 1200;
            r.lim.tick(r.now);
        }
        CHECK_COND(!r.lim.armed());
        CHECK_EQ(int(r.lim.status().why), int(LimWhy::Rate));
        CHECK_EQ(r.lim.status().injections, kLimMaxInjections);

        Rig epr;
        epr.lim.arm(30000, epr.now);
        epr.rx(hdr(0x11, 1, 2, kSrc, 0, true), {0x00, 0x80, 0, 0});
        CHECK_EQ(int(epr.lim.status().why), int(LimWhy::Epr));

        Rig dt;
        dt.lim.arm(10000, dt.now);
        dt.negotiate(0, rq_fixed(4, 3000, 3000));
        CHECK_COND(dt.tx(m));
        dt.request(rq_fixed(1, 2000, 2000));
        dt.lim.on_detach();
        CHECK_EQ(int(dt.lim.status().state), int(LimState::Armed));
        CHECK_EQ(dt.lim.status().req_pos, 0);
        CHECK_EQ(dt.lim.status().forged_n, 0);

        CHECK_EQ(int(lim_gate({false}, 30000)), int(LimRefusal::None));
        CHECK_EQ(int(lim_gate({false}, kLimMinMw - 1)), int(LimRefusal::BadLimit));
        CHECK_EQ(int(lim_gate({true}, 30000)), int(LimRefusal::Busy));
    }

    // ---- the Max A picker: Max, then 1 A up in 0.25 A steps to the offer, the offer's exact maximum last ----
    {
        const auto amps = [](std::uint16_t offer) {
            std::string s;
            for (int i = 0; i < lim_amp_count(offer); i++) s += std::to_string(lim_amp_at(offer, i)) + " ";
            return s;
        };
        CHECK_STR(amps(3000).c_str(), "0 1000 1250 1500 1750 2000 2250 2500 2750 3000 ");
        CHECK_STR(amps(3250).c_str(), "0 1000 1250 1500 1750 2000 2250 2500 2750 3000 3250 ");
        CHECK_STR(amps(4800).c_str(), "0 1000 1250 1500 1750 2000 2250 2500 2750 3000 3250 3500 3750 4000 4250 4500 4750 4800 ");
        CHECK_STR(amps(1000).c_str(), "0 1000 ");
        CHECK_STR(amps(500).c_str(), "0 500 ");   // under the 1 A floor: the offer alone
        CHECK_STR(amps(0).c_str(), "0 ");
        CHECK_EQ(lim_amp_count(5000), 18);

        // The offer current behind a voltage. Fixed: that voltage's object (0 V: the highest fixed current).
        const std::uint32_t raw[] = {fixed(5000, 3000), fixed(9000, 3000), fixed(15000, 3250), fixed(20000, 5000),
                                     pps(11000, 5000, 5000), pps(21000, 3300, 3000)};
        Pdo p[std::size(raw)];
        for (std::size_t i = 0; i < std::size(raw); i++) p[i] = decode_pdo(raw[i]);
        CHECK_EQ(lim_fixed_offer_ma(p, 5000), 3000);
        CHECK_EQ(lim_fixed_offer_ma(p, 15000), 3250);
        CHECK_EQ(lim_fixed_offer_ma(p, 20000), 5000);
        CHECK_EQ(lim_fixed_offer_ma(p, 0), 5000);
        CHECK_EQ(lim_fixed_offer_ma(p, 12000), 0);   // no such offer
        // PPS: the highest current among the APDOs whose range covers the voltage (0 V: the highest PPS current).
        CHECK_EQ(lim_pps_offer_ma(p, 5000), 5000);
        CHECK_EQ(lim_pps_offer_ma(p, 11000), 5000);
        CHECK_EQ(lim_pps_offer_ma(p, 4000), 3000);    // only the 3.3-21 V object covers 4 V
        CHECK_EQ(lim_pps_offer_ma(p, 15000), 3000);
        CHECK_EQ(lim_pps_offer_ma(p, 0), 5000);
        CHECK_EQ(lim_pps_offer_ma(std::span<const Pdo>(p, 4), 9000), 0);   // no PPS at all
        // SPR AVS: 15 V the 9-15 V field, Max the 15-20 V field, the highest over two APDOs; an EPR AVS never counts.
        const Pdo a[] = {decode_pdo(spr_avs(3000, 2250)), decode_pdo(spr_avs(3250, 0)), decode_pdo(0xD0000000u | (280u << 17) | (150u << 8) | 140u),
                         decode_pdo(fixed(5000, 3000))};
        CHECK_EQ(lim_avs_offer_ma(a, 15000), 3250);
        CHECK_EQ(lim_avs_offer_ma(std::span<const Pdo>(a, 1), 15000), 3000);
        CHECK_EQ(lim_avs_offer_ma(std::span<const Pdo>(a, 1), 0), 2250);
        CHECK_EQ(lim_avs_offer_ma(a, 0), 3250);      // the 15 V-only object's Max is its 15 V current
        CHECK_EQ(lim_avs_offer_ma(std::span<const Pdo>(a + 2, 2), 0), 0);
    }
    return test_report("test_pd_limiter");
}
