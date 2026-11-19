// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, the plan: which levels a Source_Capabilities list gives the checks (pure, host-tested in tests/test_pdspec_plan.cpp).
//
// Fixed transitions: one walk through every ordered pair of fixed levels (up and down), starting and ending at 5 V, so each pair is
// measured exactly once and no unmeasured step sits between two measured ones (an Euler circuit of the complete directed graph).
// EPR fixed levels at or below the K2's monitor ceiling join the walk when the cable proof is there.
// Programmable windows (SPR PPS APDOs, then the SPR AVS APDO and, in a Deep run with the cable proof, the EPR AVS APDO), modelled on the USB-C Source Power Test Specification SPT.6 procedure
// (E.2.k): per step size, up from the window's minimum while a whole step fits, then the rest up to its maximum, and down the same way;
// then the two slams (minimum to maximum and back). PPS steps 20, 100 and 500 mV (the 20 mV pass in three windows by default, the
// whole range as an option), AVS (SPR and EPR) 100, 500 and 1000 mV.
#pragma once

#include <array>
#include <cstdint>

#include "k2/pd/pd.hpp"
#include "k2/pdspec/result.hpp"

namespace k2::pdspec {

inline constexpr std::size_t kMaxNodes = 8;                       // fixed levels in the walk (7 SPR + 1 EPR)
inline constexpr std::size_t kMaxWalk = kMaxNodes * (kMaxNodes - 1) + 1;
inline constexpr std::size_t kMaxApdos = 4;                       // SPR PPS APDOs kept for the keep-alive family
inline constexpr std::size_t kMaxProg = 4;                        // programmable windows swept: up to 4, the AVS ones (SPR, EPR) first kept
inline constexpr std::size_t kClasses = 4;                        // per window: three step sizes, then the slams
inline constexpr std::uint8_t kSlam = 3;                          // the class of the slams
inline constexpr std::uint8_t kWindowSteps = 10;                  // a 20 mV window: this many steps up, as many down
inline constexpr std::uint8_t kCoverSteps = 3;                    // a covered window's cover: large steps up from the minimum, then back
inline constexpr std::uint8_t kCoverEndSteps = 5;                 // ... and resolution steps onto each end
inline constexpr std::int32_t kMinSpanMv = 1000;                  // a window narrower than this is not swept
// The floor of a window: none on a K2 powered from the computer (the run refuses a K2 powered by the charger under test, and the
// passive detach rule only fires under 3.0 V, under every SPR PPS minimum). A caller that has to keep its own rail passes 5000.
inline constexpr std::int32_t kHostFloorMv = 0;
inline constexpr std::int32_t kPpsKaMv = 5000;                    // the PPS keep-alive check holds this level (inside the window)

enum class ProgKind : std::uint8_t { Pps, Avs };
// The step sizes of a kind, by class (the slams have none): PPS 20 / 100 / 500 mV (resolution, a middle step, vPpsSmallStep), SPR AVS
// 100 / 500 / 1000 mV (resolution, a middle step, vAvsSmallStep). Reference: docs/PD_PPS_AVS_SPEC_REFERENCE.md, sections 1.1 and 4.
constexpr std::int32_t step_mv(ProgKind k, std::uint8_t cls) {
    constexpr std::int32_t pps[3] = {20, 100, 500}, avs[3] = {100, 500, 1000};
    return cls >= kSlam ? 0 : k == ProgKind::Pps ? pps[cls] : avs[cls];
}
constexpr std::int32_t resolution_mv(ProgKind k) { return k == ProgKind::Pps ? 20 : 100; }

struct Prog {
    std::uint8_t pos = 0;           // the APDO's position
    ProgKind kind = ProgKind::Pps;
    std::int32_t lo = 0, hi = 0;    // the swept window (mV, on the kind's resolution)
};

struct Levels {
    std::array<std::uint8_t, kMaxNodes> fixed{};   // positions, ascending voltage, fixed[0] = the 5 V PDO
    std::uint8_t n_fixed = 0;
    std::uint8_t top_spr = 0;                       // the highest SPR fixed position
    std::uint8_t epr = 0;                           // the lowest EPR fixed position at or below the cap (0 = none usable)
    bool epr_offered = false;                       // EPR in PDO 1, or an EPR fixed level in the list
    std::array<std::uint8_t, kMaxApdos> pps{};      // SPR PPS positions with a swept window (the keep-alive family uses the first)
    std::uint8_t n_pps = 0;
    std::array<Prog, kMaxProg> prog{};              // the windows swept: PPS first (list order), then the SPR and the EPR AVS APDO
    std::uint8_t n_prog = 0;
    std::uint8_t n_avs = 0;                         // SPR AVS APDOs in the list (the caps note: PPS with or without AVS)
};

// The window of a programmable APDO: [max(min, floor), min(max, cap)], rounded inwards to the kind's resolution.
inline Prog prog_window(const pd::Pdo& d, std::uint8_t pos, std::uint32_t cap_mv, std::int32_t floor_mv) {
    Prog p;
    p.pos = pos;
    p.kind = d.type == pd::PdoType::Avs ? ProgKind::Avs : ProgKind::Pps;
    const std::int32_t r = resolution_mv(p.kind);
    const std::int32_t lo = d.min_mv > floor_mv ? d.min_mv : floor_mv;
    const std::int32_t hi = d.max_mv < std::int32_t(cap_mv) ? d.max_mv : std::int32_t(cap_mv);
    p.lo = (lo + r - 1) / r * r;
    p.hi = hi / r * r;
    return p;
}

// cap_mv: the monitor ceiling; epr_proof: the cable proof EPR levels need. EPR levels above the cap are left out. floor_mv: the lowest
// programmable level asked for (kHostFloorMv on a K2 powered from the computer). epr_avs: the EPR AVS APDO a Deep run met at position
// epr_pos (the live list is the SPR one once the run left EPR mode): a window too, with the cable proof (the sink enters EPR mode for
// its Request).
inline Levels scan(const pd::SourceCaps& caps, std::uint32_t cap_mv, bool epr_proof, std::int32_t floor_mv = kHostFloorMv,
                   const pd::Pdo* epr_avs = nullptr, std::uint8_t epr_pos = 0) {
    Levels L;
    if (!caps.valid) return L;
    const std::size_t n = caps.num_pdos < caps.pdos.size() ? caps.num_pdos : caps.pdos.size();
    if (n > 0 && caps.pdos[0].type == pd::PdoType::Fixed && (caps.pdos[0].raw >> 23 & 1)) L.epr_offered = true;
    for (std::size_t i = 0; i < n; ++i) {
        const pd::Pdo& d = caps.pdos[i];
        const auto pos = std::uint8_t(i + 1);
        if (d.type == pd::PdoType::Fixed && d.max_mv <= cap_mv) {
            if (pos <= 7) {
                if (L.n_fixed < kMaxNodes - 1) {
                    std::size_t k = L.n_fixed++;
                    while (k > 0 && caps.pdos[L.fixed[k - 1] - 1].max_mv > d.max_mv) { L.fixed[k] = L.fixed[k - 1]; --k; }
                    L.fixed[k] = pos;
                }
                if (L.top_spr == 0 || d.max_mv >= caps.pdos[L.top_spr - 1].max_mv) L.top_spr = pos;
            } else {
                L.epr_offered = true;
                if (epr_proof && (L.epr == 0 || d.max_mv < caps.pdos[L.epr - 1].max_mv)) L.epr = pos;
            }
        } else if (d.type == pd::PdoType::Fixed && pos >= 8) {
            L.epr_offered = true;
        } else if (d.type == pd::PdoType::Pps && pos <= 7 && L.n_pps < kMaxApdos) {
            const Prog p = prog_window(d, pos, cap_mv, floor_mv);
            if (p.hi - p.lo >= kMinSpanMv) {
                L.pps[L.n_pps++] = pos;
                if (L.n_prog < kMaxProg) L.prog[L.n_prog++] = p;
            }
        } else if (d.type == pd::PdoType::Avs && pos <= 7) {
            ++L.n_avs;
        }
    }
    // The SPR AVS APDO, then the EPR one, after the PPS windows: the last PPS windows give their slots up when every slot is taken.
    std::array<Prog, 2> av{};
    std::size_t n_av = 0;
    for (std::size_t i = 0; i < n && i < 7; ++i) {
        const pd::Pdo& d = caps.pdos[i];
        if (d.type != pd::PdoType::Avs) continue;
        const Prog p = prog_window(d, std::uint8_t(i + 1), cap_mv, floor_mv);
        if (p.hi - p.lo < kMinSpanMv) continue;
        av[n_av++] = p;
        break;
    }
    if (epr_avs && epr_proof && epr_pos >= 8 && epr_avs->type == pd::PdoType::Avs) {
        const Prog p = prog_window(*epr_avs, epr_pos, cap_mv, floor_mv);
        if (p.hi - p.lo >= kMinSpanMv) av[n_av++] = p;
    }
    if (L.n_prog + n_av > kMaxProg) L.n_prog = static_cast<std::uint8_t>(kMaxProg - n_av);
    for (std::size_t k = 0; k < n_av; ++k) L.prog[L.n_prog++] = av[k];
    if (L.epr) L.fixed[L.n_fixed++] = L.epr;   // the EPR level is the walk's highest node
    return L;
}

// The caps note (advisory): which programmable APDOs the SPR list carries, whatever the ceiling.
inline CapsNote caps_note(const pd::SourceCaps& caps) {
    bool pps = false, avs = false;
    const std::size_t n = caps.num_pdos < caps.pdos.size() ? caps.num_pdos : caps.pdos.size();
    for (std::size_t i = 0; i < n && i < 7; ++i) {
        if (caps.pdos[i].type == pd::PdoType::Pps) pps = true;
        if (caps.pdos[i].type == pd::PdoType::Avs) avs = true;
    }
    return pps ? (avs ? CapsNote::PpsAvs : CapsNote::PpsNoAvs) : avs ? CapsNote::AvsOnly : CapsNote::None;
}

// The walk: node indices into Levels::fixed. Every ordered pair (a, b), a != b, appears exactly once as consecutive entries; the walk
// starts and ends at node 0 (5 V). Length n*(n-1)+1, or 0 with fewer than two nodes. Hierholzer's algorithm on fixed arrays.
struct Walk {
    std::array<std::uint8_t, kMaxWalk> node{};
    std::uint8_t len = 0;
};

inline Walk make_walk(std::uint8_t n) {
    Walk w;
    if (n < 2 || n > kMaxNodes) return w;
    std::array<std::uint8_t, kMaxNodes> next{};     // per node: the next out-edge target to use (skipping itself)
    std::array<std::uint8_t, kMaxWalk> stack{};
    std::size_t sp = 0;
    stack[sp++] = 0;
    std::array<std::uint8_t, kMaxWalk> out{};
    std::size_t no = 0;
    while (sp > 0) {
        const std::uint8_t v = stack[sp - 1];
        if (next[v] == v) ++next[v];
        if (next[v] < n) {
            const std::uint8_t u = next[v]++;
            if (next[v] == v) ++next[v];
            stack[sp++] = u;
        } else {
            out[no++] = v;
            --sp;
        }
    }
    // Hierholzer emits the circuit in reverse.
    for (std::size_t i = 0; i < no; ++i) w.node[i] = out[no - 1 - i];
    w.len = std::uint8_t(no);
    return w;
}

// One move of a sweep: the level to ask for and whether it is a measured step (false: a move between two 20 mV windows).
struct Move {
    std::int32_t mv = 0;
    bool measured = false;
};

// One pass over a window (pure, no buffer): next() hands out the moves in order, starting from the window's minimum (the caller is
// there before the pass) and ending back at it.
class Sweep {
public:
    Sweep() = default;
    Sweep(std::int32_t lo, std::int32_t hi, std::int32_t step, bool windows)
        : lo_(lo), hi_(hi), step_(step), cur_(lo), windows_(windows), half_(0) {}
    // The cover's resolution pass (make_cover_sweep): kCoverEndSteps down onto the minimum, as many up onto the maximum, then back to
    // the minimum (the moves onto each start are not measured).
    static Sweep ends(std::int32_t lo, std::int32_t hi, std::int32_t step) {
        Sweep s(lo, hi, step, false);
        s.ends_ = true;
        return s;
    }
    // half(): 0 while the moves go up, 1 once they come down, 2 when the pass is over.
    std::uint8_t half() const { return half_; }
    bool next(Move& m) {
        if (step_ == 0) {   // the slams: the maximum, then the minimum
            if (half_ >= 2) return false;
            m = {half_ == 0 ? hi_ : lo_, true};
            ++half_;
            return true;
        }
        if (ends_) {
            while (half_ < 2) {
                const std::int32_t start = half_ == 0 ? lo_ + kCoverEndSteps * step_ : hi_ - kCoverEndSteps * step_;
                if (k_ == 0) {
                    ++k_;
                    if (cur_ != start) { cur_ = start; m = {cur_, false}; return true; }
                }
                if (k_ <= kCoverEndSteps) { ++k_; cur_ += half_ == 0 ? -step_ : step_; m = {cur_, true}; return true; }
                half_ = static_cast<std::uint8_t>(half_ + 1);
                k_ = 0;
            }
            if (cur_ == lo_) return false;
            cur_ = lo_;
            m = {cur_, false};
            return true;
        }
        if (!windows_) {
            if (half_ == 0) {
                if (cur_ < hi_) { cur_ = cur_ + step_ <= hi_ ? cur_ + step_ : hi_; m = {cur_, true}; return true; }
                half_ = 1;
            }
            if (half_ == 1 && cur_ > lo_) { cur_ = cur_ - step_ >= lo_ ? cur_ - step_ : lo_; m = {cur_, true}; return true; }
            half_ = 2;
            return false;
        }
        while (half_ < 2) {
            if (win_ >= 3) { half_ = static_cast<std::uint8_t>(half_ + 1); win_ = 0; k_ = 0; continue; }
            const std::int32_t span = kWindowSteps * step_;
            const std::int32_t mid = ((lo_ + hi_) / 2 - span / 2) / step_ * step_;
            const std::int32_t start = half_ == 0 ? (win_ == 0 ? lo_ : win_ == 1 ? mid : hi_ - span)
                                                  : (win_ == 0 ? hi_ : win_ == 1 ? mid + span : lo_ + span);
            if (k_ == 0 && cur_ != start) { cur_ = start; m = {cur_, false}; return true; }
            if (k_ < kWindowSteps) { ++k_; cur_ += half_ == 0 ? step_ : -step_; m = {cur_, true}; return true; }
            ++win_;
            k_ = 0;
        }
        return false;
    }

private:
    std::int32_t lo_ = 0, hi_ = 0, step_ = 0, cur_ = 0;
    bool windows_ = false, ends_ = false;
    std::uint8_t half_ = 2, win_ = 0, k_ = 0;
};

// The pass of class cls over window p. The 20 mV PPS pass runs in three windows unless full20 asks for the whole range (or the window
// is too narrow for three).
inline Sweep make_sweep(const Prog& p, std::uint8_t cls, bool full20) {
    const std::int32_t step = step_mv(p.kind, cls);
    const bool windows = p.kind == ProgKind::Pps && cls == 0 && !full20 && p.hi - p.lo >= 3 * kWindowSteps * step;
    return Sweep(p.lo, p.hi, step, windows);
}

// The passes of a window the charger test swept already (its moves judged into the engine): class 2 = kCoverSteps large steps up from
// the minimum and back, class 0 = the ends, class 1 = nothing, the slams as in make_sweep. Down-steps and the ends are judged again.
static_assert(2 * kCoverEndSteps * 100 <= kMinSpanMv, "the AVS ends fit in the narrowest window");
inline constexpr std::uint8_t kCoverPasses = 3;   // classes 0, 2 and the slams
inline Sweep make_cover_sweep(const Prog& p, std::uint8_t cls) {
    const std::int32_t step = step_mv(p.kind, cls);
    if (cls == 0) return Sweep::ends(p.lo, p.hi, step);
    if (cls == 1) return Sweep{};
    if (cls == 2) return Sweep(p.lo, p.lo + kCoverSteps * step < p.hi ? p.lo + kCoverSteps * step : p.hi, step, false);
    return make_sweep(p, cls, false);
}

// How many measured moves a pass / the whole family makes: a run's time budget is about this many capture windows.
inline std::uint32_t measured_moves(Sweep s) {
    std::uint32_t n = 0;
    Move m;
    while (s.next(m)) n += m.measured;
    return n;
}
inline std::uint32_t cover_moves(const Prog& p) {
    std::uint32_t n = 0;
    for (std::uint8_t c = 0; c < kClasses; ++c) n += measured_moves(make_cover_sweep(p, c));
    return n;
}
inline std::uint32_t family_moves(const Levels& L, bool full20) {
    std::uint32_t n = 0;
    for (std::size_t w = 0; w < L.n_prog; ++w)
        for (std::uint8_t c = 0; c < kClasses; ++c) n += measured_moves(make_sweep(L.prog[w], c, full20));
    return n;
}

}  // namespace k2::pdspec
