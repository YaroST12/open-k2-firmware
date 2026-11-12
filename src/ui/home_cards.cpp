// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/home_cards.hpp"

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

constexpr Card kDeck[] = {Card::Peak, Card::Drop, Card::Cable, Card::Lines};
constexpr int kDeckN = 4;

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
        return static_cast<uint32_t>(static_cast<uint64_t>(p.mv) * p.ma / 1000);
    case ProtoKind::Pps:
        return static_cast<uint32_t>(static_cast<uint64_t>(p.hi_mv) * p.ma / 1000);
    case ProtoKind::Avs:
        return p.mw;
    case ProtoKind::TypeC:
        return static_cast<uint32_t>(5000ull * p.ma / 1000);
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
    const uint32_t a = mag(ibus_ma), w = mag(power_mw);
    if (w > peak_mw_) peak_mw_ = w;
    if (a > peak_ma_) peak_ma_ = a;
    if (a <= kSagMinMa || level_mv == 0) return;
    const uint32_t v = vbus_mv < 0 ? 0 : static_cast<uint32_t>(vbus_mv);
    const uint32_t dip = v < level_mv ? level_mv - v : 0;
    const uint32_t bp = static_cast<uint32_t>((static_cast<uint64_t>(dip) * 10000 + level_mv / 2) / level_mv);
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
    if (step_index(act_ms_, kDwellMs) != step_index(old, kDwellMs)) {
        forced_ = false;
        const Card c = next_card(top_, energy_ ? Card::Count : bot_, ok);
        if (c != Card::Count) top_ = c;
    }
    settle(ok);
}

void CardCycler::force_top(Card c, CardMask ok) {
    if (!live_ || c == Card::Energy || c == Card::Count || !has(ok, c)) return;
    top_ = c;
    forced_ = true;
    settle(ok);
}

}  // namespace k2::home
