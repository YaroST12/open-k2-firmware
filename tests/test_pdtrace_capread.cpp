// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remote capture read must terminate on any slot content and any host offset: a slot restored
// from flash is not validated, and the host passes back an arbitrary offset. A record length
// byte below the header size (0 included) used to spin the record walk forever.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "k2/pd/pdtrace.hpp"
using namespace k2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void on_alarm(int) {
    std::printf("FAIL capture read hung (record walk did not terminate)\n");
    std::fflush(stdout);
    std::_Exit(1);
}

int main() {
    std::signal(SIGALRM, on_alarm);
    alarm(5);
    std::uint8_t out[58];
    {   // corrupt slot: a zero length byte at the read offset
        const std::uint8_t data[16] = {0, 1, 2, 3, 4, 5, 6, 7};
        CHECK(pdtrace::restore_capture(pdtrace::Trigger::Contract, data, 0));
        const std::size_t n = pdtrace::remote(pdtrace::kOpCapRead, 0, out);
        CHECK(n == pdtrace::kReadHeader);   // no whole record to return
    }
    {   // valid record, host offset lands mid-record on a zero timestamp byte
        const std::uint8_t data[8] = {8, std::uint8_t(pdtrace::Type::Vbus), 0, 0, 0, 0, 0x88, 0x13};
        CHECK(pdtrace::restore_capture(pdtrace::Trigger::Contract, data, 0));
        for (unsigned k = 0; k < pdtrace::kCapSlots; k++)
            for (std::uint32_t off = 0; off < 8; off++)
                pdtrace::remote(pdtrace::kOpCapRead, (k << 16) | off, out);
    }
    alarm(0);
    std::printf("test_pdtrace_capread: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
