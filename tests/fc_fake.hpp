// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A fake QC charger and clock for the engine tests: implements fc::Env, models the HVDCP D+/D- short
// that opens 1.25 s after D+ goes to 0.6 V, the QC2.0 level table and the QC3 +-200 mV pulses.
#pragma once
#include <cstdint>
#include <string_view>
#include <span>
#include <utility>
#include <vector>
#include <algorithm>

#include <array>

#include "k2/fastcharge/accp.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/fastcharge/vooc.hpp"

namespace accp = k2::accp;

struct FakeCharger : k2::fc::Env {
    using Drive = k2::fc::Drive;

    // configuration
    bool hvdcp = true;
    int ceiling = 36000;
    std::uint32_t qc2_delay_ms = 100;
    int qc3_max_mv = 12000;
    k2::link::Reason inline_now = k2::link::Reason::None;   // PD contract / load behind the K2
    bool vbus_known = true;
    bool afc_ok = true;            // AFC charger answers the ping
    bool accp_charger = false;     // answers ACCP register frames (FCP / SCP) once the switch opened
    bool scp_charger = false;
    std::array<bool, 256> nack_write{};   // registers whose write is refused with [0x03, crc] (read-only on that adapter)
    std::array<std::uint8_t, 256> regs{};
    bool scp_on = false;
    std::uint32_t last_xfer = 0;
    bool stuck_at_12v = false;     // misbehaving charger: ignores requests and sits at 12 V once opened
    int dm_fixed_mv = -1;          // >= 0: D- reads this with the pin released (Apple divider 2.7 V, a bare 0 V), never an HVDCP
    // A QC-only bank (Xiaomi 20K, hardware 2026-11-07): it reads D+ 0.6 V / D- 0 V as a QC request and moves VBUS to this
    // many mV 100 ms later, whatever protocol the K2 means to speak (AFC / FCP / SCP never get an answer unless the usual
    // afc_ok / accp_charger knobs say so). 0 = off.
    int dpdm_rise_mv = 0;
    // clock
    std::uint32_t now = 0;
    std::uint32_t us_rem = 0;
    // pins and charger state
    Drive dp = Drive::V0p6, dm = Drive::V0p6;
    bool open = false;
    std::uint32_t dp_since = 0;
    bool continuous = false;
    int vbus = 5000;               // volts the charger delivers right now
    int vbus_pending = 5000;
    int rest_mv = 5000;            // where the output sits with no handshake (a bank reads 5.1-5.2 V unloaded)
    bool hold_vbus = false;        // the watchdog never drops VBUS (the charger keeps its output though it stops answering)
    std::uint32_t apply_at = 0;
    // QC3+ model knobs: mV per 40/50 us pulse, delay before the output starts moving, slew (mV/s, 0 = jump)
    int plus_step_mv = 20, qc3_step_mv = 200, qc3_delay_ms = 20, slew_mv_per_s = 0;
    std::uint32_t slew_t = 0;
    // counters
    std::vector<std::uint8_t> sent;   // bytes passed to xfer
    int resets = 0, set_calls = 0, pulses_up = 0, pulses_down = 0;
    bool resetting = false;
    std::uint32_t reset_until = 0;

    std::uint32_t now_ms() override { return now; }
    int vbus_mv() override {
        if (!vbus_known) return -1;
        if (now >= apply_at) {
            if (slew_mv_per_s > 0 && continuous) {
                const std::uint32_t t0 = slew_t > apply_at ? slew_t : apply_at;
                const int d = static_cast<int>((now - t0) * slew_mv_per_s / 1000);
                if (d > 0) {
                    slew_t = now;
                    vbus += vbus_pending > vbus ? std::min(d, vbus_pending - vbus) : -std::min(d, vbus - vbus_pending);
                }
            } else vbus = vbus_pending;
        }
        if (scp_on && !hold_vbus && now - last_xfer > 500) { scp_on = false; vbus = vbus_pending = 5000; }   // watchdog
        return vbus;
    }
    int ceiling_mv() override { return ceiling; }
    k2::link::Reason inline_reason() override { return inline_now; }
    static int level(Drive d) {
        switch (d) {
        case Drive::V0p6: return 600;
        case Drive::V2p0: return 2000;
        case Drive::V3p3: return 3300;
        default: return 0;
        }
    }
    int dp_mv() override { return level(dp); }
    int dm_mv() override {
        if (dm != Drive::HiZ) return level(dm);
        if (dm_fixed_mv >= 0) return dm_fixed_mv;   // a divider (Apple style) or a bare pin, whatever D+ does
        if (!hvdcp || !open) return level(dp);   // D- follows D+ through the shorted switch
        return 0;                                // charger pull-down after it opened the switch
    }
    void update_open() {
        if (dp == Drive::V0p6 && dm == Drive::HiZ && hvdcp && !open && now - dp_since >= 1250) open = true;
    }
    void apply_levels() {
        if (!open || stuck_at_12v) {
            if (stuck_at_12v && open) vbus_pending = 12000;
            return;
        }
        const bool was_continuous = continuous;
        continuous = dp == Drive::V0p6 && dm == Drive::V3p3;
        if (continuous) {
            if (!was_continuous && rest_mv != 5000) { vbus_pending = rest_mv; apply_at = now + qc3_delay_ms; }   // the bank settles at its own level
            return;
        }
        int mv = 0;
        if (dp == Drive::V0p6 && dm == Drive::V0) mv = 5000;
        else if (dp == Drive::V3p3 && dm == Drive::V0p6) mv = 9000;
        else if (dp == Drive::V0p6 && dm == Drive::V0p6) mv = 12000;
        else if (dp == Drive::V3p3 && dm == Drive::V3p3) mv = 20000;
        else return;
        vbus_pending = mv;
        apply_at = now + qc2_delay_ms;
    }
    void qc_only_rise() {
        if (dpdm_rise_mv > 0 && dp == Drive::V0p6 && dm == Drive::V0) { vbus_pending = dpdm_rise_mv; apply_at = now + 100; }
    }
    void set_dp(Drive d) override {
        set_calls++;
        if (d != dp) {
            if (d == Drive::V0p6 && dp == Drive::V0 && dm == Drive::HiZ && hvdcp && now - dp_since >= 80 && now - dp_since <= 95)
                open = true;   // strict handshake: the 87 ms low pulse of the wake-up sequence
            dp_since = now;
        }
        dp = d;
        if (d == Drive::V0 && dm == Drive::V0) { open = false; continuous = false; vbus_pending = rest_mv; apply_at = now + 100; }
        update_open();
        apply_levels();
        qc_only_rise();
    }
    void set_dm(Drive d) override {
        set_calls++;
        dm = d;
        if (d == Drive::V0 && dp == Drive::V0) { open = false; continuous = false; vbus_pending = rest_mv; apply_at = now + 100; }
        apply_levels();
        qc_only_rise();
    }
    struct Qc3Batch { std::uint32_t t; int vbus, pending, step, n; };
    std::vector<Qc3Batch> qc3_batches;   // one entry per QC3 pulse batch the engine logged
    void log_qc3(int v, int pending, int step, int n) override { qc3_batches.push_back({now, v, pending, step, n}); }
    int coarse_logged = 0;   // log_qc3_coarse calls, with the measured step of the last one
    int coarse_step = 0;
    void log_qc3_coarse(int step) override { ++coarse_logged; coarse_step = step; }
    std::size_t played_steps = 0; std::uint32_t played_us = 0;
    int wakes = 0, vooc_wake_needed = 1;   // the adapter clocks only from the Nth VOOC wake train on
    void play(std::span<const k2::fc::Step> steps) override {
        if (steps.size() == 55) ++wakes;
        for (const auto& s : steps) {
            ++played_steps; played_us += s.us;
            if (continuous && open && !stuck_at_12v) {
                const int step = s.us >= 1000 ? qc3_step_mv : plus_step_mv;
                if (s.dp == Drive::V3p3 && s.dm == Drive::V3p3) { vbus_pending += step; pulses_up++; }
                else if (s.dp == Drive::V0p6 && s.dm == Drive::V0p6) { vbus_pending -= step; pulses_down++; }
                if (vbus_pending < 3600) vbus_pending = 3600;
                if (vbus_pending > qc3_max_mv) vbus_pending = qc3_max_mv;
                apply_at = now + qc3_delay_ms;
            }
            us_rem += s.us;
            now += us_rem / 1000;
            us_rem %= 1000;
        }
    }
    bool accp_xfer(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx) {
        if (!afc_ok || !open || dm != Drive::V0) return false;
        last_xfer = now;
        if (tx.size() == 3 && tx[0] == accp::kCmdRead && tx[2] == accp::crc8(tx.first(2)) && rx.size() == 3) {
            std::uint8_t v = regs[tx[1]];
            if (tx[1] == accp::kRegScpVread) v = accp::hi8(vbus);
            if (tx[1] == accp::kRegScpVread + 1) v = accp::lo8(vbus);
            rx[0] = accp::kAck;
            rx[1] = v;
            rx[2] = accp::crc8(rx.first(2));
            reads.push_back(tx[1]);
            return true;
        }
        if (tx.size() == 4 && tx[0] == accp::kCmdWrite && tx[3] == accp::crc8(tx.first(3)) && rx.size() == 2) {
            writes.push_back({tx[1], tx[2]});
            if (nack_write[tx[1]]) { const std::uint8_t n[1] = {0x03}; rx[0] = n[0]; rx[1] = accp::crc8(n); return true; }
            regs[tx[1]] = tx[2];
            if (tx[1] == accp::kRegFcpVoutEn && tx[2] == 1 && !scp_charger) { vbus_pending = regs[accp::kRegFcpVout] * 100; apply_at = now + 100; }
            if (tx[1] == accp::kRegScpCtrl1 && regs[accp::kRegScpCtrl0] == 0xC0) scp_on = true;
            if (tx[1] == accp::kRegScpVset + 1 && scp_on) { vbus_pending = accp::be16(regs[accp::kRegScpVset], tx[2]); apply_at = now + 50; }
            rx[0] = accp::kAck;
            rx[1] = accp::kSpecWriteAckCrc;
            return true;
        }
        return false;
    }
    std::vector<std::uint8_t> reads;
    std::vector<std::pair<std::uint8_t, std::uint8_t>> writes;
    bool xfer(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx) override {
        for (auto b : tx) sent.push_back(b);
        now += 5;   // the exchange takes a few ms on the wire
        if (accp_charger) return accp_xfer(tx, rx);
        if (!afc_ok || dp != Drive::V0p6 || tx.size() != 1) return false;
        if (tx[0] == 0x46) { vbus_pending = 9000; apply_at = now + 100; }
        else if (tx[0] == 0x79) { vbus_pending = 12000; apply_at = now + 100; }
        if (!rx.empty()) rx[0] = 0x00;
        return true;
    }
    void reset_bus() override {
        resets++;
        resetting = true;
        reset_until = now + 500;
        dp = dm = Drive::V0;
        open = false;
        continuous = false;
        vbus_pending = rest_mv;
        apply_at = now + 100;
    }
    bool reset_busy() override { return now < reset_until; }   // the 500 ms hold of dpdm::start_reset

    // VOOC: the charger sends the queued command frames one per vooc_rx call; replies are recorded.
    std::vector<std::uint8_t> vooc_cmds;
    std::size_t vooc_i = 0;
    std::vector<std::uint16_t> vooc_replies;
    int vooc_rx(std::uint32_t) override {
        if (wakes < vooc_wake_needed || vooc_i >= vooc_cmds.size()) return -1;
        return vooc_cmds[vooc_i++] << 1;   // adapter frame: 8 command bits + a trailing 0
    }
    int vooc_raise_mv = 0;          // the adapter ramps to this once it hears the accepting 0x04D word
    bool vooc_tx(std::uint16_t word) override {
        vooc_replies.push_back(word);
        if (word == 0x04D && vooc_raise_mv) { vbus_pending = vooc_raise_mv; apply_at = now + 100; }
        return true;
    }

    // Advance the clock one millisecond at a time, polling the engine after each.
    void run(k2::fc::Engine& e, std::uint32_t ms) {
        for (std::uint32_t i = 0; i < ms; i++) {
            now++;
            update_open();
            e.poll();
        }
    }
};
