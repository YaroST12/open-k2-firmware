// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// "AI..." charger-recognition banner (k2::ai): the passive gen-2 summary built from the PD source
// capabilities and the D+/D- classifier verdict. Expected strings are the printf formats filled in by hand.
#include "test_util.h"
#include "k2/fastcharge/ai_recog.hpp"

using namespace k2;
using namespace k2::ai;
using k2::fastcharge::Protocol;

static pd::SourceCaps caps_of(std::initializer_list<pd::Pdo> l, std::uint8_t rev = 2) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = rev;
    for (const auto &p : l) c.pdos[c.num_pdos++] = p;
    return c;
}
static pd::Pdo fixed(unsigned mv, unsigned ma) {
    return {pd::PdoType::Fixed, 0, static_cast<std::uint16_t>(mv), static_cast<std::uint16_t>(mv),
            static_cast<std::uint16_t>(ma), mv * ma / 1000};
}
static pd::Pdo pps(unsigned min_mv, unsigned max_mv, unsigned ma) {
    return {pd::PdoType::Pps, 0, static_cast<std::uint16_t>(min_mv), static_cast<std::uint16_t>(max_mv),
            static_cast<std::uint16_t>(ma), max_mv * ma / 1000};
}
static pd::Pdo avs(unsigned max_mv, unsigned ma, bool spr = false) {
    return {pd::PdoType::Avs, spr ? 0xE0000000u : 0xD0000000u, std::uint16_t(spr ? 9000 : 15000),
            static_cast<std::uint16_t>(max_mv), static_cast<std::uint16_t>(ma), max_mv * ma / 1000};
}

static void test_searching() {
    pd::SourceCaps none;
    // While the classifier window fills, the widget animates AI. -> AI.. -> AI... every 400 ms.
    CHECK_STR(recognise(Protocol::Pending, none, 0).line1.c_str(), "AI.");
    CHECK_STR(recognise(Protocol::Pending, none, 399).line1.c_str(), "AI.");
    CHECK_STR(recognise(Protocol::Pending, none, 400).line1.c_str(), "AI..");
    CHECK_STR(recognise(Protocol::Pending, none, 800).line1.c_str(), "AI...");
    CHECK_STR(recognise(Protocol::Pending, none, 1200).line1.c_str(), "AI.");
    CHECK_EQ(static_cast<int>(recognise(Protocol::Pending, none, 0).source), static_cast<int>(Source::Searching));
    CHECK_STR(recognise(Protocol::Pending, none, 0).line2.c_str(), "");

    CHECK_STR(home_text(Protocol::Pending, 800), "AI...");
    CHECK_STR(home_text(Protocol::Qc2_9V, 800), "QC2 9V");     // a verdict: the existing short name
    CHECK_STR(home_text(Protocol::NoInput, 800), "NO INPUT");
}

static void test_pd_paths() {
    // Wattage and PPS do not prove a minor revision.
    auto c = caps_of({fixed(5000, 3000), fixed(20000, 5000), pps(3300, 21000, 3000)});   // PPS 63 W
    Banner b = recognise(Protocol::Pd, c, 0);
    CHECK_EQ(static_cast<int>(b.source), static_cast<int>(Source::Pd));
    CHECK_STR(b.line1.c_str(), "PD3.x 100W");
    CHECK_STR(b.line2.c_str(), "FIX:100W PPS:63W");

    // Fixed only.
    c = caps_of({fixed(5000, 3000), fixed(9000, 3000)});
    b = recognise(Protocol::Pd, c, 0);
    CHECK_STR(b.line1.c_str(), "PD3.x 27W");
    CHECK_STR(b.line2.c_str(), "FIX:27W");

    // PD 2.0 header.
    c = caps_of({fixed(5000, 3000), fixed(12000, 3000)}, 1);
    CHECK_STR(recognise(Protocol::Pd, c, 0).line1.c_str(), "PD2.0 36W");

    // EPR positions prove an advertised 3.1+ feature, not exactly revision 3.1.
    c = caps_of({fixed(5000, 3000), fixed(20000, 5000)});
    c.epr = true; c.num_pdos = 8; c.pdos[7] = fixed(28000, 5000);
    b = recognise(Protocol::Pd, c, 0);
    CHECK_STR(b.line1.c_str(), "PD3.1+ 140W");
    CHECK_STR(b.line2.c_str(), "EPR:140W AVS:0W");

    // EPR AVS existed in 3.1; it is not evidence of SPR AVS or 3.2.
    c.num_pdos = 10; c.pdos[8] = fixed(48000, 5000); c.pdos[9] = avs(48000, 5000);
    b = recognise(Protocol::Pd, c, 0);
    CHECK_STR(b.line1.c_str(), "PD3.1+ 240W");
    CHECK_STR(b.line2.c_str(), "EPR:240W AVS:240W");

    // EPR AVS in an SPR slot cannot prove a revision/feature.
    c = caps_of({fixed(5000, 3000), fixed(20000, 5000), avs(28000, 5000)});
    b = recognise(Protocol::Pd, c, 0);
    CHECK_STR(b.line1.c_str(), "PD3.x 140W");
    CHECK_STR(b.line2.c_str(), "FIX:100W AVS:140W");

    // Live caps win over a legacy-looking D+/D- verdict (a PD sink's data pair is just USB idle).
    c = caps_of({fixed(5000, 3000), fixed(9000, 2000)});
    CHECK_STR(recognise(Protocol::UsbData, c, 0).line1.c_str(), "PD3.x 18W");
    // ... except with no VBUS at all.
    CHECK_STR(recognise(Protocol::NoInput, c, 0).line1.c_str(), "NO INPUT");
    CHECK_EQ(static_cast<int>(recognise(Protocol::NoInput, c, 0).source), static_cast<int>(Source::None));

    c = caps_of({fixed(5000, 3000), fixed(20000, 6000)});
    CHECK_STR(recognise(Protocol::Pd, c, 0).line1.c_str(), "PD3.x 120W");
    c = caps_of({fixed(5000, 3000), fixed(20000, 5000), avs(20000, 5000, true)});
    CHECK_STR(recognise(Protocol::Pd, c, 0).line1.c_str(), "PD3.2+ 100W");
    c.spec_rev = 1;
    CHECK_STR(recognise(Protocol::Pd, c, 0).line1.c_str(), "PD2.0 100W");
    c.spec_rev = 3;
    CHECK_STR(recognise(Protocol::Pd, c, 0).line1.c_str(), "reserved 100W");
}

static void test_legacy_paths() {
    pd::SourceCaps none;
    Banner b = recognise(Protocol::Qc2_9V, none, 0);
    CHECK_EQ(static_cast<int>(b.source), static_cast<int>(Source::Legacy));
    CHECK_STR(b.line1.c_str(), "QC2.0 9V");
    CHECK_STR(b.line2.c_str(), "");
    CHECK_STR(recognise(Protocol::Apple24, none, 0).line1.c_str(), "APPLE 2.4A");
    CHECK_STR(recognise(Protocol::Qc3, none, 0).line1.c_str(), "QC3.0");
    // A PD-looking source whose caps were not seen yet: the classifier's own "PD?" is shown as is.
    CHECK_STR(recognise(Protocol::PdLikely, none, 0).line1.c_str(), "PD?");
    // Caps flagged valid but empty do not count as live caps.
    pd::SourceCaps empty; empty.valid = true;
    CHECK_STR(recognise(Protocol::NoQuick, empty, 0).line1.c_str(), "NO QUICK");
}

static void test_width() {
    // Both lines must fit the 22 character content width of the small font.
    auto c = caps_of({fixed(5000, 3000), fixed(20000, 5000), fixed(28000, 5000), fixed(48000, 5000), avs(48000, 5000)});
    Banner b = recognise(Protocol::Pd, c, 0);
    CHECK_EQ(b.line1.view().size() <= 22, 1);
    CHECK_EQ(b.line2.view().size() <= 22, 1);
}

int main() {
    test_searching();
    test_pd_paths();
    test_legacy_paths();
    test_width();
    return test_report("test_ai_recog");
}
