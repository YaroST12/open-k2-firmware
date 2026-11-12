// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home 2c pure parts: the hero quantity cycle, the contract power, the peak / sag tracker and its texts, and the
// right-column card cycler (dwell, offset, skip, no duplicate, Wh / Ah alternation, reset, freeze, the force seam).
#include <cstring>
#include "test_util.h"
#include "k2/ui/home_cards.hpp"

using namespace k2;
using namespace k2::home;

static int ci(Card c) { return static_cast<int>(c); }

static void test_hero_cycle() {
    CHECK_EQ(static_cast<int>(next(Qty::W)), static_cast<int>(Qty::V));
    CHECK_EQ(static_cast<int>(next(Qty::V)), static_cast<int>(Qty::A));
    CHECK_EQ(static_cast<int>(next(Qty::A)), static_cast<int>(Qty::W));
    // The stored byte: 0 (and anything unknown) is W, the migration-safe default.
    CHECK_EQ(static_cast<int>(qty_from(0)), static_cast<int>(Qty::W));
    CHECK_EQ(static_cast<int>(qty_from(1)), static_cast<int>(Qty::V));
    CHECK_EQ(static_cast<int>(qty_from(2)), static_cast<int>(Qty::A));
    CHECK_EQ(static_cast<int>(qty_from(3)), static_cast<int>(Qty::W));
    // The ledger rows keep W, V, A order without the hero.
    const Ledger w = ledger_of(Qty::W), v = ledger_of(Qty::V), a = ledger_of(Qty::A);
    CHECK_EQ(static_cast<int>(w.row[0]), static_cast<int>(Qty::V));
    CHECK_EQ(static_cast<int>(w.row[1]), static_cast<int>(Qty::A));
    CHECK_EQ(static_cast<int>(v.row[0]), static_cast<int>(Qty::W));
    CHECK_EQ(static_cast<int>(v.row[1]), static_cast<int>(Qty::A));
    CHECK_EQ(static_cast<int>(a.row[0]), static_cast<int>(Qty::W));
    CHECK_EQ(static_cast<int>(a.row[1]), static_cast<int>(Qty::V));
}

static void test_contract_power() {
    ProtoInfo p;
    p.kind = ProtoKind::PdFixed; p.mv = 20000; p.ma = 5000;
    CHECK_EQ(contract_mw(p), 100000u);                 // PD fixed: V x I
    p.kind = ProtoKind::PdEpr; p.mv = 28000; p.ma = 5000;
    CHECK_EQ(contract_mw(p), 140000u);
    p = {}; p.kind = ProtoKind::Pps; p.lo_mv = 3300; p.hi_mv = 21000; p.ma = 5000; p.mv = 9200;
    CHECK_EQ(contract_mw(p), 105000u);                 // PPS: the APDO's maximum
    p = {}; p.kind = ProtoKind::Avs; p.lo_mv = 15000; p.hi_mv = 28000; p.mw = 140000; p.ma = 5000;
    CHECK_EQ(contract_mw(p), 140000u);                 // AVS: its PDP
    p = {}; p.kind = ProtoKind::TypeC; p.mv = 5000; p.ma = 3000;
    CHECK_EQ(contract_mw(p), 15000u);                  // Type-C: 5 V x the Rp current
    for (ProtoKind k : {ProtoKind::Qc2, ProtoKind::Qc3, ProtoKind::Fcp, ProtoKind::Scp, ProtoKind::Svooc, ProtoKind::Dcp,
                        ProtoKind::None, ProtoKind::Unknown, ProtoKind::Detecting}) {
        p = {}; p.kind = k; p.mv = 9000; p.ma = 2000; p.hi_mv = 12000;
        CHECK_EQ(contract_mw(p), 0u);                  // no power rating for the legacy protocols
    }
    p = {}; p.kind = ProtoKind::PdFixed; p.mv = 20000;
    CHECK_EQ(contract_mw(p), 0u);                      // current unknown: no rating
}

static void test_sag_level() {
    ProtoInfo p;
    p.kind = ProtoKind::PdFixed; p.mv = 20000; p.ma = 5000;
    CHECK_EQ(sag_level_mv(p, 0), 20000u);              // the fixed PDO's volts
    p = {}; p.kind = ProtoKind::Pps; p.mv = 9200; p.lo_mv = 3300; p.hi_mv = 21000;
    CHECK_EQ(sag_level_mv(p, 0), 9200u);               // the requested level
    p = {}; p.kind = ProtoKind::Avs; p.mv = 15000; p.lo_mv = 15000; p.hi_mv = 28000;
    CHECK_EQ(sag_level_mv(p, 0), 15000u);
    p = {}; p.kind = ProtoKind::Qc3; p.lo_mv = 3600; p.hi_mv = 12000;
    CHECK_EQ(sag_level_mv(p, 9200), 9200u);           // the level the K2's own trigger holds
    CHECK_EQ(sag_level_mv(p, 0), 0u);                  // a phone's QC3 steps are not heard: no level
    p = {}; p.kind = ProtoKind::Qc2; p.mv = 9000;
    CHECK_EQ(sag_level_mv(p, 0), 9000u);               // decoded from the data lines
    p = {}; p.kind = ProtoKind::TypeC; p.mv = 4000; p.ma = 3000;
    CHECK_EQ(sag_level_mv(p, 0), 5000u);               // a Type-C or BC1.2 source is a 5 V source
    p = {}; p.kind = ProtoKind::Dcp; p.mv = 5000;
    CHECK_EQ(sag_level_mv(p, 0), 5000u);
    for (ProtoKind k : {ProtoKind::Fcp, ProtoKind::Scp, ProtoKind::Svooc, ProtoKind::Ufcs, ProtoKind::None,
                        ProtoKind::Unknown, ProtoKind::Detecting}) {
        p = {}; p.kind = k; p.mv = 9000;               // a level rounded from VBUS itself is no level
        CHECK_EQ(sag_level_mv(p, 0), 0u);
    }
    p = {}; p.kind = ProtoKind::PdFixed; p.measured = true; p.mv = 9000;
    CHECK_EQ(sag_level_mv(p, 0), 0u);
}

static void test_peaks_and_sag() {
    Extremes e;
    CHECK_EQ(e.peak_mw(), 0u);
    CHECK_EQ(e.has_sag() ? 1 : 0, 0);
    e.sample(20000, 1000, 20000, 20000);
    e.sample(19800, -3000, -59400, 20000);             // reverse flow: magnitudes
    e.sample(19900, 2000, 39800, 20000);
    CHECK_EQ(e.peak_mw(), 59400u);
    CHECK_EQ(e.peak_ma(), 3000u);
    CHECK_EQ(e.has_sag() ? 1 : 0, 1);
    CHECK_EQ(e.sag_bp(), 100u);                        // (20 - 19.8) / 20 = 1.00 %
    CHECK_EQ(e.sag_level_mv(), 20000u);
    e.sample(18000, 40, 720, 20000);                   // 50 mA or less: not counted
    CHECK_EQ(e.sag_bp(), 100u);
    e.sample(18000, 51, 918, 20000);
    CHECK_EQ(e.sag_bp(), 1000u);                       // 10.00 %
    e.sample(20500, 2000, 41000, 20000);               // above the level: no dip, never a negative sag
    CHECK_EQ(e.sag_bp(), 1000u);
    e.sample(8280, 2000, 16560, 9200);                 // a new contract: the worst dip stays with its level
    CHECK_EQ(e.sag_bp(), 1000u);
    CHECK_EQ(e.sag_level_mv(), 20000u);
    e.sample(8000, 2000, 16000, 9200);                 // 13.04 % of 9.2 V is worse
    CHECK_EQ(e.sag_bp(), 1304u);
    CHECK_EQ(e.sag_level_mv(), 9200u);
    e.sample(5000, 2000, 10000, 0);                    // no level in force: no sag sample
    CHECK_EQ(e.sag_bp(), 1304u);
    e.reset();
    CHECK_EQ(e.peak_mw(), 0u);
    CHECK_EQ(e.peak_ma(), 0u);
    CHECK_EQ(e.has_sag() ? 1 : 0, 0);
    // A flat contract still reads a sag: 0.0 % at its level.
    e.sample(20100, 1000, 20100, 20000);
    CHECK_EQ(e.has_sag() ? 1 : 0, 1);
    CHECK_EQ(e.sag_bp(), 0u);
}

static void test_card_texts() {
    char b[16];
    fmt_sag(b, 100, 20000); CHECK_STR(b, "1.0% 20V");
    fmt_sag(b, 0, 20000); CHECK_STR(b, "0.0% 20V");
    fmt_sag(b, 1200, 9200); CHECK_STR(b, "12% 9.2V");
    fmt_sag(b, 1304, 9200); CHECK_STR(b, "13% 9.2V");
    fmt_sag(b, 994, 9200); CHECK_STR(b, "9.9% 9.2V");
    fmt_sag(b, 996, 9200); CHECK_STR(b, "10% 9.2V");    // rounds into the integer form
    fmt_sag(b, 950, 11400); CHECK_STR(b, "9.5% 11V");   // 10 characters would not fit: the level drops its decimal
    fmt_sag(b, 1500, 11400); CHECK_STR(b, "15% 11.4V");
    fmt_sag(b, 10000, 28000); CHECK_STR(b, "100% 28V");
    fmt_sag(b, 25, 5000); CHECK_STR(b, "0.3% 5V");
    for (uint32_t bp : {0u, 5u, 994u, 996u, 1500u, 9999u, 10000u})
        for (uint32_t mv : {3300u, 5000u, 9200u, 11400u, 20000u, 21000u, 28000u, 48000u}) {
            const int n = fmt_sag(b, bp, mv);
            CHECK_COND(n <= kSagChars);
            CHECK_EQ(static_cast<int>(std::strlen(b)), n);
        }
    fmt_drop(b, 123); CHECK_STR(b, "0.123V");
    fmt_drop(b, 2500); CHECK_STR(b, "2.500V");
    fmt_drop(b, 0); CHECK_STR(b, "0.000V");
}

// ---- cycler ----------------------------------------------------------------------------------

constexpr CardMask kAll = card_bit(Card::Peak) | card_bit(Card::Drop) | card_bit(Card::Cable) | card_bit(Card::Lines);

// Runs the cycler at 100 ms steps from `t` to `t_end` (exclusive), checking the invariants on every step.
static uint32_t run(CardCycler &c, uint32_t t, uint32_t t_end, bool live, CardMask ok) {
    for (; t < t_end; t += 100) {
        c.tick(t, live, ok);
        CHECK_COND(c.top() != Card::Energy);                        // the upper cell never shows Wh / Ah
        CHECK_COND(c.top() != c.bottom());                          // never the same card twice
    }
    return t;
}

static void test_cycler_dwell_and_offset() {
    CardCycler c;
    c.reset();
    c.tick(1000, true, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Peak));
    CHECK_EQ(ci(c.bottom()), ci(Card::Energy));
    // The lower cell moves first, half a dwell in; the upper one at the full dwell; only one cell changes at a time.
    uint32_t t = run(c, 1100, 1000 + 2400, true, kAll);
    CHECK_EQ(ci(c.bottom()), ci(Card::Energy));
    t = run(c, t, 1000 + 2600, true, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Peak));
    CHECK_EQ(ci(c.bottom()), ci(Card::Cable));                      // next after its start (Drop), skipping the top's card
    t = run(c, t, 1000 + 4900, true, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Peak));
    t = run(c, t, 1000 + 5100, true, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Drop));                          // dwell 5 s
    CHECK_EQ(ci(c.bottom()), ci(Card::Cable));
    t = run(c, t, 1000 + 7600, true, kAll);
    CHECK_EQ(ci(c.bottom()), ci(Card::Energy));                     // every other turn of the lower cell is Wh / Ah
    t = run(c, t, 1000 + 10100, true, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Cable));                         // Wh / Ah below: the top may take any card
    t = run(c, t, 1000 + 12600, true, kAll);
    CHECK_EQ(ci(c.bottom()), ci(Card::Lines));                      // Cable is on top: skipped
    t = run(c, t, 1000 + 15100, true, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Peak));                          // Lines is below: skipped
    // Over a long run every deck card shows in each cell, and the lower one is Wh / Ah half the time.
    int top_seen[5] = {}, bot_seen[5] = {}, energy = 0, turns = 0;
    for (uint32_t end = t + 60000; t < end; t += 100) {
        c.tick(t, true, kAll);
        CHECK_COND(c.top() != c.bottom());
        top_seen[ci(c.top())]++;
        bot_seen[ci(c.bottom())]++;
        energy += c.bottom() == Card::Energy ? 1 : 0;
        turns++;
    }
    CHECK_EQ(top_seen[ci(Card::Energy)], 0);
    for (Card k : {Card::Peak, Card::Drop, Card::Cable, Card::Lines}) {
        CHECK_COND(top_seen[ci(k)] > 0);
        CHECK_COND(bot_seen[ci(k)] > 0);
    }
    CHECK_COND(energy * 2 >= turns - 60 && energy * 2 <= turns + 60);
}

static void test_cycler_skips_cards_without_data() {
    CardCycler c;
    c.reset();
    const CardMask ok = card_bit(Card::Peak) | card_bit(Card::Lines);   // no drop or sag yet, no e-marker
    uint32_t t = run(c, 0, 60000, true, ok);
    (void)t;
    // Only Peak and Lines exist: the cells trade them and never show the others.
    CardCycler d;
    d.reset();
    for (uint32_t s = 0; s < 60000; s += 100) {
        d.tick(s, true, ok);
        CHECK_COND(d.top() == Card::Peak || d.top() == Card::Lines);
        CHECK_COND(d.bottom() == Card::Energy || d.bottom() == Card::Peak || d.bottom() == Card::Lines);
    }
    // A card that loses its data mid-dwell leaves at once.
    CardCycler e;
    e.reset();
    run(e, 0, 5100, true, kAll);
    CHECK_EQ(ci(e.top()), ci(Card::Drop));
    e.tick(5200, true, kAll & ~card_bit(Card::Drop));
    CHECK_COND(e.top() != Card::Drop);
    CHECK_COND(e.top() != e.bottom());
}

static void test_cycler_freeze_and_reset() {
    CardCycler c;
    c.reset();
    uint32_t t = run(c, 0, 7600, true, kAll);
    const Card top = c.top(), bot = c.bottom();
    CHECK_COND(top != Card::Peak || bot != Card::Energy);
    // Stale or idle: cycling stops, the cells show PEAK above Wh / Ah.
    t = run(c, t, t + 20000, false, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Peak));
    CHECK_EQ(ci(c.bottom()), ci(Card::Energy));
    // Back live without a reset: the deck resumes where it stopped; the gap does not count toward the dwell.
    c.tick(t, true, kAll);
    CHECK_EQ(ci(c.top()), ci(top));
    CHECK_EQ(ci(c.bottom()), ci(bot));
    // A stalled loop does not skip cards: one late tick moves at most one cell.
    c.tick(t + 30000, true, kAll);
    const bool moved_top = c.top() != top, moved_bot = c.bottom() != bot;
    CHECK_COND(!(moved_top && moved_bot));
    // Reset (attach, long OK): the phase starts over.
    c.reset();
    c.tick(t + 40000, true, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Peak));
    CHECK_EQ(ci(c.bottom()), ci(Card::Energy));
    c.tick(t + 42400, true, kAll);
    CHECK_EQ(ci(c.bottom()), ci(Card::Energy));
}

static void test_cycler_force_seam() {
    CardCycler c;
    c.reset();
    uint32_t t = run(c, 0, 2600, true, kAll);
    CHECK_EQ(ci(c.bottom()), ci(Card::Cable));
    CHECK_EQ(c.forced() ? 1 : 0, 0);
    c.force_top(Card::Cable, kAll);                    // the attention seam: a card takes the upper cell
    CHECK_EQ(ci(c.top()), ci(Card::Cable));
    CHECK_EQ(c.forced() ? 1 : 0, 1);
    CHECK_COND(c.bottom() != Card::Cable);             // the lower cell moves off it
    CHECK_COND(c.bottom() != Card::Energy);
    c.force_top(Card::Energy, kAll);                   // never Wh / Ah on top
    CHECK_EQ(ci(c.top()), ci(Card::Cable));
    // The forced card holds until the next dwell of the upper cell, then the deck moves on and the mark goes.
    t = run(c, t, 5100, true, kAll);
    CHECK_COND(c.top() != Card::Cable);
    CHECK_EQ(c.forced() ? 1 : 0, 0);
    // Not live: no forcing.
    c.tick(t, false, kAll);
    c.force_top(Card::Lines, kAll);
    CHECK_EQ(ci(c.top()), ci(Card::Peak));
    CHECK_EQ(c.forced() ? 1 : 0, 0);
}

// DROP: the cable_r two-point idea on the live reading: the idle reference, then at least 1 A more.
static void test_drop_state() {
    using S = DropState;
    CHECK_EQ(static_cast<int>(drop_of(false, true, 20000, 50, 19800, 3000).state), static_cast<int>(S::None));   // not live
    CHECK_EQ(static_cast<int>(drop_of(true, false, 0, 0, 19800, 3000).state), static_cast<int>(S::NoIdle));
    CHECK_EQ(static_cast<int>(drop_of(true, true, 20000, 100, 19950, 1050).state), static_cast<int>(S::NeedLoad));   // 0.95 A more
    const Drop d = drop_of(true, true, 20000, 100, 19877, -1100);    // |I| counts: 1.0 A more
    CHECK_EQ(static_cast<int>(d.state), static_cast<int>(S::Value));
    CHECK_EQ(d.mv, 123u);
    CHECK_EQ(drop_of(true, true, 20000, 100, 20050, 2000).mv, 0u);  // a rise is no drop: 0, never a sign
    // Every text of the card fits its 74 px (font 16: 9 characters; font 12: 12).
    char t[12];
    CHECK_EQ(fmt_drop(t, 9999) <= 9, 1);
}

// The level the sag is measured against only counts once it has held kSettleMs: a new contract's level arrives before VBUS
// has ramped to it (5 V -> 20 V would read as a 75 % sag under load), and a PPS step moves it for a moment.
static void test_settled_level() {
    SettledLevel s;
    CHECK_EQ(s.update(1000, 20000), 0u);                          // just seen
    CHECK_EQ(s.update(1000 + SettledLevel::kSettleMs - 1, 20000), 0u);
    CHECK_EQ(s.update(1000 + SettledLevel::kSettleMs, 20000), 20000u);
    CHECK_EQ(s.update(5000, 9000), 0u);                           // a new level starts over
    CHECK_EQ(s.update(5000 + SettledLevel::kSettleMs, 9000), 9000u);
    CHECK_EQ(s.update(9000, 0), 0u);                              // none known: none, at once
    CHECK_EQ(s.update(9100, 9000), 0u);
    s.reset();
    CHECK_EQ(s.update(20000, 9000), 0u);
    // The sag tracker sees no dip while the level settles.
    Extremes x;
    SettledLevel lv;
    x.sample(5100, 2000, 10200, lv.update(0, 20000));             // 5 V still on the wire, 20 V just accepted
    CHECK_EQ(x.has_sag() ? 1 : 0, 0);
}

int main() {
    test_settled_level();
    test_drop_state();
    test_hero_cycle();
    test_contract_power();
    test_sag_level();
    test_peaks_and_sag();
    test_card_texts();
    test_cycler_dwell_and_offset();
    test_cycler_skips_cards_without_data();
    test_cycler_freeze_and_reset();
    test_cycler_force_seam();
    return test_report("test_home_cards");
}
