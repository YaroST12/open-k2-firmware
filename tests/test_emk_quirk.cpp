// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::emk_quirk (response rows, storage block, unmatched log) and k2::emk_spoof::respond (the reply builder).
#include <array>
#include <vector>

#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "test_util.h"

using namespace k2;
using emk_quirk::Kind;
using emk_quirk::Quirks;
using emk_quirk::Row;
using emk_quirk::Unmatched;

static emrec::Record sample(std::uint8_t rev) {
    const std::uint32_t v[5] = {0xFF00A041, 0x1C00203C, 0x00000123, 0x00010000, 0x00082052};
    return emrec::make(v, rev);
}

static std::vector<std::uint8_t> bytes(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> b;
    for (std::uint32_t w : words)
        for (int i = 0; i < 4; i++) b.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
    return b;
}

static std::vector<std::uint8_t> toks(const emk_spoof::Frame& f) { return {f.t.begin(), f.t.begin() + f.n}; }

static Quirks one(const Row& r) {
    Quirks q;
    emk_quirk::add_row(q, r);
    return q;
}

// The brick's request: SOP' VDM, PD 3.x, id 1, one data object (the VDM header).
static constexpr std::uint16_t kReqHdr = 0x128F;

static void test_ack_row() {
    const std::uint32_t vdos[2] = {0xAAAA0001u, 0xBBBB0002u};
    const Quirks q = one(emk_quirk::svdm_row(0x22D9, 31, Kind::Ack, vdos));
    const auto req = bytes({0x22D9A01Fu});
    const auto o = emk_spoof::respond(sample(2), q, emk_quirk::kSopP, kReqHdr, req, 3);
    CHECK_EQ(int(o.how), int(emk_spoof::Match::Row)); CHECK_EQ(o.row, 0); CHECK_EQ(o.counter, true);
    // PD header 0x378F: VDM, PD 3.x, cable plug, id 3, 3 objects; VDM header ACKs the request (type 1).
    const std::vector<std::uint8_t> want = {0x12, 0x12, 0x1B, 0x1B, 0x8E, 0x8F, 0x37, 0x5F, 0xA0, 0xD9, 0x22,
                                            0x01, 0x00, 0xAA, 0xAA, 0x02, 0x00, 0xBB, 0xBB, 0xFF, 0x14, 0xFE};
    CHECK_EQ(toks(o.frame) == want, true);
}

static void test_version_and_object_position_are_ignored_and_copied() {
    const Quirks q = one(emk_quirk::svdm_row(0x22D9, 31, Kind::Ack));
    const auto req = bytes({0x22D9A71Fu});                            // object position 7
    const auto o = emk_spoof::respond(sample(2), q, emk_quirk::kSopP, kReqHdr, req, 0);
    CHECK_EQ(int(o.how), int(emk_spoof::Match::Row));
    CHECK_EQ(o.frame.t[7], 0x5F); CHECK_EQ(o.frame.t[8], 0xA7);       // 0x22D9A75F: position kept, ACK
}

static void test_request_must_match() {
    const Quirks q = one(emk_quirk::svdm_row(0x22D9, 31, Kind::Ack));
    auto run = [&](std::uint32_t vdm) {
        return emk_spoof::respond(sample(2), q, emk_quirk::kSopP, kReqHdr, bytes({vdm}), 0).how;
    };
    CHECK_EQ(int(run(0x22D9A01F)), int(emk_spoof::Match::Row));
    CHECK_EQ(int(run(0x22D9A01E)), int(emk_spoof::Match::None));      // other command
    CHECK_EQ(int(run(0x22DAA01F)), int(emk_spoof::Match::None));      // other SVID
    CHECK_EQ(int(run(0x22D9A05F)), int(emk_spoof::Match::None));      // an ACK is not a request
    CHECK_EQ(int(run(0x22D9201F)), int(emk_spoof::Match::None));      // unstructured
}

static void test_nak_busy_raw_replies() {
    const auto req = bytes({0x22D9A01Fu});
    auto nak = emk_spoof::respond(sample(2), one(emk_quirk::svdm_row(0x22D9, 31, Kind::Nak)), emk_quirk::kSopP, kReqHdr, req, 0);
    CHECK_EQ(int(nak.how), int(emk_spoof::Match::Row));
    CHECK_EQ(nak.frame.n, 14);                                        // sync 4, PACKSYM, header 2, one VDM header, 3
    CHECK_EQ(nak.frame.t[5], 0x8F); CHECK_EQ(nak.frame.t[6], 0x11);   // 0x118F: one object, id 0
    CHECK_EQ(nak.frame.t[7], 0x9F);                                   // type 2
    auto busy = emk_spoof::respond(sample(2), one(emk_quirk::svdm_row(0x22D9, 31, Kind::Busy)), emk_quirk::kSopP, kReqHdr, req, 0);
    CHECK_EQ(busy.frame.t[7], 0xDF);                                  // type 3

    const std::uint32_t raw[2] = {0x22D9A55Fu, 0x12345678u};
    auto r = emk_spoof::respond(sample(2), one(emk_quirk::svdm_row(0x22D9, 31, Kind::Raw, raw)), emk_quirk::kSopP, kReqHdr, req, 5);
    CHECK_EQ(int(r.how), int(emk_spoof::Match::Row));
    CHECK_EQ(r.frame.t[4], 0x80 | (2 + 8));
    CHECK_EQ(r.frame.t[5], 0x8F); CHECK_EQ(r.frame.t[6], 0x2B);       // 0x2B8F: two objects, id 5
    CHECK_EQ(r.frame.t[7], 0x5F); CHECK_EQ(r.frame.t[8], 0xA5);       // stored header verbatim
    CHECK_EQ(r.frame.t[11], 0x78);
}

static void test_first_match_wins_and_sop_mask() {
    Quirks q;
    emk_quirk::add_row(q, emk_quirk::svdm_row(0x22D9, 31, Kind::Ack, {}, emk_quirk::kSopPP));   // SOP'' only
    const std::uint32_t a[1] = {0x11111111u}, b[1] = {0x22222222u};
    emk_quirk::add_row(q, emk_quirk::svdm_row(0x22D9, 31, Kind::Ack, a));
    emk_quirk::add_row(q, emk_quirk::svdm_row(0x22D9, 31, Kind::Ack, b));
    const auto req = bytes({0x22D9A01Fu});
    auto o = emk_spoof::respond(sample(2), q, emk_quirk::kSopP, kReqHdr, req, 0);
    CHECK_EQ(o.row, 1); CHECK_EQ(o.frame.t[11], 0x11);
    o = emk_spoof::respond(sample(2), q, emk_quirk::kSopPP, kReqHdr, req, 0);
    CHECK_EQ(o.row, 0);
    CHECK_EQ(o.frame.t[1], 0x1B); CHECK_EQ(o.frame.t[2], 0x12);       // SOP'' sync: 12 1B 12 1B
}

static void test_request_vdo_masks() {
    Row r = emk_quirk::svdm_row(0x22D9, 31, Kind::Ack);
    r.nreq = 1; r.req_val[0] = 0x10; r.req_mask[0] = 0xF0;
    const Quirks q = one(r);
    auto run = [&](std::initializer_list<std::uint32_t> w) {
        const auto p = bytes(w);
        const std::uint16_t h = static_cast<std::uint16_t>(0x008F | (w.size() << 12));
        return emk_spoof::respond(sample(2), q, emk_quirk::kSopP, h, p, 0).how;
    };
    CHECK_EQ(int(run({0x22D9A01Fu, 0x12})), int(emk_spoof::Match::Row));
    CHECK_EQ(int(run({0x22D9A01Fu, 0x22})), int(emk_spoof::Match::None));
    CHECK_EQ(int(run({0x22D9A01Fu})), int(emk_spoof::Match::None));   // the object is missing
}

static void test_discover_identity_stays_byte_exact() {
    const emrec::Record rec = sample(2);
    const auto req = bytes({0xFF00A001u});
    const auto o = emk_spoof::respond(rec, Quirks{}, emk_quirk::kSopP, 0x108F, req, 3);
    CHECK_EQ(int(o.how), int(emk_spoof::Match::Identity));
    const auto old_frame = emk_spoof::build_reply(rec, 3);   // one object: begin() and end() of two temporaries are not a range
    CHECK_EQ(toks(o.frame) == std::vector<std::uint8_t>(old_frame.begin(), old_frame.end()), true);
    // An explicit row is matched before the identity.
    const std::uint32_t w[1] = {0x1u};
    const auto o2 = emk_spoof::respond(rec, one(emk_quirk::svdm_row(0xFF00, 1, Kind::Nak, w)), emk_quirk::kSopP, 0x108F, req, 0);
    CHECK_EQ(int(o2.how), int(emk_spoof::Match::Row));
    // SOP'' only with the flag.
    CHECK_EQ(int(emk_spoof::respond(rec, Quirks{}, emk_quirk::kSopPP, 0x108F, req, 0).how), int(emk_spoof::Match::None));
    Quirks f; f.flags = emk_quirk::kQSopPP;
    const auto o3 = emk_spoof::respond(rec, f, emk_quirk::kSopPP, 0x108F, req, 0);
    CHECK_EQ(int(o3.how), int(emk_spoof::Match::Identity));
    CHECK_EQ(o3.frame.t[1], 0x1B);
}

static void test_unmatched_policy() {
    const auto req = bytes({0x22D9A01Fu});
    auto run = [&](Unmatched u, std::uint8_t rev, std::uint16_t hdr, std::vector<std::uint8_t> payload) {
        Quirks q; q.unmatched = u;
        return emk_spoof::respond(sample(rev), q, emk_quirk::kSopP, hdr, payload, 0);
    };
    auto o = run(Unmatched::Ignore, 2, kReqHdr, req);
    CHECK_EQ(int(o.how), int(emk_spoof::Match::None)); CHECK_EQ(o.unmatched, true); CHECK_EQ(o.frame.n, 0);
    o = run(Unmatched::Nak, 2, kReqHdr, req);
    CHECK_EQ(int(o.how), int(emk_spoof::Match::Policy)); CHECK_EQ(o.frame.t[7], 0x9F); CHECK_EQ(o.frame.t[8], 0xA0);
    o = run(Unmatched::NotSupported, 2, kReqHdr, req);
    CHECK_EQ(int(o.how), int(emk_spoof::Match::Policy));
    const std::vector<std::uint8_t> ns = {0x12, 0x12, 0x1B, 0x1B, 0x82, 0x90, 0x01, 0xFF, 0x14, 0xFE};   // Not_Supported, rev 2, cable plug
    CHECK_EQ(toks(o.frame) == ns, true);
    // Not_Supported exists from PD 3.x: a PD 2.0 identity stays silent. Attention and non-requests are never answered.
    CHECK_EQ(int(run(Unmatched::NotSupported, 1, kReqHdr, req).how), int(emk_spoof::Match::None));
    CHECK_EQ(int(run(Unmatched::Nak, 2, kReqHdr, bytes({0x22D9A006u})).how), int(emk_spoof::Match::None));   // Attention
    CHECK_EQ(int(run(Unmatched::Nak, 2, kReqHdr, bytes({0x22D9A05Fu})).how), int(emk_spoof::Match::None));   // an ACK
    CHECK_EQ(int(run(Unmatched::Nak, 2, 0x1085, bytes({0x1u})).how), int(emk_spoof::Match::None));            // not a VDM
    CHECK_EQ(run(Unmatched::Nak, 2, 0x1085, bytes({0x1u})).unmatched, true);                                  // but it is logged
}

static void test_message_id_echo() {
    Quirks q = one(emk_quirk::svdm_row(0x22D9, 31, Kind::Nak));
    q.flags = emk_quirk::kQMsgIdEcho;
    const auto o = emk_spoof::respond(sample(2), q, emk_quirk::kSopP, kReqHdr, bytes({0x22D9A01Fu}), 6);
    CHECK_EQ(o.counter, false);
    CHECK_EQ(o.frame.t[6], 0x13);                                     // the request's id 1: 0x138F
}

static void test_block_roundtrip_and_validation() {
    Quirks q;
    q.flags = emk_quirk::kQMsgIdEcho; q.unmatched = Unmatched::Nak;
    const std::uint32_t w[2] = {1, 2};
    Row r = emk_quirk::svdm_row(0x22D9, 31, Kind::Ack, w);
    r.nreq = 2; r.req_val = {5, 6}; r.req_mask = {7, 8};
    emk_quirk::add_row(q, r);
    emk_quirk::add_row(q, emk_quirk::svdm_row(0xFF00, 2, Kind::Nak, {}, emk_quirk::kSopP | emk_quirk::kSopPP));
    std::array<std::uint8_t, emk_quirk::kMaxBlock> buf{};
    const std::size_t n = emk_quirk::encode(q, buf);
    CHECK_EQ(n, emk_quirk::block_size(2)); CHECK_EQ(n, 3 + 2 * 52);
    Quirks back;
    CHECK_EQ(emk_quirk::decode({buf.data(), n}, back), true);
    CHECK_EQ(back == q, true);
    CHECK_EQ(emk_quirk::encode(q, std::span<std::uint8_t>(buf).first(n - 1)), 0);
    CHECK_EQ(emk_quirk::decode({buf.data(), n - 1}, back), false);
    auto bad = [&](std::size_t at, std::uint8_t v) {
        auto c = buf; c[at] = v;
        Quirks t;
        return emk_quirk::decode({c.data(), n}, t);
    };
    CHECK_EQ(bad(2, 9), false);                                       // count past the table (and past the length)
    CHECK_EQ(bad(1, 3), false);                                       // unknown unmatched policy
    CHECK_EQ(bad(0, 0x80), false);                                    // unknown flag
    CHECK_EQ(bad(3 + 0, 0), false);                                   // row sop 0
    CHECK_EQ(bad(3 + 1, 4), false);                                   // unknown kind
    CHECK_EQ(bad(3 + 2, 3), false);                                   // nreq past the limit
    CHECK_EQ(bad(3 + 3, 7), false);                                   // nrep past the limit
    CHECK_EQ(bad(3 + 52 + 1, 3), false);                              // Raw with nrep 0: no header word
    Quirks empty;
    CHECK_EQ(emk_quirk::encode(empty, buf), 3);
    CHECK_EQ(emk_quirk::decode({buf.data(), 3}, back), true); CHECK_EQ(back.empty(), true);
}

static void test_raw_row_needs_a_header_word() {
    Quirks q;
    Row r = emk_quirk::svdm_row(0x22D9, 31, Kind::Raw);   // nrep 0: no header word
    emk_quirk::add_row(q, r);
    std::array<std::uint8_t, emk_quirk::kMaxBlock> buf{};
    const std::size_t n = emk_quirk::encode(q, buf);
    Quirks back;
    CHECK_EQ(emk_quirk::decode({buf.data(), n}, back), false);
}

static void test_seen_log() {
    emk_quirk::SeenLog log;
    emk_quirk::Seen s;
    CHECK_EQ(log.size(), 0); CHECK_EQ(log.get(0, s), false);
    for (std::uint32_t i = 1; i <= 6; i++) log.push(emk_quirk::kSopP, static_cast<std::uint16_t>(0x100 + i), bytes({0x22D9A01Fu, i}));
    CHECK_EQ(log.total(), 6); CHECK_EQ(log.size(), 4);
    CHECK_EQ(log.get(0, s), true); CHECK_EQ(s.header, 0x106); CHECK_EQ(s.n, 2); CHECK_EQ(s.w[0], 0x22D9A01Fu); CHECK_EQ(s.w[1], 6);
    CHECK_EQ(log.get(3, s), true); CHECK_EQ(s.header, 0x103);
    CHECK_EQ(log.get(4, s), false);
    log.clear(); CHECK_EQ(log.size(), 0);
    log.push(emk_quirk::kSopPP, 0x1, bytes({1, 2, 3, 4, 5, 6, 7, 8, 9}));   // more words than a message holds
    CHECK_EQ(log.get(0, s), true); CHECK_EQ(s.n, 7); CHECK_EQ(s.sop, emk_quirk::kSopPP);
}

static void test_builtin_quirks_are_valid() {
    for (unsigned id = 0; id < emrec::kBuiltinCount; id++) {
        const Quirks& q = emk_quirk::builtin_quirks(id);
        std::array<std::uint8_t, emk_quirk::kMaxBlock> buf{};
        const std::size_t n = emk_quirk::encode(q, buf);
        Quirks back;
        CHECK_EQ(emk_quirk::decode({buf.data(), n}, back), true);
    }
    CHECK_EQ(emk_quirk::builtin_quirks(99).empty(), true);
}

int main() {
    test_ack_row();
    test_version_and_object_position_are_ignored_and_copied();
    test_request_must_match();
    test_nak_busy_raw_replies();
    test_first_match_wins_and_sop_mask();
    test_request_vdo_masks();
    test_discover_identity_stays_byte_exact();
    test_unmatched_policy();
    test_message_id_echo();
    test_block_roundtrip_and_validation();
    test_raw_row_needs_a_header_word();
    test_seen_log();
    test_builtin_quirks_are_valid();
    return test_report("test_emk_quirk");
}
