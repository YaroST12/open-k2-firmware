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
Protocol classify(const Ring& ring, int dp_mv, int dm_mv, int vbus_mv, bool pd_active = false, bool pd_likely = false);

// Display string of a verdict (including "NO INPUT" and "..."); "?" if unknown.
const char* name(Protocol p);

// At most 8 characters, for the Home status bar.
const char* short_name(Protocol p);

// The idle state of the four D+/D- switch pins is PA7 high, PB0 low, PB1 high,
// PB2 low. Returns a bit per pin that differs: bit0 PA7, bit1 PB0, bit2 PB1, bit3 PB2.
unsigned idle_mismatch(bool pa7, bool pb0, bool pb1, bool pb2);

}  // namespace k2::fastcharge
