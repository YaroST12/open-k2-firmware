// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Self-contained SHA-256 (FIPS 180-4) / HMAC-SHA256 (RFC 2104) for the Xiaomi 0x2717 auth
// digest. No libc, no exceptions: plain portable C++23 for the AT32F403A and the host tests.
#include "k2/pd/xiaomi_auth.hpp"

namespace k2::pd::mi {
namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    std::uint32_t h[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                         0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
    std::uint8_t buf[64] = {};
    std::size_t buf_len = 0;
    std::uint32_t total = 0;   // bytes hashed (32-bit only: the target is a 32-bit MCU)

    void block(const std::uint8_t* p) {
        std::uint32_t w[64];
        for (unsigned i = 0; i < 16; i++)
            w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
                   (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
        for (unsigned i = 16; i < 64; i++) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (unsigned i = 0; i < 64; i++) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const std::uint8_t* data, std::size_t len) {
        total += len;
        while (len > 0) {
            const std::size_t n = 64 - buf_len < len ? 64 - buf_len : len;
            for (std::size_t i = 0; i < n; i++) buf[buf_len + i] = data[i];
            buf_len += n;
            data += n;
            len -= n;
            if (buf_len == 64) {
                block(buf);
                buf_len = 0;
            }
        }
    }

    void finish(std::uint8_t out[32]) {
        // the 64-bit bit-count length field, split into two 32-bit words: bits = total * 8
        const std::uint32_t bits_hi = total >> 29;
        const std::uint32_t bits_lo = total << 3;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (buf_len != 56) update(&zero, 1);
        std::uint8_t lenb[8];
        for (int i = 0; i < 4; i++) lenb[i] = std::uint8_t(bits_hi >> (24 - 8 * i));
        for (int i = 0; i < 4; i++) lenb[4 + i] = std::uint8_t(bits_lo >> (24 - 8 * i));
        update(lenb, 8);
        for (unsigned i = 0; i < 8; i++)
            for (int b = 0; b < 4; b++) out[4 * i + b] = std::uint8_t(h[i] >> (24 - 8 * b));
    }
};

}  // namespace

void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]) {
    Sha256 s;
    s.update(data, len);
    s.finish(out);
}

void hmac_sha256(const std::uint8_t* key, std::size_t key_len, const std::uint8_t* msg, std::size_t msg_len,
                 std::uint8_t out[32]) {
    std::uint8_t k[64] = {};
    if (key_len > 64) {
        sha256(key, key_len, k);   // long keys are hashed first; k stays zero-padded
    } else {
        for (std::size_t i = 0; i < key_len; i++) k[i] = key[i];
    }
    std::uint8_t pad[64];
    Sha256 inner;
    for (unsigned i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    inner.update(pad, 64);
    inner.update(msg, msg_len);
    std::uint8_t inner_out[32];
    inner.finish(inner_out);
    Sha256 outer;
    for (unsigned i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    outer.update(pad, 64);
    outer.update(inner_out, 32);
    outer.finish(out);
}

void auth_digest(unsigned idx, const std::uint32_t challenge[4], std::uint32_t adapter_id, std::uint8_t out[32]) {
    std::uint8_t msg[20];
    for (unsigned w = 0; w < 4; w++)
        for (unsigned b = 0; b < 4; b++) msg[4 * w + b] = std::uint8_t(challenge[w] >> (24 - 8 * b));
    for (unsigned b = 0; b < 4; b++) msg[16 + b] = std::uint8_t(adapter_id >> (24 - 8 * b));
    const unsigned i = idx < kKeyCount ? idx : 0;
    hmac_sha256(kSecret[i], 32, msg, sizeof(msg), out);
}

}  // namespace k2::pd::mi
