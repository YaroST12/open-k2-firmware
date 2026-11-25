// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_cabler.hpp"
#include "k2/core/strpack.hpp"
#include "k2/core/math_opt.hpp"
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
    case cable_r::Grade::Excellent: return K2_P("EXCEL");
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
// "19.74V 0.31A": the volts and the amps, which the page paints in their own colours.
void put_va(CrCard &c, int32_t mv, int32_t ma) {
    c.value.put_fixed(mv, 2).put("V ");
    c.amp_at = static_cast<uint8_t>(c.value.view().size());
    c.value.put_abs_fixed(ma, 2).put("A");
    c.tone = CrTone::Ok;
}
}  // namespace

CrView cr_view(bool online, int32_t vbus_mv, int32_t ibus_ma, cable_r::Mode mode,
               const cable_r::Meter &m, const std::optional<cable_r::Kelvin> &kin) {
    CrView v;
    const cable_r::Kelvin *const k = kin ? &*kin : nullptr;   // cable_r::kelvin() already rejects "no fixture"
    if (!online) {
        v.meter_missing = true;
        v.foot.put(K2_P("INA2xx not on I2C"));
        v.foot_tone = CrTone::Error;
        return v;
    }
    const int32_t amag = k2::power::abs_sat(ibus_ma);
    if (mode == cable_r::Mode::Kelvin) {
        v.step[0].label = "LIVE";
        put_va(v.step[0], vbus_mv, ibus_ma);
        v.step[1].label = K2_P("SENSE");
        if (k) {
            v.step[1].value.put("+");
            put_mohm(v.step[1].value, k->pos_mohm);
            v.step[1].tone = CrTone::Ok;
        } else {
            v.step[1].value.put(amag < cable_r::kKelvinMinMa ? K2_P(">= 0.45A") : K2_P("bad fixture"));
            v.step[1].tone = CrTone::Caution;
        }
        if (k) {
            v.has_result = true;
            cr_split_mohm(static_cast<uint32_t>(k->total_mohm), v.number, v.unit);
            v.grade = cable_r::grade_mohm(k->total_mohm);
            v.grade_pill = cr_grade_pill(v.grade);
            v.detail.put(K2_P("GND leg "));
            StrBuf<16> g;
            put_mohm(g, k->gnd_mohm);
            v.detail.put(g.view());
            v.foot.put(K2_P("Kelvin: no K2 path"));
        } else {
            v.foot.put(amag < cable_r::kKelvinMinMa ? K2_P("Draw at least 0.45A") : K2_P("Sense wires not seen"));
            v.foot_tone = CrTone::Caution;
        }
        return v;
    }
    const auto est = m.estimate();
    v.step[0].label = "IDLE";
    if (m.has_idle()) {
        put_va(v.step[0], m.idle_mv(), m.idle_ma());
    } else {
        v.step[0].value.put(vbus_mv < cable_r::kPresentMv ? K2_P("No VBUS") : K2_P("need < 0.2A"));
        v.step[0].tone = vbus_mv < cable_r::kPresentMv ? CrTone::Dim : CrTone::Caution;
    }
    v.step[1].label = "LOAD";
    if (est) {
        put_va(v.step[1], est->load_mv, est->load_ma);
    } else if (m.has_idle()) {
        v.step[1].value.put(K2_P(">= 1.0A"));
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
        const int64_t num = int64_t(drop) * est->load_ma + 500;
        const int32_t mw = math::div_i64_to_i32(drop < 0 ? int64_t(drop) * est->load_ma - 500 : num, 1000);   // mV * mA = uW; /1000 = mW
        v.detail.put_fixed(drop, 2).put(K2_P("V drop, ")).put_fixed((mw + 50) / 100 * 100, 1).put(K2_P("W lost"));
        v.foot.put(K2_P("Passive two-point"));
    } else {
        v.foot.put(m.has_idle() ? K2_P("Draw at least 1A") : K2_P("Unload, then add 1A"));
        v.foot_tone = m.has_idle() ? CrTone::Caution : CrTone::Dim;
    }
    return v;
}

}  // namespace k2
