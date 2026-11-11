// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Gathers the boot-info Snapshot from the drivers' read-only records (see k2/core/bootinfo_hw.hpp).
#include "k2/core/bootinfo_hw.hpp"

#include <cstddef>

#include "k2/drivers/adc.hpp"
#include "k2/pd/boot_ra.hpp"
#include "k2/core/boottrace_store.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/sysreset.hpp"
#include "k2/core/remote_proto.hpp"
#include "k2/core/version.hpp"

namespace k2::bootinfo {
namespace {

bootmode::Mode g_mode = bootmode::Mode::Plain;
std::uint8_t g_keys = 0, g_pa = 0;

std::uint16_t clamp16(std::int32_t v) { return std::uint16_t(v < 0 ? 0 : v > 65535 ? 65535 : v); }

// NUL terminated copy that is cut, never overrun.
template <std::size_t N>
void copy_str(char (&out)[N], const char* src) {
    std::size_t n = 0;
    while (src[n] && n + 1 < N) { out[n] = src[n]; n++; }
    out[n] = '\0';
}

}  // namespace

void note_boot(bootmode::Mode mode, std::uint8_t keys, std::uint8_t pa) {
    g_mode = mode;
    g_keys = keys;
    g_pa = pa;
}

bool boot_key_held() {
    switch (g_mode) {
    case bootmode::Mode::OkRdCc1: return !pins::key_ok.read();
    case bootmode::Mode::PlusRdCc2: return !pins::key_plus.read();
    default: return false;
    }
}

void collect(Snapshot& s) {
    s = Snapshot{};
    copy_str(s.fw, kFwVersion);      // the strings PING answers
    copy_str(s.git, kGitHash);
    s.proto = remote::kProtoVersion;
    s.mode = g_mode;
    s.keys = g_keys;
    s.pa = g_pa;
    s.reset_flags = sysreset::reset_flags();
    s.warm = sysreset::warm(s.reset_flags);
    const fusb302::BootRecord r = fusb302::boot_record();
    s.mark_read = r.mark_read;
    s.mark = r.mark;
    s.pin_initial = r.pin_initial;
    s.pin_now = fusb302::pin_rd();
    s.pin_boot = r.pin_boot;
    s.inherited = r.inherited;
    s.ra_on = fusb302::ra_active();
    s.ra_why = std::uint8_t(bootra::last());
    const rdcomeback::BootLook& l = rdcomeback::boot_look();
    s.ri_looked = l.looked;
    s.ri_kind = l.kind == rdintent::IntentKind::Session ? 's' : l.kind == rdintent::IntentKind::Reattach ? 'a'
              : l.kind == rdintent::IntentKind::Rescan ? 'r' : 0;
    s.ri_pin = l.pin;
    s.ri_canary = l.canary;
    s.ri_host = l.host;
    s.sram_words = l.sram_words;
    s.bp_boots = session::boots();
    s.bp_prev = session::previous().has_value();
    s.move = boottrace::move_record();
    const unsigned n = fusb302::boot_note_count();
    for (unsigned i = 0; i < n && s.note_n < s.notes.size(); i++) {
        const fusb302::BootNote b = fusb302::boot_note_at(i);
        s.notes[s.note_n++] = {b.ms, b.kind, b.val};
    }
    s.samples = std::uint16_t(boottrace::sample_count());
    s.t_ms = systick::millis();
    if (const auto v = power::vbus_fast_mv()) s.vbus_mv = clamp16(*v);
    if (const auto a = adc::read()) {
        s.cc1_mv = clamp16(a->cc1_mv);
        s.cc2_mv = clamp16(a->cc2_mv);
    }
    s.plus_now = !pins::key_plus.read();
}

}  // namespace k2::bootinfo
