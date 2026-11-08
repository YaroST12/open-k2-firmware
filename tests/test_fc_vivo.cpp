// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// VIVO FlashCharge trigger through fc::Engine against a fake VIVO source speaking the VIVO
// frames: refusals, sequence, voltage ramp, keepalive, release, and the failure paths.
#include <cstdio>
#include <vector>

#include "fc_fake.hpp"

using namespace k2::fc;
namespace vv = k2::vivo;

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

struct FakeVivo : FakeCharger {
    bool open_ok = true;
    bool answers = true;            // acknowledges every command frame
    bool notice_on_open = true;     // sends AA 01 55 when the UART is claimed
    int extra_notices = 0;          // further notices sent after the first command frame
    int overshoot_mv = 0;           // extra volts delivered on a voltage frame (misbehaving source)
    bool drop_after_ramp = false;   // falls back to 5 V once the target was granted
    // A plain source with D+ and D- shorted (a DCP): the K2's own frames come straight back on RX, valid checksum and all.
    // Nothing else answers: no notice, no acks, no voltage change.
    bool loopback = false;
    bool up = false, closed = false;
    std::vector<std::uint8_t> cmds;          // command byte of every frame received, in order
    std::vector<int> volts;                  // requested mV of every command-3 frame
    std::vector<int> milliamps;
    int keepalives = 0, notice_acks = 0;
    std::vector<std::uint8_t> rxq;

    static std::array<std::uint8_t, 8> ack(std::uint8_t cmd) {
        std::array<std::uint8_t, 8> b{0xAA, 0x02, vv::kReplyCmd, cmd, 0};
        b[4] = vv::checksum({b.data(), 4});
        return b;
    }
    void push(std::span<const std::uint8_t> b) { rxq.insert(rxq.end(), b.begin(), b.end()); }
    void notice() {
        std::array<std::uint8_t, 4> n{0xAA, 0x01, vv::kNoticeCmd, 0};
        n[3] = vv::checksum({n.data(), 3});
        push(n);
    }
    bool vivo_open() override {
        if (!open_ok) return false;
        up = true;
        if (notice_on_open) notice();
        return true;
    }
    void ufcs_close() override { closed = true; up = false; }
    bool ufcs_send(std::span<const std::uint8_t> tx) override {
        if (!up) return false;
        if (tx.size() < 4 || tx[0] != vv::kSync || tx.size() != std::size_t(tx[1]) + 3) return false;
        const std::uint8_t cmd = tx[2];
        if (loopback) { cmds.push_back(cmd); push(tx); return true; }
        if (cmd == vv::kReplyCmd) { notice_acks++; return true; }
        cmds.push_back(cmd);
        if (cmd == vv::kKeepaliveCmd && cmds.size() > vv::kSequence.size()) keepalives++;
        if (!answers) return true;
        if (cmd == 0x03) {
            const int ma = tx[3] | tx[4] << 8, mv = tx[5] | tx[6] << 8;
            volts.push_back(mv);
            milliamps.push_back(ma);
            vbus_pending = mv + overshoot_mv;
            apply_at = now + 50;
            if (drop_after_ramp && mv >= 9000) { vbus_pending = 5000; apply_at = now + 400; }
        }
        const auto a = ack(cmd);
        push({a.data(), 5});
        if (extra_notices > 0 && cmds.size() == 3) { extra_notices--; notice(); }
        return true;
    }
    std::size_t ufcs_recv(std::span<std::uint8_t> out) override {
        const std::size_t n = std::min(out.size(), rxq.size());
        for (std::size_t i = 0; i < n; i++) out[i] = rxq[i];
        rxq.erase(rxq.begin(), rxq.begin() + n);
        return n;
    }
    int max_requested() const { int m = 0; for (int v : volts) m = std::max(m, v); return m; }
};

static void refusals() {
    FakeVivo c;
    Engine e(c);
    c.ceiling = 0;
    CHECK(e.start(Proto::Vivo, 5000, false) == Fail::NoMonitor);
    c.ceiling = 36000;
    CHECK(e.start(Proto::Vivo, 9000, false) == Fail::NeedConfirm);
    CHECK(e.start(Proto::Vivo, 6000, true) == Fail::Unsupported);    // neither 5 V nor in the 7..11 V window
    CHECK(e.start(Proto::Vivo, 12000, true) == Fail::Unsupported);   // above the unidentified-charger cap
    c.ceiling = 8000;
    CHECK(e.start(Proto::Vivo, 9000, true) == Fail::OverLimit);
    c.ceiling = 36000;
    c.inline_now = k2::link::Reason::PdContract;
    CHECK(e.start(Proto::Vivo, 5000, false) == Fail::Inline);
    CHECK(c.set_calls == 0 && c.resets == 0 && !c.up);   // refusals never touch the pins
    CHECK(std::string_view(name(Proto::Vivo)) == "VIVO");
}

static void happy_path() {
    FakeVivo c;
    Engine e(c);
    CHECK(e.start(Proto::Vivo, 9000, true) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Active);
    // the canned sequence goes out first, in this order
    const std::uint8_t want[14] = {0x04, 0x55, 0x02, 0x16, 0x01, 0x1E, 0x03, 0x19, 0x1E, 0x1F, 0x16, 0xA6, 0x20, 0x0D};
    CHECK(c.cmds.size() > 14);
    for (std::size_t i = 0; i < 14 && i < c.cmds.size(); i++) CHECK(c.cmds[i] == want[i]);
    CHECK(c.notice_acks >= 1);                      // the entry notice was answered
    // canned 5 V frame inside the sequence, then the ramp from 7 V to the target
    CHECK(c.volts.size() >= 2 && c.volts[0] == 5000 && c.milliamps[0] == 2000);
    CHECK(c.volts[1] == 7000 && c.milliamps[1] == 6000);
    CHECK(c.max_requested() == 9000);
    for (std::size_t i = 2; i < c.volts.size(); i++) CHECK(c.volts[i] >= c.volts[i - 1] && c.volts[i] - c.volts[i - 1] <= 100);   // 100 mV steps
    CHECK(e.reached_mv() >= 8800 && e.reached_mv() <= 9200);
    const int k0 = c.keepalives;
    c.run(e, 1000);
    CHECK(c.keepalives - k0 >= 3 && c.keepalives - k0 <= 12);   // keepalive about every 100 ms
    CHECK(e.state() == State::Active);
    // go to 11 V, then back down
    CHECK(e.set_target(11000, false) == Fail::NeedConfirm);
    CHECK(e.set_target(11000, true) == Fail::None);
    c.run(e, 2500);
    CHECK(c.max_requested() == 11000 && e.state() == State::Active);
    // stop: the canned 5 V frame goes out, then the bus is reset and the UART released
    const std::size_t n = c.volts.size();
    e.stop();
    CHECK(c.volts.size() >= n + 2 && c.volts.back() == 5000 && c.milliamps.back() == 2000);
    CHECK(c.resets >= 1 && c.closed);
    CHECK(e.state() == State::Idle);
}

static void five_volts_never_raises() {
    FakeVivo c;
    Engine e(c);
    CHECK(e.start(Proto::Vivo, 5000, false) == Fail::None);
    c.run(e, 2500);
    CHECK(e.state() == State::Active);
    CHECK(c.max_requested() == 5000);
    CHECK(c.keepalives >= 3);
    CHECK(e.set_target(7000, true) == Fail::None);
    c.run(e, 600);
    CHECK(c.max_requested() == 7000);
    CHECK(e.set_target(5000, false) == Fail::None);   // lowering needs no confirmation
    c.run(e, 400);
    CHECK(c.volts.back() == 5000);
}

static void no_handshake() {
    FakeVivo c;
    c.open_ok = false;
    Engine e(c);
    CHECK(e.start(Proto::Vivo, 5000, false) == Fail::None);
    c.run(e, 200);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoHandshake);
    CHECK(c.resets >= 1);
}

static void no_answer() {
    FakeVivo c;
    c.answers = false;
    c.notice_on_open = false;
    Engine e(c);
    CHECK(e.start(Proto::Vivo, 9000, true) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
    CHECK(c.resets >= 1 && c.closed);
    CHECK(c.max_requested() < 7000);   // nothing above 5 V without a charger that talks back
}

// The K2 hearing itself through a shorted D+/D- is no charger: every target, 5 V included, ends in "no response".
static void own_echo_is_not_a_charger() {
    for (int mv : {5000, 9000}) {
        FakeVivo c;
        c.loopback = true;
        c.notice_on_open = false;
        Engine e(c);
        CHECK(e.start(Proto::Vivo, mv, mv > 5000) == Fail::None);
        c.run(e, 3000);
        CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
        CHECK(c.resets >= 1 && c.closed);
        CHECK(c.max_requested() < 7000);
    }
}

static void notice_restarts_sequence() {
    FakeVivo c;
    c.extra_notices = 1;
    Engine e(c);
    CHECK(e.start(Proto::Vivo, 5000, false) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Active);
    int fours = 0;
    for (auto v : c.cmds) fours += v == 0x04;
    CHECK(fours == 2);                // sequence restarted from index 0
    CHECK(c.notice_acks >= 2);
}

static void overvoltage() {
    FakeVivo c;
    c.overshoot_mv = 3000;
    Engine e(c);
    CHECK(e.start(Proto::Vivo, 9000, true) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage);
    CHECK(c.resets >= 1 && c.volts.back() == 5000);   // asked for 5 V on the way out
}

static void source_lost() {
    FakeVivo c;
    c.drop_after_ramp = true;
    Engine e(c);
    CHECK(e.start(Proto::Vivo, 9000, true) == Fail::None);
    c.run(e, 5000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::SourceLost);
}

int main() {
    refusals();
    happy_path();
    five_volts_never_raises();
    no_handshake();
    no_answer();
    own_echo_is_not_a_charger();
    notice_restarts_sequence();
    overvoltage();
    source_lost();
    std::printf("test_fc_vivo: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
