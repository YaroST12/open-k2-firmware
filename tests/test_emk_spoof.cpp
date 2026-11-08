// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure frame logic of k2::emk_spoof: request matching and the SOP' identity reply.
#include "k2/pd/emk_spoof.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/usb.hpp"
#include "test_util.h"
#include <vector>

using namespace k2;
static fusb302::SopPrimeHook hook3 = nullptr;   // the armed hook, as fusb302 calls it: (sop, header, payload)
// A SOP' frame, which is what every test below delivers; SOP'' goes through hook3 directly.
static void hook(uint16_t header, std::span<const uint8_t> payload) { hook3(1, header, payload); }
static bool inline_mode = false, send_ok = true;
static std::vector<std::vector<uint8_t>> sent;
namespace k2::fusb302 {
bool inline_mode() { return ::inline_mode; }
void set_sop_prime_hook(SopPrimeHook h) { hook3 = h; }
bool send_sop_prime_frame(std::span<const uint8_t> frame) {
    sent.emplace_back(frame.begin(), frame.end()); return send_ok;
}
}
namespace k2::usb {
void log(std::string_view) {}
void log_fmt(const char*, ...) {}
}

static emrec::Record sample(std::uint8_t rev) {
    const std::uint32_t v[5] = {0xFF00A041, 0x1C00203C, 0x00000123, 0x00010000, 0x00082052};
    return emrec::make(v, rev);
}

static void test_match() {
    const std::uint8_t di[4] = {0x01, 0xA0, 0x00, 0xFF};      // 0xFF00A001: REQ, cmd 1
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x108F, di), 1);
    const std::uint8_t ack[4] = {0x41, 0xA0, 0x00, 0xFF};     // ACK, not a request
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x108F, ack), 0);
    const std::uint8_t modes[4] = {0x02, 0xA0, 0x00, 0xFF};   // Discover SVIDs
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x108F, modes), 0);
    const std::uint8_t unstr[4] = {0x01, 0x20, 0x00, 0xFF};   // unstructured
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x108F, unstr), 0);
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x108E, di), 0);   // not a VDM
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x008F, di), 0);   // no data objects
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x908F, di), 0);   // extended
    CHECK_EQ(emk_spoof::is_discover_identity_req(0x108F, {di, 3}), 0);
}

static void test_reply() {
    const emrec::Record r = sample(2);
    CHECK_EQ(emk_spoof::reply_header(r, 0), 0x518F);
    CHECK_EQ(emk_spoof::reply_header(r, 5), 0x5B8F);
    CHECK_EQ(emk_spoof::reply_header(sample(1), 0), 0x514F);       // PD 2.0 record
    const auto f = emk_spoof::build_reply(r, 3);
    CHECK_EQ(f.size(), 30);
    CHECK_EQ(f[0], 0x12); CHECK_EQ(f[3], 0x1B);
    CHECK_EQ(f[4], 0x96);                                          // PACKSYM | 22
    CHECK_EQ(f[5], 0x8F); CHECK_EQ(f[6], 0x57);                    // 0x578F: id 3
    CHECK_EQ(f[7], 0x41); CHECK_EQ(f[10], 0xFF);                   // VDM header 0xFF00A041
    CHECK_EQ(f[11], 0x3C);                                         // ID header low byte
    CHECK_EQ(f[26], 0x00); CHECK_EQ(f[27], 0xFF); CHECK_EQ(f[28], 0x14); CHECK_EQ(f[29], 0xFE);
}
static void test_explicit_activation_and_identity() {
    CHECK_EQ(emk_spoof::active(), false); CHECK_EQ(hook3 == nullptr, true);
    CHECK_EQ(emk_spoof::generation(), 0);
    const auto a = sample(2), b = sample(1);
    CHECK_EQ(emk_spoof::matches(a), false);
    auto invalid = a; invalid[0] ^= 1;
    CHECK_EQ(emk_spoof::arm(invalid), emk_spoof::Err::Invalid);
    CHECK_EQ(hook3 == nullptr, true); CHECK_EQ(emk_spoof::generation(), 0);
    inline_mode = true;
    CHECK_EQ(emk_spoof::arm(a), emk_spoof::Err::Inline);
    CHECK_EQ(emk_spoof::active(), false); CHECK_EQ(emk_spoof::generation(), 0);
    inline_mode = false;
    CHECK_EQ(emk_spoof::arm(a), emk_spoof::Err::Ok);
    CHECK_EQ(emk_spoof::matches(a), true); CHECK_EQ(emk_spoof::matches(b), false);
    CHECK_EQ(emk_spoof::generation(), 1); CHECK_EQ(sent.size(), 0);
    CHECK_EQ(hook3 != nullptr, true);
    const uint8_t di[4] = {0x01, 0xA0, 0, 0xFF}, ack[4] = {0x41, 0xA0, 0, 0xFF};
    hook(0x108F, {ack, 4}); CHECK_EQ(emk_spoof::requests(), 0);
    inline_mode = true; hook(0x108F, {di, 4});
    CHECK_EQ(emk_spoof::requests(), 0); CHECK_EQ(emk_spoof::active(), true);
    inline_mode = false; hook(0x108F, {di, 4});
    CHECK_EQ(sent.size(), 1); CHECK_EQ(emk_spoof::requests(), 1); CHECK_EQ(emk_spoof::replies(), 1);
    CHECK_EQ(sent[0][5], 0x8F); CHECK_EQ(sent[0][6], 0x51);
    send_ok = false; hook(0x108F, {di, 4});
    CHECK_EQ(emk_spoof::requests(), 2); CHECK_EQ(emk_spoof::replies(), 1);
    send_ok = true; hook(0x108F, {di, 4});
    CHECK_EQ(sent.back()[6], 0x53); // failed TX does not consume the next MessageID
    CHECK_EQ(emk_spoof::arm(invalid), emk_spoof::Err::Invalid);
    CHECK_EQ(emk_spoof::matches(a), true); CHECK_EQ(emk_spoof::generation(), 1);
    CHECK_EQ(emk_spoof::arm(b), emk_spoof::Err::Ok);
    CHECK_EQ(emk_spoof::matches(a), false); CHECK_EQ(emk_spoof::matches(b), true);
    CHECK_EQ(emk_spoof::generation(), 2); CHECK_EQ(emk_spoof::requests(), 0); CHECK_EQ(emk_spoof::replies(), 0);
    inline_mode = true; emk_spoof::disarm();
    CHECK_EQ(emk_spoof::active(), false); CHECK_EQ(emk_spoof::matches(b), false);
    CHECK_EQ(emk_spoof::generation(), 3); CHECK_EQ(hook3 == nullptr, true);
    emk_spoof::disarm(); CHECK_EQ(emk_spoof::generation(), 3);
    inline_mode = false;
}

static void test_soft_reset() {
    const emrec::Record r = sample(2);
    CHECK_EQ(emk_spoof::is_soft_reset(0x010D), true);               // control, type 13, no data objects
    CHECK_EQ(emk_spoof::is_soft_reset(0x110D), false);              // has a data object
    CHECK_EQ(emk_spoof::is_soft_reset(0x010E), false);              // Data_Reset, not Soft_Reset
    CHECK_EQ(emk_spoof::is_soft_reset(0x800D), false);              // extended
    CHECK_EQ(emk_spoof::is_soft_reset(0x010C), false);
    const auto a = emk_spoof::build_accept(r, 5);
    CHECK_EQ(a.size(), 10);
    CHECK_EQ(a[0], 0x12); CHECK_EQ(a[3], 0x1B);
    CHECK_EQ(a[4], 0x82);                                           // PACKSYM | 2
    CHECK_EQ(a[5], 0x83); CHECK_EQ(a[6], 0x0B);                     // 0x0B83: Accept, rev 2, cable plug, id 5
    CHECK_EQ(a[7], 0xFF); CHECK_EQ(a[8], 0x14); CHECK_EQ(a[9], 0xFE);

    sent.clear();
    inline_mode = false; send_ok = true;
    CHECK_EQ(emk_spoof::arm(r), emk_spoof::Err::Ok);
    const uint8_t di[4] = {0x01, 0xA0, 0, 0xFF};
    hook(0x108F, {di, 4});                                          // one reply: the id counter is now 1
    hook(0x010D, {});                                               // Soft_Reset
    CHECK_EQ(sent.size(), 2);
    CHECK_EQ(sent[1].size(), 10); CHECK_EQ(sent[1][5], 0x83); CHECK_EQ(sent[1][6], 0x01);   // Accept, id 0
    CHECK_EQ(emk_spoof::requests(), 1);                             // a Soft_Reset is no identity request
    hook(0x108F, {di, 4});
    CHECK_EQ(sent.back()[6], 0x53);                                 // the next reply continues at id 1
    emk_spoof::disarm();
}

// Every built-in answers a Discover Identity request with exactly its captured words: the VDM header as read from the
// cable (ACK, SVDM v2.0, SOP' cable-plug bit set in the PD header, PD 3.x), then the four identity VDOs byte-exact.
static void test_builtin_replies_carry_the_captured_words() {
    static const std::uint32_t kWords[5][5] = {
        {},                                              // id 0, the generic cable, is not a capture
        {0xFF00A041u, 0x18002B01u, 0x00000910u, 0x40516910u, 0x000A2640u},
        {0xFF00A041u, 0x18602B01u, 0x00000A2Eu, 0x40516910u, 0x000A2640u},
        {0xFF00A041u, 0x180022D9u, 0x00000000u, 0x61160000u, 0x80082A40u},   // id 3 is retired: it answers as id 4
        {0xFF00A041u, 0x180022D9u, 0x00000000u, 0x61160000u, 0x80082A40u},
    };
    inline_mode = false; send_ok = true;
    const uint8_t di[4] = {0x01, 0xA0, 0x00, 0xFF};
    for (unsigned id = 1; id < emrec::kBuiltinCount; ++id) {
        sent.clear();
        CHECK_EQ(emk_spoof::arm(emrec::builtin_cable(id)), emk_spoof::Err::Ok);
        hook(0x108F, {di, 4});
        CHECK_EQ(sent.size(), 1);
        if (sent.size() != 1) { emk_spoof::disarm(); continue; }
        const auto& f = sent[0];
        CHECK_EQ(f.size(), emk_spoof::kFrameSize);
        CHECK_EQ(f[4], 0x96);                                       // PACKSYM | 22: header + 5 objects
        CHECK_EQ(f[5], 0x8F); CHECK_EQ(f[6], 0x51);                 // SOP' VDM, 5 objects, PD 3.x, cable plug, id 0
        for (unsigned w = 0; w < 5; ++w) {
            const std::uint32_t got = f[7 + 4 * w] | (f[8 + 4 * w] << 8) | (f[9 + 4 * w] << 16) | (std::uint32_t(f[10 + 4 * w]) << 24);
            CHECK_EQ(got, kWords[id][w]);
        }
        CHECK_EQ(f[27], 0xFF); CHECK_EQ(f[28], 0x14); CHECK_EQ(f[29], 0xFE);
        emk_spoof::disarm();
    }
}

static emk_quirk::Quirks oppo_row_quirks(std::uint8_t flags = 0) {
    emk_quirk::Quirks q;
    const std::uint32_t vdo[1] = {0xAAAA0001u};
    emk_quirk::add_row(q, emk_quirk::svdm_row(0x22D9, 31, emk_quirk::Kind::Ack, vdo));
    q.flags = flags;
    return q;
}

// The hook answers a quirk row, keeps its MessageID counter in step with the replies it really sent, and
// logs what nothing answered.
static void test_hook_rows_counter_and_unmatched_log() {
    inline_mode = false; send_ok = true;
    sent.clear();
    const std::uint8_t oppo[4] = {0x1F, 0xA0, 0xD9, 0x22};            // 0x22D9A01F, id 1 in the PD header
    const std::uint8_t di[4] = {0x01, 0xA0, 0x00, 0xFF};
    const std::uint8_t other[4] = {0x1E, 0xA0, 0xD9, 0x22};           // OPPO command 30: no row
    CHECK_EQ(emk_spoof::arm(sample(2), oppo_row_quirks()), emk_spoof::Err::Ok);
    CHECK_EQ(emk_spoof::unmatched(), 0); CHECK_EQ(emk_spoof::seen().size(), 0);

    hook(0x128F, {oppo, 4});                                           // row, cable id 0
    CHECK_EQ(sent.size(), 1); CHECK_EQ(emk_spoof::replies(), 1);
    CHECK_EQ(sent[0][5], 0x8F); CHECK_EQ(sent[0][6], 0x21);            // 0x218F: two objects, id 0
    CHECK_EQ(sent[0][7], 0x5F); CHECK_EQ(sent[0][10], 0x22);           // 0x22D9A05F
    CHECK_EQ(sent[0][11], 0x01); CHECK_EQ(sent[0][12], 0x00); CHECK_EQ(sent[0][13], 0xAA);
    CHECK_EQ(emk_spoof::requests(), 0);                                // not a Discover Identity
    send_ok = false; hook(0x128F, {oppo, 4});                          // TX failure: counter stays
    send_ok = true; hook(0x108F, {di, 4});
    CHECK_EQ(sent.back()[6], 0x53);                                    // identity reply continues at id 1
    CHECK_EQ(emk_spoof::requests(), 1);
    CHECK_EQ(emk_spoof::unmatched(), 0);                               // answered ones are not "unmatched"
    hook(0x0001, {});                                                  // a GoodCRC is no request
    CHECK_EQ(emk_spoof::unmatched(), 0);

    const std::size_t before = sent.size();
    hook(0x128F, {other, 4});                                          // nobody answers: logged, silent
    CHECK_EQ(sent.size(), before); CHECK_EQ(emk_spoof::unmatched(), 1);
    CHECK_EQ(emk_spoof::seen().size(), 1);
    emk_quirk::Seen s;
    CHECK_EQ(emk_spoof::seen().get(0, s), true);
    CHECK_EQ(s.sop, emk_quirk::kSopP); CHECK_EQ(s.header, 0x128F); CHECK_EQ(s.n, 1); CHECK_EQ(s.w[0], 0x22D9A01Eu);
    emk_spoof::disarm();
    emk_spoof::arm(sample(2));                                         // re-arm: counters and the log start over
    CHECK_EQ(emk_spoof::unmatched(), 0); CHECK_EQ(emk_spoof::seen().size(), 0);
    emk_spoof::disarm();
}

static void test_hook_policy_and_msg_id_echo() {
    inline_mode = false; send_ok = true;
    sent.clear();
    const std::uint8_t oppo[4] = {0x1F, 0xA0, 0xD9, 0x22};
    // MessageID echo: the reply carries the request's id (1) and the cable's counter is not used
    CHECK_EQ(emk_spoof::arm(sample(2), oppo_row_quirks(emk_quirk::kQMsgIdEcho)), emk_spoof::Err::Ok);
    hook(0x128F, {oppo, 4});
    CHECK_EQ(sent.size(), 1); CHECK_EQ(sent[0][6], 0x23);              // id 1
    hook(0x128F, {oppo, 4});
    CHECK_EQ(sent.size(), 2); CHECK_EQ(sent[1][6], 0x23);
    emk_spoof::disarm();
    // unmatched policy NAK
    emk_quirk::Quirks q;
    q.unmatched = emk_quirk::Unmatched::Nak;
    const std::uint8_t other[4] = {0x1E, 0xA0, 0xD9, 0x22};
    sent.clear();
    CHECK_EQ(emk_spoof::arm(sample(2), q), emk_spoof::Err::Ok);
    hook(0x128F, {other, 4});
    CHECK_EQ(sent.size(), 1); CHECK_EQ(sent[0][7], 0x9E);              // NAK of 0x22D9A01E
    CHECK_EQ(emk_spoof::unmatched(), 1);                               // still logged: a row did not answer it
    emk_spoof::disarm();
    // a damaged table is refused
    emk_quirk::Quirks bad;
    bad.flags = 0x80;
    CHECK_EQ(emk_spoof::arm(sample(2), bad), emk_spoof::Err::Invalid);
    CHECK_EQ(emk_spoof::active(), false);
}

// A SOP'' frame must never get the cable plug's reply: the chip listens on SOP'' for the cable-sniff measurement only.
static void test_sop_pp_is_ignored() {
    inline_mode = false; send_ok = true;
    sent.clear();
    const std::uint8_t di[4] = {0x01, 0xA0, 0x00, 0xFF};
    CHECK_EQ(emk_spoof::arm(sample(2), emk_quirk::Quirks{}), emk_spoof::Err::Ok);
    const std::uint32_t req0 = emk_spoof::requests();
    hook3(2, 0x108F, {di, 4});                                         // Discover Identity on SOP''
    hook3(2, 0x010D, {});                                              // Soft_Reset on SOP''
    CHECK_EQ(sent.size(), 0); CHECK_EQ(emk_spoof::requests(), req0);
    CHECK_EQ(emk_spoof::replies(), 0); CHECK_EQ(emk_spoof::unmatched(), 0);
    hook3(1, 0x108F, {di, 4});                                         // the same frame on SOP' is still answered
    CHECK_EQ(sent.size(), 1); CHECK_EQ(emk_spoof::requests(), req0 + 1);
    emk_spoof::disarm();
}

int main() {
    test_match();
    test_reply();
    test_explicit_activation_and_identity();
    test_soft_reset();
    test_builtin_replies_carry_the_captured_words();
    test_hook_rows_counter_and_unmatched_log();   // the statics leak between cases: new ones go last
    test_hook_policy_and_msg_id_echo();
    test_sop_pp_is_ignored();
    return test_report("test_emk_spoof");
}
