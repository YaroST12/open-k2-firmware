// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// D+/D- edge sniffer, see k2/fastcharge/dpsniff.hpp.
#include "k2/core/log.hpp"
#include "k2/fastcharge/dpsniff.hpp"

#include <new>

#include "k2/core/critical.hpp"
#include "k2/core/hot.h"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/mode_ram.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::dpsniff {

void isr_body();

namespace {

K2_MODE_RAM_SNIFF std::uint32_t g_buf[kCapacity];

edges::Recorder g_rec;
edges::StreamRing g_ring;
volatile bool g_stream;
bool g_synth;
std::uint32_t g_synth_rate, g_synth_next, g_synth_lcg;
std::uint8_t g_stream_idx;
std::uint32_t g_stream_bytes;
edges::Session g_sess;
Status g_status;
std::uint32_t g_arm_cycles, g_arm_us;
volatile bool g_live;     // the ISR records only while set

// Inline VOOC monitor: the decoder runs in the ISR (no edge buffer); the main loop ticks it and copies the summary with
// interrupts masked. g_mon_ev collects the decoder's events between two monitor_poll() calls.
alignas(vooc::Decoder) std::uint8_t g_dec_raw[sizeof(vooc::Decoder)];
bool g_dec_inited;
volatile bool g_mon;
volatile bool g_mon_paused;   // data-traffic back-off: the ISR does not feed the decoder
volatile unsigned g_mon_ev;
bool g_mon_disabled;

K2_HOT vooc::Decoder& dec() {   // called from the edge ISR
    if (!g_dec_inited) {
        new (g_dec_raw) vooc::Decoder();
        g_dec_inited = true;
    }
    return *std::launder(reinterpret_cast<vooc::Decoder*>(g_dec_raw));
}

// Edge-storm guard (k2/fastcharge/edge_guard.hpp), shared by every user of the ISR. The ISR masks its lines and sets
// g_storm on a trip; the main loop (storm_service) ends a sniff session, pauses the monitor and logs.
edges::StormGuard g_guard;
volatile bool g_storm;
edges::Backoff g_back;
std::uint16_t g_storms;       // since boot, monitor and sniff
edges::Storm g_last_storm;

// The SuperVOOC trigger's hook (hook_claim .. hook_end). g_hook is what the ISR calls; g_hook_owned keeps the
// sniffer's other users out and the EXTI lines on between hook_mute() and hook_listen().
Hook volatile g_hook;
volatile bool g_hook_storm;
bool g_hook_owned;
bool g_hook_recorded;         // the recorder was armed for this claim

constexpr std::uint32_t kLines = (1u << 7) | (1u << 2);   // EXTI7 = PA7 (D+), EXTI2 = PB2 (D-)

}  // namespace

K2_HOT void isr_body() {
    const std::uint32_t c = dpdm::cycles();
    auto& ex = reg::exti();
    ex.PR = kLines;
    if (g_guard.hit(c)) {   // data traffic: our lines off now, the main loop does the rest
        ex.IMR &= ~kLines;
        ex.PR = kLines;
        g_storm = true;
        if (g_hook) g_hook_storm = true;
        return;
    }
    const Hook hook = g_hook;
    if (!g_live && !g_mon && !hook) return;
    const std::uint32_t w = edges::pack(c, pins::dp_dm_sw2.read(), pins::sw_pa7.read());
    if (hook) hook(w);   // the trigger first: its bit timing runs from the edge (it may wait up to 50 us here)
    if (g_mon && !g_mon_paused) g_mon_ev = g_mon_ev | dec().push(w);
    if (!g_live) return;
    if (g_stream) g_ring.push(w);
    else g_rec.push(w);
}

namespace {

void exti_on() {
    reg::crm().APB2EN |= reg::Crm::APB2EN_AFIO | reg::Crm::APB2EN_GPIOA | reg::Crm::APB2EN_GPIOB;
    auto& af = reg::afio();
    af.EXTICR[1] = (af.EXTICR[1] & ~(0xFu << 12)) | (0u << 12);   // line 7: port A
    af.EXTICR[0] = (af.EXTICR[0] & ~(0xFu << 8)) | (1u << 8);     // line 2: port B
    auto& ex = reg::exti();
    Critical cs;   // the ISR masks IMR on a storm
    g_guard.reset(dpdm::cycles());
    g_storm = false;
    ex.RTSR |= kLines;
    ex.FTSR |= kLines;
    ex.PR = kLines;
    ex.IMR |= kLines;
    reg::nvic().ISER[0] = (1u << 8) | (1u << 23);
}
void exti_off() {
    if (g_mon || g_hook) return;   // the inline monitor or the SuperVOOC trigger still listens
    reg::nvic().ICER[0] = (1u << 8) | (1u << 23);
    auto& ex = reg::exti();
    Critical cs;
    ex.IMR &= ~kLines;
    ex.PR = kLines;
}

void storm_service(std::uint32_t now_ms);

void finish(edges::End why) {
    g_live = false;
    if (g_stream) {
        g_stream = false;
        g_synth = false;
        exti_off();
        g_status.state = State::Idle;
        g_status.reason = why;
        g_status.edges = 0;
        g_status.stored = false;
        K2LOG("[SNIFF] stream end pushed=%u overruns=%u\n", static_cast<unsigned>(g_ring.pushed()),
                     static_cast<unsigned>(g_ring.overruns()));
        return;
    }
    exti_off();
    g_status.state = State::Idle;
    g_status.reason = why;
    g_status.edges = std::uint16_t(g_rec.size());
    g_status.overflow = g_rec.overflowed();
    g_status.stored = false;
    g_status.total = g_rec.total();
    if (g_rec.size() != 0) {
        const auto w = g_rec.words();
        // The Black Box slot (2 KB) takes the first ~1400 edges; the whole buffer stays readable through read_record().
        const std::size_t kept = pdtrace::edges_store(w, g_arm_cycles, g_arm_us);
        g_status.stored = true;
        const std::uint32_t span_us = edges::word_us(w[w.size() - 1], g_arm_cycles, g_arm_us) - edges::word_us(w[0], g_arm_cycles, g_arm_us);
        static_assert(kCapacity <= 4294);
        K2LOG("[SNIFF] end=%u kept=%u total=%u overflow=%u slot=%u first_to_last_us=%u rate=%u/s\n", unsigned(why),
                     unsigned(w.size()), unsigned(g_rec.total()), unsigned(g_rec.overflowed()), unsigned(kept),
                     static_cast<unsigned>(span_us),
                     static_cast<unsigned>(span_us ? std::uint32_t(w.size()) * 1000000u / span_us : 0));
    } else {
        K2LOG("[SNIFF] end=%u no edges\n", unsigned(why));
    }
}

}  // namespace

ArmResult arm() {
    if (g_hook_owned) return ArmResult::Busy;   // the SuperVOOC trigger drives D-
    if (g_status.state != State::Idle) return ArmResult::Ok;
    if (!mode_ram::claim(mode_ram::Tenant::Sniff)) {
        K2LOG("[SNIFF] refused: the shared RAM belongs to the USB drive or net function; switch the drive off in Tools > USB link and replug\n");
        return ArmResult::NoRam;
    }
    g_rec.init(g_buf, kCapacity);
    g_sess.arm(systick::millis());
    dpdm::set_dp(dpdm::Drive::Sniff);
    dpdm::set_dm(dpdm::Drive::Sniff);
    g_arm_us = systick::micros();
    g_arm_cycles = dpdm::cycles();
    g_status = Status{State::Armed, edges::End::None, 0, false, false, 0};
    g_live = true;
    exti_on();
    K2LOG("[SNIFF] armed (passive: D+ PA7 and D- PB1/PB2 floating)\n");
    return ArmResult::Ok;
}

ArmResult arm_stream() {
    if (g_hook_owned || g_status.state != State::Idle) return ArmResult::Busy;
    if (!mode_ram::claim(mode_ram::Tenant::Sniff)) {
        K2LOG("[SNIFF] refused: the shared RAM belongs to the USB drive or net function\n");
        return ArmResult::NoRam;
    }
    g_ring.init(g_buf, kCapacity);
    g_stream_idx = 0;
    g_stream_bytes = 0;
    g_stream = true;
    g_synth = false;
    g_sess.arm(systick::millis());
    dpdm::set_dp(dpdm::Drive::Sniff);
    dpdm::set_dm(dpdm::Drive::Sniff);
    g_arm_us = systick::micros();
    g_arm_cycles = dpdm::cycles();
    g_status = Status{State::Recording, edges::End::None, 0, false, false, 0};
    g_live = true;
    exti_on();
    K2LOG("[SNIFF] stream armed (passive)\n");
    return ArmResult::Ok;
}

ArmResult arm_synth(std::uint32_t rate) {
    if (g_hook_owned || g_status.state != State::Idle) return ArmResult::Busy;
    if (!mode_ram::claim(mode_ram::Tenant::Sniff)) return ArmResult::NoRam;
    g_ring.init(g_buf, kCapacity);
    g_stream_idx = 0;
    g_stream_bytes = 0;
    g_stream = true;
    g_synth = true;
    if (g_synth_lcg == 0) g_synth_lcg = 12345;
    g_synth_rate = rate < 1 ? 1 : rate > 100000 ? 100000 : rate;
    g_synth_next = dpdm::cycles();
    g_sess.arm(systick::millis());
    g_status = Status{State::Recording, edges::End::None, 0, false, false, 0};
    g_live = true;
    return ArmResult::Ok;
}

bool streaming() { return g_stream && g_sess.armed(); }
StreamStats stream_stats() { return {g_ring.pushed(), g_ring.overruns(), g_stream_bytes}; }

void stream_service() {
    if (!g_stream || !g_synth) return;
    // Synthetic edges, mean spacing 240e6/rate cycles, jittered 0.5x..1.5x; at most 64 per call.
    const std::uint32_t mean = 240000000u / g_synth_rate;
    std::uint32_t now = dpdm::cycles();
    for (int i = 0; i < 64 && std::int32_t(now - g_synth_next) >= 0; i++) {
        g_synth_lcg = g_synth_lcg * 1664525u + 1013904223u;
        g_ring.push(edges::pack(g_synth_next, (g_synth_lcg >> 20) & 1, (g_synth_lcg >> 21) & 1));
        g_synth_next += mean / 2 + (g_synth_lcg >> 8) % (mean ? mean : 1);
    }
    if (std::int32_t(now - g_synth_next) > 64 * std::int32_t(mean)) g_synth_next = now;   // fell far behind: resync
}

std::size_t stream_frame(std::span<std::uint8_t> out) {
    if (!g_stream || g_ring.pending() == 0 || out.size() < edges::kFrameMax) return 0;
    std::uint32_t w[24];
    const std::size_t n = g_ring.peek(w);
    std::uint8_t pl[edges::kChunkBytes + 8];
    std::size_t len = 0;
    const std::size_t used = edges::encode_chunk({w, n}, 0, g_stream_idx, pl, len);
    if (used == 0) return 0;
    g_stream_idx++;
    g_ring.drop(used);   // words that did not fit the payload stay queued for the next frame
    const std::size_t f = edges::frame({pl, len}, out);
    g_stream_bytes += std::uint32_t(f);
    return f;
}

void cancel() {
    if (g_sess.cancel() != edges::End::None) finish(edges::End::Cancel);
}

void poll(std::uint32_t now_ms, std::int32_t vbus_mv) {
    storm_service(now_ms);
    if (!g_sess.armed() || g_stream) return;
    const std::size_t n = g_rec.size();
    const edges::End e = g_sess.poll(now_ms, n, g_rec.full(), vbus_mv);
    if (e != edges::End::None) { finish(e); return; }
    g_status.state = g_sess.started() ? State::Recording : State::Armed;
    g_status.edges = std::uint16_t(n);
}

bool armed() { return g_sess.armed(); }
Status status() { return g_status; }

std::size_t read_record(std::size_t from, std::span<std::uint8_t> out, std::size_t& used, std::size_t& total) {
    used = 0;
    total = g_sess.armed() ? 0 : g_rec.size();
    if (g_sess.armed() || from >= total) return 0;
    const auto w = g_rec.words();
    return edges::read_record(w, from, std::uint8_t(pdtrace::Type::Edges), edges::word_us(w[from], g_arm_cycles, g_arm_us), out, used);
}

bool selftest() {
    if (!g_sess.armed() || g_hook_owned) return false;
    // Not passive by design: only with nothing attached. D+ low then high, twice, 100 us apart.
    for (int i = 0; i < 2; i++) {
        dpdm::set_dp(dpdm::Drive::V0);
        dpdm::delay_us(100);
        dpdm::set_dp(dpdm::Drive::V3p3);
        dpdm::delay_us(100);
    }
    dpdm::set_dp(dpdm::Drive::Sniff);
    return true;
}

// ---- the SuperVOOC trigger's hook ----

ArmResult hook_claim() {
    if (g_hook_owned || g_sess.armed() || g_stream || g_mon) return ArmResult::Busy;
    if (!mode_ram::claim(mode_ram::Tenant::Sniff)) {
        K2LOG("[SNIFF] trigger refused: the shared RAM belongs to the USB drive or net function\n");
        return ArmResult::NoRam;
    }
    g_hook_owned = true;
    g_hook_recorded = false;
    return ArmResult::Ok;
}

void hook_listen(Hook h) {
    if (!g_hook_owned) return;
    dpdm::set_dp(dpdm::Drive::Sniff);
    dpdm::set_dm(dpdm::Drive::Sniff);
    if (!g_hook_recorded) {   // the first listen of the claim records the session (the wake burst stays out of it)
        g_hook_recorded = true;
        g_rec.init(g_buf, kCapacity);
        g_sess.arm(systick::millis());
        g_arm_us = systick::micros();
        g_arm_cycles = dpdm::cycles();
        g_status = Status{State::Armed, edges::End::None, 0, false, false, 0};
        g_live = true;
    }
    {
        Critical cs;
        g_hook_storm = false;
        g_hook = h;
    }
    exti_on();
}

void hook_mute() {
    {
        Critical cs;
        g_hook = nullptr;
    }
    exti_off();
}

void hook_end() {
    if (!g_hook_owned) return;
    {
        Critical cs;
        g_hook = nullptr;
    }
    g_hook_owned = false;
    if (g_sess.cancel() != edges::End::None) finish(edges::End::Cancel);   // stores the capture, EXTI off
    else exti_off();
}

bool hooked() { return g_hook_owned; }
bool hook_storm() { return g_hook_storm; }
std::uint32_t line_word() { return edges::pack(dpdm::cycles(), pins::dp_dm_sw2.read(), pins::sw_pa7.read()); }

// ---- inline VOOC monitor ----

namespace {

void monitor_log(unsigned ev, const vooc::Summary& s) {
    if (ev & vooc::kSessionEnd) {
        K2LOG("[VOOC] session %u end frames=%u pairs=%u hs=%u/%u ident=0x%02X/0x%03X cmds=%u+%u r82=0x%03X min=0x%03X "
              "max=0x%03X n=%u p82_us=%u\n",
              unsigned(s.sessions), unsigned(s.frames), unsigned(s.pairs), unsigned(s.handshake), unsigned(s.hs_pairs),
              unsigned(s.ident), unsigned(s.ident_reply), unsigned(s.ncmds), unsigned(s.other), unsigned(s.r82_last),
              unsigned(s.r82_min), unsigned(s.r82_max), unsigned(s.r82_n), unsigned(s.p82_mean_us()));
        if (s.stalled())   // raw facts: the polled command and the silent replies in a row, no guessed cause
            K2LOG("[VOOC] session %u end: stalled on 0x%02X, %u silent replies\n", unsigned(s.sessions),
                  unsigned(s.stall_cmd), unsigned(s.stall_n));
    }
    if (ev & vooc::kSessionStart)
        K2LOG("[VOOC] session %u start (inline, passive) wakes=%u partials=%u edges=%u\n", unsigned(s.sessions),
              unsigned(s.wakes), unsigned(s.partials), unsigned(s.edges));
}

// The ISR tripped the guard: end a sniff session, pause the monitor, one log line each. Then the monitor's back-off:
// when the pause is over, unmask the lines for a probe. Without a storm, close the guard's expired windows.
void storm_service(std::uint32_t now_ms) {
    edges::Storm st;
    {
        Critical cs;
        if (g_storm) {
            g_storm = false;
            st = g_guard.last();   // the guard stays tripped (and the lines masked) until exti_on() or a probe
        } else {
            g_guard.tick(dpdm::cycles());
        }
    }
    if (st.edges != 0) {
        if (g_storms < 0xFFFF) ++g_storms;
        g_last_storm = st;
        const unsigned n = st.edges, ms = st.ms();
        if (g_live && !g_synth) {
            K2LOG("[SNIFF] stopped: data traffic (%u edges in %u ms)\n", n, ms);
            g_sess.cancel();
            finish(edges::End::Storm);
        }
        if (g_mon) {
            unsigned ev;
            vooc::Summary s;
            {
                Critical cs;
                g_mon_paused = true;
                ev = g_mon_ev | dec().end(dpdm::cycles());
                g_mon_ev = 0;
                s = dec().summary();
            }
            monitor_log(ev, s);
            const unsigned wait = g_back.trip(now_ms);
            K2LOG("[VOOC] monitor paused: data traffic (%u edges in %u ms), retry in %u ms\n", n, ms, wait);
        }
    }
    if (g_mon && g_back.poll(now_ms)) {   // the pause is over: the lines listen again for a probe
        auto& ex = reg::exti();
        Critical cs;
        g_mon_paused = false;
        g_guard.reset(dpdm::cycles());
        ex.PR = kLines;
        ex.IMR |= kLines;
    }
}

void monitor_start() {
    {
        Critical cs;
        dec().reset();
        g_mon_ev = 0;
        g_mon_paused = false;
    }
    g_back.reset();
    dpdm::set_dp(dpdm::Drive::Sniff);   // floating inputs: nothing is driven, no pull
    dpdm::set_dm(dpdm::Drive::Sniff);
    g_mon = true;
    exti_on();
    K2LOG("[VOOC] monitor on (inline: D+ PA7 / D- PB1, PB2 floating)\n");
}

void monitor_stop(bool lines_free) {
    unsigned ev;
    vooc::Summary s;
    {
        Critical cs;
        g_mon = false;
        g_mon_paused = false;
        ev = g_mon_ev | dec().end(dpdm::cycles());   // closes the open session
        g_mon_ev = 0;
        s = dec().summary();
    }
    g_back.reset();   // a new device starts from the 1 s step
    monitor_log(ev, s);
    const bool sniffing = g_sess.armed() || g_stream;
    if (!sniffing) exti_off();
    // Only undo our own state: an engine that took the lines meanwhile owns them; a Black Box arm keeps the Sniff state.
    if (lines_free && !sniffing) dpdm::idle();
    K2LOG("[VOOC] monitor off sessions=%u frames=%u lines=%s\n", unsigned(s.sessions), unsigned(s.frames),
          lines_free && !sniffing ? "idle" : "kept");
}

}  // namespace

void monitor_poll(bool wanted, bool lines_free) {
    if (wanted && !g_mon && !g_hook_owned) monitor_start();
    else if (!wanted && g_mon) monitor_stop(lines_free);
    storm_service(systick::millis());
    if (!g_mon) return;
    unsigned ev;
    vooc::Summary s;
    {
        Critical cs;
        ev = g_mon_ev | dec().tick(dpdm::cycles());
        g_mon_ev = 0;
        if (ev) s = dec().summary();
    }
    if (ev) monitor_log(ev, s);
}

bool monitoring() { return g_mon; }

bool monitor_yield() {
    if (!g_mon) return false;
    monitor_stop(true);   // the trigger takes the lines next: the passive idle is its start state
    return true;
}

void monitor_enable(bool on) { g_mon_disabled = !on; }
bool monitor_enabled() { return !g_mon_disabled; }

MonitorInfo monitor_info() {
    MonitorInfo m;
    m.enabled = !g_mon_disabled;
    m.guard.paused = g_mon && g_back.paused();
    m.guard.storms = g_storms;
    m.guard.edges = std::uint16_t(g_last_storm.edges > 0xFFFF ? 0xFFFF : g_last_storm.edges);
    const std::uint32_t ms = g_last_storm.edges ? g_last_storm.ms() : 0;
    m.guard.ms = std::uint16_t(ms > 0xFFFF ? 0xFFFF : ms);
    m.guard.retry_ms = g_back.retry_in(systick::millis());
    Critical cs;
    m.running = g_mon;
    m.s = dec().summary();
    return m;
}

vooc::Live monitor_live() {
    Critical cs;
    return vooc::live_of(g_mon && !g_mon_paused, dec().summary());
}

}  // namespace k2::dpsniff

extern "C" K2_HOT void EXTI2_IRQHandler() { k2::dpsniff::isr_body(); }
extern "C" K2_HOT void EXTI9_5_IRQHandler() { k2::dpsniff::isr_body(); }
