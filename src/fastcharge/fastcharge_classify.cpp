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

// The bias window, in decivolt buckets: 0.5 .. 0.79 V (5..7) on both lines. An equal pair there is a D+/D- short
// carrying a 0.6 V detection source: a sink's BC1.2 / HVDCP D+ source behind the K2, or (until 2026-11-11) the K2's own
// idle, which drove both lines to 0.6 V (hardware: 653/653 mV at boot, 592/591 mV behind a phone). Passively it does
// not say who drives the level, so it proves no short on its own.
constexpr int kBiasMinDv = 5;
constexpr int kBiasMaxDv = 7;
// The released idle (dpdm kIdle) leaves an undriven pair floating, and a floating BC1.2 short reads equal and low.
// A quiet pair is an equal steady pair at or below the bias window's top: the levels the active check may probe.
constexpr int kQuietMaxDv = kBiasMaxDv;
// equal_pair_moving tolerates this many samples with the lines apart (one line leaving a step before the other).
constexpr int kTrackApartMax = 3;
// ... and at most this many level jumps (> 0.2 V between consecutive samples): a step, ramp or dip, not a toggle.
constexpr int kTrackJumpsMax = 8;

// An equal pair: every sample pair of the whole window within 0.2 V of each other and the combined level steady
// within 0.2 V. A live data link can average equal (SE0 idle, high-speed signalling) but its levels move with bus
// activity, so one equal-level sample never proves a short. `steady` needs at least 0.4 V (a passive DCP verdict),
// `bias` marks a level inside the bias window and `quiet` one at or below kQuietMaxDv (0 V included).
// `equal` is that test alone, at any level (0 V included): the pair is one level on both lines, so no handshake is
// being driven. FCP and AFC put D+ and D- on different levels; the 0.3 V bucket edge can split an equal low pair into
// "0.6 V" and "0 V" (hardware 2026-11-11: 420 / 387 mV read AFC FCP).
struct SteadyPair {
    bool equal = false;
    bool steady = false;
    bool bias = false;
    bool quiet = false;
};
SteadyPair steady_pair(const Ring& ring) {
    int lo = 255, hi = 0;
    for (std::size_t i = 0; i < kRingSize; i++) {
        const int d = ring.dp[i], m = ring.dm[i];
        const int diff = d > m ? d - m : m - d;
        if (diff > 2) return {};
        if (d < lo) lo = d;
        if (m < lo) lo = m;
        if (d > hi) hi = d;
        if (m > hi) hi = m;
    }
    if (hi - lo > 2) return {};
    return {true, lo >= 4, lo >= kBiasMinDv && hi <= kBiasMaxDv, hi <= kQuietMaxDv};
}

// BC1.2 short: a steady equal pair outside the bias window passively, or any quiet pair the active check proved
// (D+ pulled low, D- followed).
bool dcp_shorted(const Ring& ring, bool active_proof) {
    const SteadyPair p = steady_pair(ring);
    return (p.steady && !p.bias) || (active_proof && p.quiet);
}

// FCP / AFC handshake levels: D+ on the 0.6 V rail with D- at 0 V. Those are two different levels; an equal pair (see
// SteadyPair) that the bucket edge split that way is one level on both lines, not a handshake.
bool afc_fcp_levels(const Ring& ring, int bdp, int bdm) {
    return bdp == 1 && bdm == 0 && !steady_pair(ring).equal;
}

Protocol band_5v(const Ring& ring, int bdp, int bdm, int dp_mv, int dm_mv, bool cc_source_no_pd, bool dcp_proof) {
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (afc_fcp_levels(ring, bdp, bdm)) return Protocol::AfcFcp;   // passive: QC2.0 5 V needs an active trigger
    if (bdp == 1 && bdm == 4) return Protocol::Scp;
    if (bdp == 2 && bdm == 2) return Protocol::Apple24;
    if (bdp == 2 && dm_mv >= 1900 && dm_mv <= 2100) return Protocol::Apple21;
    if (bdp == 8 && dm_mv >= 2600 && dm_mv <= 2800) return Protocol::Apple10;
    if (high(bdp) && bdm == 1) return Protocol::Usb2Hi;
    if (high(bdp) && pulsing(bdm)) return Protocol::VoocWarp;
    if (bdp == 0 && high(bdm)) return Protocol::Usb11Low;
    if (dp_mv >= 1100 && dp_mv <= 1300 && dm_mv >= 1100 && dm_mv <= 1300) return Protocol::Sam20;
    // An equal pair that steps together is a divider following the supply, never data (SteadyPair, equal_pair_moving).
    if (equal_pair_moving(ring)) return Protocol::NoQuick;
    // Tail of the 5 V band: a data host on CC (default-USB Rp, no PD contract) owns the pair. An A-to-C cable puts that
    // Rp in the plug for any USB-A charger port, so a steady equal pair outside the bias window is no data and goes on to
    // the DCP rule (hardware 2026-11-11: a charger port at 2.0 / 2.0 V read USB DATA). The bias window stays ambiguous.
    if (cc_source_no_pd) {
        const SteadyPair p = steady_pair(ring);
        if (!(p.equal && p.steady && !p.bias)) return Protocol::UsbData;
    }
    // Only a short that stayed put over the whole window is a dedicated charger; activity on the
    // pair (pulse train or mix) is a data link, and anything else is simply no handshake.
    if (dcp_shorted(ring, dcp_proof)) return Protocol::Dcp5V;
    if (pulsing(bdp) || pulsing(bdm)) return Protocol::UsbData;
    return Protocol::NoQuick;
}

Protocol band_low_voltage(int bdp, int bdm) {
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (bdp == 1 && (bdm == 4 || bdm == 6 || bdm == 7)) return Protocol::Scp;
    if (high(bdp) && pulsing(bdm)) return Protocol::VoocWarp;
    return Protocol::PdPe;
}

Protocol band_9v(const Ring& ring, int bdp, int bdm) {
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (high(bdp) && bdm == 1) return Protocol::Qc2_9V;
    if (afc_fcp_levels(ring, bdp, bdm)) return Protocol::AfcFcp;
    if (bdp == 1 && bdm == 4) return Protocol::Scp;
    if (high(bdp) && pulsing(bdm)) return Protocol::Svooc;
    return Protocol::PdMtk;
}

Protocol band_12v(const Ring& ring, int bdp, int bdm) {
    if (bdp == 1 && bdm == 1) return Protocol::Qc2_12V;
    if (bdp == 1 && high(bdm)) return Protocol::Qc3;
    if (afc_fcp_levels(ring, bdp, bdm)) return Protocol::AfcFcp;
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
                  bool cc_source_no_pd, bool cc_attach_quiet, bool dcp_proof, bool lines_floating) {
    if (vbus_mv < kVbusPresentMv) return Protocol::NoInput;
    if (pd_active) return Protocol::Pd;
    if (pd_likely) return Protocol::PdLikely;
    if (lines_floating) return Protocol::UsbData;   // the VOOC monitor's floating lines: no level is a handshake
    // ROADMAP 2026-11-10 (`fc: PD MTK` on a settled fixed contract): CC attached (a source's Rp, per the
    // FUSB302 - never the passive CC ADC levels, the CC2 bias net reads ~1.69 V with nothing on it),
    // VBUS above vSafe5V and no D+/D- handshake seen this attach = a PD contract the K2 did not decode.
    // A settled fixed contract sends no frame for many seconds, so no Request is decoded and
    // pd_active() stays false; the phone's idle data pair must not be read as a legacy mode.
    // Only a full window can say "no handshake": a part-filled ring is Pending (the arrays still hold
    // the previous attach until they are overwritten).
    if (!ring.full()) return Protocol::Pending;
    if (cc_source_no_pd && cc_attach_quiet && vbus_mv > kBand5VMax && !handshake_pattern(ring, dp_mv, dm_mv))
        return Protocol::Pd;   // kBand5VMax = 5500 = above vSafe5V
    const int bdp = bucket(ring.dp);
    const int bdm = bucket(ring.dm);
    // A full-speed device idles with its D+ pull-up read through the divider at about 0.7..1.0 V and
    // D- near 0. The charger-driven 0.6 V of FCP/AFC reads below that window.
    if (bdp == 1 && bdm == 0 && dp_mv >= kDataIdleMinMv && dp_mv <= kDataIdleMaxMv) return Protocol::UsbData;
    if (vbus_mv < kBand5VMax) return band_5v(ring, bdp, bdm, dp_mv, dm_mv, cc_source_no_pd, dcp_proof);
    if (vbus_mv < kBand7VMax) return band_low_voltage(bdp, bdm);
    if (vbus_mv <= kBand9VMax) return band_9v(ring, bdp, bdm);
    if (vbus_mv >= kBand12VMin && vbus_mv <= kBand12VMax) return band_12v(ring, bdp, bdm);
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

// A charger handshake on the data pair: a pulsing or high bucket on either line, a steady 0.6 V rail
// below the USB data-idle window (FCP/AFC and a BC1.2 short hold the lines there; a data device's
// pull-up reads 0.7..1.0 V), or the Samsung divider levels. Idle lines - 0 V, the data pull-up, a
// steady ~2 V rail (the "MTK-like" levels of a phone on a PD contract, which the 9 V band reads as
// PD MTK, not as a handshake), and an equal 0.6 V pair in the bias window - are not a handshake.
// Only a full window counts. One line at 0.6 V with the other not (QC2 D+ 0.6 / D- 0, FCP, AFC) stays one.
bool handshake_pattern(const Ring& ring, int dp_mv, int dm_mv) {
    // A part-filled ring still holds the previous attach's samples (clear() resets the counters only).
    if (!ring.full()) return false;
    // An equal pair at or below 0.7 V is one level on both lines: a short carrying a sink's detection source (once the
    // K2's own idle, 0.6 V) or a floating short (420 / 387 mV after the D+/D- release), never a charger driving D+.
    if (const SteadyPair p = steady_pair(ring); p.quiet) return false;
    const int bdp = bucket(ring.dp), bdm = bucket(ring.dm);
    if (pulsing(bdp) || pulsing(bdm) || high(bdp) || high(bdm)) return true;
    if (bdp == 1 && dp_mv < kDataIdleMinMv) return true;
    if (bdm == 1 && dm_mv < kDataIdleMinMv) return true;
    if (dp_mv >= 1100 && dp_mv <= 1300 && dm_mv >= 1100 && dm_mv <= 1300) return true;   // Samsung 2A divider
    return false;
}

bool update_handshake_latch(bool latched, const Ring& ring, int dp_mv, int dm_mv) {
    return latched || (ring.full() && handshake_pattern(ring, dp_mv, dm_mv));
}

bool quiet_pair(const Ring& ring) {
    return ring.full() && steady_pair(ring).quiet;
}

bool equal_pair_moving(const Ring& ring) {
    if (!ring.full()) return false;
    int apart = 0, lo = 255, hi = 0, jumps = 0, prev = -1;
    for (std::size_t k = 0; k < kRingSize; k++) {
        const std::size_t i = (ring.next + k) % kRingSize;   // oldest first
        const int d = ring.dp[i], m = ring.dm[i];
        if ((d > m ? d - m : m - d) > 2) { apart++; continue; }   // lines apart in this sample
        const int level = (d + m) / 2;
        if (level < lo) lo = level;
        if (level > hi) hi = level;
        if (prev >= 0 && (level > prev ? level - prev : prev - level) > 2) jumps++;
        prev = level;
    }
    // A step, a ramp or a dip-and-return has a few jumps; a level toggling sample by sample is signalling. The pair stays
    // at 0.4 V or more (SE0 idle with a few packets is data). A stray sample with the lines apart (the two ADC reads fall
    // on either side of a step) makes the window moving too, so the first sample of a fall is not read as data.
    return apart <= kTrackApartMax && jumps <= kTrackJumpsMax && lo >= 4 && (hi - lo > 2 || apart > 0);
}

Protocol SagGuard::apply(Protocol fresh, int vbus_mv, bool lines_moving) {
    if (fresh == Protocol::NoInput) {   // a cut starts a new episode: the next attach refills the window from nothing
        held = fresh;
        sag_polls = 0;
        refill = static_cast<unsigned>(kRingSize);
        return fresh;
    }
    // The lines can collapse while VBUS is still near 5 V (hardware 2026-11-11: 5005 mV with the pair at 1.95 / 1.95 V),
    // so a pair that moves together starts the hold as well as the supply falling below vSafe5V min.
    const bool low = vbus_mv < kVsafe5MinMv || lines_moving;
    if (low) {
        if (sag_polls <= kSagHoldPolls) sag_polls++;
        refill = static_cast<unsigned>(kRingSize);
    }
    const bool active = (low || refill > 0) && sag_polls <= kSagHoldPolls;
    if (!low && refill > 0 && --refill == 0) sag_polls = 0;
    // Only a verdict read off the data pair is held; PD and PD? come from the FUSB302 and Pending is no verdict.
    if (fresh == Protocol::Pd || fresh == Protocol::PdLikely || fresh == Protocol::Pending) {
        if (!active) held = fresh;
        return fresh;
    }
    if (!active) {
        held = fresh;
        return fresh;
    }
    const bool held_is_data_verdict = held != Protocol::NoInput && held != Protocol::Pending && held != Protocol::Pd &&
                                      held != Protocol::PdLikely;
    return held_is_data_verdict ? held : Protocol::Pending;
}

DcpCheck dcp_check_result(int ref_dm_mv, int probed_dm_mv) {
    if (ref_dm_mv < kBiasMinDv * 100 || ref_dm_mv >= (kBiasMaxDv + 1) * 100) return DcpCheck::Invalid;
    return probed_dm_mv <= ref_dm_mv - kDcpDropMv ? DcpCheck::Shorted : DcpCheck::Open;
}

bool dcp_check_allowed(const DcpCheckGate& g, const Ring& ring) {
    if (g.done_this_attach) return false;
    if (g.vbus_mv < kVbusPresentMv || g.vbus_mv >= kBand5VMax) return false;   // the 5 V band only
    if (!g.not_inline || g.pd_seen || g.pd_likely || g.cc_source_no_pd) return false;
    if (!g.engine_idle || !g.lines_free) return false;
    return quiet_pair(ring);
}

}  // namespace k2::fastcharge
