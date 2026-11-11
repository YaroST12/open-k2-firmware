// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Passive D+/D- charger classifier: a 50-sample
// decivolt ring per line, the 9-way waveform bucket and the VBUS-band decision tree that names the
// legacy charging protocol. Pure logic, no registers: tests/test_fastcharge_classify.cpp covers it.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::fastcharge {

inline constexpr std::size_t kRingSize = 50;

// Rolling window of one decivolt (0.1 V) sample per line.
struct Ring {
    std::array<std::uint8_t, kRingSize> dp{};
    std::array<std::uint8_t, kRingSize> dm{};
    std::size_t next = 0;      // slot the next push() writes
    std::size_t count = 0;     // samples held, saturates at kRingSize

    // Store one sample pair given in millivolts: dV = mV / 100 truncated, clamped to 0..255.
    void push(int dp_mv, int dm_mv);
    bool full() const { return count >= kRingSize; }
    void clear() { next = 0; count = 0; }
};

// Waveform bucket of one line's window, 0..8:
// 0 steady 0 V, 1 steady 0.6 V, 2 steady 2.7 V, 3 steady 3.3 V, 4 pulse train low<->high,
// 5 high rail with ripple, 6 low rail with ripple, 7 other mix, 8 steady 1.1..2.3 V.
int bucket(std::span<const std::uint8_t, kRingSize> samples);

enum class Protocol : std::uint8_t {
    Pending,    // the window has not filled yet
    NoInput,    // VBUS below the 4.0 V presence level
    NoQuick,    // 5 V and nothing recognisable on the data lines
    Dcp5V,      // BC1.2 dedicated charger: D+ shorted to D-
    Qc3, Qc2_5V, Qc2_9V, Qc2_12V, Qc2_20V,
    AfcFcp, Scp,
    Apple24, Apple21, Apple10,
    Usb2Hi, Usb11Low, Sam20,
    VoocWarp, Svooc,
    PdPe, PdMtk, Pd,
    UsbData,    // a data device idling on the pair (D+ pull-up ~0.8 V, D- ~0): no handshake at all
    PdLikely,   // CC attached, VBUS > 5.5 V, load flowing, caps not seen: a contract must exist
};

// Verdict from the two windows, the latest line voltages and VBUS (all millivolts).
// `pd_active`: the FUSB302 holds a PD contract or traffic (k2::link); "PD" then wins over any D+/D-
// reading, because a PD sink's data pair carries its own USB idle level, not a handshake.
// `pd_likely` (k2::link::pd_likely): CC attached, VBUS above 5.5 V and load flowing but no caps seen;
// the verdict is "PD?" instead of a data-pair guess.
// `cc_source_no_pd`: CC shows an attached source (Rp) but no Source_Capabilities were decoded, i.e. a
// default-USB data host. Its D+/D- pair carries data, so the 5 V band says USB data, never a short.
// `cc_attach_quiet`: no D+/D- handshake seen this attach (latched by the caller over the window below);
// together with `cc_source_no_pd` and VBUS above vSafe5V it means a PD contract the K2 did not decode
// (a settled fixed contract sends no frame for many seconds): the verdict is "PD", never a legacy
// mode (ROADMAP 2026-11-10, `fc: PD MTK`). The passive CC ADC levels are deliberately not an input
// here: the CC2 bias net reads ~1.69 V with nothing attached, and only the FUSB302's own Rp
// detection counts as "CC attached".
// A DCP (D+ shorted to D-) is only concluded from a level that sits still over the whole window:
// a live data link averages equal too, but its levels move with bus activity. Passively that needs an
// equal steady pair of at least 0.4 V outside the bias window (0.5 .. 0.79 V on both lines). A bias-window
// pair is a short carrying someone's 0.6 V detection source (until 2026-11-11 the K2's own idle drove both
// lines there) and proves no short on its own; a floating short reads equal and low (the K2's idle releases
// both lines). Both read NO QUICK passively; `dcp_proof` (the active check below found D- following a
// pulled-low D+) turns any quiet pair (quiet_pair) into DCP 5V.
// `lines_floating`: the inline VOOC monitor owns D+/D- (dpsniff::monitoring(): Sniff floating inputs, a data
// device inline). The levels are then the device's and the charger's, not a handshake: USB DATA (PD still wins).
Protocol classify(const Ring& ring, int dp_mv, int dm_mv, int vbus_mv, bool pd_active = false, bool pd_likely = false,
                  bool cc_source_no_pd = false, bool cc_attach_quiet = true, bool dcp_proof = false,
                  bool lines_floating = false);

// The window holds a quiet pair: full, both lines equal and steady at or below 0.79 V.
bool quiet_pair(const Ring& ring);

// Active DCP check (steps in dpdm::kDcpProbe, one per poll): D+ to 0 V with D- released (clears any HVDCP
// state), then D- driven to 0.6 V with D+ released (the reference reading `ref_dm_mv`), then D+ pulled to 0 V
// with D- still on 0.6 V (`probed_dm_mv`). A BC1.2 short drags D- down with D+; an open pair leaves it on its
// 0.6 V. Both lines use the K2's symmetric 0.6 V / 0 V dividers, there is no strong / weak asymmetry between
// them, so the decision is relative to the reference: shorted when D- fell by at least kDcpDropMv. Invalid when
// the reference is outside the bias window (0.5 .. 0.79 V: something else pulls D-), which gives no verdict.
// Unverified on hardware.
inline constexpr int kDcpDropMv = 150;
enum class DcpCheck : std::uint8_t { Invalid, Open, Shorted };
DcpCheck dcp_check_result(int ref_dm_mv, int probed_dm_mv);

// Everything that must hold before the check may drive the lines. The caller fills it from the other
// drivers (link, fctrig, dpdm); the rule is pure.
struct DcpCheckGate {
    int vbus_mv = 0;
    bool not_inline = false;        // k2::link::inline_now() is None: nothing is plugged in behind the K2
    bool pd_seen = false;           // a PD contract or traffic (probe.pd_active)
    bool pd_likely = false;         // CC attached, VBUS above 5.5 V, load flowing, no caps seen
    bool cc_source_no_pd = false;   // CC shows an attached source with no PD: a default-USB data host
    bool engine_idle = false;       // no fctrig trigger, scan, UFCS probe or reset hold
    bool lines_free = false;        // both lines at the passive idle (dpdm::mismatch(dpdm::drive()) == 0)
    bool done_this_attach = false;  // the check ran once already for this attach
};
bool dcp_check_allowed(const DcpCheckGate& g, const Ring& ring);

// True when the window shows a charger handshake on the data pair: a pulsing or high bucket on either
// line, a steady 0.6 V rail below the USB data-idle window (FCP/AFC), a BC1.2 short, or the Samsung
// divider levels. Idle lines (0 V or floating low, the ~0.8 V data pull-up, a steady ~2 V, an equal pair
// in the bias window) are not a handshake.
bool handshake_pattern(const Ring& ring, int dp_mv, int dm_mv);

// The "handshake seen this attach" latch: stays set once set, and is only fed from a full window
// that is not an equal bias-window pair.
bool update_handshake_latch(bool latched, const Ring& ring, int dp_mv, int dm_mv);

// A live, proven VOOC session counts only with VBUS present: without it nothing is live. The one rule behind both the
// remote/shell label (report_label) and the Home protocol row (home::derive_proto).
constexpr bool vooc_session_live(Protocol verdict, bool vooc_live) {
    return vooc_live && verdict != Protocol::NoInput;
}

// The verdict as the remote and the shell report it, so they say what Home says: a live, proven VOOC session names the
// source SVOOC whatever the passive classifier read (USB DATA while the lines float, PD next to a contract). Without VBUS
// nothing is live.
constexpr Protocol report_label(Protocol verdict, bool vooc_live) {
    return vooc_session_live(verdict, vooc_live) ? Protocol::Svooc : verdict;
}

// Output sag guard. A source whose VBUS sags or is cut (a port that switches off with no load) drags the data lines down
// with it: the window mixes two levels, or the Type-C data-host tail reads a divider level as data, so the classifier
// flaps to USB DATA between a real verdict and NO INPUT. Below vSafe5V min (4.75 V), and for one window after VBUS
// returns (the ring still holds sag samples), the data pair is no basis for a judgment: the guard keeps the last verdict
// that was reached with a good supply, or says Pending when there is none. NO INPUT, PD and PD? come from CC / VBUS /
// the FUSB302 and pass through. A source that simply sits below 4.75 V (weak, loaded) is judged again after
// kSagHoldPolls polls (about 5 s at the 20 ms poll). Real data behind the K2 reads USB DATA with a good supply as before.
inline constexpr int kVsafe5MinMv = 4750;
inline constexpr unsigned kSagHoldPolls = 250;
struct SagGuard {
    Protocol held = Protocol::Pending;   // last verdict reached outside a sag
    unsigned sag_polls = 0;              // consecutive polls of the current sag, saturating at kSagHoldPolls + 1
    unsigned refill = 0;                 // polls still owed for the window to refill after the supply returned

    Protocol apply(Protocol fresh, int vbus_mv);
    void reset() { *this = SagGuard{}; }
};

// Display string of a verdict (including "NO INPUT" and "..."); "?" if unknown.
const char* name(Protocol p);

// At most 8 characters, for the Home status bar.
const char* short_name(Protocol p);

}  // namespace k2::fastcharge
