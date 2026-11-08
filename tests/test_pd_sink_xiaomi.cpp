// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for Xiaomi Mi Turbo Charge authentication (SVID 0x2717).
#include <vector>

#include "k2/pd/pd_sink.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

SourceCaps make_caps_xiaomi() {
    SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    std::uint32_t raw[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000640C8,
                           0xC0DC2164, 0xC1A4213C};
    c.num_pdos = 6;
    for (int i = 0; i < 6; i++) c.pdos[i] = decode_pdo(raw[i]);
    return c;
}

// Helper to build VDM header word
constexpr std::uint32_t vdm_header(std::uint16_t svid, bool structured, std::uint8_t cmd_type, std::uint8_t cmd) {
    return (std::uint32_t(svid) << 16) | (structured ? (1u << 15) : 0) | (std::uint32_t(cmd_type) << 6) | cmd;
}

// Helper to send a VDM message to the sink
void send_vdm(Sink& sink, std::span<const std::uint32_t> vdos, std::uint32_t now) {
    std::array<std::uint8_t, 28> buf{};
    for (std::size_t i = 0; i < vdos.size(); i++) {
        const std::uint32_t v = vdos[i];
        buf[4 * i + 0] = std::uint8_t(v);
        buf[4 * i + 1] = std::uint8_t(v >> 8);
        buf[4 * i + 2] = std::uint8_t(v >> 16);
        buf[4 * i + 3] = std::uint8_t(v >> 24);
    }
    sink.on_message(Sop::Sop, make_header(kMsgVendorDefined, std::uint8_t(vdos.size()), 2), buf, now);
}

struct Rig {
    SourceCaps caps = make_caps_xiaomi();
    Sink sink;
    std::uint32_t now = 1000;
    std::vector<TxMsg> sent;
    explicit Rig(std::uint32_t cap = 36000) {
        sink.bind(&caps);
        sink.set_cap_mv(cap);
    }
    void pump() {
        TxMsg m;
        while (sink.take_tx(m)) { sent.push_back(m); sink.on_tx_result(true, now); }
    }
    void run(std::uint32_t ms) { for (std::uint32_t i = 0; i < ms; i += 10) { now += 10; sink.tick(now); pump(); } }
    // Send control message
    void ctl(std::uint8_t type) { sink.on_message(Sop::Sop, make_header(type, 0, 2) | (1u << 8), {}, now); pump(); }
    // Send data message with single object
    void data(std::uint8_t type, std::uint32_t obj) {
        const std::uint8_t b[4] = {std::uint8_t(obj), std::uint8_t(obj >> 8), std::uint8_t(obj >> 16), std::uint8_t(obj >> 24)};
        sink.on_message(Sop::Sop, make_header(type, 1, 2) | (1u << 8), b, now);
        pump();
    }
    // Get last sent message type
    unsigned last_type() const { return sent.empty() ? 0 : (sent.back().header & 0x1F); }
    // Get last sent VDM header from payload
    std::uint32_t last_vdm_header() const {
        if (sent.empty()) return 0;
        const auto& m = sent.back();
        if (m.len < 4) return 0;
        return std::uint32_t(m.data[0]) | (std::uint32_t(m.data[1]) << 8) | 
               (std::uint32_t(m.data[2]) << 16) | (std::uint32_t(m.data[3]) << 24);
    }
    // Get VDO at index from last message
    std::uint32_t last_vdo(std::size_t idx) const {
        if (sent.empty() || idx * 4 + 3 >= sent.back().len) return 0;
        const auto& m = sent.back();
        return std::uint32_t(m.data[idx * 4 + 0]) | (std::uint32_t(m.data[idx * 4 + 1]) << 8) | 
               (std::uint32_t(m.data[idx * 4 + 2]) << 16) | (std::uint32_t(m.data[idx * 4 + 3]) << 24);
    }
};

}  // namespace

// Test that without Xiaomi SVID, we don't respond to Xiaomi VDMs
static void test_no_xiaomi_svid_no_response() {
    Rig r;
    // Source sends Discover SVIDs (structured, cmd=2, ACK=1)
    // VDM header: 0xFF008042 (SVID 0xFF00, structured, ACK, cmd=2)
    // But source doesn't advertise 0x2717, so we should not reply with Xiaomi SVID
    constexpr std::array vdos = {vdm_header(0xFF00, true, 1, 2), 0x12D10000u};  // Only Huawei SVID
    send_vdm(r.sink, vdos, r.now);
    r.pump();
    // No VDM reply should be sent
    for (const auto& m : r.sent) {
        CHECK_EQ((m.header & 0x1F) != kMsgVendorDefined, true);
    }
}

// Test Discover SVIDs ACK from the source (answering OUR discover): opens the gate, no reply
static void test_discover_svids_reply() {
    Rig r;
    // Source (charger) sends Discover SVIDs ACK listing SVID 0x2717
    constexpr std::array vdos = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos, r.now);
    r.pump();
    // We must NOT reply to an ACK (it is the answer to our request, not a request to us)
    for (const auto& m : r.sent) {
        CHECK_EQ((m.header & 0x1F) != kMsgVendorDefined, true);
    }
    // But the gate opens: a following Xiaomi auth VDM gets an answer
    constexpr std::array auth = {vdm_header(0x2717, false, 0, 1)};
    send_vdm(r.sink, auth, r.now);
    r.pump();
    bool replied = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kMsgVendorDefined && r.last_vdo(0) == kXiaomiStep1ReplyVdo) replied = true;
    }
    CHECK_EQ(replied, true);
}

// H3: RX 0x27170201 -> TX kXiaomiStep1ReplyVdo (0x27170106).
static void test_xiaomi_cmd_01() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170201 (unstructured, REQ=0, cmd=1)
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 1)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with the step-1 word
    CHECK_EQ(r.sent.size() >= 1, true);
    const auto& m = r.sent.back();
    CHECK_EQ((m.header & 0x1F), kMsgVendorDefined);
    CHECK_EQ(r.last_vdm_header(), kXiaomiStep1ReplyVdo);
}

// Test Xiaomi auth command 0x27170202 -> reply 0x27170103
static void test_xiaomi_cmd_02() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170202
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 2)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with 0x27170103
    CHECK_EQ(r.sent.size() >= 1, true);
    CHECK_EQ(r.last_vdm_header(), 0x27170103u);
}

// Test Xiaomi auth command 0x27170203 -> reply 5-DO payload
static void test_xiaomi_cmd_03() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170203
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 3)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with 5-DO: 0x27170104, 0xA601934C, 0x1403BFD9, 0x55EBD77F, 0x509BA17F
    CHECK_EQ(r.sent.size() >= 1, true);
    const auto& m = r.sent.back();
    CHECK_EQ((m.header & 0x1F), kMsgVendorDefined);
    CHECK_EQ(m.len, 20);  // 5 * 4 bytes
    CHECK_EQ(r.last_vdm_header(), 0x27170104u);
    CHECK_EQ(r.last_vdo(1), 0xA601934Cu);
    CHECK_EQ(r.last_vdo(2), 0x1403BFD9u);
    CHECK_EQ(r.last_vdo(3), 0x55EBD77Fu);
    CHECK_EQ(r.last_vdo(4), 0x509BA17Fu);
}

// Test Xiaomi auth command 0x27170204 -> reply 5-DO payload
static void test_xiaomi_cmd_04() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170204
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 4)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with 5-DO: 0x27170105, 0x0BED65CD, 0xB2579250, 0x3AA3D437, 0x51EB9F51
    CHECK_EQ(r.sent.size() >= 1, true);
    const auto& m = r.sent.back();
    CHECK_EQ(m.len, 20);
    CHECK_EQ(r.last_vdm_header(), 0x27170105u);
    CHECK_EQ(r.last_vdo(1), 0x0BED65CDu);
    CHECK_EQ(r.last_vdo(2), 0xB2579250u);
    CHECK_EQ(r.last_vdo(3), 0x3AA3D437u);
    CHECK_EQ(r.last_vdo(4), 0x51EB9F51u);
}

// Test Xiaomi auth command 0x27170205 -> reply 2-DO payload
static void test_xiaomi_cmd_05() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170205
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 5)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with 2-DO: 0x27170106, 0x00000001
    CHECK_EQ(r.sent.size() >= 1, true);
    const auto& m = r.sent.back();
    CHECK_EQ(m.len, 8);
    CHECK_EQ(r.last_vdm_header(), 0x27170106u);
    CHECK_EQ(r.last_vdo(1), 0x00000001u);
}

// Test Xiaomi auth command 0x27170206 -> reply 2-DO payload
static void test_xiaomi_cmd_06() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170206
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 6)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with 2-DO: 0x27170107, 0x00000001
    CHECK_EQ(r.sent.size() >= 1, true);
    const auto& m = r.sent.back();
    CHECK_EQ(m.len, 8);
    CHECK_EQ(r.last_vdm_header(), 0x27170107u);
    CHECK_EQ(r.last_vdo(1), 0x00000001u);
}

// Test Xiaomi auth command 0x27170207 -> reply 5-DO payload
static void test_xiaomi_cmd_07() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170207
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 7)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with 5-DO: 0x27170108, 0x5D808AA6, 0x1D79FAA7, 0x11D10BDD, 0x8F158DE2
    CHECK_EQ(r.sent.size() >= 1, true);
    const auto& m = r.sent.back();
    CHECK_EQ(m.len, 20);
    CHECK_EQ(r.last_vdm_header(), 0x27170108u);
    CHECK_EQ(r.last_vdo(1), 0x5D808AA6u);
    CHECK_EQ(r.last_vdo(2), 0x1D79FAA7u);
    CHECK_EQ(r.last_vdo(3), 0x11D10BDDu);
    CHECK_EQ(r.last_vdo(4), 0x8F158DE2u);
}

// Test Xiaomi auth command 0x27170208 -> reply 2-DO payload
static void test_xiaomi_cmd_08() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x27170208
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 8)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should reply with 2-DO: 0x2717010A, 0x00000001
    // Note: cmd 0x08 -> response cmd 0x0A (cmd + 2 pattern)
    CHECK_EQ(r.sent.size() >= 1, true);
    const auto& m = r.sent.back();
    CHECK_EQ(m.len, 8);
    CHECK_EQ(r.last_vdm_header(), 0x2717010Au);
    CHECK_EQ(r.last_vdo(1), 0x00000001u);
}

// Test Xiaomi auth command 0x2717020A -> triggers Get_Source_Cap
static void test_xiaomi_cmd_0A() {
    Rig r;
    // First, mark that source has Xiaomi SVID
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Source sends Xiaomi cmd 0x2717020A
    constexpr std::array vdos2 = {vdm_header(0x2717, false, 0, 0x0A)};
    send_vdm(r.sink, vdos2, r.now);
    r.pump();
    
    // Should send Get_Source_Cap (control message 0x07)
    bool found = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kCtlGetSourceCap) {
            found = true;
            break;
        }
    }
    CHECK_EQ(found, true);
}

// Test that Discover SVIDs is sent once the contract settles (opt-in), paced ~220 ms after
// PS_RDY, on SOP, still only once.
static void test_discover_svids_sent_after_contract() {
    Rig r;
    r.sink.set_proactive_discover(true);
    CHECK_EQ(r.sink.trigger({1, 5000, 1000}, r.now), Err::Ok);
    r.pump();
    const std::uint8_t caps_data[4] = {0x2C, 0x91, 0x01, 0};
    r.sink.on_message(Sop::Sop, make_header(kDataSourceCap, 1, 2) | (1u << 8), caps_data, r.now);
    r.pump();
    // Contract settles: PS_RDY arms the paced Discover SVIDs REQ
    r.sent.clear();
    r.sink.on_message(Sop::Sop, make_header(kCtlAccept, 0, 2) | (1u << 8), {}, r.now);
    r.sink.on_message(Sop::Sop, make_header(kCtlPsRdy, 0, 2) | (1u << 8), {}, r.now);
    r.pump();
    // Paced: not immediately on PS_RDY
    CHECK_EQ(r.sent.empty(), true);
    r.run(300);
    bool found_svids = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kMsgVendorDefined) {
            CHECK_EQ(r.last_vdm_header(), 0xFF008002u);  // structured, REQ, cmd 2
            CHECK_EQ(int(m.sop), int(Sop::Sop));         // H4: the REQ goes on SOP, never SOP'
            CHECK_EQ(m.len, 4);
            found_svids = true;
        }
    }
    CHECK_EQ(found_svids, true);
    // Only once: a second PS_RDY does not repeat it
    r.sent.clear();
    r.sink.on_message(Sop::Sop, make_header(kCtlPsRdy, 0, 2) | (1u << 8), {}, r.now);
    r.run(500);
    for (const auto& m : r.sent) {
        CHECK_EQ((m.header & 0x1F) != kMsgVendorDefined || r.last_vdm_header() != 0xFF008002u || m.header != r.sent.back().header, true);
    }
    // Get_Sink_Cap still answers promptly (no pacing on the sink-cap path)
    r.sent.clear();
    r.run(1);
    r.sink.on_message(Sop::Sop, make_header(kCtlGetSinkCap, 0, 2) | (1u << 8), {}, r.now);
    r.pump();
    CHECK_EQ(r.last_type(), kDataSinkCap);
}

// Test full Xiaomi auth sequence
static void test_full_xiaomi_auth_sequence() {
    Rig r;
    // First trigger to set trigger_session_
    CHECK_EQ(r.sink.trigger({1, 5000, 1000}, r.now), Err::Ok);
    r.pump();
    r.sent.clear();
    
    // Step 1: Source sends Discover SVIDs ACK with 0x2717
    constexpr std::array vdos1 = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, vdos1, r.now);
    r.pump();
    r.sent.clear();
    
    // Step 2: Source sends Xiaomi commands in sequence
    const std::uint8_t cmds[] = {1, 2, 3, 4, 5, 6, 7, 8, 0x0A};
    for (std::uint8_t cmd : cmds) {
        std::array<std::uint32_t, 1> vdos = {vdm_header(0x2717, false, 0, cmd)};
        send_vdm(r.sink, vdos, r.now);
        r.pump();
    }
    
    // Check that Get_Source_Cap was sent after 0x2717020A
    bool found_get_cap = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kCtlGetSourceCap) {
            found_get_cap = true;
            break;
        }
    }
    CHECK_EQ(found_get_cap, true);
}

// Test the charger's Discover SVIDs REQ (0xFF008002) -> we reply with 0x2717
static void test_discover_svids_req_reply() {
    Rig r;
    // Source (charger) sends Discover SVIDs REQ: structured, cmd_type 0 (REQ), cmd 2
    constexpr std::array vdos = {vdm_header(0xFF00, true, 0, 2)};
    send_vdm(r.sink, vdos, r.now);
    r.pump();
    // We should reply with Discover SVIDs ACK listing 0x2717
    bool found = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kMsgVendorDefined) {
            CHECK_EQ(r.last_vdm_header() == 0xFF008042u, true);  // structured, ACK, cmd 2
            CHECK_EQ(r.last_vdo(1), 0x27170000u);
            found = true;
        }
    }
    CHECK_EQ(found, true);
    // After answering a 0x2717 SVID request the auth VDMs may follow: gate is open.
    constexpr std::array auth = {vdm_header(0x2717, false, 0, 1)};
    send_vdm(r.sink, auth, r.now);
    r.pump();
    bool replied = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kMsgVendorDefined && r.last_vdm_header() == kXiaomiStep1ReplyVdo) replied = true;
    }
    CHECK_EQ(replied, true);
}

// Test the charger's Discover Identity REQ -> we reply with an identity ACK
// H2: the Discover Identity ACK carries these record bytes (reverse-engineered):
// VDM header with bit 13 set, the Xiaomi ID header VDO, an empty cert-stat slot and the
// named product VDO (variants per capture).
static void test_discover_identity_req_reply() {
    Rig r;
    constexpr std::array vdos = {vdm_header(0xFF00, true, 0, 1)};
    send_vdm(r.sink, vdos, r.now);
    r.pump();
    bool found = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kMsgVendorDefined) {
            CHECK_EQ(r.last_vdm_header() == 0xFF00A041u, true);  // structured, ACK, cmd 1, bit 13 set
            CHECK_EQ(r.last_vdo(1), 0x05C02717u);                // ID header VDO (VID 0x2717)
            CHECK_EQ(r.last_vdo(2), 0x00000000u);                // Cert Stat: none
            CHECK_EQ(r.last_vdo(3), kXiaomiProductVdo);          // product VDO
            CHECK_EQ(m.len, 16);
            found = true;
        }
    }
    CHECK_EQ(found, true);
}

// Full hardware-shaped sequence: identity REQ, SVIDs REQ, then the auth VDMs (no prior
// trigger, no prior SVIDs from us) - the CUKTECH flow.
static void test_charger_initiated_flow() {
    Rig r;
    constexpr std::array id_req = {vdm_header(0xFF00, true, 0, 1)};
    send_vdm(r.sink, id_req, r.now);
    r.pump();
    r.sent.clear();
    constexpr std::array sv_req = {vdm_header(0xFF00, true, 0, 2)};
    send_vdm(r.sink, sv_req, r.now);
    r.pump();
    r.sent.clear();
    const std::uint8_t cmds[] = {1, 2, 3, 4, 5, 6, 7, 8, 0x0A};
    for (std::uint8_t cmd : cmds) {
        std::array<std::uint32_t, 1> vdos = {vdm_header(0x2717, false, 0, cmd)};
        send_vdm(r.sink, vdos, r.now);
        r.pump();
    }
    // After 0x2717020A: Get_Source_Cap queued
    bool found_get_cap = false;
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) == kCtlGetSourceCap) found_get_cap = true;
    }
    CHECK_EQ(found_get_cap, true);
}

// Bring the sink to Ready with a 5 V fixed contract (keep-alive stays off on fixed PDOs, so the
// Xiaomi VDM traffic below runs on a quiet line).
static void build_contract(Rig& r) {
    CHECK_EQ(r.sink.trigger({1, 5000, 1000}, r.now), Err::Ok);
    r.pump();
    r.ctl(kCtlAccept);
    r.ctl(kCtlPsRdy);
}

// Read VDO `idx` out of a sent message (0 while it does not reach that far).
static std::uint32_t vdo_of(const TxMsg& m, std::size_t idx) {
    if (idx * 4 + 3 >= m.len) return 0;
    return std::uint32_t(m.data[idx * 4 + 0]) | (std::uint32_t(m.data[idx * 4 + 1]) << 8) |
           (std::uint32_t(m.data[idx * 4 + 2]) << 16) | (std::uint32_t(m.data[idx * 4 + 3]) << 24);
}

// The opener message shape: 1-DO Vendor_Defined, DO 0x27170101.
static bool is_opener(const TxMsg& m) {
    if ((m.header & 0x1F) != kMsgVendorDefined) return false;
    return ((m.header >> 12) & 7) == 1 && m.len == 4 && vdo_of(m, 0) == 0x27170101u;
}

// True when any sent message is the Xiaomi chain opener: 1-DO Vendor_Defined, DO 0x27170101.
static bool has_opener(const std::vector<TxMsg>& sent) {
    for (const auto& m : sent)
        if (is_opener(m)) return true;
    return false;
}

// H1 (sink-initiated opener): the charger's Discover SVIDs ACK that lists 0x2717 is the cue -
// after a ~200 ms pacing delay open the chain with the 1-DO Vendor_Defined 0x27170101 on SOP,
// once per contract. The CUKTECH C1 sat in this exact silence on trace5: it waits for the sink.
static void test_opener_after_charger_svids_ack() {
    Rig r;
    build_contract(r);
    r.sent.clear();
    constexpr std::array ack = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};
    send_vdm(r.sink, ack, r.now);
    r.pump();
    // Paced: nothing for the first ~150 ms
    r.run(150);
    CHECK_EQ(has_opener(r.sent), false);
    // ~200 ms after the ACK the opener goes out, on SOP, as a 1-DO message
    r.run(150);
    CHECK_EQ(has_opener(r.sent), true);
    for (const auto& m : r.sent) {
        if ((m.header & 0x1F) != kMsgVendorDefined || m.len != 4 || vdo_of(m, 0) != 0x27170101u) continue;
        CHECK_EQ(int(m.sop), int(Sop::Sop));
        CHECK_EQ(unsigned((m.header >> 12) & 7), unsigned(1));
    }
    // Once per contract: another SVIDs ACK does not repeat the opener
    const std::size_t count = r.sent.size();
    send_vdm(r.sink, ack, r.now);
    r.run(500);
    CHECK_EQ(r.sent.size(), count);
    bool again = false;
    for (std::size_t i = count; i < r.sent.size(); i++) again = again || is_opener(r.sent[i]);
    CHECK_EQ(again, false);
}

// H1, charger-driven discovery flow: the charger asks for our SVIDs and we answer with the
// 0x2717 ACK - this also counts as "the chain can start" and the opener follows (trace6/7:
// the charger went silent after this flow with no opener ever sent).
static void test_opener_on_charger_svids_req_flow() {
    Rig r;
    build_contract(r);
    r.sent.clear();
    constexpr std::array req = {vdm_header(0xFF00, true, 0, 2)};
    send_vdm(r.sink, req, r.now);
    r.pump();
    CHECK_EQ(r.sent.size() >= 1, true);   // our SVIDs ACK went out first
    r.run(50);
    CHECK_EQ(has_opener(r.sent), false);  // none of the first ~250 ms
    r.run(500);
    CHECK_EQ(has_opener(r.sent), true);
}

// ---- chain guard: no sink-initiated Request while the Xiaomi VDM exchange runs ----
// Hardware 2026-11-06 (CUKTECH 120 W C1): a Request sent 35 ms after the last 0x2717 VDM made the charger drop the
// chain - no 0x2717020A, no Get_Source_Cap, no 6 A caps for the rest of the session.

static std::size_t count_type(const std::vector<TxMsg>& sent, std::uint8_t type, bool data) {
    std::size_t n = 0;
    for (const auto& m : sent) n += (m.header & 0x1F) == type && (((m.header >> 12) & 7) != 0) == data;
    return n;
}
static std::size_t count_requests(const std::vector<TxMsg>& sent) { return count_type(sent, kDataRequest, true); }
static unsigned request_pos(const std::vector<TxMsg>& sent) {
    for (const auto& m : sent)
        if ((m.header & 0x1F) == kDataRequest) return unsigned((vdo_of(m, 0) >> 28) & 7);
    return 0;
}

// A source that lists 0x2717 right after the contract: the chain is live and the opener follows.
static void start_chain(Rig& r) {
    build_contract(r);
    r.sent.clear();
    constexpr std::array ack = {vdm_header(0xFF00, true, 1, 2), 0x27170000u, 0x00000000u};   // the charger's SVIDs ACK lists 0x2717
    send_vdm(r.sink, ack, r.now);
    r.pump();
}

static void xiaomi_cmd(Rig& r, std::uint8_t cmd) {
    const std::array<std::uint32_t, 1> v = {vdm_header(0x2717, false, 0, cmd)};
    send_vdm(r.sink, v, r.now);
    r.pump();
}

// A source's own Discover SVIDs REQUEST is a question to us, not evidence about the source (Lenovo 65 W laptop brick,
// hardware 2026-11-08: it asks, never lists 0x2717, never runs a chain). We still answer [0x2717] (our sink identity) and
// the opener still goes out for the chargers that wait on it, but nothing treats the source as Xiaomi-family.
static void test_source_svids_req_alone_is_not_xiaomi() {
    Rig r;
    build_contract(r);
    r.sent.clear();
    constexpr std::array req = {vdm_header(0xFF00, true, 0, 2)};
    send_vdm(r.sink, req, r.now);
    r.pump();
    CHECK_EQ(r.last_vdm_header(), 0xFF008042u);          // answered, listing 0x2717
    CHECK_EQ(r.last_vdo(1), 0x27170000u);
    CHECK_EQ(r.sink.status().xiaomi_svid, false);
    CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
    r.run(500);
    CHECK_EQ(has_opener(r.sent), true);                  // kept: the source-driven Xiaomi chargers wait on it
    CHECK_EQ(r.sink.status().xiaomi_svid, false);
    CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
    // No Request is held for a chain window, not even inside the opener pacing delay
    Rig q;
    build_contract(q);
    q.sent.clear();
    send_vdm(q.sink, req, q.now);
    q.pump();
    q.sent.clear();
    CHECK_EQ(q.sink.trigger({2, 9000, 3000}, q.now), Err::Ok);
    q.pump();
    CHECK_EQ(count_requests(q.sent), std::size_t(1));    // out in the same pump
    CHECK_EQ(q.sink.xiaomi_trace().req_held, false);
    CHECK_EQ(q.sink.xiaomi_trace().req_in_chain, false);
}

// A source that lists 0x2717 in its SVIDs ACK is Xiaomi-family at once: flag, chain guard and opener as before.
static void test_source_svids_ack_listing_2717_is_xiaomi() {
    Rig r;
    start_chain(r);
    CHECK_EQ(r.sink.status().xiaomi_svid, true);
    CHECK_EQ(r.sink.xiaomi_chain_active(r.now), true);
    r.run(250);
    CHECK_EQ(has_opener(r.sent), true);
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
    r.pump();
    CHECK_EQ(count_requests(r.sent), std::size_t(0));    // held for the chain window
    CHECK_EQ(r.sink.xiaomi_trace().req_held, true);
}

// OPPO's SVID 0x22D9 in the source's Discover SVIDs ACK is recorded and nothing else: no Xiaomi chain, no opener, no VDM sent
// (the note is discovery only). Either half of a SVID word counts; a Hard Reset or a detach forgets it.
static void test_source_svids_ack_listing_22d9_is_recorded_and_never_answered() {
    for (unsigned half = 0; half < 2; ++half) {
        Rig r;
        build_contract(r);
        r.sent.clear();
        const std::array ack = {vdm_header(0xFF00, true, 1, 2), half ? 0x12340000u | 0x22D9u : 0x22D90000u, 0x00000000u};
        send_vdm(r.sink, ack, r.now);
        r.pump();
        r.run(500);
        CHECK_EQ(r.sink.status().oppo_svid, true);
        CHECK_EQ(r.sink.status().xiaomi_svid, false);
        CHECK_EQ(has_opener(r.sent), false);
        CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
        r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(r.sink.status().oppo_svid, false);
    }
    Rig q;
    start_chain(q);   // a Xiaomi list does not set it
    CHECK_EQ(q.sink.status().oppo_svid, false);
}

// Source-driven flow (traces 6/7): REQ first (not yet Xiaomi), then the charger's own 0x2717 steps. The first 0x2717 VDM
// is the evidence: the flag goes up there, the chain is answered and the auth completes as with an ACK-listed source.
static void test_source_driven_chain_marks_xiaomi_on_the_first_2717_vdm() {
    Rig r;
    build_contract(r);
    r.sent.clear();
    constexpr std::array req = {vdm_header(0xFF00, true, 0, 2)};
    send_vdm(r.sink, req, r.now);
    r.pump();
    r.run(250);
    CHECK_EQ(has_opener(r.sent), true);
    CHECK_EQ(r.sink.status().xiaomi_svid, false);
    xiaomi_cmd(r, 1);
    CHECK_EQ(r.sink.status().xiaomi_svid, true);
    CHECK_EQ(r.sink.xiaomi_chain_active(r.now), true);
    CHECK_EQ(r.last_vdm_header(), kXiaomiStep1ReplyVdo);   // answered
    for (std::uint8_t cmd = 2; cmd <= 8; cmd++) { r.run(30); xiaomi_cmd(r, cmd); }
    r.run(30);
    xiaomi_cmd(r, 0x0A);
    CHECK_EQ(r.sink.status().xiaomi_authed, true);
    CHECK_EQ(count_type(r.sent, kCtlGetSourceCap, false), std::size_t(1));
}

static void test_chain_defers_a_trigger_until_the_auth_completes() {
    Rig r;
    start_chain(r);
    r.run(250);                                          // the opener went out; the charger is about to start its VDMs
    CHECK_EQ(has_opener(r.sent), true);
    CHECK_EQ(r.sink.xiaomi_chain_active(r.now), true);
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);   // the Runner's row: queued, not yet on the wire
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
    r.pump();
    CHECK_EQ(count_requests(r.sent), std::size_t(0));
    for (std::uint8_t cmd = 1; cmd <= 8; cmd++) {
        r.run(30);
        xiaomi_cmd(r, cmd);
        CHECK_EQ(count_requests(r.sent), std::size_t(0));
        CHECK_EQ(r.sink.xiaomi_chain_active(r.now), true);
    }
    r.run(30);
    xiaomi_cmd(r, 0x0A);                                 // auth done: our Get_Source_Cap goes out, the Request still waits for the answer
    CHECK_EQ(count_type(r.sent, kCtlGetSourceCap, false), std::size_t(1));
    r.run(100);
    CHECK_EQ(count_requests(r.sent), std::size_t(0));
    r.data(kDataSourceCap, 0x0001912C);                  // the re-announce: the Request goes out now, planned on the new list
    CHECK_EQ(count_requests(r.sent), std::size_t(1));
    CHECK_EQ(request_pos(r.sent), 2u);
    CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
    CHECK_EQ(r.sink.status().hard_resets, std::uint32_t(0));
    CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
}

// The authed source that never answers our Get_Source_Cap: the Request is released after the caps wait.
static void test_chain_releases_after_a_silent_get_source_cap() {
    Rig r;
    start_chain(r);
    r.run(250);
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
    for (std::uint8_t cmd = 1; cmd <= 8; cmd++) { r.run(30); xiaomi_cmd(r, cmd); }
    r.run(30);
    xiaomi_cmd(r, 0x0A);
    r.run(kXiaomiCapsWaitMs - 100);
    CHECK_EQ(count_requests(r.sent), std::size_t(0));
    r.run(300);
    CHECK_EQ(count_requests(r.sent), std::size_t(1));
    CHECK_EQ(r.sink.status().hard_resets, std::uint32_t(0));
}

// A chain that keeps talking but never finishes: the Request goes out at the bound, never later.
static void test_chain_stalled_releases_the_request_at_the_bound() {
    Rig r;
    start_chain(r);
    const std::uint32_t t0 = r.now;
    r.run(250);
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
    while (r.now - t0 < kXiaomiChainBoundMs - 100) {
        r.run(200);
        xiaomi_cmd(r, 1);                                // an endless step 1: busy, never authed
        if (r.now - t0 < kXiaomiChainBoundMs - 100) CHECK_EQ(count_requests(r.sent), std::size_t(0));
        CHECK_EQ(r.sink.status().hard_resets, std::uint32_t(0));   // the deferred Request must not run into its own timeout
    }
    r.run(300);
    CHECK_EQ(count_requests(r.sent), std::size_t(1));
    CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
    CHECK_EQ(r.now - t0 <= kXiaomiChainBoundMs + 500, true);
}

// A chain that went quiet (the charger never started its VDMs): released kXiaomiChainQuietMs after the last exchange, not
// inside a CUKTECH-sized pause (~336 ms between two of its steps on hardware).
static void test_chain_quiet_releases_the_request() {
    Rig r;
    start_chain(r);
    const std::uint32_t t0 = r.now;
    r.run(250);                                          // opener out at ~t0+200
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
    r.run(340);                                          // a real chain's longest pause: still held
    CHECK_EQ(count_requests(r.sent), std::size_t(0));
    r.run(kXiaomiChainQuietMs);
    CHECK_EQ(count_requests(r.sent), std::size_t(1));
    CHECK_EQ(r.now - t0 < 250 + kXiaomiChainQuietMs + 500, true);
}

// A source that does not list 0x2717, or whose chain is over, is never delayed.
static void test_non_xiaomi_source_is_not_delayed() {
    {
        Rig r;
        build_contract(r);
        r.sent.clear();
        CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
        CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
        r.pump();
        CHECK_EQ(count_requests(r.sent), std::size_t(1));   // out in the same pump
    }
    {   // lists another vendor's SVID and talks VDMs: still no delay
        Rig r;
        build_contract(r);
        r.sent.clear();
        constexpr std::array ack = {vdm_header(0xFF00, true, 1, 2), 0x12D10000u, 0x00000000u};
        send_vdm(r.sink, ack, r.now);
        r.pump();
        CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
        CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
        r.pump();
        CHECK_EQ(count_requests(r.sent), std::size_t(1));
    }
    {   // a finished chain (authed, caps answered) lets the next row straight through
        Rig r;
        start_chain(r);
        r.run(250);
        for (std::uint8_t cmd = 1; cmd <= 8; cmd++) { r.run(30); xiaomi_cmd(r, cmd); }
        xiaomi_cmd(r, 0x0A);
        r.data(kDataSourceCap, 0x0001912C);
        r.sent.clear();
        r.run(100);
        CHECK_EQ(r.sink.status().xiaomi_authed, true);
        CHECK_EQ(r.sink.xiaomi_chain_active(r.now), false);
    }
}

// The source re-announces its caps while our Request waits: that is answered at once (the source's
// tSenderResponse would otherwise hard reset), and the deferral is gone.
static void test_reannounce_mid_chain_answers_the_deferred_request_at_once() {
    Rig r;
    start_chain(r);
    r.run(250);
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
    r.pump();
    CHECK_EQ(count_requests(r.sent), std::size_t(0));
    r.data(kDataSourceCap, 0x0001912C);
    CHECK_EQ(count_requests(r.sent), std::size_t(1));
    r.run(100);                                          // inside the Accept window: one Request, no second one, no reset
    CHECK_EQ(count_requests(r.sent), std::size_t(1));
    CHECK_EQ(r.sink.status().hard_resets, std::uint32_t(0));
}

// ---- the trace the charger test's hunt evidence reads (diagnostics only: nothing here changes what goes on the wire) ----

static void test_trace_follows_the_chain_the_request_and_the_answer() {
    Rig r;
    const XiaomiTrace& t = r.sink.xiaomi_trace();
    start_chain(r);                                               // the 5 V contract's own Request is Request number 1
    const unsigned n0 = t.req_n;
    CHECK_EQ(n0, 1u);
    r.run(250);
    CHECK_EQ(t.opener_sent, true); CHECK_EQ(t.authed, false); CHECK_EQ(unsigned(t.last_step), 0u);
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);   // the chain guard holds it back: nothing sent, the trace says so
    CHECK_EQ(t.req_held, true); CHECK_EQ(unsigned(t.req_n), n0);
    for (std::uint8_t cmd = 1; cmd <= 8; cmd++) { r.run(30); xiaomi_cmd(r, cmd); CHECK_EQ(unsigned(t.last_step), unsigned(cmd)); }
    r.run(30);
    xiaomi_cmd(r, 0x0A);
    CHECK_EQ(unsigned(t.last_step), 0x0Au); CHECK_EQ(t.authed, true); CHECK_EQ(t.authed_at, r.now); CHECK_EQ(t.reannounced, false);
    r.run(100);
    CHECK_EQ(unsigned(t.req_n), n0);                              // still waiting for the re-announce
    r.data(kDataSourceCap, 0x0001912C);                           // the re-announce: the Request goes out
    CHECK_EQ(t.reannounced, true); CHECK_EQ(t.reannounce_at, r.now);
    CHECK_EQ(unsigned(t.req_n), n0 + 1); CHECK_EQ(t.req_held, false); CHECK_EQ(t.req_at, r.now);
    CHECK_EQ(unsigned(t.req_pos), 2u); CHECK_EQ(unsigned(t.req_mv), 9000u); CHECK_EQ(unsigned(t.req_ma), 3000u);
    CHECK_EQ(t.req_pdo, r.caps.pdos[1].raw); CHECK_EQ(unsigned(t.req_caps), 6u); CHECK_EQ(int(t.req_type), int(PdoType::Fixed));
    CHECK_EQ(t.req_rdo, r.sink.plan().rdo); CHECK_EQ(t.req_in_chain, false);
    CHECK_EQ(int(t.answer), int(ReqAnswer::None));
    r.run(40);
    r.ctl(kCtlReject);
    CHECK_EQ(int(t.answer), int(ReqAnswer::Reject)); CHECK_EQ(t.answer_at - t.req_at, 40u);
    CHECK_EQ(t.authed, true);                                     // an answer does not touch the chain half
}

static void test_trace_records_accept_and_wait() {
    Rig r;
    const XiaomiTrace& t = r.sink.xiaomi_trace();
    build_contract(r);                                            // a source with no 0x2717: the Request half is recorded all the same
    CHECK_EQ(unsigned(t.req_n), 1u); CHECK_EQ(t.opener_sent, false); CHECK_EQ(t.authed, false); CHECK_EQ(t.req_in_chain, false);
    CHECK_EQ(int(t.answer), int(ReqAnswer::Accept));
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
    r.pump();
    CHECK_EQ(unsigned(t.req_n), 2u); CHECK_EQ(int(t.answer), int(ReqAnswer::None)); CHECK_EQ(t.answer_at, 0u);   // a new Request clears the answer
    r.run(20);
    r.ctl(kCtlWait);
    CHECK_EQ(int(t.answer), int(ReqAnswer::Wait)); CHECK_EQ(t.answer_at - t.req_at, 20u);
}

static void test_trace_marks_a_request_that_left_inside_the_chain() {
    Rig r;
    const XiaomiTrace& t = r.sink.xiaomi_trace();
    start_chain(r);
    const unsigned n0 = t.req_n;
    r.run(250);
    CHECK_EQ(r.sink.trigger({2, 9000, 3000}, r.now), Err::Ok);
    r.pump();
    CHECK_EQ(t.req_held, true);
    r.data(kDataSourceCap, 0x0001912C);                           // a re-announce mid-chain answers the held Request at once
    CHECK_EQ(unsigned(t.req_n), n0 + 1); CHECK_EQ(t.req_held, false); CHECK_EQ(t.req_in_chain, true); CHECK_EQ(t.reannounced, false);
}

static void test_trace_clears_on_detach_and_hard_reset() {
    for (int kind = 0; kind < 2; ++kind) {
        Rig r;
        const XiaomiTrace& t = r.sink.xiaomi_trace();
        start_chain(r);
        r.run(250);
        for (std::uint8_t cmd = 1; cmd <= 8; cmd++) { r.run(30); xiaomi_cmd(r, cmd); }
        xiaomi_cmd(r, 0x0A);
        r.data(kDataSourceCap, 0x0001912C);
        CHECK_EQ(t.authed, true);
        if (kind == 0) r.sink.detach(); else r.sink.on_hard_reset_rx(r.now);
        CHECK_EQ(t.authed, false); CHECK_EQ(t.opener_sent, false); CHECK_EQ(t.reannounced, false);
        CHECK_EQ(unsigned(t.last_step), 0u); CHECK_EQ(unsigned(t.req_n), 0u); CHECK_EQ(t.req_held, false);
    }
}

int main() {
    test_source_svids_ack_listing_22d9_is_recorded_and_never_answered();
    test_source_svids_req_alone_is_not_xiaomi();
    test_source_svids_ack_listing_2717_is_xiaomi();
    test_source_driven_chain_marks_xiaomi_on_the_first_2717_vdm();
    test_trace_follows_the_chain_the_request_and_the_answer();
    test_trace_records_accept_and_wait();
    test_trace_marks_a_request_that_left_inside_the_chain();
    test_trace_clears_on_detach_and_hard_reset();
    test_chain_defers_a_trigger_until_the_auth_completes();
    test_chain_releases_after_a_silent_get_source_cap();
    test_chain_stalled_releases_the_request_at_the_bound();
    test_chain_quiet_releases_the_request();
    test_non_xiaomi_source_is_not_delayed();
    test_reannounce_mid_chain_answers_the_deferred_request_at_once();
    test_opener_after_charger_svids_ack();
    test_opener_on_charger_svids_req_flow();
    test_charger_initiated_flow();
    test_discover_identity_req_reply();
    test_discover_svids_req_reply();
    test_no_xiaomi_svid_no_response();
    test_discover_svids_sent_after_contract();
    test_discover_svids_reply();
    test_xiaomi_cmd_01();
    test_xiaomi_cmd_02();
    test_xiaomi_cmd_03();
    test_xiaomi_cmd_04();
    test_xiaomi_cmd_05();
    test_xiaomi_cmd_06();
    test_xiaomi_cmd_07();
    test_xiaomi_cmd_08();
    test_xiaomi_cmd_0A();
    test_full_xiaomi_auth_sequence();
    return test_report("test_pd_sink_xiaomi");
}
