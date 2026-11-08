// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emname.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "k2/pd/pd.hpp"

namespace k2::emname {

namespace {

struct Out {
    std::array<char, kMaxLen + 1> buf{};
    std::size_t n = 0;
    void put(std::string_view s) {
        for (char c : s)
            if (n < buf.size() - 1) buf[n++] = c;
    }
};

std::string_view version(std::uint16_t vid, std::uint32_t cab) {
    const unsigned bits = cab & 7;
    if (vid == 0x8087 && bits >= 2) return "TBT3";
    switch (bits) {
        case 1: return "USB3.2G1";
        case 2: return "USB3.2G2";
        case 3: return "USB4G3";
        case 4: return "USB4G4";
        default: return "USB2";
    }
}

}  // namespace

std::array<char, kMaxLen + 1> make(const emrec::Record& rec, unsigned ordinal) {
    const std::uint32_t id = emrec::vdo(rec, 1), cab = emrec::vdo(rec, 4);
    const auto vid = static_cast<std::uint16_t>(id & 0xFFFF);
    std::array<std::uint8_t, 6 * 4> raw{};
    for (unsigned i = 0; i < 5; i++)
        for (unsigned b = 0; b < 4; b++) raw[4 * i + b] = static_cast<std::uint8_t>(emrec::vdo(rec, i) >> (8 * b));
    const pd::Emarker em = pd::parse_emarker(raw, 5);

    char mfg[16];
    std::string_view vendor = pd::vendor_name(vid);
    if (vendor == "Unknown") {
        const char hex[] = "0123456789ABCDEF";
        mfg[0] = '0'; mfg[1] = 'x';
        for (int k = 0; k < 4; k++) mfg[2 + k] = hex[(vid >> (12 - 4 * k)) & 0xF];
        mfg[6] = 0;
        vendor = mfg;
    }
    char watts[8], suffix[8];
    {
        unsigned w = em.max_power_w, k = 0;
        char d[6];
        do { d[k++] = static_cast<char>('0' + w % 10); w /= 10; } while (w);
        std::size_t n = 0;
        while (k) watts[n++] = d[--k];
        watts[n++] = 'W';
        watts[n] = 0;
        suffix[0] = 0;
        if (ordinal >= 2) {
            unsigned o = ordinal, j = 0;
            char e[6];
            do { e[j++] = static_cast<char>('0' + o % 10); o /= 10; } while (o);
            std::size_t s = 0;
            suffix[s++] = '-';
            while (j) suffix[s++] = e[--j];
            suffix[s] = 0;
        }
    }
    const std::string_view ver = version(vid, cab);
    const std::size_t tail = 1 + ver.size() + 1 + std::strlen(watts) + std::strlen(suffix);
    const std::size_t room = kMaxLen > tail ? kMaxLen - tail : 0;
    Out out;
    out.put(vendor.substr(0, room));
    out.put("-");
    out.put(ver);
    out.put("-");
    out.put(watts);
    out.put(suffix);
    return out.buf;
}

Short83 short_name(std::string_view name, TakenFn taken, const void* ctx) {
    char base[kMaxLen + 1];
    std::size_t n = 0;
    for (char c : name) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok && n < kMaxLen) base[n++] = c;
    }
    if (n == 0) {
        std::memcpy(base, "CABLE", 5);
        n = 5;
    }
    for (unsigned k = 0;; k++) {
        Short83 out;
        out.fill(' ');
        std::size_t o = 0;
        if (k == 0 && n <= 8) {
            for (; o < n; o++) out[o] = base[o];
        } else {
            char tail[8];
            std::size_t t = 0;
            tail[t++] = '~';
            char d[6];
            unsigned v = k == 0 ? 1 : k, j = 0;
            do { d[j++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
            while (j) tail[t++] = d[--j];
            const std::size_t keep = std::min<std::size_t>(n, 8 - t);
            for (; o < keep; o++) out[o] = base[o];
            for (std::size_t i = 0; i < t; i++) out[o++] = tail[i];
        }
        out[8] = 'J'; out[9] = 'S'; out[10] = 'N';
        if (!taken(ctx, out) || k > 9998) return out;
    }
}

Short83 short_name(std::string_view name, std::span<const Short83> taken) {
    return short_name(name, [](const void* c, const Short83& n) {
        for (const Short83& u : *static_cast<const std::span<const Short83>*>(c)) if (u == n) return true;
        return false;
    }, &taken);
}

}  // namespace k2::emname
