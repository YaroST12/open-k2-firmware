// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the USB vendor table (tools/vendors.csv -> include/k2/pd/vendor_table.hpp): the lookup at the ends,
// the gaps and every skip-index boundary, the weak flag, and the one name every caller sees.
#include <cstring>

#include "k2/pd/emlib.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/vendor_table.hpp"
#include "test_util.h"

using namespace k2::pd;

// Every entry comes back as its own pool name: a wrong skip offset or stride shows on the entry after a boundary.
static void test_every_entry_round_trips() {
    namespace vt = vendor_table;
    const char* p = vt::kPool;
    for (unsigned i = 0; i < vt::kCount; i++) {
        if (i) CHECK_EQ(vt::kVid[i - 1] < vt::kVid[i], 1);
        bool weak = false;
        const char* n = vendor_find(vt::kVid[i], &weak);
        CHECK_EQ(n != nullptr, 1);
        if (n) CHECK_STR(n, p);
        CHECK_EQ(weak, ((vt::kWeak[i >> 3] >> (i & 7)) & 1) != 0);
        CHECK_STR(vendor_name(vt::kVid[i]), p);
        p += std::strlen(p) + 1;
    }
    CHECK_EQ(p == vt::kPool + sizeof vt::kPool - 1, 1);   // the pool holds nothing but the names
}

static void test_ends_gaps_and_unknown() {
    CHECK_STR(vendor_name(0x03F0), "HP");      // first
    CHECK_STR(vendor_name(0x8087), "Intel");   // last
    CHECK_EQ(vendor_find(0x03EF) == nullptr, 1);
    CHECK_EQ(vendor_find(0x8088) == nullptr, 1);
    CHECK_EQ(vendor_find(0x0000) == nullptr, 1);
    CHECK_EQ(vendor_find(0xFFFF) == nullptr, 1);
    CHECK_EQ(vendor_find(0x1234) == nullptr, 1);
    CHECK_STR(vendor_name(0x1234), "Unknown");
    CHECK_STR(vendor_name(0xFFFF), "Unknown");
}

// The two old tables disagreed on 0x291A: the USB-IF list gives it to Anker, Hynetek is 0x2E99.
static void test_known_vendors() {
    CHECK_STR(vendor_name(0x291A), "Anker");
    CHECK_STR(vendor_name(0x2E99), "Hynetek");
    CHECK_STR(vendor_name(0x05AC), "Apple");
    CHECK_STR(vendor_name(0x2B89), "UGREEN");
    CHECK_STR(vendor_name(0x2109), "VIA Labs");
    CHECK_STR(vendor_name(0x2095), "CE Link");
    CHECK_STR(vendor_name(0x0451), "TI");
}

// Cable e-marker ID headers carry the cable maker's VID: weak names, shared by one company's VIDs.
static void test_cable_makers() {
    bool weak = false;
    CHECK_STR(vendor_find(0x2CA1, &weak), "Veetone");
    CHECK_EQ(weak, true);
    CHECK_STR(vendor_name(0x2B1D), "Lintes");
    CHECK_STR(vendor_name(0x2BE2), "ECT");
    CHECK_STR(vendor_name(0x0DBF), "JPC");
    CHECK_STR(vendor_name(0x0766), "JPC");
    CHECK_STR(vendor_name(0x062E), "JPC");
    CHECK_STR(vendor_name(0x2E25), "GoldCable");
    CHECK_STR(vendor_name(0x3875), "GoldCable");
    vendor_find(0x3271, &weak); CHECK_EQ(weak, true);    // a charger ODM yields to the charger's own string
    vendor_find(0x2943, &weak); CHECK_EQ(weak, false);   // a retail brand
}

static void test_weak_flag() {
    bool weak = false;
    vendor_find(0x291A, &weak); CHECK_EQ(weak, false);   // a brand
    vendor_find(0x05AC, &weak); CHECK_EQ(weak, false);
    vendor_find(0x0451, &weak); CHECK_EQ(weak, true);    // a chip maker
    vendor_find(0x1C7B, &weak); CHECK_EQ(weak, true);    // a cable assembler
    vendor_find(0x2B01, &weak); CHECK_EQ(weak, true);    // shared by two brands
    vendor_find(0x1234, &weak); CHECK_EQ(weak, false);   // unknown: cleared, not left over
    CHECK_EQ(vendor_find(0x291A) != nullptr, 1);         // the flag is optional
}

// Names reach cable names and the PD page rows ("VID xxxx <vendor>", 22 characters) unchanged.
static void test_names_fit() {
    unsigned hits = 0;
    for (unsigned id = 0; id <= 0xFFFF; id++) {
        const char* n = vendor_find(std::uint16_t(id));
        if (!n) continue;
        hits++;
        CHECK_EQ(k2::emlib::name_valid(n), 1);
        CHECK_EQ(std::strlen(n) <= 10, 1);
    }
    CHECK_EQ(hits, vendor_table::kCount);
    CHECK_EQ(hits >= 150, 1);
}

int main() {
    test_every_entry_round_trips();
    test_ends_gaps_and_unknown();
    test_known_vendors();
    test_cable_makers();
    test_weak_flag();
    test_names_fit();
    return test_report("test_vendors");
}
