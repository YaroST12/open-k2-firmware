// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the sink wants list: where the list comes from, how its rows line up with the
// charger's offers, the first-PDO flags and the "ask the device" hint. Vectors are the Pixel's real
// Sink_Capabilities (hardware, 2026-11).
#include <string>
#include "test_util.h"
#include "k2/ui/ui_match.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"

using namespace k2;

namespace {
constexpr uint32_t kPixel[3] = {0x3601912C, 0x0002D12C, 0xC0DC323C};   // 5 V 3 A +flags, 9 V 3 A, PPS 5-11 V 3 A

constexpr uint32_t fixed(int mv, int ma) { return (uint32_t(mv / 50) << 10) | uint32_t(ma / 10); }
constexpr uint32_t pps(int min_mv, int max_mv, int ma) {
    return 0xC0000000u | (uint32_t(max_mv / 100) << 17) | (uint32_t(min_mv / 100) << 8) | uint32_t(ma / 50);
}
pd::SourceCaps caps_of(std::initializer_list<uint32_t> raws) {
    pd::SourceCaps c;
    c.valid = true;
    for (uint32_t r : raws) c.pdos[c.num_pdos++] = pd::decode_pdo(r);
    return c;
}
// A 67 W charger: 5 / 9 / 15 / 20 V and PPS 3.3-11 V.
pd::SourceCaps charger67() {
    return caps_of({fixed(5000, 3000), fixed(9000, 3000), fixed(15000, 3000), fixed(20000, 3350), pps(3300, 11000, 5500)});
}

void test_first_pdo_flags() {
    const SinkFlags f = sink_flags(kPixel);
    CHECK_EQ(f.valid, true);
    CHECK_EQ(f.dual_role_power, true);
    CHECK_EQ(f.higher_cap, true);
    CHECK_EQ(f.unconstrained, false);
    CHECK_EQ(f.usb_comm, true);
    CHECK_EQ(f.dual_role_data, true);
    CHECK_EQ(int(f.fr_swap), 0);
    StrBuf<34> l1, l2;
    sink_flags_lines(f, l1, l2);
    CHECK_STR(std::string(l1.view()).c_str(), "Dual-role DR data USB comm");
    CHECK_STR(std::string(l2.view()).c_str(), "Higher cap");

    const uint32_t all = fixed(5000, 3000) | 0x3F800000u;   // bits 29:23 set: DR pwr, higher, unconstrained, USB, DR data, FR 3 A
    const SinkFlags g = sink_flags(std::span<const uint32_t>(&all, 1));
    CHECK_EQ(g.unconstrained, true);
    CHECK_EQ(int(g.fr_swap), 3);
    sink_flags_lines(g, l1, l2);
    CHECK_STR(std::string(l2.view()).c_str(), "Unconstrained Higher cap FR3A");

    CHECK_EQ(sink_flags({}).valid, false);                       // no list
    const uint32_t bat = 0x40000000u | 0x3F800000u;               // the first PDO is a battery: no flags in it
    CHECK_EQ(sink_flags(std::span<const uint32_t>(&bat, 1)).valid, false);
}

void test_all_flag_combinations_fit() {
    static constexpr const char *names[] = {"Dual-role", "Higher cap", "Unconstrained", "USB comm", "DR data"};
    static constexpr const char *fr[] = {"", "FR USB", "FR1.5A", "FR3A"};
    for (unsigned mask = 0; mask < 32; ++mask) for (unsigned swap = 0; swap < 4; ++swap) {
        const uint32_t raw = fixed(5000, 3000) | (mask << 25) | (swap << 23);
        const auto flags = sink_flags(std::span<const uint32_t>(&raw, 1));
        StrBuf<34> a, b;
        sink_flags_lines(flags, a, b);
        CHECK_EQ(text_width(kLabelFont, a.view()) <= 218, true);
        CHECK_EQ(text_width(kLabelFont, b.view()) <= 218, true);
        const std::string text = std::string(a.view()) + " " + std::string(b.view());
        for (unsigned bit = 0; bit < 5; ++bit)
            CHECK_EQ(text.find(names[bit]) != std::string::npos, (mask & (1u << (4 - bit))) != 0);
        for (unsigned i = 1; i < 4; ++i)
            CHECK_EQ(text.find(fr[i]) != std::string::npos, swap == i);
    }
}

void test_pixel_against_67w_charger() {
    const WantsLayout w = wants_layout(charger67(), ~0u, kPixel, 2);
    CHECK_EQ(w.n, 5);
    CHECK_EQ(int(w.row[0].src), 0); CHECK_EQ(int(w.row[0].sink), 0);   // 5 V lines up
    CHECK_EQ(int(w.row[1].src), 1); CHECK_EQ(int(w.row[1].sink), 1);   // 9 V lines up
    CHECK_EQ(int(w.row[2].sink), -1);                                    // 15 V: the Pixel does not list it
    CHECK_EQ(int(w.row[3].sink), -1);
    CHECK_EQ(int(w.row[4].src), 4); CHECK_EQ(int(w.row[4].sink), 2);   // PPS 5-11 V sits in the PPS row
    CHECK_EQ(w.row[1].contract, true);                                   // contract = position 2, both columns
    CHECK_EQ(w.row[0].contract, false);
    CHECK_EQ(w.unmet, 0);
}

void test_unmet_sink_pdos_get_extra_rows() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), fixed(20000, 3000)});   // no 9 V, no PPS
    const WantsLayout w = wants_layout(c, ~0u, kPixel, 1);
    CHECK_EQ(w.n, 4);                              // 2 source rows + the 2 sink PDOs nothing can meet
    CHECK_EQ(int(w.row[0].sink), 0);
    CHECK_EQ(int(w.row[1].src), 1); CHECK_EQ(int(w.row[1].sink), -1);
    CHECK_EQ(int(w.row[2].src), -1); CHECK_EQ(int(w.row[2].sink), 1); CHECK_EQ(w.row[2].unmet, true);
    CHECK_EQ(int(w.row[3].sink), 2); CHECK_EQ(w.row[3].unmet, true);
    CHECK_EQ(w.unmet, 2);
}

void test_fixed_sink_uses_a_pps_range_and_gone_rows_do_not_count() {
    const pd::SourceCaps c = caps_of({fixed(5000, 3000), pps(3300, 11000, 3000)});   // no fixed 9 V: PPS covers it
    const WantsLayout w = wants_layout(c, ~0u, kPixel, 0);
    CHECK_EQ(int(w.row[1].src), 1); CHECK_EQ(int(w.row[1].sink), 1);   // 9 V placed on the PPS row
    CHECK_EQ(int(w.row[1].src) == 1 && w.row[1].unmet, false);
    CHECK_EQ(w.n, 3);                                                    // the PPS sink PDO finds its row taken: extra row, not unmet
    CHECK_EQ(int(w.row[2].sink), 2); CHECK_EQ(w.row[2].unmet, false);
    CHECK_EQ(w.unmet, 0);

    const WantsLayout g = wants_layout(c, 0x1u, kPixel, 0);   // the PPS offer is gone: 9 V and PPS have no source now
    CHECK_EQ(g.unmet, 2);
}

void test_pick_source_and_session() {
    pd::SinkCapsSeen sn;
    pd::IdStatus id;
    CHECK_EQ(int(pick_wants(sn, false, id).src), int(WantsSrc::None));
    id.ok_mask = 1u << int(pd::IdStep::SinkCap);
    id.num_sink_pdos = 3;
    for (int i = 0; i < 3; i++) id.sink_pdos[i] = kPixel[i];
    WantsList l = pick_wants(sn, true, id);
    CHECK_EQ(int(l.src), int(WantsSrc::Identify));
    CHECK_EQ(int(l.pdos.size()), 3);
    CHECK_EQ(l.pdos[2], 0xC0DC323Cu);
    CHECK_EQ(int(pick_wants(sn, false, id).src), int(WantsSrc::None));   // Identify ran in an earlier attach
    id.ok_mask = 0;                                                      // asked, not answered
    CHECK_EQ(int(pick_wants(sn, true, id).src), int(WantsSrc::None));
    id.ok_mask = 1u << int(pd::IdStep::SinkCap);
    sn.valid = true; sn.n = 1; sn.raw[0] = fixed(5000, 1500);
    l = pick_wants(sn, true, id);                                        // the source asked: what it saw wins
    CHECK_EQ(int(l.src), int(WantsSrc::Sniffed));
    CHECK_EQ(int(l.pdos.size()), 1);
    CHECK_STR(wants_src_name(WantsSrc::Sniffed), "sniffed");
    CHECK_STR(wants_src_name(WantsSrc::Identify), "from Identify");
}
}  // namespace

int main() {
    test_first_pdo_flags();
    test_all_flag_combinations_fit();
    test_pixel_against_67w_charger();
    test_unmet_sink_pdos_get_extra_rows();
    test_fixed_sink_uses_a_pps_range_and_gone_rows_do_not_count();
    test_pick_source_and_session();
    return test_report("test_ui_wants");
}
