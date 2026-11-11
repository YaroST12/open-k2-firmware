// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The SinkTxOK rule (PD 3: a sink starts its own message sequence only while the source advertises 3 A on CC): the pure gate,
// and its place in the sink (trigger, PPS keep-alive, Wait retry hold back; answers and the safety paths do not).
#include <vector>

#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pd/pd_txok.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

SourceCaps make_caps() {
    SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    // | 5 V 3 A | 9 V 3 A | 15 V 3 A | 20 V 2 A | PPS 3.3-11 V 3 A | PPS 3.3-21 V 5 A
    const std::uint32_t raw[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000640C8, 0xC0DC2164, 0xC1A4213C};
    c.num_pdos = 6;
    for (int i = 0; i < 6; i++) c.pdos[i] = decode_pdo(raw[i]);
    return c;
}

struct Rig {
    SourceCaps caps = make_caps();
    Sink sink;
    std::uint32_t now = 1000;
    std::vector<TxMsg> sent;
    Rig() {
        sink.bind(&caps);
        sink.set_cap_mv(36000);
        sink.set_proactive_discover(false);
    }
    void pump() {
        TxMsg m;
        while (sink.take_tx(m)) { sent.push_back(m); sink.on_tx_result(true, now); }
    }
    void run(std::uint32_t ms, int rp = -1) {
        for (std::uint32_t i = 0; i < ms; i += 5) {
            now += 5;
            if (rp >= 0) sink.on_rp(std::uint8_t(rp), now);
            sink.tick(now);
            pump();
        }
    }
    void accept_and_ready() { ctl(kCtlAccept); ctl(kCtlPsRdy); }
    void ctl(std::uint8_t type) { sink.on_message(Sop::Sop, make_header(type, 0, 2) | (1u << 8), {}, now); pump(); }
    void data(std::uint8_t type, std::uint32_t obj) {
        const std::uint8_t b[4] = {std::uint8_t(obj), std::uint8_t(obj >> 8), std::uint8_t(obj >> 16), std::uint8_t(obj >> 24)};
        sink.on_message(Sop::Sop, make_header(type, 1, 2) | (1u << 8), b, now);
        pump();
    }
    unsigned last_type() const { return sent.back().header & 0x1F; }
};

void gate_cases() {
    {   // no reading yet: the gate is open (fail open, never a stall)
        TxOkGate g;
        CHECK_COND(!g.hold(2, 100));
    }
    {   // 3 A: go. 1.5 A after a 3 A was seen: wait. Back to 3 A: go again.
        TxOkGate g;
        g.feed(kRpTxOk, 100);
        CHECK_COND(!g.hold(2, 101));
        g.feed(kRpTxNg, 120);
        CHECK_COND(g.hold(2, 121));
        g.feed(kRpTxOk, 140);
        CHECK_COND(!g.hold(2, 141));
        CHECK_EQ(g.waits(), 1);
        CHECK_EQ(g.overrides(), 0);
    }
    {   // bounded: kTxOkWaitMs of 1.5 A and the sink goes anyway, counted once
        TxOkGate g;
        g.feed(kRpTxOk, 0);
        std::uint32_t t = 10;
        g.feed(kRpTxNg, t);
        CHECK_COND(g.hold(2, t));
        for (t = 20; t < 10 + kTxOkWaitMs; t += 10) { g.feed(kRpTxNg, t); CHECK_COND(g.hold(2, t)); }
        t = 10 + kTxOkWaitMs;
        g.feed(kRpTxNg, t);
        CHECK_COND(!g.hold(2, t));
        CHECK_COND(!g.hold(2, t + 1));   // still one override, not one per call
        CHECK_EQ(g.overrides(), 1);
        g.sent();
        g.feed(kRpTxNg, t + 100);
        CHECK_COND(g.hold(2, t + 100));  // the next message starts a new episode
        CHECK_EQ(g.waits(), 2);
    }
    {   // a source that never showed 3 A does not signal TxOK at all: its 1.5 A is just its Rp
        TxOkGate g;
        g.feed(kRpTxNg, 10);
        CHECK_COND(!g.hold(2, 11));
        CHECK_EQ(g.waits(), 0);
    }
    {   // PD 2.0 has no collision avoidance by Rp
        TxOkGate g;
        g.feed(kRpTxOk, 0);
        g.feed(kRpTxNg, 10);
        CHECK_COND(!g.hold(1, 11));
    }
    {   // a reading older than kTxOkFreshMs is no reading; level 0 / 1 are not the TxNG level
        TxOkGate g;
        g.feed(kRpTxOk, 0);
        g.feed(kRpTxNg, 10);
        CHECK_COND(!g.hold(2, 10 + kTxOkFreshMs + 1));
        g.feed(1, 100);
        CHECK_COND(!g.hold(2, 101));
        g.feed(0, 120);
        CHECK_COND(!g.hold(2, 121));
    }
    {   // reset (detach) forgets that 3 A was ever seen
        TxOkGate g;
        g.feed(kRpTxOk, 0);
        g.reset();
        g.feed(kRpTxNg, 10);
        CHECK_COND(!g.hold(2, 11));
    }
}

void sink_cases() {
    {   // trigger while the source shows 1.5 A: nothing goes out until 3 A (or the bound)
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.sink.on_rp(kRpTxNg, r.now + 1);
        r.now += 2;
        CHECK_EQ(int(r.sink.trigger({2, 0, 0}, r.now)), int(Err::Ok));
        r.pump();
        CHECK_EQ(r.sent.size(), 0);
        r.run(30, kRpTxNg);
        CHECK_EQ(r.sent.size(), 0);
        r.run(10, kRpTxOk);
        CHECK_EQ(r.sent.size(), 1);
        CHECK_EQ(r.last_type(), unsigned(kDataRequest));
        CHECK_EQ(int(r.sink.txok().waits()), 1);
        // the Accept timer starts at the real send: a hold shorter than kAcceptMs is not a timeout
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }
    {   // the bound: 1.5 A for the whole kTxOkWaitMs and the Request goes
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.now += 1;
        r.sink.on_rp(kRpTxNg, r.now);
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        r.run(kTxOkWaitMs + 20, kRpTxNg);
        CHECK_EQ(r.sent.size(), 1);
        CHECK_EQ(int(r.sink.txok().overrides()), 1);
    }
    {   // no Rp reading at all (inline, a self-powered brick that never reads): exactly the old behaviour
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), 1);
        CHECK_EQ(int(r.sink.txok().waits()), 0);
    }
    {   // the answer to a re-announced Source_Capabilities is not sink-initiated: it goes out at once whatever the Rp says
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.sink.trigger({2, 0, 0}, r.now); r.pump();
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        const std::size_t n = r.sent.size();
        r.sink.on_rp(kRpTxNg, r.now + 1);
        r.now += 2;
        r.data(kDataSourceCap, 0x0002D12C);   // a list with the held 9 V level in it
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(r.last_type(), unsigned(kDataRequest));
    }
    {   // the safety abort (release to 5 V after a failed guard) is never held back
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.sink.trigger({2, 0, 0}, r.now); r.pump();
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        const std::size_t n = r.sent.size();
        r.sink.on_rp(kRpTxNg, r.now + 1);
        r.now += 2;
        r.sink.release(r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
    }
    {   // the retry after a Wait is a new sink-initiated message: it waits for 3 A as well
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.sink.trigger({2, 0, 0}, r.now); r.pump();
        CHECK_EQ(r.sent.size(), 1);
        r.sink.on_rp(kRpTxNg, r.now + 1);
        r.ctl(kCtlWait);
        r.run(90, kRpTxNg);
        CHECK_EQ(r.sent.size(), 1);          // kWaitRetryMs not over yet
        r.run(30, kRpTxNg);
        CHECK_EQ(r.sent.size(), 1);          // due at +100, held by 1.5 A
        r.run(10, kRpTxOk);
        CHECK_EQ(r.sent.size(), 2);
        CHECK_EQ(r.last_type(), unsigned(kDataRequest));
    }
    {   // Get_Source_Cap asks (Refresh) are sink-initiated too: Busy while held, Ok once the source shows 3 A
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.sink.trigger({2, 0, 0}, r.now); r.pump();
        r.accept_and_ready();
        r.run(20, kRpTxNg);
        CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
        r.run(10, kRpTxOk);
        CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Ok));
    }
    {   // detach forgets the 3 A history: a new attach starts fail open
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.sink.detach();
        r.sink.on_rp(kRpTxNg, r.now + 1);
        r.now += 2;
        r.sink.trigger({2, 0, 0}, r.now); r.pump();
        CHECK_EQ(r.sent.size(), 1);
    }
    {   // a PPS keep-alive waits for 3 A too
        Rig r;
        r.sink.on_rp(kRpTxOk, r.now);
        r.sink.trigger({5, 9100, 2000}, r.now); r.pump();
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        const std::size_t n = r.sent.size();
        r.run(kPpsKeepAliveMs - 100, kRpTxOk);
        CHECK_EQ(r.sent.size(), n);
        r.run(130, kRpTxNg);
        CHECK_EQ(r.sent.size(), n);          // due at +5000, held by 1.5 A for 30 ms of the 50 ms bound
        r.run(30, kRpTxOk);
        CHECK_EQ(r.sent.size(), n + 1);
    }
}

}  // namespace

int main() {
    CHECK_EQ(int(spec::band_mv(20000)), 1000);                 // vSrcNew: +-5 %
    CHECK_EQ(int(spec::kAcceptLimitMs), 52);                   // tSenderResponse max plus the frames on the wire
    CHECK_COND(kAcceptMs > spec::kAcceptLimitMs);              // the K2 itself still takes an Accept the scorer calls late
    gate_cases();
    sink_cases();
    return test_report("test_pd_txok");
}
