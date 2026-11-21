// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for what the Charger and Cable tabs keep (k2/pd/pd_held.hpp): what counts as the same offers or the same cable,
// that quiet periods change nothing, and that only a different charger or cable replaces what is held.
#include "k2/pd/pd_held.hpp"
#include "test_util.h"

using namespace k2::pd;
namespace cable_full = k2::cable_full;
namespace emk_variant = k2::emk_variant;

namespace {

Emarker em_of(std::uint16_t vid, std::uint32_t cable_vdo, bool valid = true) {
    Emarker e;
    e.valid = valid;
    e.vid = vid;
    e.num_vdos = 5;
    e.raw_vdos[3] = cable_vdo;
    return e;
}

cable_full::Full full_of(std::initializer_list<std::uint16_t> svids, bool acked = true) {
    cable_full::Full f;
    f.valid = true;
    f.p1.svids_ack = acked;
    for (std::uint16_t s : svids) f.p1.svid[f.p1.nsvid++] = s;
    return f;
}

const Emarker kNoEm{};
const cable_full::Full kNoFull{};

SourceCaps caps(std::initializer_list<std::uint32_t> raws, bool epr = false) {
    SourceCaps c;
    c.valid = true;
    c.epr = epr;
    c.num_pdos = static_cast<std::uint8_t>(raws.size());
    std::size_t i = 0;
    for (std::uint32_t r : raws) c.pdos[i++].raw = r;
    return c;
}

const SourceCaps kNone{};

void test_same_offers_ignore_the_message_bookkeeping() {
    SourceCaps a = caps({1, 2, 3}), b = a;
    b.raw_header = 0x1234; b.rx_count = 99; b.active_cc = 2;
    CHECK_EQ(offers_same(a, b), true);
    b.pdos[2].raw = 4;
    CHECK_EQ(offers_same(a, b), false);
    CHECK_EQ(offers_same(caps({1, 2}), caps({1, 2, 3})), false);
    CHECK_EQ(offers_same(caps({1, 2}, false), caps({1, 2}, true)), false);
}

void test_spr_part_is_the_first_seven_positions() {
    const SourceCaps spr = caps({1, 2, 3, 4, 5, 6, 7});
    const SourceCaps eprb = caps({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, true);
    CHECK_EQ(spr_same(spr, eprb), true);
    CHECK_EQ(spr_same(spr, caps({1, 2, 3, 4, 5, 6, 77})), false);
    CHECK_EQ(spr_same(caps({1, 2}), spr), false);
}

void test_quiet_periods_change_nothing() {
    HeldOffers h;
    const SourceCaps live = caps({10, 20, 30});
    h.capture(live, kNone);
    CHECK_EQ(h.caps().num_pdos, 3);
    h.capture(kNone, kNone);   // the charger sleeps, Rd drops, the port detaches: the driver's lists are empty
    CHECK_EQ(h.caps().valid, true);
    CHECK_EQ(h.caps().pdos[1].raw, 20u);
    SourceCaps empty = caps({});
    h.capture(empty, kNone);   // a valid list with no PDOs is no offer
    CHECK_EQ(h.caps().num_pdos, 3);
}

void test_identical_caps_only_refresh() {
    HeldOffers h;
    SourceCaps a = caps({10, 20, 30});
    a.rx_count = 1;
    h.capture(a, kNone);
    SourceCaps b = a;
    b.rx_count = 2;
    h.capture(b, kNone);
    CHECK_EQ(h.caps().rx_count, 1u);   // not replaced
}

void test_different_caps_replace_and_drop_the_old_epr_list() {
    HeldOffers h;
    h.capture(caps({1, 2, 3, 4, 5, 6, 7}), caps({1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, true));
    CHECK_EQ(h.epr().size(), 10u);
    h.capture(kNone, kNone);
    CHECK_EQ(h.epr().size(), 10u);   // kept while the charger is quiet
    h.capture(caps({1, 2, 3, 4, 5, 6, 77}), kNone);   // a renegotiation with other SPR offers: the EPR list was the old one's
    CHECK_EQ(h.caps().pdos[6].raw, 77u);
    CHECK_EQ(h.epr().size(), 0u);
}

void test_an_epr_built_list_with_the_same_spr_part_keeps_the_epr_list() {
    HeldOffers h;
    h.capture(caps({1, 2, 3, 4, 5, 6, 7}), caps({1, 2, 3, 4, 5, 6, 7, 8, 9}, true));
    h.capture(caps({1, 2, 3, 4, 5, 6, 7, 8, 9}, true), kNone);
    CHECK_EQ(h.caps().num_pdos, 9);
    CHECK_EQ(h.epr().size(), 9u);
}

void test_a_different_epr_list_replaces_the_held_one() {
    HeldOffers h;
    h.capture(caps({1, 2, 3}), caps({1, 2, 3, 8, 9}, true));
    h.capture(caps({1, 2, 3}), caps({1, 2, 3, 8, 90, 91}, true));
    CHECK_EQ(h.epr().size(), 6u);
    CHECK_EQ(h.epr()[4], 90u);
}

void test_cable_stays_after_the_driver_forgets_it() {
    HeldCable h;
    h.capture(em_of(0x2717, 7), full_of({0xFF01, 0x8087}), {});
    CHECK_EQ(h.em().valid, true);
    h.capture(kNoEm, kNoFull, {});   // detach, or a read starting: the driver's copies are cleared
    CHECK_EQ(h.em().vid, 0x2717);
    CHECK_EQ(h.full().p1.nsvid, 2);
    CHECK_EQ(h.thunderbolt(), true);
}

void test_same_identity_without_svids_keeps_them() {
    HeldCable h;
    h.capture(em_of(0x2717, 7), full_of({0xFF01}), {});
    h.capture(em_of(0x2717, 7), full_of({}, false), {});   // a re-read whose SVID step was refused
    CHECK_EQ(h.full().p1.svids_ack, true);
    CHECK_EQ(h.full().p1.svid[0], 0xFF01);
    h.capture(em_of(0x2717, 7), kNoFull, {});   // identity only so far
    CHECK_EQ(h.full().p1.nsvid, 1);
}

void test_a_different_identity_replaces_everything() {
    HeldCable h;
    h.capture(em_of(0x2717, 7), full_of({0xFF01}), {});
    h.capture(em_of(0x2717, 8), kNoFull, {});   // same maker, other cable VDO: another cable
    CHECK_EQ(h.em().raw_vdos[3], 8u);
    CHECK_EQ(h.full().valid, false);   // the old cable's SVIDs are not this one's
    h.capture(em_of(0x05AC, 8), full_of({0x05AC}), {});
    CHECK_EQ(h.em().vid, 0x05AC);
    CHECK_EQ(h.full().p1.svid[0], 0x05AC);
}

void test_a_different_svid_set_replaces_the_held_one() {
    HeldCable h;
    h.capture(em_of(0x2717, 7), full_of({0xFF01}), {});
    h.capture(em_of(0x2717, 7), full_of({0xFF01, 0x8087}), {});
    CHECK_EQ(h.full().p1.nsvid, 2);
    CHECK_EQ(h.thunderbolt(), true);
}

void test_the_answer_variant_follows_the_read() {
    HeldCable h;
    emk_variant::Answered a;
    a.orientation = 2;
    h.capture(em_of(0x2717, 7), kNoFull, a);
    h.capture(kNoEm, kNoFull, {});
    CHECK_EQ(h.answered().orientation, 2);
}

void test_a_full_read_needs_a_valid_identity() {
    HeldCable h;
    h.capture(kNoEm, full_of({0xFF01}), {});   // a leftover full with no identity is not kept
    CHECK_EQ(h.full().valid, false);
}

Pdo fixed_pdo(std::uint16_t mv, std::uint16_t ma) {
    const std::uint32_t raw = (std::uint32_t(mv / 50u) << 10) | std::uint32_t(ma / 10u);
    return decode_pdo(raw);
}

Pdo epr_avs_pdo(std::uint16_t min_mv, std::uint16_t max_mv, std::uint8_t pdp_w) {
    const std::uint32_t raw = (3u << 30) | (1u << 28) | (std::uint32_t(max_mv / 100u) << 17) |
                              (std::uint32_t(min_mv / 100u) << 8) | std::uint32_t(pdp_w);
    return decode_pdo(raw);
}

SourceCaps offer_15w() {
    SourceCaps c;
    c.valid = true;
    c.num_pdos = 1;
    c.pdos[0] = fixed_pdo(5000, 3000);
    return c;
}

SourceCaps offer_60w() {
    SourceCaps c;
    c.valid = true;
    c.num_pdos = 2;
    c.pdos[0] = fixed_pdo(5000, 3000);
    c.pdos[1] = fixed_pdo(20000, 3000);
    return c;
}

SourceCaps offer_100w() {
    SourceCaps c;
    c.valid = true;
    c.num_pdos = 2;
    c.pdos[0] = fixed_pdo(5000, 3000);
    c.pdos[1] = fixed_pdo(20000, 5000);
    return c;
}

SourceCaps offer_140w() {
    SourceCaps c;
    c.valid = true;
    c.epr = true;
    c.num_pdos = 3;
    c.pdos[0] = fixed_pdo(5000, 3000);
    c.pdos[1] = fixed_pdo(20000, 5000);
    c.pdos[2] = epr_avs_pdo(15000, 28000, 140);
    return c;
}

void test_max_caps_mw_handles_decoded_raw_and_dummy_pdos() {
    CHECK_EQ(max_caps_mw(kNone), 0u);
    CHECK_EQ(max_caps_mw(caps({1, 2, 3})), 0u);
    CHECK_EQ(max_caps_mw(offer_15w()), 15000u);
    CHECK_EQ(max_caps_mw(offer_60w()), 60000u);
    CHECK_EQ(max_caps_mw(offer_140w()), 140000u);

    // Pre-populated max_mv/max_ma without max_mw set
    SourceCaps manual;
    manual.valid = true;
    manual.num_pdos = 1;
    manual.pdos[0].max_mv = 28000;
    manual.pdos[0].max_ma = 5000;
    CHECK_EQ(max_caps_mw(manual), 140000u);

    // Raw-only PDO (not pre-decoded)
    SourceCaps raw_only;
    raw_only.valid = true;
    raw_only.num_pdos = 1;
    raw_only.pdos[0].raw = fixed_pdo(20000, 5000).raw;
    CHECK_EQ(max_caps_mw(raw_only), 100000u);
}

void test_settle_gate_rejects_rapid_10hz_offer_storm() {
    HeldOffers h;
    const SourceCaps a = offer_60w();
    const SourceCaps b = offer_100w();

    // 10 Hz alternating offer storm for 2 seconds in inline mode: zero premature commits
    std::uint32_t t = 1000;
    for (int i = 0; i < 20; i++, t += 100) {
        h.capture_settled((i & 1) ? b : a, kNone, t, /*inline_mode=*/true);
        CHECK_EQ(h.caps().valid, false);
        CHECK_EQ(int(h.source()), int(OfferSource::None));
    }

    // Hold steady on a (different from the last storm packet b): must not commit at 799 ms, must commit at 800 ms
    h.capture_settled(a, kNone, t, true);
    h.capture_settled(a, kNone, t + kOfferSettleMs - 1, true);
    CHECK_EQ(h.caps().valid, false);
    h.capture_settled(a, kNone, t + kOfferSettleMs, true);
    CHECK_EQ(h.caps().valid, true);
    CHECK_EQ(max_caps_mw(h.caps()), 60000u);
    CHECK_EQ(int(h.source()), int(OfferSource::Sniffed));
}

void test_initial_boot_glitch_settles_directly_to_140w() {
    HeldOffers h;
    const SourceCaps w15 = offer_15w();
    const SourceCaps w140 = offer_140w();

    // t=1000: 15W boot glitch inline — must never flash on screen
    h.capture_settled(w15, kNone, 1000, /*inline_mode=*/true);
    CHECK_EQ(h.caps().valid, false);

    // t=1200 (200 ms later): 140W arrives, resetting settle window
    h.capture_settled(w140, kNone, 1200, true);
    CHECK_EQ(h.caps().valid, false);

    // t=1999: 799 ms since 140W — still staging
    h.capture_settled(w140, kNone, 1999, true);
    CHECK_EQ(h.caps().valid, false);

    // t=2000: 800 ms since 140W — settles directly to 140W
    h.capture_settled(w140, kNone, 2000, true);
    CHECK_EQ(h.caps().valid, true);
    CHECK_EQ(max_caps_mw(h.caps()), 140000u);
    CHECK_EQ(h.throttled(), false);
    CHECK_EQ(h.live_mw(), 0u);
    CHECK_EQ(int(h.source()), int(OfferSource::Sniffed));
}

void test_high_water_mark_protects_peak_and_recovers() {
    HeldOffers h;
    const SourceCaps w140 = offer_140w();
    const SourceCaps w60 = offer_60w();
    const SourceCaps w15 = offer_15w();

    // Establish 140W peak
    h.capture_settled(w140, kNone, 0, true);
    h.capture_settled(w140, kNone, kOfferSettleMs, true);
    CHECK_EQ(max_caps_mw(h.caps()), 140000u);
    CHECK_EQ(h.throttled(), false);

    // Charger throttles to 60W: staged until 800 ms
    h.capture_settled(w60, kNone, 2000, true);
    h.capture_settled(w60, kNone, 2000 + kOfferSettleMs - 1, true);
    CHECK_EQ(h.throttled(), false);
    CHECK_EQ(h.live_mw(), 0u);

    // After 800 ms: peak stays 140W, throttled() becomes true with live_mw() == 60000
    h.capture_settled(w60, kNone, 2000 + kOfferSettleMs, true);
    CHECK_EQ(max_caps_mw(h.caps()), 140000u);
    CHECK_EQ(h.throttled(), true);
    CHECK_EQ(h.live_mw(), 60000u);

    // Further drop to 15W: still protects 140W, live_mw() updates to 15000
    h.capture_settled(w15, kNone, 4000, true);
    h.capture_settled(w15, kNone, 4000 + kOfferSettleMs, true);
    CHECK_EQ(max_caps_mw(h.caps()), 140000u);
    CHECK_EQ(h.throttled(), true);
    CHECK_EQ(h.live_mw(), 15000u);

    // Recovery back to 140W clears throttled() once settled
    h.capture_settled(w140, kNone, 6000, true);
    h.capture_settled(w140, kNone, 6000 + kOfferSettleMs - 1, true);
    CHECK_EQ(h.throttled(), true);
    h.capture_settled(w140, kNone, 6000 + kOfferSettleMs, true);
    CHECK_EQ(h.throttled(), false);
    CHECK_EQ(h.live_mw(), 0u);
    CHECK_EQ(max_caps_mw(h.caps()), 140000u);
}

void test_probed_commits_immediately_and_overrides_prior() {
    HeldOffers h;
    const SourceCaps w140 = offer_140w();
    const SourceCaps w60 = offer_60w();

    // Non-inline initial commit is immediate with OfferSource::Sniffed
    h.capture_settled(w140, kNone, 100, /*inline_mode=*/false);
    CHECK_EQ(h.caps().valid, true);
    CHECK_EQ(max_caps_mw(h.caps()), 140000u);
    CHECK_EQ(int(h.source()), int(OfferSource::Sniffed));

    // Probed lower offer bypasses settle gate and high-water guard
    h.capture_settled(w60, kNone, 150, /*inline_mode=*/true, /*probed=*/true);
    CHECK_EQ(max_caps_mw(h.caps()), 60000u);
    CHECK_EQ(int(h.source()), int(OfferSource::Probed));
    CHECK_EQ(h.throttled(), false);
    CHECK_EQ(h.live_mw(), 0u);

    // mark_probed() only marks when caps are valid
    HeldOffers empty;
    empty.mark_probed();
    CHECK_EQ(int(empty.source()), int(OfferSource::None));
    empty.capture_settled(w60, kNone, 200, false);
    empty.mark_probed();
    CHECK_EQ(int(empty.source()), int(OfferSource::Probed));
}

void test_clear_wipes_offers_epr_provenance_and_staging() {
    HeldOffers h;
    const SourceCaps w140 = offer_140w();
    const SourceCaps w60 = offer_60w();

    h.capture_settled(w140, caps({1, 2, 3, 4, 5, 6, 7, 8, 9}, true), 100, false, /*probed=*/true);
    h.capture_settled(w60, kNone, 200, true);   // start staging a pending offer
    CHECK_EQ(h.caps().valid, true);
    CHECK_EQ(h.epr().size(), 9u);
    CHECK_EQ(int(h.source()), int(OfferSource::Probed));

    h.clear();
    CHECK_EQ(h.caps().valid, false);
    CHECK_EQ(h.epr().size(), 0u);
    CHECK_EQ(int(h.source()), int(OfferSource::None));
    CHECK_EQ(h.throttled(), false);
    CHECK_EQ(h.live_mw(), 0u);

    // Quiet wire after clear must not revive any old pending staging
    h.capture_settled(kNone, kNone, 2000, true);
    CHECK_EQ(h.caps().valid, false);
}

void test_cable_provenance_and_clear() {
    HeldCable h;
    CHECK_EQ(int(h.source()), int(CableSource::None));
    h.mark_probed();   // no-op when empty
    CHECK_EQ(int(h.source()), int(CableSource::None));

    // Sniffed identity without SVIDs
    h.capture(em_of(0x2717, 7), kNoFull, {});
    CHECK_EQ(int(h.source()), int(CableSource::Sniffed));

    // Same cable with full SVID ACK upgrades to Probed
    h.capture(em_of(0x2717, 7), full_of({0xFF01}), {});
    CHECK_EQ(int(h.source()), int(CableSource::Probed));

    // Subsequent identity-only capture of same cable keeps Probed
    h.capture(em_of(0x2717, 7), kNoFull, {});
    CHECK_EQ(int(h.source()), int(CableSource::Probed));

    // Different cable without probe resets provenance to Sniffed
    h.capture(em_of(0x05AC, 9), kNoFull, {});
    CHECK_EQ(int(h.source()), int(CableSource::Sniffed));
    h.mark_probed();
    CHECK_EQ(int(h.source()), int(CableSource::Probed));

    // Different cable with probed=true sets Probed immediately even without full SVIDs
    h.capture(em_of(0x1037, 11), kNoFull, {}, /*probed=*/true);
    CHECK_EQ(int(h.source()), int(CableSource::Probed));

    h.clear();
    CHECK_EQ(h.em().valid, false);
    CHECK_EQ(h.full().valid, false);
    CHECK_EQ(int(h.source()), int(CableSource::None));
}

}  // namespace

int main() {
    test_same_offers_ignore_the_message_bookkeeping();
    test_spr_part_is_the_first_seven_positions();
    test_quiet_periods_change_nothing();
    test_identical_caps_only_refresh();
    test_different_caps_replace_and_drop_the_old_epr_list();
    test_an_epr_built_list_with_the_same_spr_part_keeps_the_epr_list();
    test_a_different_epr_list_replaces_the_held_one();
    test_cable_stays_after_the_driver_forgets_it();
    test_same_identity_without_svids_keeps_them();
    test_a_different_identity_replaces_everything();
    test_a_different_svid_set_replaces_the_held_one();
    test_the_answer_variant_follows_the_read();
    test_a_full_read_needs_a_valid_identity();
    test_max_caps_mw_handles_decoded_raw_and_dummy_pdos();
    test_settle_gate_rejects_rapid_10hz_offer_storm();
    test_initial_boot_glitch_settles_directly_to_140w();
    test_high_water_mark_protects_peak_and_recovers();
    test_probed_commits_immediately_and_overrides_prior();
    test_clear_wipes_offers_epr_provenance_and_staging();
    test_cable_provenance_and_clear();
    return test_report("test_pd_held");
}
