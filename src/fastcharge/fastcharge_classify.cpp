// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/fastcharge/fastcharge_classify.hpp"

#include <algorithm>

namespace k2::fastcharge {

namespace {

constexpr int kVbusPresentMv = 4000;   // auto-detect step 1: wait for VBUS >= 4.0 V
constexpr int kDataIdleMinMv = 700;    // USB data idle window on D+ (hardware: 819 mV on a Pixel)
constexpr int kDataIdleMaxMv = 1000;
constexpr int kBand5VMax = 5500;       // below: 5 V band
constexpr int kBand7VMax = 7700;       // below: low-voltage direct-charge band
constexpr int kBand9VMax = 9900;       // up to and including: 9 V band
constexpr int kBand12VMin = 10500;     // 12 V band
constexpr int kBand12VMax = 13700;
constexpr int kBand20VMin = 18500;     // 20 V band
constexpr int kBand20VMax = 21500;

constexpr bool high(int b) { return b == 2 || b == 3 || b == 5; }     // 2.4 .. 3.3 V
constexpr bool pulsing(int b) { return b == 4 || b == 7; }

// A BC1.2 short ties D+ to D- at one level that sits still: every sample pair of the whole window
// within 0.2 V of each other, at least 0.4 V high, and the combined level steady within 0.2 V.
// A live data link can average equal (SE0 idle, high-speed signalling) but its levels move with
// bus activity, so one equal-level sample never proves a short.
bool dcp_shorted(const Ring& ring) {
    int lo = 255, hi = 0;
    for (std::size_t i = 0; i < kRingSize; i++) {
        const int d = ring.dp[i], m = ring.dm[i];
        const int diff = d > m ? d - m : m - d;
        if (diff > 2) return false;
        if (d < lo) lo = d;
        if (m < lo) lo = m;
        if (d > hi) hi = d;
        if (m > hi) hi = m;
    }
    return lo >= 4 && hi - lo <= 2;
}

Protocol band_5v(const Ring& ring, int bdp, int bdm, int dp_mv, int dm_mv, bool cc_source_no_pd) {
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (bdp == 1 && bdm == 0) return Protocol::AfcFcp;     // passive: QC2.0 5 V needs an active trigger
    if (bdp == 1 && bdm == 4) return Protocol::Scp;
    if (bdp == 2 && bdm == 2) return Protocol::Apple24;
    if (bdp == 2 && dm_mv >= 1900 && dm_mv <= 2100) return Protocol::Apple21;
    if (bdp == 8 && dm_mv >= 2600 && dm_mv <= 2800) return Protocol::Apple10;
    if (high(bdp) && bdm == 1) return Protocol::Usb2Hi;
    if (high(bdp) && pulsing(bdm)) return Protocol::VoocWarp;
    if (bdp == 0 && high(bdm)) return Protocol::Usb11Low;
    if (dp_mv >= 1100 && dp_mv <= 1300 && dm_mv >= 1100 && dm_mv <= 1300) return Protocol::Sam20;
    // Tail of the 5 V band: a data host on CC (default-USB Rp, no PD contract) owns the pair.
    if (cc_source_no_pd) return Protocol::UsbData;
    // Only a short that stayed put over the whole window is a dedicated charger; activity on the
    // pair (pulse train or mix) is a data link, and anything else is simply no handshake.
    if (dcp_shorted(ring)) return Protocol::Dcp5V;
    if (pulsing(bdp) || pulsing(bdm)) return Protocol::UsbData;
    return Protocol::NoQuick;
}

Protocol band_low_voltage(int bdp, int bdm) {
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (bdp == 1 && (bdm == 4 || bdm == 6 || bdm == 7)) return Protocol::Scp;
    if (high(bdp) && pulsing(bdm)) return Protocol::VoocWarp;
    return Protocol::PdPe;
}

Protocol band_9v(int bdp, int bdm) {
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (high(bdp) && bdm == 1) return Protocol::Qc2_9V;
    if (bdp == 1 && bdm == 0) return Protocol::AfcFcp;
    if (bdp == 1 && bdm == 4) return Protocol::Scp;
    if (high(bdp) && pulsing(bdm)) return Protocol::Svooc;
    return Protocol::PdMtk;
}

Protocol band_12v(int bdp, int bdm) {
    if (bdp == 1 && bdm == 1) return Protocol::Qc2_12V;
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (bdp == 1 && bdm == 0) return Protocol::AfcFcp;
    if (bdp == 1 && bdm == 4) return Protocol::Scp;
    return Protocol::PdMtk;
}

Protocol band_20v(int bdp, int bdm) {
    if (high(bdp) && high(bdm)) return Protocol::Qc2_20V;
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    return Protocol::Pd;
}

}  // namespace

void Ring::push(int dp_mv, int dm_mv) {
    auto dv = [](int mv) { return static_cast<std::uint8_t>(std::clamp(mv / 100, 0, 255)); };
    dp[next] = dv(dp_mv);
    dm[next] = dv(dm_mv);
    next = (next + 1) % kRingSize;
    if (count < kRingSize) count++;
}

int bucket(std::span<const std::uint8_t, kRingSize> samples) {
    int c0 = 0, c06 = 0, c20 = 0, c27 = 0, c33 = 0;
    for (const std::uint8_t v : samples) {
        if (v <= 3) c0++;
        else if (v <= 10) c06++;
        else if (v <= 23) c20++;
        else if (v <= 29) c27++;
        else c33++;
    }
    const int n = static_cast<int>(kRingSize);
    const int low = c0 + c06, hi = c27 + c33;
    if (c0 == n) return 0;
    if (c06 == n) return 1;
    if (c27 == n) return 2;
    if (c33 == n) return 3;
    if (c20 == n) return 8;
    if (low > 0 && hi > 0) return 4;
    if (hi == n) return 5;
    if (low == n) return 6;
    return 7;
}

Protocol classify(const Ring& ring, int dp_mv, int dm_mv, int vbus_mv, bool pd_active, bool pd_likely,
                  bool cc_source_no_pd, bool cc_attach_quiet) {
    if (vbus_mv < kVbusPresentMv) return Protocol::NoInput;
    if (pd_active) return Protocol::Pd;
    if (pd_likely) return Protocol::PdLikely;
    // ROADMAP 2026-11-10 (`fc: PD MTK` on a settled fixed contract): CC attached (a source's Rp, per the
    // FUSB302 - never the passive CC ADC levels, the CC2 bias net reads ~1.69 V with nothing on it),
    // VBUS above vSafe5V and no D+/D- handshake seen this attach = a PD contract the K2 did not decode.
    // A settled fixed contract sends no frame for many seconds, so no Request is decoded and
    // pd_active() stays false; the phone's idle data pair must not be read as a legacy mode.
    if (cc_source_no_pd && cc_attach_quiet && vbus_mv > kBand5VMax && !handshake_pattern(ring, dp_mv, dm_mv))
        return Protocol::Pd;   // kBand5VMax = 5500 = above vSafe5V
    if (!ring.full()) return Protocol::Pending;
    const int bdp = bucket(ring.dp);
    const int bdm = bucket(ring.dm);
    // A full-speed device idles with its D+ pull-up read through the divider at about 0.7..1.0 V and
    // D- near 0. The charger-driven 0.6 V of FCP/AFC reads below that window.
    if (bdp == 1 && bdm == 0 && dp_mv >= kDataIdleMinMv && dp_mv <= kDataIdleMaxMv) return Protocol::UsbData;
    if (vbus_mv < kBand5VMax) return band_5v(ring, bdp, bdm, dp_mv, dm_mv, cc_source_no_pd);
    if (vbus_mv < kBand7VMax) return band_low_voltage(bdp, bdm);
    if (vbus_mv <= kBand9VMax) return band_9v(bdp, bdm);
    if (vbus_mv >= kBand12VMin && vbus_mv <= kBand12VMax) return band_12v(bdp, bdm);
    if (vbus_mv >= kBand20VMin && vbus_mv <= kBand20VMax) return band_20v(bdp, bdm);
    return Protocol::Pd;   // gaps between the bands and above 21.5 V
}

const char* name(Protocol p) {
    switch (p) {
        case Protocol::Pending:   return "...";
        case Protocol::NoInput:   return "NO INPUT";
        case Protocol::NoQuick:   return "NO QUICK";
        case Protocol::Dcp5V:     return "DCP 5V";
        case Protocol::Qc3:       return "QC3.0";
        case Protocol::Qc2_5V:    return "QC2.0 5V";
        case Protocol::Qc2_9V:    return "QC2.0 9V";
        case Protocol::Qc2_12V:   return "QC2.0 12V";
        case Protocol::Qc2_20V:   return "QC2.0 20V";
        case Protocol::AfcFcp:    return "AFC FCP";
        case Protocol::Scp:       return "SCP";
        case Protocol::Apple24:   return "APPLE 2.4A";
        case Protocol::Apple21:   return "APPLE 2.1A";
        case Protocol::Apple10:   return "APPLE 1.0A";
        case Protocol::Usb2Hi:    return "USB2.0 HI";
        case Protocol::Usb11Low:  return "USB1.1 LOW";
        case Protocol::Sam20:     return "SAM 2.0A";
        case Protocol::VoocWarp:  return "VOOC WARP";
        case Protocol::Svooc:     return "SVOOC";
        case Protocol::PdPe:      return "PD PE";
        case Protocol::PdMtk:     return "PD MTK";
        case Protocol::Pd:        return "PD";
        case Protocol::UsbData:   return "USB DATA";
        case Protocol::PdLikely:  return "PD?";
    }
    return "?";
}

const char* short_name(Protocol p) {
    switch (p) {
        case Protocol::Pending:   return "...";
        case Protocol::NoInput:   return "NO INPUT";
        case Protocol::NoQuick:   return "NO QUICK";
        case Protocol::Dcp5V:     return "DCP 5V";
        case Protocol::Qc3:       return "QC3.0";
        case Protocol::Qc2_5V:    return "QC2 5V";
        case Protocol::Qc2_9V:    return "QC2 9V";
        case Protocol::Qc2_12V:   return "QC2 12V";
        case Protocol::Qc2_20V:   return "QC2 20V";
        case Protocol::AfcFcp:    return "AFC FCP";
        case Protocol::Scp:       return "SCP";
        case Protocol::Apple24:   return "APL 2.4A";
        case Protocol::Apple21:   return "APL 2.1A";
        case Protocol::Apple10:   return "APL 1.0A";
        case Protocol::Usb2Hi:    return "USB2 HI";
        case Protocol::Usb11Low:  return "USB1 LOW";
        case Protocol::Sam20:     return "SAM 2.0A";
        case Protocol::VoocWarp:  return "WARP";
        case Protocol::Svooc:     return "SVOOC";
        case Protocol::PdPe:      return "PD PE";
        case Protocol::PdMtk:     return "PD MTK";
        case Protocol::Pd:        return "PD";
        case Protocol::UsbData:   return "USB DATA";
        case Protocol::PdLikely:  return "PD?";
    }
    return "?";
}

unsigned idle_mismatch(bool pa7, bool pb0, bool pb1, bool pb2) {
    return (pa7 ? 0u : 1u) | (pb0 ? 2u : 0u) | (pb1 ? 0u : 4u) | (pb2 ? 8u : 0u);
}

// A charger handshake on the data pair: a pulsing or high bucket on either line, a steady 0.6 V rail
// below the USB data-idle window (FCP/AFC and a BC1.2 short hold the lines there; a data device's
// pull-up reads 0.7..1.0 V), or the Samsung divider levels. Idle lines - 0 V, the data pull-up, a
// steady ~2 V rail (the "MTK-like" levels of a phone on a PD contract, which the 9 V band reads as
// PD MTK, not as a handshake) - are not a handshake.
bool handshake_pattern(const Ring& ring, int dp_mv, int dm_mv) {
    const int bdp = bucket(ring.dp), bdm = bucket(ring.dm);
    if (pulsing(bdp) || pulsing(bdm) || high(bdp) || high(bdm)) return true;
    if (bdp == 1 && dp_mv < kDataIdleMinMv) return true;
    if (bdm == 1 && dm_mv < kDataIdleMinMv) return true;
    if (dp_mv >= 1100 && dp_mv <= 1300 && dm_mv >= 1100 && dm_mv <= 1300) return true;   // Samsung 2A divider
    return false;
}

}  // namespace k2::fastcharge
