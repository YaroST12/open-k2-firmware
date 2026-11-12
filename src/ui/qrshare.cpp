// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR share framework (see k2/ui/qrshare.hpp and docs/QR_SHARE.md).
#include "k2/ui/qrshare.hpp"

#include <cstring>

#include "k2/core/lz.hpp"

namespace k2::qrshare {
namespace {

constexpr char kAlphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

struct TagRow { Type type; const char* tag; };
constexpr TagRow kTags[] = {
    {Type::BootInfo, "BI"}, {Type::Emarker, "EMK"}, {Type::PdCaps, "PDC"}, {Type::Card, "CARD"}, {Type::Identity, "ID"},
    {Type::Log, "LOG"}, {Type::LoopProbe, "LOOP"}, {Type::Battery, "BATT"},
};

int alphabet_index(char c) {
    for (int i = 0; i < 45; i++)
        if (kAlphabet[i] == c) return i;
    return -1;
}

void put16(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
void put32(std::uint8_t* p, std::uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
std::uint32_t get16(const std::uint8_t* p) { return p[0] | std::uint32_t(p[1]) << 8; }
std::uint32_t get32(const std::uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

std::size_t chunk(std::size_t record_len, unsigned count) { return (record_len + count - 1) / count; }

}  // namespace

std::string_view tag(Type t) {
    for (const auto& r : kTags)
        if (r.type == t) return r.tag;
    return {};
}

bool type_from_tag(std::string_view s, Type& out) {
    for (const auto& r : kTags)
        if (s == r.tag) { out = r.type; return true; }
    return false;
}

std::uint32_t crc32(std::span<const std::uint8_t> data, std::uint32_t crc) {
    crc = ~crc;
    for (std::uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::size_t base45_len(std::size_t bytes) { return bytes / 2 * 3 + bytes % 2 * 2; }

std::size_t base45_encode(std::span<const std::uint8_t> in, std::span<char> out) {
    if (out.size() < base45_len(in.size())) return 0;
    std::size_t o = 0;
    for (std::size_t i = 0; i < in.size(); i += 2) {
        if (i + 1 < in.size()) {
            const unsigned n = in[i] * 256u + in[i + 1];
            out[o++] = kAlphabet[n % 45];
            out[o++] = kAlphabet[n / 45 % 45];
            out[o++] = kAlphabet[n / 2025];
        } else {
            out[o++] = kAlphabet[in[i] % 45];
            out[o++] = kAlphabet[in[i] / 45];
        }
    }
    return o;
}

bool base45_decode(std::string_view in, std::span<std::uint8_t> out, std::size_t& n) {
    if (in.size() % 3 == 1) return false;
    n = 0;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const std::size_t left = in.size() - i;
        int d[3] = {0, 0, 0};
        for (std::size_t k = 0; k < (left >= 3 ? 3u : 2u); k++) {
            d[k] = alphabet_index(in[i + k]);
            if (d[k] < 0) return false;
        }
        if (left >= 3) {
            const unsigned v = unsigned(d[0]) + 45u * unsigned(d[1]) + 2025u * unsigned(d[2]);
            if (v > 0xFFFF || n + 2 > out.size()) return false;
            out[n++] = std::uint8_t(v >> 8);
            out[n++] = std::uint8_t(v);
        } else {
            const unsigned v = unsigned(d[0]) + 45u * unsigned(d[1]);
            if (v > 0xFF || n + 1 > out.size()) return false;
            out[n++] = std::uint8_t(v);
        }
    }
    return true;
}

unsigned part_count(std::size_t record_len) {
    if (record_len == 0 || record_len > kMaxRecord) return 0;
    return unsigned((record_len + kPartPayload - 1) / kPartPayload);
}

std::size_t slice_len(std::size_t record_len, unsigned index) {
    const unsigned n = part_count(record_len);
    if (index >= n) return 0;
    const std::size_t c = chunk(record_len, n);
    return index + 1 < n ? c : record_len - c * (n - 1);
}

std::size_t pack(std::span<const std::uint8_t> record, std::span<std::uint8_t> out) {
    const std::size_t n = lz::compress(record, out);
    return n != 0 && n * 4 <= record.size() * 3 ? n : 0;
}

std::size_t encode_part(Type type, std::uint8_t schema, std::span<const std::uint8_t> record, unsigned index,
                        std::span<char> out, bool packed) {
    const std::string_view t = tag(type);
    const unsigned n = part_count(record.size());
    if (t.empty() || n == 0 || index >= n) return 0;
    const std::size_t off = index * chunk(record.size(), n), len = slice_len(record.size(), index);

    std::uint8_t frame[kMaxFrame];
    frame[0] = 'K'; frame[1] = '2';
    frame[2] = std::uint8_t(type) | (packed ? kPackedFlag : 0); frame[3] = schema;
    frame[4] = std::uint8_t(index); frame[5] = std::uint8_t(n);
    put16(frame + 6, std::uint32_t(record.size()));
    put32(frame + 8, crc32(record));
    std::memcpy(frame + kHeaderLen, record.data() + off, len);
    put32(frame + kHeaderLen + len, crc32({frame, kHeaderLen + len}));
    const std::size_t flen = kHeaderLen + len + kCrcLen;

    char prefix[kMaxTextPrefix + 1];
    std::size_t p = 0;
    prefix[p++] = 'K'; prefix[p++] = '2'; prefix[p++] = ':';
    for (char c : t) prefix[p++] = c;
    if (schema >= 100) prefix[p++] = char('0' + schema / 100);
    if (schema >= 10) prefix[p++] = char('0' + schema / 10 % 10);
    prefix[p++] = char('0' + schema % 10);
    prefix[p++] = ':';

    if (out.size() < p + base45_len(flen) + 1) return 0;
    std::memcpy(out.data(), prefix, p);
    const std::size_t b = base45_encode({frame, flen}, out.subspan(p));
    out[p + b] = '\0';
    return p + b;
}

bool parse_part(std::string_view text, Part& out, std::span<std::uint8_t> buf) {
    if (!text.starts_with("K2:")) return false;
    std::size_t i = 3;
    const std::size_t tag_at = i;
    while (i < text.size() && text[i] >= 'A' && text[i] <= 'Z') i++;
    Type type;
    if (!type_from_tag(text.substr(tag_at, i - tag_at), type)) return false;
    unsigned schema = 0;
    std::size_t digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 3) { schema = schema * 10 + unsigned(text[i] - '0'); i++; digits++; }
    if (digits == 0 || schema > 255 || i >= text.size() || text[i] != ':') return false;
    i++;

    std::size_t flen = 0;
    if (buf.size() < kMaxFrame || !base45_decode(text.substr(i), buf.first(kMaxFrame), flen)) return false;
    if (flen < kHeaderLen + 1 + kCrcLen) return false;
    const std::uint8_t* f = buf.data();
    if (f[0] != 'K' || f[1] != '2' || (f[2] & ~kPackedFlag) != std::uint8_t(type) || f[3] != schema) return false;
    if (get32(f + flen - kCrcLen) != crc32({f, flen - kCrcLen})) return false;
    const unsigned index = f[4], count = f[5];
    const std::size_t rec_len = get16(f + 6);
    if (count == 0 || count > kMaxParts || index >= count || part_count(rec_len) != count) return false;
    const std::size_t payload = flen - kHeaderLen - kCrcLen;
    if (payload != slice_len(rec_len, index)) return false;

    out.type = type;
    out.packed = (f[2] & kPackedFlag) != 0;
    out.schema = std::uint8_t(schema);
    out.index = std::uint8_t(index);
    out.count = std::uint8_t(count);
    out.record_len = std::uint16_t(rec_len);
    out.record_crc = get32(f + 8);
    out.payload = {f + kHeaderLen, payload};
    return true;
}

unsigned Assembler::parts_seen() const {
    unsigned n = 0;
    for (unsigned m = seen_; m; m >>= 1) n += m & 1u;
    return n;
}

Assembler::Result Assembler::feed(std::string_view text) {
    Part p;
    if (!parse_part(text, p, frame_)) return Result::Bad;
    if (p.record_len > buf_.size()) return Result::TooBig;
    if (!have_) {
        have_ = true;
        seen_ = 0;
        type_ = p.type; packed_ = p.packed; schema_ = p.schema; count_ = p.count; len_ = p.record_len; crc_ = p.record_crc;
    } else if (p.type != type_ || p.packed != packed_ || p.schema != schema_ || p.count != count_ || p.record_len != len_ || p.record_crc != crc_) {
        return Result::Mismatch;
    }
    if (seen_ >> p.index & 1u) return Result::Duplicate;
    std::memcpy(buf_.data() + p.index * chunk(len_, count_), p.payload.data(), p.payload.size());
    seen_ |= 1u << p.index;
    if (seen_ != full_mask()) return Result::Accepted;
    if (crc32(buf_.first(len_)) != crc_) { reset(); return Result::Bad; }
    return Result::Done;
}

}  // namespace k2::qrshare
