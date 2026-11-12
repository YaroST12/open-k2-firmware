// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <vector>

#include "k2/pd/pd_sink.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

// | 9 V 3 A | 15 V 3 A | 20 V 5 A | PPS 3.3-11 V 3 A | PPS 3.3-21 V 5 A
SourceCaps make_caps(bool with_epr = false) {
    SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    std::uint32_t raw[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000640C8,
                           0xC0DC2164, 0xC1A4213C};
    c.num_pdos = 6;
    for (int i = 0; i < 6; i++) c.pdos[i] = decode_pdo(raw[i]);
    if (with_epr) {  // 28 V fixed at 8, AVS 15-48 V at 9
        c.epr = true;
        c.num_pdos = 9;
        c.pdos[7] = decode_pdo(0x0008C1F4);  // fixed 28 V 5 A
        c.pdos[8] = decode_pdo(0xD3C0968C);  // EPR AVS 15-48 V, 140 W
    }
    return c;
}

struct Rig {
    SourceCaps caps = make_caps();
    Sink sink;
    std::uint32_t now = 1000;
    std::vector<TxMsg> sent;
    explicit Rig(std::uint32_t cap = 36000) {
        sink.bind(&caps);
        sink.set_cap_mv(cap);
        sink.set_proactive_discover(false);  // these tests cover the Request engine, not VDM discovery
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
    std::uint32_t last_rdo() const {
        const auto& m = sent.back();
        return m.data[0] | (m.data[1] << 8) | (m.data[2] << 16) | (std::uint32_t(m.data[3]) << 24);
    }
    unsigned last_type() const { return sent.back().header & 0x1F; }
    void accept_and_ready() { ctl(kCtlAccept); ctl(kCtlPsRdy); }
    // one complete (single chunk) EPR_Source_Capabilities message from the source
    void epr_caps_done() {
        std::uint8_t chunk[28] = {};
        const std::uint16_t eh = 20 | (1u << 15);
        chunk[0] = eh & 0xFF; chunk[1] = eh >> 8;
        sink.on_message(Sop::Sop, make_header(kExtEprSourceCaps, 6, 2, true) | (1u << 8), chunk, now);
        pump();
    }
};

}  // namespace

int main() {
    for (uint8_t wire : {1, 2, 3}) {
        Rig r;
        const std::uint8_t data[4] = {0x2C, 0x91, 0x01, 0};
        decode_source_caps(r.caps, make_header(1, 1, wire) | (1u << 8), data, 1);
        CHECK_EQ(r.caps.spec_rev, wire);
        CHECK_EQ(r.sink.trigger({1, 5000, 1000}, r.now), Err::Ok);
        r.pump();
        CHECK_EQ((r.sent.back().header >> 6) & 3, wire == 2 ? 2 : 1);
    }
    {
        SourceCaps c = make_caps();
        c.pdos[1] = decode_pdo(0xE0000000 | (50u << 10) | 200);
        for (unsigned mv : {9000u, 15000u, 15100u, 20000u}) {
            const auto p = plan_request(c, {2, std::uint16_t(mv), 1000}, 36000, false);
            CHECK_EQ(p.err, Err::Ok); CHECK_EQ(p.ma, mv <= 15000 ? 500 : 1000);
            CHECK_EQ(p.rdo, rdo_avs(2, mv, p.ma, false));
        }
    }
    // ---- builders ----
    CHECK_EQ(make_header(kDataRequest, 1, 2), 0x1082);
    CHECK_EQ(with_msg_id(0x1082, 5), 0x1A82);
    CHECK_EQ(with_msg_id(0x1A82, 0), 0x1082);
    // Fixed 9 V PDO 2, 3 A: pos 2 << 28, op 300 << 10, max 300, no USB suspend, USB comm capable (bits 24, 25)
    CHECK_EQ(rdo_fixed(2, 3000, 3000, false), (2u << 28) | (3u << 24) | (300u << 10) | 300u);
    CHECK_EQ(rdo_fixed(2, 3000, 3000, true), (2u << 28) | (3u << 24) | (1u << 22) | (300u << 10) | 300u);
    // PPS 9.1 V (455 * 20 mV) at 2 A (40 * 50 mA), position 5
    CHECK_EQ(rdo_pps(5, 9100, 2000, false), (5u << 28) | (3u << 24) | (455u << 9) | 40u);
    // AVS 21.7 V rounds down to 21.7 -> 217 * 4 units of 25 mV; 21.75 V rounds to 21.7 V
    CHECK_EQ(rdo_avs(9, 21700, 5000), (9u << 28) | (3u << 24) | (1u << 22) | ((217u * 4) << 9) | 100u);
    CHECK_EQ(rdo_avs(9, 21750, 5000), rdo_avs(9, 21700, 5000));
    CHECK_EQ(int(request_cap_mv(36000, 36000)), 36000);
    CHECK_EQ(int(request_cap_mv(48000, 85000)), 48000);
    CHECK_EQ(int(request_cap_mv(0, 36000)), 5000);   // unknown chip: 5 V
    CHECK_EQ(int(request_cap_mv(36000, 0)), 5000);

    // ---- a level is allowed when its nominal voltage is at or under the monitor ceiling (36 V runs on a 36 V monitor, 48 V does not) ----
    {
        SourceCaps c = make_caps();
        c.num_pdos = 10; c.epr = true;
        c.pdos[7] = decode_pdo((560u << 10) | 500);   // 28 V
        c.pdos[8] = decode_pdo((720u << 10) | 500);   // 36 V
        c.pdos[9] = decode_pdo((960u << 10) | 500);   // 48 V
        CHECK_EQ(int(plan_request(c, {8, 0, 0}, 36000, true).err), int(Err::Ok));
        CHECK_EQ(int(plan_request(c, {9, 0, 0}, 36000, true).err), int(Err::Ok));
        CHECK_EQ(int(plan_request(c, {10, 0, 0}, 36000, true).err), int(Err::OverCap));
        CHECK_EQ(int(plan_request(c, {9, 0, 0}, 48000, true).err), int(Err::Ok));
        CHECK_EQ(int(plan_request(c, {10, 0, 0}, 48000, true).err), int(Err::Ok));
        CHECK_EQ(int(plan_request(c, {4, 0, 0}, 20000, false).err), int(Err::Ok));   // 20 V on a 20 V cap: no margin up to 20 V
    }
    // ---- plan_request and the cap ----
    {
        const SourceCaps c = make_caps();
        Plan p = plan_request(c, {2, 0, 0}, 36000, false);
        CHECK_EQ(int(p.err), int(Err::Ok));
        CHECK_EQ(p.mv, 9000);
        CHECK_EQ(p.ma, 3000);
        CHECK_EQ(p.rdo, rdo_fixed(2, 3000, 3000, false));
        // wanted current above the PDO maximum is clamped to it: min(wanted, PDO max)
        Plan big = plan_request(c, {2, 0, 5000}, 36000, false);
        CHECK_EQ(big.ma, 3000);
        CHECK_EQ(big.rdo, p.rdo);
        CHECK_EQ(plan_request(c, {2, 0, 1000}, 36000, false).ma, 1000);
        CHECK_EQ(int(plan_request(c, {7, 0, 0}, 36000, false).err), int(Err::BadPos));
        // a 0 can never reach the RDO: the UI's 0 mV / 0 mA means "PDO maximum", and a PDO that
        // advertises no current is refused instead of requesting 0 mA
        for (std::uint8_t pos = 1; pos <= c.num_pdos; pos++) {
            const Plan q = plan_request(c, {pos, 0, 0}, 36000, false);
            if (q.err != Err::Ok) continue;
            CHECK_EQ(int(q.ma > 0), 1);
            CHECK_EQ(int(q.mv > 0), 1);
            if (q.type == PdoType::Fixed) {
                CHECK_EQ(int((q.rdo >> 10) & 0x3FF), q.ma / 10);   // operating
                CHECK_EQ(int(q.rdo & 0x3FF), q.ma / 10);           // max (both = current / 10)
            }
        }
        {
            SourceCaps z = c;
            z.pdos[1].max_ma = 0;
            CHECK_EQ(int(plan_request(z, {2, 0, 0}, 36000, false).err), int(Err::BadChoice));
        }
        CHECK_EQ(int(plan_request(SourceCaps{}, {1, 0, 0}, 36000, false).err), int(Err::NoCaps));
        // a fixed 20 V PDO over a 15 V cap is refused, not clamped
        CHECK_EQ(int(plan_request(c, {4, 0, 0}, 15000, false).err), int(Err::OverCap));
        CHECK_EQ(int(plan_request(c, {3, 0, 0}, 15000, false).err), int(Err::Ok));
        // a current above the PDO is clamped
        CHECK_EQ(plan_request(c, {1, 0, 9000}, 36000, false).ma, 3000);
        // PPS: default = PDO max below the cap; clamp to cap and to the PDO range
        p = plan_request(c, {6, 0, 0}, 36000, false);
        CHECK_EQ(p.mv, 21000);
        p = plan_request(c, {6, 0, 0}, 12000, false);
        CHECK_EQ(p.mv, 12000);
        p = plan_request(c, {6, 30000, 2000}, 36000, false);
        CHECK_EQ(p.mv, 21000);
        CHECK_EQ(p.ma, 2000);
        CHECK_EQ(int(plan_request(c, {6, 1000, 2000}, 36000, false).err), int(Err::Ok));
        CHECK_EQ(plan_request(c, {6, 1000, 2000}, 36000, false).mv, 3300);  // below the PDO minimum
        // a cap below 5 V counts as 5 V
        CHECK_EQ(plan_request(c, {6, 0, 0}, 3000, false).mv, 5000);
        // EPR position without EPR caps
        const SourceCaps e = make_caps(true);
        p = plan_request(e, {8, 0, 0}, 36000, true);
        CHECK_EQ(int(p.err), int(Err::Ok));
        CHECK_EQ(p.mv, 28000);
        CHECK_EQ(p.needs_epr ? 1 : 0, 1);
        CHECK_EQ(p.rdo, rdo_fixed(8, 5000, 5000, true));
        CHECK_EQ(int(plan_request(e, {8, 0, 0}, 20000, true).err), int(Err::OverCap));
        CHECK_EQ(int(plan_request(e, {9, 0, 0}, 12000, true).err), int(Err::OverCap));  // AVS minimum 15 V
        p = plan_request(e, {9, 0, 0}, 36000, true);       // AVS: voltage clamped to the monitor ceiling
        CHECK_EQ(p.mv, 36000);
        CHECK_EQ(p.ma, 3888);                                // 140 W / 36 V
        p = plan_request(e, {9, 21750, 5000}, 36000, true);
        CHECK_EQ(p.mv, 21700);                               // 100 mV steps
        CHECK_EQ(p.ma, 5000);
        CHECK_EQ(int(plan_request(e, {9, 10000, 1000}, 36000, true).err), int(Err::Ok));
        CHECK_EQ(plan_request(e, {9, 10000, 1000}, 36000, true).mv, 15000);  // below the AVS minimum
    }

    // ---- fixed request: Request -> Accept -> PS_RDY ----
    {
        Rig r;
        CHECK_EQ(int(r.sink.trigger({3, 0, 0}, r.now)), int(Err::Ok));
        r.pump();
        CHECK_EQ(r.sent.size(), 1u);
        CHECK_EQ(r.last_type(), kDataRequest);
        CHECK_EQ(r.last_rdo(), rdo_fixed(3, 3000, 3000, false));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.ctl(kCtlAccept);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Ok));
        CHECK_EQ(r.sink.status().pos, 3);
        CHECK_EQ(r.sink.status().mv, 15000);
        CHECK_EQ(r.sent.size(), 1u);       // a fixed contract needs no keep-alive
        r.run(20000);
        CHECK_EQ(r.sent.size(), 1u);
    }

    // ---- Reject, Wait, timeouts ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.ctl(kCtlReject);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));  // stays at the old contract (5 V)
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Rejected));
        CHECK_EQ(int(r.sink.status().held), 0);
    }
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.ctl(kCtlWait);
        CHECK_EQ(r.sent.size(), 1u);
        r.run(90);
        CHECK_EQ(r.sent.size(), 1u);          // retries only after tSinkRequest
        r.run(30);
        CHECK_EQ(r.sent.size(), 2u);
        r.ctl(kCtlWait); r.run(120);
        CHECK_EQ(r.sent.size(), 3u);
        r.ctl(kCtlWait); r.run(120);
        CHECK_EQ(r.sent.size(), 4u);
        r.ctl(kCtlWait); r.run(300);          // fourth Wait: give up, no more requests
        CHECK_EQ(r.sent.size(), 4u);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::WaitExhausted));
        CHECK_EQ(int(r.sink.status().held), 0);
    }
    {
        Rig r;  // no answer at all: hard reset after the Accept timeout
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.run(kAcceptMs - 20);
        CHECK_EQ(r.sent.size(), 1u);
        r.run(60);
        CHECK_EQ(r.sent.size(), 2u);
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Timeout));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Failed));
        CHECK_EQ(int(r.sink.status().hard_resets), 1);
    }
    {
        Rig r;  // a self-powered K2 never sends a Hard Reset on its own: its VBUS is the brick's (k2/selfpower-pd-contract)
        r.sink.set_never_hard_reset(true);
        r.sink.trigger({1, 0, 0}, r.now); r.pump();
        r.run(kAcceptMs + 200);                      // no Accept: the timeout ends the attempt, nothing goes on the wire
        CHECK_EQ(r.sent.size(), 1u);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Timeout));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Failed));
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
        CHECK_EQ(int(r.sink.status().held), 0);
        CHECK_EQ(int(r.sink.trigger({1, 0, 0}, r.now)), int(Err::Ok));   // a re-announce can ask again
        r.pump();
        r.ctl(kCtlAccept);
        r.run(kPsRdyMs + 200);                       // Accept but no PS_RDY: same
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
        for (const TxMsg& m : r.sent) CHECK_EQ(int(m.hard_reset), 0);
        r.sink.hard_reset(r.now); r.pump();          // the explicit tool still can
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
        CHECK_EQ(int(r.sink.status().hard_resets), 1);
    }
    {
        Rig r;  // a down-step gets the longer PS_RDY wait (no-load sources drain slowly)
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.release(r.now); r.pump();
        r.ctl(kCtlAccept);
        r.run(kPsRdyMs + 500);
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 0);   // still waiting after the normal PS_RDY window
        r.run(kPsRdyDownMs);
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
    }
    {
        Rig r;  // Accept but no PS_RDY
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.ctl(kCtlAccept);
        r.run(kPsRdyMs + 50);
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Timeout));
    }
    {
        Rig r;  // no GoodCRC: retried, then given up (no hard reset: nothing is listening)
        r.sink.trigger({3, 0, 0}, r.now);
        TxMsg m;
        int n = 0;
        while (n < 10) {
            if (!r.sink.take_tx(m)) { r.now += 100; r.sink.tick(r.now); if (!r.sink.take_tx(m)) break; }
            n++;
            r.sink.on_tx_result(false, r.now);
        }
        CHECK_EQ(n, 1 + kMaxTxRetries);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::TxFailed));
    }

    // ---- refusals ----
    {
        Rig r(15000);
        CHECK_EQ(int(r.sink.trigger({4, 0, 0}, r.now)), int(Err::OverCap));
        CHECK_EQ(r.sent.size(), 0u);
        r.pump();
        CHECK_EQ(r.sent.size(), 0u);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));
        Rig n;
        n.caps = SourceCaps{};
        CHECK_EQ(int(n.sink.trigger({1, 0, 0}, n.now)), int(Err::NoCaps));
        Rig b;
        b.sink.trigger({2, 0, 0}, b.now);           // Request in flight
        CHECK_EQ(int(b.sink.trigger({3, 0, 0}, b.now)), int(Err::Busy));
    }

    // ---- PPS: request, keep-alive every 5 s ----
    {
        Rig r;
        r.sink.trigger({6, 9100, 2000}, r.now); r.pump();
        CHECK_EQ(r.last_rdo(), rdo_pps(6, 9100, 2000, false));
        r.accept_and_ready();
        CHECK_EQ(r.sent.size(), 1u);
        r.run(4900);
        CHECK_EQ(r.sent.size(), 1u);
        r.run(200);
        CHECK_EQ(r.sent.size(), 2u);
        CHECK_EQ(r.last_rdo(), rdo_pps(6, 9100, 2000, false));
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        r.run(5100);
        CHECK_EQ(r.sent.size(), 3u);
        r.ctl(kCtlAccept); r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(r.sink.status().mv, 9100);
    }

    // ---- source capabilities re-sent while held: request again ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), 2u);
        CHECK_EQ(r.last_type(), kDataRequest);
    }

    // ---- caps re-announced while a Request is in flight (held, Requesting): re-request for the new
    // caps - the in-flight one was for the old caps and the source Hard Resets on unanswered caps
    // (CUKTECH C1 right after the Xiaomi auth re-announce, hardware 2026-11-04) ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump();   // Request out, no Accept yet
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        const std::size_t before = r.sent.size();
        CHECK_EQ(before, 1u);
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), before + 1);
        CHECK_EQ(r.last_type(), kDataRequest);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }

    // ---- caps re-announced while Ready but NOT held (the 5 V contract after a release, a rescan's re-announce):
    // the sink answers with a Request for the same position instead of staying silent (the C135 Hard Resets 28 ms
    // after caps nobody answered) ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.release(r.now); r.pump(); r.accept_and_ready();   // back at 5 V (PDO 1), held off
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().held), 0);
        const std::size_t before = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), before + 1);
        CHECK_EQ(r.last_type(), kDataRequest);
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);   // same position as the contract
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().pos), 1);
        CHECK_EQ(int(r.sink.status().held), 0);       // still not held: a plain 5 V contract
    }
    {
        Rig r;   // never contracted (Idle): the engine's start request handles it, the sink stays silent
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), 0u);
    }

    // ---- hard reset from the source drops the trigger ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::HardReset));
        CHECK_EQ(int(r.sink.status().held), 0);
        r.run(20000);
        CHECK_EQ(r.sent.size(), 1u);  // the sink itself sends nothing; the caller resumes on the next caps
    }
    // ---- a Hard Reset the SOURCE sends is counted apart from the ones the K2 sends (hardware 2026-11-07: the runner never saw it) ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::uint32_t sent = r.sink.status().hard_resets;
        CHECK_EQ(r.sink.status().source_hard_resets, std::uint32_t(0));
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(r.sink.status().source_hard_resets, std::uint32_t(1));
        CHECK_EQ(r.sink.status().hard_resets, sent);                     // "Hard resets: N" and the K2-must-not-send checks stay as they were
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(r.sink.status().source_hard_resets, std::uint32_t(2));
        r.sink.detach();
        CHECK_EQ(r.sink.status().source_hard_resets, std::uint32_t(2));   // outlives a detach, like hard_resets
        r.sink.hard_reset(r.now);
        CHECK_EQ(r.sink.status().hard_resets, sent + 1);
        CHECK_EQ(r.sink.status().source_hard_resets, std::uint32_t(2));   // a K2-sent reset does not count as the source's
    }
    // ---- after a hard reset the next Source_Capabilities flag a resume; asking for PDO 1 restores 5 V ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_hard_reset_rx(r.now);
        r.run(1000);
        CHECK_EQ(r.sink.take_resume(), true);
        CHECK_EQ(r.sink.take_resume(), false);
        r.sink.trigger({1, 0, 0}, r.now); r.pump();
        CHECK_EQ(r.last_rdo(), rdo_fixed(1, 3000, 3000, false));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().pos), 1);
    }
    // ---- a K2 that never asked for anything has nothing to resume (a passive K2 stays silent after a Hard Reset) ----
    {
        Rig r;
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(r.sink.take_resume(), false);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::HardReset));
    }
    // ---- two Hard Resets right behind each other (the second finds the sink idle) keep the resume of the first ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_hard_reset_rx(r.now);
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(r.sink.take_resume(), true);
    }
    // ---- a release after the Hard Reset ends the pending resume ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_hard_reset_rx(r.now);
        r.sink.release(r.now);
        CHECK_EQ(r.sink.take_resume(), false);
    }
    // ---- a detach clears the resume flag: nothing to resume on a bare port ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_hard_reset_rx(r.now);
        r.sink.detach();
        CHECK_EQ(r.sink.take_resume(), false);
    }
    // ---- the wire revision survives a hard reset (a PD2.0 source falls back only when its caps say so) ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(int(r.sink.rev()), 2);
    }

    // ---- release: back to 5 V, hard reset when that fails ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.release(r.now); r.pump();
        CHECK_EQ(r.last_rdo(), rdo_fixed(1, 3000, 3000, false));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(r.sink.status().mv, 5000);
        CHECK_EQ(int(r.sink.status().held), 0);
    }
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.release(r.now); r.pump();
        r.ctl(kCtlReject);
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
    }
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.release(r.now); r.pump();
        r.run(kAcceptMs + 50);
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
    }
    {
        Rig r;
        r.sink.hard_reset(r.now); r.pump();
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
        Rig e;
        e.sink.release(e.now); e.pump();   // idle at 5 V: nothing to do
        CHECK_EQ(e.sent.size(), 0u);
    }

    // ---- release while the Request for 5 V (PDO 1) is already in flight: one Request, no second one racing the Accept ----
    // (hardware 2026-11-04, UGREEN X865: the run start's PDO 1 Request was TXSENT, a refusal released the sink and a second
    // Request left 12 ms after the caps; the source's Accept answered the first, the TX of the second timed out)
    {
        Rig r;
        r.sink.trigger({1, 0, 0}, r.now); r.pump();
        const auto n = r.sent.size();
        r.sink.release(r.now); r.pump();
        CHECK_EQ(r.sent.size(), n);                                          // nothing more on the wire
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));    // still waiting for the Accept
        CHECK_EQ(int(r.sink.status().held), 0);                              // but no longer a held trigger
        r.ctl(kCtlAccept);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Ok));
        CHECK_EQ(int(r.sink.status().pos), 1); CHECK_EQ(int(r.sink.status().held), 0);
        CHECK_EQ(r.sent.size(), n);
    }
    {
        Rig r;   // the same once the Accept is in (Transition)
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.ctl(kCtlAccept);
        const auto n = r.sent.size();
        r.sink.release(r.now); r.pump();
        CHECK_EQ(r.sent.size(), n);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition)); CHECK_EQ(int(r.sink.status().held), 0);
        r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready)); CHECK_EQ(int(r.sink.status().pos), 1);
        CHECK_EQ(r.sent.size(), n);
    }
    {
        Rig r;   // a Request for anything above 5 V in flight is pulled back with PDO 1 - once it has settled, never beside it
        r.sink.trigger({2, 0, 0}, r.now); r.pump();
        const auto n = r.sent.size();
        r.sink.release(r.now); r.pump();
        CHECK_EQ(r.sent.size(), n);                                          // the Request in flight is the only one on the wire
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.ctl(kCtlAccept);
        CHECK_EQ(r.sent.size(), n);
        r.ctl(kCtlPsRdy);                                                    // 9 V is up: now the way back goes out
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(r.last_rdo(), rdo_fixed(1, 3000, 3000, false));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().pos), 1); CHECK_EQ(int(r.sink.status().held), 0);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Ok));
    }

    // ---- pattern budget-reannounce-mid-run (hardware 2026-11-09, Anker 2-way splitter, runs/anker-split/bb9.txt): the source
    // re-announces a shorter list while the K2 holds 15 V; the sink answers it with a Request for the held position and the
    // runner's release arrives while that Request is in flight. Exactly one Request is ever outstanding: the release waits
    // for the PS_RDY, then goes out, and VBUS ends at 5 V. ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();    // 15 V
        CHECK_EQ(int(r.sink.status().pos), 3);
        r.caps = SourceCaps{};                                               // 5 V 1.5 A, 9 V 3 A, 15 V 3 A (20 V gone)
        r.caps.valid = true; r.caps.spec_rev = 2; r.caps.num_pdos = 3;
        r.caps.pdos[0] = decode_pdo(0x0001905A); r.caps.pdos[1] = decode_pdo(0x0002D12C); r.caps.pdos[2] = decode_pdo(0x0004B12C);
        const std::size_t n = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x5A, 0x90, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 3, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);                                      // the sink's answer: PDO 3 again
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 3);
        r.ctl(kCtlAccept);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        r.sink.release(r.now); r.pump();                                     // the runner's release, 1.7 ms after the Accept
        CHECK_EQ(r.sent.size(), n + 1);                                      // not beside the first Request
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        r.sink.release(r.now); r.pump();                                     // the runner asks every 250 ms: still nothing
        CHECK_EQ(r.sent.size(), n + 1);
        r.ctl(kCtlPsRdy);                                                    // PS_RDY of the first Request (VBUS stays 15 V)
        CHECK_EQ(r.sent.size(), n + 2);                                      // now the release goes out
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        r.accept_and_ready();
        CHECK_EQ(r.sent.size(), n + 2);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().pos), 1); CHECK_EQ(int(r.sink.status().mv), 5000);
        CHECK_EQ(int(r.sink.status().held), 0);
        for (const TxMsg& m : r.sent) CHECK_EQ(int(m.hard_reset), 0);
    }
    {   // the same with the release landing before the Accept (Requesting)
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        const std::size_t n = r.sent.size();
        r.sink.release(r.now); r.pump();
        CHECK_EQ(r.sent.size(), n);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.accept_and_ready();
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().pos), 1); CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }
    {   // caps re-announced while a release waits behind a Request: the answer is PDO 1 directly, one Request for the new list
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump();                          // Request for 15 V in flight
        r.sink.release(r.now); r.pump();                                     // the release waits
        const std::size_t n = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);                          // not PDO 3 first
        r.accept_and_ready();
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().pos), 1); CHECK_EQ(int(r.sink.status().held), 0);
    }
    {   // caps re-announced while the 5 V Request itself is out (Aborting): the answer replaces it with a Request for the new list
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.release(r.now); r.pump();                                     // abort Request out
        const std::size_t n = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready)); CHECK_EQ(int(r.sink.status().pos), 1);
    }
    {   // the Request the release waited behind is Rejected: the old contract stands (15 V), the release goes out then
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        const std::size_t n = r.sent.size();
        r.sink.release(r.now); r.pump();
        CHECK_EQ(r.sent.size(), n);
        r.ctl(kCtlReject);
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().pos), 1);
    }
    {   // a trigger is Busy while a release waits; once the release has landed, a new trigger starts clean
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.sink.release(r.now); r.pump();
        const std::size_t n = r.sent.size();
        CHECK_EQ(int(r.sink.trigger({2, 0, 0}, r.now)), int(Err::Busy));
        r.pump();
        CHECK_EQ(r.sent.size(), n);
        r.accept_and_ready(); r.accept_and_ready();                          // 15 V up, then 5 V
        CHECK_EQ(int(r.sink.status().pos), 1);
        CHECK_EQ(int(r.sink.trigger({2, 0, 0}, r.now)), int(Err::Ok));
        r.pump();
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().pos), 2);                               // and no stale release drags it back
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().held), 1);
    }

    // ---- the 5 V release is verified: after its PS_RDY VBUS must be at vSafe5V, else retry, then Hard Reset ----
    {
        auto feed = [](Rig& r, int mv, std::uint32_t ms) {
            for (std::uint32_t i = 0; i < ms; i += 10) { r.now += 10; r.sink.on_vbus(mv, r.now); r.sink.tick(r.now); r.pump(); }
        };
        auto requests = [](const Rig& r) {
            std::size_t n = 0;
            for (const TxMsg& m : r.sent) if (!m.hard_reset && (m.header & 0x1F) == kDataRequest) ++n;
            return n;
        };
        {   // VBUS stays at 15 V: the release Request is repeated twice, then the sink sends a Hard Reset
            Rig r;
            r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
            feed(r, 14850, 100);
            r.sink.release(r.now); r.pump(); r.accept_and_ready();
            CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
            CHECK_EQ(int(r.sink.status().verifying), 1);
            const std::size_t first = requests(r);
            feed(r, 14850, kSettleMs - 100);
            CHECK_EQ(requests(r), first);                                    // still inside the window
            feed(r, 14850, 200);
            CHECK_EQ(requests(r), first + 1);                                // retry 1: PDO 1 again
            CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);
            CHECK_EQ(int(r.sink.status().verifying), 1);
            r.accept_and_ready();
            feed(r, 14850, kSettleMs + 200);
            CHECK_EQ(requests(r), first + 2);                                // retry 2
            r.accept_and_ready();
            CHECK_EQ(r.sink.status().hard_resets, 0u);
            feed(r, 14850, kSettleMs + 200);
            CHECK_EQ(requests(r), first + 2);                                // bounded: no third retry
            CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);                   // the Hard Reset
            CHECK_EQ(r.sink.status().hard_resets, 1u);
            CHECK_EQ(int(r.sink.status().verifying), 0);
            CHECK_EQ(int(r.sink.status().held), 0);
        }
        {   // VBUS comes down to 5 V in time: verified, no retry
            Rig r;
            r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
            feed(r, 14850, 100);
            r.sink.release(r.now); r.pump(); r.accept_and_ready();
            const std::size_t first = requests(r);
            feed(r, 5050, 100);
            CHECK_EQ(int(r.sink.status().verifying), 0);
            feed(r, 5050, kSettleMs + kDescentMs);
            CHECK_EQ(requests(r), first);
            CHECK_EQ(r.sink.status().hard_resets, 0u);
        }
        {   // a no-load source still draining (falling >= 50 mV per 600 ms) gets the descent window, not a retry
            Rig r;
            r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
            feed(r, 14850, 100);
            r.sink.release(r.now); r.pump(); r.accept_and_ready();
            const std::size_t first = requests(r);
            int mv = 14000;
            for (int i = 0; i < 20; ++i) { feed(r, mv, 200); mv -= 100; }    // 4 s: ~0.5 V/s fall
            CHECK_EQ(requests(r), first);
            CHECK_EQ(r.sink.status().hard_resets, 0u);
            feed(r, 5100, 100);
            CHECK_EQ(int(r.sink.status().verifying), 0);
        }
        {   // no VBUS samples at all (monitor offline): nothing to verify against, the release is taken as done
            Rig r;
            r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
            r.sink.release(r.now); r.pump(); r.accept_and_ready();
            CHECK_EQ(int(r.sink.status().verifying), 0);
            r.run(kSettleMs * 3);
            CHECK_EQ(r.sink.status().hard_resets, 0u);
        }
        {   // a self-powered K2 retries the same way but never puts the Hard Reset on the wire
            Rig r;
            r.sink.set_never_hard_reset(true);
            r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
            feed(r, 14850, 100);
            r.sink.release(r.now); r.pump(); r.accept_and_ready();
            const std::size_t first = requests(r);
            feed(r, 14850, kSettleMs + 200); r.accept_and_ready();
            feed(r, 14850, kSettleMs + 200); r.accept_and_ready();
            feed(r, 14850, kSettleMs + 200);
            CHECK_EQ(requests(r), first + 2);
            for (const TxMsg& m : r.sent) CHECK_EQ(int(m.hard_reset), 0);
            CHECK_EQ(r.sink.status().hard_resets, 0u);
            CHECK_EQ(int(r.sink.status().state), int(SinkState::Failed));
            CHECK_EQ(int(r.sink.status().verifying), 0);
        }
        {   // a retry that is not answered ends in the abort timeout, as before (no extra machinery)
            Rig r;
            r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
            feed(r, 14850, 100);
            r.sink.release(r.now); r.pump(); r.accept_and_ready();
            feed(r, 14850, kSettleMs + 200);                                 // retry out, nobody answers
            feed(r, 14850, kAcceptMs + 100);
            CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
        }
    }

    // ---- VBUS guard ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.on_vbus(15600, r.now); r.pump();      // inside target + 10 %
        CHECK_EQ(r.sent.size(), 1u);
        r.sink.on_vbus(16600, r.now); r.pump();      // above target + 10 %: drop to 5 V
        CHECK_EQ(r.sent.size(), 2u);
        CHECK_EQ(r.last_rdo(), rdo_fixed(1, 3000, 3000, false));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Aborted));
        CHECK_EQ(int(r.sink.status().held), 0);
    }
    {
        Rig r;  // during the transition only the cap counts: a legal slew is not an abort
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.ctl(kCtlAccept);
        r.sink.on_vbus(5000, r.now); r.sink.on_vbus(9000, r.now); r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        r.sink.on_vbus(37900, r.now); r.pump();      // over the 36 V cap + 5 % (37.8 V)
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
    }
    // The overvoltage abort is the monitor ceiling + 5 % (overcap_abort_mv): a 36 V EPR source that sits at 36.3 V is in tolerance.
    for (const std::uint32_t cap : {36000u, 48000u}) {
        const std::int32_t limit = std::int32_t(overcap_abort_mv(cap));
        CHECK_EQ(limit, cap == 36000u ? 37800 : 50400);
        Rig r(cap);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.ctl(kCtlAccept);
        r.sink.on_vbus(std::int32_t(cap) + 300, r.now); r.pump();            // 300 mV over the nominal ceiling: tolerance
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        r.sink.on_vbus(limit, r.now); r.pump();                              // exactly at the abort line: still no abort
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        r.sink.on_vbus(limit + 100, r.now); r.pump();                        // 100 mV past it
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
    }
    {
        Rig r;  // right after PS_RDY from a higher voltage the check waits for the settle time
        r.sink.trigger({4, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        CHECK_EQ(int(r.sink.trigger({3, 0, 0}, r.now)), int(Err::Ok));  // a held trigger can be re-aimed
        r.pump(); r.accept_and_ready();
        r.sink.on_vbus(19000, r.now); r.pump();      // still slewing down from 20 V
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(r.sent.size(), 2u);
        r.run(kSettleMs + 100);
        r.sink.on_vbus(19000, r.now); r.pump();      // settled: 19 V for a 15 V target is an abort
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
    }
    {
        Rig r;  // large PPS-style step-down: VBUS still falls after the settle time, guard waits
        r.sink.trigger({4, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        CHECK_EQ(int(r.sink.trigger({3, 0, 0}, r.now)), int(Err::Ok));
        r.pump(); r.accept_and_ready();
        for (int mv = 19800; mv > 15400; mv -= 200) {  // 200 mV per 100 ms, above 16.5 V for ~1.5 s
            r.sink.on_vbus(mv, r.now); r.pump(); r.run(100);
        }
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(r.sent.size(), 2u);
    }
    {
        Rig r;  // falling that stalls above the limit still aborts
        r.sink.trigger({4, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        for (int i = 0; i < 40; i++) {
            r.sink.on_vbus(i < 5 ? 19800 - 200 * i : 18800, r.now); r.pump(); r.run(100);
        }
        CHECK_EQ(int(r.sink.status().state) != int(SinkState::Ready) ? 1 : 0, 1);  // aborted (may be past it)
    }
    {
        // Lenovo C135 (hardware 2026-11-03), pattern no-psrdy-on-downstep: PPS 3.3 V from a 5 V contract is Accepted,
        // no PS_RDY ever follows, VBUS drains ~2 V/s. When VBUS has reached the target the output did make the
        // step: the contract is taken as ready (no Hard Reset, which cost this charger its output for good).
        Rig r;
        r.caps.pdos[5] = decode_pdo(0xC0000000 | (110u << 17) | (33u << 8) | 60u);   // PPS 3.3-11 V 3 A
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        CHECK_EQ(int(r.sink.trigger({6, 3300, 3000}, r.now)), int(Err::Ok));
        r.pump(); r.ctl(kCtlAccept);
        const std::size_t sent = r.sent.size();
        for (int mv = 5000; mv > 4000; mv -= 200) { r.sink.on_vbus(mv, r.now); r.pump(); r.run(100); }
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));   // still falling toward the target
        r.sink.on_vbus(3800, r.now); r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));   // one sample in the band is no step (review item B)
        for (std::uint32_t t = 0; t < kDownSettleMs + 100; t += 50) { r.sink.on_vbus(3800, r.now); r.pump(); r.run(50); }
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Ok));
        CHECK_EQ(int(r.sink.status().pos), 6);
        CHECK_EQ(int(r.sink.status().mv), 3300);
        CHECK_EQ(int(r.sink.status().held), 1);
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
        CHECK_EQ(r.sent.size(), sent);                                      // nothing sent, in particular no Hard Reset
        r.run(kPsRdyDownMs + 500);                                          // the old deadline passes quietly
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
        r.ctl(kCtlPsRdy);                                                   // a late PS_RDY changes nothing
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Ok));
    }
    {
        // Item G: the status stamps the source's Accept and PS_RDY (the sink's `now`), a new Accept clears the PS_RDY stamp, and a
        // contract taken as ready without a PS_RDY says so (ps_rdy_inferred) and keeps ps_rdy_at 0.
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump();
        r.run(100); r.ctl(kCtlAccept);
        const std::uint32_t accepted = r.now;
        CHECK_EQ(int(r.sink.status().accept_at == accepted), 1);
        CHECK_EQ(int(r.sink.status().ps_rdy_at), 0);
        r.run(600); r.ctl(kCtlPsRdy);
        CHECK_EQ(int(r.sink.status().ps_rdy_at == r.now), 1);
        CHECK_EQ(int(r.sink.status().ps_rdy_at - r.sink.status().accept_at), 600);
        CHECK_EQ(int(r.sink.status().ps_rdy_inferred), 0);
        r.run(kSettleMs + 100);
        r.caps.pdos[5] = decode_pdo(0xC0000000 | (110u << 17) | (33u << 8) | 60u);   // PPS 3.3-11 V 3 A
        CHECK_EQ(int(r.sink.trigger({6, 3300, 3000}, r.now)), int(Err::Ok));
        r.pump(); r.ctl(kCtlAccept);
        CHECK_EQ(int(r.sink.status().ps_rdy_at), 0);                              // cleared by the new Accept
        CHECK_EQ(int(r.sink.status().accept_at == r.now), 1);
        for (std::uint32_t t = 0; t < 1200; t += 100) { r.sink.on_vbus(3500, r.now); r.pump(); r.run(100); }
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));              // inferred from the settled VBUS
        CHECK_EQ(int(r.sink.status().ps_rdy_inferred), 1);
        CHECK_EQ(int(r.sink.status().ps_rdy_at), 0);
    }
    {
        // Item B: the shortcut needs VBUS steady inside the band [target - margin, ceiling] for kDownSettleMs. A single sample, a
        // sample below the floor (a glitch, an output that went dark), a fall through the band and a bounce out of it all keep the
        // sink waiting for PS_RDY.
        Rig r;
        r.caps.pdos[5] = decode_pdo(0xC0000000 | (110u << 17) | (33u << 8) | 60u);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        CHECK_EQ(int(r.sink.trigger({6, 3300, 3000}, r.now)), int(Err::Ok));
        r.pump(); r.ctl(kCtlAccept);
        const std::size_t sent = r.sent.size();
        for (int mv : {2000, 0, 1000}) { r.sink.on_vbus(mv, r.now); r.pump(); r.run(100); }   // below the floor: no step
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        for (int mv : {3800, 3600, 3400, 3200, 3000}) { r.sink.on_vbus(mv, r.now); r.pump(); r.run(100); }   // still falling through the band
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        for (std::uint32_t t = 0; t < kDownSettleMs - 100; t += 50) { r.sink.on_vbus(3500, r.now); r.pump(); r.run(50); }
        r.sink.on_vbus(5000, r.now); r.pump(); r.run(50);                                       // out of the band: the dwell restarts
        for (std::uint32_t t = 0; t < kDownSettleMs - 100; t += 50) { r.sink.on_vbus(3500, r.now); r.pump(); r.run(50); }
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        CHECK_EQ(r.sent.size(), sent);
        for (std::uint32_t t = 0; t < 300; t += 50) { r.sink.on_vbus(3500, r.now); r.pump(); r.run(50); }   // steady long enough
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Ok));
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
    }
    {
        // The same step with VBUS stuck above the target never gets the benefit: the 4 s window ends in a Hard Reset.
        Rig r;
        r.caps.pdos[5] = decode_pdo(0xC0000000 | (110u << 17) | (33u << 8) | 60u);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({6, 3300, 3000}, r.now); r.pump(); r.ctl(kCtlAccept);
        for (std::uint32_t t = 0; t < kPsRdyDownMs + 100; t += 100) { r.sink.on_vbus(4500, r.now); r.pump(); r.run(100); }
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Timeout));
    }
    {
        // A fixed-voltage step down is not covered (the pattern is about programmable sources): still PS_RDY or reset.
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();   // 20 V
        r.run(kSettleMs + 100);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.ctl(kCtlAccept);      // 9 V fixed: a down-step
        r.sink.on_vbus(9500, r.now); r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
    }
    {
        // Baseus 35 W, PPS 3.3 V from 5.12 V: VBUS falls 0.57 V/s, then sits at 3.73 V (unloaded source, no pull-down).
        // That is the best the source can do, not a fault: the guard must hold the contract.
        Rig r;
        r.caps.pdos[5] = decode_pdo(0xC0000000 | (110u << 17) | (33u << 8) | 60u);   // PPS 3.3-11 V 3 A
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        CHECK_EQ(int(r.sink.trigger({6, 3300, 3000}, r.now)), int(Err::Ok));
        r.pump(); r.accept_and_ready();
        const std::size_t sent = r.sent.size();
        for (int i = 0; i < 100; i++) {   // 180 ms per sample, 20 s in all
            const int mv = 5120 - 102 * i;
            r.sink.on_vbus(mv > 3730 ? mv : 3730, r.now); r.pump(); r.run(180);
            if (r.sink.status().state == SinkState::Transition || r.sink.status().state == SinkState::Requesting) r.accept_and_ready();   // keep-alive
        }
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().held), 1);
        CHECK_EQ(r.sent.size() - sent <= 4 ? 1 : 0, 1);   // keep-alive re-requests only, no abort to 5 V
    }
    {
        // The same step that stalls well above the floor (4.5 V for a 3.3 V target) is still a fault.
        Rig r;
        r.caps.pdos[5] = decode_pdo(0xC0000000 | (110u << 17) | (33u << 8) | 60u);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({6, 3300, 3000}, r.now); r.pump(); r.accept_and_ready();
        for (int i = 0; i < 40; i++) {
            const int mv = 5120 - 102 * i;
            r.sink.on_vbus(mv > 4500 ? mv : 4500, r.now); r.pump(); r.run(180);
        }
        CHECK_EQ(int(r.sink.status().held), 0);
    }
    {
        Rig r;  // a rising high voltage aborts at once after the settle time
        r.sink.trigger({4, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_vbus(19000, r.now); r.run(kSettleMs + 100);
        r.sink.on_vbus(18000, r.now); r.run(100);
        r.sink.on_vbus(18500, r.now); r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
    }
    {
        Rig r;  // a slow fall that outlasts the bounded window aborts
        r.sink.trigger({4, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        for (int mv = 19900; mv > 16700; mv -= 60) { r.sink.on_vbus(mv, r.now); r.pump(); r.run(100); }
        CHECK_EQ(int(r.sink.status().state) != int(SinkState::Ready) ? 1 : 0, 1);  // aborted (may be past it)
    }
    {
        // Item F: a big no-load PPS down-step (20 V contract to PPS 5 V) drains at ~2 V/s and needs ~7 s to reach the 5.6 V
        // ceiling. The descent window scales with the step, so the guard must not abort this honest fall.
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();   // 20 V
        r.run(kSettleMs + 100);
        CHECK_EQ(int(r.sink.trigger({6, 5000, 3000}, r.now)), int(Err::Ok));
        r.pump(); r.accept_and_ready();
        const std::size_t sent = r.sent.size();
        for (int i = 0; i < 100; i++) {   // 200 mV per 100 ms from 19.8 V, bottoming out at the target
            const int mv = 19800 - 200 * i;
            r.sink.on_vbus(mv > 5000 ? mv : 5000, r.now); r.pump(); r.run(100);
            if (r.sink.status().state == SinkState::Transition || r.sink.status().state == SinkState::Requesting) r.accept_and_ready();   // keep-alive
        }
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().held), 1);
        CHECK_EQ(r.sent.size() - sent <= 2 ? 1 : 0, 1);   // keep-alive re-requests only, no abort to 5 V
    }
    {
        // The window is bounded: the same step falling at 0.4 V/s (36 s to the ceiling) still aborts, and so does a stall.
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({6, 5000, 3000}, r.now); r.pump(); r.accept_and_ready();
        for (int i = 0; i < 400; i++) { r.sink.on_vbus(19800 - 40 * i, r.now); r.pump(); r.run(100); }
        CHECK_EQ(int(r.sink.status().held), 0);
    }
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({6, 5000, 3000}, r.now); r.pump(); r.accept_and_ready();
        for (int i = 0; i < 100; i++) { r.sink.on_vbus(i < 10 ? 19800 - 200 * i : 17800, r.now); r.pump(); r.run(100); }
        CHECK_EQ(int(r.sink.status().held), 0);   // fell for a second, then stalled at 17.8 V
    }
    {
        Rig r;  // abort that never settles above the cap: hard reset
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.on_vbus(40000, r.now); r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        r.sink.on_vbus(40000, r.now + 600); r.sink.tick(r.now + 600); r.pump();
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
    }

    // ---- EPR: enter, chunked caps, AVS request, keep-alive ----
    {
        Rig r;
        r.caps = make_caps();               // SPR list only
        CHECK_EQ(int(r.sink.trigger({0, 0, 0}, r.now)), int(Err::Ok));   // enter EPR, read the list
        r.pump();
        CHECK_EQ(r.last_type(), kDataEprMode);
        CHECK_EQ(r.last_rdo() >> 24, kEprEnter);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::EprEntering));
        r.data(kDataEprMode, std::uint32_t(kEprAck) << 24);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::EprEntering));
        r.data(kDataEprMode, std::uint32_t(kEprSucceeded) << 24);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::EprWaitCaps));
        CHECK_EQ(r.sink.status().epr_mode ? 1 : 0, 1);
        // chunk 0 of a 40 byte list arrives: the sink asks for chunk 1
        std::uint8_t chunk[28] = {};
        const std::uint16_t eh = 40 | (1u << 15);  // data size 40, chunked, chunk 0
        chunk[0] = eh & 0xFF; chunk[1] = eh >> 8;
        r.sink.on_message(Sop::Sop, make_header(kExtEprSourceCaps, 7, 2, true) | (1u << 8), chunk, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), 2u);
        const TxMsg& q = r.sent.back();
        CHECK_EQ(q.header & 0x1F, kExtEprSourceCaps);
        CHECK_EQ((q.header >> 15) & 1, 1);
        // 4 B = ext header padded to the 4-byte boundary per USB PD R3.2 §6.5.1.1 (Data Size < 26, chunked)
        CHECK_EQ(q.len, 4);
        const unsigned qeh = q.data[0] | (q.data[1] << 8);
        CHECK_EQ((qeh >> 15) & 1, 1);       // chunked
        CHECK_EQ((qeh >> 10) & 1, 1);       // request
        CHECK_EQ((qeh >> 11) & 0xF, 1);     // chunk number 1
        CHECK_EQ(qeh & 0x1FF, 0);           // data size 0
        // last chunk (chunk 1 of 40 bytes, 14 left): no further request
        const std::uint16_t eh1 = 40 | (1u << 11) | (1u << 15);
        chunk[0] = eh1 & 0xFF; chunk[1] = eh1 >> 8;
        r.sink.on_message(Sop::Sop, make_header(kExtEprSourceCaps, 5, 2, true) | (1u << 8), chunk, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), 2u);
    }
    {
        Rig r;
        r.caps = make_caps(true);           // pretend the engine decoded the whole EPR list
        // AVS 9 at 21.7 V (EPR PDO), the source is already in EPR mode
        r.sink.trigger({9, 21700, 5000}, r.now); r.pump();
        CHECK_EQ(r.last_type(), kDataEprMode);   // not yet in EPR mode: enter first
        r.data(kDataEprMode, std::uint32_t(kEprAck) << 24);
        r.data(kDataEprMode, std::uint32_t(kEprSucceeded) << 24);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::EprWaitCaps));
        r.epr_caps_done();                        // the engine decoded the full list
        CHECK_EQ(r.last_type(), kDataEprRequest);
        CHECK_EQ(r.sent.back().len, 8);          // RDO + copy of the PDO
        const auto& d = r.sent.back().data;
        const std::uint32_t pdo_copy = d[4] | (d[5] << 8) | (d[6] << 16) | (std::uint32_t(d[7]) << 24);
        CHECK_EQ(pdo_copy, r.caps.pdos[8].raw);
        CHECK_EQ(r.last_rdo(), rdo_avs(9, 21700, 5000));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(r.sink.status().mv, 21700);
        // EPR keep-alive every 400 ms (Extended_Control type 3)
        const std::size_t n0 = r.sent.size();
        r.run(1000);
        CHECK_EQ(r.sent.size() >= n0 + 2 ? 1 : 0, 1);
        const TxMsg* ka = nullptr;
        for (const TxMsg& m : r.sent) if ((m.header & 0x1F) == kExtControl && ((m.header >> 15) & 1)) ka = &m;
        CHECK_EQ(ka != nullptr ? 1 : 0, 1);
        if (ka) {
            CHECK_EQ(ka->data[2], kEprKeepAlive); CHECK_EQ(ka->len, 4);
            // Extended header 0x8002: Chunked set, size 2. A source without "unchunked extended messages"
            // (the Baseus 140W) answers an unchunked one with Not_Supported and Hard Resets the EPR contract.
            CHECK_EQ(ka->data[0], 2); CHECK_EQ(ka->data[1], 0x80);
        }
        // AVS also needs the 5 s re-request
        r.run(5000);
        int requests = 0;
        for (const TxMsg& m : r.sent) if ((m.header & 0x1F) == kDataEprRequest) requests++;
        CHECK_EQ(requests >= 2 ? 1 : 0, 1);
    }
    {
        Rig r;
        r.caps = make_caps(true);
        r.sink.trigger({9, 21700, 5000}, r.now); r.pump();
        r.data(kDataEprMode, std::uint32_t(kEprFailed) << 24);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::EprFailed));
        CHECK_EQ(int(r.sink.status().held), 0);
        Rig t;
        t.caps = make_caps(true);
        t.sink.trigger({9, 21700, 5000}, t.now); t.pump();
        t.run(kEprEnterMs + 100);            // the source never answers
        CHECK_EQ(int(t.sink.status().result), int(SinkResult::Timeout));
    }
    {
        Rig r;  // EPR release uses EPR_Request for PDO 1
        r.caps = make_caps(true);
        r.sink.trigger({9, 21700, 5000}, r.now); r.pump();
        r.data(kDataEprMode, std::uint32_t(kEprAck) << 24);
        r.data(kDataEprMode, std::uint32_t(kEprSucceeded) << 24);
        r.epr_caps_done(); r.accept_and_ready();
        r.sink.release(r.now); r.pump();
        CHECK_EQ(r.last_type(), kDataEprRequest);
        CHECK_EQ(r.last_rdo() & 0xF0000000u, 1u << 28);
        r.accept_and_ready();
        CHECK_EQ(r.last_type(), kDataEprMode);
        CHECK_EQ(r.sent.back().data[3], kEprExit);
        CHECK_EQ(r.sink.status().epr_mode, false);
        CHECK_EQ(r.sink.status().held, false);
        CHECK_EQ(r.sink.status().mv, 5000);
    }

    // ---- EPR mode: a Source_Capabilities that is not the answer to our Get_Source_Cap is a Hard Reset by the SINK
    // (USB PD R3.2 7.30.5: "the Sink Shall initiate a Hard Reset"); only EPR_Source_Capabilities exist in EPR mode.
    // In SPR mode the sink keeps re-requesting (tests above: CUKTECH C1 re-announce was SPR mode). ----
    {
        Rig r;
        r.caps = make_caps(true);
        r.sink.trigger({9, 21700, 5000}, r.now); r.pump();
        r.data(kDataEprMode, std::uint32_t(kEprAck) << 24);
        r.data(kDataEprMode, std::uint32_t(kEprSucceeded) << 24);
        r.epr_caps_done(); r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(r.sink.status().epr_mode, true);
        const std::uint32_t resets = r.sink.status().hard_resets;
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.back().hard_reset ? 1 : 0, 1);
        CHECK_EQ(r.sink.status().hard_resets, resets + 1);
        CHECK_EQ(r.sink.status().epr_mode, false);
        CHECK_EQ(int(r.sink.status().held), 0);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::HardReset));
        CHECK_EQ(r.sink.take_resume() ? 1 : 0, 1);   // the post-reset caps get a PDO 1 Request, like after a source reset
    }
    {   // the answer to our own Get_Source_Cap (the Xiaomi auth sends one after step 0x0A) is not unsolicited
        Rig r;
        r.caps = make_caps(true);
        r.sink.trigger({9, 21700, 5000}, r.now); r.pump();
        r.data(kDataEprMode, std::uint32_t(kEprAck) << 24);
        r.data(kDataEprMode, std::uint32_t(kEprSucceeded) << 24);
        r.epr_caps_done(); r.accept_and_ready();
        auto vdm = [&](std::uint32_t w) { r.data(kMsgVendorDefined, w); };
        vdm(0xFF008002u);          // Discover SVIDs REQ: the sink answers [0x2717]
        vdm(0x2717020Au);          // authenticated: the sink asks Get_Source_Cap
        bool asked = false;
        for (const TxMsg& m : r.sent) if ((m.header & 0x1F) == kCtlGetSourceCap && !((m.header >> 15) & 1)) asked = true;
        CHECK_EQ(asked ? 1 : 0, 1);
        const std::uint32_t resets = r.sink.status().hard_resets;
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sink.status().hard_resets, resets);
        // only the first Source_Capabilities answers it: the next unsolicited one is a Hard Reset again
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sink.status().hard_resets, resets + 1);
    }

    // ---- EPR entry preconditions (PD 3.2 EPR Mode entry step 2a: the most recent RDO must carry "EPR Mode Capable") ----
    {   // Baseus 140 W (hardware 2026-11-04): PDO1 5 V 3 A has the EPR bit (bit 23); our START Request left RDO bit 22 clear and the
        // source answered Enter_Failed. Every Request to such a source sets bit 22, SPR contract or not.
        Rig r;
        r.caps.pdos[0] = decode_pdo(0x0081912C);     // 5 V 3 A, EPR Mode Capable
        r.sink.trigger({1, 0, 0}, r.now); r.pump();
        CHECK_EQ(r.last_type(), kDataRequest);
        CHECK_EQ(int(r.last_rdo() >> 22 & 1), 1);
        r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({2, 0, 0}, r.now); r.pump();   // a later SPR re-request keeps the bit
        CHECK_EQ(int(r.last_rdo() >> 22 & 1), 1);
        r.accept_and_ready();
        r.run(kSettleMs + 100);
        r.sink.trigger({5, 5000, 3000}, r.now); r.pump();   // PPS too
        CHECK_EQ(int(r.last_rdo() >> 22 & 1), 1);
        r.accept_and_ready();
        r.sink.release(r.now); r.pump();               // and the release to 5 V
        CHECK_EQ(int(r.last_rdo() >> 22 & 1), 1);
    }
    {   // no EPR bit in PDO1: the bit stays clear
        Rig r;
        r.sink.trigger({1, 0, 0}, r.now); r.pump();
        CHECK_EQ(int(r.last_rdo() >> 22 & 1), 0);
        CHECK_EQ(plan_request(r.caps, {3, 0, 0}, 36000, false).rdo >> 22 & 1, 0u);
    }
    {   // a PD 2.0 source: bit 23 of the 5 V PDO is reserved there, the RDO bit stays clear
        Rig r;
        r.caps.pdos[0] = decode_pdo(0x0081912C);
        r.caps.spec_rev = 1;
        CHECK_EQ(plan_request(r.caps, {1, 0, 0}, 36000, false).rdo >> 22 & 1, 0u);
    }
    {   // EPR_Mode Enter carries the sink's EPR Operational PDP; Enter_Failed's reason is kept for the log and the UI
        Rig r;
        r.caps.pdos[0] = decode_pdo(0x0081912C);
        CHECK_EQ(int(r.sink.trigger({0, 0, 0}, r.now)), int(Err::Ok));
        r.pump();
        CHECK_EQ(r.last_type(), kDataEprMode);
        CHECK_EQ(int(r.sent.back().data[3]), int(kEprEnter));
        CHECK_EQ(int(r.sent.back().data[2]), int(kEprSinkPdpW));
        CHECK_EQ(int(r.sent.back().data[0] | r.sent.back().data[1]), 0);   // reserved bits stay zero
        r.data(kDataEprMode, std::uint32_t(kEprFailed) << 24 | 3u << 16);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::EprFailed));
        CHECK_EQ(int(r.sink.status().epr_fail), 3);
        CHECK_STR(epr_fail_name(0), "unknown cause");
        CHECK_STR(epr_fail_name(1), "cable not EPR capable");
        CHECK_STR(epr_fail_name(2), "source not VCONN source");
        CHECK_STR(epr_fail_name(3), "EPR bit not set in RDO");
        CHECK_STR(epr_fail_name(4), "source unable now");
        CHECK_STR(epr_fail_name(5), "EPR bit not set in 5 V PDO");
        CHECK_STR(epr_fail_name(9), "invalid reason");
        r.sink.trigger({0, 0, 0}, r.now); r.pump();      // a new attempt clears the old reason
        CHECK_EQ(int(r.sink.status().epr_fail), 0);
    }

    // ---- detach: everything forgotten, nothing transmitted ----
    {
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.detach();
        TxMsg m;
        CHECK_EQ(int(r.sink.take_tx(m)), 0);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));
        CHECK_EQ(int(r.sink.status().held), 0);
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::None));
        CHECK_EQ(int(r.sink.status().pos), 0);
        r.run(20000);
        CHECK_EQ(r.sent.size(), 1u);
    }

    // ---- Get_Sink_Cap (spec PE_SNK_Give_Sink_Cap): answered with Sink_Capabilities ----
    {
        Rig r;
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.size(), 1u);
        const auto& m = r.sent.back();
        CHECK_EQ(m.header, 0x2084);   // Sink_Capabilities, 2 objects, PD3.x, sink/UFP, id stamped by the driver
        CHECK_EQ(m.len, 8);
        const auto word = [&](int i) {
            return m.data[4 * i] | (m.data[4 * i + 1] << 8) | (m.data[4 * i + 2] << 16) | (std::uint32_t(m.data[4 * i + 3]) << 24);
        };
        CHECK_EQ(word(0), 0x0001900A);   // fixed 5 V 100 mA (first word)
        CHECK_EQ(word(1), 0xC0DC213C);   // PPS 3.3-11 V 3 A
        CHECK_EQ(int(m.hard_reset), 0);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));   // a pure answer: no state change
        r.run(2000);
        CHECK_EQ(r.sent.size(), 1u);   // nothing else, nothing repeated
    }
    {   // the header follows the revision the source announced (PD 2.0 source: rev field 1)
        Rig r;
        r.caps.spec_rev = 1;
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.size(), 1u);
        CHECK_EQ(r.sent.back().header, 0x2044);
    }
    {   // PD-silent legacy session: no answer (the source is never acknowledged either)
        Rig r;
        r.sink.set_gives_sink_cap(false);
        r.ctl(kCtlGetSinkCap);
        r.run(500);
        CHECK_EQ(r.sent.size(), 0u);
        r.sink.set_gives_sink_cap(true);
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.size(), 1u);
    }
    {   // a Request already queued is not lost to the answer: both go out, the contract still completes
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now);   // Request queued, not yet taken
        r.sink.on_message(Sop::Sop, make_header(kCtlGetSinkCap, 0, 2) | (1u << 8), {}, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), 2u);
        CHECK_EQ(r.sent[0].header & 0x1F, kDataSinkCap);
        CHECK_EQ(r.sent[1].header & 0x1F, kDataRequest);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }
    {   // a detach drops an unsent answer
        Rig r;
        r.sink.on_message(Sop::Sop, make_header(kCtlGetSinkCap, 0, 2) | (1u << 8), {}, r.now);
        r.sink.detach();
        TxMsg m;
        CHECK_EQ(int(r.sink.take_tx(m)), 0);
    }

    // ---- Get_Sink_Cap with an override (the device twin's live proof answers as the twin) ----
    {
        Rig r;
        static const std::uint32_t twin_pdos[4] = {0x0801912C, 0x0002D0C8, 0xC0DC2164, 0x0004B0C8};   // 5 V 3 A, 9 V 2 A, PPS, 20 V 2 A
        r.sink.set_sink_cap_override(twin_pdos, 4);
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.size(), 1u);
        const auto& m = r.sent.back();
        CHECK_EQ(m.header, 0x4084);   // four objects now; the revision and role bits are the ones of the plain answer
        CHECK_EQ(m.len, 16);
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t w = m.data[4 * i] | (m.data[4 * i + 1] << 8) | (m.data[4 * i + 2] << 16) | (std::uint32_t(m.data[4 * i + 3]) << 24);
            CHECK_EQ(w, twin_pdos[i]);
        }
        // the Sink_Capabilities_Extended answer stays the K2's own (a twin keeps no SKEDB)
        r.ctl(kCtlGetSinkCapExt);
        CHECK_EQ(r.sent.size(), 2u);
        // back to the K2's own words, and a detach does not clear the override (the live run owns it)
        r.sink.detach();
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.size(), 3u);
        CHECK_EQ(r.sent.back().len, 16);
        r.sink.set_sink_cap_override(nullptr, 0);
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.size(), 4u);
        CHECK_EQ(r.sent.back().header, 0x2084);
        CHECK_EQ(r.sent.back().len, 8);
        CHECK_EQ(int(r.sent.back().data[0]), 0x0A);
    }
    {   // more than seven words or none: the K2's own answer (an override is never a malformed message)
        Rig r;
        static const std::uint32_t nine[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        r.sink.set_sink_cap_override(nine, 9);
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.back().len, 8);
        r.sink.set_sink_cap_override(nine, 0);
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.back().len, 8);
    }

    // ---- Get_Sink_Cap_Extended (control type 22, USB PD 7.19.4): Sink_Capabilities_Extended / Not_Supported / silence ----
    {   // the SKEDB itself: 24 bytes, version 1, Sink Modes = AVS + VBUS powered + PPS, EPR fields zero (not EPR capable)
        const auto s = make_skedb();
        CHECK_EQ(s.size(), 24u);
        CHECK_EQ(int(s[0] | s[1] << 8), 0x0716);    // VID
        CHECK_EQ(int(s[2] | s[3] << 8), 0x5060);    // PID
        CHECK_EQ(int(s[4] | s[5] | s[6] | s[7]), 0);   // XID: none assigned
        CHECK_EQ(int(s[10]), 1);                    // SKEDB Version
        CHECK_EQ(int(s[17]), 0x23);                 // xx1xxxxx AVS, xxxxxx1x VBUS powered, xxxxxxx1 PPS
        CHECK_EQ(int(s[18]), 1);                    // SPR Min PDP, 0.5 W of the 5 V 100 mA mode rounded up
        CHECK_EQ(int(s[19]), 15);                   // SPR Operational PDP
        CHECK_EQ(int(s[20]), 33);                   // SPR Max PDP = 11 V * 3 A, the PPS word of Sink_Capabilities
        CHECK_EQ(int(s[18] <= s[19] && s[19] <= s[20]), 1);   // USB PD 6.5.16: min <= operational <= max
        CHECK_EQ(int(s[21] | s[22] | s[23]), 0);    // EPR PDPs: zero, the sink is not EPR capable
    }
    {   // default: the SKEDB. The UGREEN X865 (hardware 2026-11-04) asks right after Get_Sink_Cap and re-announces its caps with
        // the 7th PDO (SPR AVS APDO) 17 ms after it; Not_Supported or silence leaves it at 6 PDOs.
        Rig r;
        CHECK_EQ(int(r.sink.sink_cap_ext_mode()), int(SinkCapExtMode::Skedb));
        r.ctl(kCtlGetSinkCapExt);
        CHECK_EQ(r.sent.size(), 1u);
        CHECK_EQ(r.sent.back().header, 0xF08F);
    }
    {   // the lab switch can still silence it
        Rig r;
        r.sink.set_sink_cap_ext_mode(SinkCapExtMode::Silent);
        r.ctl(kCtlGetSinkCapExt);
        r.run(500);
        CHECK_EQ(r.sent.size(), 0u);
    }
    {   // Skedb: Sink_Capabilities_Extended, one chunk: ext header 0x8018 (chunked, chunk 0, 24 bytes) + SKEDB + 2 pad
        Rig r;
        r.sink.set_sink_cap_ext_mode(SinkCapExtMode::Skedb);
        r.ctl(kCtlGetSinkCapExt);
        CHECK_EQ(r.sent.size(), 1u);
        const auto& m = r.sent.back();
        CHECK_EQ(m.header, 0xF08F);                 // extended, 7 objects, PD3.x, type 15 (Sink_Capabilities_Extended)
        CHECK_EQ(m.len, 28);
        CHECK_EQ(int(m.sop == Sop::Sop), 1);
        CHECK_EQ(int(m.data[0] | m.data[1] << 8), 0x8018);
        const auto s = make_skedb();
        for (unsigned i = 0; i < 24; i++) CHECK_EQ(int(m.data[2 + i]), int(s[i]));
        CHECK_EQ(int(m.data[26] | m.data[27]), 0);  // padding to the object boundary
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));
        r.run(2000);
        CHECK_EQ(r.sent.size(), 1u);
    }
    {   // NotSupported: control message type 16, no payload
        Rig r;
        r.sink.set_sink_cap_ext_mode(SinkCapExtMode::NotSupported);
        r.ctl(kCtlGetSinkCapExt);
        CHECK_EQ(r.sent.size(), 1u);
        CHECK_EQ(r.sent.back().header, 0x0090);
        CHECK_EQ(r.sent.back().len, 0);
    }
    {   // the header follows the revision the source announced
        Rig r;
        r.caps.spec_rev = 1;
        r.sink.set_sink_cap_ext_mode(SinkCapExtMode::NotSupported);
        r.ctl(kCtlGetSinkCapExt);
        CHECK_EQ(r.sent.back().header, 0x0050);
    }
    {   // never while the sink must not transmit (PD-silent session, inline); a detach drops an unsent answer
        Rig r;
        r.sink.set_sink_cap_ext_mode(SinkCapExtMode::Skedb);
        r.sink.set_gives_sink_cap(false);
        r.ctl(kCtlGetSinkCapExt);
        r.run(500);
        CHECK_EQ(r.sent.size(), 0u);
        r.sink.set_gives_sink_cap(true);
        r.sink.on_message(Sop::Sop, make_header(kCtlGetSinkCapExt, 0, 2) | (1u << 8), {}, r.now);
        r.sink.detach();
        TxMsg m;
        CHECK_EQ(int(r.sink.take_tx(m)), 0);
    }
    {   // a queued Request is not lost to the answer
        Rig r;
        r.sink.set_sink_cap_ext_mode(SinkCapExtMode::Skedb);
        r.sink.trigger({2, 0, 0}, r.now);
        r.sink.on_message(Sop::Sop, make_header(kCtlGetSinkCapExt, 0, 2) | (1u << 8), {}, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), 2u);
        CHECK_EQ(r.sent[0].header & 0x1F, kExtSinkCapsExt);
        CHECK_EQ(r.sent[1].header & 0x1F, kDataRequest);
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }

    // ---- EPR caps without entering EPR (USB PD R3.2 6.5.17.1 / 7.30.8): an SPR-mode sink may send EPR_Get_Source_Cap; the answer is
    // informational and is never answered with an EPR_Request ----
    {
        Rig r;
        SourceCaps info;   // where the engine decodes an informational list (never the live caps)
        r.sink.bind_epr_info(&info);
        r.caps.pdos[0] = decode_pdo(0x0081912C);   // EPR Mode Capable
        r.sink.trigger({1, 0, 0}, r.now); r.pump();
        CHECK_EQ(int(r.sink.request_epr_info(r.now)), int(Err::Busy));   // not Ready yet: no query
        r.accept_and_ready();
        const std::size_t n0 = r.sent.size();
        CHECK_EQ(int(r.sink.request_epr_info(r.now)), int(Err::Ok));
        r.pump();
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), kExtControl);
        CHECK_EQ(int(r.sent.back().data[2]), int(kEprGetSourceCap));
        CHECK_EQ(r.sent.back().data[0], 2); CHECK_EQ(r.sent.back().data[1], 0x80);   // chunked, size 2
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Pending));
        CHECK_EQ(int(r.sink.request_epr_info(r.now)), int(Err::Busy));                // one query at a time
        // the source answers with its whole list (one chunk): the engine decoded it into `info`
        info = make_caps(true); info.epr_partial = false;
        r.epr_caps_done();
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Got));
        CHECK_EQ(r.sent.size(), n0 + 1);                       // no EPR_Request, no Enter: informational only
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(r.sink.status().epr_mode, false);
        CHECK_EQ(r.caps.num_pdos, 6);                           // the live list is untouched
        r.run(2000);
        for (const TxMsg& m : r.sent) CHECK_EQ(int((m.header & 0x1F) == kDataEprRequest), 0);
        CHECK_EQ(r.sink.status().epr_mode, false);
    }
    {   // a long list: the sink still asks for the next chunk, and is Got only when the engine's list is complete
        Rig r;
        SourceCaps info;
        r.sink.bind_epr_info(&info);
        r.caps.pdos[0] = decode_pdo(0x0081912C);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.request_epr_info(r.now); r.pump();
        info = make_caps(true); info.epr_partial = true;
        std::uint8_t chunk[28] = {};
        const std::uint16_t eh = 40 | (1u << 15);
        chunk[0] = eh & 0xFF; chunk[1] = eh >> 8;
        const std::size_t n0 = r.sent.size();
        r.sink.on_message(Sop::Sop, make_header(kExtEprSourceCaps, 7, 2, true) | (1u << 8), chunk, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n0 + 1);                        // chunk request
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Pending));
        info.epr_partial = false;
        const std::uint16_t eh1 = 40 | (1u << 11) | (1u << 15);
        chunk[0] = eh1 & 0xFF; chunk[1] = eh1 >> 8;
        r.sink.on_message(Sop::Sop, make_header(kExtEprSourceCaps, 5, 2, true) | (1u << 8), chunk, r.now);
        r.pump();
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Got));
        CHECK_EQ(r.sent.size(), n0 + 1);
    }
    for (const std::uint8_t refusal : {kCtlNotSupported, kCtlReject}) {   // a source that cannot be an EPR source
        Rig r;
        SourceCaps info;
        r.sink.bind_epr_info(&info);
        r.caps.pdos[0] = decode_pdo(0x0081912C);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.request_epr_info(r.now); r.pump();
        r.ctl(refusal);
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Refused));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.request_epr_info(r.now)), int(Err::Busy));   // asked once per attach
    }
    {   // silence ends the wait
        Rig r;
        SourceCaps info;
        r.sink.bind_epr_info(&info);
        r.caps.pdos[0] = decode_pdo(0x0081912C);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.request_epr_info(r.now); r.pump();
        r.run(kEprInfoMs + 100);
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Timeout));
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }
    {   // a source without the EPR bit in PDO 1 is never asked; a detach forgets the answer
        Rig r;
        SourceCaps none;
        r.sink.bind_epr_info(&none);
        r.sink.trigger({1, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        CHECK_EQ(int(r.sink.request_epr_info(r.now)), int(Err::BadChoice));
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::None));
        // the debug ask (negative case: a charger that never advertised EPR) bypasses the gate
        CHECK_EQ(int(r.sink.request_epr_info(r.now, true)), int(Err::Ok));
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Pending));
        r.pump(); r.ctl(kCtlNotSupported);
        CHECK_EQ(int(r.sink.status().epr_info), int(EprInfo::Refused));
        Rig d;
        SourceCaps info;
        d.sink.bind_epr_info(&info);
        d.caps.pdos[0] = decode_pdo(0x0081912C);
        d.sink.trigger({1, 0, 0}, d.now); d.pump(); d.accept_and_ready();
        d.sink.request_epr_info(d.now); d.pump();
        d.ctl(kCtlNotSupported);
        d.sink.detach();
        CHECK_EQ(int(d.sink.status().epr_info), int(EprInfo::None));
    }

    // ---- Soft_Reset: the VBUS-preserving unit boundary of a self-powered run ----
    {   // a live contract is dropped with a Soft_Reset that restarts our MessageID, no Hard Reset
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        const std::size_t n0 = r.sent.size();
        r.sink.soft_reset(r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), unsigned(kCtlSoftReset));
        CHECK_EQ(int(r.sent.back().restart_id), 1);
        CHECK_EQ(int(r.sent.back().hard_reset), 0);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));
        CHECK_EQ(int(r.sink.status().pos), 0);
        CHECK_EQ(int(r.sink.status().held), 0);
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
    }
    {   // the source's Source_Capabilities after the reset reach the caller as usual; no Get_Source_Cap is needed then
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.soft_reset(r.now); r.pump();
        const std::size_t n0 = r.sent.size();
        r.ctl(kCtlAccept);
        r.run(200);
        r.data(kDataSourceCap, 0x0001912C);
        r.run(1000);
        CHECK_EQ(r.sent.size(), n0);
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
    }
    {   // a source that stays silent after the Accept gets a Get_Source_Cap after 400 ms, once
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.soft_reset(r.now); r.pump();
        const std::size_t n0 = r.sent.size();
        r.run(300);
        CHECK_EQ(r.sent.size(), n0);
        r.run(200);
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSourceCap));
        r.run(2000);
        CHECK_EQ(r.sent.size(), n0 + 1);
    }
    {   // no GoodCRC for the Soft_Reset is never escalated: the sink stays Idle and still asks for the caps
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.soft_reset(r.now);
        TxMsg m;
        CHECK_EQ(int(r.sink.take_tx(m)), 1);
        r.sink.on_tx_result(false, r.now);
        const std::size_t n0 = r.sent.size();
        r.run(2000);
        CHECK_EQ(int(r.sink.status().hard_resets), 0);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), unsigned(kCtlGetSourceCap));
    }
    {   // a detach cancels the pending Get_Source_Cap
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.soft_reset(r.now); r.pump();
        r.sink.detach();
        const std::size_t n0 = r.sent.size();
        r.run(1000);
        CHECK_EQ(r.sent.size(), n0);
    }

    // ---- role swaps: DR_Swap / PR_Swap -> Reject, FR_Swap -> Not_Supported (PD 3 only), never Accept ----
    {   // DR_Swap (control 9, no data objects) is answered with Reject in the same take_tx
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::size_t n0 = r.sent.size();
        r.ctl(kCtlDrSwap);
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), unsigned(kCtlReject));
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Dr)), 1);
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Pr)), 0);
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Fr)), 0);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));   // the contract is untouched
        r.run(500);
        CHECK_EQ(r.sent.size(), n0 + 1);                                // answered once, nothing else
    }
    {   // type 9 WITH a data object is EPR_Request, not DR_Swap: the sink does not answer it as a swap
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::size_t n0 = r.sent.size();
        r.data(kDataEprRequest, 0x00000000);
        CHECK_EQ(r.sent.size(), n0);
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Dr)), 0);
    }
    {   // PR_Swap is refused: Accept would make the partner expect the K2 to drive VBUS
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::size_t n0 = r.sent.size();
        r.ctl(kCtlPrSwap);
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), unsigned(kCtlReject));
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Pr)), 1);
    }
    {   // FR_Swap on a PD 3 contract: Not_Supported
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::size_t n0 = r.sent.size();
        r.ctl(kCtlFrSwap);
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), unsigned(kCtlNotSupported));
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Fr)), 1);
    }
    {   // FR_Swap does not exist in PD 2.0: no answer, still counted
        Rig r;
        r.caps.spec_rev = 1;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        const std::size_t n0 = r.sent.size();
        r.ctl(kCtlFrSwap);
        CHECK_EQ(r.sent.size(), n0);
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Fr)), 1);
        r.ctl(kCtlDrSwap);   // DR_Swap exists in PD 2.0 and is still refused
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(r.last_type(), unsigned(kCtlReject));
    }
    {   // a swap arriving while our Request is in flight does not displace it or move the state machine
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump();
        const std::size_t n0 = r.sent.size();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.ctl(kCtlDrSwap);
        CHECK_EQ(r.sent.size(), n0 + 1);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().pos), 2);
    }
    {   // the same gate as Get_Sink_Cap: inline or PD-silent, the K2 is not the addressed sink and sends nothing
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        r.sink.set_gives_sink_cap(false);
        const std::size_t n0 = r.sent.size();
        r.ctl(kCtlDrSwap);
        r.ctl(kCtlPrSwap);
        r.ctl(kCtlFrSwap);
        CHECK_EQ(r.sent.size(), n0);
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Dr)), 0);
    }
    {   // counters saturate at 255 and each swap type has its own
        Rig r;
        r.sink.trigger({2, 0, 0}, r.now); r.pump(); r.accept_and_ready();
        for (int i = 0; i < 300; i++) r.ctl(kCtlPrSwap);
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Pr)), 255);
        CHECK_EQ(int(r.sink.swaps_rx(SwapKind::Dr)), 0);
    }

    // ---- review 2026-11-09, red first: a re-announce while held re-requests the held LEVEL by voltage
    // (fixed) or range (programmable), never by position ----
    {   // 15 V held at PDO 3, the source re-announces 5/9/20 V: the held level is gone, so the answer is a
        // Request for PDO 1 (5 V) - never PDO 3, which now holds 20 V (a legal but unchosen voltage) - and the
        // hold is reported lost (held off, result Aborted) once that Request is over.
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();   // hold 15 V at PDO 3
        CHECK_EQ(int(r.sink.status().pos), 3); CHECK_EQ(r.sink.status().mv, 15000);
        r.caps = SourceCaps{};
        r.caps.valid = true; r.caps.spec_rev = 2; r.caps.num_pdos = 3;
        r.caps.pdos[0] = decode_pdo(0x0001912C);   // 5 V 3 A
        r.caps.pdos[1] = decode_pdo(0x0002D12C);   // 9 V 3 A
        r.caps.pdos[2] = decode_pdo(0x000640C8);   // 20 V 3 A: PDO 3 is now 20 V
        const std::size_t n = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 3, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);                                      // the re-announce is answered
        CHECK_EQ(r.last_type(), unsigned(kDataRequest));
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 1);                          // PDO 1, never 20 V
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().held), 0);                              // the hold is reported lost
        CHECK_EQ(int(r.sink.status().result), int(SinkResult::Aborted));
        CHECK_EQ(int(r.sink.status().pos), 1); CHECK_EQ(int(r.sink.status().mv), 5000);
        for (std::size_t i = n; i < r.sent.size(); ++i) {   // after the re-announce, every Request is PDO 1 (20 V sits at PDO 3 now)
            const TxMsg& m = r.sent[i];
            CHECK_EQ(int((m.header & 0x1F) == kDataRequest ? int((m.data[3] >> 4) & 7) : 9), 1);
        }
    }
    {   // the same hold, the source re-announces 5/15/20 V: 15 V moved to PDO 2, the answer is a Request for
        // PDO 2 at the new current word, and the hold stands.
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump(); r.accept_and_ready();   // hold 15 V at PDO 3
        r.caps = SourceCaps{};
        r.caps.valid = true; r.caps.spec_rev = 2; r.caps.num_pdos = 3;
        r.caps.pdos[0] = decode_pdo(0x0001912C);   // 5 V 3 A
        r.caps.pdos[1] = decode_pdo(0x0004B096);   // 15 V 1.5 A: the held level at slot 2, lower current
        r.caps.pdos[2] = decode_pdo(0x000640C8);   // 20 V 3 A
        const std::size_t n = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 3, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(r.last_type(), unsigned(kDataRequest));
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 2);                          // the held 15 V at its new slot
        CHECK_EQ(r.last_rdo(), rdo_fixed(2, 1500, 1500, false));             // the new, lower current word
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().held), 1);                              // the hold stands
        CHECK_EQ(int(r.sink.status().pos), 2); CHECK_EQ(int(r.sink.status().mv), 15000); CHECK_EQ(int(r.sink.status().ma), 1500);
    }
    {   // a PPS hold: the APDO moved to another slot but still covers the held voltage: re-request there
        Rig r;
        r.sink.trigger({5, 9000, 2000}, r.now); r.pump(); r.accept_and_ready();   // hold PPS 9 V at PDO 5
        CHECK_EQ(r.sink.status().pos, 5); CHECK_EQ(r.sink.status().mv, 9000);
        r.caps = make_caps();
        r.caps.num_pdos = 6;
        r.caps.pdos[5] = r.caps.pdos[4];            // the held 3.3-11 V APDO moves to PDO 6
        r.caps.pdos[4] = decode_pdo(0xC0000000u | (210u << 17) | (110u << 8) | 100u);   // an 11-21 V APDO at PDO 5: it does not cover the held 9 V
        const std::size_t n = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x64, 0x21, 0xDC, 0xC0};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 6, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 6);                          // the APDO at its new slot
        CHECK_EQ(int((r.last_rdo() >> 9) & 0x1FF), 9000 / 20);               // the held 9 V target
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().held), 1); CHECK_EQ(int(r.sink.status().pos), 6); CHECK_EQ(r.sink.status().mv, 9000);
    }
    {   // a Source_Capabilities arriving in Transition while held, with no release pending, gets a Request
        // within tSenderResponse (the source Hard Resets otherwise); the source restarts negotiation, so the
        // fresh list is answered like any re-announce.
        Rig r;
        r.sink.trigger({3, 0, 0}, r.now); r.pump();                          // Request for 15 V out
        r.ctl(kCtlAccept);                                                   // Transition, held, no release
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Transition));
        const std::size_t n = r.sent.size();
        const std::uint8_t caps_bytes[4] = {0x2C, 0x91, 0x01, 0x00};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_bytes, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), n + 1);                                      // a Request answered the caps
        CHECK_EQ(r.last_type(), unsigned(kDataRequest));
        CHECK_EQ(int((r.last_rdo() >> 28) & 7), 3);                          // the held 15 V, still at PDO 3 here
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().held), 1); CHECK_EQ(int(r.sink.status().mv), 15000);
    }

    // ---- the driver's own PDO 1 answer is marked (auto_hold); any trigger clears the mark (hardware 2026-11-11) ----
    {
        Rig r;
        CHECK_EQ(r.sink.answer_5v({1, 0, 0}, r.now), Err::Ok);
        r.pump();
        CHECK_COND(r.sink.status().held);              // held stays: the re-announce answer in Requesting needs it
        CHECK_COND(r.sink.status().auto_hold);
        CHECK_COND(!user_hold(r.sink.status()));
        CHECK_COND(!auto_own_hold(r.sink.status()));   // not settled yet
        r.accept_and_ready();
        CHECK_COND(r.sink.status().state == SinkState::Ready);
        CHECK_COND(auto_own_hold(r.sink.status()));
        CHECK_COND(!user_hold(r.sink.status()));
        CHECK_EQ(r.sink.trigger({2, 0, 0}, r.now), Err::Ok);   // a user trigger owns it now
        r.pump();
        CHECK_COND(!r.sink.status().auto_hold);
        CHECK_COND(user_hold(r.sink.status()));
        r.accept_and_ready();
        CHECK_COND(!auto_own_hold(r.sink.status()));
        CHECK_EQ(r.sink.trigger({1, 0, 0}, r.now), Err::Ok);   // a user 5 V trigger is a user hold too
        r.pump(); r.accept_and_ready();
        CHECK_COND(user_hold(r.sink.status()));
        CHECK_COND(!auto_own_hold(r.sink.status()));
    }
    {   // a refused answer leaves the mark alone; a failed attempt clears held and the mark
        Rig r;
        CHECK_EQ(r.sink.trigger({2, 0, 0}, r.now), Err::Ok);
        r.pump();
        CHECK_COND(r.sink.answer_5v({1, 0, 0}, r.now) == Err::Busy);   // the user's Request is in flight
        CHECK_COND(!r.sink.status().auto_hold);
        Rig q;
        CHECK_EQ(q.sink.answer_5v({1, 0, 0}, q.now), Err::Ok);
        q.pump();
        q.ctl(kCtlReject);
        CHECK_COND(!q.sink.status().held);
        CHECK_COND(!q.sink.status().auto_hold);
    }
    // ---- HID-cable loop 5 V lock (k2/core/loop_guard.hpp): every raise refused, a held level back to PDO 1 ----
    {   // Locked: fixed above 5 V, PPS above 5.5 V and EPR entry are refused and nothing goes on the wire; 5 V passes.
        Rig r;
        r.sink.set_hv_lock(true, r.now);
        CHECK_COND(r.sink.hv_lock());
        // The lock is a 5 V cap: OverCap (pd::Err has no room for a new value, see its comment).
        CHECK_EQ(r.sink.trigger({2, 0, 0}, r.now), Err::OverCap);
        CHECK_EQ(r.sink.trigger({3, 0, 0}, r.now), Err::OverCap);
        CHECK_EQ(r.sink.trigger({5, 9000, 0}, r.now), Err::OverCap);   // PPS 9 V
        CHECK_EQ(r.sink.trigger({5, 0, 0}, r.now), Err::OverCap);      // PPS at its maximum (11 V)
        CHECK_EQ(r.sink.trigger({0, 0, 0}, r.now), Err::OverCap);      // EPR entry
        CHECK_EQ(r.sink.trigger({8, 0, 0}, r.now), Err::OverCap);      // an EPR position (needs the entry first)
        r.pump();
        CHECK_EQ(r.sent.size(), 0u);
        CHECK_EQ(r.sink.trigger({5, 5500, 1000}, r.now), Err::Ok);    // PPS at 5.5 V: still vSafe5V
        r.pump(); r.accept_and_ready();
        CHECK_EQ(r.sink.trigger({1, 0, 0}, r.now), Err::Ok);          // PDO 1
        r.pump(); r.accept_and_ready();
        CHECK_EQ(r.sink.answer_5v({1, 0, 0}, r.now), Err::Ok);
        r.sink.set_hv_lock(false, r.now);                             // unlocked: raises go out again
        r.pump(); r.accept_and_ready();
        CHECK_EQ(r.sink.trigger({2, 0, 0}, r.now), Err::Ok);
    }
    {   // The lock edge with a 15 V contract held: a Request for PDO 1 at once, the hold dropped.
        Rig r;
        CHECK_EQ(r.sink.trigger({3, 0, 0}, r.now), Err::Ok);
        r.pump(); r.accept_and_ready();
        CHECK_EQ(r.sink.status().mv, 15000);
        const std::size_t before = r.sent.size();
        r.sink.set_hv_lock(true, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), before + 1);
        CHECK_EQ(r.last_type(), kDataRequest);
        CHECK_EQ((r.last_rdo() >> 28) & 7, 1);   // object position 1
        CHECK_COND(!r.sink.status().held);
        r.accept_and_ready();
        CHECK_EQ(r.sink.status().mv, 5000);
        r.sink.set_hv_lock(true, r.now);        // still locked: no second edge, nothing sent
        r.pump();
        CHECK_EQ(r.sent.size(), before + 1);
    }
    {   // The lock edge at a 5 V contract: nothing to send.
        Rig r;
        CHECK_EQ(r.sink.trigger({1, 0, 0}, r.now), Err::Ok);
        r.pump(); r.accept_and_ready();
        const std::size_t before = r.sent.size();
        r.sink.set_hv_lock(true, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), before);
    }
    {   // The lock edge while a 20 V Request is in flight: PDO 1 follows right after its PS_RDY (one Request at a time).
        Rig r;
        CHECK_EQ(r.sink.trigger({4, 0, 0}, r.now), Err::Ok);
        r.pump();
        const std::size_t before = r.sent.size();
        r.sink.set_hv_lock(true, r.now);
        r.pump();
        CHECK_EQ(r.sent.size(), before);       // queued behind the Request in flight
        r.accept_and_ready();
        CHECK_EQ(r.last_type(), kDataRequest);
        CHECK_EQ((r.last_rdo() >> 28) & 7, 1);
    }
    {   // The lock edge while the EPR entry is under way: the entry is dropped.
        Rig r;
        r.caps = make_caps(true);
        CHECK_EQ(r.sink.trigger({1, 0, 0}, r.now), Err::Ok);
        r.pump(); r.accept_and_ready();
        CHECK_EQ(r.sink.trigger({8, 0, 0}, r.now), Err::Ok);   // EPR 28 V: EPR_Mode Enter first
        r.pump();
        CHECK_COND(r.sink.status().state == SinkState::EprEntering);
        r.sink.set_hv_lock(true, r.now);
        CHECK_COND(r.sink.status().state != SinkState::EprEntering && r.sink.status().state != SinkState::EprWaitCaps);
        CHECK_COND(!r.sink.status().held);
    }
    {   // A re-announce while locked, contract at 5 V held by nobody: answered with PDO 1, never a level above 5 V.
        Rig r;
        CHECK_EQ(r.sink.trigger({1, 0, 0}, r.now), Err::Ok);
        r.pump(); r.accept_and_ready();
        r.sink.set_hv_lock(true, r.now);
        const std::uint8_t caps[4] = {0x2C, 0x91, 0x01, 0};
        r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps, r.now);
        r.pump();
        CHECK_EQ(r.last_type(), kDataRequest);
        CHECK_EQ((r.last_rdo() >> 28) & 7, 1);
    }
    // ---- external VBUS owner (the SuperVOOC trigger raises VBUS under the K2's own 5 V contract, hardware 2026-11-12) ----
    {   // Control: without the suspension a rise above target + 10 % under the held 5 V contract is an abort (existing guard).
        Rig r;
        CHECK_EQ(r.sink.answer_5v({1, 0, 0}, r.now), Err::Ok);
        r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        const std::size_t before = r.sent.size();
        r.sink.on_vbus(5000, r.now); r.pump(); r.run(100);   // the rise is judged against the lowest sample so far
        CHECK_EQ(r.sent.size(), before);
        r.sink.on_vbus(9000, r.now); r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
        CHECK_EQ(r.sent.size(), before + 1);
    }
    {   // Suspended: VBUS anywhere from 5 V to far over the cap produces no TX and no state change, for as long as it lasts.
        Rig r;
        CHECK_EQ(r.sink.answer_5v({1, 0, 0}, r.now), Err::Ok);
        r.pump(); r.accept_and_ready();
        r.run(kSettleMs + 100);
        const std::size_t before = r.sent.size();
        r.sink.set_vbus_external(true, r.now);
        CHECK_COND(r.sink.vbus_external());
        for (int mv : {5000, 8000, 9000, 12000, 12000, 38000}) {
            r.sink.on_vbus(mv, r.now); r.pump(); r.run(100);
        }
        r.run(2 * kSettleMs + kDescentMaxMs);   // long enough for the Hard Reset timers and any keepalive
        r.sink.on_vbus(12000, r.now); r.pump();
        CHECK_EQ(r.sent.size(), before);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_COND(r.sink.status().held);
        // Clear while VBUS is still high and settling: the settle window applies, nothing goes out yet.
        r.sink.set_vbus_external(false, r.now);
        CHECK_COND(!r.sink.vbus_external());
        for (int mv = 12000; mv > 5000; mv -= 500) { r.sink.on_vbus(mv, r.now); r.pump(); r.run(100); }
        CHECK_EQ(r.sent.size(), before);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        // Settled at 5 V: still quiet.
        for (int i = 0; i < 40; ++i) { r.sink.on_vbus(5000, r.now); r.pump(); r.run(100); }
        CHECK_EQ(r.sent.size(), before);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        // The guard is back after the clear: a rise past the settle window aborts again.
        r.sink.on_vbus(9000, r.now); r.pump();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Aborting));
    }
    {   // Messages from the source are still handled while suspended.
        Rig r;
        CHECK_EQ(r.sink.answer_5v({1, 0, 0}, r.now), Err::Ok);
        r.pump(); r.accept_and_ready();
        r.sink.set_vbus_external(true, r.now);
        const std::size_t before = r.sent.size();
        r.sink.on_vbus(12000, r.now); r.pump();
        CHECK_EQ(r.sent.size(), before);
        r.ctl(kCtlGetSinkCap);   // the answer to the source's question still goes out
        r.run(100);
        CHECK_EQ(r.sent.size(), before + 1);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
    }
    return test_report("test_pd_sink");
}
