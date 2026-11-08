// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the PD session record kept in the backup registers across a reset, and the
// reset-cause pick.
#include <cstring>

#define K2_HOST_TEST 1
#include "k2/chargertest/session.hpp"
#include "k2/drivers/sysreset.hpp"
#include "test_util.h"

using namespace k2;

static session::Record sample() {
    session::Record r;
    r.boots = 7;
    r.cc = 2;
    r.caps_valid = true;
    r.contract_valid = true;
    r.contract_ready = true;
    r.identify_running = true;
    r.id_state = 1;
    r.id_step = 3;
    r.id_ok_mask = 0x07;
    r.pos = 2;
    r.mv = 9000;
    r.op_ma = 3000;
    r.max_ma = 3000;
    r.num_pdos = 5;
    r.spec_rev = 2;
    r.pdo = {0x0801912C, 0x0002D12C, 0xDEADBEEF};
    return r;
}

int main() {
    // round trip keeps every field
    const session::Record r = sample();
    const auto w = session::encode(r);
    CHECK_EQ(w[0], session::kMagic);
    const auto back = session::decode(w);
    CHECK_EQ(back.has_value(), true);
    if (back) CHECK_EQ(*back == r, true);

    // blank backup domain (all zero) and erased flash-like all-ones are not a record
    std::array<std::uint16_t, session::kWords> z{}, f{};
    f.fill(0xFFFF);
    CHECK_EQ(session::decode(z).has_value(), false);
    CHECK_EQ(session::decode(f).has_value(), false);

    // any single flipped bit in the payload is caught by the CRC
    for (std::size_t i = 1; i < session::kWords; i++) {
        for (int b = 0; b < 16; b++) {
            auto bad = w;
            bad[i] ^= std::uint16_t(1u << b);
            CHECK_EQ(session::decode(bad).has_value(), false);
        }
    }
    // wrong magic with a valid CRC is refused (a foreign writer)
    auto foreign = w;
    foreign[0] = 0x5044;  // "PD"
    CHECK_EQ(session::decode(foreign).has_value(), false);

    // an empty record still round-trips (no partner at the last save)
    const session::Record e;
    const auto ed = session::decode(session::encode(e));
    CHECK_EQ(ed.has_value(), true);
    if (ed) CHECK_EQ(*ed == e, true);

    // summary line: what the boot log prints about the previous session
    char buf[96];
    session::summarize(r, buf, sizeof buf);
    CHECK_STR(buf, "cc=2 caps=5 pos=2 9000mV 3000mA ready id=run:Sink caps ok=07");
    session::summarize(e, buf, sizeof buf);
    CHECK_STR(buf, "cc=0 caps=0 no-contract id=idle");
    char tiny[8];
    session::summarize(r, tiny, sizeof tiny);
    CHECK_EQ(tiny[7], 0);

    // reset cause: PIN is set by every reset, so the more specific flag wins
    using sysreset::Cause;
    CHECK_STR(sysreset::cause_name(sysreset::cause(0x0C000000u)), "POR");   // PIN|POR
    CHECK_STR(sysreset::cause_name(sysreset::cause(0x04000000u)), "PIN");
    CHECK_STR(sysreset::cause_name(sysreset::cause(0x14000000u)), "SW");    // PIN|SW
    CHECK_STR(sysreset::cause_name(sysreset::cause(0x24000000u)), "IWDG");  // PIN|IWDG
    CHECK_STR(sysreset::cause_name(sysreset::cause(0x44000000u)), "WWDG");
    CHECK_STR(sysreset::cause_name(sysreset::cause(0x84000000u)), "LPWR");
    CHECK_STR(sysreset::cause_name(sysreset::cause(0x3C000000u)), "IWDG");  // newest-worst: IWDG over SW over POR
    CHECK_STR(sysreset::cause_name(sysreset::cause(0)), "none");
    return test_report("test_session");
}
