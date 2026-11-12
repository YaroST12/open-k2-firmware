// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cable R v2 view: card texts, units and the footer for each state of both modes.
#include <cstring>
#include "test_util.h"
#include "k2/ui/ui_cabler.hpp"

using namespace k2;

static cable_r::Meter metered(int idle_mv, int idle_ma, int load_mv, int load_ma) {
    cable_r::Meter m;
    m.sample(idle_mv, idle_ma);
    if (load_ma) m.sample(load_mv, -load_ma);
    return m;
}

int main() {
    {   // split
        StrBuf<8> n; const char *u;
        cr_split_mohm(110, n, u); CHECK_STR(n.c_str(), "110"); CHECK_STR(u, "m\xCE\xA9");
        n = {}; cr_split_mohm(1250, n, u); CHECK_STR(n.c_str(), "1.25"); CHECK_STR(u, "\xCE\xA9");
        n = {}; cr_split_mohm(12300, n, u); CHECK_STR(n.c_str(), "12.3");
    }
    {   // no meter
        const CrView v = cr_view(false, 0, 0, cable_r::Mode::Passive, cable_r::Meter{}, std::nullopt);
        CHECK_EQ(int(v.meter_missing), 1);
        CHECK_STR(v.foot.c_str(), "INA2xx not on I2C");
        CHECK_EQ(int(v.foot_tone == CrTone::Error), 1);
    }
    {   // waiting for the idle reference, with load on the bus
        const CrView v = cr_view(true, 5000, 900, cable_r::Mode::Passive, cable_r::Meter{}, std::nullopt);
        CHECK_STR(v.step[0].label, "IDLE");
        CHECK_STR(v.step[0].value.c_str(), "need < 0.2A");
        CHECK_STR(v.step[1].value.c_str(), "--");
        CHECK_STR(v.foot.c_str(), "Unload, then add 1A");
        CHECK_EQ(int(v.has_result), 0);
        const CrView n = cr_view(true, 0, 0, cable_r::Mode::Passive, cable_r::Meter{}, std::nullopt);
        CHECK_STR(n.step[0].value.c_str(), "No VBUS");
    }
    {   // idle taken, waiting for the load step
        const cable_r::Meter m = metered(20040, 50, 0, 0);
        const CrView v = cr_view(true, 20040, 50, cable_r::Mode::Passive, m, std::nullopt);
        CHECK_STR(v.step[0].value.c_str(), "20.04V 0.05A");
        CHECK_EQ(int(v.step[0].tone == CrTone::Ok), 1);
        CHECK_STR(v.step[1].value.c_str(), ">= 1.0A");
        CHECK_EQ(int(v.step[1].tone == CrTone::Caution), 1);
        CHECK_STR(v.foot.c_str(), "Draw at least 1A");
    }
    {   // result: 0.33 V over 3 A
        const cable_r::Meter m = metered(20040, 50, 19710, 3010);
        const CrView v = cr_view(true, 19710, -3010, cable_r::Mode::Passive, m, std::nullopt);
        CHECK_EQ(int(v.has_result), 1);
        CHECK_STR(v.step[1].value.c_str(), "19.71V 3.01A");
        CHECK_STR(v.number.c_str(), "111");
        CHECK_STR(v.unit, "m\xCE\xA9");
        CHECK_STR(v.grade_pill, "EXCEL");
        CHECK_STR(v.detail.c_str(), "0.33V drop, 1.0W lost");
        CHECK_STR(v.foot.c_str(), "Passive two-point");
        CHECK_EQ(int(v.foot_tone == CrTone::Dim), 1);
    }
    {   // kelvin
        const CrView w = cr_view(true, 5000, 100, cable_r::Mode::Kelvin, cable_r::Meter{}, std::nullopt);
        CHECK_STR(w.step[1].label, "SENSE");
        CHECK_STR(w.step[1].value.c_str(), ">= 0.45A");
        const auto k = cable_r::kelvin(100, 400, 250, 2000);
        CHECK_EQ(int(k.has_value()), 1);
        const CrView v = cr_view(true, 5000, 2000, cable_r::Mode::Kelvin, cable_r::Meter{}, k);
        CHECK_EQ(int(v.has_result), 1);
        CHECK_STR(v.number.c_str(), "150");
        CHECK_STR(v.foot.c_str(), "Kelvin: no K2 path");
        const CrView f = cr_view(true, 5000, 2000, cable_r::Mode::Kelvin, cable_r::Meter{}, std::nullopt);
        CHECK_STR(f.step[1].value.c_str(), "bad fixture");
    }
    return test_report("test_ui_cabler");
}
