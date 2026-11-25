// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/home_cards.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/core/strpack.hpp"

namespace k2::home {
namespace {

uint32_t mag(int32_t v) { return v < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(v)) : static_cast<uint32_t>(v); }

// Appends `v` in decimal; returns the new length (stops at the buffer's end, leaving room for the terminator).
int put_uint(std::span<char> out, int n, uint32_t v) {
    char tmp[10];
    int k = 0;
    do { tmp[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 10);
    while (k && n + 1 < static_cast<int>(out.size())) out[n++] = tmp[--k];
    return n;
}
int put_char(std::span<char> out, int n, char c) {
    if (n + 1 < static_cast<int>(out.size())) out[n++] = c;
    return n;
}
int finish(std::span<char> out, int n) {
    if (!out.empty()) out[n] = '\0';
    return n;
}

// "20V" or "9.2V" (one decimal unless whole), or the whole volts only ("11V").
int put_level(std::span<char> out, int n, uint32_t mv, bool whole_only) {
    if (whole_only) return put_char(out, put_uint(out, n, (mv + 500) / 1000), 'V');
    const uint32_t tenths = (mv + 50) / 100;
    n = put_uint(out, n, tenths / 10);
    if (tenths % 10) n = put_uint(out, put_char(out, n, '.'), tenths % 10);
    return put_char(out, n, 'V');
}

constexpr Card kDeck[] = {Card::Peak, Card::Drop, Card::Cable, Card::Lines, Card::Battery};
constexpr int kDeckN = static_cast<int>(sizeof(kDeck) / sizeof(kDeck[0]));

int deck_index(Card c) {
    for (int i = 0; i < kDeckN; i++)
        if (kDeck[i] == c) return i;
    return 0;
}
bool has(CardMask ok, Card c) { return (ok & card_bit(c)) != 0; }

// The deck card after `cur` that has data and is not `avoid`; Card::Count when there is none.
Card next_card(Card cur, Card avoid, CardMask ok) {
    const int at = deck_index(cur);
    for (int i = 1; i <= kDeckN; i++) {
        const Card c = kDeck[(at + i) % kDeckN];
        if (c != avoid && has(ok, c)) return c;
    }
    return Card::Count;
}

// The index of the last step at or before `t` of a step train starting at `off` and repeating every dwell (0 = none yet).
uint32_t step_index(uint32_t t, uint32_t off) {
    return t < off ? 0 : (t - off) / CardCycler::kDwellMs + 1;
}

}  // namespace

uint32_t contract_mw(const ProtoInfo &p) {
    if (p.measured) return 0;
    switch (p.kind) {
    case ProtoKind::PdFixed: case ProtoKind::PdEpr:
        return math::mw_of(p.mv, p.ma);
    case ProtoKind::Pps:
        return math::mw_of(p.hi_mv, p.ma);
    case ProtoKind::Avs:
        return p.mw;
    case ProtoKind::TypeC:
        return 5u * p.ma;
    default:
        return 0;
    }
}

uint32_t sag_level_mv(const ProtoInfo &p, uint32_t qc3_target_mv) {
    if (p.measured) return 0;
    switch (p.kind) {
    case ProtoKind::PdFixed: case ProtoKind::PdEpr: case ProtoKind::Pps: case ProtoKind::Avs: case ProtoKind::Qc2:
        return p.mv;
    case ProtoKind::TypeC: case ProtoKind::Dcp:
        return 5000;
    case ProtoKind::Qc3:
        return qc3_target_mv;
    default:
        return 0;
    }
}

void Extremes::sample(int32_t vbus_mv, int32_t ibus_ma, int32_t power_mw, uint32_t level_mv) {
    const uint32_t v = vbus_mv < 0 ? 0 : static_cast<uint32_t>(vbus_mv);
    const uint32_t a = mag(ibus_ma), w = mag(power_mw);
    if (w > peak_mw_) peak_mw_ = w;
    if (v > peak_mv_) peak_mv_ = v;
    if (a > peak_ma_) peak_ma_ = a;
    if (a <= kSagMinMa || level_mv == 0) return;
    const uint32_t dip = v < level_mv ? level_mv - v : 0;
    const uint32_t bp = math::div_u64_to_u32(static_cast<uint64_t>(dip) * 10000u + level_mv / 2u, level_mv);
    if (!has_sag_ || bp > sag_bp_) {
        has_sag_ = true;
        sag_bp_ = bp;
        sag_level_mv_ = level_mv;
    }
}

int fmt_sag(std::span<char> out, uint32_t sag_bp, uint32_t level_mv) {
    if (out.empty()) return 0;
    int n = 0;
    const uint32_t tenths = (sag_bp + 5) / 10;
    if (tenths < 100) {
        n = put_uint(out, n, tenths / 10);
        n = put_uint(out, put_char(out, n, '.'), tenths % 10);
    } else {
        n = put_uint(out, n, (sag_bp + 50) / 100);
    }
    n = put_char(out, put_char(out, n, '%'), ' ');
    const int head = n;
    n = put_level(out, head, level_mv, false);
    if (n > kSagChars) n = put_level(out, head, level_mv, true);
    return finish(out, n);
}

int fmt_drop(std::span<char> out, uint32_t drop_mv) {
    if (out.empty()) return 0;
    const uint32_t v = drop_mv > 9999 ? 9999 : drop_mv;
    int n = put_uint(out, 0, v / 1000);
    n = put_char(out, n, '.');
    n = put_uint(out, n, v / 100 % 10);
    n = put_uint(out, n, v / 10 % 10);
    n = put_uint(out, n, v % 10);
    return finish(out, put_char(out, n, 'V'));
}

Drop drop_of(bool live, bool has_idle, int32_t idle_mv, int32_t idle_ma, int32_t vbus_mv, int32_t ibus_ma) {
    if (!live) return {};
    if (!has_idle) return {DropState::NoIdle, 0};
    const int64_t di = static_cast<int64_t>(mag(ibus_ma)) - idle_ma;
    if (di < 1000) return {DropState::NeedLoad, 0};   // cable_r::kMinStepMa
    return {DropState::Value, idle_mv > vbus_mv ? static_cast<uint32_t>(idle_mv - vbus_mv) : 0u};
}

void CardCycler::reset() { *this = CardCycler{}; }

void CardCycler::settle(CardMask ok) {
    if (!has(ok, top_)) {
        const Card c = next_card(top_, energy_ ? Card::Count : bot_, ok);
        if (c != Card::Count) top_ = c;
    }
    if (!energy_ && (!has(ok, bot_) || bot_ == top_)) {
        const Card c = next_card(bot_, top_, ok);
        if (c == Card::Count) energy_ = true;   // nothing else to show: Wh / Ah keep the cell
        else bot_ = c;
    }
}

void CardCycler::tick(uint32_t now_ms, bool live, CardMask ok) {
    if (!live) {
        live_ = primed_ = forced_ = false;
        return;
    }
    uint32_t dt = primed_ ? now_ms - last_ms_ : 0;
    if (dt > kMaxStepMs) dt = kMaxStepMs;
    last_ms_ = now_ms;
    primed_ = live_ = true;
    const uint32_t old = act_ms_;
    act_ms_ += dt;
    if (step_index(act_ms_, kDwellMs / 2) != step_index(old, kDwellMs / 2)) {
        energy_ = !energy_;
        if (!energy_) {
            const Card c = next_card(bot_, top_, ok);
            if (c == Card::Count) energy_ = true;
            else bot_ = c;
        }
    }
    if (step_index(act_ms_, kDwellMs) != step_index(old, kDwellMs) && !forced_) {   // an attention card holds the cell
        const Card c = next_card(top_, energy_ ? Card::Count : bot_, ok);
        if (c != Card::Count) top_ = c;
    }
    settle(ok);
}

void CardCycler::force_top(Card c, CardMask ok) {
    if (c == Card::Count) {   // released: the card stays until the next dwell, without the dot
        forced_ = false;
        return;
    }
    if (!live_ || c == Card::Energy || !has(ok, c)) return;
    top_ = c;
    forced_ = true;
    settle(ok);
}

namespace {
constexpr int prio(Card c) { return c == Card::Drop ? 3 : c == Card::Lines ? 2 : c == Card::Peak ? 1 : 0; }
constexpr int32_t off(int32_t x, int32_t avg256) {
    const int32_t d = x - (avg256 >> 8);
    return d < 0 ? -d : d;
}
// avg += (x - avg) * 3 %, in 1/256 mV
constexpr int32_t fold(int32_t avg256, int32_t x) { return avg256 + math::div_i64_to_i32((int64_t{x} * 256 - avg256) * 3, 100); }
}  // namespace

void Attention::prime(uint32_t now_ms, int32_t vbus_mv, int32_t cc1_mv, int32_t cc2_mv, uint32_t peak_mw) {
    avg_v_ = vbus_mv * 256;
    const int32_t cc[2] = {cc1_mv, cc2_mv};
    for (int i = 0; i < 2; i++) {
        cc_ok_[i] = cc[i] >= 0;
        avg_cc_[i] = cc_ok_[i] ? cc[i] * 256 : 0;
    }
    peak_ref_mw_ = peak_mw;
    start_ms_ = last_ms_ = now_ms;
    primed_ = true;
    card_ = Card::Count;
}

void Attention::trigger(uint32_t now_ms, Card c) {
    if (card_ != Card::Count && prio(c) < prio(card_)) return;   // a weaker card never takes over
    card_ = c;
    until_ms_ = now_ms + kHoldMs;
}

Card Attention::update(uint32_t now_ms, bool live, int32_t vbus_mv, int32_t cc1_mv, int32_t cc2_mv, uint32_t peak_mw) {
    if (!live) {
        reset();
        return card_;
    }
    if (vbus_mv < 0) vbus_mv = -vbus_mv;
    if (!primed_ || now_ms - last_ms_ > kGapMs) {
        prime(now_ms, vbus_mv, cc1_mv, cc2_mv, peak_mw);
        return card_;
    }
    if (now_ms - last_ms_ < kStepMs) return card_;
    last_ms_ = now_ms;
    const bool quiet = now_ms - start_ms_ < kQuietMs;

    const int32_t level = avg_v_ >> 8;
    const int32_t thr = level * kVbusPermille / 1000 > kVbusMinMv ? level * kVbusPermille / 1000 : kVbusMinMv;
    if (!quiet && off(vbus_mv, avg_v_) > thr) trigger(now_ms, Card::Drop);
    avg_v_ = fold(avg_v_, vbus_mv);

    const int32_t cc[2] = {cc1_mv, cc2_mv};
    for (int i = 0; i < 2; i++) {
        if (cc[i] < 0) continue;
        if (!cc_ok_[i]) {   // a line that just got a reading: learn it first
            cc_ok_[i] = true;
            avg_cc_[i] = cc[i] * 256;
            continue;
        }
        if (!quiet && off(cc[i], avg_cc_[i]) > kCcMv) trigger(now_ms, Card::Lines);
        avg_cc_[i] = fold(avg_cc_[i], cc[i]);
    }

    if (quiet) {
        if (peak_mw > peak_ref_mw_) peak_ref_mw_ = peak_mw;
    } else if (peak_mw >= peak_ref_mw_ + (peak_ref_mw_ / 10 > kPeakMinMw ? peak_ref_mw_ / 10 : kPeakMinMw)) {
        peak_ref_mw_ = peak_mw;
        trigger(now_ms, Card::Peak);
    } else if (peak_mw < peak_ref_mw_) {
        peak_ref_mw_ = peak_mw;   // the peaks were reset (long OK): announce from the new start
    }

    if (card_ != Card::Count && static_cast<int32_t>(now_ms - until_ms_) >= 0) card_ = Card::Count;
    return card_;
}

// ---- BoxCycler ---------------------------------------------------------------------------------
const char *box_short_name(BoxKind b) {
    static constexpr const char *kNames[static_cast<unsigned>(BoxKind::Count)] = {
        K2_P("Rotating"), K2_P("PD Req"), K2_P("Energy"), "Drop", K2_P("E-Marker"), K2_P("CC1/2"), K2_P("D+/D-"), K2_P("Battery"), K2_P("Load R"), "Off"
    };
    const unsigned i = static_cast<unsigned>(b);
    return kNames[i < static_cast<unsigned>(BoxKind::Count) ? i : 0];
}

const char *box_note(BoxKind b) {
    static constexpr const char *kNotes[static_cast<unsigned>(BoxKind::Count)] = {
        K2_P("Cycles unpinned + active"), K2_P("PD charger max & sink req"), K2_P("Session Wh and Ah"), K2_P("Cable drop & VBUS sag %"),
        K2_P("E-Marker rating & speed"), K2_P("CC1 and CC2 voltages"), K2_P("D+ and D- voltages"), K2_P("PD battery state of charge"),
        K2_P("Apparent load resistance"), K2_P("Leave box blank")
    };
    const unsigned i = static_cast<unsigned>(b);
    return kNotes[i < static_cast<unsigned>(BoxKind::Count) ? i : 0];
}

namespace {
constexpr BoxKind kBoxDeck[] = {BoxKind::Pd, BoxKind::Energy, BoxKind::Drop, BoxKind::Cable,
                                BoxKind::Cc, BoxKind::Dpdm, BoxKind::Battery, BoxKind::LoadR};
constexpr int kBoxDeckN = static_cast<int>(sizeof(kBoxDeck) / sizeof(kBoxDeck[0]));

int box_deck_idx(BoxKind b) {
    for (int i = 0; i < kBoxDeckN; i++)
        if (kBoxDeck[i] == b) return i;
    return 0;
}

BoxKind next_unpinned(BoxKind cur, BoxMask used, BoxMask ok) {
    const int at = box_deck_idx(cur);
    for (int i = 1; i <= kBoxDeckN; i++) {
        const BoxKind c = kBoxDeck[(at + i) % kBoxDeckN];
        const BoxMask bit = box_bit(c);
        if ((used & bit) == 0 && (ok & bit) != 0) return c;
    }
    return BoxKind::Off;
}
}  // namespace

void BoxCycler::reset() { *this = BoxCycler{}; }

void BoxCycler::tick(uint32_t now_ms, bool live, const std::array<BoxKind, kBoxCount> &cfg, BoxMask ok) {
    BoxMask pinned = 0;
    for (int i = 0; i < kBoxCount; i++) {
        rotating_[i] = cfg[i] == BoxKind::Rotating;
        if (!rotating_[i]) {
            shown_[i] = cfg[i];
            if (cfg[i] != BoxKind::Off) pinned |= box_bit(cfg[i]);
        }
    }
    if (!live) {
        live_ = primed_ = false;
        attn_slot_ = -1;
        for (bool &g : glow_on_) g = false;
        // When idle/stale, give each Rotating slot a deterministic unpinned default from the deck.
        BoxMask used = pinned;
        for (int i = 0; i < kBoxCount; i++) {
            if (!rotating_[i]) continue;
            const BoxKind c = next_unpinned(BoxKind::LoadR, used, static_cast<BoxMask>(~0u));
            shown_[i] = c;
            if (c != BoxKind::Off) used |= box_bit(c);
        }
        return;
    }
    uint32_t dt = primed_ ? now_ms - last_ms_ : 0;
    if (dt > kMaxStepMs) dt = kMaxStepMs;
    last_ms_ = now_ms;
    primed_ = live_ = true;
    const uint32_t old = act_ms_;
    act_ms_ += dt;

    if (attn_slot_ >= 0 && static_cast<int32_t>(now_ms - attn_until_) >= 0) attn_slot_ = -1;
    for (int i = 0; i < kBoxCount; i++)
        if (glow_on_[i] && now_ms - glow_start_[i] >= kGlowMs) glow_on_[i] = false;

    // Step rotating slots (staggered by kDwellMs / 3 so only one changes at a time).
    BoxMask used = pinned;
    for (int i = 0; i < kBoxCount; i++) {
        if (!rotating_[i]) continue;
        if (attn_slot_ == i && (ok & box_bit(attn_metric_)) != 0) {
            shown_[i] = attn_metric_;
            used |= box_bit(shown_[i]);
            continue;
        }
        const uint32_t off = static_cast<uint32_t>(i) * (kDwellMs / 3);
        const bool step_now = step_index(act_ms_, off + kDwellMs) != step_index(old, off + kDwellMs);
        const bool cur_ok = shown_[i] != BoxKind::Off && shown_[i] != BoxKind::Rotating &&
                            (ok & box_bit(shown_[i])) != 0 && (used & box_bit(shown_[i])) == 0;
        if (!cur_ok || step_now) {
            const BoxKind nxt = next_unpinned(cur_ok ? shown_[i] : BoxKind::LoadR, used, ok);
            if (nxt != BoxKind::Off) shown_[i] = nxt;
            else if (!cur_ok) shown_[i] = next_unpinned(BoxKind::LoadR, used, static_cast<BoxMask>(~0u));
        }
        if (shown_[i] != BoxKind::Off) used |= box_bit(shown_[i]);
    }
}

void BoxCycler::trigger_attention(uint32_t now_ms, BoxKind metric, const std::array<BoxKind, kBoxCount> &cfg, BoxMask ok) {
    if (!live_ || metric == BoxKind::Off || metric == BoxKind::Rotating) return;
    // 1. If already pinned in a box, glow that box directly.
    for (int i = 0; i < kBoxCount; i++) {
        if (cfg[i] == metric) {
            glow_on_[i] = true;
            glow_start_[i] = now_ms;
            return;
        }
    }
    // 2. Otherwise pull into the first Rotating slot (when data is present) and glow it.
    if ((ok & box_bit(metric)) == 0) return;
    for (int i = 0; i < kBoxCount; i++) {
        if (cfg[i] == BoxKind::Rotating) {
            shown_[i] = metric;
            attn_slot_ = static_cast<int8_t>(i);
            attn_metric_ = metric;
            attn_until_ = now_ms + kDwellMs;
            glow_on_[i] = true;
            glow_start_[i] = now_ms;
            return;
        }
    }
}

uint8_t BoxCycler::glow_level(int i, uint32_t now_ms) const {
    if (i < 0 || i >= kBoxCount || !glow_on_[i]) return 0;
    const uint32_t el = now_ms - glow_start_[i];
    if (el >= kGlowMs) return 0;
    const uint32_t rem = kGlowMs - el;
    const uint32_t step = (rem * kGlowSteps + kGlowMs - 1) / kGlowMs;
    return static_cast<uint8_t>(step > kGlowSteps ? kGlowSteps : step);
}

}  // namespace k2::home
