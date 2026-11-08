// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// D+/D- edge sniffer, see k2/fastcharge/dpsniff.hpp.
#include "k2/core/log.hpp"
#include "k2/fastcharge/dpsniff.hpp"

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
std::uint32_t g_synth_rate, g_synth_next, g_synth_lcg = 12345;
std::uint8_t g_stream_idx;
std::uint32_t g_stream_bytes;
edges::Session g_sess;
Status g_status;
std::uint32_t g_arm_cycles, g_arm_us;
volatile bool g_live;     // the ISR records only while set

constexpr std::uint32_t kLines = (1u << 7) | (1u << 2);   // EXTI7 = PA7 (D+), EXTI2 = PB2 (D-)

}  // namespace

void isr_body() {
    const std::uint32_t c = dpdm::cycles();
    reg::exti().PR = kLines;
    if (!g_live) return;
    const std::uint32_t w = edges::pack(c, pins::dp_dm_sw2.read(), pins::sw_pa7.read());
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
    ex.RTSR |= kLines;
    ex.FTSR |= kLines;
    ex.PR = kLines;
    ex.IMR |= kLines;
    reg::nvic().ISER[0] = (1u << 8) | (1u << 23);
}
void exti_off() {
    reg::nvic().ICER[0] = (1u << 8) | (1u << 23);
    auto& ex = reg::exti();
    ex.IMR &= ~kLines;
    ex.PR = kLines;
}

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
        K2LOG("[SNIFF] end=%u kept=%u total=%u overflow=%u slot=%u first_to_last_us=%u rate=%u/s\n", unsigned(why),
                     unsigned(w.size()), unsigned(g_rec.total()), unsigned(g_rec.overflowed()), unsigned(kept),
                     static_cast<unsigned>(span_us),
                     static_cast<unsigned>(span_us ? std::uint64_t(w.size()) * 1000000u / span_us : 0));
    } else {
        K2LOG("[SNIFF] end=%u no edges\n", unsigned(why));
    }
}

}  // namespace

ArmResult arm() {
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
    if (g_status.state != State::Idle) return ArmResult::Busy;
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
    if (g_status.state != State::Idle) return ArmResult::Busy;
    if (!mode_ram::claim(mode_ram::Tenant::Sniff)) return ArmResult::NoRam;
    g_ring.init(g_buf, kCapacity);
    g_stream_idx = 0;
    g_stream_bytes = 0;
    g_stream = true;
    g_synth = true;
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
    if (!g_sess.armed()) return false;
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

}  // namespace k2::dpsniff

extern "C" void EXTI2_IRQHandler() { k2::dpsniff::isr_body(); }
extern "C" void EXTI9_5_IRQHandler() { k2::dpsniff::isr_body(); }
