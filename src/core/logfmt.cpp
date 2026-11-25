// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Compact log line encoder, see k2/core/log.hpp and k2/core/logfmt.hpp.
#include "k2/core/logfmt.hpp"

#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/core/strpack.hpp"

namespace k2::logfmt {

namespace {

struct Out {
    std::span<char> buf;
    std::size_t len = 0;
    K2_HOT void put(char c) { if (len + 2 < buf.size()) buf[len++] = c; }   // keeps room for '\n' and NUL
    void hex(std::uint32_t v) {
        char d[8];
        int n = 0;
        do { d[n++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v); // k2-plain: indexed digit table
        while (n) put(d[--n]);
    }
    K2_HOT void dec(std::uint32_t v) {
        char d[10];
        int n = 0;
        do { d[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (n) put(d[--n]);
    }
};

}  // namespace

std::size_t vencode(std::span<char> buf, std::uint16_t id, std::uint32_t sig, std::va_list ap) {
    if (buf.size() < 2) {
        if (!buf.empty()) buf[0] = '\0';
        return 0;
    }
    Out out{buf};
    for (char c : {'m', 's', 'g', '#'}) out.put(c);
    out.dec(id);
    for (; sig; sig >>= 2) {
        out.put(' ');
        switch (sig & 3) {
        case kSigned: {
            const int v = va_arg(ap, int);
            if (v < 0) out.put('-');
            out.dec(v < 0 ? 0u - static_cast<std::uint32_t>(v) : static_cast<std::uint32_t>(v));
            break;
        }
        case kUnsigned: out.hex(va_arg(ap, std::uint32_t)); break;
        default: {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)"; // k2-plain: the decoder fallback
            auto emit = [&](unsigned char c) {
                if (c <= ' ' || c == '%' || c >= 0x7F) {
                    out.put('%');
                    out.put("0123456789ABCDEF"[c >> 4]); // k2-plain: indexed digit table
                    out.put("0123456789ABCDEF"[c & 15]); // k2-plain: indexed digit table
                } else {
                    out.put(static_cast<char>(c));
                }
            };
            if (strpack::is_packed(s)) {
                strpack::Reader r(s);
                for (int ch; (ch = r.next()) >= 0;) emit(static_cast<unsigned char>(ch));
            } else {
                for (; *s; s++) emit(static_cast<unsigned char>(*s));
            }
            break;
        }
        }
    }
    buf[out.len++] = '\n';
    buf[out.len] = '\0';
    return out.len;
}

std::size_t encode(std::span<char> out, std::uint16_t id, std::uint32_t sig, ...) {
    std::va_list ap;
    va_start(ap, sig);
    const std::size_t n = vencode(out, id, sig, ap);
    va_end(ap);
    return n;
}

}  // namespace k2::logfmt
