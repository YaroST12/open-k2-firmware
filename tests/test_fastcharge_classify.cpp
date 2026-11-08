// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <string_view>

#include "k2/fastcharge/fastcharge_classify.hpp"
#include "test_util.h"

using namespace k2::fastcharge;

// Fill both rings with a constant line voltage (mV), return the verdict at the given VBUS.
static Protocol run(int dp_mv, int dm_mv, int vbus_mv) {
    Ring r;
    for (int i = 0; i < 50; i++) r.push(dp_mv, dm_mv);
    return classify(r, dp_mv, dm_mv, vbus_mv);
}

static void test_ring() {
    Ring r;
    CHECK_EQ(r.full(), false);
    for (int i = 0; i < 49; i++) r.push(600, 0);
    CHECK_EQ(r.full(), false);
    r.push(600, 0);
    CHECK_EQ(r.full(), true);
    CHECK_EQ(r.dp[0], 6);              // 600 mV -> 6 dV
    r.push(3299, 2799);                // truncates like uint8_t(V * 10)
    CHECK_EQ(r.dp[0], 32);
    CHECK_EQ(r.dm[0], 27);
    CHECK_EQ(r.dp[1], 6);
    r.push(-50, 99999);                // clamps: negative -> 0, huge -> 255
    CHECK_EQ(r.dp[1], 0);
    CHECK_EQ(r.dm[1], 255);
    r.clear();
    CHECK_EQ(r.full(), false);
}

static void test_bucket() {
    std::array<std::uint8_t, 50> b{};
    b.fill(0);   CHECK_EQ(bucket(b), 0);
    b.fill(6);   CHECK_EQ(bucket(b), 1);
    b.fill(27);  CHECK_EQ(bucket(b), 2);
    b.fill(33);  CHECK_EQ(bucket(b), 3);
    b.fill(20);  CHECK_EQ(bucket(b), 8);
    // band edges
    b.fill(3);   CHECK_EQ(bucket(b), 0);
    b.fill(4);   CHECK_EQ(bucket(b), 1);
    b.fill(10);  CHECK_EQ(bucket(b), 1);
    b.fill(11);  CHECK_EQ(bucket(b), 8);
    b.fill(23);  CHECK_EQ(bucket(b), 8);
    b.fill(24);  CHECK_EQ(bucket(b), 2);
    b.fill(29);  CHECK_EQ(bucket(b), 2);
    b.fill(30);  CHECK_EQ(bucket(b), 3);
    // digital pulse train: low and high mixed
    for (int i = 0; i < 50; i++) b[i] = (i & 1) ? 33 : 0;
    CHECK_EQ(bucket(b), 4);
    for (int i = 0; i < 50; i++) b[i] = (i & 1) ? 27 : 6;
    CHECK_EQ(bucket(b), 4);
    // high rail with ripple, low rail with ripple
    for (int i = 0; i < 50; i++) b[i] = (i & 1) ? 33 : 27;
    CHECK_EQ(bucket(b), 5);
    for (int i = 0; i < 50; i++) b[i] = (i & 1) ? 6 : 0;
    CHECK_EQ(bucket(b), 6);
    // anything else: mid rail mixed with a low or high band
    for (int i = 0; i < 50; i++) b[i] = (i & 1) ? 20 : 0;
    CHECK_EQ(bucket(b), 7);
    for (int i = 0; i < 50; i++) b[i] = (i & 1) ? 20 : 33;
    CHECK_EQ(bucket(b), 7);
}

static void test_five_volt_band() {
    CHECK_EQ(run(0, 0, 3000), Protocol::NoInput);        // below the 4.0 V presence level
    CHECK_EQ(run(600, 3300, 5000), Protocol::Qc3);
    CHECK_EQ(run(600, 2700, 5000), Protocol::Qc3);
    CHECK_EQ(run(600, 0, 5000), Protocol::AfcFcp);       // QC2.0 5V needs an active trigger
    CHECK_EQ(run(2700, 2700, 5100), Protocol::Apple24);
    CHECK_EQ(run(2700, 2000, 5100), Protocol::Apple21);
    CHECK_EQ(run(2000, 2700, 5100), Protocol::Apple10);
    CHECK_EQ(run(3300, 600, 5000), Protocol::Usb2Hi);
    CHECK_EQ(run(0, 3300, 5000), Protocol::Usb11Low);
    CHECK_EQ(run(1200, 1200, 5000), Protocol::Sam20);
    CHECK_EQ(run(3300, 3300, 5000), Protocol::Dcp5V);    // BC1.2 DCP: D+ shorted to D-
    CHECK_EQ(run(3300, 3200, 5000), Protocol::Dcp5V);
    CHECK_EQ(run(600, 600, 5000), Protocol::Dcp5V);      // shorted at 0.6 V, within 0.2 V and >= 0.4 V
    CHECK_EQ(run(0, 0, 5000), Protocol::NoQuick);        // nothing on the data lines
    CHECK_EQ(run(1500, 600, 5000), Protocol::NoQuick);    // mid-rail D+ against 0.6 V D-: not shorted, not a known divider
    CHECK_EQ(run(2000, 0, 5000), Protocol::NoQuick);
    // pulse trains on D-
    Ring r;
    for (int i = 0; i < 50; i++) r.push(600, (i & 1) ? 3300 : 0);
    CHECK_EQ(classify(r, 600, 3300, 5000), Protocol::Scp);
    Ring w;
    for (int i = 0; i < 50; i++) w.push(3300, (i & 1) ? 3300 : 0);
    CHECK_EQ(classify(w, 3300, 3300, 5000), Protocol::VoocWarp);
}

static void test_other_bands() {
    // 5.5 .. 7.7 V
    CHECK_EQ(run(600, 3300, 6000), Protocol::Qc3);
    CHECK_EQ(run(0, 0, 6000), Protocol::PdPe);
    Ring r;
    for (int i = 0; i < 50; i++) r.push(600, (i & 1) ? 3300 : 0);
    CHECK_EQ(classify(r, 600, 0, 6500), Protocol::Scp);
    // 7.7 .. 9.9 V
    CHECK_EQ(run(600, 3300, 9000), Protocol::Qc3);
    CHECK_EQ(run(3300, 600, 9000), Protocol::Qc2_9V);
    CHECK_EQ(run(600, 0, 9000), Protocol::AfcFcp);
    CHECK_EQ(run(0, 0, 9000), Protocol::PdMtk);
    Ring s;
    for (int i = 0; i < 50; i++) s.push(3300, (i & 1) ? 3300 : 0);
    CHECK_EQ(classify(s, 3300, 0, 9000), Protocol::Svooc);
    // 10.5 .. 13.7 V
    CHECK_EQ(run(600, 600, 12000), Protocol::Qc2_12V);
    CHECK_EQ(run(600, 3300, 12000), Protocol::Qc3);
    CHECK_EQ(run(600, 0, 12000), Protocol::AfcFcp);
    CHECK_EQ(run(0, 0, 12000), Protocol::PdMtk);
    // 18.5 .. 21.5 V
    CHECK_EQ(run(3300, 3300, 20000), Protocol::Qc2_20V);
    CHECK_EQ(run(600, 3300, 20000), Protocol::Qc3);
    CHECK_EQ(run(0, 0, 20000), Protocol::Pd);
    // the gaps between bands read as plain PD
    CHECK_EQ(run(600, 600, 10000), Protocol::Pd);
    CHECK_EQ(run(600, 600, 16000), Protocol::Pd);
    CHECK_EQ(run(600, 600, 28000), Protocol::Pd);
    // band edges
    CHECK_EQ(run(0, 0, 5499), Protocol::NoQuick);
    CHECK_EQ(run(0, 0, 5500), Protocol::PdPe);
    CHECK_EQ(run(0, 0, 7699), Protocol::PdPe);
    CHECK_EQ(run(0, 0, 7700), Protocol::PdMtk);
    CHECK_EQ(run(0, 0, 9900), Protocol::PdMtk);
    CHECK_EQ(run(0, 0, 10500), Protocol::PdMtk);
    CHECK_EQ(run(0, 0, 13700), Protocol::PdMtk);
    CHECK_EQ(run(0, 0, 13701), Protocol::Pd);
    CHECK_EQ(run(0, 0, 18500), Protocol::Pd);
    CHECK_EQ(run(0, 0, 3999), Protocol::NoInput);
    CHECK_EQ(run(0, 0, 4000), Protocol::NoQuick);
}

static void test_pending() {
    Ring r;
    for (int i = 0; i < 49; i++) r.push(600, 0);
    CHECK_EQ(classify(r, 600, 0, 5000), Protocol::Pending);   // window not full yet
    CHECK_EQ(classify(r, 600, 0, 2000), Protocol::NoInput);   // no VBUS is known without a window
}

static void test_names() {
    CHECK_STR(name(Protocol::Qc3), "QC3.0");
    CHECK_STR(name(Protocol::Qc2_9V), "QC2.0 9V");
    CHECK_STR(name(Protocol::Qc2_12V), "QC2.0 12V");
    CHECK_STR(name(Protocol::Qc2_20V), "QC2.0 20V");
    CHECK_STR(name(Protocol::Qc2_5V), "QC2.0 5V");
    CHECK_STR(name(Protocol::AfcFcp), "AFC FCP");
    CHECK_STR(name(Protocol::Scp), "SCP");
    CHECK_STR(name(Protocol::Apple24), "APPLE 2.4A");
    CHECK_STR(name(Protocol::Apple21), "APPLE 2.1A");
    CHECK_STR(name(Protocol::Apple10), "APPLE 1.0A");
    CHECK_STR(name(Protocol::Usb2Hi), "USB2.0 HI");
    CHECK_STR(name(Protocol::VoocWarp), "VOOC WARP");
    CHECK_STR(name(Protocol::Svooc), "SVOOC");
    CHECK_STR(name(Protocol::Usb11Low), "USB1.1 LOW");
    CHECK_STR(name(Protocol::Sam20), "SAM 2.0A");
    CHECK_STR(name(Protocol::Dcp5V), "DCP 5V");
    CHECK_STR(name(Protocol::NoQuick), "NO QUICK");
    CHECK_STR(name(Protocol::PdPe), "PD PE");
    CHECK_STR(name(Protocol::PdMtk), "PD MTK");
    CHECK_STR(name(Protocol::Pd), "PD");
    CHECK_STR(name(Protocol::NoInput), "NO INPUT");
    CHECK_STR(name(Protocol::Pending), "...");
    CHECK_STR(name(static_cast<Protocol>(200)), "?");
}

static void test_wire_values() {
    // The numbers go out in GET_STATE; tools/k2_remote.py FC_NAMES lists them in this order.
    CHECK_EQ(static_cast<int>(Protocol::Pending), 0);
    CHECK_EQ(static_cast<int>(Protocol::NoInput), 1);
    CHECK_EQ(static_cast<int>(Protocol::Qc2_9V), 6);
    CHECK_EQ(static_cast<int>(Protocol::Apple24), 11);
    CHECK_EQ(static_cast<int>(Protocol::Pd), 21);
}

static void test_short_names() {
    // The Home screen has room for 8 characters.
    for (int i = 0; i <= static_cast<int>(Protocol::UsbData); i++)
        CHECK_EQ(std::string_view(short_name(static_cast<Protocol>(i))).size() <= 8, true);
    CHECK_STR(short_name(Protocol::Apple24), "APL 2.4A");
    CHECK_STR(short_name(Protocol::Usb11Low), "USB1 LOW");
    CHECK_STR(short_name(Protocol::VoocWarp), "WARP");
    CHECK_STR(short_name(Protocol::Qc2_12V), "QC2 12V");
    CHECK_STR(short_name(Protocol::NoInput), "NO INPUT");
    CHECK_STR(short_name(Protocol::Qc3), "QC3.0");
    CHECK_STR(short_name(static_cast<Protocol>(200)), "?");
}

static void test_idle() {
    // idle: PA7 high, PB0 low, PB1 high, PB2 low
    CHECK_EQ(idle_mismatch(true, false, true, false), 0);
    CHECK_EQ(idle_mismatch(false, false, true, false), 1);
    CHECK_EQ(idle_mismatch(true, true, true, false), 2);
    CHECK_EQ(idle_mismatch(true, false, false, false), 4);
    CHECK_EQ(idle_mismatch(true, false, true, true), 8);      // what i2c.cpp leaves PB2 at today
    CHECK_EQ(idle_mismatch(false, true, false, true), 15);
}

// Hardware finding: a Pixel on a PD3.1 contract (8.9 V, 1.35 A) showed "AFC FCP" with D+ = 819 mV,
// D- = 1 mV: the phone's own data pull-up, not a legacy handshake.
static void test_pd_likely() {
    Ring r;
    for (int i = 0; i < 50; i++) r.push(820, 1);
    // Pixel after a K2 reboot mid-contract: caps missed, CC active, 8.9 V, 1.35 A.
    CHECK_EQ(classify(r, 820, 1, 8898, false, true), Protocol::PdLikely);
    CHECK_EQ(classify(r, 820, 1, 8898, true, true), Protocol::Pd);        // caps seen: plain PD
    CHECK_EQ(classify(r, 820, 1, 3000, false, true), Protocol::NoInput);
    Ring part;
    part.push(820, 1);
    CHECK_EQ(classify(part, 820, 1, 8898, false, true), Protocol::PdLikely);   // no window needed
    CHECK_EQ(classify(r, 820, 1, 8898, false, false), Protocol::UsbData);
    CHECK_STR(name(Protocol::PdLikely), "PD?");
    CHECK_STR(short_name(Protocol::PdLikely), "PD?");
}

static void test_pd_and_usb_data() {
    Ring r;
    for (int i = 0; i < 50; i++) r.push(819, 1);
    CHECK_EQ(classify(r, 819, 1, 8900), Protocol::UsbData);
    CHECK_EQ(classify(r, 819, 1, 5000), Protocol::UsbData);
    CHECK_EQ(classify(r, 819, 1, 12000), Protocol::UsbData);
    CHECK_EQ(classify(r, 819, 1, 8900, true), Protocol::Pd);          // a PD contract wins
    CHECK_EQ(classify(r, 819, 1, 3000, true), Protocol::NoInput);     // no VBUS is still no input
    Ring part;                                                         // window not full yet
    part.push(819, 1);
    CHECK_EQ(classify(part, 819, 1, 9000), Protocol::Pending);
    CHECK_EQ(classify(part, 819, 1, 9000, true), Protocol::Pd);
    Ring q;                                                            // nothing recognisable at 5 V
    for (int i = 0; i < 50; i++) q.push(2700, 2700);
    CHECK_EQ(classify(q, 2700, 2700, 5100, true), Protocol::Pd);
    // A charger-driven 0.6 V on D+ (below the data pull-up window) is still AFC/FCP.
    CHECK_EQ(run(600, 0, 9000), Protocol::AfcFcp);
    CHECK_EQ(run(650, 20, 5000), Protocol::AfcFcp);
    CHECK_EQ(run(1000, 0, 9000), Protocol::UsbData);                   // 1.0 V: pull-up window edge
    CHECK_EQ(run(1090, 0, 9000), Protocol::AfcFcp);                    // above the window: unchanged
    CHECK_STR(name(Protocol::UsbData), "USB DATA");
    CHECK_STR(short_name(Protocol::UsbData), "USB DATA");
}

int main() {
    test_ring();
    test_bucket();
    test_five_volt_band();
    test_other_bands();
    test_pending();
    test_names();
    test_wire_values();
    test_short_names();
    test_idle();
    test_pd_and_usb_data();
    test_pd_likely();
    return test_report("test_fastcharge_classify");
}
