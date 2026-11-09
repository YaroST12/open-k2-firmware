// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Rd comeback on the real hardware, see k2/core/rd_comeback.hpp.
#include "k2/core/rd_comeback.hpp"

#include "k2/core/log.hpp"
#include "k2/core/storage.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/pdtrace.hpp"

namespace k2::rdcomeback {
namespace {

using rdintent::Intent;
using rdintent::IntentKind;

constexpr std::uint32_t kQuietMs = 300;      // PD link quiet before a flash write (as the black box persister)
constexpr std::uint32_t kRpLookEveryMs = 50; // the Rp comparator look is a few I2C transactions: not on every pass

Intent g_found;                  // what begin() read (None unless this boot is a comeback)
bool g_pending = false;          // comeback boot, no verdict yet
bool g_spoof = false;            // arm() armed the recorded identity
bool g_rp_seen = false;
std::uint32_t g_rp_look = 0;
rdintent::Follower g_follower;
bool g_dirty = false;            // the follower's record is not on the volume yet
ctcable::Choice g_run_cable{};

const char* kind_name(IntentKind k) { return k == IntentKind::Session ? "session" : k == IntentKind::Reattach ? "reattach" : "none"; }

// A recorded identity, armed like the cable dialog arms it (k2/ui/ui_cable_list.cpp arm()) but without the page.
[[gnu::noinline]] bool arm_identity(ctcable::Choice c) {
    c = ctcable::canonical(c);
    emlib::Entry e;
    emk_quirk::Quirks quirks;
    if (c.kind == ctcable::Kind::Builtin) {
        e.rec = emrec::builtin_cable(c.seq);
        quirks = emk_quirk::builtin_quirks(c.seq);
    } else if (!storage::lib_get(c.seq, e, quirks)) return false;
    return emk_spoof::arm(e.rec, quirks) == emk_spoof::Err::Ok;
}

void release() {
    g_pending = false;
    if (g_spoof) emk_spoof::disarm();
    g_spoof = false;
    fusb302::ra_set(false);
    fusb302::self_power_drop();
    rdintent::save(fs::instance(), {});
}

}  // namespace

bootmode::Mode begin(bootmode::Mode keys, bool warm) {
    if (!rdintent::reads_marker(keys, warm)) return keys;
    fs::Fs& f = fs::instance();
    if (f.mount() != fs::Err::Ok) return keys;   // never formats here: the ordinary init does, after the Rd is decided
    const Intent in = rdintent::load(f);
    const bootmode::Mode mode = rdintent::boot_mode(keys, warm, in);
    g_pending = mode != keys;
    if (g_pending) g_found = in;
    return mode;
}

bool pending() { return g_pending; }
const Intent& found() { return g_found; }

void early() {
    if (g_pending && g_found.cable.kind != ctcable::Kind::Off) fusb302::self_power_early_ra();
}

void arm() {
    if (!g_pending) return;
    K2LOG("[RDI] comeback boot: %s pin=%u cable=%u/%u\n", kind_name(g_found.kind), static_cast<unsigned>(g_found.pin),
          static_cast<unsigned>(g_found.cable.kind), static_cast<unsigned>(g_found.cable.seq));
    if (g_found.cable.kind == ctcable::Kind::Off) return;
    g_spoof = arm_identity(g_found.cable);
    if (g_spoof && fusb302::ra_set(true)) return;
    K2LOG("[RDI] identity not armed (spoof=%d); Rd kept, no Ra\n", g_spoof ? 1 : 0);
    if (g_spoof) emk_spoof::disarm();
    g_spoof = false;
    fusb302::ra_set(false);   // drops the early Ra too
}

bool judge_step(std::uint32_t ms) {
    if (!g_pending) return false;
    rdintent::Watch w;
    w.ms = ms;
    w.caps_seen = fusb302::sink_status().state != pd::SinkState::Idle;
    if (!w.caps_seen && ms >= rdintent::kNoRpMs && !g_rp_seen && ms - g_rp_look >= kRpLookEveryMs) {
        g_rp_look = ms;
        g_rp_seen = fusb302::rp_on_line();
    }
    w.rp_seen = g_rp_seen;
    switch (rdintent::judge(w)) {
    case rdintent::Verdict::Wait: return false;
    case rdintent::Verdict::Vouched:
        g_pending = false;
        K2LOG("[RDI] comeback vouched after %u ms (caps seen)\n", static_cast<unsigned>(ms));
        return false;
    case rdintent::Verdict::Release: break;
    }
    K2LOG("[RDI] comeback released after %u ms (%s): Rd, Ra, spoof and record dropped\n", static_cast<unsigned>(ms),
          g_rp_seen ? "no capabilities" : "no source");
    release();
    return true;
}

void poll() {
    if (g_pending) return;
    const std::uint8_t pin = fusb302::pin_rd();
    rdintent::Follower::Sample s;
    s.pinned = pin == 1 || pin == 2;
    s.pin = s.pinned ? pin : 0;
    s.ready = fusb302::sink_status().state == pd::SinkState::Ready;
    s.host = usb::configured();
    s.cable = g_run_cable;
    if (g_follower.update(s) == rdintent::Follower::Act::Apply) g_dirty = true;
    if (!g_dirty || pdtrace::since_activity_ms() < kQuietMs) return;
    g_dirty = false;
    const Intent& want = g_follower.wanted();
    const std::uint32_t t0 = systick::millis();
    const bool ok = rdintent::save(fs::instance(), want);
    K2LOG("[RDI] marker %s: %s pin=%u cable=%u/%u (%s, %u ms)\n", want.kind == IntentKind::None ? "removed" : "written",
          kind_name(want.kind), static_cast<unsigned>(want.pin), static_cast<unsigned>(want.cable.kind),
          static_cast<unsigned>(want.cable.seq), ok ? "ok" : "FAILED", static_cast<unsigned>(systick::millis() - t0));
}

void set_run_cable(ctcable::Choice cable) { g_run_cable = cable; }

bool begin_reattach(ctcable::Choice cable) {
    const std::uint8_t pin = fusb302::pin_rd();
    if (pin != 2) return false;
    const Intent in{IntentKind::Reattach, pin, ctcable::canonical(cable)};
    if (!rdintent::save(fs::instance(), in)) return false;
    return rdintent::load(fs::instance()) == in;
}

void end_reattach() {
    g_follower.forget();   // the volume holds the Reattach: the follower's next look writes the session over it
    g_dirty = false;
}

}  // namespace k2::rdcomeback
