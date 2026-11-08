// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Compact log lines: K2LOG("[TAG] fmt", args...) keeps the format string out of flash.
//
// On the firmware the format is looked up at compile time in the build-generated table
// (tools/logdb.py scans every K2LOG in src/ and include/, assigns stable 16-bit IDs in
// tools/logdb.json and writes build/gen/k2_logdb.hpp). The device emits the line
//     msg#<id> <arg> <arg>...\n
// with %d args in signed decimal, %u/%x/%X/%c args in unsigned hex and %s args with
// ' ', '%', control and non-ASCII bytes %XX-escaped. Host tools turn that back into the text
// (tools/logdb.py decode_line); an ID the table does not know stays "msg#<id> args".
//
// Host tests (no __arm__) and K2_LOG_TEXT builds format the text directly through log_fmt, so
// tests keep seeing the full line. The format is printf-checked in both builds.
#pragma once
#include <cstdint>
#include <string_view>

#include "k2/drivers/usb.hpp"

namespace k2::logfmt {

// Argument kinds, 2 bits per argument (argument i at bits 2i..2i+1), 0 ends the list.
enum : std::uint32_t { kEnd = 0, kSigned = 1, kUnsigned = 2, kString = 3 };

// Same conversion rules as k2::fmt::vformat: '%', optional digits, one conversion character.
consteval std::uint32_t signature(std::string_view f) {
    std::uint32_t sig = 0;
    int n = 0;
    for (std::size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%' || i + 1 >= f.size()) continue;
        i++;
        while (f[i] >= '0' && f[i] <= '9' && i + 1 < f.size()) i++;
        std::uint32_t k = kEnd;
        switch (f[i]) {
        case 'd': k = kSigned; break;
        case 'u': case 'x': case 'X': case 'c': k = kUnsigned; break;
        case 's': k = kString; break;
        default: break;
        }
        if (k != kEnd) sig |= k << (2 * n++);
    }
    return sig;
}

}  // namespace k2::logfmt

namespace k2::usb {
// Emits "msg#<id> args\n" through log(); `sig` is logfmt::signature of the format.
void log_packed(std::uint16_t id, std::uint32_t sig, ...);
// Never defined: only named inside sizeof, so the compiler checks the format and arguments.
int log_format_check(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}  // namespace k2::usb

#if defined(__arm__) && !defined(K2_LOG_TEXT)
#include "k2_logdb.hpp"   // generated: k2::logdb::id(fmt)

#define K2LOG(fmt, ...)                                                                            \
    ((void)sizeof(::k2::usb::log_format_check(fmt __VA_OPT__(, ) __VA_ARGS__)),                    \
     ::k2::usb::log_packed(::k2::logdb::id(fmt), ::k2::logfmt::signature(fmt) __VA_OPT__(, ) __VA_ARGS__))
#else
#define K2LOG(fmt, ...) ::k2::usb::log_fmt(fmt __VA_OPT__(, ) __VA_ARGS__)
#endif
