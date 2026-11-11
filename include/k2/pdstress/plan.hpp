// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress sequences: what each one sends, in what order, at what pace, how many cycles. Pure code over the Source_Capabilities in hand
// (host-tested in tests/test_pdstress_plan.cpp). Only advertised PDOs and APDO windows are ever planned, never more current than
// advertised, never above the monitor ceiling. The engine (engine.hpp) walks these steps; nothing here touches hardware.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pdstress/dist.hpp"

namespace k2::pdstress {

// The sequences, in run order. Ids are stable (journal, wire).
enum class Seq : std::uint8_t {
    Ladder,       // P1  fixed ladder up and down
    Slam,         // P2  5 V <-> the top fixed level
    SameReq,      // P3  the same Request again and again at 9 V and at the top level
    Reannounce,   // P4  solicited Source_Capabilities (Get_Source_Cap)
    FixedPps,     // P5  fixed <-> PPS at the same voltage
    PpsSteps,     // P6  PPS steps of 20 mV / 100 mV / 500 mV / 2 V
    AvsSteps,     // P7  SPR AVS steps of 100 mV / 1 V / 2 V
    CurrentOnly,  // P8  the same voltage, half the current and back
    Epr,          // P9  EPR 5 V / 20 V <-> 28 V (opt-in)
    SoftReset,    // P10 repeated Soft_Reset (legal probe)
    PpsLow,       // P11 PPS requests below 5 V (legal probe, needs an APDO that starts below 5 V)
    Mismatch,     // P12 Capability Mismatch set with no real mismatch (legal probe)
    Count
};
inline constexpr std::size_t kSeqCount = static_cast<std::size_t>(Seq::Count);
static_assert(kSeqCount <= kMaxSeqs, "one record slot per sequence");

// How the next step waits: Settled = the contract is Ready, VBUS is in the band and has stayed there kSettledMs; Ready = 100 ms after
// the source's PS_RDY whatever VBUS is doing (and only at SinkTxOK, which the sink enforces); Spaced = kSpacedMs after the last answer.
enum class Pace : std::uint8_t { Settled, Ready, Spaced };
inline constexpr std::uint32_t kSettledMs = 1000;
inline constexpr std::uint32_t kReadyMs = 100;
inline constexpr std::uint32_t kSpacedMs = 500;

enum class StepKind : std::uint8_t { Request, GetCaps, SoftReset, EprEnter };

struct SeqInfo {
    const char* name;
    const char* what;       // one line for the table
    std::uint16_t cycles;   // the proposal's cycle count; meaning per sequence (see length())
    Pace pace;
    bool opt_in;            // not in the default mask
    bool pps;               // a PPS sequence: "needs load" ends it
};

inline constexpr std::array<SeqInfo, kSeqCount> kSeqs = {{
    {"ladder", "fixed levels up then down", 10, Pace::Settled, false, false},
    {"slam", "5 V <-> top level, settled then ready", 40, Pace::Settled, false, false},
    {"same", "same Request repeated at 9 V and top", 50, Pace::Ready, false, false},
    {"reannounce", "Get_Source_Cap every 500 ms", 20, Pace::Spaced, false, false},
    {"fixpps", "fixed <-> PPS at one voltage", 10, Pace::Settled, false, true},
    {"ppssteps", "PPS steps 20/100/500/2000 mV", 50, Pace::Ready, false, true},
    {"avssteps", "AVS steps 100/1000/2000 mV", 30, Pace::Ready, false, false},
    {"current", "half current <-> full, same voltage", 10, Pace::Settled, false, false},
    {"epr", "EPR 5/20 V <-> 28 V", 5, Pace::Settled, true, false},
    {"softreset", "repeated Soft_Reset", 10, Pace::Settled, false, false},
    {"ppslow", "PPS below 5 V", 10, Pace::Ready, false, true},
    {"mismatch", "mismatch bit set, no mismatch", 10, Pace::Settled, false, false},
}};

constexpr std::uint16_t seq_bit(Seq s) { return std::uint16_t(1u << static_cast<unsigned>(s)); }
inline constexpr std::uint16_t kAllSeqMask = (1u << kSeqCount) - 1;
// P1-P8 and the three probes; P9 (EPR) only when asked for.
inline constexpr std::uint16_t kDefaultSeqMask = kAllSeqMask & ~seq_bit(Seq::Epr);

inline constexpr std::array<std::uint16_t, 4> kPpsSizesMv = {20, 100, 500, 2000};
inline constexpr std::array<std::uint16_t, 3> kAvsSizesMv = {100, 1000, 2000};

// Where the planner may go, found once per Source_Capabilities list.
struct Levels {
    std::array<std::uint8_t, 7> fixed{};   // SPR fixed positions (<= cap), ascending
    std::uint8_t n_fixed = 0;
    std::uint8_t top = 0;                  // the highest fixed position
    std::uint8_t pps = 0, avs = 0;         // first PPS / SPR AVS position whose window starts at or below the cap
    std::uint8_t epr28 = 0;                // an EPR fixed 28 V position (pos >= 8)
    std::uint32_t cap_mv = 0;
};

inline Levels scan(const pd::SourceCaps& caps, std::uint32_t cap_mv) {
    Levels L;
    L.cap_mv = cap_mv;
    if (!caps.valid) return L;
    const std::size_t n = std::min<std::size_t>(caps.num_pdos, caps.pdos.size());
    for (std::size_t i = 0; i < n; ++i) {
        const pd::Pdo& d = caps.pdos[i];
        const std::uint8_t pos = std::uint8_t(i + 1);
        switch (d.type) {
            case pd::PdoType::Fixed:
                if (pos <= 7 && L.n_fixed < L.fixed.size() && d.max_mv <= cap_mv) {
                    L.fixed[L.n_fixed++] = pos;
                    if (L.top == 0 || d.max_mv >= caps.pdos[L.top - 1].max_mv) L.top = pos;
                } else if (pos >= 8 && d.max_mv == 28000 && d.max_mv <= cap_mv && !L.epr28) {
                    L.epr28 = pos;
                }
                break;
            case pd::PdoType::Pps:
                if (pos <= 7 && !L.pps && d.min_mv <= cap_mv) L.pps = pos;
                break;
            case pd::PdoType::Avs:
                if (pos <= 7 && !L.avs && d.min_mv <= cap_mv) L.avs = pos;
                break;
            default: break;
        }
    }
    return L;
}

// Why a sequence cannot run on this list (None = it can).
enum class Na : std::uint8_t { None, NoLevels, NoPps, NoAvs, PpsAbove5V, NoHeadroom, NoEpr, NoEprProof, SelfPowered, NotSelected };
constexpr const char* na_name(Na n) {
    switch (n) {
        case Na::None: return "ok";
        case Na::NoLevels: return "fewer than 2 fixed levels";
        case Na::NoPps: return "no PPS";
        case Na::NoAvs: return "no SPR AVS";
        case Na::PpsAbove5V: return "PPS starts at 5 V or more";
        case Na::NoHeadroom: return "under 1 A advertised";
        case Na::NoEpr: return "no EPR 28 V";
        case Na::NoEprProof: return "no EPR cable proof";
        case Na::SelfPowered: return "K2 would brown out";
        case Na::NotSelected: return "not selected";
    }
    return "?";
}

namespace detail {
constexpr std::uint32_t round_to(std::uint32_t v, std::uint32_t step) { return (v + step / 2) / step * step; }
struct Window { std::uint32_t lo = 0, hi = 0; };
inline Window window(const pd::SourceCaps& c, std::uint8_t pos, std::uint32_t cap_mv) {
    const pd::Pdo& d = c.pdos[pos - 1];
    return {d.min_mv, std::min<std::uint32_t>(d.max_mv, cap_mv)};
}
// Middle of an APDO window on the type's resolution, with room for the biggest step above it.
inline std::uint32_t mid_mv(const Window& w, std::uint32_t res) { return round_to((w.lo + w.hi) / 2, res); }
// How many of `sizes` fit above `base` inside the window.
template <std::size_t N>
inline std::size_t fit_sizes(const std::array<std::uint16_t, N>& sizes, const Window& w, std::uint32_t base) {
    std::size_t k = 0;
    for (auto s : sizes) if (base + s <= w.hi) ++k; else break;
    return k;
}
// Highest fixed level of the list that offers at least `min_ma`.
inline std::uint8_t level_with_current(const pd::SourceCaps& c, const Levels& L, std::uint16_t min_ma) {
    std::uint8_t best = 0;
    for (std::uint8_t i = 0; i < L.n_fixed; ++i)
        if (c.pdos[L.fixed[i] - 1].max_ma >= min_ma) best = L.fixed[i];
    return best;
}
// The 9 V level for P3: the first fixed level above 5 V, preferring exactly 9 V.
inline std::uint8_t nine_volt(const pd::SourceCaps& c, const Levels& L) {
    std::uint8_t first = 0;
    for (std::uint8_t i = 0; i < L.n_fixed; ++i) {
        const std::uint8_t p = L.fixed[i];
        if (c.pdos[p - 1].max_mv == 9000) return p;
        if (!first && c.pdos[p - 1].max_mv > 5000) first = p;
    }
    return first;
}
// A fixed level above 5 V that a PPS window also covers (P5): the lowest such.
inline std::uint8_t shared_level(const pd::SourceCaps& c, const Levels& L) {
    if (!L.pps) return 0;
    const Window w = window(c, L.pps, L.cap_mv);
    for (std::uint8_t i = 0; i < L.n_fixed; ++i) {
        const std::uint32_t mv = c.pdos[L.fixed[i] - 1].max_mv;
        if (mv > 5000 && mv >= w.lo && mv <= w.hi) return L.fixed[i];
    }
    return 0;
}
}  // namespace detail

inline std::uint16_t scaled_cycles(Seq s, std::uint16_t pct) {
    const std::uint32_t c = std::uint32_t(kSeqs[static_cast<std::size_t>(s)].cycles) * pct / 100;
    return std::uint16_t(c < 1 ? 1 : c);
}

// Why `s` cannot run here; Na::None when it can.
inline Na not_applicable(Seq s, const pd::SourceCaps& caps, const Levels& L, bool epr_proof, bool self_powered) {
    switch (s) {
        case Seq::Ladder:
        case Seq::Slam: return L.n_fixed >= 2 ? Na::None : Na::NoLevels;
        case Seq::SameReq: return detail::nine_volt(caps, L) ? Na::None : Na::NoLevels;
        case Seq::Reannounce:
        case Seq::SoftReset: return L.n_fixed >= 1 ? Na::None : Na::NoLevels;
        case Seq::FixedPps: return !L.pps ? Na::NoPps : detail::shared_level(caps, L) ? Na::None : Na::NoLevels;
        case Seq::PpsSteps: {
            if (!L.pps) return Na::NoPps;
            const auto w = detail::window(caps, L.pps, L.cap_mv);
            return detail::fit_sizes(kPpsSizesMv, w, detail::mid_mv(w, 20)) ? Na::None : Na::NoLevels;
        }
        case Seq::AvsSteps: {
            if (!L.avs) return Na::NoAvs;
            const auto w = detail::window(caps, L.avs, L.cap_mv);
            return detail::fit_sizes(kAvsSizesMv, w, detail::mid_mv(w, 100)) ? Na::None : Na::NoLevels;
        }
        case Seq::CurrentOnly: return detail::level_with_current(caps, L, 1000) ? Na::None : Na::NoHeadroom;
        case Seq::Epr:
            if (!L.epr28) return Na::NoEpr;
            return epr_proof ? Na::None : Na::NoEprProof;
        case Seq::PpsLow:
            if (!L.pps) return Na::NoPps;
            if (self_powered) return Na::SelfPowered;
            return caps.pdos[L.pps - 1].min_mv < 5000 ? Na::None : Na::PpsAbove5V;
        case Seq::Mismatch: return L.n_fixed >= 2 ? Na::None : Na::NoLevels;
        case Seq::Count: break;
    }
    return Na::NoLevels;
}

// Number of steps of `s` at `cycles` (0 = not applicable).
inline std::uint16_t length(Seq s, const pd::SourceCaps& caps, const Levels& L, std::uint16_t cycles, bool epr_proof, bool self_powered) {
    if (not_applicable(s, caps, L, epr_proof, self_powered) != Na::None) return 0;
    std::uint32_t n = 0;
    switch (s) {
        case Seq::Ladder: n = std::uint32_t(cycles) * 2 * (L.n_fixed - 1); break;
        case Seq::Slam: n = std::uint32_t(cycles) * 2; break;
        case Seq::SameReq: n = (detail::nine_volt(caps, L) == L.top ? 1u : 2u) * (1u + cycles); break;
        case Seq::Reannounce: n = cycles; break;
        case Seq::FixedPps: n = std::uint32_t(cycles) * 2; break;
        case Seq::PpsSteps: {
            const auto w = detail::window(caps, L.pps, L.cap_mv);
            n = 1u + std::uint32_t(detail::fit_sizes(kPpsSizesMv, w, detail::mid_mv(w, 20))) * cycles * 2;
            break;
        }
        case Seq::AvsSteps: {
            const auto w = detail::window(caps, L.avs, L.cap_mv);
            n = 1u + std::uint32_t(detail::fit_sizes(kAvsSizesMv, w, detail::mid_mv(w, 100))) * cycles;
            break;
        }
        case Seq::CurrentOnly: n = std::uint32_t(cycles) * 2; break;
        case Seq::Epr: n = 1u + std::uint32_t(cycles) * 4; break;
        case Seq::SoftReset: n = cycles; break;
        case Seq::PpsLow: n = std::uint32_t(cycles) * 2; break;
        case Seq::Mismatch: n = std::uint32_t(cycles) * 2; break;
        case Seq::Count: break;
    }
    return std::uint16_t(n > 0xFFFF ? 0xFFFF : n);
}

struct Step {
    StepKind kind = StepKind::Request;
    pd::Choice choice{};
    std::uint16_t target_mv = 0;   // what VBUS should reach (0 = none, a non-voltage step)
    Pace pace = Pace::Settled;
};

namespace detail {
inline Step fixed_step(const pd::SourceCaps& c, std::uint8_t pos, Pace p, std::uint16_t ma = 0, bool mismatch = false) {
    Step s;
    s.choice.pos = pos;
    s.choice.ma = ma;
    s.choice.mismatch = mismatch;
    s.target_mv = c.pdos[pos - 1].max_mv;
    s.pace = p;
    return s;
}
inline Step prog_step(const pd::SourceCaps& c, std::uint8_t pos, std::uint32_t mv, Pace p) {
    Step s;
    s.choice.pos = pos;
    s.choice.mv = std::uint16_t(mv);
    s.target_mv = std::uint16_t(mv);
    s.pace = p;
    (void)c;
    return s;
}
}  // namespace detail

// Step `idx` (0-based) of sequence `s`; nullopt past the end or when the sequence does not apply.
inline std::optional<Step> step(Seq s, std::uint16_t idx, const pd::SourceCaps& caps, const Levels& L, std::uint16_t cycles, bool epr_proof,
                                bool self_powered) {
    const std::uint16_t len = length(s, caps, L, cycles, epr_proof, self_powered);
    if (idx >= len) return std::nullopt;
    const Pace base = kSeqs[static_cast<std::size_t>(s)].pace;
    switch (s) {
        case Seq::Ladder: {   // up through every fixed level then back down, `cycles` times
            const std::uint32_t half = L.n_fixed - 1u, p = idx % (2 * half);
            const std::uint32_t k = p < half ? p + 1 : 2 * half - p - 1;
            return detail::fixed_step(caps, L.fixed[k], base);
        }
        case Seq::Slam: {     // the first half of the cycles settled, the second half ready
            const Pace p = idx < (cycles / 2) * 2u ? Pace::Settled : Pace::Ready;
            return detail::fixed_step(caps, (idx & 1) ? L.fixed[0] : L.top, p);
        }
        case Seq::SameReq: {  // block 1: 9 V, block 2: the top level; the first of a block is the move (settled), the rest repeat it
            const std::uint8_t nine = detail::nine_volt(caps, L);
            const std::uint32_t block = cycles + 1u;
            const std::uint8_t pos = idx / block == 0 ? nine : L.top;
            return detail::fixed_step(caps, pos, idx % block == 0 ? Pace::Settled : Pace::Ready);
        }
        case Seq::Reannounce: {
            Step st;
            st.kind = StepKind::GetCaps;
            st.pace = Pace::Spaced;
            return st;
        }
        case Seq::FixedPps: {
            const std::uint8_t f = detail::shared_level(caps, L);
            if ((idx & 1) == 0) return detail::fixed_step(caps, f, base);
            return detail::prog_step(caps, L.pps, caps.pdos[f - 1].max_mv, base);
        }
        case Seq::PpsSteps: {
            const auto w = detail::window(caps, L.pps, L.cap_mv);
            const std::uint32_t v0 = detail::mid_mv(w, 20);
            if (idx == 0) return detail::prog_step(caps, L.pps, v0, Pace::Settled);
            const std::uint32_t j = idx - 1, per = cycles * 2u;
            const std::uint32_t d = kPpsSizesMv[j / per];
            return detail::prog_step(caps, L.pps, (j & 1) == 0 ? v0 + d : v0, base);   // up first, then down
        }
        case Seq::AvsSteps: {
            const auto w = detail::window(caps, L.avs, L.cap_mv);
            const std::uint32_t v0 = detail::mid_mv(w, 100);
            if (idx == 0) return detail::prog_step(caps, L.avs, v0, Pace::Settled);
            const std::uint32_t j = idx - 1;
            const std::uint32_t d = kAvsSizesMv[j / cycles];
            return detail::prog_step(caps, L.avs, (j & 1) == 0 ? v0 + d : v0, base);
        }
        case Seq::CurrentOnly: {
            const std::uint8_t pos = detail::level_with_current(caps, L, 1000);
            const std::uint16_t full = caps.pdos[pos - 1].max_ma;
            const std::uint16_t half = std::uint16_t(std::max<unsigned>(full / 2 / 10 * 10, 100));
            return detail::fixed_step(caps, pos, base, (idx & 1) == 0 ? half : full);
        }
        case Seq::Epr: {
            if (idx == 0) {
                Step st;
                st.kind = StepKind::EprEnter;
                st.pace = Pace::Settled;
                return st;
            }
            const std::uint32_t j = (idx - 1) % 4;
            std::uint8_t pos = j == 0 ? L.fixed[0] : j == 2 ? L.top : L.epr28;   // 5 V, 28 V, 20 V (the SPR top), 28 V
            // "20 V" is the highest SPR fixed level at or below 20 V.
            if (j == 2) {
                pos = L.fixed[0];
                for (std::uint8_t i = 0; i < L.n_fixed; ++i)
                    if (caps.pdos[L.fixed[i] - 1].max_mv <= 20000) pos = L.fixed[i];
            }
            return detail::fixed_step(caps, pos, base);
        }
        case Seq::SoftReset: {
            Step st;
            st.kind = StepKind::SoftReset;
            st.pace = base;
            return st;
        }
        case Seq::PpsLow: {
            const std::uint32_t lo = detail::round_to(caps.pdos[L.pps - 1].min_mv, 20);
            return detail::prog_step(caps, L.pps, (idx & 1) == 0 ? lo : 5000, base);
        }
        case Seq::Mismatch: {   // the top level with the bit set (full advertised current), then 5 V plain
            if ((idx & 1) == 0) return detail::fixed_step(caps, L.top, base, 0, true);
            return detail::fixed_step(caps, L.fixed[0], base);
        }
        case Seq::Count: break;
    }
    return std::nullopt;
}

}  // namespace k2::pdstress
