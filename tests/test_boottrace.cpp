// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the boot power trace kept in the backup registers (include/k2/core/boottrace.hpp).
#include <cstring>

#include "k2/core/boottrace.hpp"
#include "test_util.h"

using namespace k2::boottrace;

int main() {
    // Quantisation: values come back rounded down to the step, clamped at full scale.
    Sample s{5012, 4980, 1650, 120, true};
    const Sample q = unpack(pack(s));
    CHECK_EQ(q.vbus_mv, 5012 / kVbusStepMv * kVbusStepMv);
    CHECK_EQ(q.aux_mv, 4980 / kAuxStepMv * kAuxStepMv);
    CHECK_EQ(q.cc1_mv, 1650 / kCcStepMv * kCcStepMv);
    CHECK_EQ(q.cc2_mv, 120 / kCcStepMv * kCcStepMv);
    CHECK_EQ(q.plus, 1);
    const Sample big = unpack(pack({60000, 60000, 60000, 60000, false}));
    CHECK_EQ(big.vbus_mv, 127 * kVbusStepMv);
    CHECK_EQ(big.cc1_mv, 15 * kCcStepMv);
    CHECK_EQ(big.plus, 0);
    CHECK_EQ(pack({0, 0, 0, 0, false}), 0);
    CHECK_EQ(pack(s) >> 24, 0);   // 24 bits only

    // Recorder: three samples, then wraps the 7-slot ring.
    Trace t;
    CHECK_EQ(t.count, 0);
    t.push({5000, 0, 0, 0, true});
    t.push({4000, 0, 0, 0, true});
    t.push({100, 0, 0, 0, false});
    CHECK_EQ(t.count, 3);
    CHECK_EQ(t.stored(), 3);
    CHECK_EQ(unpack(t.at(0)).vbus_mv, 100 / kVbusStepMv * kVbusStepMv);    // newest first
    CHECK_EQ(unpack(t.at(2)).vbus_mv, 5000 / kVbusStepMv * kVbusStepMv);
    for (int i = 0; i < 10; i++) t.push({std::uint16_t(160 * (7 + i)), 0, 0, 0, false});
    CHECK_EQ(t.count, 13);
    CHECK_EQ(t.stored(), kRing);
    CHECK_EQ(unpack(t.at(0)).vbus_mv, 160 * 16);
    CHECK_EQ(unpack(t.at(kRing - 1)).vbus_mv, 160 * 10);

    // Register image round trip keeps count, mode and the ring.
    t.mode = 2;
    const auto w = encode(t);
    CHECK_EQ(w[0], kMagic);
    const auto back = decode(w);
    CHECK_EQ(back.has_value(), true);
    if (back) {
        CHECK_EQ(back->count, 13);
        CHECK_EQ(back->mode, 2);
        CHECK_EQ(back->at(0), t.at(0));
        CHECK_EQ(back->at(kRing - 1), t.at(kRing - 1));
    }
    auto bad = w;
    bad[5] ^= 0x10;
    CHECK_EQ(decode(bad).has_value(), false);          // CRC
    bad = w;
    bad[0] = 0;
    CHECK_EQ(decode(bad).has_value(), false);          // magic
    CHECK_EQ(decode(std::array<std::uint16_t, kWords>{}).has_value(), false);  // erased registers

    // Log line text.
    char line[96];
    format_sample(line, sizeof line, {5000, 4950, 1600, 0, true}, 150);
    CHECK_STR(line, "t=150ms vbus=5000 aux=4950 plus=1 cc1=1600 cc2=0");
    format_sample(line, 12, {1, 2, 3, 4, false}, 5);   // truncates, NUL terminated
    CHECK_EQ(strlen(line), 11);
    return test_report("boottrace");
}
