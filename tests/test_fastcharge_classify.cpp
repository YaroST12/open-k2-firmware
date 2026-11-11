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
    CHECK_EQ(run(600, 600, 5000), Protocol::NoQuick);    // the K2's own 0.6 V bias on both lines proves nothing passively
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

// A live USB data link can average D+ = D- (SE0 idle / high-speed signalling on both lines), so an
// equal-level sample never proves a BC1.2 short: the levels have to sit still over the whole window.
// Recorded on a data link behind a laptop port: D+ 0.59 V / D- 0.59 V on average, and D+ 2.03 V /
// D- 0.59 V in an earlier capture of the same kind of link - the levels move with bus activity.
static void test_dcp_needs_stability() {
    // A steady short sits still: D+ = D- = 0.59 V for the whole window -> DCP stands.
    Ring steady;
    for (int i = 0; i < 50; i++) steady.push(590, 590);
    // ... unless it is the K2's own idle bias (both lines driven to ~0.6 V): see test_bias_pair_is_not_a_dcp.
    CHECK_EQ(classify(steady, 590, 590, 5000), Protocol::NoQuick);
    // D+ alternating 0.59 / 2.03 V against a steady 0.59 V D-: traffic, not a short,
    // whichever level the latest sample happens to hold.
    Ring traffic;
    for (int i = 0; i < 50; i++) traffic.push((i & 1) ? 2030 : 590, 590);
    CHECK_EQ(classify(traffic, 590, 590, 5000), Protocol::UsbData);
    CHECK_EQ(classify(traffic, 2030, 590, 5000), Protocol::UsbData);
    // One equal-level sample at the end of an active window is not a short either.
    Ring tail;
    for (int i = 0; i < 49; i++) tail.push((i & 1) ? 2030 : 590, 590);
    tail.push(590, 590);
    CHECK_EQ(classify(tail, 590, 590, 5000), Protocol::UsbData);
    // Both lines equal but varying together: still moving, still not a short.
    Ring both;
    for (int i = 0; i < 50; i++) { const int v = (i & 1) ? 2030 : 590; both.push(v, v); }
    CHECK_EQ(classify(both, 2030, 2030, 5000), Protocol::UsbData);
    // Full-speed J/K traffic on D+, D- low: activity on the pair reads as data.
    Ring fs;
    for (int i = 0; i < 50; i++) fs.push((i & 1) ? 3300 : 0, 0);
    CHECK_EQ(classify(fs, 3300, 0, 5000), Protocol::UsbData);
    CHECK_EQ(classify(fs, 0, 0, 5000), Protocol::UsbData);
    // The steady shorts of the 5 V band test above keep their verdict.
    CHECK_EQ(run(3300, 3300, 5000), Protocol::Dcp5V);
    CHECK_EQ(run(3300, 3200, 5000), Protocol::Dcp5V);
    CHECK_EQ(run(600, 600, 5000), Protocol::NoQuick);
    // A default-USB data host on CC (Rp attached, no PD contract): the pair is data even when
    // the levels are equal or unrecognisable - never a dedicated charger.
    CHECK_EQ(classify(steady, 590, 590, 5000, false, false, true), Protocol::UsbData);
    CHECK_EQ(classify(traffic, 590, 590, 5000, false, false, true), Protocol::UsbData);
    Ring unknown;
    for (int i = 0; i < 50; i++) unknown.push(1500, 600);
    CHECK_EQ(classify(unknown, 1500, 600, 5000, false, false, true), Protocol::UsbData);
    CHECK_EQ(classify(unknown, 1500, 600, 5000), Protocol::NoQuick);
    // A PD contract or a likely one still wins over the data-host flag.
    CHECK_EQ(classify(steady, 590, 590, 5000, true, false, true), Protocol::Pd);
    CHECK_EQ(classify(steady, 590, 590, 8900, false, true, true), Protocol::PdLikely);
}

// ROADMAP 2026-11-10, still-open hardware note of the "PD 0V5.82A" row: a settled fixed PD contract is
// silent on CC (no frame for 14 s), so no Request is decoded and pd_active() stays false, and the
// D+/D- classifier used to read the phone's idle lines as "PD MTK". Rule: CC attached (a source's Rp
// on a CC line), VBUS above vSafe5V (> 5.5 V) and no D+/D- handshake seen = "PD (not decoded)" -
// never a legacy mode. The footer keeps "CC 9V" (measured level, no current) from the 27255b3 fix.
static void test_silent_pd_contract_above_vsafe5_reads_pd() {
    // MTK-like levels (both lines near 0 V) at 9 V, CC attached, no PD decoded: PD, not PD MTK.
    Ring mtk;
    for (int i = 0; i < 50; i++) mtk.push(0, 0);
    CHECK_EQ(classify(mtk, 0, 0, 9000, false, false, true), Protocol::Pd);
    // The phone's data-idle levels (D+ pull-up ~0.8 V, D- ~0) at 8.9 V, CC attached, no caps, no load.
    Ring px;
    for (int i = 0; i < 50; i++) px.push(820, 1);
    CHECK_EQ(classify(px, 820, 1, 8898, false, false, true), Protocol::Pd);
    // Every band above vSafe5V: never PdMtk / PdPe / a QC level.
    CHECK_EQ(classify(mtk, 0, 0, 6000, false, false, true), Protocol::Pd);
    CHECK_EQ(classify(mtk, 0, 0, 12000, false, false, true), Protocol::Pd);
    CHECK_EQ(classify(mtk, 0, 0, 20000, false, false, true), Protocol::Pd);
    // Band edge: only above 5.5 V (vSafe5V max itself is still the old reading).
    CHECK_EQ(classify(mtk, 0, 0, 5500, false, false, true), Protocol::PdPe);
    CHECK_EQ(classify(mtk, 0, 0, 5501, false, false, true), Protocol::Pd);
    // At 5 V the rule does not apply (a default-USB host on CC is data there).
    CHECK_EQ(classify(mtk, 0, 0, 5000, false, false, true), Protocol::UsbData);
    // A decoded contract or a likely one still wins.
    CHECK_EQ(classify(mtk, 0, 0, 9000, true, false, true), Protocol::Pd);
    CHECK_EQ(classify(px, 820, 1, 8898, false, true, true), Protocol::PdLikely);
    // No CC attached: the same silent levels read as before, and a real handshake at 9 V reads legacy.
    CHECK_EQ(classify(mtk, 0, 0, 9000, false, false, false), Protocol::PdMtk);
    Ring qc;
    for (int i = 0; i < 50; i++) qc.push(3300, 600);
    CHECK_EQ(classify(qc, 3300, 600, 9000, false, false, false), Protocol::Qc2_9V);
    // With CC attached but a handshake in the window, the legacy reading stands.
    CHECK_EQ(classify(qc, 3300, 600, 9000, false, false, true), Protocol::Qc2_9V);
    Ring svooc;
    for (int i = 0; i < 50; i++) svooc.push(3300, (i & 1) ? 3300 : 0);
    CHECK_EQ(classify(svooc, 3300, 0, 9000, false, false, true), Protocol::Svooc);
}

// What counts as a D+/D- handshake for the "no handshake seen this attach" part of the rule.
static void test_handshake_pattern() {
    Ring idle0;
    for (int i = 0; i < 50; i++) idle0.push(0, 0);
    CHECK_EQ(handshake_pattern(idle0, 0, 0), false);            // silent lines (the "MTK-like" case)
    Ring px;
    for (int i = 0; i < 50; i++) px.push(820, 1);
    CHECK_EQ(handshake_pattern(px, 820, 1), false);             // USB data idle: pull-up inside the window
    Ring mid;
    for (int i = 0; i < 50; i++) mid.push(2000, 2000);
    CHECK_EQ(handshake_pattern(mid, 2000, 2000), false);        // steady ~2 V rail: not a handshake
    Ring afc;
    for (int i = 0; i < 50; i++) afc.push(600, 0);
    CHECK_EQ(handshake_pattern(afc, 600, 0), true);             // steady 0.6 V rail below the data window
    Ring qc;
    for (int i = 0; i < 50; i++) qc.push(3300, 600);
    CHECK_EQ(handshake_pattern(qc, 3300, 600), true);           // high D+
    Ring pulse;
    for (int i = 0; i < 50; i++) pulse.push((i & 1) ? 3300 : 0, 0);
    CHECK_EQ(handshake_pattern(pulse, 3300, 0), true);          // pulse train
    Ring dcp;
    for (int i = 0; i < 50; i++) dcp.push(590, 590);
    CHECK_EQ(handshake_pattern(dcp, 590, 590), false);          // the K2's own bias on both lines: not a handshake
    Ring dcp_hi;
    for (int i = 0; i < 50; i++) dcp_hi.push(3300, 3300);
    CHECK_EQ(handshake_pattern(dcp_hi, 3300, 3300), true);      // a short at a level the bias cannot make
    Ring sam;
    for (int i = 0; i < 50; i++) sam.push(1200, 1200);
    CHECK_EQ(handshake_pattern(sam, 1200, 1200), true);         // Samsung 2A divider
    // A handshake earlier in the attach still blocks the rule even once the window went idle again
    // (the caller latches it): cc_attach_quiet=false.
    Ring mtk;
    for (int i = 0; i < 50; i++) mtk.push(0, 0);
    CHECK_EQ(classify(mtk, 0, 0, 9000, false, false, true, false), Protocol::PdMtk);
    CHECK_EQ(classify(mtk, 0, 0, 9000, false, false, true, true), Protocol::Pd);
}

// The passive idle drives BOTH lines to the 0.6 V level (dpdm kPassiveIdle), so an undriven pair reads
// 0.59..0.70 V equal and steady (hardware: 653/653 mV at boot with nothing attached; 592/591 mV for 89 s
// behind a phone and an SSD, no charger in the chain, which read "DCP 5V"). That reading is our own
// bias: it proves no short. Bias window: 0.5 .. 0.79 V (decivolt buckets 5..7) on both lines.
static void test_bias_pair_is_not_a_dcp() {
    CHECK_EQ(run(653, 653, 5000), Protocol::NoQuick);
    CHECK_EQ(run(592, 591, 5000), Protocol::NoQuick);
    CHECK_EQ(run(500, 500, 5000), Protocol::NoQuick);    // window edges
    CHECK_EQ(run(799, 799, 5000), Protocol::NoQuick);
    CHECK_EQ(run(700, 600, 5000), Protocol::NoQuick);
    // Equal steady pairs outside the window keep today's verdict.
    CHECK_EQ(run(800, 800, 5000), Protocol::Dcp5V);
    CHECK_EQ(run(450, 450, 5000), Protocol::Dcp5V);
    CHECK_EQ(run(2000, 2000, 5000), Protocol::Dcp5V);    // Apple 0.5 A divider: still unclassified here (no enum value)
    CHECK_EQ(run(3300, 3300, 5000), Protocol::Dcp5V);
    // A data host on CC still reads USB DATA, never NO QUICK.
    Ring bias;
    for (int i = 0; i < 50; i++) bias.push(653, 653);
    CHECK_EQ(classify(bias, 653, 653, 5000, false, false, true), Protocol::UsbData);
}

// 1b: the bias pair is not a handshake, and a ring read before it is full is never a handshake (clear()
// resets the counters only, so the arrays still hold the previous attach).
static void test_handshake_latch_and_stale_ring() {
    Ring afc;
    for (int i = 0; i < 50; i++) afc.push(600, 0);
    CHECK_EQ(handshake_pattern(afc, 600, 0), true);
    afc.clear();
    afc.push(1500, 1500);                                     // the arrays still hold the old 0.6 V / 0 V window
    CHECK_EQ(handshake_pattern(afc, 1500, 1500), false);
    CHECK_EQ(update_handshake_latch(false, afc, 1500, 1500), false);
    // Latch only moves up, only from a full window.
    Ring bias;
    for (int i = 0; i < 50; i++) bias.push(650, 650);
    CHECK_EQ(update_handshake_latch(false, bias, 650, 650), false);
    Ring qc2;                                                 // a real QC2 session: D+ 0.6 V, D- 0 V
    for (int i = 0; i < 50; i++) qc2.push(600, 0);
    CHECK_EQ(update_handshake_latch(false, qc2, 600, 0), true);
    Ring fcp;                                                 // FCP/AFC: one line at 0.6 V, the other not
    for (int i = 0; i < 50; i++) fcp.push(0, 600);
    CHECK_EQ(update_handshake_latch(false, fcp, 0, 600), true);
    CHECK_EQ(update_handshake_latch(true, bias, 650, 650), true);   // once latched it stays
    // A fixed contract behind an idle pair: with CC attached, no decoded PD and no handshake the
    // verdict is PD at 9 V and 12 V - not PD MTK / QC2.0 12V.
    bool latched = false;
    for (int s = 0; s < 3; s++) {                             // three windows of the bias pair
        latched = update_handshake_latch(latched, bias, 650, 650);
    }
    CHECK_EQ(latched, false);
    CHECK_EQ(classify(bias, 650, 650, 9000, false, false, true, !latched), Protocol::Pd);
    CHECK_EQ(classify(bias, 650, 650, 12000, false, false, true, !latched), Protocol::Pd);
    // The previous attach's stale samples do not latch the new one on its first sample.
    Ring r;
    for (int i = 0; i < 50; i++) r.push(3300, 600);
    r.clear();
    r.push(650, 650);
    CHECK_EQ(update_handshake_latch(false, r, 650, 650), false);
    // ... and a part-filled ring never claims a silent PD contract either (Pending until full).
    CHECK_EQ(classify(r, 650, 650, 9000, false, false, true, true), Protocol::Pending);
    // A real handshake still keeps the legacy reading and blocks the silent-PD rule.
    CHECK_EQ(classify(qc2, 600, 0, 9000, false, false, true, !update_handshake_latch(false, qc2, 600, 0)), Protocol::AfcFcp);
}

// 1c: the active DCP check. D+ is pulled to 0 V while D- keeps the bias; a BC1.2 short drags D- down
// with it, an open pair leaves D- on the bias.
static void test_active_dcp_check() {
    // Decision: D- at least 150 mV below its own idle reading = shorted.
    CHECK_EQ(dcp_check_result(592, 80), DcpCheck::Shorted);      // a short to a strong 0 V
    CHECK_EQ(dcp_check_result(653, 330), DcpCheck::Shorted);     // a symmetric divider: about half the bias
    CHECK_EQ(dcp_check_result(592, 442), DcpCheck::Shorted);     // exactly the margin
    CHECK_EQ(dcp_check_result(592, 443), DcpCheck::Open);
    CHECK_EQ(dcp_check_result(592, 585), DcpCheck::Open);        // D- stays on the bias
    CHECK_EQ(dcp_check_result(592, 700), DcpCheck::Open);        // moved up: still no short
    // An idle reading outside the bias window is not the K2's own drive: no verdict from this check.
    CHECK_EQ(dcp_check_result(0, 0), DcpCheck::Invalid);
    CHECK_EQ(dcp_check_result(2000, 100), DcpCheck::Invalid);
    CHECK_EQ(dcp_check_result(499, 100), DcpCheck::Invalid);
    CHECK_EQ(dcp_check_result(800, 100), DcpCheck::Invalid);

    // The proven short reads DCP 5V at the bias pair, an open pair stays NO QUICK.
    Ring bias;
    for (int i = 0; i < 50; i++) bias.push(592, 591);
    CHECK_EQ(classify(bias, 592, 591, 5000), Protocol::NoQuick);
    CHECK_EQ(classify(bias, 592, 591, 5000, false, false, false, true, true), Protocol::Dcp5V);
    // The proof never overrides a data host, a PD contract, or a pair that is not steady.
    CHECK_EQ(classify(bias, 592, 591, 5000, false, false, true, true, true), Protocol::UsbData);
    CHECK_EQ(classify(bias, 592, 591, 5000, true, false, false, true, true), Protocol::Pd);
    Ring moving;
    for (int i = 0; i < 50; i++) moving.push((i & 1) ? 2030 : 590, 590);
    CHECK_EQ(classify(moving, 590, 590, 5000, false, false, false, true, true), Protocol::UsbData);
    // bias_pair: full window, equal and steady, inside 0.5 .. 0.79 V.
    CHECK_EQ(bias_pair(bias), true);
    Ring part;
    part.push(592, 591);
    CHECK_EQ(bias_pair(part), false);
    Ring shorted_hi;
    for (int i = 0; i < 50; i++) shorted_hi.push(3300, 3300);
    CHECK_EQ(bias_pair(shorted_hi), false);
    Ring qc2;
    for (int i = 0; i < 50; i++) qc2.push(600, 0);
    CHECK_EQ(bias_pair(qc2), false);

    // Gate: every condition must hold, and a failed attach does not retry within the same attach.
    DcpCheckGate g;
    g.vbus_mv = 5000; g.not_inline = true; g.engine_idle = true; g.lines_free = true;
    CHECK_EQ(dcp_check_allowed(g, bias), true);
    DcpCheckGate x = g; x.vbus_mv = 3900;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    x = g; x.vbus_mv = 9000;
    CHECK_EQ(dcp_check_allowed(x, bias), false);                 // only the 5 V band
    x = g; x.not_inline = false;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    x = g; x.pd_seen = true;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    x = g; x.pd_likely = true;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    x = g; x.cc_source_no_pd = true;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    x = g; x.engine_idle = false;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    x = g; x.lines_free = false;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    x = g; x.done_this_attach = true;
    CHECK_EQ(dcp_check_allowed(x, bias), false);
    CHECK_EQ(dcp_check_allowed(g, part), false);                 // ring not full
    CHECK_EQ(dcp_check_allowed(g, qc2), false);                  // not the bias pair
}

// While the inline VOOC monitor owns D+/D- (floating inputs, no 0.6 V bias, dpsniff::monitoring()), the levels are
// the data device's and the charger's own, not a handshake the K2 can read: every window reads USB DATA (the monitor
// only runs with a data device inline), whatever level it holds. PD and "no input" still win.
static void test_floating_lines_are_not_a_protocol() {
    struct Level { int dp, dm; };
    const Level levels[] = {{600, 600}, {600, 0}, {3300, 600}, {600, 3300}, {3300, 3300}, {2700, 2000},
                            {2000, 2700}, {1200, 1200}, {0, 0}, {800, 0}, {2000, 2000}};
    for (const int vbus : {5000, 9000, 12000, 20000}) {
        for (const Level l : levels) {
            Ring r;
            for (int i = 0; i < 50; i++) r.push(l.dp, l.dm);
            CHECK_EQ(int(classify(r, l.dp, l.dm, vbus, false, false, false, true, false, true)), int(Protocol::UsbData));
            CHECK_EQ(int(classify(r, l.dp, l.dm, vbus, false, false, true, true, true, true)), int(Protocol::UsbData));
            CHECK_EQ(int(classify(r, l.dp, l.dm, vbus, true, false, false, true, false, true)), int(Protocol::Pd));
            CHECK_EQ(int(classify(r, l.dp, l.dm, vbus, false, true, false, true, false, true)), int(Protocol::PdLikely));
        }
    }
    Ring part;
    part.push(3300, 600);
    CHECK_EQ(int(classify(part, 3300, 600, 9000, false, false, false, true, false, true)), int(Protocol::UsbData));
    CHECK_EQ(int(classify(part, 3300, 600, 1000, false, false, false, true, false, true)), int(Protocol::NoInput));
    // Not floating: the same 9 V QC2 levels read as a protocol (the flag is what changes the verdict).
    Ring qc;
    for (int i = 0; i < 50; i++) qc.push(3300, 600);
    CHECK_EQ(int(classify(qc, 3300, 600, 9000) != Protocol::UsbData), 1);
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
    test_dcp_needs_stability();
    test_silent_pd_contract_above_vsafe5_reads_pd();
    test_handshake_pattern();
    test_bias_pair_is_not_a_dcp();
    test_handshake_latch_and_stale_ring();
    test_active_dcp_check();
    test_floating_lines_are_not_a_protocol();
    return test_report("test_fastcharge_classify");
}
