// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the log line formatter (k2::fmt::format).
#include <climits>
#include <cstdint>

#include "k2/core/fmt.hpp"
#include "test_util.h"

static char g_buf[64];

// Format into g_buf with capacity `cap`; returns the produced length.
template <typename... A>
static std::size_t fmt_cap(std::size_t cap, const char* f, A... args) {
    return k2::fmt::format({g_buf, cap}, f, args...);
}
template <typename... A>
static const char* fmt(const char* f, A... args) {
    fmt_cap(sizeof g_buf, f, args...);
    return g_buf;
}

static void test_plain_and_percent() {
    CHECK_STR(fmt("hello"), "hello");
    CHECK_STR(fmt("100%%"), "100%");
    CHECK_STR(fmt("50%"), "50%");   // lone trailing '%' is literal, as in the old formatter
    CHECK_STR(fmt("%q"), "q");      // unknown conversion prints its letter
}

static void test_decimal() {
    CHECK_STR(fmt("%d", 0), "0");
    CHECK_STR(fmt("%d", 42), "42");
    CHECK_STR(fmt("%d", -7), "-7");
    CHECK_STR(fmt("%d", INT_MIN), "-2147483648");
    CHECK_STR(fmt("%u", 4000000000u), "4000000000");
    CHECK_STR(fmt("[USB] gap=%dms isr=%d", 3, 1200), "[USB] gap=3ms isr=1200");
}

static void test_hex() {
    CHECK_STR(fmt("%X", 0u), "0");
    CHECK_STR(fmt("%X", 0xABCu), "ABC");
    CHECK_STR(fmt("%x", 255u), "FF");  // lower-case request still prints upper-case, as before
    CHECK_STR(fmt("%X", 0xDEADBEEFu), "DEADBEEF");
}

static void test_width_and_zero_pad() {
    CHECK_STR(fmt("%02X", 5u), "05");
    CHECK_STR(fmt("%04X", 0xABu), "00AB");
    CHECK_STR(fmt("%08X", 0xDEADBEEFu), "DEADBEEF");
    CHECK_STR(fmt("%08X", 1u), "00000001");
    CHECK_STR(fmt("%02d", 7), "07");
    CHECK_STR(fmt("%03d", -5), "-05");
    CHECK_STR(fmt("%5d", 42), "   42");
    CHECK_STR(fmt("%2d", 12345), "12345");  // width is a minimum
    // The old formatter did not consume the argument of "%02X"; the next %s then read an
    // integer as a pointer. Arguments must line up now.
    CHECK_STR(fmt("%02X:%s", 1u, "x"), "01:x");
}

static void test_strings_and_chars() {
    CHECK_STR(fmt("%s", "abc"), "abc");
    CHECK_STR(fmt("[%s]", ""), "[]");
    CHECK_STR(fmt("%s", static_cast<const char*>(nullptr)), "(null)");
    CHECK_STR(fmt("%c%c", 'o', 'k'), "ok");
    CHECK_STR(fmt("[PWR] detected %s id=%X\n", "INA226", 0x2260u), "[PWR] detected INA226 id=2260\n");
}

static void test_truncation() {
    CHECK_EQ(fmt_cap(8, "0123456789"), 7);
    CHECK_STR(g_buf, "0123456");
    CHECK_EQ(fmt_cap(4, "%d", 123456), 3);
    CHECK_STR(g_buf, "123");
    CHECK_EQ(fmt_cap(1, "abc"), 0);
    CHECK_STR(g_buf, "");
    CHECK_EQ(fmt_cap(0, "abc"), 0);  // no room even for the terminator: nothing written
}

int main() {
    test_plain_and_percent();
    test_decimal();
    test_hex();
    test_width_and_zero_pad();
    test_strings_and_chars();
    test_truncation();
    return test_report("test_fmt");
}
