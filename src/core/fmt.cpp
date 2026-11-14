// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Log line formatter, see k2/core/fmt.hpp.
#include "k2/core/fmt.hpp"

#include <cstdint>

#include "k2/core/hot.h"

namespace k2::fmt {

namespace {

// Bounded output cursor: silently drops what does not fit, keeps one byte for the NUL.
struct Out {
    char* cur;
    char* end;  // one before the last byte of the buffer: that byte is the NUL

    [[gnu::always_inline]] void put(char c) { if (cur < end) *cur++ = c; }
    [[gnu::always_inline]] void pad(char c, int n) { while (n-- > 0) put(c); }
};

// Integer conversion with optional sign, zero or space padding to `width`.
K2_HOT void put_number(Out& out, std::uint32_t mag, bool negative, unsigned base, int width, bool zero) {
    char digits[10];  // 4294967295 is the longest
    int n = 0;
    do {
        const unsigned d = mag % base;
        digits[n++] = static_cast<char>(d < 10 ? '0' + d : 'A' + d - 10);
        mag /= base;
    } while (mag);

    const int total = n + (negative ? 1 : 0);
    if (!zero) out.pad(' ', width - total);
    if (negative) out.put('-');
    if (zero) out.pad('0', width - total);
    while (n > 0) out.put(digits[--n]);
}

}  // namespace

K2_HOT std::size_t vformat(std::span<char> buf, const char* fmt, std::va_list ap) {
    if (buf.empty()) return 0;
    Out out{buf.data(), buf.data() + buf.size() - 1};
    for (const char* p = fmt; *p; p++) {
        if (*p != '%' || !p[1]) {  // plain text, or a lone '%' at the end of the string
            out.put(*p);
            continue;
        }
        p++;
        const bool zero = *p == '0';
        int width = 0;
        while (*p >= '0' && *p <= '9' && p[1]) width = width * 10 + (*p++ - '0');

        switch (*p) {
        case 'd': {
            const int v = va_arg(ap, int);
            put_number(out, v < 0 ? 0u - static_cast<std::uint32_t>(v) : static_cast<std::uint32_t>(v), v < 0, 10, width, zero);
            break;
        }
        case 'u': put_number(out, va_arg(ap, std::uint32_t), false, 10, width, zero); break;
        case 'x':
        case 'X': put_number(out, va_arg(ap, std::uint32_t), false, 16, width, zero); break;
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            while (*s) out.put(*s++);
            break;
        }
        case 'c': out.put(static_cast<char>(va_arg(ap, int))); break;
        default: out.put(*p); break;  // "%%" and unknown conversions
        }
    }
    *out.cur = '\0';
    return static_cast<std::size_t>(out.cur - buf.data());
}

K2_HOT std::size_t format(std::span<char> out, const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    const std::size_t n = vformat(out, fmt, ap);
    va_end(ap);
    return n;
}

}  // namespace k2::fmt
