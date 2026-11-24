// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/strpack.hpp"

namespace k2::strpack {

int Reader::next_rt() { return step(); }

std::size_t decode(const char *s, char *out, std::size_t cap) {
    Reader r(s);
    std::size_t n = 0;
    for (int b; (b = r.step()) >= 0; n++)
        if (n + 1 < cap) out[n] = static_cast<char>(b);
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return n;
}

std::string_view unpack_rt(std::string_view s, char *buf, std::size_t cap) {
    if (!is_packed(s)) return s;
    const std::size_t n = decode(s.data(), buf, cap);
    return {buf, n < cap ? n : cap - 1};
}

int cells_rt(const char *s) {
    Reader r(s);
    int n = 0;
    for (int b; (b = r.step()) >= 0;) n += (b & 0xC0) != 0x80;
    return n;
}

}  // namespace k2::strpack

#ifndef __arm__
extern "C" const char *k2_strpack_unpack_c(const char *s, char *buf, std::size_t cap) {
    if (!s || !k2::strpack::is_packed(s)) return s;
    k2::strpack::decode(s, buf, cap);
    return buf;
}
#endif
