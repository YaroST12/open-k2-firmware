// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress saved results: the newest kMaxSaved result blobs (result.hpp) the user chose to keep, one file each. The interface is narrow
// and says nothing of files (put a blob through a fill callback, list the ids newest first, read bytes of one, remove one), so a later
// store (a ring log) can take its place without touching the page or the remote.
//
// Today it is littlefs: /ps/<id>.psr, the id as 4 lower-case hex digits, never 0 and never reused (the next id follows the newest), the
// directory the run journal (/ps/run.rec) also lives in. A blob is kFileBytes, over fs::kInlineMax, so every result takes one data
// block of its own. Files of another name or size are not results and are never listed, read or removed.
// Pure code over fs::Fs; tests/test_pdstress_store.cpp. Bound to the flash volume in src/pdstress/pdstress_store_hw.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/fs.hpp"
#include "k2/pdstress/result.hpp"

namespace k2::pdstress::store {

inline constexpr const char* kDir = "/ps";
inline constexpr std::size_t kMaxSaved = 8;
inline constexpr std::size_t kFileBytes = result::kTotalBytes;
inline constexpr std::size_t kPathLen = 13;   // "/ps/xxxx.psr" + NUL
static_assert(kFileBytes > fs::kInlineMax, "a result takes a data block; the volume budget counts one per result");

enum class Err : std::uint8_t { Ok, Invalid, NotFound, Full, Flash };

inline void path_of(std::uint16_t id, std::span<char, kPathLen> out) {
    constexpr char kHex[] = "0123456789abcdef";
    constexpr char kHead[] = "/ps/";
    for (std::size_t i = 0; i < 4; ++i) out[i] = kHead[i];
    for (std::size_t i = 0; i < 4; ++i) out[4 + i] = kHex[(id >> (12 - 4 * i)) & 0xF];
    constexpr char kTail[] = ".psr";
    for (std::size_t i = 0; i < 5; ++i) out[8 + i] = kTail[i];
}

// "xxxx.psr" (lower-case hex, id not 0) -> id.
inline bool parse_name(const char* name, std::uint16_t& id) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const char c = name[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') d = unsigned(c - 'a' + 10);
        else return false;
        v = v << 4 | d;
    }
    if (name[4] != '.' || name[5] != 'p' || name[6] != 's' || name[7] != 'r' || name[8] != 0 || v == 0) return false;
    id = static_cast<std::uint16_t>(v);
    return true;
}

class Store {
public:
    explicit Store(fs::Fs& f) : fs_(f) {}

    // Writes a new result of kFileBytes, served by `fill` in fs::Fs::kChunk pieces, then drops the oldest beyond kMaxSaved.
    // Invalid = the blob is not a result of this layout; Full = no room on the volume (or the ids ran out); Flash = the write failed.
    Err put(fs::Fs::Fill fill, void* ctx, std::uint16_t& id) {
        std::array<std::uint8_t, 1> v{};
        fill(ctx, 0, v);
        if (v[0] != result::kVersion) return Err::Invalid;
        fs_.mkdir(kDir);
        const Scan s = scan({});
        if (s.newest == 0xFFFF) return Err::Full;
        id = static_cast<std::uint16_t>(s.newest + 1);
        std::array<char, kPathLen> p{};
        path_of(id, p);
        const fs::Err e = fs_.write_chunks(p.data(), kFileBytes, fill, ctx);
        if (e == fs::Err::NoSpace) return Err::Full;
        if (e != fs::Err::Ok) return Err::Flash;
        for (Scan t = scan({}); t.count > kMaxSaved; t = scan({})) {
            path_of(t.oldest, p);
            if (fs_.remove(p.data()) != fs::Err::Ok) break;
        }
        return Err::Ok;
    }

    // The saved ids, newest first; returns how many it wrote (at most out.size()).
    std::size_t list(std::span<std::uint16_t> out) {
        const Scan s = scan(out);
        return s.count < out.size() ? s.count : out.size();
    }

    // Bytes [off, off + out.size()) of result `id`; 0 when there is no such result or `off` is at or past its end.
    std::size_t read(std::uint16_t id, std::size_t off, std::span<std::uint8_t> out) {
        if (id == 0 || off >= kFileBytes) return 0;
        std::array<char, kPathLen> p{};
        path_of(id, p);
        std::size_t size = 0;
        if (fs_.size_of(p.data(), size) != fs::Err::Ok || size != kFileBytes) return 0;
        std::size_t n = 0;
        if (fs_.read_at(p.data(), static_cast<std::uint32_t>(off), out, n) != fs::Err::Ok) return 0;
        return n;
    }

    Err remove(std::uint16_t id) {
        std::array<std::uint8_t, 1> b{};
        if (read(id, 0, b) == 0) return Err::NotFound;
        std::array<char, kPathLen> p{};
        path_of(id, p);
        return fs_.remove(p.data()) == fs::Err::Ok ? Err::Ok : Err::Flash;
    }

private:
    struct Scan {
        std::size_t count = 0;
        std::uint16_t newest = 0, oldest = 0;
    };
    // Every valid result: the count, the newest and oldest id, and the newest ids (newest first) into `top`.
    Scan scan(std::span<std::uint16_t> top) {
        Scan s;
        fs_.list(kDir, [&](const fs::Info& i) {
            std::uint16_t id = 0;
            if (i.dir || i.size != kFileBytes || !parse_name(i.name, id)) return true;
            const std::size_t kept = s.count < top.size() ? s.count : top.size();
            ++s.count;
            if (id > s.newest) s.newest = id;
            if (s.oldest == 0 || id < s.oldest) s.oldest = id;
            std::size_t k = kept;   // insertion into the top list, newest first
            while (k > 0 && top[k - 1] < id) {
                if (k < top.size()) top[k] = top[k - 1];
                --k;
            }
            if (k < top.size()) top[k] = id;
            return true;
        });
        return s;
    }

    fs::Fs& fs_;
};

}  // namespace k2::pdstress::store
