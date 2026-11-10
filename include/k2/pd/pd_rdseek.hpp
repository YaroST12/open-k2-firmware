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
    // A fresh attach picked by a scan (or a detach): the whole budget again. With `keep_open` the open readings STAY:
    // they were taken while no Rd of ours was on any line and are the only standby-signature evidence a later Rd
    // presentation has (the sleeping-port row, 2026-11-10: reset() used to wipe them on attach, so an idle K2 never held
    // a steady Rd long enough to wake a sleeping source). The driver passes the Wake sleeping port setting: off, the
    // readings are wiped exactly as before that feature (the seek's slow-dwell evidence then lives only until the next
    // attach, as it always did).
    void reset(bool keep_open = true) {
        silent_ = false; since_ = 0; moves_ = 0;
        if (!keep_open) open_mv_[0] = open_mv_[1] = -1;
    }
    void clear() { *this = RdSeek{}; }   // a boot: nothing is known about any line yet
    // The open (no Rd of ours on any line) ADC reading of a line, noted by the scan while no Rd is presented and kept for
    // when our Rd sits on a line: "high open, collapses under Rd" is the standby signature of a sleeping source.
    void note_open(std::uint8_t line, std::int32_t mv) { if (line >= 1 && line <= 2) open_mv_[line - 1] = mv; }
    std::int32_t open_mv(std::uint8_t line) const { return line >= 1 && line <= 2 ? open_mv_[line - 1] : -1; }
    bool standby(std::uint8_t line) const { return line >= 1 && line <= 2 && open_mv_[line - 1] >= kStandbyOpenMv; }
    // One call per poll while attached. `line`: the active CC line. `eligible`: the Rd on the active line is the K2's own,
    // put there by an active feature after the line was picked (never inline, never a key mode's pin, never the Ra mode,
    // never a forced line, never once PD traffic or VBUS has shown the line alive). `rp_seen`: the comparator sees an Rp on
    // the active line. Returns true once when the Rd should move to the other line; the caller then attaches there (the
    // next update starts a new dwell).
    bool update(std::uint32_t now, std::uint8_t line, bool eligible, bool rp_seen) {
        if (!eligible || rp_seen) { silent_ = false; return false; }
        if (moves_ >= kRdSeekMoves) return false;
        // pattern slow-rp-wake: the active line read the standby signature open and is now collapsing under our Rd (no
        // Rp) - the source is there but asleep, its Rp measured 1072 ms after the Rd: stay kSlowWakeDwellMs before
        // moving. If only the OTHER line had the signature, go there first: move at the first silent poll.
        if (!standby(line) && standby(line == 1 ? 2 : 1)) { ++moves_; silent_ = false; return true; }
        const std::uint32_t dwell = standby(line) ? kSlowWakeDwellMs : kRdSeekDwellMs;
        if (!silent_) { silent_ = true; since_ = now; return false; }
        if (now - since_ < dwell) return false;
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

// The idle wake (pattern slow-rp-wake, idle path, 2026-11-10): an idle K2 with no source detected and no Rd of its
// own presents none, so a sleeping source - the splitter's port with nothing attached, one CC line reading the
// standby signature open, VBUS 78 mV, "No source" - is never asked and never wakes; only a pinned re-read woke it
// (be3f589). So the idle/no-source path treats a line that read the standby signature open (>= kStandbyOpenMv) as a
// sleeping source and holds our Rd on that ONE line for kSlowWakeDwellMs at a time: a sleeping source's Rp measured
// 1072 ms after the Rd, an awake source's Rp answers within tCCDebounce, and the ~2 Hz scan attaches on the line
// once it does (the PD list on screen without any pin). When both lines read the signature the hold alternates line
// by line every dwell - never Rd on both lines (the Debug Accessory signature). The caller gates this to the plain
// passive idle: never with a sink downstream, a boot pin, a run start, an active operation, a forced line or a live
// VBUS - nobody else owns the idle Rd.
// It is an opt-in (Settings > Wake sleeping port, default off): an idle Rd breaks the passive-idle invariants (a plain
// boot and a waiting charger present no Rd), so disabled it picks no line, whatever the open readings say.
class IdleWake {
public:
    void reset() { holding_ = false; line_ = 0; attached_ = 0; last_ = 0; since_ = 0; }   // the setting itself stays
    void set_enabled(bool on) { enabled_ = on; if (!on) reset(); }
    bool enabled() const { return enabled_; }
    // One call per idle poll. `open1`/`open2`: the open (no-Rd) readings kept by the scan (RdSeek::open_mv, noted
    // while no Rd is presented, so they stay put while the hold has our Rd on). Returns the line to hold Rd on
    // (1/2), or 0 when no line reads the signature. A dwell that ran out re-arms in the same call, so the Rd is
    // held steadily (the sleeping source must see one steady Rd, not a blinking one).
    std::uint8_t update(std::uint32_t now, std::int32_t open1, std::int32_t open2) {
        attached_ = 0;   // an idle poll means no attach is holding the window open any more
        if (!enabled_) return 0;
        if (holding_ && now - since_ >= kSlowWakeDwellMs) { holding_ = false; line_ = 0; }   // the dwell ran out: release, re-decide now
        if (!holding_) {
            const bool s1 = open1 >= kStandbyOpenMv, s2 = open2 >= kStandbyOpenMv;
            std::uint8_t pick = 0;
            if (s1 && s2) pick = last_ == 1 ? 2 : last_ == 2 ? 1 : (open1 >= open2 ? 1 : 2);
            else if (s1) pick = 1;
            else if (s2) pick = 2;
            if (pick != 0) { holding_ = true; line_ = pick; last_ = pick; since_ = now; }
        }
        return line_;
    }
    // An attach took the line while the hold was on it: the 500 ms no-Rp detach must stay off for the rest of the
    // dwell window, same as the seek's holding_slow (the sleeping source may still be waking when the scan picks
    // its line - that is not a detach).
    void on_attach(std::uint8_t line) { if (holding_) { attached_ = line; holding_ = false; line_ = 0; } }
    // The Rp answered on the active line: the window is over, exactly like the seek's rp_seen clearing silent_.
    void on_rp_seen() { attached_ = 0; }
    bool holding_slow(std::uint32_t now, std::uint8_t line, std::int32_t open_mv_line) const {
        return attached_ != 0 && attached_ == line && open_mv_line >= kStandbyOpenMv && now - since_ < kSlowWakeDwellMs;
    }
    std::uint8_t line() const { return holding_ ? line_ : 0; }
private:
    bool enabled_ = false;       // Settings > Wake sleeping port
    bool holding_ = false;
    std::uint8_t line_ = 0;      // the held line
    std::uint8_t attached_ = 0;  // the line an attach took over during the hold (the detach hold-off window)
    std::uint8_t last_ = 0;      // the line held last (alternate when both lines read the signature)
    std::uint32_t since_ = 0;    // when the current hold started
};

}  // namespace k2::pd
