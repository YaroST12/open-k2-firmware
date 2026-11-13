// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the device twin's prediction (k2::twin::predict): one case per numbered rule of include/k2/twin/predict.hpp,
// then the real devices (sink PDO lists read by Identify) against the real chargers (Source_Capabilities words read on the
// bench, with and without the 5 A cable identity).
#include <array>
#include <cstdint>
#include <initializer_list>

#include "k2/twin/predict.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::twin;

namespace {

pd::SourceCaps caps_of(std::initializer_list<std::uint32_t> words) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    for (std::uint32_t w : words) c.pdos[c.num_pdos++] = pd::decode_pdo(w);
    return c;
}

Twin twin_of(std::initializer_list<std::uint32_t> sink, const char* name = "T") {
    Twin t;
    for (std::size_t i = 0; name[i] && i < kNameMax; ++i) t.name[i] = name[i];
    for (std::uint32_t w : sink) t.pdos[t.num_pdos++] = w;
    return t;
}

// Words: fixed = V/50 mV << 10 | I/10 mA; PPS APDO = 0xC0 << 24 | maxV/100 mV << 17 | minV/100 mV << 8 | I/50 mA.
constexpr std::uint32_t fx(unsigned mv, unsigned ma) { return (mv / 50u) << 10 | ma / 10u; }
constexpr std::uint32_t pps(unsigned lo_mv, unsigned hi_mv, unsigned ma) {
    return 0xC0000000u | (hi_mv / 100u) << 17 | (lo_mv / 100u) << 8 | ma / 50u;
}
constexpr std::uint32_t kHigherCap = 1u << 28;  // sink PDO 1: needs more than vSafe5V for full function

// Real devices: the sink PDO words Identify read from each of them with the K2 alone on the port.
const Twin kPixel = twin_of({0x3601912C, 0x0002D12C, 0xC0DC323C}, "Pixel");               // 5/9 V 3 A, PPS 5-11 V 3 A, higher capability
const Twin kOp15 = twin_of({0x2601912C, 0x0002D12C, 0x0003C12C, 0xC0DC3264}, "OP15");     // 5/9/12 V 3 A, PPS 5-11 V 5 A
const Twin kLaptop = twin_of({0x3601912C}, "Laptop");                                      // 5 V 3 A, higher capability

// Real chargers: Source_Capabilities words, k2_remote pdos, 2026-11-11. "3A" = the plain cable, "5A" = the 5 A cable identity.
const std::array<std::uint32_t, 7> kC1_3a = {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0006412C, 0xC8DC323C, 0xC990323C};
const std::array<std::uint32_t, 7> kC1_5a = {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC8DC3278, 0xC9903278};
const std::array<std::uint32_t, 6> kAsus = {0x0B01912C, 0x0002D12C, 0x0003C0FA, 0x0004B0C8, 0x00064096, 0xC8DC213C};
const std::array<std::uint32_t, 5> kNovoo3a = {0x0801912C, 0x0002D12C, 0x0004B12C, 0x0006412C, 0xC1A4323C};
const std::array<std::uint32_t, 5> kNovoo5a = {0x0801912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC1A43264};

template <std::size_t N>
pd::SourceCaps caps_arr(const std::array<std::uint32_t, N>& a) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    for (std::uint32_t w : a) c.pdos[c.num_pdos++] = pd::decode_pdo(w);
    return c;
}

void expect(const Outcome& o, Kind kind, unsigned pos, unsigned mv, unsigned ma, Limiter lim, bool mismatch, int line) {
    const bool ok = o.kind == kind && o.pos == pos && o.mv == mv && o.ma == ma && o.limiter == lim && o.mismatch == mismatch;
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("FAIL line %d: kind %d pos %u %u mV %u mA limiter %d mismatch %d; expected kind %d pos %u %u mV %u mA limiter %d mismatch %d\n",
               line, int(o.kind), o.pos, o.mv, o.ma, int(o.limiter), o.mismatch, int(kind), pos, mv, ma, int(lim), mismatch);
    }
}
#define EXPECT(o, kind, pos, mv, ma, lim, mm) expect((o), (kind), (pos), (mv), (ma), (lim), (mm), __LINE__)

}  // namespace

// Rule 1: a fixed source level is usable only where the twin has a fixed sink PDO at the same voltage.
static void test_rule1_fixed_needs_a_sink_pdo_at_that_voltage() {
    const auto src = caps_of({fx(5000, 3000), fx(9000, 3000), fx(15000, 3000), fx(20000, 3000)});
    EXPECT(predict(twin_of({fx(5000, 3000), fx(9000, 3000)}), src, {}), Kind::Fixed, 2, 9000, 3000, Limiter::Device, false);
    EXPECT(predict(twin_of({fx(5000, 3000), fx(20000, 3000)}), src, {}), Kind::Fixed, 4, 20000, 3000, Limiter::None, false);
    // 12 V is not offered and 15 V is not asked for: only 5 V is common (the 20 V / 3.00 A top level is the cable signature)
    EXPECT(predict(twin_of({fx(5000, 3000), fx(12000, 3000)}), src, {}), Kind::FiveV, 1, 5000, 3000, Limiter::Cable, false);
}

// Rule 2: the current is the lesser of what the source offers and what the twin's sink PDO says it needs at that voltage.
static void test_rule2_current_is_the_minimum() {
    const auto src = caps_of({fx(5000, 3000), fx(9000, 2000)});
    const Outcome o = predict(twin_of({fx(5000, 3000), fx(9000, 3000)}), src, {});
    EXPECT(o, Kind::Fixed, 2, 9000, 2000, Limiter::Charger, true);   // 18 W beats 15 W; the source gave 2 A of the 3 A it needs
    CHECK_EQ(o.mw, 18000);
    const auto big = caps_of({fx(5000, 3000), fx(9000, 5000)});
    EXPECT(predict(twin_of({fx(5000, 1000), fx(9000, 1500)}), big, {}), Kind::Fixed, 2, 9000, 1500, Limiter::Device, false);
}

// Rule 3: a PPS source range is usable against a twin's PPS APDO when the ranges overlap; the voltage is the top of the overlap.
static void test_rule3_pps_overlap() {
    const auto src = caps_of({fx(5000, 3000), pps(3300, 21000, 3000)});
    const Twin t = twin_of({fx(5000, 3000), pps(5000, 11000, 3000)});
    Outcome o = predict(t, src, {});
    EXPECT(o, Kind::Pps, 2, 11000, 3000, Limiter::Device, false);   // the source reaches 21 V, the twin takes 11 V
    CHECK_EQ(o.mw, 33000);
    // the source range ends below the twin's: the top of the overlap is the source's top
    o = predict(t, caps_of({fx(5000, 3000), pps(3300, 9000, 3000)}), {});
    EXPECT(o, Kind::Pps, 2, 9000, 3000, Limiter::Charger, false);
    // no overlap at all: the PPS is unusable, the contract falls to 5 V
    o = predict(t, caps_of({fx(5000, 3000), pps(12000, 21000, 3000)}), {});
    EXPECT(o, Kind::FiveV, 1, 5000, 3000, Limiter::Charger, false);
    // a twin without a PPS APDO never gets a PPS contract
    o = predict(twin_of({fx(5000, 3000)}), src, {});
    EXPECT(o, Kind::FiveV, 1, 5000, 3000, Limiter::Device, false);
}

// Rule 4: a PPS level above 5 V beats every fixed level whatever the power; within a kind the highest power wins, then the higher voltage.
static void test_rule4_pps_first_then_power() {
    const Twin t = twin_of({fx(5000, 3000), fx(9000, 3000), pps(5000, 11000, 3000)});
    // 9 V fixed 27 W vs PPS 11 V 3 A 33 W
    EXPECT(predict(t, caps_of({fx(5000, 3000), fx(9000, 3000), pps(3300, 11000, 3000)}), {}), Kind::Pps, 3, 11000, 3000, Limiter::None, false);   // 33 W, the best the charger has
    // PPS capped to 9 V by the source: 27 W tie with the 9 V fixed level -> PPS
    EXPECT(predict(t, caps_of({fx(5000, 3000), fx(9000, 3000), pps(3300, 9000, 3000)}), {}), Kind::Pps, 3, 9000, 3000, Limiter::Charger, false);
    // fixed ties inside the fixed kind: 9 V 2 A (18 W) vs 18 V 1 A (18 W): higher voltage
    const Twin f = twin_of({fx(5000, 3000), fx(9000, 2000), fx(18000, 1000)});
    EXPECT(predict(f, caps_of({fx(5000, 3000), fx(9000, 2000), fx(18000, 1000)}), {}), Kind::Fixed, 3, 18000, 1000, Limiter::None, false);
    // a PPS that offers LESS power than a fixed level still wins: 6 V 3 A = 18 W against 9 V 3 A = 27 W
    Outcome o = predict(t, caps_of({fx(5000, 3000), fx(9000, 3000), pps(3300, 6000, 3000)}), {});
    EXPECT(o, Kind::Pps, 3, 6000, 3000, Limiter::Charger, false);
    CHECK_EQ(o.mw, 18000);
    // the OP15 (5/9/12 V 3 A, PPS 5-11 V 5 A) on a charger with fixed up to 12 V and PPS 5-11 V 3 A: PPS, up to 33 W (not 12 V 3 A = 36 W)
    o = predict(kOp15, caps_of({fx(5000, 3000), fx(9000, 3000), fx(12000, 3000), pps(3300, 11000, 3000)}), {});
    EXPECT(o, Kind::Pps, 4, 11000, 3000, Limiter::Charger, true);
    CHECK_EQ(o.mw, 33000);
    // a big fixed level (20 V 5 A, 100 W) does not beat a small PPS the twin can use
    EXPECT(predict(twin_of({fx(5000, 3000), fx(20000, 5000), pps(5000, 11000, 3000)}), caps_of({fx(5000, 3000), fx(20000, 5000), pps(3300, 11000, 3000)}), {}),
           Kind::Pps, 3, 11000, 3000, Limiter::Charger, false);   // 33 W of the 100 W it wants
    // a PPS that tops out at 5 V is no step up: the fixed level above it wins
    EXPECT(predict(twin_of({fx(5000, 3000), fx(9000, 3000), pps(3300, 5000, 3000)}), caps_of({fx(5000, 3000), fx(9000, 3000), pps(3300, 5000, 3000)}), {}),
           Kind::Fixed, 2, 9000, 3000, Limiter::None, false);
    // the PPS the twin cannot use (no overlap) leaves the fixed levels
    EXPECT(predict(t, caps_of({fx(5000, 3000), fx(9000, 3000), pps(12000, 21000, 3000)}), {}), Kind::Fixed, 2, 9000, 3000, Limiter::Charger, false);
}

// Rule 5: nothing above 5 V usable -> the 5 V PDO; Capability Mismatch when the twin's Higher Capability bit says it needs more.
static void test_rule5_five_volts_and_mismatch() {
    const auto five = caps_of({fx(5000, 3000)});
    EXPECT(predict(twin_of({fx(5000, 3000), fx(9000, 3000)}), five, {}), Kind::FiveV, 1, 5000, 3000, Limiter::Charger, false);
    EXPECT(predict(twin_of({fx(5000, 3000) | kHigherCap, fx(9000, 3000)}), five, {}), Kind::FiveV, 1, 5000, 3000, Limiter::Charger, true);
    // a 5 V-only device that is happy at 5 V: no mismatch, and the device is the limit on a bigger charger
    EXPECT(predict(twin_of({fx(5000, 1500)}), caps_of({fx(5000, 3000), fx(9000, 3000)}), {}), Kind::FiveV, 1, 5000, 1500, Limiter::Device, false);
    // the source offers less than the twin needs at 5 V
    EXPECT(predict(twin_of({fx(5000, 3000)}), caps_of({fx(5000, 900)}), {}), Kind::FiveV, 1, 5000, 900, Limiter::Charger, true);
}

// Rule 6: no usable result at all -> Kind::None (no caps, no sink PDOs, a first sink PDO that is not 5 V).
static void test_rule6_none() {
    CHECK_EQ(int(predict(kPixel, pd::SourceCaps{}, {}).kind), int(Kind::None));
    CHECK_EQ(int(predict(Twin{}, caps_of({fx(5000, 3000)}), {}).kind), int(Kind::None));
    CHECK_EQ(int(predict(twin_of({fx(9000, 3000)}), caps_of({fx(5000, 3000), fx(9000, 3000)}), {}).kind), int(Kind::None));
}

// Rule 7: a sniffed Request wins when the new list has the same shape of PDO (fixed: same voltage, PPS: the voltage inside the range).
static void test_rule7_observed_request() {
    Twin t = twin_of({fx(5000, 3000), fx(9000, 3000), fx(15000, 3000), pps(5000, 11000, 3000)});
    // the device took 9 V 2 A out of a 9 V 3 A offer although 15 V was there: it asked LESS than it was offered, that is its need
    t.rdo = 2u << 28 | (2000u / 10) << 10 | 2000u / 10;
    t.rdo_pdo = fx(9000, 3000);
    const auto src = caps_of({fx(5000, 3000), fx(9000, 3000), fx(15000, 3000), pps(3300, 11000, 3000)});
    Outcome o = predict(t, src, {});
    EXPECT(o, Kind::Fixed, 2, 9000, 2000, Limiter::Device, false);   // the policy alone would say 15 V 3 A
    CHECK_EQ(o.observed, 1);
    CHECK_EQ(o.want_mw, o.mw);                                        // what the device asked for is what it wants
    // another charger that lacks 9 V: the observation does not apply, the policy picks and says so
    o = predict(t, caps_of({fx(5000, 3000), fx(15000, 3000)}), {});
    EXPECT(o, Kind::Fixed, 2, 15000, 3000, Limiter::None, false);
    CHECK_EQ(o.observed, 0);
    // a source that offers less than it asked: capped, flagged
    o = predict(t, caps_of({fx(5000, 3000), fx(9000, 1000)}), {});
    EXPECT(o, Kind::Fixed, 2, 9000, 1000, Limiter::Charger, true);
    CHECK_EQ(o.observed, 1);
    // PPS observed at 8.6 V 3 A out of a 3 A offer: saturated, so the sink PDO current (3 A) is the need
    Twin p = twin_of({fx(5000, 3000), pps(5000, 11000, 3000)});
    p.rdo = 2u << 28 | (8600u / 20) << 9 | 3000u / 50;
    p.rdo_pdo = pps(3300, 11000, 3000);
    o = predict(p, caps_of({fx(5000, 3000), pps(3300, 21000, 3000)}), {});
    EXPECT(o, Kind::Pps, 2, 8600, 3000, Limiter::Device, false);
    CHECK_EQ(o.observed, 1);
    // PPS observed voltage outside the new charger's range: no match, the policy takes the top of the overlap
    o = predict(p, caps_of({fx(5000, 3000), pps(9000, 21000, 3000)}), {});
    CHECK_EQ(o.observed, 0);
    EXPECT(o, Kind::Pps, 2, 11000, 3000, Limiter::Device, false);
    // a twin made from a sniffed Request alone (no Sink_Capabilities were ever read) predicts from that Request only:
    // a charger with the same shape of PDO answers, any other gets Kind::None (rule 6: there is no sink PDO 1 to judge by)
    Twin r;
    r.rdo = 2u << 28 | (2000u / 10) << 10 | 2000u / 10;
    r.rdo_pdo = fx(9000, 3000);
    o = predict(r, caps_of({fx(5000, 3000), fx(9000, 3000), fx(20000, 3000)}), {});
    EXPECT(o, Kind::Fixed, 2, 9000, 2000, Limiter::Device, false);
    CHECK_EQ(o.observed, 1);
    CHECK_EQ(o.want_mw, o.mw);
    o = predict(r, caps_of({fx(5000, 3000), fx(20000, 3000)}), {});
    CHECK_EQ(int(o.kind), int(Kind::None));
    o = predict(r, caps_of({fx(5000, 3000), fx(9000, 1000)}), {});   // the charger gives less than it asked
    EXPECT(o, Kind::Fixed, 2, 9000, 1000, Limiter::Charger, true);
}

// Rule 8: never above what the charger advertises, and never above the limits of a cable that is known (Facts::cable_*, 0 = unknown).
static void test_rule8_advertised_and_cable_limits() {
    const Twin t = twin_of({fx(5000, 3000), fx(9000, 3000), fx(20000, 3000), pps(5000, 21000, 3000)});
    const auto src = caps_of({fx(5000, 3000), fx(9000, 3000), fx(20000, 3000), pps(3300, 21000, 3000)});
    Facts f;
    f.cable_ma = 2000;   // a cable that carries 2 A: every level is clipped to it, the PPS 21 V x 2 A = 42 W still wins
    EXPECT(predict(t, src, f), Kind::Pps, 4, 21000, 2000, Limiter::Cable, true);
    f.cable_ma = 0;
    f.cable_mv = 12000;  // a cable rated for 12 V: the 20 V level and the top of the PPS range are out
    EXPECT(predict(t, src, f), Kind::Pps, 4, 12000, 3000, Limiter::Cable, false);
    f.cable_mv = 5000;
    EXPECT(predict(t, src, f), Kind::FiveV, 1, 5000, 3000, Limiter::Cable, false);
    // property: over every real device x charger pair the contract is inside the advertised PDO
    for (const Twin* d : {&kPixel, &kOp15, &kLaptop}) {
        for (const pd::SourceCaps& c : {caps_arr(kC1_3a), caps_arr(kC1_5a), caps_arr(kAsus), caps_arr(kNovoo3a), caps_arr(kNovoo5a)}) {
            const Outcome r = predict(*d, c, {});
            CHECK_COND(r.pos >= 1 && r.pos <= c.num_pdos);
            const pd::Pdo& p = c.pdos[r.pos - 1];
            CHECK_COND(r.ma <= p.max_ma);
            CHECK_COND(r.mv <= p.max_mv && r.mv >= p.min_mv);
            CHECK_EQ(r.mw, std::uint32_t(r.mv) * r.ma / 1000);
        }
    }
}

// Rule 9: the limiter. Satisfied twin: the charger offered more (Device) or nothing more (None). Short twin: the cable when the offer
// is held by it (rd_intent's cable_limited, from the source's claim or its no-claim signature), else the charger.
static void test_rule9_limiter() {
    const Twin laptop = twin_of({fx(5000, 3000), fx(20000, 4800)}, "MacBook");
    const auto src3 = caps_of({fx(5000, 3000), fx(9000, 3000), fx(15000, 3000), fx(20000, 3000)});
    Facts claim100;
    claim100.claim_w = 100;
    Outcome o = predict(laptop, src3, claim100);
    EXPECT(o, Kind::Fixed, 4, 20000, 3000, Limiter::Cable, true);
    CHECK_EQ(o.mw, 60000);
    CHECK_EQ(o.want_mw, 96000);
    // no claim: the 20 V / 3.00 A signature says the cable
    EXPECT(predict(laptop, src3, {}), Kind::Fixed, 4, 20000, 3000, Limiter::Cable, true);
    // a plain 60 W charger that claims 60 W: nothing held down, the charger is the limit
    Facts claim60;
    claim60.claim_w = 60;
    EXPECT(predict(laptop, src3, claim60), Kind::Fixed, 4, 20000, 3000, Limiter::Charger, true);
    // a 30 W charger: the charger
    EXPECT(predict(laptop, caps_of({fx(5000, 3000), fx(9000, 3000), fx(20000, 1500)}), {}), Kind::Fixed, 3, 20000, 1500, Limiter::Charger, true);
    // exactly what the device wants and the charger offers no more: full
    EXPECT(predict(laptop, caps_of({fx(5000, 3000), fx(20000, 4800)}), {}), Kind::Fixed, 2, 20000, 4800, Limiter::None, false);
    // more on offer than the device takes: the device is the limit
    EXPECT(predict(laptop, caps_of({fx(5000, 3000), fx(20000, 5000)}), {}), Kind::Fixed, 2, 20000, 4800, Limiter::Device, false);
}

// Rule 11: the kind the device last chose while both were offered (Twin::pref, set by a sniff) replaces rule 4's default on any charger.
static void test_rule11_kind_preference() {
    const auto src = caps_of({fx(5000, 3000), fx(9000, 3000), fx(12000, 3000), pps(3300, 11000, 3000)});
    Twin t = kOp15;
    CHECK_EQ(int(t.pref), int(KindPref::None));
    EXPECT(predict(t, src, {}), Kind::Pps, 4, 11000, 3000, Limiter::Charger, true);   // no preference: rule 4
    t.pref = KindPref::Pps;
    EXPECT(predict(t, src, {}), Kind::Pps, 4, 11000, 3000, Limiter::Charger, true);
    t.pref = KindPref::Fixed;                                                          // it took fixed 12 V with PPS on offer
    Outcome o = predict(t, src, {});
    EXPECT(o, Kind::Fixed, 3, 12000, 3000, Limiter::Charger, false);
    CHECK_EQ(o.mw, 36000);
    CHECK_EQ(o.observed, 0);
    // on a charger of another shape: the preference still applies (the best fixed level there)
    EXPECT(predict(t, caps_of({fx(5000, 3000), fx(9000, 2000), pps(3300, 21000, 5000)}), {}), Kind::Fixed, 2, 9000, 2000, Limiter::Charger, true);
    // no usable fixed level above 5 V: the PPS is what it gets
    EXPECT(predict(t, caps_of({fx(5000, 3000), fx(20000, 3000), pps(3300, 11000, 3000)}), {}), Kind::Pps, 3, 11000, 3000, Limiter::Cable, true);   // 20 V 3 A: the cable signature
    // a charger with no PPS: fixed either way
    EXPECT(predict(t, caps_of({fx(5000, 3000), fx(9000, 3000)}), {}), Kind::Fixed, 2, 9000, 3000, Limiter::Charger, false);
    // a preference for PPS when the charger has none: the fixed levels
    t.pref = KindPref::Pps;
    EXPECT(predict(t, caps_of({fx(5000, 3000), fx(9000, 3000)}), {}), Kind::Fixed, 2, 9000, 3000, Limiter::Charger, false);
    // rule 7 wins: an exact-shape Request applies whatever the preference says
    t.pref = KindPref::Fixed;
    t.rdo = 2u << 28 | (8600u / 20) << 9 | 3000u / 50;
    t.rdo_pdo = pps(3300, 11000, 3000);
    o = predict(t, src, {});
    EXPECT(o, Kind::Pps, 4, 8600, 3000, Limiter::Charger, true);   // it asked 3 A from a 3 A offer: the OP15 needs 5
    CHECK_EQ(o.observed, 1);
}

// The twin's wish: the best power any of its sink PDOs says it can take.
static void test_want_mw() {
    CHECK_EQ(want_mw(kPixel), 33000);
    CHECK_EQ(want_mw(kOp15), 55000);
    CHECK_EQ(want_mw(kLaptop), 15000);
    CHECK_EQ(want_mw(Twin{}), 0);
}

// The grade of a prediction: power it gets against the power it wants. Full at 100 % and over, Partial from 60 %, else Poor;
// no contract is Poor; every other kind, 5 V only included, is graded by the ratio alone.
static void test_grade() {
    Outcome o;
    o.kind = Kind::Fixed;
    o.mw = 33000;
    CHECK_EQ(int(grade(o, 33000)), int(Grade::Full));     // exactly 100 %
    o.mw = 36000;
    CHECK_EQ(int(grade(o, 33000)), int(Grade::Full));     // more than it wants
    o.mw = 19800;
    CHECK_EQ(int(grade(o, 33000)), int(Grade::Partial));  // exactly 60 %
    o.mw = 32999;
    CHECK_EQ(int(grade(o, 33000)), int(Grade::Partial));  // just under 100 %
    o.mw = 599;
    CHECK_EQ(int(grade(o, 1000)), int(Grade::Poor));      // 59.9 %
    o.mw = 600;
    CHECK_EQ(int(grade(o, 1000)), int(Grade::Partial));   // 60.0 %
    o.mw = 0;
    CHECK_EQ(int(grade(o, 33000)), int(Grade::Poor));
    o.mw = 15000;
    CHECK_EQ(int(grade(o, 0)), int(Grade::Full));         // it wants nothing: nothing is missing
    o.kind = Kind::FiveV;
    o.mw = 15000;
    CHECK_EQ(int(grade(o, 15000)), int(Grade::Full));     // 5 V only is graded by ratio: a 15 W device getting 15 W is Full
    o.mw = 15000;
    CHECK_EQ(int(grade(o, 55000)), int(Grade::Poor));     // a 55 W device held at 5 V (27 %)
    o.mw = 33000;
    CHECK_EQ(int(grade(o, 55000)), int(Grade::Partial));  // 60 %: by ratio, whatever the kind
    o.kind = Kind::None;
    o.mw = 33000;
    CHECK_EQ(int(grade(o, 33000)), int(Grade::Poor));     // no contract
    // The real pairs: the 5 A cable identity lifts the OP15 from 36 W (65 %, Partial) to its 55 W
    CHECK_EQ(int(grade(predict(kOp15, caps_arr(kC1_3a), {}), want_mw(kOp15))), int(Grade::Partial));
    CHECK_EQ(int(grade(predict(kOp15, caps_arr(kC1_5a), {}), want_mw(kOp15))), int(Grade::Full));
    CHECK_EQ(int(grade(predict(kLaptop, caps_arr(kC1_3a), {}), want_mw(kLaptop))), int(Grade::Full));   // 5 V 3 A is its 15 W
}

// The Request the live proof sends: the position and the voltage and current this prediction chose.
static void test_to_choice() {
    const Outcome o = predict(kOp15, caps_arr(kC1_5a), {});
    const pd::Choice c = to_choice(o);
    CHECK_EQ(c.pos, 6);
    CHECK_EQ(c.mv, 11000);
    CHECK_EQ(c.ma, 5000);
    CHECK_EQ(int(c.hunt), 0);
    CHECK_EQ(to_choice(Outcome{}).pos, 0);
    // a PPS prediction up to 33 W: the proof asks the PPS position at the top of the overlap (11 V, in range of the PPS APDO), not 12 V fixed
    const Outcome p = predict(kOp15, caps_arr(kC1_3a), {});
    const pd::Choice pc = to_choice(p);
    CHECK_EQ(pc.pos, 6);
    CHECK_EQ(pc.mv, 11000);
    CHECK_EQ(pc.ma, 3000);
}

// The real data: Pixel / OP15 / Laptop x the five bench chargers. The values are worked out by hand from the PDO words.
static void test_real_matrix() {
    const auto c1_3 = caps_arr(kC1_3a), c1_5 = caps_arr(kC1_5a), asus = caps_arr(kAsus), n3 = caps_arr(kNovoo3a), n5 = caps_arr(kNovoo5a);
    // Pixel: PPS 11 V 3 A = 33 W everywhere a PPS reaches 11 V; the charger's own 30 W brick has nothing above 33 W to give
    EXPECT(predict(kPixel, c1_3, {}), Kind::Pps, 6, 11000, 3000, Limiter::Device, false);
    EXPECT(predict(kPixel, c1_5, {}), Kind::Pps, 6, 11000, 3000, Limiter::Device, false);
    EXPECT(predict(kPixel, asus, {}), Kind::Pps, 6, 11000, 3000, Limiter::None, false);
    EXPECT(predict(kPixel, n3, {}), Kind::Pps, 5, 11000, 3000, Limiter::Device, false);
    EXPECT(predict(kPixel, n5, {}), Kind::Pps, 5, 11000, 3000, Limiter::Device, false);
    // OP15: 55 W needs the PPS at 5 A; the 3 A cable holds C1 and NOVOO at 33 W, the 5 A identity lifts both
    EXPECT(predict(kOp15, c1_3, {}), Kind::Pps, 6, 11000, 3000, Limiter::Cable, true);   // PPS up to 33 W, not the 12 V 36 W level (rule 4)
    EXPECT(predict(kOp15, c1_5, {}), Kind::Pps, 6, 11000, 5000, Limiter::Device, false);
    EXPECT(predict(kOp15, asus, {}), Kind::Pps, 6, 11000, 3000, Limiter::Charger, true);
    EXPECT(predict(kOp15, n3, {}), Kind::Pps, 5, 11000, 3000, Limiter::Cable, true);
    EXPECT(predict(kOp15, n5, {}), Kind::Pps, 5, 11000, 5000, Limiter::Device, false);
    // Laptop: its one sink PDO is 5 V 3 A with the Higher Capability bit: 5 V with Capability Mismatch on every charger
    for (const pd::SourceCaps* c : {&c1_3, &c1_5, &asus, &n3, &n5}) EXPECT(predict(kLaptop, *c, {}), Kind::FiveV, 1, 5000, 3000, Limiter::Device, true);
}

int main() {
    test_rule1_fixed_needs_a_sink_pdo_at_that_voltage();
    test_rule2_current_is_the_minimum();
    test_rule3_pps_overlap();
    test_rule4_pps_first_then_power();
    test_rule5_five_volts_and_mismatch();
    test_rule6_none();
    test_rule7_observed_request();
    test_rule8_advertised_and_cable_limits();
    test_rule9_limiter();
    test_rule11_kind_preference();
    test_want_mw();
    test_grade();
    test_to_choice();
    test_real_matrix();
    return test_report("test_twin_predict");
}
