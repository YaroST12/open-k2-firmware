// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Size-optimized replacements for non-hot Newlib string/memory functions on ARM firmware.
// memcpy, memmove, and memset are intentionally left to Newlib's fast implementations.
#if defined(__arm__) && !defined(__unix__)

#include <cstddef>

#pragma GCC push_options
#pragma GCC optimize("Os", "no-tree-loop-distribute-patterns")

namespace {

std::size_t span_impl(const char* s, const char* set, bool accept) {
    const char* p = s;
    for (; *p != '\0'; ++p) {
        bool found = false;
        for (const char* q = set; *q != '\0'; ++q) {
            if (*p == *q) {
                found = true;
                break;
            }
            __asm__ volatile("" : "+r"(q));
        }
        if (found != accept) break;
        __asm__ volatile("" : "+r"(p));
    }
    return static_cast<std::size_t>(p - s);
}

}  // namespace

extern "C" {

int strcmp(const char* a, const char* b) {
    const auto* p = reinterpret_cast<const unsigned char*>(a);
    const auto* q = reinterpret_cast<const unsigned char*>(b);
    while (*p && *p == *q) {
        ++p;
        ++q;
        __asm__ volatile("" : "+r"(p));
    }
    return static_cast<int>(*p) - static_cast<int>(*q);
}

int strncmp(const char* a, const char* b, std::size_t n) {
    const auto* p = reinterpret_cast<const unsigned char*>(a);
    const auto* q = reinterpret_cast<const unsigned char*>(b);
    while (n--) {
        if (*p != *q) return static_cast<int>(*p) - static_cast<int>(*q);
        if (*p == 0) return 0;
        ++p;
        ++q;
        __asm__ volatile("" : "+r"(p));
    }
    return 0;
}

char* strcpy(char* __restrict__ dst, const char* __restrict__ src) {
    char* d = dst;
    while ((*d++ = *src++) != '\0') {
        __asm__ volatile("" : "+r"(d));
    }
    return dst;
}

char* strncpy(char* __restrict__ dst, const char* __restrict__ src, std::size_t n) {
    char* d = dst;
    while (n && (*d = *src) != '\0') {
        ++d;
        ++src;
        --n;
        __asm__ volatile("" : "+r"(d));
    }
    while (n--) {
        *d++ = '\0';
        __asm__ volatile("" : "+r"(d));
    }
    return dst;
}

char* strchr(const char* s, int c) {
    const char ch = static_cast<char>(c);
    for (;; ++s) {
        if (*s == ch) return const_cast<char*>(s);
        if (*s == '\0') return nullptr;
        __asm__ volatile("" : "+r"(s));
    }
}

char* strrchr(const char* s, int c) {
    const char ch = static_cast<char>(c);
    const char* last = nullptr;
    for (;; ++s) {
        if (*s == ch) last = s;
        if (*s == '\0') return const_cast<char*>(last);
        __asm__ volatile("" : "+r"(s));
    }
}

std::size_t strspn(const char* s, const char* accept) {
    return span_impl(s, accept, true);
}

std::size_t strcspn(const char* s, const char* reject) {
    return span_impl(s, reject, false);
}

void* memchr(const void* s, int c, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(s);
    const unsigned char uc = static_cast<unsigned char>(c);
    while (n--) {
        if (*p == uc) return const_cast<unsigned char*>(p);
        ++p;
        __asm__ volatile("" : "+r"(p));
    }
    return nullptr;
}

int memcmp(const void* a, const void* b, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(a);
    const auto* q = static_cast<const unsigned char*>(b);
    while (n--) {
        if (*p != *q) return static_cast<int>(*p) - static_cast<int>(*q);
        ++p;
        ++q;
        __asm__ volatile("" : "+r"(p));
    }
    return 0;
}

}  // extern "C"

#pragma GCC pop_options

#endif  // defined(__arm__) && !defined(__unix__)
