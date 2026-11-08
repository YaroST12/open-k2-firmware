// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The main-loop scratch arena (k2/core/scratch.hpp): one holder at a time, released at scope end, a refused lease
// is reported to the violation handler, and emlib / the Black Box persister really draw from it.
#include <cstring>

#include "k2/pd/emlib.hpp"
#include "k2/core/scratch.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;

namespace {
int g_violations;
const char* g_want;
const char* g_held;

void on_violation(const scratch::Violation& v) {
    g_violations++;
    g_want = v.want;
    g_held = v.held;
}

struct Pair {
    std::uint8_t a[64];
    std::uint32_t b[16];
};
}  // namespace

int main() {
    scratch::set_violation_handler(on_violation);

    CHECK_EQ(scratch::held(), 0);
    {   // a lease takes and returns the arena; the bytes are usable and aligned
        scratch::Lease l("test.a");
        CHECK_EQ(scratch::held(), 1);
        CHECK_STR(scratch::owner(), "test.a");
        CHECK_EQ(reinterpret_cast<std::uintptr_t>(scratch::arena()) % scratch::kAlign, 0);
        auto& p = scratch::as<Pair>();
        std::memset(&p, 0x5A, sizeof p);
        CHECK_EQ(p.a[63], 0x5A);
        CHECK_EQ(g_violations, 0);
    }
    CHECK_EQ(scratch::held(), 0);
    CHECK_EQ(g_violations, 0);

    {   // make<T>() value-initialises over stale bytes (a type with default member initialisers)
        scratch::Lease l("test.make");
        std::memset(scratch::arena(), 0xEE, 64);
        struct Init { std::uint32_t a = 7; std::uint8_t b[16]; };
        auto& v = scratch::make<Init>();
        CHECK_EQ(v.a, 7);
        CHECK_EQ(v.b[15], 0);
    }

    {   // a second lease while one is held is a violation that names both tags
        scratch::Lease outer("test.outer");
        {
            scratch::Lease inner("test.inner");
            CHECK_EQ(g_violations, 1);
            CHECK_STR(g_want, "test.inner");
            CHECK_STR(g_held, "test.outer");
        }
    }
    CHECK_EQ(scratch::held(), 0);

    g_violations = 0;
    (void)scratch::arena();          // no lease: the arena may not be touched
    CHECK_EQ(g_violations, 1);
    CHECK_STR(g_want, "(no lease)");

    // emlib: save, rename, get and the duplicate test run without a violation and leave the arena free
    {
        g_violations = 0;
        k2test::RamDisk disk{64};
        fs::Fs fsys{disk, 64};
        bool fm = false;
        fsys.mount_or_format(fm);
        emlib::Library lib(fsys);
        const std::array<std::uint32_t, 5> v = {0xFF00A041, 0x18002B1Du, 0, 0x40510001u, 0x00000C43};
        const emrec::Record rec = emrec::make(v, 2, "x");
        const auto r = lib.save(rec);
        CHECK_EQ(int(r.err), int(emlib::Err::Ok));
        CHECK_EQ(lib.save(rec).existed, 1);
        CHECK_EQ(int(lib.rename(r.seq, "my cable")), int(emlib::Err::Ok));
        emlib::Entry e;
        CHECK_EQ(lib.get(r.seq, e), 1);
        CHECK_STR(e.name.data(), "my cable");
        CHECK_EQ(g_violations, 0);
        CHECK_EQ(scratch::held(), 0);
    }
    scratch::set_violation_handler(nullptr);
    return test_report("test_scratch");
}
