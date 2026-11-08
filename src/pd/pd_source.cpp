// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Debug override for the source capabilities, see k2/pd/pd_source.hpp.
#include "k2/pd/pd_source.hpp"

namespace k2::pd {

namespace {

SourceCaps g_fake;
bool g_fake_on;

struct Fake {
    PdoType type;
    std::uint16_t min_mv, max_mv, ma;
};
// One of each layout the PD page has to draw; the order puts the fixed supplies first as a
// real source does.
constexpr std::array<Fake, kMaxPdos> kFakes{{
    {PdoType::Fixed, 5000, 5000, 3000},
    {PdoType::Fixed, 9000, 9000, 3000},
    {PdoType::Fixed, 15000, 15000, 3000},
    {PdoType::Fixed, 20000, 20000, 5000},
    {PdoType::Pps, 3300, 11000, 5000},
    {PdoType::Pps, 3300, 21000, 5000},
    {PdoType::Avs, 15000, 28000, 5000},
    {PdoType::Fixed, 28000, 28000, 5000},  // EPR positions 8 to 11
    {PdoType::Fixed, 36000, 36000, 5000},
    {PdoType::Fixed, 48000, 48000, 5000},
    {PdoType::Avs, 15000, 48000, 5000},
}};

}  // namespace

void fake_fill(SourceCaps& out, unsigned n) {
    if (n > kMaxPdos) n = kMaxPdos;
    out = {};
    out.valid = true;
    out.num_pdos = static_cast<std::uint8_t>(n);
    out.spec_rev = 2;
    for (unsigned i = 0; i < n; i++) {
        const Fake& f = kFakes[i];
        out.pdos[i] = {f.type, 0, f.min_mv, f.max_mv, f.ma, static_cast<std::uint32_t>(f.max_mv) * f.ma / 1000};
    }
}

void set_fake(unsigned n) {
    g_fake_on = n != 0;
    if (g_fake_on) fake_fill(g_fake, n);
}

bool is_fake() { return g_fake_on; }

const SourceCaps& active_caps() { return g_fake_on ? g_fake : fusb302::caps(); }

}  // namespace k2::pd
