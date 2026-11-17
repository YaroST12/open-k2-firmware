// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Xiaomi 0x2717 (Mi Turbo Charge) authentication crypto: a self-contained SHA-256 / HMAC-SHA256
// and the 0x2717 auth digest. The HMAC key comes from the key table (k2/pd/xiaomi_keys.hpp,
// bytes in the gitignored priv/), referenced by variable name only.
#pragma once

#include <cstddef>
#include <cstdint>

#include "k2/pd/xiaomi_keys.hpp"

namespace k2::pd::mi {

// The fixed 16-byte challenge sent in cmd 0x27170105, as 4 VDO words (wire order).
inline constexpr std::uint32_t kChallenge[4] = {0x0BED65CDu, 0xB2579250u, 0x3AA3D437u, 0x51EB9F51u};

// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104).
void sha256(const std::uint8_t* data, std::size_t len, std::uint8_t out[32]);
void hmac_sha256(const std::uint8_t* key, std::size_t key_len, const std::uint8_t* msg, std::size_t msg_len,
                 std::uint8_t out[32]);

// The 0x2717 auth digest: HMAC-SHA256(kSecret[idx], BE32(challenge[0..3]) || BE32(adapter_id)),
// 32 bytes. adapter_id is the low half of our Discover Identity ACK's product VDO when the
// source asked for our identity, 0 when it did not (a charger that never sends Discover
// Identity REQ computes the digest with adapter_id 0).
// out[0..15] answers the source's cmd 0x27170205; out[16..31] goes out in cmd 0x27170108.
void auth_digest(unsigned idx, const std::uint32_t challenge[4], std::uint32_t adapter_id, std::uint8_t out[32]);

// One 4-byte slice of a digest as a VDO word (big-endian byte order inside the word), wire-ready.
inline std::uint32_t digest_word(const std::uint8_t* digest, unsigned i) {
    return (std::uint32_t(digest[4 * i]) << 24) | (std::uint32_t(digest[4 * i + 1]) << 16) |
           (std::uint32_t(digest[4 * i + 2]) << 8) | std::uint32_t(digest[4 * i + 3]);
}

}  // namespace k2::pd::mi
