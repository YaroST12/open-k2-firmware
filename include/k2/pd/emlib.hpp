// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// E-marker library: any number of saved cables (up to kCapacity), one file each in the /emk directory of
// the littlefs volume (k2/core/fs.hpp). Pure logic over k2::fs::Fs, host-tested by tests/test_emlib.cpp on a RAM
// disk that can lose power at any operation. It never touches the native pages 0x080E3000 (single
// copied-e-marker slot) and 0x080E3800 (native 10-slot library).
//
// File /emk/NNNN.emk (NNNN = the cable number 1..9999, stays the same through renames), 98 bytes:
//     +0  "K2E1"   magic and format version
//     +4  seq      u16, equals the number in the file name
//     +6  version  u16, 1 for a new cable, +1 for every rename
//     +8  name     kNameMax (24) characters, NUL padded: the user's name, default the auto name
//     +32 record   the 66-byte e-marker record (k2::emrec); its own name field keeps the
//                  auto name (<maker>-<USB version>-<watts>, k2/pd/emname.hpp)
// A clone that also answers more than Discover Identity (k2/pd/emk_quirk.hpp: response rows, unmatched policy,
// flags) is a "K2E2" file: the same 98 bytes, then the quirk block (emk_quirk::encode, 3 + 52 per row bytes).
// A cable without quirks is still written as K2E1, so an older build reads everything it could before. A K2E2
// file whose block does not parse still loads as a cable; only its quirks are dropped.
// A save or rename replaces the file atomically (littlefs commit + rename), so a power cut leaves the old
// cable or the new one. littlefs checksums its metadata, so the entry carries no CRC of its own.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/pd/emk_quirk.hpp"
#include "k2/pd/emname.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/core/fs.hpp"
#include "k2/core/scratch.hpp"

namespace k2::emlib {

inline constexpr std::size_t kNameMax = 24;
inline constexpr std::size_t kOffSeq = 4, kOffVer = 6, kOffName = 8, kOffRec = 8 + kNameMax;   // 32
inline constexpr std::size_t kFileSize = kOffRec + emrec::kRecordSize;                         // 98
inline constexpr unsigned kCapacity = 140;                    // list buffers are sized for this many cables
inline constexpr std::uint16_t kMaxSeq = 9999;
inline constexpr std::size_t kMaxFileSize = kFileSize + emk_quirk::kMaxBlock;
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'E', '1'};       // no quirks
inline constexpr std::array<std::uint8_t, 4> kMagicQ{'K', '2', 'E', '2'};      // quirk block follows
inline constexpr const char* kDir = "/emk";

struct Entry {
    std::uint16_t seq = 0;
    std::uint16_t version = 0;
    std::array<char, kNameMax + 1> name{};      // the user's name, NUL terminated
    emrec::Record rec{};
};

// A name the user may give: 1..kNameMax characters from [A-Za-z0-9 -_.], not starting or ending
// with a space. sanitize() makes any text into one (drops other characters, trims, cuts);
// false when nothing is left.
constexpr bool name_valid(std::string_view n) {
    if (n.empty() || n.size() > kNameMax || n.front() == ' ' || n.back() == ' ') return false;
    for (char c : n) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

constexpr bool sanitize(std::string_view in, std::array<char, kNameMax + 1>& out) {
    out = {};
    std::size_t n = 0;
    for (char c : in) {
        if (n == kNameMax) break;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.';
        if (!ok || (c == ' ' && n == 0)) continue;
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
    return n > 0;
}

enum class Err : std::uint8_t { Ok, Full, Invalid, NotFound, Flash, Exists };

struct Result {
    Err err = Err::Ok;
    std::uint16_t seq = 0;                       // the cable's number (save)
    bool existed = false;                        // save: the same cable was saved before; nothing written
};

constexpr const char* err_name(Err e) {
    switch (e) {
    case Err::Ok: return "ok";
    case Err::Full: return "library full";
    case Err::Invalid: return "invalid record";
    case Err::NotFound: return "no such cable";
    case Err::Flash: return "flash error";
    case Err::Exists: return "name in use";
    }
    return "?";
}

class Library {
public:
    explicit Library(fs::Fs& f) : fs_(f) {}

    // Cable numbers in ascending order into `out`; returns how many (at most out.size()).
    unsigned list(std::span<std::uint16_t> out) const {
        unsigned n = 0;
        fs_.list(kDir, [&](const fs::Info& i) {
            std::uint16_t q;
            if (i.dir || !size_ok(i.size) || !parse_name(i.name, q)) return true;
            if (n < out.size()) out[n++] = q;
            return n < out.size();
        });
        return n;
    }

    unsigned count() const {
        std::array<std::uint16_t, kCapacity + 1> all;
        return list(all);
    }

    bool get(std::uint16_t seq, Entry& out) const { return load(seq, out, nullptr); }

    // The cable and its quirk table in one read; a damaged block gives an empty table (the cable is still usable).
    bool get(std::uint16_t seq, Entry& out, emk_quirk::Quirks& quirks) const {
        bool ok = false;
        if (!load(seq, out, &quirks, &ok)) return false;
        if (!ok) quirks = emk_quirk::Quirks{};
        return true;
    }

    // The cable's quirk table: empty for a K2E1 file; false when the cable is missing or its block is damaged
    // (then `out` is untouched and the cable should be treated as a plain identity).
    bool get_quirks(std::uint16_t seq, emk_quirk::Quirks& out) const {
        Entry e;
        bool ok = false;
        return load(seq, e, &out, &ok) && ok;     // load() writes `out` only for a parsed table
    }

    // Saves a cable. The record's name is replaced by the auto name (k2/pd/emname.hpp, -2, -3 on a collision).
    // The same cable data (everything but the name) with the same quirks saved twice answers the first entry
    // with existed = true.
    Result save(const emrec::Record& in, const emk_quirk::Quirks& quirks = {}) {
        Result r;
        if (!emrec::valid(in)) { r.err = Err::Invalid; return r; }
        const scratch::Lease lease("emlib.save");
        auto& blk = scratch().block;
        const std::size_t blen = quirks.empty() ? 0 : emk_quirk::encode(quirks, blk);
        if (!quirks.empty() && blen == 0) { r.err = Err::Invalid; return r; }
        All all;
        const unsigned n = list(all);
        for (unsigned i = 0; i < n; i++) {
            if (same_cable(all[i], in, std::span<const std::uint8_t>(blk.data(), blen))) {
                r.seq = all[i];
                r.existed = true;
                return r;
            }
        }
        if (n >= kCapacity) { r.err = Err::Full; return r; }
        r.seq = next_seq(all, n);
        std::array<std::uint32_t, 5> vdos;
        for (unsigned i = 0; i < 5; i++) vdos[i] = emrec::vdo(in, i);
        Entry item;
        item.seq = r.seq;
        item.version = 1;
        item.name = unique_name(in, all, n);
        item.rec = emrec::make(vdos, in[kDataAt], emname::make(in).data());
        r.err = store(item, std::span<const std::uint8_t>(blk.data(), blen));
        return r;
    }

    Err remove(std::uint16_t seq) {
        char path[kPathLen];
        make_path(seq, path);
        const fs::Err e = fs_.remove(path);
        return e == fs::Err::Ok ? Err::Ok : e == fs::Err::NotFound ? Err::NotFound : Err::Flash;
    }

    // Gives the cable a new name (name_valid, not used by another cable, compared ignoring case).
    Err rename(std::uint16_t seq, std::string_view name) {
        if (!name_valid(name)) return Err::Invalid;
        const scratch::Lease lease("emlib.rename");
        Entry cur;
        const std::size_t flen = read_file(seq, cur);
        if (flen == 0) return Err::NotFound;
        const std::size_t blen = flen - kFileSize;
        auto& blk = scratch().block;               // name_taken() below reloads the file buffer, so keep the block here
        std::memcpy(blk.data(), &scratch().file[kFileSize], blen);
        if (std::string_view(cur.name.data()) == name) return Err::Ok;
        All all;
        const unsigned n = list(all);
        if (name_taken(name, seq, all, n)) return Err::Exists;
        cur.version = static_cast<std::uint16_t>(cur.version + 1);
        cur.name = {};
        std::memcpy(cur.name.data(), name.data(), name.size());
        return store(cur, std::span<const std::uint8_t>(blk.data(), blen));   // the raw block rides along unchanged
    }

private:
    using All = std::array<std::uint16_t, kCapacity + 1>;
    static constexpr std::size_t kDataAt = 0x14;                  // record bytes covered by the duplicate test:
    static constexpr std::size_t kDataLen = 0x41 - 0x14;          // spec rev, the VDOs, reserved (not name, not checksum)
    static constexpr std::size_t kPathLen = 16;                   // "/emk/0000.emk"

    fs::Fs& fs_;

    static std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

    static void make_path(std::uint16_t seq, char (&p)[kPathLen]) {
        std::memcpy(p, "/emk/0000.emk", 14);
        for (int k = 8; k >= 5; k--) { p[k] = static_cast<char>('0' + seq % 10); seq = static_cast<std::uint16_t>(seq / 10); }
    }
    static bool parse_name(const char* n, std::uint16_t& seq) {
        if (std::strlen(n) != 8 || std::memcmp(n + 4, ".emk", 4) != 0) return false;
        unsigned v = 0;
        for (int k = 0; k < 4; k++) { if (n[k] < '0' || n[k] > '9') return false; v = v * 10 + unsigned(n[k] - '0'); }
        seq = static_cast<std::uint16_t>(v);
        return v >= 1 && v <= kMaxSeq;
    }

    // A file is the plain 98 bytes or those plus a quirk block of 0..kMaxRows rows (the block is validated on read).
    static bool size_ok(std::size_t sz) {
        if (sz == kFileSize) return true;
        if (sz < kFileSize + emk_quirk::block_size(1) || sz > kMaxFileSize) return false;
        return (sz - kFileSize - emk_quirk::kBlockHeader) % emk_quirk::kRowBytes == 0;
    }

    // One file image and one quirk block, taken from the shared main-loop scratch arena (k2/core/scratch.hpp) instead of
    // ~940 B of stack on call chains that already sit deep (littlefs runs under them). The public calls that need it
    // (load, save, rename) hold the lease; everything private below runs under one of them and must never lease
    // (name_taken reads through read_file, not load). `file` holds the last file read or assembled; `block` is a
    // block kept across calls that reload `file` (save: the encoded table while same_cable() reads files; rename:
    // the stored block).
    struct Scratch {
        std::array<std::uint8_t, kMaxFileSize> file;
        std::array<std::uint8_t, emk_quirk::kMaxBlock> block;
    };
    static Scratch& scratch() { return scratch::as<Scratch>(); }

    // Reads and checks cable `seq` into `out`; returns the file length (0 when missing or invalid). The stored
    // quirk block is then scratch().file[kFileSize .. length), valid until the next call that uses the scratch.
    std::size_t read_file(std::uint16_t seq, Entry& out) const {
        char path[kPathLen];
        make_path(seq, path);
        auto& b = scratch().file;
        std::size_t n = 0;
        if (fs_.read_file(path, b, n) != fs::Err::Ok || !size_ok(n)) return 0;
        const bool plain = n == kFileSize;
        if (std::memcmp(b.data(), (plain ? kMagic : kMagicQ).data(), kMagic.size()) != 0 || le16(&b[kOffSeq]) != seq) return 0;
        out.seq = seq;
        out.version = le16(&b[kOffVer]);
        std::memcpy(out.name.data(), &b[kOffName], kNameMax);
        out.name[kNameMax] = 0;
        std::memcpy(out.rec.data(), &b[kOffRec], emrec::kRecordSize);
        return n;
    }

    // Reads cable `seq`. `q` / `q_ok` get the parsed quirk table (empty without a block; q_ok false and `*q`
    // untouched when the block does not parse). A null `q` skips the table (q_ok must then be null too).
    bool load(std::uint16_t seq, Entry& out, emk_quirk::Quirks* q, bool* q_ok = nullptr) const {
        const scratch::Lease lease("emlib.load");
        const std::size_t n = read_file(seq, out);
        if (n == 0) return false;
        if (q != nullptr) {
            bool ok = true;
            if (n == kFileSize) *q = emk_quirk::Quirks{};
            else ok = emk_quirk::decode(std::span<const std::uint8_t>(&scratch().file[kFileSize], n - kFileSize), *q);
            if (q_ok != nullptr) *q_ok = ok;
        }
        return true;
    }

    // True when stored cable `seq` is the same clone as `in` + block: equal cable data (spec rev, VDOs, reserved)
    // and equal quirk block bytes (the encoding is canonical). `block` must not point into scratch().file.
    bool same_cable(std::uint16_t seq, const emrec::Record& in, std::span<const std::uint8_t> block) const {
        Entry e;
        const std::size_t n = read_file(seq, e);
        if (n == 0) return false;
        const std::size_t rawlen = n - kFileSize;
        return std::memcmp(e.rec.data() + kDataAt, in.data() + kDataAt, kDataLen) == 0 && rawlen == block.size() &&
               std::memcmp(&scratch().file[kFileSize], block.data(), rawlen) == 0;
    }

    Err store(const Entry& it, std::span<const std::uint8_t> block) {
        auto& b = scratch().file;
        std::memset(b.data(), 0, kFileSize);       // the bytes no field covers are written as zeros
        std::memcpy(b.data(), (block.empty() ? kMagic : kMagicQ).data(), kMagic.size());
        b[kOffSeq] = static_cast<std::uint8_t>(it.seq);
        b[kOffSeq + 1] = static_cast<std::uint8_t>(it.seq >> 8);
        b[kOffVer] = static_cast<std::uint8_t>(it.version);
        b[kOffVer + 1] = static_cast<std::uint8_t>(it.version >> 8);
        std::memcpy(&b[kOffName], it.name.data(), kNameMax);
        std::memcpy(&b[kOffRec], it.rec.data(), emrec::kRecordSize);
        if (block.size() > emk_quirk::kMaxBlock) return Err::Invalid;
        if (!block.empty()) std::memcpy(&b[kFileSize], block.data(), block.size());
        if (fs_.mkdir(kDir) != fs::Err::Ok) return Err::Flash;
        char path[kPathLen];
        make_path(it.seq, path);
        const fs::Err e = fs_.write_file(path, std::span<const std::uint8_t>(b.data(), kFileSize + block.size()));
        return e == fs::Err::Ok ? Err::Ok : e == fs::Err::NoSpace ? Err::Full : Err::Flash;
    }

    // The lowest number not in use. Refilling the holes keeps the numbers dense, so the directory's metadata
    // blocks stay full (a counter that only grows would spread deletions over ever more half-empty blocks).
    static std::uint16_t next_seq(const All& all, unsigned n) {
        unsigned q = 1;
        for (unsigned i = 0; i < n && all[i] == q; i++) q++;          // list() is ascending
        return static_cast<std::uint16_t>(q);
    }

    static char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

    // True when another cable carries `name` (ignoring case).
    bool name_taken(std::string_view name, std::uint16_t except, const All& all, unsigned n) const {
        Entry e;
        for (unsigned i = 0; i < n; i++) {
            if (all[i] == except || read_file(all[i], e) == 0) continue;
            bool same = true;
            for (std::size_t k = 0; k <= kNameMax && same; k++) {
                const char a = k < name.size() ? name[k] : 0, b = k < kNameMax ? e.name[k] : 0;
                same = lower(a) == lower(b);
                if (a == 0 && b == 0) break;
            }
            if (same) return true;
        }
        return false;
    }

    // emname::make() for the cable, with -2, -3, ... until no other cable carries the name.
    std::array<char, kNameMax + 1> unique_name(const emrec::Record& in, const All& all, unsigned n) const {
        for (unsigned ord = 1;; ord++) {
            const auto auto_name = emname::make(in, ord);
            std::array<char, kNameMax + 1> name{};
            std::memcpy(name.data(), auto_name.data(), auto_name.size());
            if (!name_taken(name.data(), 0, all, n) || ord >= kMaxSeq) return name;
        }
    }
};

}  // namespace k2::emlib
