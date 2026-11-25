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
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace k2::strpack {
namespace {
void abort_hook(const char *what) {
    std::fprintf(stderr, "strpack misuse: %s\n", what); // k2-plain: the decoder's own diagnostics
    std::abort();
}
MisuseHook g_hook = abort_hook;

bool valid_text(std::string_view s) {   // ASCII text, tabs/newlines and well-formed UTF-8 (packed codes are neither)
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<std::uint8_t>(s[i]);
        if (b < 0x80) {
            // \x01 / \x02 are the UI's check / cross marks (k2/ui/text.hpp), the only control bytes plain text carries
            if ((b < 0x20 && b > 0x02 && b != '\t' && b != '\n' && b != '\r') || b == 0x7F) return false;
            i++;
            continue;
        }
        const std::size_t len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC2 ? 2 : 0;
        if (!len || i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; k++)
            if ((static_cast<std::uint8_t>(s[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}
}  // namespace

void set_misuse_hook(MisuseHook hook) { g_hook = hook ? hook : abort_hook; }

void check_whole(const char *s, std::size_t n) {
    if (s != __start_k2_strpack && s[-1] != '\0') return g_hook("pointer into the middle of a packed string"); // k2-plain: the decoder's own diagnostics
    if (n != SIZE_MAX && n != std::strlen(s)) g_hook("packed string cut by a string_view length"); // k2-plain: the decoder's own diagnostics
}

void expect_plain(std::string_view s) {
    if (in_section(s.data())) return g_hook("packed string at a raw seam (decode it first)"); // k2-plain: the decoder's own diagnostics
    if (!valid_text(s)) g_hook("packed bytes copied out of their section"); // k2-plain: the decoder's own diagnostics
}

}  // namespace k2::strpack

extern "C" const char *k2_strpack_unpack_c(const char *s, char *buf, std::size_t cap) {
    if (!s || !k2::strpack::is_packed(s)) return s;
    k2::strpack::decode(s, buf, cap);
    return buf;
}
#endif
