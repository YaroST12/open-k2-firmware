// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

/* Minimal check macros for the host tests; include from the test's main file only. */
#include <stdio.h>
#include <string.h>

static int g_checks, g_failures;

#define CHECK_COND(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; \
        printf("FAIL %s:%d: %s is false\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(actual, expected) do { \
    long long a_ = (long long)(actual), e_ = (long long)(expected); \
    g_checks++; \
    if (a_ != e_) { g_failures++; \
        printf("FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); } \
} while (0)

#ifdef __cplusplus
extern "C" const char *k2_strpack_unpack_c(const char *s, char *buf, size_t cap) __attribute__((weak));
static inline void k2_check_str_(const char *actual, const char *expected, const char *file, int line, const char *expr) {
    char ab_[256], eb_[256];
    const char *as_ = k2_strpack_unpack_c ? k2_strpack_unpack_c(actual, ab_, sizeof(ab_)) : actual;
    const char *es_ = k2_strpack_unpack_c ? k2_strpack_unpack_c(expected, eb_, sizeof(eb_)) : expected;
    g_checks++;
    if (strcmp(as_, es_) != 0) {
        g_failures++;
        printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", file, line, expr, as_, es_);
    }
}
#define CHECK_STR(actual, expected) k2_check_str_((actual), (expected), __FILE__, __LINE__, #actual)
#else
#define CHECK_STR(actual, expected) do { \
    g_checks++; \
    if (strcmp((actual), (expected)) != 0) { g_failures++; \
        printf("FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #actual, (actual), (expected)); } \
} while (0)
#endif

#ifdef __cplusplus
/* Defined by tests/trunc_log.cpp (linked into the UI tests): the number of distinct UI copy strings that were cut or
   clipped while this test drove the UI. Weak, so tests that do not link it are unaffected. */
extern "C" int k2_trunc_failures(void) __attribute__((weak));
#endif

static int test_report(const char *name) {
    int truncated = 0;
#ifdef __cplusplus
    if (k2_trunc_failures) truncated = k2_trunc_failures();
#endif
    printf("%s: %d checks, %d failed%s\n", name, g_checks, g_failures, truncated ? " (truncated copy, see above)" : "");
    return g_failures || truncated ? 1 : 0;
}

#endif /* TEST_UTIL_H */
