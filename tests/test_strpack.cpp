// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/strpack.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_calc.hpp"

#include <cstdio>
#include <cstring>

using namespace k2;

static int g_fail = 0;
static int g_misuse = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

int main() {
    // 1. Packed strings live inside k2_strpack and decode identically.
    const char *p1 = K2_P("Source_Capabilities not answered by the charger");
    const char *p2 = K2_P("No PD source on either port");
    const char *p3 = K2_P("Limit off: charger refuses");
    const char *plain = "Plain ASCII string";
    const char *utf8 = "20.0V \xC2\xB1" "5%";

    CHECK(strpack::is_packed(p1));
    CHECK(strpack::is_packed(p2));
    CHECK(strpack::is_packed(p3));
    CHECK(!strpack::is_packed(plain));
    CHECK(!strpack::is_packed(utf8));
    CHECK(!strpack::is_packed(nullptr));

    // Packed size should actually be smaller than plain for typical prose.
    CHECK(std::strlen(p1) < std::strlen("Source_Capabilities not answered by the charger"));
    CHECK(std::strlen(p2) < std::strlen("No PD source on either port"));

    char buf[128];
    CHECK(strpack::decode(p1, buf, sizeof(buf)) == std::strlen("Source_Capabilities not answered by the charger"));
    CHECK(std::strcmp(buf, "Source_Capabilities not answered by the charger") == 0);
    CHECK(strpack::decode(p2, buf, sizeof(buf)) == std::strlen("No PD source on either port"));
    CHECK(std::strcmp(buf, "No PD source on either port") == 0);
    CHECK(strpack::decode(p3, buf, sizeof(buf)) == std::strlen("Limit off: charger refuses"));
    CHECK(std::strcmp(buf, "Limit off: charger refuses") == 0);

    // 2. Truncated decode buffer stays NUL-terminated and returns full length.
    char small[8];
    CHECK(strpack::decode(p2, small, sizeof(small)) == std::strlen("No PD source on either port"));
    CHECK(std::strcmp(small, "No PD s") == 0);

    // 3. StrBuf::put handles both packed and plain transparently.
    StrBuf<64> sb;
    sb.put(p2).put(" / ").put(p3);
    CHECK(sb.view() == "No PD source on either port / Limit off: charger refuses");

    StrBuf<64> sb2;
    sb2.put(std::string_view(p1));
    CHECK(sb2.view() == "Source_Capabilities not answered by the charger");

    // 4. Compile-time width checks work on K2_P tables.
    static constexpr const char *kTbl[] = {
        K2_P("No PD source on either port"),
        K2_P("Power meter not found"),
        "Plain entry",
    };
    static_assert(all_fit_p(kTbl, 8, 27 * 8), "packed table should fit 27 cells");
    static_assert(!fits_px_p(8, K2_P("No PD source on either port"), 20 * 8), "too narrow must fail");

    // 5. Host guard: a sink gets a packed string whole, a raw seam (usb::log, QR text) never gets one.
    strpack::set_misuse_hook([](const char *) { g_misuse++; });
    CHECK(strpack::is_packed(std::string_view(p1)));
    CHECK(g_misuse == 0);
    (void)strpack::is_packed(std::string_view(p1, 2));   // cut: decodes the whole string anyway
    CHECK(g_misuse == 1);
    (void)strpack::is_packed(p1 + 1);                    // a pointer into the middle decodes garbage
    CHECK(g_misuse == 2);
    strpack::expect_plain("Plain ASCII string");
    CHECK(g_misuse == 2);
    strpack::expect_plain(p2);                           // packed pointer at a raw seam
    CHECK(g_misuse == 3);
    char copied[64];                                     // packed bytes copied out of the section
    std::strcpy(copied, p2);
    strpack::expect_plain(copied);
    CHECK(g_misuse == 4);

    if (g_fail) return 1;
    std::printf("test_strpack: PASS\n");
    return 0;
}
