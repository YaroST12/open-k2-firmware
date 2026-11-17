// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/log.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/pd/xiaomi_auth.hpp"

#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::emk_spoof {
namespace {
emrec::Record g_rec;
emk_quirk::Quirks g_quirks;
emk_quirk::SeenLog g_seen;
bool g_active = false;
bool g_run_scoped = false;   // armed by the interrogation: it answers inline too (inside the driver's window)
std::uint8_t g_msg_id = 0;
std::uint32_t g_replies = 0;
std::uint32_t g_requests = 0;
std::uint32_t g_unmatched = 0;
std::uint32_t g_generation = 0;
MiAuthCfg g_mi_auth;

std::uint32_t word(std::span<const std::uint8_t> p, unsigned i) {
    if (p.size() < 4 * (i + 1)) return 0;
    return p[4 * i] | (p[4 * i + 1] << 8) | (p[4 * i + 2] << 16) | (std::uint32_t(p[4 * i + 3]) << 24);
}

// True for an SOP' unstructured VDM with SVID 0x2717, command 4, carrying 4 challenge words (the cable-auth
// request some Xiaomi-family sources send to a Xiaomi-VID cable after Discover Identity).
bool is_mi_cable_challenge(std::uint16_t header, std::span<const std::uint8_t> payload) {
    if ((header & 0x8000) != 0) return false;                       // extended
    if ((header & 0x1F) != kMsgVendorDefined || ((header >> 12) & 7) < 5 || payload.size() < 20) return false;
    const std::uint32_t vdm = word(payload, 0);
    return (vdm >> 16) == 0x2717u && (vdm & 0x8000u) == 0 && (vdm & 0x1Fu) == 4;
}

// FIFO tokens of the 0x27170105 answer: sync, PACKSYM, cable header (5 objects), VDM header + 4 digest words.
Frame mi_auth_frame(std::uint32_t req_vdm, std::span<const std::uint8_t> challenge, std::uint8_t msg_id) {
    std::uint32_t ch[4];
    for (unsigned i = 0; i < 4; i++) ch[i] = word(challenge, 1 + i);
    std::uint8_t digest[32];
    pd::mi::auth_digest(g_mi_auth.key, ch, g_mi_auth.trailing, digest);
    const unsigned off = g_mi_auth.half ? 16 : 0;
    const std::uint32_t reply_vdm = g_mi_auth.reply ? (req_vdm + 0x100u) : ((req_vdm & 0xFFFFFF00u) | 0x05u);
    std::array<std::uint32_t, 5> w = {reply_vdm, 0, 0, 0, 0};
    if (g_mi_auth.payload == 1) {   // answer with the session seed instead of a digest (the sink-flow 0104 content)
        const unsigned ki = g_mi_auth.key < pd::mi::kKeyCount ? g_mi_auth.key : 0;
        for (unsigned i = 0; i < 4; i++) w[1 + i] = pd::mi::kSessionSeed[ki][i];
    } else {
        for (unsigned i = 0; i < 4; i++) w[1 + i] = pd::mi::digest_word(digest + off, i);
    }
    Frame f;
    std::size_t n = 0;
    for (std::uint8_t b : {0x12, 0x12, 0x1B, 0x1B}) f.t[n++] = b;
    f.t[n++] = static_cast<std::uint8_t>(0x80 | (2 + 4 * w.size()));
    const std::uint16_t h = static_cast<std::uint16_t>(kMsgVendorDefined | ((g_rec[0x14] & 3u) << 6) | (1u << 8) |
                                                       ((msg_id & 7u) << 9) | (5u << 12));
    f.t[n++] = static_cast<std::uint8_t>(h);
    f.t[n++] = static_cast<std::uint8_t>(h >> 8);
    for (std::uint32_t v : w)
        for (int i = 0; i < 4; i++) f.t[n++] = static_cast<std::uint8_t>(v >> (8 * i));
    f.t[n++] = 0xFF;   // JAM_CRC
    f.t[n++] = 0x14;   // EOP
    f.t[n++] = 0xFE;   // TXOFF
    f.n = static_cast<std::uint8_t>(n);
    return f;
}

// Called by fusb302 for every received SOP' / SOP'' message while armed. Only SOP' is answered: the SOP'' answer
// (quirk flag kQSopPP) is not wired, and a SOP'' request must never get the cable plug's reply.
void on_sop_prime(std::uint8_t sop, std::uint16_t header, std::span<const std::uint8_t> payload) {
    if (sop != 1 || !g_active || (fusb302::inline_mode() && !g_run_scoped)) return;
    if (is_soft_reset(header)) {   // the cable resets its MessageID counter and Accepts with id 0
        g_msg_id = 0;
        const bool ok = fusb302::send_sop_prime_frame(build_accept(g_rec, g_msg_id));
        K2LOG("[SPOOF] Soft_Reset on SOP': Accept %s\n", ok ? "sent" : "FAILED");
        if (ok) g_msg_id = 1;
        return;
    }
    if (is_discover_identity_req(header, payload)) g_requests++;
    if (g_mi_auth.on && sop == 1 && is_mi_cable_challenge(header, payload)) {
        const std::uint32_t req_vdm = word(payload, 0);
        Frame f = mi_auth_frame(req_vdm, payload, g_msg_id);
        const bool ok = fusb302::send_sop_prime_frame(f.tokens());
        const auto fw = [&f](unsigned i) {
            return std::uint32_t(f.t[7 + 4 * i]) | (std::uint32_t(f.t[8 + 4 * i]) << 8) |
                   (std::uint32_t(f.t[9 + 4 * i]) << 16) | (std::uint32_t(f.t[10 + 4 * i]) << 24);
        };
        K2LOG("[EMK] Mi cable-auth req=%08X key=%u trail=%08X half=%u reply=%u -> %08X %08X %08X %08X %s\n",
              static_cast<unsigned>(req_vdm), static_cast<unsigned>(g_mi_auth.key),
              static_cast<unsigned>(g_mi_auth.trailing), static_cast<unsigned>(g_mi_auth.half), static_cast<unsigned>(g_mi_auth.reply),
              static_cast<unsigned>(fw(1)), static_cast<unsigned>(fw(2)), static_cast<unsigned>(fw(3)), static_cast<unsigned>(fw(4)),
              ok ? "sent" : "FAILED");
        if (!ok) return;
        g_msg_id = (g_msg_id + 1) & 7;
        g_replies++;
        return;
    }
    const Outcome o = respond(g_rec, g_quirks, emk_quirk::kSopP, header, payload, g_msg_id);
    if (o.unmatched) {   // what a source asks beyond what this cable knows: the raw words are the way to learn it
        g_unmatched++;
        g_seen.push(emk_quirk::kSopP, header, payload);
        K2LOG("[EMK] unmatched SOP' hdr=0x%04X w0=0x%08X w1=0x%08X w2=0x%08X\n", static_cast<unsigned>(header),
              static_cast<unsigned>(word(payload, 0)), static_cast<unsigned>(word(payload, 1)), static_cast<unsigned>(word(payload, 2)));
    }
    if (o.how == Match::None) return;
    const bool ok = fusb302::send_sop_prime_frame(o.frame.tokens());
    K2LOG("[EMK] SOP' %s reply (row %u) msg_id=%u %s\n", o.how == Match::Identity ? "identity" : o.how == Match::Row ? "row" : "policy",
          static_cast<unsigned>(o.row), static_cast<unsigned>((o.frame.t[6] >> 1) & 7), ok ? "sent" : "FAILED");
    if (!ok) return;
    if (o.counter) g_msg_id = (g_msg_id + 1) & 7;
    g_replies++;
}
}  // namespace

namespace {
Err arm_checked(const emrec::Record& rec, const emk_quirk::Quirks& quirks, bool run_scoped) {
    if (!usable(rec, quirks)) return Err::Invalid;
    if (fusb302::inline_mode() && !run_scoped) return Err::Inline;
    g_run_scoped = run_scoped;
    g_rec = rec;
    g_quirks = quirks;
    g_seen.clear();
    g_msg_id = 0;
    g_replies = 0;
    g_requests = 0;
    g_unmatched = 0;
    g_active = true;
    ++g_generation;
    fusb302::set_sop_prime_hook(&on_sop_prime);
    K2LOG("[SPOOF] armed%s: Discover Identity from the saved record, %u extra response rows\n", run_scoped ? " for the run" : "",
          static_cast<unsigned>(quirks.count));
    return Err::Ok;
}
}  // namespace

Err arm(const emrec::Record& rec, const emk_quirk::Quirks& quirks) { return arm_checked(rec, quirks, false); }
Err arm_run(const emrec::Record& rec, const emk_quirk::Quirks& quirks) { return arm_checked(rec, quirks, true); }

void disarm() {
    if (!g_active) return;
    g_active = false;
    g_run_scoped = false;
    ++g_generation;
    fusb302::set_sop_prime_hook(nullptr);
    K2LOG("[SPOOF] off: %u requests, %u replies, %u unmatched\n", static_cast<unsigned>(g_requests), static_cast<unsigned>(g_replies),
          static_cast<unsigned>(g_unmatched));
}

bool active() { return g_active; }
bool matches(const emrec::Record& rec, const emk_quirk::Quirks& quirks) { return g_active && g_rec == rec && g_quirks == quirks; }
std::uint32_t generation() { return g_generation; }
std::uint32_t replies() { return g_replies; }
std::uint32_t requests() { return g_requests; }
std::uint32_t unmatched() { return g_unmatched; }
const emk_quirk::SeenLog& seen() { return g_seen; }
emrec::Record& scratch_record() { return g_rec; }
emk_quirk::Quirks& scratch_quirks() { return g_quirks; }

void set_mi_auth(const MiAuthCfg& c) { g_mi_auth = c; }
MiAuthCfg mi_auth() { return g_mi_auth; }

}  // namespace k2::emk_spoof
