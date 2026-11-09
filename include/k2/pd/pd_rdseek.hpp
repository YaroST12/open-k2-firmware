// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Rd seek: when the K2 puts its own Rd on a line that the passive scan picked (no Rd was on any line then), and nothing
// answers it, the line pick was wrong. Pure timing and budget (host-tested in tests/test_rdseek.cpp).
//
// Why a passive pick can be wrong: with no Rd on either line the comparator reads whatever the nets float to. A source that
// shows its Rp only once it sees an Rd (a sink-detect-first / DRP-toggling source, an Anker splitter fed by a 140 W
// charger) leaves its CC lines open, and a coupled net (the PLUS key shares CC2) can read 1.2 V or more with nothing behind
// it. The scan then reports an attach on that line. The first Rd the K2 presents (a re-read, the charger test START, a PD
// trigger) goes on the same wrong line, reads 0 V, and ended in a "no Rp on the active line" detach after 500 ms without
// the other line ever being tried (hardware 2026-11-09, runs/anker-split). A real sink presents its Rd on the line, and on
// no answer toggles to the other one: this does the same, one Rd at a time (never Rd on both lines: that pair is the Debug
// Accessory signature, a source that sees it turns its Rp off).
//
// The passive pick itself is not changed: an open Rp reads 3.3 V (rejected as VCONN-level already) and a source over a
// real sink's Rd reads 0.4-1.9 V, the range a coupled net also falls into, so no level alone tells the two apart inline.
#pragma once
#include <cstdint>

namespace k2::pd {

inline constexpr std::uint32_t kRdSeekDwellMs = 200;   // Rd on, no Rp this long: the line is wrong (a sink's tCCDebounce is 100-200 ms; the pin path's Rp came ~20 ms after the Rd)
inline constexpr std::uint8_t kRdSeekMoves = 3;        // A -> B -> A -> B; then the ordinary 500 ms no-Rp detach on the last line
// pattern slow-rp-wake (2026-11-09, Anker 2-way splitter behind a Baseus 140 W, runs/2026-11-09-splitter): a source that is
// there but asleep reads ~3.0-3.3 V OPEN (its standby sink-detect pull-up, high impedance) and collapses below 200 mV
// (BC_LVL=0) under our Rd - a real Rp never does that (even the default-USB Rp holds ~0.4 V against 5.1 kOhm). Its 3 A Rp
// showed 1072 ms after the Rd landed on its line (bb-pin2b.txt). So a line that read the standby signature open gets
// kSlowWakeDwellMs before the seek moves off it; a line without the signature keeps kRdSeekDwellMs (the wrong-line case
// of d615f3e), and if only the OTHER line had the signature the seek goes there first (moves at once).
inline constexpr std::uint32_t kSlowWakeDwellMs = 2000;   // the measured wake was 1072 ms
inline constexpr std::int32_t kStandbyOpenMv = 2800;       // the open standby reading is ~3.0-3.3 V; a coupled net reads less

class RdSeek {
public:
    // A fresh attach picked by a scan (or a detach): the whole budget again.
    void reset() { *this = RdSeek{}; }
    // The open (no Rd of ours on any line) ADC reading of a line, noted by the scan while no Rd is presented and kept for
    // when our Rd sits on a line: "high open, collapses under Rd" is the standby signature of a sleeping source.
    void note_open(std::uint8_t line, std::int32_t mv) { if (line >= 1 && line <= 2) open_mv_[line - 1] = mv; }
    bool standby(std::uint8_t line) const { return line >= 1 && line <= 2 && open_mv_[line - 1] >= kStandbyOpenMv; }
    // One call per poll while attached. `line`: the active CC line. `eligible`: the Rd on the active line is the K2's own,
    // put there by an active feature after the line was picked (never inline, never a key mode's pin, never the Ra mode,
    // never a forced line, never once PD traffic or VBUS has shown the line alive). `rp_seen`: the comparator sees an Rp on
    // the active line. Returns true once when the Rd should move to the other line; the caller then attaches there (the
    // next update starts a new dwell).
    bool update(std::uint32_t now, std::uint8_t line, bool eligible, bool rp_seen) {
        (void)line;
        if (!eligible || rp_seen) { silent_ = false; return false; }
        if (!silent_) { silent_ = true; since_ = now; return false; }
        if (moves_ >= kRdSeekMoves || now - since_ < kRdSeekDwellMs) return false;
        ++moves_;
        silent_ = false;
        return true;
    }
    // True while the seek is dwelling on a standby-signature line: the ordinary 500 ms no-Rp detach on the active line
    // must not fire inside that window (the sleeping source has not answered yet; that is not a detach).
    bool holding_slow(std::uint32_t now, std::uint8_t line) const {
        return silent_ && moves_ < kRdSeekMoves && standby(line) && now - since_ < kSlowWakeDwellMs;
    }
    std::uint8_t moves() const { return moves_; }
private:
    bool silent_ = false;
    std::uint32_t since_ = 0;
    std::uint8_t moves_ = 0;
    std::int32_t open_mv_[2] = {-1, -1};
};

}  // namespace k2::pd
