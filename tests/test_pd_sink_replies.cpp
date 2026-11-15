// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the sink's answers to the requests a source makes (docs/PD_SINK_REPLIES.md, one row per message): every SOP
// request gets its reply from the engine itself, in the pass that received it, in SPR and in EPR mode. Structured VDM requests
// the K2 does not support are NAKed, unsupported PD 3 requests get Not_Supported, PD 2.0 requests the revision defines get
// Reject, informational messages and replies get nothing, and a K2 that is not the line's only sink stays silent.
#include <initializer_list>
#include <vector>

#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_ext.hpp"
#include "k2/pd/pd_sink.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

// 5 V 3 A, 9 V 3 A, 15 V 3 A, 20 V 5 A, PPS 3.3-11 V 3 A, PPS 3.3-21 V 5 A (+ EPR 28 V fixed at 8, AVS 15-48 V at 9)
SourceCaps make_caps(bool with_epr = false, std::uint8_t spec_rev = 2) {
    SourceCaps c;
    c.valid = true;
    c.spec_rev = spec_rev;
    const std::uint32_t raw[] = {0x0001912C | (with_epr ? 1u << 23 : 0u), 0x0002D12C, 0x0004B12C, 0x000640C8, 0xC0DC2164, 0xC1A4213C};
    c.num_pdos = 6;
    for (int i = 0; i < 6; i++) c.pdos[i] = decode_pdo(raw[i]);
    if (with_epr) {
        c.epr = true;
        c.num_pdos = 9;
        c.pdos[7] = decode_pdo(0x0008C1F4);  // fixed 28 V 5 A
        c.pdos[8] = decode_pdo(0xD3C0968C);  // EPR AVS 15-48 V, 140 W
    }
    return c;
}

struct Rig {
    SourceCaps caps;
    Sink sink;
    std::uint32_t now = 1000;
    std::uint8_t wire = 2;   // the source's header revision (1 = PD 2.0, 2 = PD 3.x)
    std::vector<TxMsg> sent;
    explicit Rig(bool epr = false, std::uint8_t rev = 2) : caps(make_caps(epr, rev)), wire(rev) {
        sink.bind(&caps);
        sink.set_cap_mv(36000);
        sink.set_proactive_discover(false);
    }
    void pump() {
        TxMsg m;
        while (sink.take_tx(m)) { sent.push_back(m); sink.on_tx_result(true, now); }
    }
    void run(std::uint32_t ms) { for (std::uint32_t i = 0; i < ms; i += 10) { now += 10; sink.tick(now); pump(); } }
    std::uint16_t hdr(std::uint8_t type, unsigned n, bool ext = false) const {
        return std::uint16_t(make_header(type, std::uint8_t(n), wire, ext) | (1u << 8));   // source / DFP
    }
    void ctl(std::uint8_t type) { sink.on_message(Sop::Sop, hdr(type, 0), {}, now); pump(); }
    void words(std::uint8_t type, std::initializer_list<std::uint32_t> w) {
        std::uint8_t b[28] = {};
        unsigned i = 0;
        for (std::uint32_t v : w) { for (int k = 0; k < 4; k++) b[4 * i + k] = std::uint8_t(v >> (8 * k)); i++; }
        sink.on_message(Sop::Sop, hdr(type, i), std::span<const std::uint8_t>(b, 4 * i), now);
        pump();
    }
    void vdm(std::initializer_list<std::uint32_t> w) { words(kMsgVendorDefined, w); }
    // an extended message: extended header + data, padded to whole data objects
    void ext(std::uint8_t type, std::initializer_list<std::uint8_t> data, bool chunked = true, std::uint8_t chunk = 0,
             std::uint16_t size = 0) {
        std::uint8_t b[28] = {};
        const std::uint16_t sz = size ? size : std::uint16_t(data.size());
        const std::uint16_t eh = std::uint16_t(sz | (chunked ? 1u << 15 : 0u) | (unsigned(chunk) << 11));
        b[0] = std::uint8_t(eh); b[1] = std::uint8_t(eh >> 8);
        unsigned i = 2;
        for (std::uint8_t v : data) b[i++] = v;
        const unsigned n = (i + 3) / 4;
        sink.on_message(Sop::Sop, hdr(type, n, true), std::span<const std::uint8_t>(b, 4 * n), now);
        pump();
    }
    void accept_and_ready() { ctl(kCtlAccept); ctl(kCtlPsRdy); }
    // a contract at PDO 2 (SPR) or, with EPR, the EPR entry and a 28 V EPR contract at PDO 8
    void contract(bool epr = false) {
        if (!epr) { sink.trigger({2, 0, 0}, now); pump(); accept_and_ready(); return; }
        sink.trigger({8, 0, 0}, now); pump();
        words(kDataEprMode, {std::uint32_t(kEprAck) << 24});
        words(kDataEprMode, {std::uint32_t(kEprSucceeded) << 24});
        epr_caps(0, 1);
        accept_and_ready();
    }
    // EPR_Source_Capabilities chunk `chunk` of a list that spans `chunks` chunks (the engine decoded it into `caps`)
    void epr_caps(std::uint8_t chunk, std::uint8_t chunks) {
        caps.epr_partial = chunk + 1 < chunks;
        std::uint8_t b[28] = {};
        const std::uint16_t size = chunks == 1 ? 24 : 40;
        const std::uint16_t eh = std::uint16_t(size | (1u << 15) | (unsigned(chunk) << 11));
        b[0] = std::uint8_t(eh); b[1] = std::uint8_t(eh >> 8);
        sink.on_message(Sop::Sop, hdr(kExtEprSourceCaps, 7, true), b, now);
        pump();
    }
    std::size_t mark() const { return sent.size(); }
    unsigned type_at(std::size_t i) const { return sent[i].header & 0x1F; }
    bool ext_at(std::size_t i) const { return (sent[i].header >> 15) & 1; }
    std::uint32_t word_at(std::size_t i, unsigned w) const {
        const auto& d = sent[i].data;
        return std::uint32_t(d[4 * w]) | std::uint32_t(d[4 * w + 1]) << 8 | std::uint32_t(d[4 * w + 2]) << 16 | std::uint32_t(d[4 * w + 3]) << 24;
    }
    // exactly one message went out since `m`: a control message of `type`, at the source's revision
    void expect_ctl(std::size_t m, unsigned type) {
        CHECK_EQ(sent.size(), m + 1);
        if (sent.size() != m + 1) return;
        CHECK_EQ(type_at(m), type);
        CHECK_EQ(int(ext_at(m)), 0);
        CHECK_EQ((sent[m].header >> 12) & 7, 0u);
        CHECK_EQ((sent[m].header >> 6) & 3, unsigned(wire == 2 ? 2 : 1));
    }
    void expect_silence(std::size_t m) { CHECK_EQ(sent.size(), m); }
    // exactly one VDM went out since `m`: a 1-object NAK carrying `want`
    void expect_nak(std::size_t m, std::uint32_t want) {
        CHECK_EQ(sent.size(), m + 1);
        if (sent.size() != m + 1) return;
        CHECK_EQ(type_at(m), unsigned(kMsgVendorDefined));
        CHECK_EQ((sent[m].header >> 12) & 7, 1u);
        CHECK_EQ(word_at(m, 0), want);
    }
};

// ---- Structured VDM requests (USB PD R3.2 6.4.4.2): a responder that does not support a command NAKs it, echoing SVID,
// version, object position and command. The 23-of-25 unanswered Discover_Modes (SVID 0x05AC, hardware 2026-11-15) are this row.

void test_discover_modes_for_an_unlisted_svid_is_naked() {
    for (bool epr : {false, true}) {
        Rig r(epr);
        r.contract(epr);
        std::size_t m = r.mark();
        r.vdm({0x05ACA003u});                 // Discover Modes REQ, SVID 0x05AC (not armed)
        r.expect_nak(m, 0x05ACA083u);
        m = r.mark();
        r.vdm({0xFF01A003u});                 // DisplayPort
        r.expect_nak(m, 0xFF01A083u);
        m = r.mark();
        r.vdm({0x8087A003u});                 // Thunderbolt
        r.expect_nak(m, 0x8087A083u);
        m = r.mark();
        r.vdm({0x2717A003u});                 // Xiaomi, structured: the K2 lists 0x2717 but has no structured modes
        r.expect_nak(m, 0x2717A083u);
    }
}

void test_enter_exit_and_svid_specific_requests_are_naked() {
    for (bool epr : {false, true}) {
        Rig r(epr);
        r.contract(epr);
        std::size_t m = r.mark();
        r.vdm({0xFF01A104u, 0x00000001u});    // Enter Mode, object position 1
        r.expect_nak(m, 0xFF01A184u);
        m = r.mark();
        r.vdm({0xFF01A105u});                 // Exit Mode, object position 1
        r.expect_nak(m, 0xFF01A185u);
        m = r.mark();
        r.vdm({0xFF01A010u, 0x00000001u});    // DisplayPort Status Update (SVID-specific command 16)
        r.expect_nak(m, 0xFF01A090u);
        m = r.mark();
        r.vdm({0xFF00A007u});                 // a reserved command (7) is invalid: NAK
        r.expect_nak(m, 0xFF00A087u);
        m = r.mark();
        r.vdm({0x05ACA001u});                 // Discover Identity aimed at a vendor SVID: not ours to answer, NAK
        r.expect_nak(m, 0x05ACA081u);
    }
}

void test_attention_and_vdm_answers_get_nothing() {
    Rig r;
    r.contract();
    const std::size_t m = r.mark();
    r.vdm({0xFF01A106u, 0x00000001u});        // Attention: no response, ever
    r.vdm({0xFF01A043u, 0x00000C05u});        // an ACK (to a Discover Modes nobody sent): not a request
    r.vdm({0xFF01A083u});                     // NAK
    r.vdm({0xFF01A0C3u});                     // BUSY
    r.expect_silence(m);
}

void test_identity_and_svids_keep_their_acks() {
    Rig r;
    r.contract();
    std::size_t m = r.mark();
    r.vdm({0xFF00A001u});
    CHECK_EQ(r.sent.size(), m + 1);
    CHECK_EQ(r.word_at(m, 0), kXiaomiIdentityAckHeader);
    m = r.mark();
    r.vdm({0xFF00A002u});
    CHECK_EQ(r.sent.size(), m + 1);
    CHECK_EQ(r.word_at(m, 0) & 0xFFFF00FFu, 0xFF000042u);   // SVIDs ACK
}

void test_unstructured_vdm_of_a_foreign_svid() {
    {   // PD 3: Not_Supported (an unstructured VDM nobody here owns)
        Rig r;
        r.contract();
        const std::size_t m = r.mark();
        r.vdm({0x12340001u, 0xDEADBEEFu});
        r.expect_ctl(m, kCtlNotSupported);
    }
    {   // PD 2.0: an unrecognised unstructured VDM is ignored
        Rig r(false, 1);
        r.contract();
        const std::size_t m = r.mark();
        r.vdm({0x12340001u});
        r.expect_silence(m);
    }
}

void test_a_nak_never_displaces_a_request() {
    Rig r;
    r.sink.trigger({3, 0, 0}, r.now);           // the Request is queued, not yet taken by the driver
    const std::size_t m = r.mark();
    r.vdm({0x05ACA003u});
    CHECK_EQ(r.sent.size(), m + 2);
    if (r.sent.size() != m + 2) return;
    CHECK_EQ(r.type_at(m), unsigned(kMsgVendorDefined));   // the answer first (its sender's timer runs)
    CHECK_EQ(r.type_at(m + 1), unsigned(kDataRequest));
}

void test_not_the_sole_sink_stays_silent() {
    Rig r;
    r.contract();
    r.sink.set_gives_sink_cap(false);   // inline / PD-silent legacy session: another device's conversation
    const std::size_t m = r.mark();
    r.vdm({0x05ACA003u});
    r.vdm({0x12340001u});
    r.expect_silence(m);
}


// ---- Control requests (pd_reply.hpp refusal_for and the sink's own answers), in SPR and in EPR mode ----

void test_pd3_control_requests_each_get_their_answer() {
    for (bool epr : {false, true}) {
        Rig r(epr);
        r.contract(epr);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().epr_mode), int(epr));
        for (std::uint8_t t : {kCtlGetSourceCap, std::uint8_t(17), kCtlGetStatus, kCtlGetPpsStatus, std::uint8_t(21), std::uint8_t(23),
                               std::uint8_t(14), std::uint8_t(0), std::uint8_t(25), std::uint8_t(26), std::uint8_t(27), std::uint8_t(28),
                               std::uint8_t(29), std::uint8_t(30), std::uint8_t(31)}) {
            const std::size_t m = r.mark();
            r.ctl(t);   // Get_Source_Cap, _Ext, Get_Status, Get_PPS_Status, Get_Country_Codes, Get_Source_Info, Data_Reset, reserved
            r.expect_ctl(m, kCtlNotSupported);
        }
        std::size_t m = r.mark();
        r.ctl(kCtlGetRevision);
        CHECK_EQ(r.sent.size(), m + 1);
        if (r.sent.size() == m + 1) {
            CHECK_EQ(r.type_at(m), 12u);                   // Revision
            CHECK_EQ((r.sent[m].header >> 12) & 7, 1u);
            CHECK_EQ(r.word_at(m, 0), 0x32100000u);        // USB PD 3.2, version 1.0
        }
        m = r.mark();
        r.ctl(kCtlGetSinkCap);
        CHECK_EQ(r.sent.size(), m + 1);
        if (r.sent.size() == m + 1) CHECK_EQ(r.type_at(m), unsigned(kDataSinkCap));
        m = r.mark();
        r.ctl(kCtlGetSinkCapExt);
        CHECK_EQ(r.sent.size(), m + 1);
        if (r.sent.size() == m + 1) CHECK_EQ(r.type_at(m), unsigned(kExtSinkCapsExt));
        m = r.mark();
        r.ctl(kCtlDrSwap);
        r.expect_ctl(m, kCtlReject);
        m = r.mark();
        r.ctl(kCtlPrSwap);
        r.expect_ctl(m, kCtlReject);
        m = r.mark();
        r.ctl(kCtlFrSwap);
        r.expect_ctl(m, kCtlNotSupported);
        m = r.mark();
        for (std::uint8_t t : {std::uint8_t(2), std::uint8_t(5), std::uint8_t(15), kCtlNotSupported, kCtlGoodCrc}) r.ctl(t);
        r.expect_silence(m);   // GotoMin, Ping, Data_Reset_Complete, Not_Supported, GoodCRC: nothing to answer
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));   // and the contract stands through all of it
    }
}

void test_soft_reset_is_accepted_in_spr_and_epr() {
    for (bool epr : {false, true}) {
        Rig r(epr);
        r.contract(epr);
        const std::size_t m = r.mark();
        r.ctl(kCtlSoftReset);
        r.expect_ctl(m, kCtlAccept);
        if (r.sent.size() == m + 1) CHECK_EQ(int(r.sent[m].restart_id), 1);   // MessageID back to 0 with the Accept
        CHECK_EQ(int(r.sink.status().epr_mode), 0);
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Idle));             // waits for the source's caps
    }
}

void test_pd2_requests_get_reject_or_nothing() {
    Rig r(false, 1);
    r.contract();
    std::size_t m = r.mark();
    r.ctl(kCtlGetSourceCap);
    r.expect_ctl(m, kCtlReject);              // defined in PD 2.0: a sink-only port refuses with Reject
    m = r.mark();
    r.ctl(kCtlDrSwap);
    r.expect_ctl(m, kCtlReject);
    m = r.mark();
    for (std::uint8_t t : {kCtlGetStatus, kCtlGetRevision, kCtlFrSwap, std::uint8_t(14), std::uint8_t(25), std::uint8_t(31)}) r.ctl(t);
    r.words(7, {0x55530000u});                // Get_Country_Info: not a PD 2.0 type
    r.ext(3, {0x00});                         // no extended messages in PD 2.0
    r.expect_silence(m);
    m = r.mark();
    r.ctl(kCtlSoftReset);
    r.expect_ctl(m, kCtlAccept);
}

// ---- Data messages ----

void test_data_requests_and_notifications() {
    for (bool epr : {false, true}) {
        Rig r(epr);
        r.contract(epr);
        std::size_t m = r.mark();
        r.words(7, {0x55530000u});                // Get_Country_Info ("US")
        r.expect_ctl(m, kCtlNotSupported);
        m = r.mark();
        r.words(8, {0x00000000u});                // Enter_USB
        r.expect_ctl(m, kCtlNotSupported);
        m = r.mark();
        r.words(13, {0u});                        // reserved
        r.expect_ctl(m, kCtlNotSupported);
        m = r.mark();
        r.words(6, {0x02000000u});                // Alert: a notification (the sink may ask Get_Status; it need not)
        r.words(5, {0x00000000u});                // Battery_Status
        r.words(3, {0x50000000u});                // BIST (carrier mode is not supported: ignored)
        r.words(11, {0u});                        // Source_Info
        r.words(12, {0x32100000u});               // Revision
        r.words(kDataRequest, {0x2304B12Cu});     // a far sink's Request, Sink_Capabilities, EPR_Request: never ours to answer
        r.words(kDataSinkCap, {0x0001912Cu});
        r.words(kDataEprRequest, {0x8304B12Cu, 0x0008C1F4u});
        r.expect_silence(m);
    }
}

// ---- Extended messages ----

void test_extended_requests() {
    for (bool epr : {false, true}) {
        Rig r(epr);
        r.contract(epr);
        for (std::uint8_t t : {std::uint8_t(3), std::uint8_t(4), std::uint8_t(6), std::uint8_t(8), std::uint8_t(10), std::uint8_t(30),
                               std::uint8_t(19), std::uint8_t(0)}) {
            const std::size_t m = r.mark();
            r.ext(t, {0x00, 0x00});   // Get_Battery_Cap, Get_Battery_Status, Get_Manufacturer_Info, Security_Request,
            r.expect_ctl(m, kCtlNotSupported);   // Firmware_Update_Request, Vendor_Defined_Extended, reserved
        }
        std::size_t m = r.mark();
        r.ext(kExtControl, {kEprGetSinkCap, 0});
        CHECK_EQ(r.sent.size(), m + 1);
        if (r.sent.size() == m + 1) {
            CHECK_EQ(r.type_at(m), unsigned(kExtEprSinkCaps));
            CHECK_EQ(int(r.ext_at(m)), 1);
            const std::uint16_t eh = std::uint16_t(r.sent[m].data[0] | r.sent[m].data[1] << 8);
            CHECK_EQ(eh, std::uint16_t(0x8000 | 8));                  // chunked, chunk 0, two PDOs
            CHECK_EQ((r.sent[m].header >> 12) & 7, 3u);
            CHECK_EQ(r.sent[m].len, 12);
            std::uint32_t w0 = 0, w1 = 0;
            for (int i = 0; i < 4; i++) { w0 |= std::uint32_t(r.sent[m].data[2 + i]) << (8 * i); w1 |= std::uint32_t(r.sent[m].data[6 + i]) << (8 * i); }
            CHECK_EQ(w0, kSinkCapPdos[0]);
            CHECK_EQ(w1, kSinkCapPdos[1]);
        }
        m = r.mark();
        r.ext(kExtControl, {kEprGetSourceCap, 0});                  // a sink-only port has no EPR source caps
        r.expect_ctl(m, kCtlNotSupported);
        m = r.mark();
        r.ext(kExtControl, {kEprKeepAlive, 0});                     // the sink's own message, sent to us
        r.expect_ctl(m, kCtlNotSupported);
        m = r.mark();
        r.ext(kExtControl, {4, 0});                                 // EPR_KeepAlive_Ack: a reply
        for (std::uint8_t t : {std::uint8_t(1), std::uint8_t(2), std::uint8_t(5), std::uint8_t(7), std::uint8_t(13), std::uint8_t(14),
                               std::uint8_t(15), std::uint8_t(18)})
            r.ext(t, {0x00, 0x00, 0x00, 0x00});                     // Source_Caps_Ext, Status, Battery_Caps, ... replies
        r.expect_silence(m);
    }
}

void test_a_chunked_unsupported_request_is_read_then_refused() {
    Rig r;
    r.contract();
    std::size_t m = r.mark();
    r.ext(8, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26}, true, 0, 40);
    CHECK_EQ(r.sent.size(), m + 1);            // Security_Request, 40 bytes: the second chunk is asked for first
    if (r.sent.size() == m + 1) {
        CHECK_EQ(r.type_at(m), 8u);
        CHECK_EQ(int(r.ext_at(m)), 1);
        const std::uint16_t eh = std::uint16_t(r.sent[m].data[0] | r.sent[m].data[1] << 8);
        CHECK_EQ(eh, std::uint16_t((1u << 15) | (1u << 10) | (1u << 11)));   // chunked, request, chunk 1
    }
    m = r.mark();
    r.ext(8, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14}, true, 1, 40);
    r.expect_ctl(m, kCtlNotSupported);          // the whole message is in: refused
    m = r.mark();
    {   // a chunk request aimed at us (our messages are all one chunk): ignored
        std::uint8_t b[4] = {};
        const std::uint16_t eh = std::uint16_t((1u << 15) | (1u << 10) | (1u << 11));
        b[0] = std::uint8_t(eh); b[1] = std::uint8_t(eh >> 8);
        r.sink.on_message(Sop::Sop, r.hdr(kExtSinkCapsExt, 1, true), b, r.now);
        r.pump();
    }
    r.expect_silence(m);
    m = r.mark();
    r.ext(3, {0x00}, false);                    // an unchunked extended message (never negotiated): refused
    r.expect_ctl(m, kCtlNotSupported);
}

void test_no_answers_when_not_the_sole_sink() {
    for (bool epr : {false, true}) {
        Rig r(epr);
        r.contract(epr);
        r.sink.set_gives_sink_cap(false);
        const std::size_t m = r.mark();
        r.ctl(kCtlGetStatus);
        r.ctl(kCtlGetRevision);
        r.ctl(kCtlGetSourceCap);
        r.ctl(kCtlDrSwap);
        r.words(7, {0x55530000u});
        r.ext(kExtControl, {kEprGetSinkCap, 0});
        r.ext(3, {0x00});
        r.expect_silence(m);
    }
}


// ---- The EPR re-announce (USB PD R3.2 8.3.3.x: in EPR mode the source re-sends EPR_Source_Capabilities and starts its
// SenderResponseTimer): the engine answers it with an EPR_Request in the pass that read the last chunk, like the SPR re-announce.
// Before, nothing answered it and the runner's release happened to go out 14.8 ms later (streamA6, 2026-11-09).

void test_an_epr_reannounce_is_answered_with_the_held_level() {
    for (std::uint8_t chunks : {std::uint8_t(1), std::uint8_t(2)}) {
        Rig r(true);
        r.contract(true);
        CHECK_EQ(int(r.sink.status().held), 1);
        std::size_t m = r.mark();
        r.epr_caps(0, chunks);
        if (chunks == 2) {   // chunk 0 of 2: the chunk request, no Request yet
            CHECK_EQ(r.sent.size(), m + 1);
            if (r.sent.size() == m + 1) CHECK_EQ(r.type_at(m), unsigned(kExtEprSourceCaps));
            m = r.mark();
            r.epr_caps(1, chunks);
        }
        CHECK_EQ(r.sent.size(), m + 1);
        if (r.sent.size() != m + 1) continue;
        CHECK_EQ(r.type_at(m), unsigned(kDataEprRequest));
        CHECK_EQ(r.word_at(m, 0) >> 28, 8u);                    // the held 28 V, found again in the new list
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Requesting));
        r.accept_and_ready();
        CHECK_EQ(int(r.sink.status().state), int(SinkState::Ready));
        CHECK_EQ(int(r.sink.status().held), 1);
        CHECK_EQ(int(r.sink.status().epr_mode), 1);
    }
}

void test_an_epr_reannounce_without_the_held_level_falls_back_to_pdo1() {
    Rig r(true);
    r.contract(true);
    r.caps.num_pdos = 6;   // the new list lost the 28 V (and the AVS)
    const std::size_t m = r.mark();
    r.epr_caps(0, 1);
    CHECK_EQ(r.sent.size(), m + 1);
    if (r.sent.size() != m + 1) return;
    CHECK_EQ(r.type_at(m), unsigned(kDataEprRequest));
    CHECK_EQ(r.word_at(m, 0) >> 28, 1u);                        // PDO 1, the hold is reported lost
}

}  // namespace

int main() {
    test_discover_modes_for_an_unlisted_svid_is_naked();
    test_enter_exit_and_svid_specific_requests_are_naked();
    test_attention_and_vdm_answers_get_nothing();
    test_identity_and_svids_keep_their_acks();
    test_unstructured_vdm_of_a_foreign_svid();
    test_a_nak_never_displaces_a_request();
    test_not_the_sole_sink_stays_silent();
    test_pd3_control_requests_each_get_their_answer();
    test_soft_reset_is_accepted_in_spr_and_epr();
    test_pd2_requests_get_reject_or_nothing();
    test_data_requests_and_notifications();
    test_extended_requests();
    test_a_chunked_unsupported_request_is_read_then_refused();
    test_no_answers_when_not_the_sole_sink();
    test_an_epr_reannounce_is_answered_with_the_held_level();
    test_an_epr_reannounce_without_the_held_level_falls_back_to_pdo1();
    return test_report("pd_sink_replies");
}
