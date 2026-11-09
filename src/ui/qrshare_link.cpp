// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The link form of a shared record (see k2/ui/qrshare_link.hpp).
#include "k2/ui/qrshare_link.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace k2::qrshare::link {

bool supported(Type type) { return kind_letter(type, false) != 0; }

bool supported(char kind) {
    switch (kind) {
        case kKindCard:
        case kKindCardPacked:
        case 'e':
        case 'E':
        case 'p':
        case 'P':
        case 'i':
        case 'I':
        case 'b':
        case 'B':
        case kKindText: return true;
        default: return false;
    }
}

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

// The frame type a plain kind letter names (0 for 't' and the packed letters, which pair with their plain letter).
static Type type_of_kind(char kind, bool& packed) {
    switch (kind) {
        case kKindCard: packed = false; return Type::Card;
        case kKindCardPacked: packed = true; return Type::Card;
        case 'e': packed = false; return Type::Emarker;
        case 'E': packed = true; return Type::Emarker;
        case 'p': packed = false; return Type::PdCaps;
        case 'P': packed = true; return Type::PdCaps;
        case 'i': packed = false; return Type::Identity;
        case 'I': packed = true; return Type::Identity;
        case 'b': packed = false; return Type::BootInfo;
        case 'B': packed = true; return Type::BootInfo;
        default: packed = false; return Type::Log;
    }
}

bool fits(Type type, std::size_t len) { return supported(type) && len != 0 && len <= kMaxRecord; }

bool fits(Type type, std::span<const std::uint8_t> record, bool packed) {
    if (!supported(type) || record.empty()) return false;
    if (!packed && records::has_v2(type) && records::valid_v1(type, record)) {
        std::array<std::uint8_t, records::kMaxEmarker> v2;   // a local: fits() is a predicate, it owns no buffer
        const std::size_t n = records::pack_v2(type, record, v2);
        if (n != 0 && n <= kMaxRecord) return true;           // the v2 link fits one code
    }
    return fits(type, record.size());
}

bool fits(char kind, std::size_t len) { return supported(kind) && len != 0 && len <= kMaxRecord; }

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

// kBase + version + kind + '.' + base64url(record), NUL terminated.
static std::size_t emit(char version, char kind, std::span<const std::uint8_t> record, std::span<char> out) {
    if (out.size() < kHead + b64_len(record.size()) + 1) return 0;
    std::memcpy(out.data(), kBase.data(), kBase.size());
    out[kBase.size()] = version;
    out[kBase.size() + 1] = kind;
    out[kBase.size() + 2] = '.';
    const std::size_t n = base64url_encode(record, out.subspan(kHead));
    out[kHead + n] = '\0';
    return kHead + n;
}

std::size_t encode(Type type, std::span<const std::uint8_t> record, bool packed, std::span<char> out, bool allow_v2) {
    if (!supported(type)) return 0;
    if (allow_v2 && !packed && records::has_v2(type) && records::valid_v1(type, record)) {
        // the v2 form first when it fits one code: the smaller QR wins over the plain and the packed v1 forms
        std::array<std::uint8_t, records::kMaxEmarker> v2;
        const std::size_t n = records::pack_v2(type, record, v2);
        if (n != 0 && fits(type, n)) return emit(kVersionV2, kind_letter(type, false), {v2.data(), n}, out);
    }
    if (!fits(type, record.size())) return 0;
    return emit(kVersion, kind_letter(type, packed), record, out);
}

std::size_t encode_v2(Type type, std::span<const std::uint8_t> v2record, std::span<char> out) {
    if (!records::has_v2(type) || v2record.empty() || v2record[0] != records::kSchemaV2) return 0;
    if (!fits(type, v2record.size())) return 0;
    return emit(kVersionV2, kind_letter(type, false), v2record, out);
}

std::size_t encode_kind(char kind, std::span<const std::uint8_t> record, std::span<char> out) {
    if (kind == kKindText) {   // a plain ASCII text record (NUL padded), version '1', never packed
        if (!fits(kind, record.size())) return 0;
        return emit(kVersion, kKindText, record, out);
    }
    bool packed = false;
    const Type type = type_of_kind(kind, packed);
    if (!supported(type)) return 0;
    return encode(type, record, packed, out);
}

}  // namespace k2::qrshare::link
