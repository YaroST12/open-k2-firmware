// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the passive contract decode: the sink's Request and the source's Accept /
// PS_RDY on SOP show which PDO an existing contract uses even when the caps were missed.
#include "k2/pd/pd_engine.hpp"
#include "test_util.h"

using namespace k2::pd;

static void put_le32(std::uint8_t* d, std::uint32_t v) { for (int i = 0; i < 4; i++) d[i] = std::uint8_t(v >> (8 * i)); }

static void send(Engine& e, std::uint16_t header, std::uint32_t rdo = 0, unsigned n = 0) {
    std::uint8_t b[4];
    put_le32(b, rdo);
    e.on_message(Sop::Sop, header, {b, n * 4}, 1);
}
static std::uint16_t sink_request = (1u << 12) | (2u << 6) | 0x02;           // power role sink
static std::uint16_t source_ctl(unsigned type) { return (1u << 8) | (2u << 6) | type; }

static void load_caps(Engine& e) {
    // 5 V 3 A, 9 V 3 A, 15 V 3 A, 20 V 3 A fixed, PPS 3.3-11 V 5 A
    const std::uint32_t pdo[5] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x0006412C, 0xC0DC2164};
    std::uint8_t b[20];
    for (int i = 0; i < 5; i++) put_le32(b + 4 * i, pdo[i]);
    e.on_message(Sop::Sop, std::uint16_t((5u << 12) | (1u << 8) | (2u << 6) | 0x01), {b, 20}, 1);
}

static void test_fixed_request() {
    Engine e;
    load_caps(e);
    send(e, sink_request, (4u << 28) | (300u << 10) | 300, 1);
    CHECK_EQ(e.contract.valid, 1);
    CHECK_EQ(e.contract.pos, 4);
    CHECK_EQ(e.contract.mv, 20000);   // from the PDO the caps list at that position
    CHECK_EQ(e.contract.op_ma, 3000);
    CHECK_EQ(e.contract.max_ma, 3000);
    CHECK_EQ(e.contract.ready, 0);
    CHECK_EQ(e.contract.requests, 1);
    CHECK_EQ(e.contract.rdo, (4u << 28) | (300u << 10) | 300);   // the RDO itself is kept
    send(e, source_ctl(3));            // Accept
    CHECK_EQ(e.contract.ready, 0);
    send(e, source_ctl(6));            // PS_RDY
    CHECK_EQ(e.contract.ready, 1);
}

static void test_pps_request_and_new_request_clears_ready() {
    Engine e;
    load_caps(e);
    send(e, source_ctl(6));
    CHECK_EQ(e.contract.valid, 0);     // PS_RDY alone proves nothing about the position
    send(e, sink_request, (5u << 28) | (450u << 9) | 40, 1);   // 9.0 V, 2.0 A
    CHECK_EQ(e.contract.pos, 5);
    CHECK_EQ(e.contract.mv, 9000);
    CHECK_EQ(e.contract.op_ma, 2000);
    send(e, source_ctl(6));
    CHECK_EQ(e.contract.ready, 1);
    send(e, sink_request, (1u << 28) | (100u << 10) | 100, 1);
    CHECK_EQ(e.contract.ready, 0);
    CHECK_EQ(e.contract.requests, 2);
    CHECK_EQ(e.contract.mv, 5000);
}

static void test_request_without_caps() {
    Engine e;
    send(e, sink_request, (4u << 28) | (300u << 10) | 300, 1);
    CHECK_EQ(e.contract.valid, 1);
    CHECK_EQ(e.contract.pos, 4);
    CHECK_EQ(e.contract.mv, 0);        // unknown without the caps
    CHECK_EQ(e.contract.op_ma, 0);     // never decoded from a guessed RDO layout (a PPS voltage
    CHECK_EQ(e.contract.max_ma, 0);    //  would read as a max current here: "PD 0V5.82A")
}

// A position the caps do not list is as unknown as no caps at all: the PDO at that slot is not ours
// to guess (pdos[] beyond num_pdos are unused slots).
static void test_request_position_beyond_the_caps_is_not_guessed() {
    Engine e;
    load_caps(e);   // 5 PDOs
    send(e, sink_request, (9u << 28) | (300u << 10) | 300, 1);
    CHECK_EQ(e.contract.valid, 1);
    CHECK_EQ(e.contract.pos, 9);
    CHECK_EQ(e.contract.mv, 0);
    CHECK_EQ(e.contract.op_ma, 0);
    CHECK_EQ(e.contract.max_ma, 0);
}

// Caps sniffed after the Request (the K2 booted mid-contract, the source re-announces while inline):
// the contract on record is re-decoded against the caps that arrived.
static void test_caps_arriving_after_the_request_redecode_it() {
    Engine e;
    send(e, sink_request, (4u << 28) | (300u << 10) | 300, 1);
    CHECK_EQ(e.contract.mv, 0);
    CHECK_EQ(e.contract.max_ma, 0);
    load_caps(e);
    CHECK_EQ(e.contract.valid, 1);     // the Request is still the contract
    CHECK_EQ(e.contract.requests, 1);
    CHECK_EQ(e.contract.pos, 4);
    CHECK_EQ(e.contract.mv, 20000);    // now from the PDO the caps list at that position
    CHECK_EQ(e.contract.op_ma, 3000);
    CHECK_EQ(e.contract.max_ma, 3000);
}

static void test_source_message_is_not_a_request() {
    Engine e;
    load_caps(e);
    send(e, (1u << 12) | (1u << 8) | (2u << 6) | 0x02, (4u << 28), 1);  // power role source
    CHECK_EQ(e.contract.valid, 0);
}

static void test_reset_source_keeps_counters() {
    Engine e;
    load_caps(e);
    send(e, sink_request, (4u << 28) | (300u << 10) | 300, 1);
    e.reset_source();
    CHECK_EQ(e.caps.valid, 0);
    CHECK_EQ(e.contract.valid, 0);
    CHECK_EQ(e.contract.requests, 1);
    CHECK_EQ(e.caps.rx_count, 1);
}


static void test_rdo_flags_and_accept() {
    Engine e;
    load_caps(e);
    // pos 4, give-back(27) | mismatch(26) | usb comm(25) | no suspend(24), 2 A operating / 3 A max
    send(e, sink_request, (4u << 28) | (0xFu << 24) | (200u << 10) | 300, 1);
    CHECK_EQ(e.contract.flags, kRdoFGiveBack | kRdoFMismatch | kRdoFUsbComm | kRdoFNoSuspend);
    CHECK_EQ(e.contract.op_ma, 2000);
    CHECK_EQ(e.contract.max_ma, 3000);
    CHECK_EQ(e.contract.accepted, 0);
    send(e, source_ctl(3));            // Accept
    CHECK_EQ(e.contract.accepted, 1);
    CHECK_EQ(e.contract.ready, 0);
    send(e, source_ctl(6));
    CHECK_EQ(e.contract.ready, 1);
    // a new Request starts over: no flags of the old one, not accepted
    send(e, sink_request, (1u << 28) | (1u << 22) | (100u << 10) | 100, 1);
    CHECK_EQ(e.contract.flags, kRdoFEpr);
    CHECK_EQ(e.contract.accepted, 0);
}

static void test_reject_and_wait() {
    Engine e;
    load_caps(e);
    send(e, sink_request, (2u << 28) | (100u << 10) | 100, 1);
    send(e, source_ctl(4));            // Reject
    CHECK_EQ(e.contract.rejected, 1);
    CHECK_EQ(e.contract.accepted, 0);
    send(e, sink_request, (1u << 28) | (100u << 10) | 100, 1);
    CHECK_EQ(e.contract.rejected, 0);
}

static void test_sink_caps_only_from_a_sink() {
    Engine e;
    CHECK_EQ(e.sink.valid, 0);
    const std::uint32_t pdo[2] = {0x0001912C, 0x0002D12C};
    std::uint8_t b[8];
    for (int i = 0; i < 2; i++) put_le32(b + 4 * i, pdo[i]);
    // Sink_Capabilities (type 4) from a source-role port (the charger answering our Get_Sink_Cap): not the sink's
    e.on_message(Sop::Sop, std::uint16_t((2u << 12) | (1u << 8) | 0x04), {b, 8}, 1);
    CHECK_EQ(e.sink.valid, 0);
    e.on_message(Sop::Sop, std::uint16_t((2u << 12) | 0x04), {b, 8}, 1);
    CHECK_EQ(e.sink.valid, 1);
    CHECK_EQ(e.sink.n, 2);
    CHECK_EQ(e.sink.raw[1], 0x0002D12Cu);
    e.reset_sink();
    CHECK_EQ(e.sink.valid, 0);
}

int main() {
    test_fixed_request();
    test_pps_request_and_new_request_clears_ready();
    test_request_without_caps();
    test_request_position_beyond_the_caps_is_not_guessed();
    test_caps_arriving_after_the_request_redecode_it();
    test_source_message_is_not_a_request();
    test_reset_source_keeps_counters();
    test_rdo_flags_and_accept();
    test_reject_and_wait();
    test_sink_caps_only_from_a_sink();
    return test_report("test_pd_contract");
}
