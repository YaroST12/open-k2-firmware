// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/adc_calc.hpp"
#include "test_util.h"

using namespace k2::adc;

static void test_scan_channels() {
    /* Hardware witness (2026-11-02, Baseus CJ21): driving D+ moved ADC ch3 (PA3), driving D- moved ch2 (PA2);
       ch4/ch5 held the CC levels (Rp 3A 1.56 V on the attached CC). PA3 is D+, PA2 is D-. */
    CHECK_EQ(kScanChannel[Dp], 3);
    CHECK_EQ(kScanChannel[Dm], 2);
    CHECK_EQ(kScanChannel[Cc1], 4);
    CHECK_EQ(kScanChannel[Cc2], 5);
    CHECK_EQ(kScanChannel[Aux], 0);
    CHECK_EQ(kScanChannel[Ntc], 6);
    CHECK_EQ(kScanChannel[Vref], 17);
    CHECK_EQ(kScanChannel[Mcu], 16);
}

static void test_average_sweeps() {
    /* 3 sweeps x kSlots slots; slot s holds 10*s + sweep, so the mean of slot s is 10*s + 1 */
    std::array<uint16_t, 3 * kSlots> dma{};
    for (size_t sweep = 0; sweep < 3; sweep++)
        for (size_t s = 0; s < kSlots; s++) dma[sweep * kSlots + s] = static_cast<uint16_t>(10 * s + sweep);
    Raw out{};
    average_sweeps(dma, out);
    for (size_t s = 0; s < kSlots; s++) CHECK_EQ(out[s], 10 * s + 1);
    /* rounds to nearest: slot 0 = 0, 0, 1 -> 0.33 -> 0; = 0, 1, 1 -> 0.67 -> 1 */
    std::array<uint16_t, 2 * kSlots> d2{};
    d2[0] = 0; d2[kSlots] = 1;
    average_sweeps(d2, out);   /* mean 0.5 rounds up */
    CHECK_EQ(out[0], 1);
    /* 4095 sums do not overflow 16 bits */
    std::array<uint16_t, 100 * kSlots> full{};
    full.fill(4095);
    average_sweeps(full, out);
    CHECK_EQ(out[Vref], 4095);
    CHECK_EQ(out[Mcu], 4095);
}

static void test_ratiometric_mv() {
    /* VDD 3.3 V: VREFINT 1.2 V reads 1.2/3.3*4095 = 1489 */
    CHECK_EQ(to_mv(2048, 1489), 1651);      /* half scale, 1650.5 mV rounds up */
    CHECK_EQ(to_mv(1489, 1489), 1200);      /* the reference itself */
    CHECK_EQ(to_mv(0, 1489), 0);
    /* the result follows the reference, not a fixed 3.3 V: VDD 3.0 V -> VREFINT 1638 */
    CHECK_EQ(to_mv(2048, 1638), 1500);
    CHECK_EQ(to_mv(100, 0), 0);             /* no reference, no answer */
    CHECK_EQ(vdd_mv(1489), 3300);
    CHECK_EQ(vdd_mv(0), 0);
}

static void test_ntc() {
    /* 10k / B3950 NTC to ground, 10k pull-up to VDD; raw = 4095 * R / (R + 10k), values computed
       from T = 1 / (1/298.15 + ln(R/10k)/3950). */
    Temp t = ntc_temp(2048);
    CHECK_EQ(t.valid, 1);
    CHECK_EQ(t.calibrated, 0);               /* constants are an assumption, not a measured curve */
    CHECK_EQ(t.deci_c >= 247 && t.deci_c <= 253, 1);
    t = ntc_temp(1081);                      /* 50 C */
    CHECK_EQ(t.deci_c >= 495 && t.deci_c <= 505, 1);
    t = ntc_temp(3156);                      /* 0 C */
    CHECK_EQ(t.deci_c >= -5 && t.deci_c <= 5, 1);
    t = ntc_temp(401);                       /* 85 C */
    CHECK_EQ(t.deci_c >= 840 && t.deci_c <= 860, 1);
    /* shorted or open sensor is invalid, not a temperature */
    CHECK_EQ(ntc_temp(0).valid, 0);
    CHECK_EQ(ntc_temp(4095).valid, 0);
    CHECK_EQ(ntc_temp(5).valid, 0);
}

static void test_make_reading() {
    Raw raw{};
    raw[Aux] = 1000; raw[Cc1] = 100; raw[Cc2] = 1489; raw[Dp] = 2048; raw[Dm] = 745; raw[Ntc] = 2048; raw[Vref] = 1489;
    const Reading r = make_reading(raw);
    CHECK_EQ(r.dp_mv, 1651);
    CHECK_EQ(r.dm_mv, 600);                  /* 745 -> 600 mV */
    CHECK_EQ(r.cc1_mv, 81);
    CHECK_EQ(r.cc2_mv, 1200);
    CHECK_EQ(r.aux_mv, 8865);                /* 1000 * 1.2 / 1489 * 11 V */
    CHECK_EQ(r.vref_raw, 1489);
    CHECK_EQ(r.vdd_mv, 3300);
    CHECK_EQ(r.temp.valid, 1);
}

static void test_mcu_temp() {
    /* AT32F403A internal sensor (ADC ch16): T = (V25 - Vsense) / Avg_Slope + 25 with V25 = 1.28 V and
       Avg_Slope = -4.13 mV/C, i.e. the voltage RISES with temperature. Vsense comes from the
       ratiometric conversion, so VDD drift cancels. */
    const Temp t25 = mcu_temp(1588, 1489);          /* 1588 * 1.2 / 1489 V = 1280 mV */
    CHECK_EQ(t25.valid, 1);
    CHECK_EQ(t25.calibrated, 0);                    /* datasheet typical values, not a measured curve */
    CHECK_EQ(t25.deci_c, 250);
    /* Hardware sample (USB powered, no load): ch16 = 1706, VREFINT = 1486 -> 1378 mV -> 48.7 C */
    CHECK_EQ(mcu_temp(1706, 1486).deci_c, 487);
    /* Colder than 25 C means a lower voltage: 1239 mV = 25 - 41/4.13 = 15.1 C */
    CHECK_EQ(mcu_temp(1537, 1489).deci_c, 151);
    /* Hotter: 1487 mV = 25 + 207/4.13 = 75.1 C */
    CHECK_EQ(mcu_temp(1845, 1489).deci_c, 751);
    /* Live trend under load: raw ch16 rose 1719 -> 1725 while the board warmed, so T must rise. */
    CHECK_EQ(mcu_temp(1725, 1486).deci_c > mcu_temp(1719, 1486).deci_c, 1);
    /* No reference reading, or a voltage no silicon can produce: invalid, never a made-up number. */
    CHECK_EQ(mcu_temp(1706, 0).valid, 0);
    CHECK_EQ(mcu_temp(0, 1489).valid, 0);
    CHECK_EQ(mcu_temp(4095, 1489).valid, 0);
}

static void test_make_reading_mcu() {
    Raw raw{};
    raw[Vref] = 1486; raw[Mcu] = 1706; raw[Ntc] = 4085;
    const Reading r = make_reading(raw);
    CHECK_EQ(r.mcu.valid, 1);
    CHECK_EQ(r.mcu.deci_c, 487);
    CHECK_EQ(r.temp.valid, 0);   /* PA6 rails high: the board NTC channel is not confirmed */
}

static void test_sample_period() {
    /* ADCCLK 15 MHz: (239.5 + 12.5) cycles = 16.8 us, (1.5 + 12.5) cycles = 933 ns */
    CHECK_EQ(sample_period_ns(7, 15000000), 16800);
    CHECK_EQ(sample_period_ns(0, 15000000), 933);
    CHECK_EQ(sample_period_ns(2, 15000000), 1733);   /* 13.5 + 12.5 = 26 cycles */
    CHECK_EQ(sample_period_ns(9, 15000000), 16800);  /* codes above 7 clamp to 7 */
}

static void test_summarize() {
    const std::array<uint16_t, 5> s{100, 4000, 2000, 50, 1850};
    const Stats st = summarize(s);
    CHECK_EQ(st.min, 50);
    CHECK_EQ(st.max, 4000);
    CHECK_EQ(st.mean, 1600);
    CHECK_EQ(summarize({}).max, 0);
}

int main() {
    test_scan_channels();
    test_average_sweeps();
    test_mcu_temp();
    test_make_reading_mcu();
    test_ratiometric_mv();
    test_ntc();
    test_make_reading();
    test_sample_period();
    test_summarize();
    return test_report("test_adc_calc");
}
