// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Transition capture driver, see k2/pd/transcap.hpp.
#include "k2/core/log.hpp"
#include "k2/pd/transcap.hpp"

#include "k2/drivers/adc.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/mode_ram.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::transcap {

namespace {

using Rec = Recorder<kSamples, kMarkers>;

// The ring is an overlay member: zero until claimed, and zeroed again by release (see mode_ram.hpp).
K2_MODE_RAM_TRANS Rec g_rec;

constexpr std::uint32_t kQuietMs = 300;          // link quiet before a flash write (as the black box)
constexpr std::uint32_t kPersistMaxWaitMs = 2000;  // ... but a kept capture waits no longer than this

bool g_held;                 // overlay claimed, sink registered, fast mode on
Pacer g_pacer;               // sample schedule (from the start of each read)
std::uint32_t g_last_ts_us;  // timestamp of the last sample, for the largest gap
bool g_in_poll;              // poll() runs from the main loop, the LCD idle hook and delay_ms()
std::uint32_t g_armed_ms;    // arm() / mark() time, for the arm timeout
std::uint32_t g_frozen_ms;   // when the capture froze
std::uint32_t g_max_gap_us;
bool g_keep;
std::uint32_t g_keep_run;
std::uint16_t g_keep_step;
std::uint32_t g_saved_seq;

Store& store() {
    static Store s(fs::instance());
    return s;
}

std::uint8_t cc8(std::uint16_t raw12) { return std::uint8_t(raw12 >> 4); }

void on_pd_event(std::uint8_t kind, std::uint16_t a, std::uint32_t b) {
    const std::uint32_t t = systick::micros();
    switch (kind) {
    case pdtrace::kEvRx:
    case pdtrace::kEvTx: {
        const PdMark m = classify_pd(kind == pdtrace::kEvRx, a, b);
        if (m.code != Mark::None) g_rec.mark(t, m.code, m.step, m.arg);
        break;
    }
    case pdtrace::kEvReset:
        g_rec.mark(t, mark_of_reset(a == std::uint16_t(pdtrace::ResetKind::HardRx) || a == std::uint16_t(pdtrace::ResetKind::HardTx)));
        break;
    case pdtrace::kEvCc:
        g_rec.mark(t, mark_of_cc(a == std::uint16_t(pdtrace::CcEvent::Attach)), 0, std::uint16_t(b));
        break;
    default: break;
    }
}

void hold_release() {
    if (!g_held) return;
    pdtrace::set_event_sink(nullptr);
    power::fast_end();
    g_rec.clear();   // zero the overlay object before the next tenant sees it
    mode_ram::release(mode_ram::Tenant::Trans);
    g_held = false;
}

void note_freeze() {
    g_frozen_ms = systick::millis();
    const Rec& r = g_rec;
    K2LOG("[TCAP] frozen reason=%s samples=%u markers=%u lost=%u maxgap=%uus\n", reason_name(r.reason()),
          unsigned(r.sample_count()), unsigned(r.marker_count()), unsigned(r.lost_samples()), unsigned(g_max_gap_us));
}

std::uint32_t save_frozen(std::uint32_t run_id, std::uint16_t step) {
    std::uint32_t seq = 0;
    const fs::Err e = store().save(g_rec, run_id, step, seq);
    if (e != fs::Err::Ok) {
        K2LOG("[TCAP] save failed err=%d\n", int(e));
        return 0;
    }
    K2LOG("[TCAP] saved seq=%u run=%u step=%u\n", unsigned(seq), unsigned(run_id), unsigned(step));
    g_saved_seq = seq;
    return seq;
}

}  // namespace

const char* name(ArmResult r) {
    switch (r) {
    case ArmResult::Ok: return "ok";
    case ArmResult::Busy: return "busy";
    default: return "no-monitor";
    }
}

ArmResult arm(std::uint8_t step, std::uint16_t arg) {
    const bool fresh = !g_held;
    if (fresh) {
        if (power::chip() == power::Chip::None) return ArmResult::NoMonitor;
        if (!mode_ram::claim(mode_ram::Tenant::Trans)) return ArmResult::Busy;
    }
    // Also when re-arming over a held (frozen) capture: freezing ended the fast mode.
    if (!power::fast_begin()) {
        if (fresh) mode_ram::release(mode_ram::Tenant::Trans);
        return ArmResult::NoMonitor;
    }
    if (fresh) {
        pdtrace::set_event_sink(on_pd_event);
        g_held = true;
    }
    g_rec.arm();
    g_pacer.reset(systick::micros());
    g_last_ts_us = 0;
    g_armed_ms = systick::millis();
    g_max_gap_us = 0;
    g_keep = false;
    g_saved_seq = 0;
    g_rec.mark(systick::micros(), Mark::Arm, step, arg);
    return ArmResult::Ok;
}

void disarm() {
    g_keep = false;
    hold_release();
}

void disarm_if_armed() {
    if (g_held && g_rec.state() == State::Armed) disarm();
}

bool trigger(Reason r) {
    return g_held && g_rec.trigger(systick::micros(), r);
}

bool triggered() {
    return g_held && g_rec.triggered();
}

void mark(Mark code, std::uint8_t step, std::uint16_t arg) {
    if (!g_held) return;
    g_rec.mark(systick::micros(), code, step, arg);
    if (g_rec.state() == State::Armed) g_armed_ms = systick::millis();
}

void keep(std::uint32_t run_id, std::uint16_t step) {
    if (!g_held || !g_rec.triggered()) return;
    g_keep = true;
    g_keep_run = run_id;
    g_keep_step = step;
}

namespace {

void poll_once() {
    const State st = g_rec.state();
    if (st == State::Armed || st == State::Post) {
        if (g_pacer.take(systick::micros())) {
            std::int32_t mv;
            std::uint16_t c1 = 0, c2 = 0;
            if (power::fast_vbus_mv(mv)) {
                adc::latest_cc(c1, c2);
                const std::uint32_t ts = systick::micros();
                const std::uint32_t gap = ts - g_last_ts_us;
                if (gap > g_max_gap_us && g_rec.sample_count() > 0) g_max_gap_us = gap;
                g_last_ts_us = ts;
                const Sample s{ts, std::uint16_t(mv < 0 ? 0 : mv > 65535 ? 65535 : mv), cc8(c1), cc8(c2)};
                if (g_rec.push(s)) note_freeze();
            }
        }
        if (g_rec.state() == State::Armed && systick::millis() - g_armed_ms >= kArmTimeoutMs) {
            K2LOG("[TCAP] arm timed out\n");
            disarm();
        }
        return;
    }
    if (st == State::Frozen) {
        if (g_keep && g_saved_seq == 0) {
            const std::uint32_t waited = systick::millis() - g_frozen_ms;
            if (pdtrace::since_activity_ms() >= kQuietMs || waited >= kPersistMaxWaitMs) {
                const std::uint32_t seq = save_frozen(g_keep_run, g_keep_step);
                g_keep = false;
                if (seq != 0) hold_release();   // the flash holds it now; the overlay is free again
            }
        }
        // The fast mode has no job once frozen.
        if (g_held && power::fast_active()) power::fast_end();
    }
}

}  // namespace

void poll() {
    if (!g_held || g_in_poll) return;
    g_in_poll = true;
    poll_once();
    g_in_poll = false;
}

Status status() {
    Status s;
    if (!g_held) return s;
    s.state = g_rec.state();
    s.reason = g_rec.reason();
    s.fast = power::fast_active();
    s.keep = g_keep;
    s.samples = std::uint16_t(g_rec.sample_count());
    s.markers = std::uint8_t(g_rec.marker_count());
    s.lost = g_rec.lost_samples();
    if (s.samples > 1) s.span_us = g_rec.sample(s.samples - 1).t_us - g_rec.sample(0).t_us;
    s.max_gap_us = g_max_gap_us;
    if (g_rec.state() == State::Post || g_rec.state() == State::Frozen) s.trigger_age_ms = (systick::micros() - g_rec.trigger_us()) / 1000;
    s.saved_seq = g_saved_seq;
    return s;
}

bool active() { return g_held; }

std::size_t read_live(std::uint32_t off, std::span<std::uint8_t> out) {
    if (!g_held || g_rec.state() != State::Frozen) return 0;
    Encoder<Rec> enc(g_rec);
    std::array<std::uint8_t, 32> skip;
    while (off > 0) {
        const std::size_t want = off < skip.size() ? off : skip.size();
        const std::size_t n = enc.read({skip.data(), want});
        if (n == 0) return 0;
        off -= std::uint32_t(n);
    }
    return enc.read(out);
}

std::uint32_t save_now(std::uint32_t run_id, std::uint16_t step) {
    if (!g_held || g_rec.state() != State::Frozen) return 0;
    return save_frozen(run_id, step);
}

std::size_t list_stored(std::span<Meta> out) { return store().list(out); }
bool find_stored(std::uint32_t run_id, std::uint16_t step, Meta& m) { return store().find(run_id, step, m); }
bool get_stored(std::uint32_t seq, Meta& m) { return store().get(seq, m); }
std::size_t read_stored(std::uint32_t seq, std::uint32_t off, std::span<std::uint8_t> out) {
    std::size_t n = 0;
    if (store().read(seq, off, out, n) != fs::Err::Ok) return 0;
    return n;
}
void clear_stored() { store().clear(); }

}  // namespace k2::transcap
