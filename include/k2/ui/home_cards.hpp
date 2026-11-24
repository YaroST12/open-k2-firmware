// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home 2c pure model: which quantity is the hero, the contract power behind the W alarm, the peak / sag tracker of the
// right-column cards and their texts, and the card cycler that decides what the two right cells show. No hardware
// access, no drawing: tests/test_home_cards.cpp covers all of it.
#pragma once
#include <cstdint>
#include <span>
#include "k2/core/bss_zero.hpp"
#include "k2/ui/home_model.hpp"

namespace k2::home {

// ---- hero --------------------------------------------------------------------------------------
// The quantity in the hero cell; the other two sit in the ledger rows. Entered Home: + walks W -> V -> A -> W, - back.
enum class Qty : uint8_t { W, V, A };
constexpr Qty next(Qty q) { return q == Qty::W ? Qty::V : q == Qty::V ? Qty::A : Qty::W; }
constexpr Qty prev(Qty q) { return q == Qty::W ? Qty::A : q == Qty::V ? Qty::W : Qty::V; }
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
    uint32_t peak_mv() const { return peak_mv_; }
    uint32_t peak_ma() const { return peak_ma_; }
    bool has_sag() const { return has_sag_; }
    uint32_t sag_bp() const { return sag_bp_; }             // 0.01 % of the level
    uint32_t sag_level_mv() const { return sag_level_mv_; }  // the level the worst dip was measured against
private:
    // The one instance is UiContext::extremes in the .bss global Ui (reset() value-initialises, which zeroes).
    uint32_t peak_mw_ K2_BSS_ZERO, peak_mv_ K2_BSS_ZERO, peak_ma_ K2_BSS_ZERO;
    uint32_t sag_bp_ K2_BSS_ZERO, sag_level_mv_ K2_BSS_ZERO;
    bool has_sag_ K2_BSS_ZERO;
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
    uint32_t level_mv_ K2_BSS_ZERO, since_ms_ K2_BSS_ZERO;   // the one instance is Ui::sag_settle_ in the .bss global Ui
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
enum class Card : uint8_t { Energy, Peak, Drop, Cable, Lines, Battery, Count };
using CardMask = uint8_t;
constexpr CardMask card_bit(Card c) { return static_cast<CardMask>(1u << static_cast<unsigned>(c)); }

class CardCycler {
public:
    static constexpr uint32_t kDwellMs = 5000, kMaxStepMs = 1000;
    void reset();
    void tick(uint32_t now_ms, bool live, CardMask ok);
    // The attention seam (Attention below): put `c` in the upper cell now (live only, never ENERGY, only a card with
    // data). It holds, the upper cell's dwell steps paused, until force_top(Card::Count) releases it; forced() is true
    // until then, so the painter draws the attention dot. On release the card stays until the next dwell.
    void force_top(Card c, CardMask ok);
    Card top() const { return live_ ? top_ : Card::Peak; }
    Card bottom() const { return !live_ || energy_ ? Card::Energy : bot_; }
    bool forced() const { return live_ && forced_; }
private:
    void settle(CardMask ok);
    Card top_ = Card::Peak, bot_ = Card::Drop;
    // The one instance is UiContext::Remembered::cards in the .bss global Ui. The non-zero defaults above stay; the
    // zero ones are elided on ARM (reset() value-initialises: zero first, then the non-zero defaults).
    bool energy_ = true, live_ K2_BSS_ZERO, primed_ K2_BSS_ZERO, forced_ K2_BSS_ZERO;
    uint32_t act_ms_ K2_BSS_ZERO, last_ms_ K2_BSS_ZERO;
};

// ---- attention ---------------------------------------------------------------------------------
// What is changing right now, for the upper card cell (CardCycler::force_top, with the dot). Every kStepMs each input is
// compared with its slow average (an EMA, 3 % per step), then folded into it:
//   VBUS more than max(250 mV, 2.5 % of its average) off   -> DROP  (VBUS dropped or jumped; the card holds DROP + SAG)
//   CC1 or CC2 more than 150 mV off its average             -> LINES
//   a peak power 10 % (and at least 0.5 W) above the last announced one -> PEAK
// A trigger holds its card kHoldMs, re-armed while the signal keeps moving. A stronger card (DROP 3 > LINES 2 > PEAK 1)
// takes over a weaker one, never the reverse. The first kQuietMs of a session, or after a gap longer than kGapMs (Home
// was off screen), only learn the averages. Not live: nothing, and the next session starts over.
class Attention {
public:
    static constexpr uint32_t kStepMs = 100, kHoldMs = 5000, kQuietMs = 2000, kGapMs = 1000;
    static constexpr int32_t kVbusMinMv = 250, kVbusPermille = 25, kCcMv = 150;
    static constexpr uint32_t kPeakMinMw = 500;
    void reset() { *this = Attention{}; }
    // `cc1_mv` / `cc2_mv` < 0: no reading (that line is not watched). Returns want().
    Card update(uint32_t now_ms, bool live, int32_t vbus_mv, int32_t cc1_mv, int32_t cc2_mv, uint32_t peak_mw);
    Card want() const { return card_; }   // the card that wants the upper cell, Card::Count for none
private:
    void prime(uint32_t now_ms, int32_t vbus_mv, int32_t cc1_mv, int32_t cc2_mv, uint32_t peak_mw);
    void trigger(uint32_t now_ms, Card c);
    // The one instance is UiContext::Remembered::attention in the .bss global Ui; card_ = Count (5) is non-zero, kept.
    int32_t avg_v_ K2_BSS_ZERO, avg_cc_[2] K2_BSS_ZERO;   // x256
    bool cc_ok_[2] K2_BSS_ZERO;
    uint32_t peak_ref_mw_ K2_BSS_ZERO, start_ms_ K2_BSS_ZERO, last_ms_ K2_BSS_ZERO, until_ms_ K2_BSS_ZERO;
    bool primed_ K2_BSS_ZERO;
    Card card_ = Card::Count;
};

// ---- configurable Home boxes (3 x 74 px row) ---------------------------------------------------
// Each of the three boxes is configured in Settings > Home boxes to either a fixed metric, Off, or
// Rotating (cycles through unpinned metrics that have data and prioritizes whichever is active).
enum class BoxKind : uint8_t {
    Rotating = 0,
    Pd,
    Energy,
    Drop,
    Cable,
    Cc,
    Dpdm,
    Battery,
    LoadR,
    Off,
    Count
};
inline constexpr int kBoxCount = 3;
inline constexpr std::array<BoxKind, kBoxCount> kDefaultBoxes = {BoxKind::Pd, BoxKind::Energy, BoxKind::Rotating};
using BoxMask = uint16_t;
constexpr BoxMask box_bit(BoxKind b) { return static_cast<BoxMask>(1u << static_cast<unsigned>(b)); }
constexpr BoxKind box_from(uint8_t v) {
    return v < static_cast<uint8_t>(BoxKind::Count) ? static_cast<BoxKind>(v) : BoxKind::Rotating;
}
constexpr BoxKind next_box(BoxKind b) {
    return static_cast<BoxKind>((static_cast<uint8_t>(b) + 1u) % static_cast<uint8_t>(BoxKind::Count));
}
const char *box_short_name(BoxKind b);   // Settings tail: "Rotating", "PD Req", "Energy", ...
const char *box_note(BoxKind b);         // Settings note: "Cycles unpinned metrics", ...

// Cycler & 3-second fading cyan attention highlight for the three Home boxes.
class BoxCycler {
public:
    static constexpr uint32_t kDwellMs = 5000, kMaxStepMs = 1000;
    static constexpr uint32_t kGlowMs = 3000;
    static constexpr uint8_t kGlowSteps = 6;   // 6 discrete 500 ms steps (low SPI traffic)

    void reset();
    void tick(uint32_t now_ms, bool live, const std::array<BoxKind, kBoxCount> &cfg, BoxMask ok);
    // Highlight `metric` (from Attention): if pinned in box i, glows box i; else pulls it into the
    // first Rotating box (if it has data) and glows it for kGlowMs.
    void trigger_attention(uint32_t now_ms, BoxKind metric, const std::array<BoxKind, kBoxCount> &cfg, BoxMask ok);
    BoxKind shown(int i) const { return shown_[i]; }
    bool is_rotating(int i) const { return rotating_[i]; }
    // 0 = no border, 1..kGlowSteps = fading cyan border intensity (kGlowSteps = brightest).
    uint8_t glow_level(int i, uint32_t now_ms) const;

private:
    BoxKind shown_[kBoxCount] = {BoxKind::Pd, BoxKind::Energy, BoxKind::Drop};
    bool rotating_[kBoxCount] = {false, false, true};
    bool live_ K2_BSS_ZERO, primed_ K2_BSS_ZERO;
    uint32_t act_ms_ K2_BSS_ZERO, last_ms_ K2_BSS_ZERO;
    uint32_t glow_start_[kBoxCount] K2_BSS_ZERO;
    bool glow_on_[kBoxCount] K2_BSS_ZERO;
    int8_t attn_slot_ = -1;
    BoxKind attn_metric_ = BoxKind::Off;
    uint32_t attn_until_ K2_BSS_ZERO;
};

}  // namespace k2::home
