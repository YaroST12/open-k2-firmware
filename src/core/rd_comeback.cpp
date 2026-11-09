// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Rd comeback on the real hardware, see k2/core/rd_comeback.hpp.
#include "k2/core/rd_comeback.hpp"

#include "k2/core/log.hpp"
#include "k2/core/rescan_bridge.hpp"
#include "k2/core/storage.hpp"
#include "k2/drivers/adc.hpp"
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

using rdintent::Canary;
using rdintent::Intent;
using rdintent::IntentKind;

// The SRAM canary armed with the marker on every save (rdintent::arm_canary; the decision table in rd_intent.hpp). .noinit:
// link.ld places it before _sbss, so Reset_Handler's zero loop does not touch it - it survives warm resets and whatever dark
// period (VBUS loss) SRAM itself survives. Whether it survives the comeback's short dark (~165 ms) on this board is the
// hardware measurement of ROADMAP "Stale Rd marker"; sysreset::wipe_and_reset (the DFU entry) zeroes all of SRAM, so a
// reflashed K2 never carries one.
__attribute__((section(".noinit"), used)) Canary g_canary;

// One owner for every write of /rd.rec: the record and its canary go together. A save that lands on the volume arms the canary
// with the same record (save() also returns true when the volume already holds it, so the canary is re-armed then too);
// None removes the record and clears the canary.
bool save_marker(const Intent& in) {
    if (!rdintent::save(fs::instance(), in)) return false;
    if (in.kind == IntentKind::None) rdintent::clear_canary(g_canary);
    else rdintent::arm_canary(g_canary, in);
    return true;
}

constexpr std::uint32_t kQuietMs = 300;      // PD link quiet before a flash write (as the black box persister)
constexpr std::uint32_t kRpLookEveryMs = 50; // the Rp comparator look is a few I2C transactions: not on every pass

Intent g_found;                  // what begin() read (None unless this boot is a comeback)
bool g_pending = false;          // comeback boot, no verdict yet
bool g_vouched = false;          // the judge vouched for this comeback (caps seen in time)
bool g_spoof = false;            // arm() armed the recorded identity
bool g_rp_seen = false;
std::uint32_t g_rp_look = 0;
rdintent::Follower g_follower;
bool g_dirty = false;            // the follower's record is not on the volume yet
ctcable::Choice g_run_cable{};

const char* kind_name(IntentKind k) {
    return k == IntentKind::Session ? "session" : k == IntentKind::Reattach ? "reattach" : k == IntentKind::Rescan ? "rescan" : "none";
}

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

// The aux rail, once, before the Rd is decided: a USB host feeds it ~5 V, a comeback boot (powered through the charger's Rp by our own
// Rd) has it dead. Only called when a record exists. The ADC starts here (its init is idempotent) and the first full ring takes ~15 ms;
// no reading in 40 ms = unknown = not a host (the comeback is kept, as before).
bool host_powered_now() {
    if (!adc::init()) return false;
    const std::uint32_t t0 = systick::millis();
    while (systick::millis() - t0 < 40) {
        adc::poll();
        if (const auto r = adc::read()) return rdintent::host_powered(static_cast<int>(r->aux_mv));
    }
    return false;
}

void release() {
    g_pending = false;
    if (g_spoof) emk_spoof::disarm();
    g_spoof = false;
    fusb302::ra_set(false);
    fusb302::self_power_drop();
    save_marker({});
}

}  // namespace

bootmode::Mode begin(bootmode::Mode keys, bool warm) {
    if (!rdintent::reads_marker(keys, warm)) return keys;
    fs::Fs& f = fs::instance();
    if (f.mount() != fs::Err::Ok) return keys;   // never formats here: the ordinary init does, after the Rd is decided
    const Intent in = rdintent::load(f);
    // The canary says whether this cold boot is the marker's comeback or a stale leftover: it survived this reset's dark period
    // and vouches for exactly this record. A bare unplug leaves the marker but not the canary (the K2 sat dead and SRAM decayed),
    // so that next plain boot stays passive - no Rd for whatever is on the port now (hardware 2026-11-09, build b00178c).
    const bool canary = in.kind != IntentKind::None && rdintent::canary_matches(g_canary, in);
    if (in.kind != IntentKind::None && !canary)
        K2LOG("[RDI] record %s pin=%u ignored: the SRAM canary did not survive this boot (a stale record)\n", kind_name(in.kind),
              static_cast<unsigned>(in.pin));
    const bool host = in.kind != IntentKind::None && host_powered_now();
    if (host) K2LOG("[RDI] record %s pin=%u ignored: a USB host powers this boot (aux rail up), plain boot stays passive\n", kind_name(in.kind),
                    static_cast<unsigned>(in.pin));
    const bootmode::Mode mode = rdintent::boot_mode(keys, warm, in, host, canary);
    g_pending = mode != keys;
    if (g_pending) g_found = in;
    return mode;
}

bool pending() { return g_pending; }
const Intent& found() { return g_found; }
bool planned_reattach() { return rdintent::planned_reattach(g_found.kind); }

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
        g_vouched = true;
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
    s.sink_downstream = fusb302::sink_downstream();
    s.cable = g_run_cable;
    if (g_follower.update(s) == rdintent::Follower::Act::Apply) g_dirty = true;
    if (!g_dirty || pdtrace::since_activity_ms() < kQuietMs) return;
    g_dirty = false;
    const Intent& want = g_follower.wanted();
    const std::uint32_t t0 = systick::millis();
    const bool ok = save_marker(want);
    K2LOG("[RDI] marker %s: %s pin=%u cable=%u/%u (%s, %u ms)\n", want.kind == IntentKind::None ? "removed" : "written",
          kind_name(want.kind), static_cast<unsigned>(want.pin), static_cast<unsigned>(want.cable.kind),
          static_cast<unsigned>(want.cable.seq), ok ? "ok" : "FAILED", static_cast<unsigned>(systick::millis() - t0));
}

void set_run_cable(ctcable::Choice cable) { g_run_cable = cable; }

void drop_identity() {
    if (!g_spoof) return;
    emk_spoof::disarm();
    g_spoof = false;
    fusb302::ra_set(false);
    K2LOG("[RDI] comeback identity dropped: no run took it over\n");
}

bool begin_reattach(ctcable::Choice cable) {
    const std::uint8_t pin = fusb302::pin_rd();
    if (pin != 2) return false;
    const Intent in{IntentKind::Reattach, pin, ctcable::canonical(cable)};
    if (!save_marker(in)) return false;
    return rdintent::load(fs::instance()) == in;
}

void end_reattach() {
    g_follower.forget();   // the volume holds the Reattach: the follower's next look writes the session over it
    g_dirty = false;
}

bool begin_rescan() {
    const std::uint8_t pin = fusb302::pin_rd();
    if (pin != 1 && pin != 2) return false;
    const Intent in{IntentKind::Rescan, pin, {}};
    if (!save_marker(in)) return false;
    return rdintent::load(fs::instance()) == in;
}

bool rescan_landing() { return rescanbridge::lands_on_page(g_found.kind, g_vouched ? rdintent::Verdict::Vouched : rdintent::Verdict::Wait); }

}  // namespace k2::rdcomeback
