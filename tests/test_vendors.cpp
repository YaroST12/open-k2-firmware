// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the USB vendor table (tools/vendors.csv -> include/k2/pd/vendor_table.hpp): the lookup at the ends,
// the gaps and every skip-index boundary, the weak flag, and the one name every caller sees.
#include <cstring>

#include "k2/pd/emlib.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/vendor_pack.hpp"
#include "k2/pd/vendor_table.hpp"
#include "test_util.h"

using namespace k2::pd;

// One entry of the committed table, expected straight from the csv (tools/gen_vendors.py writes vendor_expect.inc).
struct Expect { std::uint16_t vid; const char* name; };
static const Expect kExpect[] = {
#include "vendor_expect.inc"
};

// Every entry comes back as its capitals: a wrong skip offset, stride or 6-bit read shows on the entry after a boundary.
static void test_every_entry_round_trips() {
    namespace vt = vendor_table;
    CHECK_EQ(sizeof kExpect / sizeof kExpect[0], vt::kCount);
    for (unsigned i = 0; i < vt::kCount; i++) {
        CHECK_EQ(kExpect[i].vid, vt::kVid[i]);
        if (i) CHECK_EQ(vt::kVid[i - 1] < vt::kVid[i], 1);
        bool weak = false;
        const VendorName n = vendor_find(vt::kVid[i], &weak);
        CHECK_EQ(bool(n), true);
        CHECK_STR(n.c_str(), kExpect[i].name);
        CHECK_EQ(weak, ((vt::kWeak[i >> 3] >> (i & 7)) & 1) != 0);
        CHECK_STR(vendor_name(vt::kVid[i]).c_str(), kExpect[i].name);
    }
}

// The coding on its own, over a pool built here by an independent packer: a name of 16 characters, names that straddle
// bytes at every bit phase, the alphabet's ends, and the terminator-only (empty) name.
static void test_pack_decode_synthetic() {
    static const char kCodes[] = "?abcdefghijklmnopqrstuvwxyz0123456789 -_.";   // index = code (? unused)
    static const char* kNames[] = {"a", "sixteen chars ok", "x", "ab", "abc", "z9 -_.", "", "0123456789abcdef", "q"};
    static const char* kWant[] = {"A", "SIXTEEN CHARS OK", "X", "AB", "ABC", "Z9 -_.", "", "0123456789ABCDEF", "Q"};
    constexpr unsigned kN = 9;
    std::uint8_t pool[64] = {};
    unsigned offs[kN], bit = 0;
    for (unsigned k = 0; k < kN; k++) {
        offs[k] = bit;
        for (const char* p = kNames[k];; p++) {   // the terminator is the code 0 after the last character
            const unsigned code = *p ? unsigned(std::strchr(kCodes + 1, *p) - kCodes) : 0;
            for (unsigned b = 0; b < 6; b++, bit++)
                if ((code >> b) & 1) pool[bit >> 3] |= std::uint8_t(1u << (bit & 7));
            if (!*p) break;
        }
    }
    for (unsigned k = 0; k < kN; k++) {
        char out[kVendorMax + 1];
        const unsigned end = vendor_pack::unpack(pool, offs[k], out);
        CHECK_STR(out, kWant[k]);
        CHECK_EQ(end, k + 1 < kN ? offs[k + 1] : bit);
        CHECK_EQ(vendor_pack::unpack(pool, offs[k], nullptr), end);   // the skip form lands on the same bit
    }
}

static void test_ends_gaps_and_unknown() {
    CHECK_STR(vendor_name(0x03F0).c_str(), "HP");      // first
    CHECK_STR(vendor_name(0x8087).c_str(), "INTEL");   // last
    CHECK_EQ(bool(vendor_find(0x03EF)), false);
    CHECK_EQ(bool(vendor_find(0x8088)), false);
    CHECK_EQ(bool(vendor_find(0x0000)), false);
    CHECK_EQ(bool(vendor_find(0xFFFF)), false);
    CHECK_EQ(bool(vendor_find(0x1234)), false);
    CHECK_STR(vendor_name(0x1234).c_str(), "Unknown");
    CHECK_STR(vendor_name(0xFFFF).c_str(), "Unknown");
}

// The two old tables disagreed on 0x291A: the USB-IF list gives it to Anker, Hynetek is 0x2E99.
static void test_known_vendors() {
    CHECK_STR(vendor_name(0x291A).c_str(), "ANKER");
    CHECK_STR(vendor_name(0x2E99).c_str(), "HYNETEK");
    CHECK_STR(vendor_name(0x05AC).c_str(), "APPLE");
    CHECK_STR(vendor_name(0x2B89).c_str(), "UGREEN");
    CHECK_STR(vendor_name(0x2109).c_str(), "VIA LABS");
    CHECK_STR(vendor_name(0x2095).c_str(), "CE LINK");
    CHECK_STR(vendor_name(0x0451).c_str(), "TI");
}

// Cable e-marker ID headers carry the cable maker's VID: weak names, shared by one company's VIDs.
static void test_cable_makers() {
    bool weak = false;
    CHECK_STR(vendor_find(0x2CA1, &weak).c_str(), "VEETONE");
    CHECK_EQ(weak, true);
    CHECK_STR(vendor_name(0x2B1D).c_str(), "LINTES");
    CHECK_STR(vendor_name(0x2BE2).c_str(), "ECT");
    CHECK_STR(vendor_name(0x0DBF).c_str(), "JESS");
    CHECK_STR(vendor_name(0x0766).c_str(), "JPC");
    CHECK_STR(vendor_name(0x062E).c_str(), "JPC");
    CHECK_STR(vendor_name(0x2E25).c_str(), "GOLDCABLE");
    CHECK_STR(vendor_name(0x3875).c_str(), "GOLDCABLE");
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
    CHECK_EQ(bool(vendor_find(0x291A)), true);         // the flag is optional
}

// Names reach cable names and the PD page rows as capitals of at most kVendorMax characters.
static void test_names_fit() {
    unsigned hits = 0;
    for (unsigned id = 0; id <= 0xFFFF; id++) {
        const VendorName n = vendor_find(std::uint16_t(id));
        if (!n) continue;
        hits++;
        CHECK_EQ(k2::emlib::name_valid(n.c_str()), 1);
        CHECK_EQ(std::strlen(n.c_str()) <= kVendorMax, 1);
        for (const char* p = n.c_str(); *p; p++) CHECK_EQ(*p >= 'a' && *p <= 'z', 0);   // capitals only
    }
    CHECK_EQ(hits, vendor_table::kCount);
    CHECK_EQ(hits >= 150, 1);
}

// Two names alive at once keep their own text (the old API handed out one shared pointer into the pool), and a miss
// leaves an empty name, never the one before.
static void test_two_lookups_do_not_clobber() {
    const VendorName a = vendor_find(0x291A), b = vendor_find(0x2E99);
    CHECK_STR(a.c_str(), "ANKER");
    CHECK_STR(b.c_str(), "HYNETEK");
    CHECK_EQ(a.c_str() != b.c_str(), 1);
    VendorName c = vendor_find(0x0456);
    CHECK_STR(c.c_str(), "ANALOG DEVICES");   // a name the old 10-character cap cut to "Analog Dev"
    c = vendor_find(0x1234);
    CHECK_EQ(c.empty(), true);
    CHECK_STR(vendor_name(0x1234).c_str(), "Unknown");
    const VendorName cut = "a name that is well over sixteen";
    CHECK_STR(cut.c_str(), "a name that is w");   // a fixture's text is cut, never overrun
    CHECK_EQ(VendorName(nullptr).empty(), true);
}

// The skip index: the entries on both sides of every stride boundary, and the very first and last.
static void test_stride_boundaries() {
    namespace vt = vendor_table;
    for (unsigned i = 0; i < vt::kCount; i += vt::kStride) {
        for (unsigned j : {i == 0 ? 0u : i - 1, i, i + 1, i + vt::kStride - 1}) {
            if (j >= vt::kCount) continue;
            CHECK_STR(vendor_find(vt::kVid[j]).c_str(), kExpect[j].name);
        }
    }
    CHECK_STR(vendor_find(vt::kVid[vt::kCount - 1]).c_str(), kExpect[vt::kCount - 1].name);
}

int main() {
    test_every_entry_round_trips();
    test_pack_decode_synthetic();
    test_two_lookups_do_not_clobber();
    test_stride_boundaries();
    test_ends_gaps_and_unknown();
    test_known_vendors();
    test_cable_makers();
    test_weak_flag();
    test_names_fit();
    return test_report("test_vendors");
}
