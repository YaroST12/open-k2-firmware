// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for k2::pd::Engine: EPR_Source_Capabilities (extended, chunked) reassembly and
// EPR PDO decoding. Byte vectors are built by hand from USB PD 3.1 (6.4.1.3.1 fixed, 6.4.1.3.4
// EPR AVS, 6.2.1.2 extended header) and cross-checked with a separate script.
#include "k2/pd/pd_engine.hpp"
#include "test_util.h"

using namespace k2::pd;

static void put_le32(uint8_t* dst, uint32_t v) {
    for (int i = 0; i < 4; i++) dst[i] = (uint8_t)(v >> (8 * i));
}

// PDOs of a 240 W EPR source: 5/9/15/20 V fixed, PPS 3.3-21 V, two unused SPR slots, then
// EPR fixed 28/36/48 V and EPR AVS 15-48 V 240 W.
static const uint32_t kEpr[11] = {
    0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC1A42164, 0, 0,
    0x0008C1F4, 0x000B41F4, 0x000F01F4, 0xD3C096F0,
};

// Two chunks of a 44-byte EPR_Source_Capabilities, as they appear after the SOP token
// (header, extended header, payload). Header 0x91xx: extended, 7 / 5 data objects, PD3.x,
// type 0x11. Extended header: chunked, chunk number, data size 44.
static const uint8_t kChunk0[] = {0x91, 0xF0, 0x2C, 0x80, 0x2C, 0x91, 0x01, 0x00, 0x2C, 0xD1,
                                  0x02, 0x00, 0x2C, 0xB1, 0x04, 0x00, 0xF4, 0x41, 0x06, 0x00,
                                  0x64, 0x21, 0xA4, 0xC1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t kChunk1[] = {0x91, 0xD0, 0x2C, 0x88, 0x00, 0x00, 0xF4, 0xC1, 0x08, 0x00,
                                  0xF4, 0x41, 0x0B, 0x00, 0xF4, 0x01, 0x0F, 0x00, 0xF0, 0x96,
                                  0xC0, 0xD3};

static uint16_t hdr(const uint8_t* m) { return (uint16_t)(m[0] | (m[1] << 8)); }

static void test_avs_decode() {
    Pdo p = decode_pdo(kEpr[10]);
    CHECK_EQ((int)p.type, (int)PdoType::Avs);
    CHECK_EQ(p.min_mv, 15000);
    CHECK_EQ(p.max_mv, 48000);
    CHECK_EQ(p.max_mw, 240000);
    CHECK_EQ(p.max_ma, 5000);
    p = decode_pdo(kEpr[9]);
    CHECK_EQ((int)p.type, (int)PdoType::Fixed);
    CHECK_EQ(p.max_mv, 48000);
    CHECK_EQ(p.max_ma, 5000);
    CHECK_EQ(p.max_mw, 240000);
    CHECK_EQ(decode_pdo(kEpr[7]).max_mv, 28000);
    CHECK_EQ(decode_pdo(kEpr[8]).max_mv, 36000);
}

static void test_ext_header() {
    const ExtHeader h = decode_ext_header(0x8000 | (1u << 11) | 44);
    CHECK_EQ(h.chunked, true);
    CHECK_EQ(h.chunk_number, 1);
    CHECK_EQ(h.request, false);
    CHECK_EQ(h.data_size, 44);
    const ExtHeader r = decode_ext_header(0x8000 | (1u << 10) | (1u << 11));
    CHECK_EQ(r.request, true);
}

static void test_epr_two_chunks() {
    Engine e;
    e.on_message(Sop::Sop, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 1);
    CHECK_EQ(e.caps.valid, true);
    CHECK_EQ(e.caps.epr, true);
    CHECK_EQ(e.caps.epr_partial, true);          // chunk 1 not seen yet
    CHECK_EQ(e.caps.num_pdos, 6);                // 26 bytes carry 6 whole PDOs; the 7th straddles
    CHECK_EQ(e.caps.pdos[0].max_mv, 5000);
    CHECK_EQ(e.caps.pdos[4].max_mv, 21000);
    CHECK_EQ((int)e.caps.pdos[5].type, (int)PdoType::Unknown);
    e.on_message(Sop::Sop, hdr(kChunk1), {kChunk1 + 2, sizeof kChunk1 - 2}, 1);
    CHECK_EQ(e.caps.epr_partial, false);
    CHECK_EQ(e.caps.num_pdos, 11);
    for (int i = 0; i < 11; i++) CHECK_EQ(e.caps.pdos[i].raw, kEpr[i]);
    CHECK_EQ((int)e.caps.pdos[10].type, (int)PdoType::Avs);
    CHECK_EQ(e.caps.pdos[10].max_mv, 48000);
    CHECK_EQ(e.caps.spec_rev, 2);
    CHECK_EQ(e.caps.rx_count, 1);                // one message, not one per chunk
}

static void test_epr_chunk_order_and_replay() {
    Engine e;
    e.on_message(Sop::Sop, hdr(kChunk1), {kChunk1 + 2, sizeof kChunk1 - 2}, 2);
    CHECK_EQ(e.caps.valid, false);               // chunk 1 alone is not decodable
    e.on_message(Sop::Sop, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 2);
    CHECK_EQ(e.caps.epr_partial, true);          // chunk 0 starts a new message
    e.on_message(Sop::Sop, hdr(kChunk1), {kChunk1 + 2, sizeof kChunk1 - 2}, 2);
    CHECK_EQ(e.caps.epr_partial, false);
    CHECK_EQ(e.caps.num_pdos, 11);
    e.on_message(Sop::Sop, hdr(kChunk1), {kChunk1 + 2, sizeof kChunk1 - 2}, 2);
    CHECK_EQ(e.caps.num_pdos, 11);               // a replayed chunk 1 changes nothing
    e.on_message(Sop::Sop, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 2);
    CHECK_EQ(e.caps.epr_partial, true);          // new message: chunk 1 forgotten
    CHECK_EQ(e.caps.num_pdos, 6);
}

// An EPR_Source_Capabilities we asked for in SPR mode (EPR_Get_Source_Cap, no entry) is informational: it is decoded into
// `epr_info` and never touches the live SPR list, so nothing treats the source as already in EPR mode.
static void test_epr_info_only() {
    const uint32_t spr = 0x0881912C;
    uint8_t pl[4];
    put_le32(pl, spr);
    Engine e;
    e.on_message(Sop::Sop, 0x01 | (1u << 12) | (2u << 6), pl, 1);
    e.epr_info_only = true;
    e.on_message(Sop::Sop, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 1);
    CHECK_EQ(e.epr_info.valid, true);
    CHECK_EQ(e.epr_info.epr, true);
    CHECK_EQ(e.epr_info.epr_partial, true);
    CHECK_EQ(e.caps.epr, false);                 // the live list is untouched
    CHECK_EQ(e.caps.num_pdos, 1);
    CHECK_EQ(e.caps.pdos[0].raw, spr);
    CHECK_EQ(e.caps.rx_count, 1);
    e.on_message(Sop::Sop, hdr(kChunk1), {kChunk1 + 2, sizeof kChunk1 - 2}, 1);
    CHECK_EQ(e.epr_info.epr_partial, false);
    CHECK_EQ(e.epr_info.num_pdos, 11);
    CHECK_EQ(e.epr_info.pdos[10].max_mv, 48000);
    CHECK_EQ(e.caps.epr, false);
    CHECK_EQ(e.caps.num_pdos, 1);
    e.epr_info_only = false;                     // the glue clears it once the sink is no longer asking
    e.on_message(Sop::Sop, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 1);
    CHECK_EQ(e.caps.epr, true);                  // a list nobody asked for: the sniffer view, as before
    e.reset_source();
    CHECK_EQ(e.epr_info.valid, false);
}

static void test_spr_clears_epr() {
    Engine e;
    e.on_message(Sop::Sop, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 1);
    uint8_t pl[4];
    put_le32(pl, kEpr[0]);
    const uint16_t h = 0x01 | (1u << 12) | (2u << 6);  // Source_Capabilities, 1 PDO, PD 3.0
    e.on_message(Sop::Sop, h, pl, 1);
    CHECK_EQ(e.caps.epr, false);
    CHECK_EQ(e.caps.num_pdos, 1);
    CHECK_EQ(e.caps.spec_rev, 2);
    CHECK_EQ((int)e.caps.pdos[7].type, (int)PdoType::Unknown);  // stale EPR rows gone
}

// A charger that re-announces a longer list after the first one (the X865 case: 6 PDOs, then the SPR AVS
// APDO as a 7th): the store takes the new list whole, nothing of the old one is kept or doubled.
static void test_reannounce_longer_list() {
    const uint32_t six[6] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641A4, 0xC0DC2384, 0xC1A4213C};
    const uint32_t avs = 0xE0000000u | (300u << 10) | 325u;   // SPR AVS: 9-15 V 3 A, 15-20 V 3.25 A
    uint8_t pl[28];
    Engine e;
    for (int i = 0; i < 6; i++) put_le32(pl + 4 * i, six[i]);
    e.on_message(Sop::Sop, 0x01 | (6u << 12) | (2u << 6), {pl, 24}, 1);
    CHECK_EQ(e.caps.num_pdos, 6);
    CHECK_EQ(e.caps.valid, true);
    for (int i = 0; i < 6; i++) CHECK_EQ(e.caps.pdos[i].raw, six[i]);
    CHECK_EQ((int)e.caps.pdos[6].type, (int)PdoType::Unknown);
    put_le32(pl + 24, avs);
    e.on_message(Sop::Sop, 0x01 | (7u << 12) | (2u << 6) | (1u << 9), {pl, 28}, 2);
    CHECK_EQ(e.caps.num_pdos, 7);
    for (int i = 0; i < 6; i++) CHECK_EQ(e.caps.pdos[i].raw, six[i]);
    CHECK_EQ((int)e.caps.pdos[6].type, (int)PdoType::Avs);
    CHECK_EQ(e.caps.pdos[6].raw, avs);
    CHECK_EQ(e.caps.pdos[6].min_mv, 9000);
    CHECK_EQ(e.caps.pdos[6].max_mv, 20000);
    CHECK_EQ(e.caps.pdos[6].max_ma, 3250);
    CHECK_EQ(current_limit_ma(e.caps.pdos[6], 12000), 3000);
    CHECK_EQ(current_limit_ma(e.caps.pdos[6], 20000), 3250);
    // and back to the short list when the charger withdraws it
    e.on_message(Sop::Sop, 0x01 | (6u << 12) | (2u << 6) | (2u << 9), {pl, 24}, 3);
    CHECK_EQ(e.caps.num_pdos, 6);
    CHECK_EQ((int)e.caps.pdos[6].type, (int)PdoType::Unknown);
}

// A 1-PDO Source_Capabilities whose halfwords are known receiver noise is dropped: 0x639C, 0xC7CE, 0x27F6.
static void test_spurious_frame_filter() {
    const uint16_t h = 0x01 | (1u << 12) | (2u << 6);
    for (uint32_t noise : {0x0000639Cu, 0xC7CE0000u, 0x27F6912Cu}) {
        Engine e;
        uint8_t pl[4];
        put_le32(pl, noise);
        e.on_message(Sop::Sop, h, pl, 1);
        CHECK_EQ(e.caps.valid, false);
    }
    Engine e;
    uint8_t pl[4];
    put_le32(pl, kEpr[0]);
    e.on_message(Sop::Sop, h, pl, 1);
    CHECK_EQ(e.caps.valid, true);
}

static void test_ignores_other_sop() {
    Engine e;
    e.on_message(Sop::SopP, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 1);
    CHECK_EQ(e.caps.valid, false);
    e.on_message(Sop::SopPP, 0x1041, {}, 1);  /* control message on SOP'': nothing to decode */
    CHECK_EQ(e.cable.valid, false);
}

/* Discover Identity ACK from a cable. */
static const uint32_t kCableAck[5] = { 0xFF00A041u, (3u << 27) | 0x05ACu, 0, 0x12340005u,
                                       (3u << 9) | (2u << 5) | 2u };

static void feed_vdm(Engine& e, Sop sop, const uint32_t* v, unsigned n, uint16_t rev = 2,
                     bool src = false) {
    uint8_t pl[28] = {0};
    for (unsigned i = 0; i < n; i++) put_le32(pl + 4 * i, v[i]);
    uint16_t h = (uint16_t)(0x0F | (rev << 6) | (n << 12) | (src ? 0x100 : 0));  // bit 8: sender's power role
    e.on_message(sop, h, {pl, n * 4u}, 1);
}

// A control message on SOP (no data objects), rev 2; `src` sets the sender's power-role bit 8.
static void feed_ctl(Engine& e, uint16_t type, bool src = false) {
    const uint16_t h = (uint16_t)(type | (2u << 6) | (src ? 0x100 : 0));
    e.on_message(Sop::Sop, h, {}, 1);
}

// Discover Identity ACK of the dock from the recorded trace: VID 0x2109, PID 0x0103.
static const uint32_t kDockId[5] = {0xFF00A041u, (1u << 27) | 0x2109u, 0, 0x0103u << 16, 0};

/* PR_Swap, recorded dock sequence: the snk/ufp (header bit 8 clear) answers Discover Identity /
   SVIDs, so sink_vdm holds the dock (VID 0x2109, SVIDs [0xFF01]). PR_Swap + Accept + PS_RDY exchange
   the power roles, so the per-side records swap: vdm (source side) holds the dock, and sink_vdm is
   unknown again because the new sink never answered an identity request. */
static void test_pr_swap_swaps_side_records() {
    Engine e;
    feed_vdm(e, Sop::Sop, kDockId, 5);
    CHECK_EQ(e.sink_vdm.identity.valid, true);
    CHECK_EQ(e.sink_vdm.identity.vid, 0x2109);
    CHECK_EQ(e.sink_vdm.identity.pid, 0x0103);
    CHECK_EQ(e.vdm.identity.valid, false);
    const uint32_t svids[2] = {0xFF00A042u, 0xFF010000u};
    feed_vdm(e, Sop::Sop, svids, 2);
    CHECK_EQ(e.sink_vdm.num_svids, 1);
    CHECK_EQ(e.sink_vdm.svids[0], 0xFF01);
    feed_ctl(e, 0x0A);                        // PR_Swap (control type 0x0A) from the dock
    feed_ctl(e, 0x03, true);                  // Accept from the laptop
    CHECK_EQ(e.vdm.identity.valid, false);    // not yet: PS_RDY completes the swap
    CHECK_EQ(e.sink_vdm.identity.valid, true);
    feed_ctl(e, 0x06, true);                  // PS_RDY: the power roles have exchanged
    CHECK_EQ(e.vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.vid, 0x2109);
    CHECK_EQ(e.vdm.num_svids, 1);
    CHECK_EQ(e.vdm.svids[0], 0xFF01);
    CHECK_EQ(e.sink_vdm.identity.valid, false);  // the new sink never answered: unknown
    CHECK_EQ(e.sink_vdm.num_svids, 0);
}

/* Both partners answered identity before the swap: each keeps its own data under its NEW role
   (the side that changed role keeps its data; nothing is cleared). When the new sink then answers
   an identity request, sink_vdm holds that answer. */
static void test_pr_swap_keeps_each_side_and_new_sink_answer() {
    Engine e;
    const uint32_t laptop[5] = {0xFF00A041u, (2u << 27) | 0x18D1u, 0, 0x1234u << 16, 0};
    feed_vdm(e, Sop::Sop, laptop, 5, 2, true);   // src/dfp (bit 8 set) answered first
    CHECK_EQ(e.vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.vid, 0x18D1);
    feed_vdm(e, Sop::Sop, kDockId, 5);           // snk/ufp answered too
    CHECK_EQ(e.sink_vdm.identity.vid, 0x2109);
    feed_ctl(e, 0x0A, true);                     // PR_Swap from either side
    feed_ctl(e, 0x03);
    feed_ctl(e, 0x06);
    CHECK_EQ(e.vdm.identity.vid, 0x2109);       // dock is now the source: its data moved to vdm
    CHECK_EQ(e.sink_vdm.identity.vid, 0x18D1);   // laptop is now the sink: its data moved to sink_vdm
    const uint32_t phone[5] = {0xFF00A041u, (2u << 27) | 0x05ACu, 0, 0x7510u << 16, 0};
    feed_vdm(e, Sop::Sop, phone, 5);             // the new sink (bit 8 clear) answers identity
    CHECK_EQ(e.sink_vdm.identity.vid, 0x05AC);
    CHECK_EQ(e.sink_vdm.identity.pid, 0x7510);
    CHECK_EQ(e.vdm.identity.vid, 0x2109);       // the source side is untouched
}

/* A rejected or incomplete PR_Swap (Reject / Wait, or PS_RDY without Accept) must not move the
   per-side records. */
static void test_pr_swap_rejected_or_incomplete_keeps_records() {
    Engine e;
    feed_vdm(e, Sop::Sop, kDockId, 5);
    feed_ctl(e, 0x0A);
    feed_ctl(e, 0x04);                        // Reject: no swap
    CHECK_EQ(e.sink_vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.valid, false);
    feed_ctl(e, 0x0A);
    feed_ctl(e, 0x0C);                        // Wait: no swap either
    CHECK_EQ(e.sink_vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.valid, false);
    feed_ctl(e, 0x0A);
    feed_ctl(e, 0x03);
    feed_ctl(e, 0x03);                        // a second Accept still is not a completion
    CHECK_EQ(e.sink_vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.valid, false);
    feed_ctl(e, 0x0A);
    feed_ctl(e, 0x06);                        // PS_RDY without Accept: no swap
    CHECK_EQ(e.sink_vdm.identity.valid, true);
    CHECK_EQ(e.vdm.identity.valid, false);
    feed_ctl(e, 0x0A);                        // now it completes
    feed_ctl(e, 0x03);
    feed_ctl(e, 0x06);
    CHECK_EQ(e.vdm.identity.vid, 0x2109);
    CHECK_EQ(e.sink_vdm.identity.valid, false);
}

static void test_cable_sop_prime_and_double_prime() {
    for (Sop s : {Sop::SopP, Sop::SopPP}) {
        Engine e;
        feed_vdm(e, s, kCableAck, 5);
        CHECK_EQ(e.cable.valid, true);
        CHECK_EQ(e.cable.sop, s == Sop::SopP ? 1 : 2);
        CHECK_EQ(e.cable.max_current_a, 5);
        CHECK_EQ(e.cable.max_voltage_v, 50);
        CHECK_EQ(e.cable.spec_rev, 2);
        CHECK_EQ(e.vdm.identity.valid, false);  /* SOP identity stays separate */
        CHECK_EQ(e.caps.valid, false);
    }
    Engine e;  /* a PD 2.0 cable answer */
    feed_vdm(e, Sop::SopP, kCableAck, 5, 1);
    CHECK_EQ(e.cable.spec_rev, 1);
}

static void test_cable_ignores_requests_and_nak() {
    Engine e;
    uint32_t req[5];
    for (int i = 0; i < 5; i++) req[i] = kCableAck[i];
    for (uint32_t h : {0xFF00A001u, 0xFF00A081u, 0xFF00A0C1u}) {  /* REQ, NAK, BUSY */
        req[0] = h;
        feed_vdm(e, Sop::SopP, req, 5);
        CHECK_EQ(e.cable.valid, false);
    }
}

static void test_cable_svids_and_modes() {
    Engine e;
    uint32_t svids[2] = { 0xFF00A042u, 0x80870000u };
    feed_vdm(e, Sop::SopP, svids, 2);
    CHECK_EQ(e.cable_vdm.num_svids, 1);
    CHECK_EQ(e.cable_vdm.svids[0], 0x8087);
    CHECK_EQ(e.cable_thunderbolt(), true);
    CHECK_EQ(e.vdm.num_svids, 0);
    uint32_t modes[2] = { 0x8087A043u, 0x00CC0001u };
    feed_vdm(e, Sop::SopPP, modes, 2);
    CHECK_EQ(e.cable_vdm.num_modes, 1);
    CHECK_EQ(e.cable_vdm.modes[0], 0x00CC0001u);
    e.reset_cable();
    CHECK_EQ(e.cable_vdm.num_svids, 0);
    CHECK_EQ(e.cable_thunderbolt(), false);
}

/* Apple cable metadata: SOP' VDMs of VID 0x05AC, each with up to 6 VDOs after the header,
   reassembled in arrival order (6 x 24 = 144 bytes). */
static void test_apple_chunks() {
    Engine e;
    for (unsigned c = 0; c < 6; c++) {
        uint32_t v[7];
        v[0] = 0x05AC8140u | c;
        for (unsigned i = 1; i < 7; i++) v[i] = (c << 8) | i;
        feed_vdm(e, Sop::SopP, v, 7);
    }
    CHECK_EQ(e.apple.chunks, 6);
    CHECK_EQ(e.apple.complete(), true);
    CHECK_EQ(e.apple.bytes[0], 1);          /* chunk 0, VDO 1 low byte */
    CHECK_EQ(e.apple.bytes[1], 0);
    CHECK_EQ(e.apple.bytes[24], 1);         /* chunk 1 starts at byte 24 */
    CHECK_EQ(e.apple.bytes[25], 1);         /* its second byte = chunk number */
    CHECK_EQ(e.apple.bytes[140], 6);        /* last chunk, VDO 6 */
    CHECK_EQ(e.apple.bytes[141], 5);
    uint32_t extra[3] = {0x05AC8140u, 1, 2};
    feed_vdm(e, Sop::SopP, extra, 3);       /* a seventh is ignored */
    CHECK_EQ(e.apple.chunks, 6);
    e.reset_cable();
    CHECK_EQ(e.apple.chunks, 0);
}

static void test_source_revision_provenance() {
    Engine e;
    const std::array<uint8_t, 4> r31{0, 0, 0x17, 0x31}, r32{0, 0, 0x20, 0x32};
    e.on_message(Sop::Sop, 0x118C, r31, 1);  // source, one Revision object, PD3.x header
    CHECK_EQ(e.ext.revision.valid, true);
    CHECK_EQ(e.ext.revision.rev_major, 3);
    CHECK_EQ(e.ext.revision.rev_minor, 1);
    e.on_message(Sop::Sop, 0x108C, r32, 1);  // sink RMDO is not source evidence
    CHECK_EQ(e.ext.revision.rev_minor, 1);
    e.on_message(Sop::SopP, 0x118C, r32, 1);
    CHECK_EQ(e.ext.revision.rev_minor, 1);
    e.on_message(Sop::Sop, 0x018C, r32, 1);  // a control header cannot carry RMDO
    CHECK_EQ(e.ext.revision.rev_minor, 1);
    e.reset_source();
    CHECK_EQ(e.ext.revision.valid, false);
    e.on_message(Sop::Sop, hdr(kChunk0), {kChunk0 + 2, sizeof kChunk0 - 2}, 1);
    CHECK_EQ(e.caps.spec_rev, 2);
    CHECK_EQ(e.ext.revision.valid, false);  // EPR does not invent an exact RMDO revision
}

/* A far sink's control messages on SOP (header bit 8 clear: its GoodCRC, its Get_Source_Cap, its
   Soft_Reset) are PD traffic from it: the engine records them in sink_ctl_seen, which the passive-load
   gate (pd::over_load_allowed over fusb302::over_load_facts()) feeds on. Source-role control messages
   (bit 8 set) are not the sink's. Cleared with the attach / detach like the other sink-role records. */
static void test_sink_control_messages_are_recorded() {
    Engine e;
    CHECK_EQ(e.sink_ctl_seen, false);
    feed_ctl(e, 0x01, true);                   // the source's GoodCRC: not the sink's
    feed_ctl(e, 0x03, true);                   // Accept from the source
    feed_ctl(e, 0x06, true);                   // PS_RDY from the source
    CHECK_EQ(e.sink_ctl_seen, false);
    feed_ctl(e, 0x01);                         // the sink's GoodCRC (bit 8 clear)
    CHECK_EQ(e.sink_ctl_seen, true);
    Engine e2;
    feed_ctl(e2, 0x07);                        // the sink's Get_Source_Cap
    CHECK_EQ(e2.sink_ctl_seen, true);
    e2.reset_sink();                           // a new attach: the previous partner's records go
    CHECK_EQ(e2.sink_ctl_seen, false);
    feed_ctl(e2, 0x07);
    CHECK_EQ(e2.sink_ctl_seen, true);
    e2.reset_source();                         // a detach / hard reset: same
    CHECK_EQ(e2.sink_ctl_seen, false);
    feed_ctl(e2, 0x0D);                        // the sink's Soft_Reset counts too
    CHECK_EQ(e2.sink_ctl_seen, true);
}

int main() {
    test_source_revision_provenance();
    test_avs_decode();
    test_ext_header();
    test_epr_two_chunks();
    test_epr_chunk_order_and_replay();
    test_epr_info_only();
    test_spr_clears_epr();
    test_reannounce_longer_list();
    test_spurious_frame_filter();
    test_ignores_other_sop();
    test_cable_sop_prime_and_double_prime();
    test_cable_ignores_requests_and_nak();
    test_cable_svids_and_modes();
    test_apple_chunks();
    test_pr_swap_swaps_side_records();
    test_pr_swap_keeps_each_side_and_new_sink_answer();
    test_pr_swap_rejected_or_incomplete_keeps_records();
    test_sink_control_messages_are_recorded();
    return test_report("test_pd_engine");
}
