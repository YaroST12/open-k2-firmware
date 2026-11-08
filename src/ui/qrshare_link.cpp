// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The link form of a shared record (see k2/ui/qrshare_link.hpp).
#include "k2/ui/qrshare_link.hpp"

#include <algorithm>
#include <cstring>

namespace k2::qrshare::link {

bool supported(Type type) { return kind_letter(type, false) != 0; }

char kind_letter(Type type, bool packed) {
    switch (type) {
        case Type::Card: return packed ? kKindCardPacked : kKindCard;   // the first kind shipped keeps its pair
        case Type::Emarker: return packed ? 'E' : 'e';
        case Type::PdCaps: return packed ? 'P' : 'p';
        case Type::Identity: return packed ? 'I' : 'i';
        case Type::BootInfo: return packed ? 'B' : 'b';
        default: return 0;   // the log is far too big for one code
    }
}

bool fits(Type type, std::size_t len) { return supported(type) && len != 0 && len <= kMaxRecord; }

std::size_t base64url_encode(std::span<const std::uint8_t> in, std::span<char> out) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (out.size() < b64_len(in.size())) return 0;
    std::size_t o = 0;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const std::size_t left = in.size() - i;
        const unsigned v = unsigned(in[i]) << 16 | (left > 1 ? unsigned(in[i + 1]) << 8 : 0u) | (left > 2 ? unsigned(in[i + 2]) : 0u);
        out[o++] = kAlphabet[v >> 18];
        out[o++] = kAlphabet[v >> 12 & 63];
        if (left > 1) out[o++] = kAlphabet[v >> 6 & 63];
        if (left > 2) out[o++] = kAlphabet[v & 63];
    }
    return o;
}

std::size_t encode(Type type, std::span<const std::uint8_t> record, bool packed, std::span<char> out) {
    if (!fits(type, record.size())) return 0;
    if (out.size() < kHead + b64_len(record.size()) + 1) return 0;
    std::memcpy(out.data(), kBase.data(), kBase.size());
    out[kBase.size()] = kVersion;
    out[kBase.size() + 1] = kind_letter(type, packed);
    out[kBase.size() + 2] = '.';
    const std::size_t n = base64url_encode(record, out.subspan(kHead));
    out[kHead + n] = '\0';
    return kHead + n;
}

}  // namespace k2::qrshare::link
