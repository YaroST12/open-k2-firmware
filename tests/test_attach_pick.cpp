// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/attach_pick.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

Pdo fixed(std::uint16_t mv, std::uint16_t ma) {
    Pdo p;
    p.type = PdoType::Fixed;
    p.raw = 1;
    p.min_mv = p.max_mv = mv;
    p.max_ma = ma;
    return p;
}
Pdo range(PdoType t, std::uint16_t lo, std::uint16_t hi, std::uint16_t ma) {
    Pdo p;
    p.type = t;
    p.raw = 1;
    p.min_mv = lo;
    p.max_mv = hi;
    p.max_ma = ma;
    return p;
}

// NOVOO RG100 as seen 2026-11-11: 5/9/15 V 3 A, 20 V 5 A, PPS 5-21 V 5 A.
SourceCaps novoo() {
    SourceCaps c;
    c.valid = true;
    c.num_pdos = 5;
    c.pdos[0] = fixed(5000, 3000);
    c.pdos[1] = fixed(9000, 3000);
    c.pdos[2] = fixed(15000, 3000);
    c.pdos[3] = fixed(20000, 5000);
    c.pdos[4] = range(PdoType::Pps, 5000, 21000, 5000);
    return c;
}

}  // namespace

int main() {
    const SourceCaps n = novoo();
    {   // the hardware case: 5 V is fixed PDO 1, never the PPS range
        const Choice c = pick_attach_pdo(n, 5000, 0, 36000);
        CHECK_EQ(int(c.pos), 1);
        CHECK_EQ(int(c.mv), 0);
        CHECK_EQ(int(c.ma), 0);
    }
    {   // exact fixed levels win at every voltage the source names
        CHECK_EQ(int(pick_attach_pdo(n, 9000, 0, 36000).pos), 2);
        CHECK_EQ(int(pick_attach_pdo(n, 15000, 0, 36000).pos), 3);
        CHECK_EQ(int(pick_attach_pdo(n, 20000, 2000, 36000).pos), 4);
        CHECK_EQ(int(pick_attach_pdo(n, 20000, 2000, 36000).ma), 2000);
    }
    {   // a voltage no fixed PDO names goes to the range, at the target
        const Choice c = pick_attach_pdo(n, 12000, 1500, 36000);
        CHECK_EQ(int(c.pos), 5);
        CHECK_EQ(int(c.mv), 12000);
        CHECK_EQ(int(c.ma), 1500);
    }
    {   // no range: the lowest fixed within 1 V above, else the highest below
        SourceCaps c = n;
        c.num_pdos = 4;
        CHECK_EQ(int(pick_attach_pdo(c, 8500, 0, 36000).pos), 2);    // 9 V is 0.5 V above
        CHECK_EQ(int(pick_attach_pdo(c, 12000, 0, 36000).pos), 2);   // 15 V is 3 V above: highest below is 9 V
        CHECK_EQ(int(pick_attach_pdo(c, 4000, 0, 36000).pos), 1);    // 5 V is 1 V above
    }
    {   // the monitor ceiling: a 28 V target on a 20 V-capped monitor is clamped, the 20 V fixed PDO answers
        CHECK_EQ(int(pick_attach_pdo(n, 28000, 0, 20000).pos), 4);
    }
    {   // AVS range on an EPR list, no fixed level at the target
        SourceCaps c = n;
        c.num_pdos = 6;
        c.pdos[4] = fixed(28000, 5000);
        c.pdos[5] = range(PdoType::Avs, 15000, 28000, 5000);
        const Choice a = pick_attach_pdo(c, 24000, 0, 36000);
        CHECK_EQ(int(a.pos), 6);
        CHECK_EQ(int(a.mv), 24000);
        CHECK_EQ(int(pick_attach_pdo(c, 28000, 0, 36000).pos), 5);  // the fixed 28 V wins over AVS
    }
    {   // no list, no monitor: nothing
        SourceCaps c;
        CHECK_EQ(int(pick_attach_pdo(c, 5000, 0, 36000).pos), 0);
        CHECK_EQ(int(pick_attach_pdo(n, 5000, 0, 0).pos), 0);
    }
    return test_report("attach_pick");
}
