// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/oem_watts.hpp"
#include "k2/core/math_opt.hpp"

namespace k2::oem_watts {
namespace ct = charger_test;
namespace {
using Out = step_label::TextOut;
}  // namespace

Text watts(std::uint32_t mw) {
    Text t;
    if (!mw) return t;
    Out o{t};
    const std::uint32_t dw = (mw + 50u) / 100u;   // tenths of a watt
    if (dw >= 1000u) {   // from 100 W whole watts: at most 5 characters, the width of the "3.25A" a grid bracket already carries
        o.num((mw + 500u) / 1000u);
    } else {
        o.num(dw / 10u);
        if (dw % 10u) { o.put('.'); o.put(char('0' + dw % 10u)); }
    }
    o.put('W');
    return t;
}
Text label(std::string_view word, std::uint32_t mw) {
    Text t;
    Out o{t};
    o.put(word);
    if (const Text w = watts(mw); w.len) { o.put(' '); o.put(w.view()); }
    return t;
}
std::uint32_t step_mw(const ct::Step& s) {
    unsigned ma = 0;
    if (s.algorithm == ct::Algorithm::Scp) ma = step_label::amps_ma(s);
    else if (s.algorithm == ct::Algorithm::Ufcs) ma = step_label::ufcs_amps_ma(s);
    return ma && s.mv ? math::mw_of(s.mv, ma) : 0u;
}
Text chip(const ct::Step& s) {
    Text t;
    const std::uint32_t mw = step_mw(s);
    if (!mw) return t;
    const Text p = step_label::params(s);   // "10V 2.25A": its volts are the text before the space
    const std::string_view v = p.view().substr(0, p.view().find(' '));
    Out o{t};
    o.put(v);
    o.put(' ');
    o.put(watts(mw).view());
    return t;
}

}  // namespace k2::oem_watts
