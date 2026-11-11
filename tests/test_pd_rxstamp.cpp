// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The RX edge stamp (INT_N ISR -> one microsecond timestamp) and what the sink makes of it: the Request's TX_START, the Accept and
// PS_RDY edges, and the source's Wait / Reject / Soft_Reset / unsolicited Source_Capabilities counted for the PD Stress scorer.
#include <vector>

#include "k2/pd/pd_rxstamp.hpp"
#include "k2/pd/pd_sink.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

SourceCaps make_caps() {
    SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    const std::uint32_t raw[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000640C8};
    c.num_pdos = 4;
    for (int i = 0; i < 4; i++) c.pdos[i] = decode_pdo(raw[i]);
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
    // the driver's order: hand over TX_START's stamp, then the result
    void pump(std::uint32_t tx_us = 0) {
        TxMsg m;
        while (sink.take_tx(m)) { sent.push_back(m); sink.set_stamp_us(tx_us); sink.on_tx_result(true, now); }
    }
    void ctl(std::uint8_t type, std::uint32_t us = 0) {
        sink.set_stamp_us(us);
        sink.on_message(Sop::Sop, make_header(type, 0, 2) | (1u << 8), {}, now);
        pump();
    }
    void data(std::uint8_t type, std::uint32_t obj, std::uint32_t us = 0) {
        const std::uint8_t b[4] = {std::uint8_t(obj), std::uint8_t(obj >> 8), std::uint8_t(obj >> 16), std::uint8_t(obj >> 24)};
        sink.set_stamp_us(us);
        sink.on_message(Sop::Sop, make_header(type, 1, 2) | (1u << 8), b, now);
        pump();
    }
};

void stamp_cases() {
    {   // no edge: nothing to take
        RxStamp s;
        std::uint32_t us = 7;
        CHECK_COND(!s.take(1000, us));
    }
    {   // an edge is taken once
        RxStamp s;
        s.edge(5000);
        std::uint32_t us = 0;
        CHECK_COND(s.take(5400, us));
        CHECK_EQ(int(us), 5000);
        CHECK_COND(!s.take(5400, us));
    }
    {   // the latest edge wins: the TXSENT edge of our Request is overwritten by the Accept's
        RxStamp s;
        s.edge(5000);
        s.edge(31000);
        std::uint32_t us = 0;
        CHECK_COND(s.take(31500, us));
        CHECK_EQ(int(us), 31000);
    }
    {   // an edge older than kMaxAgeUs belongs to something else
        RxStamp s;
        s.edge(5000);
        std::uint32_t us = 0;
        CHECK_COND(!s.take(5000 + RxStamp::kMaxAgeUs + 1, us));
        s.edge(5000);
        CHECK_COND(s.take(5000 + RxStamp::kMaxAgeUs, us));
    }
    {   // the 32-bit microsecond counter wraps: age is a difference
        RxStamp s;
        s.edge(0xFFFFFF00u);
        std::uint32_t us = 0;
        CHECK_COND(s.take(0x100u, us));
        CHECK_EQ(int(us), int(0xFFFFFF00u));
    }
    {   // clear forgets the edge
        RxStamp s;
        s.edge(100);
        s.clear();
        std::uint32_t us = 0;
        CHECK_COND(!s.take(200, us));
    }
    {   // an edge at micros 0 still counts (the flag, not the value, says there is one)
        RxStamp s;
        s.edge(0);
        std::uint32_t us = 9;
        CHECK_COND(s.take(10, us));
        CHECK_EQ(int(us), 0);
    }
}

void sink_cases() {
    {   // Request TX_START, Accept edge, PS_RDY edge: kept as microseconds, never 0 for "happened"
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump(100000);
        CHECK_EQ(int(r.sink.status().req_us), 100001);
        CHECK_EQ(int(r.sink.status().accept_us), 0);
        r.ctl(kCtlAccept, 125000);
        CHECK_EQ(int(r.sink.status().accept_us), 125001);
        r.ctl(kCtlPsRdy, 620000);
        CHECK_EQ(int(r.sink.status().ps_rdy_us), 620001);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }
    {   // a message with no stamp leaves 0 (unknown), and the stamp is one-shot: the next message does not inherit it
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump(100000);
        r.sink.set_stamp_us(125000);
        r.sink.on_message(Sop::Sop, make_header(kCtlGoodCrc, 0, 2), {}, r.now);   // consumes the stamp
        r.ctl(kCtlAccept);
        CHECK_EQ(int(r.sink.status().accept_us), 0);
    }
    {   // a new Request clears the previous answers' stamps
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump(100000);
        r.ctl(kCtlAccept, 125000); r.ctl(kCtlPsRdy, 620000);
        r.sink.trigger({3, 0, 0}, r.now);
        r.pump(900000);
        CHECK_EQ(int(r.sink.status().req_us), 900001);
        CHECK_EQ(int(r.sink.status().accept_us), 0);
        CHECK_EQ(int(r.sink.status().ps_rdy_us), 0);
    }
    {   // a failed TX leaves the Request stamp alone (the sink retries it)
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        TxMsg m;
        CHECK_COND(r.sink.take_tx(m));
        r.sink.set_stamp_us(100000);
        r.sink.on_tx_result(false, r.now);
        CHECK_EQ(int(r.sink.status().req_us), 0);
    }
    {   // Wait and Reject are counted when they answer a Request
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        r.ctl(kCtlWait);
        CHECK_EQ(int(r.sink.status().waits_rx), 1);
        CHECK_EQ(int(r.sink.status().rejects_rx), 0);
    }
    {
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        r.ctl(kCtlReject);
        CHECK_EQ(int(r.sink.status().rejects_rx), 1);
    }
    {   // Soft_Reset from the source
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        r.ctl(kCtlSoftReset);
        CHECK_EQ(int(r.sink.status().soft_resets_rx), 1);
    }
    {   // a Source_Capabilities nobody asked for, inside a contract
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.status().caps_unsolicited), 0);
        r.data(kDataSourceCap, 0x0002D12C);
        CHECK_EQ(int(r.sink.status().caps_unsolicited), 1);
    }
    {   // the first announcement of an attach (no contract yet) is not unexpected
        Rig r;
        r.data(kDataSourceCap, 0x0001912C);
        CHECK_EQ(int(r.sink.status().caps_unsolicited), 0);
    }
    {   // the answer to our own Get_Source_Cap is solicited
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Ok));
        r.pump();
        r.data(kDataSourceCap, 0x0002D12C);
        CHECK_EQ(int(r.sink.status().caps_unsolicited), 0);
    }
}

}  // namespace

int main() {
    stamp_cases();
    sink_cases();
    return test_report("test_pd_rxstamp");
}
