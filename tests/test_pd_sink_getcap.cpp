// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the sink's own Get_Source_Cap (Sink::request_source_caps), the layer-1 half of the Connection page's Refresh: it
// goes through the sink's one message gate (never while a Request or another message is outstanding), the answer refreshes the
// caps and is answered with a Request like any Source_Capabilities (USB PD: the source starts tSenderResponse when it sends the
// list, solicited or not, and Hard Resets without one; an unchanged list gives the same RDO), a silent source ends the wait, and
// EPR mode never sends it.
#include <vector>

#include "k2/pd/pd_sink.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

SourceCaps make_caps(bool with_epr = false) {
    SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    const std::uint32_t raw[] = {0x0001912C, 0x0002D12C, 0x0004B12C};   // 5 V 3 A, 9 V 3 A, 15 V 3 A
    c.num_pdos = 3;
    for (int i = 0; i < 3; i++) c.pdos[i] = decode_pdo(raw[i]);
    if (with_epr) {
        c.epr = true;
        c.num_pdos = 5;
        c.pdos[3] = decode_pdo(0x0008C1F4);  // fixed 28 V 5 A
        c.pdos[4] = decode_pdo(0xD3C0968C);  // EPR AVS 15-48 V
    }
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
    void run(std::uint32_t ms) { for (std::uint32_t i = 0; i < ms; i += 10) { now += 10; sink.tick(now); pump(); } }
    void ctl(std::uint8_t type) { sink.on_message(Sop::Sop, make_header(type, 0, 2) | (1u << 8), {}, now); pump(); }
    void data(std::uint8_t type, std::uint32_t obj) {
        const std::uint8_t b[4] = {std::uint8_t(obj), std::uint8_t(obj >> 8), std::uint8_t(obj >> 16), std::uint8_t(obj >> 24)};
        sink.on_message(Sop::Sop, make_header(type, 1, 2) | (1u << 8), b, now);
        pump();
    }
    unsigned last_type() const { return sent.back().header & 0x1F; }
    // the RDO of the most recent Request we sent (the 4 data bytes, little endian)
    std::uint32_t last_rdo() const {
        for (std::size_t i = sent.size(); i-- > 0;) {
            if ((sent[i].header & 0x1F) != kDataRequest) continue;
            return std::uint32_t(sent[i].data[0]) | std::uint32_t(sent[i].data[1]) << 8 | std::uint32_t(sent[i].data[2]) << 16 |
                   std::uint32_t(sent[i].data[3]) << 24;
        }
        return 0;
    }
    void ready_at(std::uint8_t pos) {
        sink.trigger({pos, 0, 0}, now);
        pump();
        ctl(kCtlAccept);
        ctl(kCtlPsRdy);
    }
    // the source answers our Get_Source_Cap with its list (one PDO is enough: the engine decodes the real list into `caps`)
    void source_caps() { data(kDataSourceCap, 0x0001912C); }
    std::size_t count(unsigned type) const {
        std::size_t n = 0;
        for (const TxMsg& m : sent) n += (m.header & 0x1F) == type;
        return n;
    }
};

void test_a_ready_sink_asks_once_through_its_gate() {
    Rig r;
    r.ready_at(2);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    const std::size_t n0 = r.sent.size();
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Ok));
    r.pump();
    CHECK_EQ(r.sent.size(), n0 + 1);
    CHECK_EQ(r.last_type(), kCtlGetSourceCap);
    CHECK_EQ((r.sent.back().header >> 12) & 7, 0);      // a control message: no data objects
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    // one at a time: asked again before the answer, it is busy and nothing more goes out
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    r.pump();
    CHECK_EQ(r.sent.size(), n0 + 1);
}

void test_the_gate_refuses_while_anything_is_outstanding() {
    {   // nothing bound
        Sink s;
        CHECK_EQ(int(s.request_source_caps(0)), int(Err::NoCaps));
    }
    {   // no contract yet: a Request is outstanding (Requesting), the message must not overtake it
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);
        r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        const std::size_t n0 = r.sent.size();
        CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
        r.pump();
        CHECK_EQ(r.sent.size(), n0);
        r.ctl(kCtlAccept);                                // Transition: PS_RDY still to come
        CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    }
    {   // Idle (no contract at all): nothing to refresh through the sink, the caller cycles instead
        Rig r;
        CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    }
    {   // a message of ours is still queued (tx_pending_): the gate is occupied until the driver took it
        Rig r;
        r.ready_at(2);
        SourceCaps info;
        r.sink.bind_epr_info(&info);
        CHECK_EQ(int(r.sink.request_epr_info(r.now, true)), int(Err::Ok));   // queued, not pumped
        CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    }
}

void test_epr_mode_never_sends_it() {
    // EPR mode, Ready (list only wanted): an unsolicited Source_Capabilities there is a hard reset, so no Get_Source_Cap
    Rig r;
    r.caps = make_caps(true);
    CHECK_EQ(int(r.sink.trigger({0, 0, 0}, r.now)), int(Err::Ok));
    r.pump();
    r.data(kDataEprMode, std::uint32_t(kEprAck) << 24);
    r.data(kDataEprMode, std::uint32_t(kEprSucceeded) << 24);
    std::uint8_t chunk[28] = {};
    const std::uint16_t eh = 20 | (1u << 15);
    chunk[0] = eh & 0xFF; chunk[1] = eh >> 8;
    r.sink.on_message(Sop::Sop, make_header(kExtEprSourceCaps, 6, 2, true) | (1u << 8), chunk, r.now);
    r.pump();
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    CHECK_EQ(r.sink.status().epr_mode, true);
    const std::size_t n0 = r.sent.size();
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    r.pump();
    CHECK_EQ(r.sent.size(), n0);
}

void test_the_answer_is_answered_with_the_same_request() {
    // The source answers a Get_Source_Cap: its sender-response timer runs until a Request comes, so the sink Requests the position
    // it holds (same RDO: Accept + PS_RDY, no voltage change) and is back in Ready after the PS_RDY.
    Rig r;
    r.ready_at(2);
    const std::uint32_t rdo0 = r.last_rdo();
    CHECK_EQ(rdo0 != 0, true);
    r.sink.request_source_caps(r.now);
    r.pump();
    const std::size_t req0 = r.count(kDataRequest);
    r.source_caps();
    CHECK_EQ(r.count(kDataRequest), req0 + 1);
    CHECK_EQ(r.last_rdo(), rdo0);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
    // the gate stays shut while that Request is outstanding
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    r.ctl(kCtlAccept);
    r.ctl(kCtlPsRdy);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    CHECK_EQ(int(r.sink.status().pos), 2);
    // the source is quiet after that: no further Request, and the gate is free again
    r.run(200);
    CHECK_EQ(r.count(kDataRequest), req0 + 1);
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Ok));
}

void test_the_answer_without_a_held_choice_re_requests_the_position() {
    // A contract nobody triggered (here: the 5 V contract after a release) is answered too, at the position it stands on.
    Rig r;
    r.ready_at(2);
    r.sink.release(r.now);
    r.pump();
    r.ctl(kCtlAccept);
    r.ctl(kCtlPsRdy);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    CHECK_EQ(r.sink.status().held, false);
    CHECK_EQ(int(r.sink.status().pos), 1);
    const std::uint32_t rdo0 = r.last_rdo();
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Ok));
    r.pump();
    const std::size_t req0 = r.count(kDataRequest);
    r.source_caps();
    CHECK_EQ(r.count(kDataRequest), req0 + 1);
    CHECK_EQ(r.last_rdo(), rdo0);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
}

void test_an_unsolicited_announce_still_gets_its_request() {
    // The old behaviour is untouched: caps nobody asked for (a source's own re-announce) are re-Requested at the same position.
    Rig r;
    r.ready_at(2);
    const std::size_t req0 = r.count(kDataRequest);
    r.source_caps();
    CHECK_EQ(r.count(kDataRequest), req0 + 1);
    // and a Refresh answered once does not change how the next re-announce is treated
    r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
    r.sink.request_source_caps(r.now); r.pump();
    r.source_caps();
    r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
    const std::size_t req1 = r.count(kDataRequest);
    r.source_caps();
    CHECK_EQ(r.count(kDataRequest), req1 + 1);
}

void test_a_silent_source_ends_the_wait() {
    // No answer: the flag does not stay set (it would make a later unsolicited caps look like an answer, and a refresh
    // impossible); the sink may ask again after the window.
    Rig r;
    r.ready_at(2);
    r.sink.request_source_caps(r.now); r.pump();
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    r.run(kGetCapAnswerMs - 10);
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));
    r.run(40);
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Ok));
    // a re-announce after the window is unsolicited again
    r.pump();
    r.run(kGetCapAnswerMs + 20);
    const std::size_t req0 = r.count(kDataRequest);
    r.source_caps();
    CHECK_EQ(r.count(kDataRequest), req0 + 1);
}

void test_a_changed_list_is_renegotiated_with_a_different_request() {
    Rig r;
    r.ready_at(2);
    CHECK_EQ(r.sink.status().held, true);               // a trigger's contract is held
    const std::uint32_t rdo0 = r.last_rdo();
    r.sink.request_source_caps(r.now); r.pump();
    const std::size_t req0 = r.count(kDataRequest);
    // the source's list now offers PDO 2 with less current: the held choice plans a different RDO
    r.caps.pdos[1] = decode_pdo((180u << 10) | 100u);   // 9 V at 1 A: the held level, less current
    r.source_caps();
    CHECK_EQ(r.count(kDataRequest), req0 + 1);
    CHECK_EQ(r.last_rdo() != rdo0, true);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
}

void test_a_detach_forgets_the_wait() {
    Rig r;
    r.ready_at(2);
    r.sink.request_source_caps(r.now); r.pump();
    r.sink.detach();
    CHECK_EQ(int(r.sink.request_source_caps(r.now)), int(Err::Busy));   // Idle: nothing to refresh through the sink
}
}  // namespace

int main() {
    test_a_ready_sink_asks_once_through_its_gate();
    test_the_gate_refuses_while_anything_is_outstanding();
    test_epr_mode_never_sends_it();
    test_the_answer_is_answered_with_the_same_request();
    test_the_answer_without_a_held_choice_re_requests_the_position();
    test_an_unsolicited_announce_still_gets_its_request();
    test_a_silent_source_ends_the_wait();
    test_a_changed_list_is_renegotiated_with_a_different_request();
    test_a_detach_forgets_the_wait();
    return test_report("test_pd_sink_getcap");
}
