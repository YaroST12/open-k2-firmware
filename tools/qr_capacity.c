// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host measurement behind tools/qr_corpus.py: the exact level-L capacities of the vendored QR generator
// (third_party/qrcodegen) for a version range, read by binary search over the real encoder, not from a table.
//
// Usage: qr_capacity <min_version> <max_version> <byte-mode prefix of the link>
// Prints one line per version:
//   v<NN> modules=<m> byte_L=<b> alnum_L=<a> mixed_chars=<c> mixed_fits=<0/1>
// where mixed_* is the largest payload of a MIXED segmentation that fits that version at level L:
// one byte-mode segment for the prefix (the address, lower case) plus one alphanumeric segment for the
// payload over the URL-safe subset of the QR alphanumeric alphabet (0-9 A-Z $ * + - . / :, no space, no %).
// Build with -DNDEBUG (the generator's argument asserts are programmer checks; the tables cover all versions).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qrcodegen.h"

#define VMAX 40
#define BIG 8192   // the search may write up to 7000 payload chars before the encoder refuses
static uint8_t g_data[BIG];
static uint8_t g_temp[BIG];
static uint8_t g_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(VMAX)];
static uint8_t g_segbuf[2][BIG];

// The URL-safe subset of the QR alphanumeric alphabet: 43 characters (space and % excluded).
static const char kAlnum43[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$*+-./:";

static int fits_byte(int v, int n) {
    for (int i = 0; i < n; i++) g_data[i] = (uint8_t)(i * 31 + 7);
    return qrcodegen_encodeBinary(g_data, (size_t)n, g_qr, qrcodegen_Ecc_LOW, v, v, qrcodegen_Mask_AUTO, false);
}

static int fits_alnum(int v, int n) {
    for (int i = 0; i < n; i++) g_temp[i] = (uint8_t)kAlnum43[i % 43];
    g_temp[n] = 0;   // makeAlphanumeric reads to the NUL
    struct qrcodegen_Segment seg = qrcodegen_makeAlphanumeric((const char *)g_temp, g_segbuf[0]);
    return qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, v, v, qrcodegen_Mask_AUTO, false, g_temp, g_qr);
}

static int fits_mixed(int v, const char *prefix, int n) {
    size_t plen = strlen(prefix);
    memcpy(g_data, prefix, plen);
    struct qrcodegen_Segment s0 = qrcodegen_makeBytes(g_data, plen, g_segbuf[0]);
    for (int i = 0; i < n; i++) g_temp[i] = (uint8_t)kAlnum43[i % 43];
    g_temp[n] = 0;
    struct qrcodegen_Segment s1 = qrcodegen_makeAlphanumeric((const char *)g_temp, g_segbuf[1]);
    struct qrcodegen_Segment segs[2] = { s0, s1 };
    return qrcodegen_encodeSegmentsAdvanced(segs, 2, qrcodegen_Ecc_LOW, v, v, qrcodegen_Mask_AUTO, false, g_temp, g_qr);
}

static int max_fit(int v, int lo, int hi, int (*fn)(int, int)) {
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (fn(v, mid)) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int max_fit_mixed(int v, const char *prefix, int lo, int hi) {
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (fits_mixed(v, prefix, mid)) lo = mid; else hi = mid - 1;
    }
    return lo;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s min_version max_version prefix\n", argv[0]); return 2; }
    int vmin = atoi(argv[1]), vmax = atoi(argv[2]);
    const char *prefix = argv[3];
    printf("# version modules byte_L alnum_L mixed_chars mixed_fits modules_check\n");
    for (int v = vmin; v <= vmax; v++) {
        int modules = 4 * v + 17;
        int b = max_fit(v, 0, 4000, fits_byte);
        int a = max_fit(v, 0, 7000, fits_alnum);
        int c = max_fit_mixed(v, prefix, 0, 7000);
        int ok = fits_mixed(v, prefix, c) && qrcodegen_getSize(g_qr) == modules && !fits_mixed(v, prefix, c + 1);
        printf("v%-2d modules=%d byte_L=%d alnum_L=%d mixed_chars=%d mixed_fits=%d modules_check=%d\n",
               v, modules, b, a, c, ok, qrcodegen_getSize(g_qr));
    }
    return 0;
}
