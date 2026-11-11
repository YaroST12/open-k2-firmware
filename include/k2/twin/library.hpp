// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Twin library: up to kCapacity saved device twins, one file each in the /twin directory of the littlefs volume
// (k2/core/fs.hpp), the same pattern as the saved cables (k2/pd/emlib.hpp). Pure logic over k2::fs::Fs, host-tested by
// tests/test_twin_lib.cpp on a RAM disk that can lose power at any operation.
//
// File /twin/NN.twn (NN = the twin number 01..kCapacity, stays the same through renames). All words little endian.
// Version 1:
//     +0  "K2T"      magic
//     +3  version    1
//     +4  seq        equals the number in the file name
//     +5  flags      b0: a Request pair follows the sink PDOs; every other bit must be 0
//     +6  vid, +8 pid, +10 id_hi (ID Header bits 31:23), +12 pd_rev (1 PD 2.0, 2 PD 3.x, 0 not read)
//     +13 num_pdos   0..7
//     +14 name       kNameMax (24) characters, NUL padded (no terminator needed at full length)
//     +38 sink PDOs  num_pdos x u32
//         the Request pair (flag b0): the RDO, then the source PDO it pointed at, 2 x u32
//     end CRC16 CCITT (init 0xFFFF, k2::bbstore::crc16) over every byte before it
// That is 40 bytes at the least, 76 at the most, 52 for a typical phone (three sink PDOs, no Request).
// A record whose version byte is newer than this build knows is refused, never guessed at (and never overwritten by a
// list or a count). A save or rename replaces the file atomically (littlefs commit + rename): a power cut leaves the old
// twin or the new one.
// The library keeps no state of its own and takes no scratch arena: a record is under 100 bytes and lives on the stack.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/core/bbstore.hpp"
#include "k2/core/fs.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/twin/learn.hpp"
#include "k2/twin/twin.hpp"

namespace k2::twin {

inline constexpr unsigned kCapacity = 12;
inline constexpr std::uint8_t kVersion1 = 1;
inline constexpr std::uint8_t kVersionMax = kVersion1;   // the newest layout this build reads
inline constexpr std::size_t kOffSeq = 4, kOffFlags = 5, kOffNumPdos = 13, kOffName = 14;
inline constexpr std::size_t kHeaderBytes = kOffName + kNameMax;                        // 38
inline constexpr std::size_t kMaxFileBytes = kHeaderBytes + kMaxSinkPdos * 4 + 8 + 2;   // 76
inline constexpr std::size_t kMinFileBytes = kHeaderBytes + 2;
inline constexpr std::uint8_t kFlagRequest = 1;
inline constexpr const char* kDir = "/twin";

enum class Dec : std::uint8_t { Ok, Short, Magic, Version, Invalid, Crc };

namespace detail {
inline void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = std::uint8_t(v >> (8 * i)); }
inline std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
inline std::uint32_t get32(const std::uint8_t* p) { return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24); }
}  // namespace detail

// Writes the version 1 record of `t` as twin number `seq`. Returns its length, 0 when `out` is too small or the twin holds
// more than kMaxSinkPdos PDOs (nothing is written then).
inline std::size_t encode(const Twin& t, std::uint8_t seq, std::span<std::uint8_t> out) {
    if (t.num_pdos > kMaxSinkPdos) return 0;
    const bool req = t.has_request();
    const std::size_t len = kHeaderBytes + std::size_t(t.num_pdos) * 4 + (req ? 8 : 0) + 2;
    if (out.size() < len) return 0;
    std::uint8_t* b = out.data();
    b[0] = 'K'; b[1] = '2'; b[2] = 'T'; b[3] = kVersion1;
    b[kOffSeq] = seq;
    b[kOffFlags] = req ? kFlagRequest : 0;
    detail::put16(&b[6], t.vid);
    detail::put16(&b[8], t.pid);
    detail::put16(&b[10], t.id_hi);
    b[12] = t.pd_rev;
    b[kOffNumPdos] = t.num_pdos;
    std::memset(&b[kOffName], 0, kNameMax);   // canonical: bytes after the name's end are zeros
    for (std::size_t i = 0; i < kNameMax && t.name[i] != 0; ++i) b[kOffName + i] = std::uint8_t(t.name[i]);
    std::size_t at = kHeaderBytes;
    for (std::size_t i = 0; i < t.num_pdos; ++i, at += 4) detail::put32(&b[at], t.pdos[i]);
    if (req) {
        detail::put32(&b[at], t.rdo);
        detail::put32(&b[at + 4], t.rdo_pdo);
        at += 8;
    }
    detail::put16(&b[at], bbstore::crc16(0xFFFF, {b, at}));
    return len;
}

// Reads a record. Ok fills `seq` and `out`; every other answer leaves them alone.
inline Dec decode(std::span<const std::uint8_t> in, std::uint8_t& seq, Twin& out) {
    if (in.size() < 4) return Dec::Short;
    const std::uint8_t* b = in.data();
    if (b[0] != 'K' || b[1] != '2' || b[2] != 'T') return Dec::Magic;
    if (b[3] == 0 || b[3] > kVersionMax) return Dec::Version;
    if (in.size() < kMinFileBytes) return Dec::Short;
    if ((b[kOffFlags] & ~kFlagRequest) != 0 || b[kOffNumPdos] > kMaxSinkPdos) return Dec::Invalid;
    const bool req = (b[kOffFlags] & kFlagRequest) != 0;
    const std::size_t len = kHeaderBytes + std::size_t(b[kOffNumPdos]) * 4 + (req ? 8 : 0) + 2;
    if (in.size() < len) return Dec::Short;
    if (in.size() > len) return Dec::Invalid;
    if (bbstore::crc16(0xFFFF, {b, len - 2}) != detail::get16(&b[len - 2])) return Dec::Crc;
    Twin t;
    std::memcpy(t.name.data(), &b[kOffName], kNameMax);
    t.name[kNameMax] = 0;
    if (!emlib::name_valid(t.name.data())) return Dec::Invalid;
    t.vid = detail::get16(&b[6]);
    t.pid = detail::get16(&b[8]);
    t.id_hi = detail::get16(&b[10]);
    t.pd_rev = b[12];
    t.num_pdos = b[kOffNumPdos];
    std::size_t at = kHeaderBytes;
    for (std::size_t i = 0; i < t.num_pdos; ++i, at += 4) t.pdos[i] = detail::get32(&b[at]);
    if (req) {
        t.rdo = detail::get32(&b[at]);
        t.rdo_pdo = detail::get32(&b[at + 4]);
    }
    seq = b[kOffSeq];
    out = t;
    return Dec::Ok;
}

enum class Err : std::uint8_t { Ok, Full, Invalid, NotFound, Flash, Exists };

constexpr const char* err_name(Err e) {
    switch (e) {
    case Err::Ok: return "ok";
    case Err::Full: return "library full";
    case Err::Invalid: return "invalid twin";
    case Err::NotFound: return "no such twin";
    case Err::Flash: return "flash error";
    case Err::Exists: return "name in use";
    }
    return "?";
}

struct Result {
    Err err = Err::Ok;
    std::uint8_t seq = 0;     // the twin's number (save)
    bool existed = false;     // save: the device was already saved, its twin was updated
    bool unchanged = false;   // save: ... and the update changed nothing, so nothing was written
};

class Library {
public:
    explicit Library(fs::Fs& f) : fs_(f) {}

    // Twin numbers in ascending order into `out`; returns how many (at most out.size()). A file counts by its name and
    // size alone: a damaged or newer-version record still takes its number (get() says it cannot be read).
    unsigned list(std::span<std::uint8_t> out) const {
        unsigned n = 0;
        fs_.list(kDir, [&](const fs::Info& i) {
            std::uint8_t q;
            if (i.dir || i.size < kMinFileBytes || i.size > kMaxFileBytes || !parse_name(i.name, q)) return true;
            if (n < out.size()) out[n++] = q;
            return n < out.size();
        });
        return n;
    }

    unsigned count() const {
        All all;
        return list(all);
    }

    // False when the twin is missing, damaged, or written by a newer build.
    bool get(std::uint8_t seq, Twin& out) const {
        std::array<std::uint8_t, kMaxFileBytes> b;
        std::size_t n = 0;
        if (!read_raw(seq, b, n)) return false;
        std::uint8_t got = 0;
        return decode({b.data(), n}, got, out) == Dec::Ok && got == seq;
    }

    // The number of the twin of this device (same VID and PID); 0 when none, and always 0 for a twin with no VID.
    std::uint8_t find(std::uint16_t vid, std::uint16_t pid) const {
        if (vid == 0) return 0;
        All all;
        const unsigned n = list(all);
        for (unsigned i = 0; i < n; i++) {
            Twin t;
            if (get(all[i], t) && t.vid == vid && t.pid == pid) return all[i];
        }
        return 0;
    }

    // Saves a twin. A device already in the library (same VID and PID) is updated in place: the saved name stays and
    // merge() folds in what `in` learned; nothing is written when that changes nothing. A new device takes the lowest free
    // number, its name from `in` or else the auto name, made unique with " 2", " 3" ...
    Result save(const Twin& in) {
        Result r;
        if (in.num_pdos > kMaxSinkPdos || (in.name[0] != 0 && !emlib::name_valid(in.name.data()))) { r.err = Err::Invalid; return r; }
        All all;
        const unsigned n = list(all);
        if (in.vid != 0) {
            for (unsigned i = 0; i < n; i++) {
                Twin cur;
                if (!get(all[i], cur) || cur.vid != in.vid || cur.pid != in.pid) continue;
                r.seq = all[i];
                r.existed = true;
                Twin merged = cur;
                merge(merged, in);
                std::array<std::uint8_t, kMaxFileBytes> a{}, b{};
                const std::size_t la = encode(cur, r.seq, a), lb = encode(merged, r.seq, b);
                if (la == lb && std::memcmp(a.data(), b.data(), la) == 0) { r.unchanged = true; return r; }
                r.err = store(r.seq, merged);
                return r;
            }
        }
        if (n >= kCapacity) { r.err = Err::Full; return r; }
        r.seq = next_seq(all, n);
        Twin fresh = in;
        fresh.name = unique_name(in.name[0] != 0 ? in.name : auto_name(in.vid, in.id_hi), all, n);
        r.err = store(r.seq, fresh);
        return r;
    }

    Err remove(std::uint8_t seq) {
        if (seq == 0 || seq > kCapacity) return Err::NotFound;
        char path[kPathLen];
        make_path(seq, path);
        const fs::Err e = fs_.remove(path);
        return e == fs::Err::Ok ? Err::Ok : e == fs::Err::NotFound ? Err::NotFound : Err::Flash;
    }

    // Gives the twin a new name (emlib::name_valid, not used by another twin, compared ignoring case).
    Err rename(std::uint8_t seq, std::string_view name) {
        if (!emlib::name_valid(name)) return Err::Invalid;
        Twin cur;
        if (!get(seq, cur)) return Err::NotFound;
        if (std::string_view(cur.name.data()) == name) return Err::Ok;
        All all;
        const unsigned n = list(all);
        if (name_taken(name, seq, all, n)) return Err::Exists;
        cur.name = {};
        std::memcpy(cur.name.data(), name.data(), name.size());
        return store(seq, cur);
    }

private:
    using All = std::array<std::uint8_t, kCapacity + 1>;
    static constexpr std::size_t kPathLen = 16;   // "/twin/00.twn"

    fs::Fs& fs_;

    static void make_path(std::uint8_t seq, char (&p)[kPathLen]) {
        std::memcpy(p, "/twin/00.twn", 13);
        p[6] = char('0' + seq / 10);
        p[7] = char('0' + seq % 10);
    }
    static bool parse_name(const char* n, std::uint8_t& seq) {
        if (std::strlen(n) != 6 || std::memcmp(n + 2, ".twn", 4) != 0) return false;
        if (n[0] < '0' || n[0] > '9' || n[1] < '0' || n[1] > '9') return false;
        const unsigned v = unsigned(n[0] - '0') * 10 + unsigned(n[1] - '0');
        seq = std::uint8_t(v);
        return v >= 1 && v <= kCapacity;
    }

    bool read_raw(std::uint8_t seq, std::array<std::uint8_t, kMaxFileBytes>& b, std::size_t& n) const {
        if (seq == 0 || seq > kCapacity) return false;
        char path[kPathLen];
        make_path(seq, path);
        n = 0;
        return fs_.read_file(path, b, n) == fs::Err::Ok;
    }

    Err store(std::uint8_t seq, const Twin& t) {
        std::array<std::uint8_t, kMaxFileBytes> b{};
        const std::size_t n = encode(t, seq, b);
        if (n == 0) return Err::Invalid;
        if (fs_.mkdir(kDir) != fs::Err::Ok) return Err::Flash;
        char path[kPathLen];
        make_path(seq, path);
        const fs::Err e = fs_.write_file(path, std::span<const std::uint8_t>(b.data(), n));
        return e == fs::Err::Ok ? Err::Ok : e == fs::Err::NoSpace ? Err::Full : Err::Flash;
    }

    // The lowest number not in use (list() is ascending): the numbers stay dense.
    static std::uint8_t next_seq(const All& all, unsigned n) {
        unsigned q = 1;
        for (unsigned i = 0; i < n && all[i] == q; i++) q++;
        return std::uint8_t(q);
    }

    static char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; }

    // True when another twin carries `name` (ignoring case).
    bool name_taken(std::string_view name, std::uint8_t except, const All& all, unsigned n) const {
        for (unsigned i = 0; i < n; i++) {
            Twin e;
            if (all[i] == except || !get(all[i], e)) continue;
            const std::string_view other(e.name.data());
            if (other.size() != name.size()) continue;
            bool same = true;
            for (std::size_t k = 0; k < name.size() && same; k++) same = lower(name[k]) == lower(other[k]);
            if (same) return true;
        }
        return false;
    }

    // `base`, then "base 2", "base 3" ... (cut so the suffix fits) until no other twin carries it.
    std::array<char, kNameMax + 1> unique_name(const std::array<char, kNameMax + 1>& base, const All& all, unsigned n) const {
        const std::size_t blen = std::strlen(base.data());
        for (unsigned ord = 1;; ord++) {
            std::array<char, kNameMax + 1> name{};
            if (ord == 1) {
                name = base;
            } else {
                char suf[8];
                std::size_t sl = 0;
                suf[sl++] = ' ';
                char digits[4];
                int nd = 0;
                for (unsigned v = ord; v > 0; v /= 10) digits[nd++] = char('0' + v % 10);
                while (nd > 0) suf[sl++] = digits[--nd];
                std::size_t keep = std::min(blen, kNameMax - sl);
                while (keep > 0 && base[keep - 1] == ' ') keep--;
                std::memcpy(name.data(), base.data(), keep);
                std::memcpy(name.data() + keep, suf, sl);
            }
            if (!name_taken(name.data(), 0, all, n) || ord >= 99) return name;
        }
    }
};

}  // namespace k2::twin
