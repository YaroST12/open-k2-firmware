// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USART2 + DMA1 channel 6 driver, see k2/drivers/uart2.hpp. STM32F1-style registers at the Artery addresses
// (USART2 0x40004400, DMA1 0x40020000, APB1 clock 120 MHz).
#include "k2/core/log.hpp"
#include "k2/drivers/uart2.hpp"

#include <array>

#include "k2/drivers/adc.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::uart2 {

namespace {

using reg::R32;

struct UsartRegs {
    R32 STS, DT, BAUDR, CTRL1, CTRL2, CTRL3;
    static constexpr std::uint32_t STS_IDLEF = 1u << 4, STS_RDBF = 1u << 5, STS_TDBE = 1u << 7, STS_TDC = 1u << 6;
    static constexpr std::uint32_t CTRL1_REN = 1u << 2, CTRL1_TEN = 1u << 3, CTRL1_UEN = 1u << 13;
    static constexpr std::uint32_t CTRL3_DMAR = 1u << 6;
};
struct DmaChannel { R32 CTRL, DTCNT, PADDR, MADDR, reserved; };
struct DmaRegs {
    R32 STS, CLR;
    DmaChannel ch[7];
    static constexpr std::uint32_t CTRL_EN = 1u << 0, CTRL_CIRC = 1u << 5, CTRL_MINC = 1u << 7;
    static constexpr std::uint32_t STS_GIF6 = 1u << 20, CLR_ALL6 = 0xFu << 20;
};
static_assert(sizeof(DmaChannel) == 0x14);

inline UsartRegs& usart() { return reg::at<UsartRegs>(0x40004400); }
inline DmaRegs& dma1() { return reg::at<DmaRegs>(0x40020000); }

constexpr std::uint32_t kApb1enUsart2 = 1u << 17, kAhbenDma1 = 1u << 0;
constexpr std::uint32_t kApb1Hz = 120'000'000;   // APB1 runs at /2 (crm.cpp); a 60 MHz divider doubled every baud
constexpr std::uint32_t kBaudDiv = kApb1Hz / kBaud;   // 1041: 0.03 % fast

alignas(4) std::array<std::uint8_t, kRingBytes> g_ring;
std::size_t g_tail;
bool g_claimed;
bool g_idle;
Errors g_err;
std::uint32_t g_err_prev;

std::size_t head() { return kRingBytes - dma1().ch[5].DTCNT; }

}  // namespace

bool claimed() { return g_claimed; }

bool claim(const Config& cfg) {
    if (g_claimed) return false;
    adc::release_dpdm_pins();   // scan off, PA2/PA3 no longer analog
    reg::Crm& c = reg::crm();
    c.APB2EN |= reg::Crm::APB2EN_GPIOA;
    c.APB1EN |= kApb1enUsart2;
    c.AHBEN |= kAhbenDma1;
    pins::adc_dm.write(true);   // TX idles high
    pins::adc_dm.mode(reg::Mode::AfPp50);
    pins::adc_dp.mode(reg::Mode::InFloating);

    UsartRegs& u = usart();
    u.CTRL1 = 0;
    u.BAUDR = cfg.baud >= 1200 && cfg.baud <= 230400 ? kApb1Hz / cfg.baud : kBaudDiv;
    u.CTRL2 = (cfg.stop_bits == 1 ? 0u : 2u) << 12;   // default 2 stop bits (STOP field 0x2000)
    u.CTRL3 = UsartRegs::CTRL3_DMAR;
    DmaChannel& d = dma1().ch[5];
    d.CTRL = 0;
    dma1().CLR = DmaRegs::CLR_ALL6;
    d.PADDR = reinterpret_cast<std::uintptr_t>(&u.DT);
    d.MADDR = reinterpret_cast<std::uintptr_t>(g_ring.data());
    d.DTCNT = kRingBytes;
    d.CTRL = DmaRegs::CTRL_MINC | DmaRegs::CTRL_CIRC | DmaRegs::CTRL_EN;
    g_tail = 0;
    g_idle = false;
    g_err = {};
    g_err_prev = 0;
    u.CTRL1 = UsartRegs::CTRL1_UEN | UsartRegs::CTRL1_TEN | UsartRegs::CTRL1_REN;
    g_claimed = true;
    K2LOG("[UART2] claimed PA2/PA3, ADC scan paused\n");
    return true;
}

void release() {
    if (!g_claimed) return;
    usart().CTRL1 = 0;
    usart().CTRL3 = 0;
    dma1().ch[5].CTRL = 0;
    g_claimed = false;
    adc::reclaim_dpdm_pins();
    K2LOG("[UART2] released PA2/PA3, ADC scan restored\n");
}

bool tx(std::span<const std::uint8_t> bytes) {
    if (!g_claimed) return false;
    UsartRegs& u = usart();
    for (const std::uint8_t b : bytes) {
        const std::uint32_t t0 = systick::millis();
        while (!(u.STS & UsartRegs::STS_TDBE)) {
            if (systick::millis() - t0 > 5) return false;
        }
        u.DT = b;
    }
    const std::uint32_t t0 = systick::millis();
    while (!(u.STS & UsartRegs::STS_TDC)) {
        if (systick::millis() - t0 > 5) return false;
    }
    return true;
}

std::size_t rx(std::span<std::uint8_t> out) {
    if (!g_claimed) return 0;
    UsartRegs& u = usart();
    const std::uint32_t sts = u.STS;
    tally_errors(sts, g_err_prev, g_err);   // the DMA's next DT read clears the flags
    if (sts & UsartRegs::STS_IDLEF) {   // cleared by reading STS then DT
        (void)u.DT;
        g_idle = true;
    }
    const std::size_t h = head();
    std::size_t n = 0;
    while (g_tail != h && n < out.size()) {
        out[n++] = g_ring[g_tail];
        g_tail = (g_tail + 1) % kRingBytes;
    }
    return n;
}

Errors errors() { return g_err; }

bool idle_seen() {
    const bool v = g_idle;
    g_idle = false;
    return v;
}

}  // namespace k2::uart2
