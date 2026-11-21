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
    return test_report("test_pd_held");
}
