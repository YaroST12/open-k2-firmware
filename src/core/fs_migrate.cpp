// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One-time move of a wide volume into the ring layout, see k2/core/fs_migrate.hpp.
#include "k2/core/fs_migrate.hpp"

#include <cstring>

#include "k2/drivers/flash.hpp"

namespace k2::fsmigrate {
namespace {

constexpr std::uint32_t kN = flash::kFsVolumeBlocks;
constexpr int kMaxDepth = 4;
constexpr unsigned kMaxPasses = 2;
using Path = char[fs::kPathMax];

// child = dir + "/" + name; false when it does not fit kPathMax.
bool join(const char* dir, const char* name, Path& out) {
    const std::size_t dl = std::strlen(dir), nl = std::strlen(name);
    const bool root = dl == 1 && dir[0] == '/';
    const std::size_t len = (root ? 1 : dl + 1) + nl;
    if (len + 1 > fs::kPathMax) return false;
    std::size_t p = 0;
    if (!root) { std::memcpy(out, dir, dl); p = dl; }
    out[p++] = '/';
    std::memcpy(out + p, name, nl + 1);
    return true;
}
// path + kMark; false when it does not fit (a name of LFS_NAME_MAX characters has no room for the mark).
bool marked_path(const char* path, Path& out) {
    const std::size_t n = std::strlen(path);
    const char* name = std::strrchr(path, '/');
    if (n + 2 > fs::kPathMax || (name && std::strlen(name + 1) + 1 > LFS_NAME_MAX)) return false;
    std::memcpy(out, path, n);
    out[n] = kMark;
    out[n + 1] = 0;
    return true;
}
bool marked(const char* name) {
    const std::size_t n = std::strlen(name);
    return n > 1 && name[n - 1] == kMark;
}

// The idx-th entry of dir in listing order; false past the end (err: what the listing answered).
bool nth(fs::Fs& f, const char* dir, unsigned idx, fs::Info& out, fs::Err& err) {
    unsigned k = 0;
    bool got = false;
    err = f.list(dir, [&](const fs::Info& i) {
        if (k++ != idx) return true;
        out = i;
        got = true;
        return false;
    });
    return got;
}

// Removes a file, or a directory with everything below it (missing = done).
fs::Err remove_tree(fs::Fs& f, const char* path, int depth = 0) {
    std::size_t sz = 0;
    if (f.size_of(path, sz) == fs::Err::NotFound) return fs::Err::Ok;
    if (depth < kMaxDepth) {
        f.clean_tmp(path);   // a directory: its ".tmp" leftovers, which list() does not show
        for (;;) {           // the first entry each time: the listing never runs over a removal
            fs::Info i{};
            fs::Err le = fs::Err::Ok;
            if (!nth(f, path, 0, i, le)) break;
            Path child;
            if (!join(path, i.name, child)) return fs::Err::Invalid;
            const fs::Err e = i.dir ? remove_tree(f, child, depth + 1) : f.remove(child);
            if (e != fs::Err::Ok && e != fs::Err::NotFound) return e;
        }
    }
    const fs::Err e = f.remove(path);
    return e == fs::Err::NotFound ? fs::Err::Ok : e;
}

// Walks the tree. Every directory is settled first (the leftovers of an interrupted run); in a rewrite walk each file whose
// data may lie in blocks of its own is copied to "<name>~" and renamed over the original (same directory: one atomic
// commit; the copy's blocks come from below the fence, the old blocks are freed).
class Walker {
public:
    Walker(fs::Fs& f, std::span<std::uint8_t> buf, Result& r) : f_(f), buf_(buf), r_(r) {}

    // settle_only: leftovers only. copy_all: every file, inline-sized ones too (a small file appended to keeps its data
    // block until it is written whole again).
    fs::Err run(bool settle_only, bool copy_all) {
        settle_only_ = settle_only;
        copy_all_ = copy_all;
        return walk("/", 0);
    }

private:
    fs::Fs& f_;
    std::span<std::uint8_t> buf_;
    Result& r_;
    bool settle_only_ = false;
    bool copy_all_ = false;

    fs::Err copy(const char* src, const char* dst, std::size_t size) {
        std::size_t off = 0;
        do {
            const std::size_t want = size - off < buf_.size() ? size - off : buf_.size();
            std::size_t n = 0;
            if (const fs::Err e = f_.read_at(src, std::uint32_t(off), buf_.first(want), n); e != fs::Err::Ok) return e;
            if (n != want) return fs::Err::Corrupt;
            const fs::Err e = off == 0 ? f_.write_file(dst, buf_.first(n)) : f_.append_file(dst, buf_.first(n));
            if (e != fs::Err::Ok) return e;
            off += n;
        } while (off < size);
        return fs::Err::Ok;
    }

    fs::Err rewrite(const char* path, std::size_t size) {
        Path tmp;
        if (!marked_path(path, tmp)) return fs::Err::Ok;   // no room for the mark: left as it is (Stuck if it is above)
        if (const fs::Err e = copy(path, tmp, size); e != fs::Err::Ok) return e;
        if (const fs::Err e = f_.rename(tmp, path); e != fs::Err::Ok) return e;
        ++r_.files;
        return fs::Err::Ok;
    }

    // A "<name>~" file is a copy cut short (the original is still there): removed. Without its original (which only the
    // rename itself takes away) it is the whole file: renamed back.
    fs::Err settle(const char* dir) {
        f_.clean_tmp(dir);
        for (unsigned i = 0;;) {
            fs::Info e{};
            fs::Err le = fs::Err::Ok;
            if (!nth(f_, dir, i, e, le)) return le;
            if (e.dir || !marked(e.name)) { ++i; continue; }
            r_.resumed = true;
            Path p, base;
            if (!join(dir, e.name, p)) return fs::Err::Invalid;
            std::memcpy(base, p, sizeof base);
            base[std::strlen(base) - 1] = 0;
            const fs::Err r = f_.exists(base) ? f_.remove(p) : f_.rename(p, base);
            if (r != fs::Err::Ok) return r;   // the listing changed: look at index i again
        }
    }

    fs::Err walk(const char* dir, int depth) {
        if (depth > kMaxDepth) return fs::Err::Ok;
        if (const fs::Err e = settle(dir); e != fs::Err::Ok) return e;
        // The names stay the same through a rewrite (the copy is renamed over its original) and littlefs keeps a
        // directory sorted by name, so an index walk sees every entry once.
        for (unsigned i = 0;; ++i) {
            fs::Info e{};
            fs::Err le = fs::Err::Ok;
            if (!nth(f_, dir, i, e, le)) return le;
            Path p;
            if (!join(dir, e.name, p)) return fs::Err::Invalid;
            fs::Err r = fs::Err::Ok;
            if (e.dir) r = walk(p, depth + 1);
            else if (!settle_only_ && (copy_all_ || e.size > fs::kInlineMax)) r = rewrite(p, e.size);
            if (r != fs::Err::Ok) return r;
        }
    }
};

}  // namespace

Result migrate(fs::Fs& f, ringlog::Io& io, std::span<std::uint8_t> buf) {
    Result res;
    if (!f.wide() || buf.size() < 256) return res;
    const auto fail = [&](fs::Err e, Outcome o = Outcome::Failed) {
        f.fence(0);
        res.err = e;
        res.outcome = o;
        return res;
    };
    f.fence(kN);
    for (const char* p : kDropped)
        if (const fs::Err e = remove_tree(f, p); e != fs::Err::Ok) return fail(e);
    res.used_blocks = f.used_blocks();
    if (res.used_blocks + kMarginBlocks > kN) {
        res.tight = true;
        for (const char* p : kDroppedIfTight)
            if (const fs::Err e = remove_tree(f, p); e != fs::Err::Ok) return fail(e);
        res.used_blocks = f.used_blocks();
        if (res.used_blocks == 0 || res.used_blocks + kMarginBlocks > kN) return fail(fs::Err::Ok, Outcome::Deferred);
    }
    Walker w(f, buf, res);
    if (const fs::Err e = w.run(true, false); e != fs::Err::Ok) return fail(e);
    res.above = f.used_from(kN);
    for (std::uint32_t left = res.above; left != 0; left = f.used_from(kN)) {
        if (left == ~0u) return fail(fs::Err::Corrupt);
        if (res.passes == kMaxPasses) {
            res.above = left;
            return fail(fs::Err::Ok, Outcome::Stuck);
        }
        if (const fs::Err e = w.run(false, res.passes++ > 0); e != fs::Err::Ok) return fail(e);
        for (int round = 0; round < 2; ++round)   // the second round moves the other block of a pair written in the first
            if (const fs::Err e = f.compact(); e != fs::Err::Ok) return fail(e);
    }
    if (const fs::Err e = f.shrink(); e != fs::Err::Ok) return fail(e);
    f.fence(0);
    for (unsigned i = 0; i < flash::kRingSectors; ++i) {
        const std::uint32_t a = flash::kRingBase + i * flash::kSectorSize;
        const std::uint8_t* p = io.view(a);
        bool blank = p != nullptr;
        for (std::uint32_t k = 0; blank && k < flash::kSectorSize; ++k) blank = p[k] == 0xFF;
        if (!blank && io.erase_sector(a)) ++res.erased;
    }
    res.outcome = Outcome::Migrated;
    return res;
}

}  // namespace k2::fsmigrate
