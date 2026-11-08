// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// E-marker result rows and card texts, built from hand-made and parsed cables.
#include <string>
#include "test_util.h"
#include "k2/ui/ui_emarker.hpp"

using namespace k2;

static std::string kv(const pd::Emarker &e, bool tbt, int i) {
    StrBuf<12> k; StrBuf<16> v;
    emarker_kv(e, tbt, i, k, v);
    return std::string(k.view()) + "=" + std::string(v.view());
}

static pd::Emarker epr_cable() {
    pd::Emarker e;
    e.valid = true; e.max_current_a = 5; e.max_voltage_v = 50; e.max_power_w = 240;
    e.speed_str = "USB 3.2 Gen2 (10G)";
    e.vendor_str = "Apple"; e.vid = 0x05AC; e.pid = 0x1234; e.hw_rev = 1; e.fw_rev = 2;
    e.latency_code = 1; e.latency_str = "<10 ns (~1 m)";
    e.termination = 0; e.sop = 1; e.spec_rev = 2;
    return e;
}

static void test_card_texts() {
    const pd::Emarker e = epr_cable();
    StrBuf<8> a, v, w;
    emarker_amps_text(e, a); emarker_volts_text(e, v); emarker_watts_text(e, w);
    CHECK_STR(std::string(a.view()).c_str(), "5A");
    CHECK_STR(std::string(v.view()).c_str(), "50V");
    CHECK_STR(std::string(w.view()).c_str(), "240W");
    const char *names[][2] = {{"USB 3.2 Gen1 (5G)", "USB3.2 G1"}, {"USB 3.2 Gen2 (10G)", "USB3.2 G2"},
                              {"USB4 Gen3 (40G)", "USB4 G3"}, {"USB4 Gen4 (80G)", "USB4 G4"},
                              {"Thunderbolt (40G)", "TBT"}, {"USB 2.0 (480M)", "USB2.0"}};
    for (auto &n : names) {
        pd::Emarker x = e;
        x.speed_str = n[0];
        StrBuf<12> p;
        emarker_speed_pill(x, p);
        CHECK_STR(std::string(p.view()).c_str(), n[1]);
    }
    pd::Emarker nul = e;
    nul.speed_str = nullptr;
    StrBuf<12> p;
    emarker_speed_pill(nul, p);
    CHECK_STR(std::string(p.view()).c_str(), "USB2.0");
}

static void test_rows() {
    pd::Emarker e = epr_cable();
    CHECK_EQ(emarker_kv_count(e, false), 9);
    CHECK_STR(kv(e, false, 0).c_str(), "Vendor=Apple");
    CHECK_STR(kv(e, false, 1).c_str(), "VID=05AC");
    CHECK_STR(kv(e, false, 2).c_str(), "PID=1234");
    CHECK_STR(kv(e, false, 3).c_str(), "HW/FW=1 / 2");
    CHECK_STR(kv(e, false, 4).c_str(), "Length=~1 m");
    CHECK_STR(kv(e, false, 5).c_str(), "Latency=<10 ns");
    CHECK_STR(kv(e, false, 6).c_str(), "Term=passive");
    CHECK_STR(kv(e, false, 8).c_str(), "SOP='" " PD3.x");
    // Active cable with VDO 2, Thunderbolt and two raw VDOs.
    e.vdo2_valid = true; e.max_temp_c = 60; e.shutdown_temp_c = 80; e.usb_lanes = 2; e.usb4_supported = true;
    e.termination = 3; e.num_vdos = 2; e.raw_vdos[0] = 0xAABBCCDD; e.raw_vdos[1] = 0x11;
    CHECK_EQ(emarker_kv_count(e, true), 9 + 2 + 1 + 2);
    CHECK_STR(kv(e, true, 6).c_str(), "Term=active 2");
    CHECK_STR(kv(e, true, 9).c_str(), "Temp=60/80C");
    CHECK_STR(kv(e, true, 10).c_str(), "USB4=yes 2ln");
    CHECK_STR(kv(e, true, 11).c_str(), "TBT=SVID 8087");
    CHECK_STR(kv(e, true, 12).c_str(), "VDO0=AABBCCDD");
    CHECK_STR(kv(e, true, 13).c_str(), "VDO1=00000011");
    CHECK_STR(kv(e, true, 14).c_str(), "=");
}

static void test_widths_and_latency_codes() {
    // Every row of every shape fits the two columns.
    for (int code = 0; code <= 9; code++) {
        pd::Emarker e = epr_cable();
        e.latency_code = static_cast<uint8_t>(code);
        e.latency_str = code == 8 ? ">70 ns" : code == 0 || code == 9 ? "n/a" : "50-60 ns (~6 m)";
        e.vdo2_valid = true; e.num_vdos = 6; e.termination = static_cast<uint8_t>(code & 3);
        e.vendor_str = "Unknown";
        for (int i = 0; i < emarker_kv_count(e, true); i++) {
            StrBuf<12> k; StrBuf<16> v;
            emarker_kv(e, true, i, k, v);
            CHECK_EQ(k.view().size() <= static_cast<size_t>(kEmKeyChars), 1);
            CHECK_EQ(v.view().size() <= static_cast<size_t>(kEmValChars), 1);
            CHECK_EQ(k.view().empty() || v.view().empty() ? 0 : 1, 1);
        }
    }
    pd::Emarker e = epr_cable();
    e.latency_code = 8;
    CHECK_STR(kv(e, false, 4).c_str(), "Length=>7 m");
    e.latency_code = 0;
    CHECK_STR(kv(e, false, 4).c_str(), "Length=n/a");
    CHECK_EQ(emarker_kv_count(e, false) <= kEmKvMax, 1);
}

int main() {
    test_card_texts();
    test_rows();
    test_widths_and_latency_codes();
    return test_report("test_ui_emarker");
}
