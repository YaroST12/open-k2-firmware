// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The voltage fold / track break finding on an UPWARD sweep of a programmable (PPS or AVS) range, pure rules shared by the
// charger-test deep sweeps and the PD Spec SPT.6 sweeps (host-tested in tests/test_prog_track.cpp).
//
// Input: the ordered (requested, settled) points of one APDO's upward sweep, in mV, the settled value being the same one the
// step verdict uses (a tail mean, not a single sample). The finding is extra to the per-step verdicts: a source that tracks
// 60-95 mV low passes every row of a +-5 % band and still shows the shape the FH22 has, where every request at or above 20.0 V
// folds to its fixed 20 V setpoint at 19.40-19.43 V (a -481 mV step between the 19.98 V and 20.00 V requests of a 20 mV run).
//
//   Fold  = the settled level is MORE than kTrackFoldMv BELOW the previous step's settled level while the request rose.
//           The margin comes from the meter and the spec: the monitor scatters by sd ~3.5 mV and up to +-13 mV per sample (the
//           tail mean far less), two settled values differ by 26 mV at the very worst, and the programming DNL is -1 LSB
//           (-20 mV: a 20 mV request may give 0 mV, so "equal or a little lower" is the spec, never a finding). 50 mV is above
//           both together with room. Equal or a tiny fall is not a fold; a fall of exactly 50 mV is not one either.
//   Break = no fold, but the offset (request - settled) GREW by more than kTrackBreakMv between neighbouring steps: the
//           output fell behind the request without falling (a plateau at a 500 mV step). Offsets of real sources move by under
//           ~100 mV between neighbours (FH22 tracks within 95 mV, the others within 70 mV), so 150 mV clears them. At steps of
//           100 mV or less a fall-behind of that size is already a fold, so a break only shows at 500 mV and up. An offset that
//           SHRINKS is the output catching up (a first point still settling, a low edge) - not a source that stopped following.
//   Only the FIRST finding of a sweep is kept (after a fold every later point sits on the plateau). A request that did not
//   rise (a downward step, a repeat) is not judged: the rules are for the upward sweep.
#pragma once
#include <cstdint>

namespace k2::pd {

enum class Track : uint8_t { None = 0, Fold, Break };

inline constexpr int kTrackFoldMv = 50;    // the settled level fell by more than this while the request rose
inline constexpr int kTrackBreakMv = 150;  // the offset (request - settled) grew by more than this without a fold

// One step: the point before (prev_req, prev_settled) and the point now. Stateless.
constexpr Track track_step(int prev_req, int prev_settled, int req, int settled) {
    if (req <= prev_req) return Track::None;
    const int delta = settled - prev_settled;
    if (delta < -kTrackFoldMv) return Track::Fold;
    const int fell_behind = (req - prev_req) - delta;   // how far the offset grew (< 0: the output caught up, never a finding)
    return fell_behind > kTrackBreakMv ? Track::Break : Track::None;
}

// A sweep in progress: feed() the points in order, read the first finding. The first point only primes the scan.
class TrackScan {
public:
    void clear() { *this = TrackScan{}; }
    void feed(int req_mv, int settled_mv) {
        if (primed_ && kind_ == Track::None) {
            const Track k = track_step(prev_req_, prev_settled_, req_mv, settled_mv);
            if (k != Track::None) { kind_ = k; req_ = req_mv; step_ = req_mv - prev_req_; from_ = prev_settled_; to_ = settled_mv; }
        }
        prev_req_ = req_mv;
        prev_settled_ = settled_mv;
        primed_ = true;
    }
    bool found() const { return kind_ != Track::None; }
    Track kind() const { return kind_; }
    int req_mv() const { return req_; }        // the request that folded / broke
    int from_mv() const { return from_; }      // the settled level of the step before
    int to_mv() const { return to_; }          // the settled level of the step itself
    int step_mv() const { return step_; }      // the request step (this request minus the previous one)

private:
    int prev_req_ = 0, prev_settled_ = 0;
    int req_ = 0, from_ = 0, to_ = 0, step_ = 0;
    Track kind_ = Track::None;
    bool primed_ = false;
};

}  // namespace k2::pd
