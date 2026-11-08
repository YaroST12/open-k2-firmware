// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the compact log line encoder (k2::logfmt, include/k2/core/log.hpp). The decoder side
// is tests/test_logdb.py; both use the same literal lines.
#include <cstdint>

#include "k2/core/log.hpp"
#include "k2/core/logfmt.hpp"
#include "test_util.h"

static char g_buf[64];

template <typename... A>
static const char* enc(std::uint16_t id, std::uint32_t sig, A... args) {
    k2::logfmt::encode(g_buf, id, sig, args...);
    return g_buf;
}

static void test_signature() {
    using namespace k2::logfmt;
    CHECK_EQ(signature("[X] plain\n"), 0u);
    CHECK_EQ(signature("[X] 100%% done\n"), 0u);
    CHECK_EQ(signature("[X] %d %u %s\n"), kSigned | kUnsigned << 2 | kString << 4);
    CHECK_EQ(signature("[X] %04X %c %5d"), kUnsigned | kUnsigned << 2 | kSigned << 4);
    CHECK_EQ(signature("[X] 50%"), 0u);   // a lone trailing '%' is text
}

static void test_encode() {
    using namespace k2::logfmt;
    CHECK_STR(enc(7, 0), "msg#7\n");
    CHECK_STR(enc(300, signature("%d %d"), -12, 5), "msg#300 -12 5\n");
    CHECK_STR(enc(1, signature("%u %X"), 4000000000u, 0xBEEFu), "msg#1 EE6B2800 BEEF\n");
    CHECK_STR(enc(2, signature("%s|%s"), "Anker 735", "50%\n"), "msg#2 Anker%20735 50%25%0A\n");
    CHECK_STR(enc(3, signature("%s"), static_cast<const char*>(nullptr)), "msg#3 (null)\n");
    CHECK_STR(enc(4, signature("%s"), ""), "msg#4 \n");
}

static void test_truncation() {
    // A line that does not fit is cut, but always ends with a newline.
    k2::logfmt::encode({g_buf, 12}, 9, k2::logfmt::signature("%s"), "abcdefghijkl");
    CHECK_STR(g_buf, "msg#9 abcd\n");
}

int main() {
    test_signature();
    test_encode();
    test_truncation();
    return test_report("test_logfmt");
}
