// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_cabler.hpp"
#include "k2/drivers/power_calc.hpp"

#include <span>

namespace k2 {

void cr_split_mohm(uint32_t mohm, StrBuf<8> &number, const char *&unit) {
    char t[16];
    cable_r::fmt_mohm(std::span<char>(t), mohm);
    int i = 0;
    for (; t[i] && t[i] != ' '; i++) number.put_char(t[i]);
    unit = mohm < 1000 ? "m\xCE\xA9" : "\xCE\xA9";
}

const char *cr_grade_pill(cable_r::Grade g) {
    switch (g) {
    case cable_r::Grade::Excellent: return "EXCEL";
    case cable_r::Grade::Good:      return "GOOD";
    case cable_r::Grade::Fair:      return "FAIR";
    case cable_r::Grade::Poor:      return "POOR";
    default:                        return "";
    }
}

namespace {
void put_mohm(StrBuf<16> &s, int32_t v) {
    StrBuf<8> n;
    const char *u;
    cr_split_mohm(static_cast<uint32_t>(v < 0 ? 0 : v), n, u);
    s.put(n.view()).put_char(' ').put(u);
}
}  // namespace

CrView cr_view(bool online, int32_t vbus_mv, int32_t ibus_ma, cable_r::Mode mode,
               const cable_r::Meter &m, const std::optional<cable_r::Kelvin> &kin) {
    CrView v;
    const cable_r::Kelvin *const k = kin ? &*kin : nullptr;   // cable_r::kelvin() already rejects "no fixture"
    if (!online) {
        v.meter_missing = true;
        v.foot.put("INA2xx not on I2C");
        v.foot_tone = CrTone::Error;
        return v;
    }
    const int32_t amag = k2::power::abs_sat(ibus_ma);
    if (mode == cable_r::Mode::Kelvin) {
        v.step[0].label = "LIVE";
        v.step[0].value.put_fixed(vbus_mv, 2).put("V ").put_abs_fixed(ibus_ma, 2).put("A");
        v.step[0].tone = CrTone::Ok;
        v.step[1].label = "SENSE";
        if (k) {
            v.step[1].value.put("+");
            put_mohm(v.step[1].value, k->pos_mohm);
            v.step[1].tone = CrTone::Ok;
        } else {
            v.step[1].value.put(amag < cable_r::kKelvinMinMa ? ">= 0.45A" : "bad fixture");
            v.step[1].tone = CrTone::Caution;
        }
        if (k) {
            v.has_result = true;
            cr_split_mohm(static_cast<uint32_t>(k->total_mohm), v.number, v.unit);
            v.grade = cable_r::grade_mohm(k->total_mohm);
            v.grade_pill = cr_grade_pill(v.grade);
            v.detail.put("GND leg ");
            StrBuf<16> g;
            put_mohm(g, k->gnd_mohm);
            v.detail.put(g.view());
            v.foot.put("Kelvin: no K2 path");
        } else {
            v.foot.put(amag < cable_r::kKelvinMinMa ? "Draw at least 0.45A" : "Sense wires not seen");
            v.foot_tone = CrTone::Caution;
        }
        return v;
    }
    const auto est = m.estimate();
    v.step[0].label = "IDLE";
    if (m.has_idle()) {
        v.step[0].value.put_fixed(m.idle_mv(), 2).put("V ").put_abs_fixed(m.idle_ma(), 2).put("A");
        v.step[0].tone = CrTone::Ok;
    } else {
        v.step[0].value.put(vbus_mv < cable_r::kPresentMv ? "no VBUS" : "need < 0.2A");
        v.step[0].tone = vbus_mv < cable_r::kPresentMv ? CrTone::Dim : CrTone::Caution;
    }
    v.step[1].label = "LOAD";
    if (est) {
        v.step[1].value.put_fixed(est->load_mv, 2).put("V ").put_abs_fixed(est->load_ma, 2).put("A");
        v.step[1].tone = CrTone::Ok;
    } else if (m.has_idle()) {
        v.step[1].value.put(">= 1.0A");
        v.step[1].tone = CrTone::Caution;
    } else {
        v.step[1].value.put("--");
    }
    if (est) {
        v.has_result = true;
        cr_split_mohm(est->mohm, v.number, v.unit);
        v.grade = cable_r::grade(est);
        v.grade_pill = cr_grade_pill(v.grade);
        const int32_t drop = est->idle_mv - est->load_mv;
        const int32_t mw = static_cast<int32_t>((int64_t(drop) * est->load_ma + 500) / 1000);   // mV * mA = uW; /1000 = mW
        v.detail.put_fixed(drop, 2).put("V drop, ").put_fixed((mw + 50) / 100 * 100, 1).put("W lost");
        v.foot.put("Passive two-point");
    } else {
        v.foot.put(m.has_idle() ? "Draw at least 1A" : "Unload, then add 1A");
        v.foot_tone = m.has_idle() ? CrTone::Caution : CrTone::Dim;
    }
    return v;
}

}  // namespace k2
