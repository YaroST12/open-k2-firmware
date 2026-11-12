// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home 2c pure model: which quantity is the hero, the contract power behind the W alarm, the peak / sag tracker of the
// right-column cards and their texts, and the card cycler that decides what the two right cells show. No hardware
// access, no drawing: tests/test_home_cards.cpp covers all of it.
#pragma once
#include <cstdint>
#include <span>
#include "k2/ui/home_model.hpp"

namespace k2::home {

// ---- hero --------------------------------------------------------------------------------------
// The quantity in the hero cell; the other two sit in the ledger rows. Long Back walks W -> V -> A -> W.
enum class Qty : uint8_t { W, V, A };
constexpr Qty next(Qty q) { return q == Qty::W ? Qty::V : q == Qty::V ? Qty::A : Qty::W; }
// The stored setting: 1 = V, 2 = A, anything else (0, a record from older firmware) = W.
constexpr Qty qty_from(uint8_t s) { return s == 1 ? Qty::V : s == 2 ? Qty::A : Qty::W; }
constexpr uint8_t qty_byte(Qty q) { return static_cast<uint8_t>(q); }
struct Ledger { Qty row[2]; };
constexpr Ledger ledger_of(Qty hero) {
    return hero == Qty::W ? Ledger{{Qty::V, Qty::A}} : hero == Qty::V ? Ledger{{Qty::W, Qty::A}} : Ledger{{Qty::W, Qty::V}};
}

// ---- contract power ----------------------------------------------------------------------------
// The power the source promises for the contract in force, in mW (0 = no rating, so no W bar or alarm): PD fixed and
// EPR V x I, PPS the APDO's maximum V x I, AVS its PDP, a Type-C source 5 V x its Rp current. QC, FCP, SCP, SVOOC,
// UFCS and BC1.2 announce no current, so no power either.
uint32_t contract_mw(const ProtoInfo &p);
// The level a VBUS dip is measured against (0 = none known, no sag): the PD contract's volts (the requested level of
// PPS / AVS), the QC2 level decoded from the data lines, 5 V for a Type-C or BC1.2 source, and for QC3 only the level
// the K2's own trigger holds (`qc3_target_mv`). A level rounded from VBUS itself (FCP, SCP, VOOC) is no level.
uint32_t sag_level_mv(const ProtoInfo &p, uint32_t qc3_target_mv);

// ---- peaks and sag -----------------------------------------------------------------------------
// Highest |W| and |A| and the worst VBUS dip below the level in force, since attach or the last long OK. Only samples
// that carry current (|I| > 50 mA) count toward the sag: an idle source sits at its level and says nothing about it.
class Extremes {
public:
    static constexpr uint32_t kSagMinMa = 50;
    void reset() { *this = Extremes{}; }
    void sample(int32_t vbus_mv, int32_t ibus_ma, int32_t power_mw, uint32_t level_mv);
    uint32_t peak_mw() const { return peak_mw_; }
    uint32_t peak_ma() const { return peak_ma_; }
    bool has_sag() const { return has_sag_; }
    uint32_t sag_bp() const { return sag_bp_; }             // 0.01 % of the level
    uint32_t sag_level_mv() const { return sag_level_mv_; }  // the level the worst dip was measured against
private:
    uint32_t peak_mw_ = 0, peak_ma_ = 0;
    uint32_t sag_bp_ = 0, sag_level_mv_ = 0;
    bool has_sag_ = false;
};

// The level fed to Extremes::sample: sag_level_mv's value once it has held kSettleMs, else 0 (no sag counted). A new
// contract's level arrives before VBUS has moved to it, and a PPS / AVS request step moves it for a moment.
class SettledLevel {
public:
    static constexpr uint32_t kSettleMs = 1000;
    void reset() { *this = SettledLevel{}; }
    uint32_t update(uint32_t now_ms, uint32_t level_mv) {
        if (level_mv != level_mv_) { level_mv_ = level_mv; since_ms_ = now_ms; }
        return level_mv_ != 0 && now_ms - since_ms_ >= kSettleMs ? level_mv_ : 0;
    }
private:
    uint32_t level_mv_ = 0, since_ms_ = 0;
};

// The SAG card's value: "1.0% 20V", "12% 9.2V", "9.5% 11V" (the level drops its decimal when the text would pass
// kSagChars). Returns the length.
inline constexpr int kSagChars = 9;
int fmt_sag(std::span<char> out, uint32_t sag_bp, uint32_t level_mv);
// The DROP card's value: "0.123V" (capped at 9.999V).
int fmt_drop(std::span<char> out, uint32_t drop_mv);
// The DROP card's state, the passive two-point method of cable_r::Meter on the shown reading: the Meter's light-load
// reference (has_idle, idle_mv, idle_ma), then a load at least cable_r::kMinStepMa above it gives drop = V_idle - V_now
// (a rise reads 0). None when not live, NoIdle before a reference ("no idle"), NeedLoad below the step ("need 1A").
enum class DropState : uint8_t { None, NoIdle, NeedLoad, Value };
struct Drop { DropState state = DropState::None; uint32_t mv = 0; };
Drop drop_of(bool live, bool has_idle, int32_t idle_mv, int32_t idle_ma, int32_t vbus_mv, int32_t ibus_ma);

// ---- card cycler -------------------------------------------------------------------------------
// The two right-column cells. The deck is PEAK, DROP (+ SAG), CABLE, LINES; ENERGY (Wh / Ah) is the lower cell's
// every other turn and never shows above. The upper cell steps every kDwellMs, the lower one half a dwell later, so
// only one cell changes at a time; a cell never shows the other's card; a card without data is skipped (and leaves at
// once when it loses its data). The clock runs only while live and counts at most kMaxStepMs per tick, so a stalled
// loop moves one cell at most. Not live: PEAK above Wh / Ah, frozen.
enum class Card : uint8_t { Energy, Peak, Drop, Cable, Lines, Count };
using CardMask = uint8_t;
constexpr CardMask card_bit(Card c) { return static_cast<CardMask>(1u << static_cast<unsigned>(c)); }

class CardCycler {
public:
    static constexpr uint32_t kDwellMs = 5000, kMaxStepMs = 1000;
    void reset();
    void tick(uint32_t now_ms, bool live, CardMask ok);
    // The attention seam: put `c` in the upper cell now (live only, never ENERGY, only a card with data). It holds until
    // the upper cell's next dwell; forced() is true until then, so the painter can draw the attention dot.
    void force_top(Card c, CardMask ok);
    Card top() const { return live_ ? top_ : Card::Peak; }
    Card bottom() const { return !live_ || energy_ ? Card::Energy : bot_; }
    bool forced() const { return live_ && forced_; }
private:
    void settle(CardMask ok);
    Card top_ = Card::Peak, bot_ = Card::Drop;
    bool energy_ = true, live_ = false, primed_ = false, forced_ = false;
    uint32_t act_ms_ = 0, last_ms_ = 0;
};

}  // namespace k2::home
