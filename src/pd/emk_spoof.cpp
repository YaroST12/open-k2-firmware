// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/log.hpp"
#include "k2/pd/emk_spoof.hpp"

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

std::uint32_t word(std::span<const std::uint8_t> p, unsigned i) {
    if (p.size() < 4 * (i + 1)) return 0;
    return p[4 * i] | (p[4 * i + 1] << 8) | (p[4 * i + 2] << 16) | (std::uint32_t(p[4 * i + 3]) << 24);
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

}  // namespace k2::emk_spoof
