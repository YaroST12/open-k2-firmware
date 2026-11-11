// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS trigger through fc::Engine against a fake UFCS source that behaves like a real one (reverse-engineered):
// every message that is not an ACK / NCK is acknowledged with its own message number, the source numbers its
// own messages, answers Get_Output_Capabilities with the Baseus FH22 charger's two output modes, answers a Request with
// Accept and, after a delay, Power_Ready (the output moves then), and takes Exit_UFCS_Mode as "back to 5 V".
// The failure knobs cover the lost-ACK, NCK, missing-answer, missing-Power_Ready and dead-source paths.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "fc_fake.hpp"

using namespace k2::fc;
namespace uf = k2::ufcs;

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

struct FakeUfcs : FakeCharger {
    // How the source behaves.
    bool handshake_ok = true;
    bool answers = true;               // false: never speaks (no ACK, nothing)
    bool reject = false;               // Refuse (busy) instead of Accept
    bool no_caps = false;              // ACKs Get_Output_Capabilities but never sends the capabilities
    bool no_power_ready = false;       // Accept, but never Power_Ready
    int accept_copies = 1;             // the Accept goes out this many times back to back (its retries piled up in the UART ring)
    bool dead_after_ready = false;     // stops answering anything once Power_Ready was sent
    bool deaf_to_requests = false;     // never ACKs a Request
    bool loopback = false;             // D+/D- shorted (DCP): everything the sink transmits comes straight back to its receiver
    int drop_msgs = 0;                 // ignores (no ACK) the next N messages of the sink
    int drop_requests = 0;             // ignores the next N Request frames
    int nack_msgs = 0;                 // answers the next N messages with NCK
    int overshoot_mv = 0;              // extra volts delivered on a Request (misbehaving source)
    std::uint32_t ack_ms = 1, resp_ms = 5, power_ms = 100;   // the source's own timing
    // Baseus FH22 charger (hardware log): 3.40-5.50 V 1.00-3.00 A and 5.50-11.00 V 1.00-4.00 A, 50 mA steps.
    std::vector<std::uint8_t> caps = {0x18, 0x02, 0x26, 0x01, 0x54, 0x01, 0x2C, 0x64, 0x28, 0x04, 0x4C, 0x02, 0x26, 0x01, 0x90, 0x64};
    uf::Tuner st = [] { uf::Tuner t; t.addr = uf::kAddrSink; return t; }();   // the source addresses the sink

    // What the source saw.
    struct Seen { bool data; int cmd, id; std::uint32_t at; std::vector<std::uint8_t> payload; };
    struct Wire { std::uint32_t at; std::vector<std::uint8_t> bytes; };
    struct Req { int pos, mv, ma; };
    bool up = false, closed = false, accepted = false;
    std::uint32_t accept_at = 0, ready_at = 0;
    int pings = 0, getcaps = 0, exits = 0, soft_resets = 0, acks = 0, req_frames = 0;
    int id_errors = 0, ack_mismatch = 0;
    int sid = 0, exp = 0;                  // source's next message number, number it expects from the sink
    std::vector<int> owed;                 // numbers of the source's messages the sink has not acknowledged yet, oldest first
    std::vector<Seen> seen;                // every message of the sink that is not an ACK / NCK, retransmissions included
    std::vector<Wire> wire;                // every frame of the sink
    std::vector<Req> requests;             // Requests the source accepted
    struct Ev { std::uint32_t at; std::vector<std::uint8_t> b; };
    std::vector<Ev> evs;                   // frames in flight to the sink
    std::vector<std::uint8_t> rxq;
    std::vector<std::uint8_t> preload;                       // bytes already on the wire when the UART opens
    std::vector<std::pair<uf::Why, std::size_t>> drops;      // (reason, bytes dropped) as the engine reports them
    std::vector<std::pair<int, int>> frames_seen;            // (data?, cmd) of each decoded frame
    std::vector<std::string> retries;                        // phase named by each retransmit report
    void log_ufcs_drop(std::span<const std::uint8_t>, std::size_t used, uf::Why why) override { drops.push_back({why, used}); }
    void log_ufcs_frame(const uf::Frame& f) override { frames_seen.push_back({f.data ? 1 : 0, f.cmd}); }
    void log_ufcs_retry(const char* what, int, std::span<const std::uint8_t>) override { retries.push_back(what); }

    bool powered() const { return ready_at != 0 && now >= ready_at; }

    // Frames the source sends.
    void push(std::uint32_t at, const std::uint8_t* p, std::size_t n) { evs.push_back({at, {p, p + n}}); }
    void push_ack(int id) {
        std::array<std::uint8_t, 8> b{};
        const auto n = uf::encode_control(b, uf::kCtlAck, std::uint8_t(id), st);
        push(now + ack_ms, b.data(), n);
    }
    void src_ctl(std::uint8_t cmd, std::uint32_t at) {
        std::array<std::uint8_t, 8> b{};
        const auto n = uf::encode_control(b, cmd, std::uint8_t(sid), st);
        owed.push_back(sid);
        sid = (sid + 1) & 15;
        push(at, b.data(), n);
    }
    void src_data(std::uint8_t cmd, std::span<const std::uint8_t> payload, std::uint32_t at) {
        std::array<std::uint8_t, 40> b{};
        const auto n = uf::encode_data(b, cmd, payload, std::uint8_t(sid), st);
        owed.push_back(sid);
        sid = (sid + 1) & 15;
        push(at, b.data(), n);
    }
    void src_refuse(int refused_id, bool refused_data, int refused_cmd, std::uint8_t reason, std::uint32_t at) {
        std::array<std::uint8_t, 16> b{};
        const auto n = uf::encode_refuse(b, std::uint8_t(sid), std::uint8_t(refused_id), refused_data ? 1 : 0, std::uint8_t(refused_cmd), reason, st);
        owed.push_back(sid);
        sid = (sid + 1) & 15;
        push(at, b.data(), n);
    }
    void src_soft_reset() {   // the source resets its own counters as it sends
        src_ctl(uf::kCtlSoftReset, now + 1);
        sid = 0;
        exp = 0;
    }

    bool ufcs_open() override {
        if (!handshake_ok) return false;
        up = true;
        rxq.insert(rxq.end(), preload.begin(), preload.end());
        return true;
    }
    void ufcs_close() override { closed = true; up = false; }

    void request(const uf::Frame& f, std::uint32_t t) {
        std::uint64_t w = 0;
        for (auto b : f.payload) w = w << 8 | b;
        const Req r{int(w >> 60), int((w >> 16) & 0xFFFF) * 10, int(w & 0xFFFF) * 10};
        std::array<uf::Cap, uf::kMaxCaps> cs{};
        const auto n = uf::decode_caps(caps, cs);
        bool ok = false;
        for (std::size_t i = 0; i < n; i++) {
            const uf::Cap& c = cs[i];
            if (c.pos == r.pos && r.mv >= c.min_mv && r.mv <= c.max_mv && r.ma >= c.min_ma && r.ma <= c.max_ma &&
                r.mv % c.step_mv == 0 && r.ma % c.step_ma == 0)
                ok = true;
        }
        if (reject || !ok) {
            src_refuse(f.id, true, f.cmd, reject ? uf::kRefuseBusy : uf::kRefuseRange, t);
            return;
        }
        requests.push_back(r);
        src_ctl(uf::kCtlAccept, t);
        for (int i = 1; i < accept_copies; i++) evs.push_back({t, evs.back().b});   // same bytes, same message number
        accepted = true;
        accept_at = t;
        vbus_pending = r.mv + overshoot_mv;
        apply_at = t + power_ms;
        if (!no_power_ready) {
            ready_at = t + power_ms;
            src_ctl(uf::kCtlPowerReady, ready_at);
        }
    }

    bool ufcs_send(std::span<const std::uint8_t> tx) override {
        if (!up) return false;
        uf::Frame f;
        std::size_t used = 0;
        if (uf::decode(tx, f, used) != uf::Parse::Ok) return false;
        wire.push_back({now, {tx.begin(), tx.end()}});
        if (loopback) rxq.insert(rxq.end(), tx.begin(), tx.end());
        if (!f.data && f.cmd == uf::kCtlAck) {
            acks++;
            if (owed.empty() || owed.front() != f.id) ack_mismatch++;   // acknowledged in order, each once
            else owed.erase(owed.begin());
            return true;
        }
        if (!f.data && f.cmd == uf::kCtlNack) return true;
        seen.push_back({f.data, f.cmd, f.id, now, {f.payload.begin(), f.payload.end()}});
        if (!f.data && f.cmd == uf::kCtlPing) pings++;
        if (!f.data && f.cmd == uf::kCtlSoftReset) soft_resets++;
        if (!f.data && f.cmd == uf::kCtlExit) exits++;
        if (f.data && f.cmd == uf::kDataRequest) req_frames++;
        if (!f.data && f.cmd == uf::kCtlExit) { vbus_pending = 5000; apply_at = now + 10; ready_at = 0; }
        if (!answers || (dead_after_ready && powered())) return true;
        if (drop_msgs > 0) { drop_msgs--; return true; }
        if (f.data && f.cmd == uf::kDataRequest) {
            if (deaf_to_requests) return true;
            if (drop_requests > 0) { drop_requests--; return true; }
        }
        if (nack_msgs > 0) {
            nack_msgs--;
            std::array<std::uint8_t, 8> b{};
            const auto n = uf::encode_control(b, uf::kCtlNack, f.id, st);
            push(now + ack_ms, b.data(), n);
            return true;
        }
        if (!f.data && f.cmd == uf::kCtlSoftReset) {   // both sides restart their counters
            push_ack(f.id);
            exp = 0;
            sid = 0;
            owed.clear();
            return true;
        }
        if (f.id == exp) exp = (exp + 1) & 15;
        else if (f.id == ((exp + 15) & 15)) { push_ack(f.id); return true; }   // a repeat: acknowledged again, not served again
        else { id_errors++; exp = (f.id + 1) & 15; }
        push_ack(f.id);
        const std::uint32_t t = now + ack_ms + resp_ms;
        if (!f.data && f.cmd == uf::kCtlGetOutputCap) {
            getcaps++;
            if (!no_caps) src_data(uf::kDataOutputCap, caps, t);
        } else if (f.data && f.cmd == uf::kDataRequest) {
            request(f, t);
        }
        return true;
    }
    std::size_t ufcs_recv(std::span<std::uint8_t> out) override {
        std::vector<Ev> due;
        for (std::size_t i = 0; i < evs.size();) {
            if (evs[i].at <= now) { due.push_back(std::move(evs[i])); evs.erase(evs.begin() + std::ptrdiff_t(i)); }
            else i++;
        }
        std::stable_sort(due.begin(), due.end(), [](const Ev& a, const Ev& b) { return a.at < b.at; });
        for (const auto& e : due) rxq.insert(rxq.end(), e.b.begin(), e.b.end());
        const std::size_t n = std::min(out.size(), rxq.size());
        for (std::size_t i = 0; i < n; i++) out[i] = rxq[i];
        rxq.erase(rxq.begin(), rxq.begin() + std::ptrdiff_t(n));
        return n;
    }
    // Run until `pred` holds or `ms` ran out; the ms it took.
    template <class P> std::uint32_t run_until(Engine& e, P pred, std::uint32_t ms) {
        std::uint32_t i = 0;
        for (; i < ms && !pred(); i++) run(e, 1);
        return i;
    }
};

static std::vector<std::uint8_t> ctl(std::uint8_t cmd, std::uint8_t id) {
    std::array<std::uint8_t, 8> b{};
    const auto n = uf::encode_control(b, cmd, id);
    return {b.begin(), b.begin() + std::ptrdiff_t(n)};
}

static void refusals() {
    FakeUfcs c;
    Engine e(c);
    c.ceiling = 0;
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::NoMonitor);
    c.ceiling = 36000;
    CHECK(e.start(Proto::Ufcs, 9000, false) == Fail::NeedConfirm);
    CHECK(e.start(Proto::Ufcs, 2000, true) == Fail::OverLimit);   // below the 3.3 V floor
    c.ceiling = 8000;
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::OverLimit);
    c.ceiling = 36000;
    c.inline_now = k2::link::Reason::PdContract;
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::Inline);
    CHECK(c.set_calls == 0 && c.resets == 0 && !c.up);   // refusals never touch the pins
}

// The whole negotiation as the Baseus FH22 charger log shows it: Ping (message 0), Get_Output_Capabilities (1),
// our ACK of the capabilities (their message 0), Request (2), our ACK of Accept (their 1) and of Power_Ready
// (their 2); then a Ping every 100 ms.
static void happy_path() {
    FakeUfcs c;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    CHECK(c.getcaps == 1);
    CHECK(c.requests.size() == 1 && c.requests[0].pos == 2 && c.requests[0].mv == 9000 && c.requests[0].ma == 2000);
    CHECK(c.seen.size() >= 4);
    CHECK(c.seen[0].cmd == uf::kCtlPing && !c.seen[0].data && c.seen[0].id == 0);
    CHECK(c.seen[1].cmd == uf::kCtlGetOutputCap && !c.seen[1].data && c.seen[1].id == 1);
    CHECK(c.seen[2].cmd == uf::kDataRequest && c.seen[2].data && c.seen[2].id == 2);
    CHECK(c.seen[3].cmd == uf::kCtlPing && c.seen[3].id == 3);     // the keep-alive carries the next number
    CHECK(c.id_errors == 0 && c.ack_mismatch == 0 && c.soft_resets == 0);
    CHECK(c.acks == 3);                                            // Output_Capabilities, Accept, Power_Ready
    // the wire bytes of the Baseus FH22 charger log: Ping, Get_Output_Capabilities, our ACK of message 0, the Request header
    CHECK(c.wire.size() >= 4);
    CHECK((c.wire[0].bytes == std::vector<std::uint8_t>{0xAA, 0x20, 0x08, 0x00, 0x63}));
    CHECK((c.wire[1].bytes == std::vector<std::uint8_t>{0xAA, 0x22, 0x08, 0x06, 0xD6}));
    CHECK((c.wire[2].bytes == std::vector<std::uint8_t>{0xAA, 0x20, 0x08, 0x01, 0x4A}));
    CHECK(c.wire[3].bytes.size() == 14 && c.wire[3].bytes[1] == 0x24 && c.wire[3].bytes[2] == 0x09 && c.wire[3].bytes[3] == 0x02);
    CHECK(c.seen[2].at - c.wire[2].at >= uf::kMsgTransDelayMs);    // the Request follows our ACK by tMsgTransDelay
    CHECK(e.reached_mv() >= 8800 && e.reached_mv() <= 9200);
    // keep-alive: one Ping per 100 ms
    const int p0 = c.pings;
    c.run(e, 1000);
    CHECK(c.pings - p0 >= 9 && c.pings - p0 <= 11);
    CHECK(e.state() == State::Active);
    // a level above 5 V needs the confirmation, then 5 V is asked from the first output mode
    CHECK(e.set_target(10000, false) == Fail::NeedConfirm);
    CHECK(e.set_target(5000, false) == Fail::None);
    c.run(e, 800);
    CHECK(c.requests.size() == 2 && c.requests[1].pos == 1 && c.requests[1].mv == 5000 && c.requests[1].ma == 2000);
    CHECK(e.state() == State::Active);
    CHECK(c.id_errors == 0 && c.ack_mismatch == 0 && c.soft_resets == 0);
    // stop: Exit_UFCS_Mode (no Request), then the bus reset and the UART released
    e.stop();
    CHECK(c.exits == 1 && c.seen.back().cmd == uf::kCtlExit);
    CHECK(c.requests.size() == 2);
    CHECK(c.resets >= 1 && c.closed);
    CHECK(e.state() == State::Idle);
}

static void no_handshake() {
    FakeUfcs c;
    c.handshake_ok = false;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::None);
    c.run(e, 200);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoHandshake);
    CHECK(c.resets >= 1);
}

static void no_answer() {
    FakeUfcs c;
    c.answers = false;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::None);
    c.run(e, 1000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
    CHECK(c.resets >= 1);
    CHECK(c.pings >= 3);   // the opener (a Ping) was retried before giving up
    CHECK(c.getcaps == 0);
}

// Chicony W20-045N1B (2026-11-07): a DCP short that also lets the released pair rise passes the handshake, then
// USART2 TX on D- comes back on D+. The echo is our own transmission (receiver address = source), never a source:
// it is not acknowledged, not acted on, and the session ends as a handshake failure, not "no response".
static void loopback_is_no_handshake() {
    FakeUfcs c;
    c.answers = false;
    c.loopback = true;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::None);
    c.run(e, 1000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoHandshake);
    CHECK(c.acks == 0);                         // no ACK to an echo
    CHECK(c.getcaps == 0);                      // an ACKed Ping would have moved on to Get_Output_Capabilities
    CHECK(c.pings >= 3 && c.pings <= 12);       // the opener kept its own pace
    CHECK(c.resets >= 1 && c.closed);
}

// The echo and a real source together (a source behind a short is not expected, but the guard must not cost a real
// reply): echoes are skipped, the sink-addressed ACK and the answers are served, the session reaches the level.
static void echo_beside_a_real_source() {
    FakeUfcs c;
    c.loopback = true;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    CHECK(c.getcaps == 1);
    CHECK(c.requests.size() == 1 && c.requests[0].mv == 9000);
    CHECK(c.acks == 3);                         // Output_Capabilities, Accept, Power_Ready: none for an echo
    CHECK(c.ack_mismatch == 0 && c.id_errors == 0);
}

// A frame addressed to the source or a cable is not ours to answer either, whatever it carries.
static void foreign_address_is_ignored() {
    FakeUfcs c;
    c.answers = false;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::None);
    c.run(e, 5);
    uf::Tuner other;                            // default tuner addresses the source
    std::array<std::uint8_t, 8> b{};
    for (const auto addr : {uf::kAddrSource, uf::kAddrCable}) {
        other.addr = addr;
        const auto n = uf::encode_control(b, uf::kCtlGetOutputCap, 0, other);
        c.push(c.now + 1, b.data(), n);
    }
    c.run(e, 40);
    CHECK(c.acks == 0);
    CHECK(e.state() != State::Failed);
}

// A source may need a few hundred ms to start its UFCS side: the opener keeps coming for the whole
// listen window (a handful of retries fast, then at the keep-alive pace), then the attempt ends.
static void ping_listen_window() {
    FakeUfcs c;
    c.answers = false;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::None);
    c.run(e, 400);
    CHECK(e.state() != State::Failed);                 // still listening at 400 ms
    CHECK(c.pings >= 5);
    c.run(e, 200);                                     // 600 ms: past the 500 ms window
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
    CHECK(c.pings <= 12);                              // not a 20 ms flood
    CHECK(!c.retries.empty() && c.retries[0] == "open");
    for (const auto& s : c.seen) CHECK(s.id == 0);     // retransmissions keep the message number
}

// The tuner changes what the engine puts on the wire: header bits, opener code, CRC span, retransmit
// interval and listen window. A silent source sees only the opener frames.
struct TunedSilent : FakeUfcs {
    uf::Tuner tn;
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::uint32_t> sent_ms;
    const uf::Tuner& ufcs_tuner() override { return tn; }
    bool ufcs_send(std::span<const std::uint8_t> tx) override {
        sent.emplace_back(tx.begin(), tx.end());
        sent_ms.push_back(now);
        return true;
    }
    void reply_ctl(std::uint8_t cmd, std::uint8_t id) {
        const auto b = ctl(cmd, id);
        rxq.insert(rxq.end(), b.begin(), b.end());
    }
};

static void tuner_shapes_the_opener() {
    TunedSilent c;
    c.tn.version = 2;
    c.tn.addr = 3;
    c.tn.opener = 1;
    c.tn.crc_sync = true;
    c.tn.retry_ms = 50;
    c.tn.listen_ms = 300;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 5000, false) == Fail::None);
    c.run(e, 200);
    CHECK(!c.sent.empty());
    std::array<std::uint8_t, 8> want{};
    const auto n = uf::encode_control(want, 1, 0, c.tn);
    CHECK(n == 5 && c.sent[0].size() == 5 && std::equal(c.sent[0].begin(), c.sent[0].end(), want.begin()));
    CHECK(c.sent[0][1] == 0x60 && c.sent[0][2] == 0x10);   // receiver address 3, message number 0, version 2
    CHECK(c.sent.size() >= 3 && c.sent_ms[1] - c.sent_ms[0] == 50);   // the tuned retransmit interval
    CHECK(c.sent_ms[2] - c.sent_ms[1] == 50);
    CHECK(e.state() != State::Failed);                // 200 ms: still inside the 300 ms window
    c.run(e, 200);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);   // ended by the tuned window
    // A frame in the tuned format is decoded; the default format is not.
    TunedSilent d;
    d.tn.crc_sync = true;
    Engine f(d);
    CHECK(f.start(Proto::Ufcs, 5000, false) == Fail::None);
    d.reply_ctl(uf::kCtlAck, 0);   // default-format frame: wrong CRC under the tuner
    d.run(f, 20);
    CHECK(d.frames_seen.empty());
    std::array<std::uint8_t, 8> good{};
    const auto m = uf::encode_control(good, uf::kCtlAck, 0, d.tn);
    d.rxq.insert(d.rxq.end(), good.begin(), good.begin() + std::ptrdiff_t(m));
    d.run(f, 20);
    CHECK(d.frames_seen.size() == 1);
}

// Get_Output_Capabilities as the opener: its ACK is followed by the capabilities, no Ping first.
struct OpenerCaps : FakeUfcs {
    uf::Tuner tn;
    const uf::Tuner& ufcs_tuner() override { return tn; }
};

static void capabilities_opener() {
    OpenerCaps c;
    c.tn.opener = uf::kCtlGetOutputCap;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    CHECK(c.getcaps == 1 && c.pings >= 1);
    CHECK(c.seen[0].cmd == uf::kCtlGetOutputCap && c.seen[0].id == 0);
    CHECK(c.seen[1].cmd == uf::kDataRequest && c.seen[1].id == 1);
    CHECK(c.id_errors == 0);
}

// A source that starts late still gets through: nothing before 300 ms, then it answers the next opener.
static void late_source_is_heard() {
    FakeUfcs c;
    c.answers = false;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 300);
    CHECK(e.state() != State::Failed);
    c.answers = true;
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    CHECK(c.id_errors == 0);
}

// Bytes the receiver cannot use are dropped and reported with the reason, not silently lost.
static void drops_are_reported() {
    FakeUfcs c;
    // A frame with a corrupted CRC (the sync byte is dropped, then its 4 sync-less bytes and 3 bytes
    // of line noise as garbage), then the real thing arrives as usual.
    std::array<std::uint8_t, 8> bad{};
    const auto n = uf::encode_control(bad, uf::kCtlAck, 0);
    bad[n - 1] ^= 0x5A;
    c.preload.assign(bad.begin(), bad.begin() + std::ptrdiff_t(n));
    c.preload.insert(c.preload.end(), {0x00, 0xFF, 0x13});
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);                 // the garbage did not stop the session
    bool nosync = false, crc = false;
    for (const auto& d : c.drops) {
        if (d.first == uf::Why::NoSync && d.second == 7) nosync = true;
        if (d.first == uf::Why::Crc && d.second == 1) crc = true;
    }
    CHECK(nosync);
    CHECK(crc);
    CHECK(!c.frames_seen.empty());                     // decoded frames are reported too
    CHECK(c.frames_seen[0].first == 0 && c.frames_seen[0].second == uf::kCtlAck);   // the Ping's ACK
}

static void level_not_offered() {
    FakeUfcs c;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 30000, true) == Fail::None);   // above every entry (the Baseus FH22 charger stops at 11 V)
    c.run(e, 1000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoBand);
    CHECK(c.requests.empty() && c.req_frames == 0);
    FakeUfcs d;
    Engine f(d);
    CHECK(f.start(Proto::Ufcs, 12000, true) == Fail::None);
    d.run(f, 1000);
    CHECK(f.state() == State::Failed && f.fail() == Fail::NoBand);
    CHECK(d.req_frames == 0);
}

// A Refuse (data message 9) to the Request ends the attempt.
static void rejected() {
    FakeUfcs c;
    c.reject = true;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoBand);
    CHECK(c.req_frames == 1 && c.resets >= 1);
    CHECK(!c.accepted);
}

// Accept is not the end: VBUS is judged only after Power_Ready (tPowerSupply), and not before.
static void accept_then_power_ready() {
    FakeUfcs c;
    c.power_ms = 300;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run_until(e, [&] { return c.accepted && c.now >= c.accept_at; }, 600);
    c.run(e, 10);
    CHECK(e.state() == State::Negotiating);
    c.run(e, 200);                                     // 210 ms after the Accept: the output has not moved yet
    CHECK(e.state() == State::Negotiating && e.reached_mv() < 6000);
    CHECK(c.pings == 1);                               // no keep-alive before Power_Ready
    c.run(e, 400);
    CHECK(e.state() == State::Active);
    CHECK(c.id_errors == 0 && c.ack_mismatch == 0);
    // a source close to the limit (540 ms) still passes
    FakeUfcs d;
    d.power_ms = 540;
    Engine f(d);
    CHECK(f.start(Proto::Ufcs, 9000, true) == Fail::None);
    d.run(f, 1800);
    CHECK(f.state() == State::Active);
}

// No Power_Ready after the Accept: a hardware reset, once tPowerSupply (550 ms) plus slack has passed.
static void missing_power_ready() {
    FakeUfcs c;
    c.no_power_ready = true;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    const auto t = c.run_until(e, [&] { return e.state() == State::Failed; }, 3000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoBand);
    CHECK(c.resets == 1 && c.closed);
    CHECK(c.accepted && c.now - c.accept_at >= uf::kPowerSupplyMs && c.now - c.accept_at <= uf::kPowerSupplyMs + 100);
    CHECK(t < 3000);
}

// The source stays quiet once running: ACKs go missing, the ping is retransmitted nMsgRetryCount times, a
// Soft_Reset (also unanswered) follows, then the bus is reset.
static void source_lost() {
    FakeUfcs c;
    c.dead_after_ready = true;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::SourceLost);
    CHECK(c.resets >= 1);
    CHECK(c.soft_resets == 1 + uf::kMsgRetryCount);    // one Soft_Reset, retransmitted like any message
    int last_ping_id = -1, same = 0;
    for (const auto& s : c.seen) {
        if (s.cmd != uf::kCtlPing || s.at < c.ready_at) continue;
        if (s.id == last_ping_id) same++;
        last_ping_id = s.id;
    }
    CHECK(same >= uf::kMsgRetryCount);                 // the unanswered ping went out again with its own number
}

static void overvoltage() {
    FakeUfcs c;
    c.overshoot_mv = 3000;   // the source delivers 12 V on a 9 V Request
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage);
}

// NCK: the sender retransmits at once, same message number.
static void nck_is_retransmitted() {
    FakeUfcs c;
    c.nack_msgs = 1;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 10);
    CHECK(c.pings == 2 && c.seen[0].id == 0 && c.seen[1].id == 0);
    CHECK(c.seen[1].at - c.seen[0].at <= 5);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    CHECK(c.soft_resets == 0 && c.resets == 0 && c.id_errors == 0);
}

// A lost ACK: the message is repeated after tACKReceive with the same number, the source serves it once.
static void lost_ack_is_retransmitted() {
    FakeUfcs c;
    c.drop_msgs = 2;           // the opener goes unanswered twice
    c.drop_requests = 2;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 2000);
    CHECK(e.state() == State::Active);
    CHECK(c.soft_resets == 0 && c.resets == 0 && c.id_errors == 0);
    CHECK(c.pings >= 3);
    CHECK(c.seen[0].id == 0 && c.seen[1].id == 0 && c.seen[2].id == 0);
    CHECK(c.req_frames == 3 && c.requests.size() == 1);
    CHECK(c.seen[1].at - c.seen[0].at >= 20 && c.seen[1].at - c.seen[0].at <= 25);
}

// A Request that is never acknowledged: 3 retransmissions, a Soft_Reset (acknowledged), the capabilities again,
// the same failure, then the hardware reset.
static void request_never_acked() {
    FakeUfcs c;
    c.deaf_to_requests = true;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
    CHECK(c.resets >= 1);
    CHECK(c.soft_resets == 1);
    CHECK(c.req_frames == 2 * (1 + uf::kMsgRetryCount));
    CHECK(c.getcaps == 2);
    CHECK(c.id_errors == 0);
}

// The source never sends the capabilities: Get_Output_Capabilities is asked again with a new number, 3 tries.
static void capabilities_never_come() {
    FakeUfcs c;
    c.no_caps = true;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 2000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
    CHECK(c.getcaps == 3);
    CHECK(c.seen[1].id == 1 && c.seen[2].id == 2 && c.seen[3].id == 3);
    CHECK(c.resets >= 1 && c.soft_resets == 0);
}

// TOPADRE 65 W (2026-11-08): the main loop stalled ~65 ms in an LCD redraw right after the Request, the source sent its
// Accept four times (one per tACKReceive timeout) and all four sat in the UART ring. They are one message (same
// number): the Accept is honoured once and ACKed once; only a retry that arrives in a later read (our ACK may have
// been lost) gets another ACK (8.2.3.2: every message that is not an ACK / NCK is acknowledged).
static void identical_retries_in_one_read() {
    FakeUfcs c;
    c.accept_copies = 4;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run_until(e, [&] { return c.req_frames == 1; }, 1000);
    CHECK(c.req_frames == 1);
    c.now += 65;                                       // the stall: nobody polls the engine
    c.run(e, 1500);
    CHECK(e.state() == State::Active);                 // the Accept counted: Power_Ready was awaited and came
    CHECK(c.requests.size() == 1 && c.soft_resets == 0 && c.id_errors == 0);
    CHECK(c.acks == 3 && c.ack_mismatch == 0);         // Output_Capabilities, Accept (once), Power_Ready
    // the same Accept again in a later read: acknowledged again, nothing else happens
    std::array<std::uint8_t, 8> b{};
    const auto n = uf::encode_control(b, uf::kCtlAccept, 1, c.st);
    c.push(c.now + 1, b.data(), n);
    c.run(e, 20);
    CHECK(c.acks == 4);
    CHECK(e.state() == State::Active && c.soft_resets == 0);
}

// Polling the engine more than once per millisecond (the LCD idle hook does it during a redraw) changes nothing on
// the wire: same messages, same numbers, same count.
static void extra_polls_are_harmless() {
    FakeUfcs a, b;
    Engine ea(a), eb(b);
    CHECK(ea.start(Proto::Ufcs, 9000, true) == Fail::None);
    CHECK(eb.start(Proto::Ufcs, 9000, true) == Fail::None);
    a.run(ea, 1500);
    for (int i = 0; i < 1500; i++) {
        b.run(eb, 1);
        eb.service_ufcs();
        eb.poll();
        eb.service_ufcs();
    }
    CHECK(ea.state() == State::Active && eb.state() == State::Active);
    CHECK(a.wire.size() == b.wire.size());
    CHECK(a.acks == b.acks && a.seen.size() == b.seen.size());
    CHECK(b.id_errors == 0 && b.ack_mismatch == 0 && b.soft_resets == 0);
}

// Soft_Reset from the source in the Keep phase: it is acknowledged, the numbers restart at 0, the opening runs again
// (Ping, capabilities, the same Request) and the session is Active again.
static void soft_reset_from_source() {
    FakeUfcs c;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    const int p0 = c.pings;
    c.run_until(e, [&] { return c.pings > p0; }, 300);   // right after a Ping the next one is 100 ms away
    c.run(e, 5);
    const auto before = c.seen.size();
    const int acks0 = c.acks;
    c.src_soft_reset();
    c.run(e, 350);
    CHECK(e.state() == State::Active);
    CHECK(c.acks == acks0 + 4);                        // the Soft_Reset, then Output_Capabilities, Accept, Power_Ready
    CHECK(c.requests.size() == 2 && c.requests[1].mv == 9000);
    CHECK(c.seen.size() >= before + 3);
    CHECK(c.seen[before].cmd == uf::kCtlPing && c.seen[before].id == 0);   // numbering restarted
    CHECK(c.seen[before + 1].id == 1);
    CHECK(c.id_errors == 0 && c.resets == 0);
}

// The source drops the pending Power_Ready and restarts the session: what a real source does with a Soft_Reset.
static void src_reset_session(FakeUfcs& c) {
    c.evs.clear();
    c.owed.clear();
    c.ready_at = 0;
    c.src_soft_reset();
}

// TOPADRE 65 W (2026-11-08): Request ACKed, Accept heard, then the source sent Soft_Reset instead of Power_Ready. The
// engine used to wait out the 600 ms Power_Ready timer and fail "voltage not reached". It starts over instead
// (Ping, Get_Output_Capabilities, the same Request) and reaches the level (reverse-engineered: a Soft_Reset restarts both counters).
static void soft_reset_in_power_wait() {
    FakeUfcs c;
    c.power_ms = 300;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 11000, true) == Fail::None);
    c.run_until(e, [&] { return c.accepted; }, 1000);
    c.run(e, 20);                                      // the Accept is read: the engine waits for Power_Ready
    CHECK(c.requests.size() == 1 && e.state() == State::Negotiating);
    const auto at = c.now;
    const auto seen0 = c.seen.size();
    src_reset_session(c);
    c.run(e, 2000);
    CHECK(e.state() == State::Active && e.fail() == Fail::None);
    CHECK(c.requests.size() == 2 && c.requests[1].pos == c.requests[0].pos && c.requests[1].mv == 11000);
    CHECK(c.getcaps == 2 && c.resets == 0 && c.soft_resets == 0);
    CHECK(c.id_errors == 0 && c.ack_mismatch == 0);
    CHECK(c.seen.size() > seen0 + 2);
    if (c.seen.size() <= seen0 + 2) return;
    CHECK(c.seen[seen0].cmd == uf::kCtlPing && c.seen[seen0].id == 0);          // numbering restarted, opener first
    CHECK(c.seen[seen0 + 1].cmd == uf::kCtlGetOutputCap && c.seen[seen0 + 1].id == 1);
    CHECK(c.seen[seen0 + 2].cmd == uf::kDataRequest && c.seen[seen0 + 2].at - at < 100);
    CHECK(e.reached_mv() >= 10800 && e.reached_mv() <= 11200);
}

// A second Soft_Reset before the contract is up is not answered with a third try: the source keeps resetting, the
// engine stops and says so. Once Power_Ready came the budget is renewed (a reset in the Keep phase starts over again).
static void soft_reset_twice_is_bounded() {
    FakeUfcs c;
    c.power_ms = 300;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 11000, true) == Fail::None);
    c.run_until(e, [&] { return c.accepted; }, 1000);
    c.run(e, 20);
    src_reset_session(c);
    c.run_until(e, [&] { return c.requests.size() == 2; }, 1000);
    c.run(e, 20);                                      // second Accept read, Power_Ready pending again
    CHECK(e.state() == State::Negotiating);
    src_reset_session(c);
    c.run(e, 1000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::SourceLost);
    CHECK(c.requests.size() == 2 && c.getcaps == 2 && c.id_errors == 0);

    FakeUfcs k;
    k.power_ms = 50;
    Engine ek(k);
    CHECK(ek.start(Proto::Ufcs, 11000, true) == Fail::None);
    k.run(ek, 1500);
    CHECK(ek.state() == State::Active);
    for (int i = 1; i <= 2; i++) {                     // Power_Ready has come: each reset starts over once
        k.src_soft_reset();
        k.run(ek, 1500);
        CHECK(ek.state() == State::Active && k.requests.size() == std::size_t(1 + i));
    }
    CHECK(k.id_errors == 0 && k.resets == 0);
}

// Messages the sink cannot serve are refused (Refuse, reason "unsupported"), after the ACK and tMsgTransDelay.
static void unsupported_requests_are_refused() {
    FakeUfcs c;
    Engine e(c);
    CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    const int src_id = c.sid;
    const auto before = c.seen.size();
    const auto wires = c.wire.size();
    c.src_ctl(uf::kCtlGetSinkInfo, c.now + 1);
    c.run(e, 50);
    CHECK(e.state() == State::Active);
    std::size_t refuse = 0;
    for (std::size_t i = before; i < c.seen.size(); i++)
        if (c.seen[i].data && c.seen[i].cmd == uf::kDataRefuse) refuse = i;
    CHECK(refuse != 0);
    if (refuse) {
        const auto& p = c.seen[refuse].payload;
        CHECK(p.size() == 4 && p[0] == src_id && p[1] == 0 && p[2] == uf::kCtlGetSinkInfo && p[3] == uf::kRefuseUnsupported);
        // our ACK first (the wire entry after the source's frame), the Refuse at least tMsgTransDelay later
        std::uint32_t ack_at = 0;
        for (std::size_t i = wires; i < c.wire.size(); i++)
            if (c.wire[i].bytes.size() == 5 && c.wire[i].bytes[3] == uf::kCtlAck) { ack_at = c.wire[i].at; break; }
        CHECK(ack_at != 0 && c.seen[refuse].at - ack_at >= uf::kMsgTransDelayMs);
    }
    CHECK(c.acks >= 4 && c.ack_mismatch == 0);         // and the source's ACK of the Refuse came back as expected
    CHECK(c.soft_resets == 0 && c.id_errors == 0);
    // Get_Cable_Info and a Verify_Request are refused too; a Ping from the source only gets its ACK
    const auto b2 = c.seen.size();
    c.src_ctl(uf::kCtlPing, c.now + 1);
    c.run(e, 40);
    for (std::size_t i = b2; i < c.seen.size(); i++) CHECK(!(c.seen[i].data && c.seen[i].cmd == uf::kDataRefuse));
}

int main() {
    refusals();
    happy_path();
    no_handshake();
    no_answer();
    loopback_is_no_handshake();
    echo_beside_a_real_source();
    foreign_address_is_ignored();
    ping_listen_window();
    tuner_shapes_the_opener();
    capabilities_opener();
    late_source_is_heard();
    drops_are_reported();
    level_not_offered();
    rejected();
    accept_then_power_ready();
    missing_power_ready();
    source_lost();
    overvoltage();
    nck_is_retransmitted();
    lost_ack_is_retransmitted();
    request_never_acked();
    capabilities_never_come();
    identical_retries_in_one_read();
    extra_polls_are_harmless();
    soft_reset_from_source();
    soft_reset_in_power_wait();
    soft_reset_twice_is_bounded();
    unsupported_requests_are_refused();
    std::printf("test_fc_ufcs: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
