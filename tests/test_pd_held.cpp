// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the charger offers the Charger tab keeps (k2/pd/pd_held.hpp): what counts as the same offers, that quiet
// periods change nothing, and that only different offers replace the held ones (and take the EPR list with them).
#include "k2/pd/pd_held.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

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

}  // namespace

int main() {
    test_same_offers_ignore_the_message_bookkeeping();
    test_spr_part_is_the_first_seven_positions();
    test_quiet_periods_change_nothing();
    test_identical_caps_only_refresh();
    test_different_caps_replace_and_drop_the_old_epr_list();
    test_an_epr_built_list_with_the_same_spr_part_keeps_the_epr_list();
    test_a_different_epr_list_replaces_the_held_one();
    return test_report("test_pd_held");
}
