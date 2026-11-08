// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/mode_ram.hpp"
#include "test_util.h"

using namespace k2::mode_ram;

static void test_first_claim_wins() {
    Lease l;
    CHECK_EQ(l.owner() == Tenant::None, true);
    CHECK_EQ(l.claim(Tenant::Net), true);
    CHECK_EQ(l.owner() == Tenant::Net, true);
    CHECK_EQ(l.claim(Tenant::Drive), false);   // the drive would overwrite lwIP's pools
    CHECK_EQ(l.owner() == Tenant::Net, true);
}

static void test_reclaim_by_owner() {
    Lease l;
    CHECK_EQ(l.claim(Tenant::Drive), true);
    CHECK_EQ(l.claim(Tenant::Drive), true);    // init twice in one session is harmless
    CHECK_EQ(l.claim(Tenant::Net), false);
}

static void test_none_is_not_a_tenant() {
    Lease l;
    CHECK_EQ(l.claim(Tenant::None), false);
    CHECK_EQ(l.claim(Tenant::Net), true);
}

static void test_release_by_owner_only() {
    Lease l;
    CHECK_EQ(l.claim(Tenant::Trans), true);
    CHECK_EQ(l.release(Tenant::Sniff), false);     // not the owner
    CHECK_EQ(l.claim(Tenant::Sniff), false);       // still held
    CHECK_EQ(l.release(Tenant::Trans), true);
    CHECK_EQ(l.owner() == Tenant::None, true);
    CHECK_EQ(l.claim(Tenant::Sniff), true);        // free again
    CHECK_EQ(l.release(Tenant::None), false);
}

int main() {
    test_release_by_owner_only();
    test_first_claim_wins();
    test_reclaim_by_owner();
    test_none_is_not_a_tenant();
    return test_report("test_mode_ram");
}
