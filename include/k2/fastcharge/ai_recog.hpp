// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// "AI..." charger-recognition banner, pure logic (host-tested in tests/test_ai_recog.cpp).
// The "AI" banner is a heuristic summariser, not a neural network:
// with live PD capabilities it prints the highest wattage and a FIX / PPS / AVS breakdown; without
// them it shows the cached D+/D- verdict, and "AI." -> "AI.." -> "AI..." while the verdict is still
// being worked out. The input is the FUSB302's Source_Capabilities and the k2::fastcharge
// classifier verdict, and nothing is driven. The active auto-detector is the fast-charge scan
// (k2::fctrig) and stays on the Trigger tool.
// Not yet covered: the Xiaomi "MI " prefix (needs the Xiaomi VDM flags).
#pragma once
#include <cstdint>

#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/pd/pd.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2::ai {

enum class Source : std::uint8_t { Searching, None, Pd, Legacy };

struct Banner {
    Source source = Source::None;
    StrBuf<24> line1;   // headline: "PD3.1+ 140W", "QC2.0 9V", "AI..."
    StrBuf<24> line2;   // breakdown: "FIX:100W PPS:63W", empty when there is none
};

inline constexpr std::uint32_t kDotMs = 400;   // one step of the "AI." animation

// Highest wattage per PDO class, in whole watts (rounded down).
struct PdPower {
    std::uint32_t fixed_w = 0;       // SPR fixed offers
    std::uint32_t epr_fixed_w = 0;   // fixed offers in an observed EPR table
    std::uint32_t pps_w = 0;
    std::uint32_t avs_w = 0;         // advertised SPR or EPR AVS power
    std::uint32_t max_w() const {
        std::uint32_t m = fixed_w;
        for (std::uint32_t v : {epr_fixed_w, pps_w, avs_w}) m = v > m ? v : m;
        return m;
    }
};

inline PdPower summarize(const pd::SourceCaps &c) {
    PdPower p;
    for (unsigned i = 0; i < c.num_pdos && i < c.pdos.size(); i++) {
        const pd::Pdo &d = c.pdos[i];
        const std::uint32_t w = d.max_mw / 1000;
        std::uint32_t *slot = nullptr;
        switch (d.type) {
        case pd::PdoType::Fixed: slot = c.epr && i >= 7 ? &p.epr_fixed_w : &p.fixed_w; break;
        case pd::PdoType::Pps:   slot = &p.pps_w; break;
        case pd::PdoType::Avs:   slot = &p.avs_w; break;
        default: break;
        }
        if (slot && w > *slot) *slot = w;
    }
    return p;
}

inline Banner recognise(fastcharge::Protocol p, const pd::SourceCaps &caps, std::uint32_t now_ms) {
    using fastcharge::Protocol;
    Banner b;
    if (p == Protocol::NoInput) {
        b.source = Source::None;
        b.line1.put(fastcharge::name(p));
        return b;
    }
    if (p == Protocol::Pending) {
        b.source = Source::Searching;
        b.line1.put("AI");
        for (std::uint32_t i = 0; i <= (now_ms / kDotMs) % 3; i++) b.line1.put_char('.');
        return b;
    }
    if (caps.valid && caps.num_pdos > 0) {   // live PD capabilities win over any D+/D- reading
        const PdPower w = summarize(caps);
        const std::uint32_t top = w.max_w();
        bool epr = false, spr_avs = false;
        for (unsigned i = 0; i < caps.num_pdos && i < caps.pdos.size(); ++i) {
            const auto& d = caps.pdos[i];
            spr_avs |= caps.spec_rev == 2 && i < 7 && d.type == pd::PdoType::Avs && (d.raw >> 28) == 0xE;
            epr |= caps.spec_rev == 2 && caps.epr && i >= 7 &&
                (d.type == pd::PdoType::Fixed || (d.type == pd::PdoType::Avs && (d.raw >> 28) == 0xD));
        }
        b.source = Source::Pd;
        b.line1.put(spr_avs ? "PD3.2+" : epr ? "PD3.1+" : pd::wire_revision_name(caps.spec_rev))
            .put_char(' ').put_u32(top).put_char('W');
        if (w.epr_fixed_w == 0 && w.avs_w > 0) {   // AVS without EPR fixed: show the SPR fixed ceiling next to it
            b.line2.put("FIX:").put_u32(w.fixed_w).put("W AVS:").put_u32(w.avs_w).put_char('W');
        } else if (epr) {
            b.line2.put("EPR:").put_u32(w.epr_fixed_w).put("W AVS:").put_u32(w.avs_w).put_char('W');
        } else {
            b.line2.put("FIX:").put_u32(w.fixed_w).put_char('W');
            if (w.pps_w > 0) b.line2.put(" PPS:").put_u32(w.pps_w).put_char('W');
        }
        return b;
    }
    b.source = Source::Legacy;
    b.line1.put(fastcharge::name(p));
    return b;
}

// Home status-bar text (8 characters): the animated "AI..." while the verdict is pending, else the
// classifier's short name.
inline const char *home_text(fastcharge::Protocol p, std::uint32_t now_ms) {
    if (p != fastcharge::Protocol::Pending) return fastcharge::short_name(p);
    switch ((now_ms / kDotMs) % 3) {
    case 0:  return "AI.";
    case 1:  return "AI..";
    default: return "AI...";
    }
}

}  // namespace k2::ai
