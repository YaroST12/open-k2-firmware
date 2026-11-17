// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the Xiaomi 0x2717 auth crypto (src/pd/xiaomi_auth.cpp): public SHA-256 /
// HMAC-SHA256 vectors, the auth-digest wiring, and — when the gitignored priv/ is present —
// the key-table digests against the recorded expected values (priv/xiaomi_vectors_priv.h).
#include <cstdint>

#include "k2/pd/xiaomi_auth.hpp"
#include "test_util.h"

using namespace k2::pd;

namespace {

unsigned nib(char c) { return c >= '0' && c <= '9' ? unsigned(c - '0') : unsigned(c - 'a' + 10); }

void check_hex(const char* what, const std::uint8_t* got, const char* hex, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        const unsigned want = (nib(hex[2 * i]) << 4) | nib(hex[2 * i + 1]);
        g_checks++;
        if (got[i] != want) {
            g_failures++;
            printf("FAIL %s byte %u: got %02x want %02x (%s)\n", what, i, got[i], want, hex);
        }
    }
}

}  // namespace

// FIPS 180-4 SHA-256 vectors (public).
static void test_sha256_vectors() {
    std::uint8_t out[32];
    mi::sha256(nullptr, 0, out);
    check_hex("sha256('')", out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32);
    mi::sha256(reinterpret_cast<const std::uint8_t*>("abc"), 3, out);
    check_hex("sha256('abc')", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);
    const char* m56 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    mi::sha256(reinterpret_cast<const std::uint8_t*>(m56), 56, out);
    check_hex("sha256(56B)", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32);
    static std::uint8_t big[1000000];
    for (unsigned i = 0; i < sizeof(big); i++) big[i] = 'a';
    mi::sha256(big, sizeof(big), out);
    check_hex("sha256(1e6 a)", out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 32);
}

// RFC 4231 HMAC-SHA256 test cases 1 and 2 (public).
static void test_hmac_vectors() {
    std::uint8_t out[32];
    std::uint8_t key1[20];
    for (auto& b : key1) b = 0x0B;
    mi::hmac_sha256(key1, sizeof(key1), reinterpret_cast<const std::uint8_t*>("Hi There"), 8, out);
    check_hex("hmac tc1", out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);
    mi::hmac_sha256(reinterpret_cast<const std::uint8_t*>("Jefe"), 4,
                    reinterpret_cast<const std::uint8_t*>("what do ya want for nothing?"), 28, out);
    check_hex("hmac tc2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);
}

// The 0x2717 digest wiring: deterministic, adapter_id and key index change the result, the two
// halves differ (0x27170205 gets [0..15], 0x27170108 gets [16..31]).
static void test_auth_digest_wiring() {
    std::uint8_t a[32], b[32], c[32], d[32];
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0, a);
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0, b);
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0xA486u, c);
    mi::auth_digest((mi::kActiveKeyIndex + 1) % mi::kKeyCount, mi::kChallenge, 0, d);
    bool same = true, diff_adapter = false, diff_idx = false, halves_differ = true;
    for (unsigned i = 0; i < 32; i++) {
        same = same && a[i] == b[i];
        diff_adapter = diff_adapter || a[i] != c[i];
        diff_idx = diff_idx || a[i] != d[i];
        if (i < 16) halves_differ = halves_differ && a[i] != a[16 + i];
    }
    CHECK_EQ(same, true);
    CHECK_EQ(diff_adapter, true);
    CHECK_EQ(diff_idx, true);
    CHECK_EQ(halves_differ, true);
    // the digest words are big-endian slices: word 0 of a half is bytes 0..3 of it
    CHECK_EQ(mi::digest_word(a, 0),
             (std::uint32_t(a[0]) << 24) | (std::uint32_t(a[1]) << 16) | (std::uint32_t(a[2]) << 8) | std::uint32_t(a[3]));
}

// With the gitignored priv/ present, the digests must match the recorded expected values
// (priv/xiaomi_vectors_priv.h). Without priv/ the zero stubs are used; the block still runs.
#if __has_include("../priv/xiaomi_vectors_priv.h")
#include "../priv/xiaomi_vectors_priv.h"
static void test_priv_vectors() {
    std::uint8_t out[32];
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0x00000000u, out);
    CHECK_EQ(memcmp(out, mi::testvec::kDigestAdapterZero, 32), 0);
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0x0000A486u, out);
    CHECK_EQ(memcmp(out, mi::testvec::kDigestAdapterA486, 32), 0);
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0x0000A819u, out);
    CHECK_EQ(memcmp(out, mi::testvec::kDigestAdapterA819, 32), 0);
}
#else
static void test_priv_vectors() {
    std::uint8_t a[32], b[32];
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0, a);
    mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, 0, b);
    CHECK_EQ(memcmp(a, b, 32), 0);   // zero-stub path: still deterministic
}
#endif

int main() {
    test_sha256_vectors();
    test_hmac_vectors();
    test_auth_digest_wiring();
    test_priv_vectors();
    return test_report("test_xiaomi_auth");
}
