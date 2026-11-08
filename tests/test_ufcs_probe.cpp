// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS probe (k2/fastcharge/ufcs_probe.hpp): the handshake plus the opener exchange at 5 V and nothing else.
// A fake environment scripts the source; the checks cover the guards, the opener schedule, what is
// recorded (every TX frame, every RX byte with its time, the UART error counts) and the result blob.
#include <array>
#include <cstdint>
#include <vector>

#include "k2/fastcharge/ufcs.hpp"
#include "k2/fastcharge/ufcs_probe.hpp"
#include "test_util.h"

using namespace k2::ufcs;

namespace {

struct Fake final : ProbeEnv {
    std::uint32_t now = 1000;
    int vbus = 5000;
    bool inline_flag = false;
    bool open_ok = true;
    Handshake hs{{300, 310}, {320, 330}, 989};
    UartErrors err{};
    int opens = 0, closes = 0, resets = 0;
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::uint32_t> sent_ms;
    struct Rx { std::uint32_t at_ms; std::vector<std::uint8_t> bytes; };
    std::vector<Rx> script;          // delivered by recv() once now >= at_ms
    std::uint32_t opened_ms = 0;
    int vbus_after_ms = -1, vbus_after_value = 0;   // vbus changes this long after the open
    std::uint32_t inline_after_ms = 0xFFFFFFFF;
    bool opened = false;

    std::uint32_t now_ms() override { return now; }
    int vbus_mv() override {
        if (opened && vbus_after_ms >= 0 && now - opened_ms >= static_cast<std::uint32_t>(vbus_after_ms)) return vbus_after_value;
        return vbus;
    }
    bool inline_now() override { return inline_flag || (opened && now - opened_ms >= inline_after_ms); }
    bool open(Handshake& h) override {
        opens++;
        h = hs;
        opened_ms = now;
        opened = true;
        return open_ok;
    }
    bool send(std::span<const std::uint8_t> b) override {
        sent.emplace_back(b.begin(), b.end());
        sent_ms.push_back(now - opened_ms);
        return true;
    }
    std::size_t recv(std::span<std::uint8_t> out) override {
        std::size_t n = 0;
        for (auto& r : script) {
            if (now - opened_ms < r.at_ms || r.bytes.empty()) continue;
            while (!r.bytes.empty() && n < out.size()) { out[n++] = r.bytes.front(); r.bytes.erase(r.bytes.begin()); }
        }
        return n;
    }
    UartErrors errors() override { return err; }
    void close() override { closes++; }
    void reset_bus() override { resets++; }

    // Poll once per millisecond until the probe is done or `ms` elapsed.
    void run(Prober& p, std::uint32_t ms) {
        for (std::uint32_t i = 0; i < ms && p.running(); i++) {
            p.poll();
            now++;
        }
    }
};

std::uint16_t rd16(std::span<const std::uint8_t> b, std::size_t o) { return static_cast<std::uint16_t>(b[o] | b[o + 1] << 8); }

void test_start_refusals() {
    Fake e;
    Prober p(e);
    e.inline_flag = true;
    CHECK_EQ(p.start(), ProbeStart::Inline);
    e.inline_flag = false;
    e.vbus = 9000;
    CHECK_EQ(p.start(), ProbeStart::NotFiveVolts);   // 5 V only: never probe a raised output
    e.vbus = 3000;
    CHECK_EQ(p.start(), ProbeStart::NotFiveVolts);
    e.vbus = -1;
    CHECK_EQ(p.start(), ProbeStart::NoMonitor);
    CHECK_EQ(p.running(), false);
    CHECK_EQ(e.opens, 0);                             // a refusal touches nothing
    CHECK_EQ(e.resets, 0);
    e.vbus = 5000;
    CHECK_EQ(p.start(), ProbeStart::Ok);
    CHECK_EQ(p.start(), ProbeStart::Busy);
    CHECK_EQ(p.running(), true);
}

void test_no_handshake() {
    Fake e;
    e.open_ok = false;
    e.hs = {{-1, -1}, {-1, -1}, -1};
    Prober p(e);
    CHECK_EQ(p.start(), ProbeStart::Ok);
    e.run(p, 50);
    CHECK_EQ(p.running(), false);
    CHECK_EQ(p.result().verdict, ProbeVerdict::NoHandshake);
    CHECK_EQ(p.result().hs_class, HsClass::NoAnswer);
    CHECK_EQ(e.sent.size(), 0u);    // nothing is sent without a handshake
    CHECK_EQ(e.closes, 0);          // the UART never opened
    CHECK_EQ(e.resets, 1);          // the lines go back to the bus reset
}

void test_silent_source_gets_the_opener_schedule() {
    Fake e;
    Prober p(e);
    CHECK_EQ(p.start(), ProbeStart::Ok);
    e.run(p, 700);
    CHECK_EQ(p.running(), false);
    CHECK_EQ(p.result().verdict, ProbeVerdict::Silent);
    CHECK_EQ(p.result().hs_class, HsClass::Delayed);
    CHECK_EQ(p.result().tx_frames, 8);
    // 0 ms, then three fast retransmits at 20 ms, then the keep-alive pace until the 500 ms window ends.
    const std::uint32_t want[] = {0, 20, 40, 60, 160, 260, 360, 460};
    CHECK_EQ(e.sent.size(), 8u);
    for (std::size_t i = 0; i < e.sent.size() && i < 8; i++) {
        CHECK_EQ(e.sent_ms[i], want[i]);
        const std::array<std::uint8_t, 5> opener{0xAA, 0x20, 0x08, 0x00, 0x63};   // Ping, the standard's first message
        CHECK_EQ(e.sent[i].size(), 5u);
        CHECK_EQ(std::equal(opener.begin(), opener.end(), e.sent[i].begin()), true);
    }
    CHECK_EQ(p.result().total_ms, 500);
    CHECK_EQ(e.closes, 1);
    CHECK_EQ(e.resets, 1);
    // The blob log carries the same eight TX frames, in order.
    const auto blob = p.blob();
    CHECK_EQ(blob[0], 1);                 // format version
    CHECK_EQ(blob[1], 2);                 // done
    CHECK_EQ(blob[2], static_cast<std::uint8_t>(ProbeVerdict::Silent));
    CHECK_EQ(blob[3], 2);                 // delayed
    CHECK_EQ(static_cast<std::int16_t>(rd16(blob, 4)), 300);    // rise 1
    CHECK_EQ(static_cast<std::int16_t>(rd16(blob, 6)), 320);    // fall 1
    CHECK_EQ(static_cast<std::int16_t>(rd16(blob, 8)), 310);    // rise 2
    CHECK_EQ(static_cast<std::int16_t>(rd16(blob, 10)), 330);   // fall 2
    CHECK_EQ(static_cast<std::int16_t>(rd16(blob, 12)), 989);   // final
    CHECK_EQ(blob[14], 8);
    CHECK_EQ(rd16(blob, 16), 0);          // no RX bytes
    CHECK_EQ(rd16(blob, 26), 5000);       // VBUS at the start
    CHECK_EQ(rd16(blob, 32), 500);
    const std::size_t log_len = rd16(blob, 34);
    CHECK_EQ(log_len, 8u * (4 + 5));      // 8 events of header 4 + 5 bytes
    CHECK_EQ(blob.size(), kProbeHeader + log_len);
    CHECK_EQ(rd16(blob, kProbeHeader), 0);                        // first event at 0 ms
    CHECK_EQ(blob[kProbeHeader + 2], 0);                          // kind TX
    CHECK_EQ(blob[kProbeHeader + 3], 5);
    CHECK_EQ(blob[kProbeHeader + 4], 0xAA);
    CHECK_EQ(rd16(blob, kProbeHeader + 9), 20);                   // second event at 20 ms
}

void test_break_byte_is_garbage_with_its_timestamp() {
    Fake e;
    e.script.push_back({130, {0x00}});
    e.err.framing = 1;
    Prober p(e);
    CHECK_EQ(p.start(), ProbeStart::Ok);
    e.run(p, 700);
    CHECK_EQ(p.result().verdict, ProbeVerdict::Garbage);   // bytes arrived, none decodable
    CHECK_EQ(p.result().rx_bytes, 1);
    CHECK_EQ(p.result().rx_frames, 0);
    CHECK_EQ(p.result().err.framing, 1);
    const auto blob = p.blob();
    CHECK_EQ(rd16(blob, 16), 1);
    CHECK_EQ(rd16(blob, 18), 1);   // framing errors
    // find the RX event: after the TX events up to 130 ms (0,20,40,60 => 4 events)
    std::size_t o = kProbeHeader;
    bool found = false;
    while (o < blob.size()) {
        const std::uint8_t kind = blob[o + 2], n = blob[o + 3];
        if (kind == 1) {
            found = true;
            CHECK_EQ(rd16(blob, o), 130);
            CHECK_EQ(n, 1);
            CHECK_EQ(blob[o + 4], 0x00);
        }
        o += 4u + n;
    }
    CHECK_EQ(found, true);
}

void test_answer_is_acked_and_never_followed_by_a_request() {
    Fake e;
    // Output_Capabilities from the source at 30 ms: one 5 V entry.
    std::array<std::uint8_t, 16> f{};
    const std::array<std::uint8_t, 8> cap{0x10, 0x01, 0xF4, 0x01, 0xF4, 0x01, 0x2C, 0x01};
    const std::size_t n = encode_data(f, kDataOutputCap, cap, 3);
    e.script.push_back({30, {f.begin(), f.begin() + n}});
    Prober p(e);
    CHECK_EQ(p.start(), ProbeStart::Ok);
    e.run(p, 700);
    CHECK_EQ(p.result().verdict, ProbeVerdict::Answered);
    CHECK_EQ(p.result().rx_frames, 1);
    CHECK_EQ(p.result().rx_bytes, static_cast<int>(n));
    int acks = 0, requests = 0, openers_after = 0;
    for (std::size_t i = 0; i < e.sent.size(); i++) {
        Frame fr;
        std::size_t used = 0;
        CHECK_EQ(decode(e.sent[i], fr, used), Parse::Ok);
        if (!fr.data && fr.cmd == kCtlAck) {
            acks++;
            CHECK_EQ(fr.id, 3);   // acknowledges the source's message id
        }
        if (fr.data && fr.cmd == kDataRequest) requests++;
        if (e.sent_ms[i] > 30 && !fr.data && fr.cmd == kCtlPing) openers_after++;
    }
    CHECK_EQ(acks, 1);
    CHECK_EQ(requests, 0);       // the probe never sends a Request
    CHECK_EQ(openers_after, 0);  // and stops repeating the opener once answered
    CHECK_EQ(p.result().total_ms, 130);   // 100 ms after the answer
    CHECK_EQ(e.closes, 1);
    CHECK_EQ(e.resets, 1);
}

void test_guards_abort_and_release() {
    {
        Fake e;
        e.vbus_after_ms = 50;
        e.vbus_after_value = 6500;
        Prober p(e);
        CHECK_EQ(p.start(), ProbeStart::Ok);
        e.run(p, 700);
        CHECK_EQ(p.result().verdict, ProbeVerdict::Overvoltage);
        CHECK_EQ(p.result().vbus_max, 6500);
        CHECK_EQ(e.closes, 1);
        CHECK_EQ(e.resets, 1);
    }
    {
        Fake e;
        e.vbus_after_ms = 50;
        e.vbus_after_value = 300;
        Prober p(e);
        CHECK_EQ(p.start(), ProbeStart::Ok);
        e.run(p, 700);
        CHECK_EQ(p.result().verdict, ProbeVerdict::SourceLost);
        CHECK_EQ(p.result().vbus_min, 300);
        CHECK_EQ(e.resets, 1);
    }
    {
        Fake e;
        e.inline_after_ms = 40;
        Prober p(e);
        CHECK_EQ(p.start(), ProbeStart::Ok);
        e.run(p, 700);
        CHECK_EQ(p.result().verdict, ProbeVerdict::Inline);
        CHECK_EQ(e.resets, 1);
    }
}

void test_stop_ends_the_probe_and_releases() {
    Fake e;
    Prober p(e);
    CHECK_EQ(p.start(), ProbeStart::Ok);
    e.run(p, 100);
    CHECK_EQ(p.running(), true);
    p.stop();
    CHECK_EQ(p.running(), false);
    CHECK_EQ(p.result().verdict, ProbeVerdict::Stopped);
    CHECK_EQ(e.closes, 1);
    CHECK_EQ(e.resets, 1);
    p.stop();   // idempotent
    CHECK_EQ(e.resets, 1);
    // A probe that never ran has nothing to stop.
    Fake e2;
    Prober q(e2);
    q.stop();
    CHECK_EQ(e2.resets, 0);
}

void test_tuner_shapes_the_probe() {
    Fake e;
    Tuner t;
    t.opener = 1;
    t.crc_sync = true;
    t.listen_ms = 100;
    t.retry_ms = 30;
    Prober p(e);
    p.set_tuner(t);
    CHECK_EQ(p.start(), ProbeStart::Ok);
    e.run(p, 300);
    CHECK_EQ(p.result().verdict, ProbeVerdict::Silent);
    CHECK_EQ(p.result().total_ms, 100);
    std::array<std::uint8_t, 8> want{};
    const std::size_t n = encode_control(want, 1, 0, t);
    CHECK_EQ(e.sent.size() >= 3, true);
    CHECK_EQ(std::equal(want.begin(), want.begin() + n, e.sent[0].begin()), true);
    CHECK_EQ(e.sent_ms[1], 30u);
    // A reply in the tuned format decodes; the same bytes in the default format would not.
    Fake e2;
    std::array<std::uint8_t, 8> reply{};
    const std::size_t m = encode_control(reply, kCtlAck, 0, t);
    e2.script.push_back({10, {reply.begin(), reply.begin() + m}});
    Prober q(e2);
    q.set_tuner(t);
    CHECK_EQ(q.start(), ProbeStart::Ok);
    e2.run(q, 300);
    CHECK_EQ(q.result().verdict, ProbeVerdict::Answered);
}

void test_log_overflow_is_flagged_not_written() {
    Fake e;
    std::vector<std::uint8_t> noise(2000, 0x55);
    for (std::uint32_t at = 10; at < 400; at += 10) e.script.push_back({at, noise});
    Prober p(e);
    CHECK_EQ(p.start(), ProbeStart::Ok);
    e.run(p, 700);
    const auto blob = p.blob();
    CHECK_EQ(blob.size() <= kProbeBlobMax, true);
    CHECK_EQ(blob[36] & 1, 1);   // truncated
    CHECK_EQ(blob.size(), kProbeHeader + rd16(blob, 34));
}

}  // namespace

int main() {
    test_start_refusals();
    test_no_handshake();
    test_silent_source_gets_the_opener_schedule();
    test_break_byte_is_garbage_with_its_timestamp();
    test_answer_is_acked_and_never_followed_by_a_request();
    test_guards_abort_and_release();
    test_stop_ends_the_probe_and_releases();
    test_tuner_shapes_the_probe();
    test_log_overflow_is_flagged_not_written();
    return test_report("test_ufcs_probe");
}
