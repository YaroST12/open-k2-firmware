// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ADC1 + DMA1 channel 1 driver, see k2/drivers/adc.hpp. Register layout is the STM32F1 one at the Artery
// addresses (ADC1 0x40012400, DMA1 0x40020000). Channel roles: k2/drivers/adc_calc.hpp.
#include "k2/core/log.hpp"
#include "k2/drivers/adc.hpp"

#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::adc {

namespace {

using reg::R32;

struct AdcRegs {
    R32 STS, CTRL1, CTRL2, SPT1, SPT2;
    R32 PCDTO[4], VMHB, VMLB;
    R32 OSQ1, OSQ2, OSQ3, PSQ;
    R32 PDT[4];
    R32 ODT;

    static constexpr std::uint32_t STS_EC = 1u << 1;
    static constexpr std::uint32_t CTRL1_SCAN = 1u << 8;
    static constexpr std::uint32_t CTRL2_ADON = 1u << 0, CTRL2_CONT = 1u << 1, CTRL2_RSTCAL = 1u << 3,
                                   CTRL2_CAL = 1u << 2, CTRL2_DMAEN = 1u << 8, CTRL2_EXTTRIG = 1u << 20,
                                   CTRL2_EXTSEL_SWSTART = 7u << 17, CTRL2_SWSTART = 1u << 22,
                                   CTRL2_TSVREN = 1u << 23;
};
static_assert(offsetof(AdcRegs, ODT) == 0x4C);
inline AdcRegs& adc1() { return reg::at<AdcRegs>(0x40012400); }

struct DmaChannel { R32 CTRL, DTCNT, PADDR, MADDR, reserved; };
struct DmaRegs {
    R32 STS, CLR;
    DmaChannel ch[7];

    static constexpr std::uint32_t CTRL_EN = 1u << 0, CTRL_CIRC = 1u << 5, CTRL_MINC = 1u << 7,
                                   CTRL_PSIZE16 = 1u << 8, CTRL_MSIZE16 = 1u << 10, CTRL_PRIO_HIGH = 2u << 12;
    static constexpr std::uint32_t STS_TC1 = 1u << 1;
};
static_assert(offsetof(DmaRegs, ch) == 0x08);
inline DmaRegs& dma1() { return reg::at<DmaRegs>(0x40020000); }

constexpr std::uint32_t kApb2enAdc1 = 1u << 9, kAhbenDma1 = 1u << 0;
constexpr std::uint32_t kCfgAdcDivMask = (3u << 14) | (1u << 28);
constexpr std::uint32_t kCfgAdcDiv8 = 3u << 14;     // ADCDIV = 0b011: PCLK2 / 8 = 15 MHz

constexpr std::uint8_t kRippleChannel = 1;
constexpr std::uint32_t kSample239 = 7;

alignas(4) std::uint16_t g_dma[kScanSweeps * kSlots];
std::optional<Reading> g_reading;
std::uint32_t g_start_ms;
bool g_ready;
bool g_released;   // PA2/PA3 belong to another driver: no scan, no conversions

void set_sample_time(std::uint8_t channel, std::uint32_t code) {
    AdcRegs& a = adc1();
    R32& r = channel < 10 ? a.SPT2 : a.SPT1;
    const unsigned shift = (channel < 10 ? channel : channel - 10) * 3u;
    r = (r & ~(7u << shift)) | (code << shift);
}

void set_sequence(std::span<const std::uint8_t> channels) {
    AdcRegs& a = adc1();
    std::uint32_t sq3 = 0, sq2 = 0;
    for (std::size_t i = 0; i < channels.size(); i++) {
        if (i < 6) sq3 |= std::uint32_t{channels[i]} << (5 * i);
        else sq2 |= std::uint32_t{channels[i]} << (5 * (i - 6));
    }
    a.OSQ3 = sq3;
    a.OSQ2 = sq2;
    a.OSQ1 = static_cast<std::uint32_t>(channels.size() - 1) << 20;
}

bool wait_flag(const volatile std::uint32_t& reg, std::uint32_t mask, bool want_set, std::uint32_t ms) {
    const std::uint32_t t0 = systick::millis();
    while (((reg & mask) != 0) != want_set) {
        if (systick::millis() - t0 > ms) return false;
    }
    return true;
}

void stop() {
    dma1().ch[0].CTRL &= ~DmaRegs::CTRL_EN;
    AdcRegs& a = adc1();
    a.CTRL2 &= ~(AdcRegs::CTRL2_ADON | AdcRegs::CTRL2_DMAEN | AdcRegs::CTRL2_CONT);
    dma1().CLR = 0xF;   // channel 1 flags
}

void start_scan() {
    AdcRegs& a = adc1();
    DmaChannel& d = dma1().ch[0];
    d.CTRL = 0;
    d.PADDR = reinterpret_cast<std::uintptr_t>(&a.ODT);
    d.MADDR = reinterpret_cast<std::uintptr_t>(g_dma);
    d.DTCNT = kScanSweeps * kSlots;
    d.CTRL = DmaRegs::CTRL_MINC | DmaRegs::CTRL_PSIZE16 | DmaRegs::CTRL_MSIZE16 | DmaRegs::CTRL_PRIO_HIGH |
             DmaRegs::CTRL_CIRC | DmaRegs::CTRL_EN;
    for (const std::uint8_t ch : kScanChannel) set_sample_time(ch, kSample239);
    set_sequence(kScanChannel);
    a.CTRL1 = AdcRegs::CTRL1_SCAN;
    a.CTRL2 = AdcRegs::CTRL2_ADON | AdcRegs::CTRL2_CONT | AdcRegs::CTRL2_DMAEN | AdcRegs::CTRL2_EXTTRIG |
              AdcRegs::CTRL2_EXTSEL_SWSTART | AdcRegs::CTRL2_TSVREN;
    systick::delay_us(5);   // ADC wake-up time
    a.CTRL2 |= AdcRegs::CTRL2_SWSTART;
    g_start_ms = systick::millis();
}

bool calibrate() {
    AdcRegs& a = adc1();
    a.CTRL2 = AdcRegs::CTRL2_ADON | AdcRegs::CTRL2_TSVREN;
    systick::delay_us(20);
    a.CTRL2 |= AdcRegs::CTRL2_RSTCAL;
    if (!wait_flag(a.CTRL2, AdcRegs::CTRL2_RSTCAL, false, 5)) return false;
    a.CTRL2 |= AdcRegs::CTRL2_CAL;
    return wait_flag(a.CTRL2, AdcRegs::CTRL2_CAL, false, 5);
}

}  // namespace

bool init() {
    if (g_ready) return true;   // the Rd comeback samples the aux rail early: the boot trace's init then finds the scan running
    reg::Crm& c = reg::crm();
    c.APB2EN |= reg::Crm::APB2EN_GPIOA | kApb2enAdc1;
    c.AHBEN |= kAhbenDma1;
    c.CFG = (c.CFG & ~kCfgAdcDivMask) | kCfgAdcDiv8;

    for (const reg::Pin pin : {pins::adc_aux, pins::adc_ripple, pins::adc_cc1, pins::adc_cc2, pins::adc_dp,
                               pins::adc_dm, pins::adc_ntc})
        pin.mode(reg::Mode::Analog);

    g_ready = calibrate();
    if (g_ready) start_scan();
    K2LOG("[ADC] init %s adcclk=%dHz ring=%dx%d\n", g_ready ? "ok" : "CAL FAILED",
                 static_cast<int>(kAdcClockHz), static_cast<int>(kScanSweeps), static_cast<int>(kSlots));
    return g_ready;
}

void release_dpdm_pins() {
    if (g_released) return;
    g_released = true;
    if (g_ready) stop();
    g_reading.reset();
    pins::adc_dm.mode(reg::Mode::InFloating);
    pins::adc_dp.mode(reg::Mode::InFloating);
}

void reclaim_dpdm_pins() {
    if (!g_released) return;
    pins::adc_dm.mode(reg::Mode::Analog);
    pins::adc_dp.mode(reg::Mode::Analog);
    g_released = false;
    if (g_ready) start_scan();
}

void poll() {
    if (!g_ready || g_released) return;
    // 100 sweeps x 8 slots x 252 cycles / 15 MHz = 13.4 ms until the ring has been filled once.
    if (systick::millis() - g_start_ms < 15) return;
    asm volatile("" ::: "memory");
    Raw avg;
    average_sweeps(g_dma, avg);
    g_reading = make_reading(avg);
}

std::optional<Reading> read() { return g_reading; }

bool latest_cc(std::uint16_t& cc1_raw, std::uint16_t& cc2_raw) {
    if (!g_ready || g_released || systick::millis() - g_start_ms < 15) return false;
    const std::uint32_t left = dma1().ch[0].DTCNT;          // transfers still to go in this lap of the ring
    if (left == 0 || left > kScanSweeps * kSlots) return false;
    const std::uint32_t written = kScanSweeps * kSlots - left;
    const std::uint32_t sweep = (written / kSlots + kScanSweeps - 1) % kScanSweeps;   // last one that finished
    asm volatile("" ::: "memory");
    cc1_raw = g_dma[sweep * kSlots + Cc1];
    cc2_raw = g_dma[sweep * kSlots + Cc2];
    return true;
}

bool latest_dpdm(std::int32_t& dp_mv, std::int32_t& dm_mv) {
    if (!g_ready || g_released || systick::millis() - g_start_ms < 15) return false;
    const std::uint32_t left = dma1().ch[0].DTCNT;
    if (left == 0 || left > kScanSweeps * kSlots) return false;
    const std::uint32_t written = kScanSweeps * kSlots - left;
    const std::uint32_t newest = (written / kSlots + kScanSweeps - 1) % kScanSweeps;
    asm volatile("" ::: "memory");
    constexpr std::uint32_t kAvg = 4;                       // the newest four sweeps, ~0.5 ms of history
    std::uint32_t dp = 0, dm = 0, vref = 0;
    for (std::uint32_t k = 0; k < kAvg; k++) {
        const std::uint32_t s = (newest + kScanSweeps - k) % kScanSweeps;
        dp += g_dma[s * kSlots + Dp];
        dm += g_dma[s * kSlots + Dm];
        vref += g_dma[s * kSlots + Vref];
    }
    const auto vr = static_cast<std::uint16_t>(vref / kAvg);
    dp_mv = to_mv(static_cast<std::uint16_t>(dp / kAvg), vr);
    dm_mv = to_mv(static_cast<std::uint16_t>(dm / kAvg), vr);
    return true;
}

bool sweep_all(RawAll& out) {
    if (!g_ready || g_released) return false;
    stop();
    AdcRegs& a = adc1();
    a.CTRL1 = 0;
    a.CTRL2 = AdcRegs::CTRL2_ADON | AdcRegs::CTRL2_TSVREN | AdcRegs::CTRL2_EXTTRIG | AdcRegs::CTRL2_EXTSEL_SWSTART;
    systick::delay_us(5);
    bool ok = true;
    for (std::size_t i = 0; i < kRawChannels && ok; i++) {
        const std::uint8_t ch = kRawChannelIds[i];
        set_sample_time(ch, kSample239);
        set_sequence({&ch, 1});
        for (int pass = 0; pass < 2 && ok; pass++) {   // the second conversion settles after the mux change
            a.CTRL2 |= AdcRegs::CTRL2_SWSTART;
            ok = wait_flag(a.STS, AdcRegs::STS_EC, true, 5);
            out[i] = static_cast<std::uint16_t>(a.ODT);   // reading ODT clears EC
        }
    }
    stop();
    start_scan();
    return ok;
}

void log_raw() {
    RawAll r;
    if (!sweep_all(r)) {
        K2LOG("[ADC] raw sweep failed\n");
        return;
    }
    K2LOG("[ADC] raw ch0=%d ch1=%d ch2=%d ch3=%d ch4=%d\n", r[0], r[1], r[2], r[3], r[4]);
    K2LOG("[ADC] raw ch5=%d ch6=%d ch7=%d ch16=%d ch17=%d\n", r[5], r[6], r[7], r[8], r[9]);
}

std::uint32_t capture_ripple(std::span<std::uint16_t, kRippleSamples> out, std::uint8_t sample_code) {
    if (!g_ready || g_released) return 0;
    if (sample_code > 7) sample_code = 7;
    stop();
    AdcRegs& a = adc1();
    DmaChannel& d = dma1().ch[0];
    d.CTRL = 0;
    d.PADDR = reinterpret_cast<std::uintptr_t>(&a.ODT);
    d.MADDR = reinterpret_cast<std::uintptr_t>(out.data());
    d.DTCNT = kRippleSamples;
    set_sample_time(kRippleChannel, sample_code);
    set_sequence({&kRippleChannel, 1});
    a.CTRL1 = 0;
    a.CTRL2 = AdcRegs::CTRL2_ADON | AdcRegs::CTRL2_CONT | AdcRegs::CTRL2_DMAEN | AdcRegs::CTRL2_EXTTRIG |
              AdcRegs::CTRL2_EXTSEL_SWSTART | AdcRegs::CTRL2_TSVREN;
    // Not circular: the channel stops after 512 halfwords and raises TC1.
    d.CTRL = DmaRegs::CTRL_MINC | DmaRegs::CTRL_PSIZE16 | DmaRegs::CTRL_MSIZE16 | DmaRegs::CTRL_PRIO_HIGH |
             DmaRegs::CTRL_EN;
    systick::delay_us(5);
    a.CTRL2 |= AdcRegs::CTRL2_SWSTART;
    const bool done = wait_flag(dma1().STS, DmaRegs::STS_TC1, true, 50);
    stop();
    start_scan();
    return done ? sample_period_ns(sample_code, kAdcClockHz) : 0;
}

}  // namespace k2::adc
