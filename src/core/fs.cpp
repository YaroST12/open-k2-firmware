// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::fs over littlefs: see include/k2/core/fs.hpp. Pure code over a BlockDevice, host-tested (tests/test_fs.cpp).
#include "k2/core/fs.hpp"

#include <cstring>

namespace k2::fs {

namespace {

Err map(int e) {
    if (e >= 0) return Err::Ok;
    switch (e) {
        case LFS_ERR_NOENT: return Err::NotFound;
        case LFS_ERR_NOSPC: return Err::NoSpace;
        case LFS_ERR_CORRUPT: return Err::Corrupt;
        case LFS_ERR_EXIST: return Err::Exists;
        case LFS_ERR_INVAL:
        case LFS_ERR_NAMETOOLONG:
        case LFS_ERR_NOTDIR:
        case LFS_ERR_ISDIR: return Err::Invalid;
        case LFS_ERR_FBIG: return Err::TooBig;
        default: return Err::Io;
    }
}

BlockDevice& dev_of(const lfs_config* c) { return *static_cast<BlockDevice*>(c->context); }

int cb_read(const lfs_config* c, lfs_block_t b, lfs_off_t o, void* buf, lfs_size_t n) { return dev_of(c).read(b, o, buf, n); }
int cb_prog(const lfs_config* c, lfs_block_t b, lfs_off_t o, const void* buf, lfs_size_t n) { return dev_of(c).prog(b, o, buf, n); }
int cb_erase(const lfs_config* c, lfs_block_t b) { return dev_of(c).erase(b); }
int cb_sync(const lfs_config*) { return 0; }

// "<path>.tmp" into out; false when it does not fit.
bool tmp_name(const char* path, char (&out)[kPathMax]) {
    const std::size_t n = std::strlen(path);
    if (n + 5 > kPathMax) return false;
    std::memcpy(out, path, n);
    std::memcpy(out + n, ".tmp", 5);
    return true;
}

bool is_tmp(const char* name) {
    const std::size_t n = std::strlen(name);
    return n >= 4 && std::memcmp(name + n - 4, ".tmp", 4) == 0;
}

}  // namespace

const char* err_name(Err e) {
    switch (e) {
        case Err::Ok: return "ok";
        case Err::NotFound: return "not found";
        case Err::NoSpace: return "no space";
        case Err::Corrupt: return "corrupt";
        case Err::Io: return "io";
        case Err::Invalid: return "invalid";
        case Err::TooBig: return "too big";
        case Err::Exists: return "exists";
        case Err::NotReady: return "not ready";
    }
    return "?";
}

Fs::Fs(BlockDevice& dev, std::uint32_t block_count, std::uint32_t grown_blocks)
    : dev_(dev), blocks_(block_count), grown_(grown_blocks > block_count ? grown_blocks : 0) {
    cfg_.context = &dev_;
    cfg_.read = cb_read;
    cfg_.prog = cb_prog;
    cfg_.erase = cb_erase;
    cfg_.sync = cb_sync;
    cfg_.read_size = 2;
    cfg_.prog_size = 2;            // half-word programming
    cfg_.block_size = kBlockSize;
    cfg_.block_count = blocks_;
    cfg_.block_cycles = 500;       // wear levelling: relocate a metadata pair after 500 erases
    cfg_.cache_size = kCache;
    cfg_.lookahead_size = kLookahead;
    cfg_.inline_max = kInlineMax;
    cfg_.read_buffer = rcache_;
    cfg_.prog_buffer = pcache_;
    cfg_.lookahead_buffer = look_;
    fcfg_.buffer = fcache_;
}

Err Fs::mount() {
    if (mounted_) return Err::Ok;
    changed();
    // littlefs takes the size from the superblock only when the config says 0, and then trusts any count it finds; an
    // explicit count refuses a mismatch (LFS_ERR_INVAL) after reading just the superblock pair. So: the base size, then
    // the grown one.
    cfg_.block_count = blocks_;
    int r = lfs_mount(&lfs_, &cfg_);
    if (r == LFS_ERR_INVAL && grown_ != 0) {
        cfg_.block_count = grown_;
        r = lfs_mount(&lfs_, &cfg_);
    }
    const Err e = map(r);
    mounted_ = e == Err::Ok;
    return e;
}

Err Fs::format(std::uint32_t blocks) {
    unmount();
    changed();
    if (blocks == 0) blocks = blocks_;
    if (blocks != blocks_ && blocks != grown_) return Err::Invalid;
    cfg_.block_count = blocks;
    return map(lfs_format(&lfs_, &cfg_));
}

Err Fs::grow() {
    if (!mounted_) return Err::NotReady;
    if (grown_ == 0) return Err::Invalid;
    if (lfs_.block_count == grown_) return Err::Ok;
    changed();
    const Err e = map(lfs_fs_grow(&lfs_, grown_));
    if (e != Err::Ok) {   // lfs_fs_grow sets the new size before its commit: let the superblock decide it again
        unmount();
        mount();
    }
    return e;
}

Err Fs::mount_or_format(bool& formatted) {
    formatted = false;
    if (mount() == Err::Ok) return Err::Ok;
    formatted = true;
    if (Err e = format(); e != Err::Ok) return e;
    return mount();
}

void Fs::unmount() {
    if (mounted_) lfs_unmount(&lfs_);
    mounted_ = false;
    changed();
}

Err Fs::open(lfs_file_t& f, const char* path, int flags) {
    if (!mounted_) return Err::NotReady;
    if (flags & LFS_O_WRONLY) changed();          // every write path opens for writing first
    return map(lfs_file_opencfg(&lfs_, &f, path, flags, &fcfg_));
}

Err Fs::read_file(const char* path, std::span<std::uint8_t> out, std::size_t& n) {
    n = 0;
    lfs_file_t f{};
    if (Err e = open(f, path, LFS_O_RDONLY); e != Err::Ok) return e;
    const lfs_soff_t sz = lfs_file_size(&lfs_, &f);
    Err r = Err::Ok;
    if (sz < 0) r = map(sz);
    else if (std::size_t(sz) > out.size()) { n = std::size_t(sz); r = Err::TooBig; }
    else if (sz > 0) {
        const lfs_ssize_t got = lfs_file_read(&lfs_, &f, out.data(), lfs_size_t(sz));
        if (got < 0) r = map(got);
        else {
            n = std::size_t(got);
            if (got != sz) r = Err::Corrupt;   // short read: the file is not what its size says
        }
    }
    lfs_file_close(&lfs_, &f);
    return r;
}

Err Fs::read_at(const char* path, std::uint32_t off, std::span<std::uint8_t> out, std::size_t& n) {
    n = 0;
    lfs_file_t f{};
    if (Err e = open(f, path, LFS_O_RDONLY); e != Err::Ok) return e;
    Err r = Err::Ok;
    const lfs_soff_t sz = lfs_file_size(&lfs_, &f);
    if (sz < 0) r = map(sz);
    else if (off < std::uint32_t(sz) && !out.empty()) {
        const lfs_soff_t at = lfs_file_seek(&lfs_, &f, lfs_soff_t(off), LFS_SEEK_SET);
        if (at < 0) r = map(at);
        else {
            const lfs_ssize_t got = lfs_file_read(&lfs_, &f, out.data(), lfs_size_t(out.size()));
            if (got < 0) r = map(got);
            else n = std::size_t(got);
        }
    }
    lfs_file_close(&lfs_, &f);
    return r;
}

Err Fs::write_file(const char* path, std::span<const std::uint8_t> data) {
    char tmp[kPathMax];
    if (!tmp_name(path, tmp)) return Err::Invalid;
    lfs_file_t f{};
    if (Err e = open(f, tmp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC); e != Err::Ok) return e;
    Err r = Err::Ok;
    if (!data.empty()) {
        const lfs_ssize_t w = lfs_file_write(&lfs_, &f, data.data(), lfs_size_t(data.size()));
        if (w < 0) r = map(w);
        else if (std::size_t(w) != data.size()) r = Err::NoSpace;
    }
    const int c = lfs_file_close(&lfs_, &f);          // commits the file
    changed();                                        // a used_blocks() asked while the file was open is stale now
    if (r == Err::Ok) r = map(c);
    if (r == Err::Ok) r = map(lfs_rename(&lfs_, tmp, path));
    if (r != Err::Ok) lfs_remove(&lfs_, tmp);
    return r;
}

Err Fs::write_chunks(const char* path, std::size_t total, Fill fill, void* ctx) {
    char tmp[kPathMax];
    if (!tmp_name(path, tmp)) return Err::Invalid;
    lfs_file_t f{};
    if (Err e = open(f, tmp, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC); e != Err::Ok) return e;
    Err r = Err::Ok;
    std::uint8_t buf[kChunk];
    for (std::size_t off = 0; off < total && r == Err::Ok; off += kChunk) {
        const std::size_t n = total - off < kChunk ? total - off : kChunk;
        fill(ctx, off, {buf, n});
        const lfs_ssize_t w = lfs_file_write(&lfs_, &f, buf, lfs_size_t(n));
        if (w < 0) r = map(w);
        else if (std::size_t(w) != n) r = Err::NoSpace;
    }
    const int c = lfs_file_close(&lfs_, &f);          // commits the file
    changed();                                        // the fill may have asked used_blocks() mid-write
    if (r == Err::Ok) r = map(c);
    if (r == Err::Ok) r = map(lfs_rename(&lfs_, tmp, path));
    if (r != Err::Ok) lfs_remove(&lfs_, tmp);
    return r;
}

Err Fs::append_file(const char* path, std::span<const std::uint8_t> data) {
    lfs_file_t f{};
    if (Err e = open(f, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND); e != Err::Ok) return e;
    Err r = Err::Ok;
    const lfs_ssize_t w = lfs_file_write(&lfs_, &f, data.data(), lfs_size_t(data.size()));
    if (w < 0) r = map(w);
    else if (std::size_t(w) != data.size()) r = Err::NoSpace;
    const int c = lfs_file_close(&lfs_, &f);
    changed();
    return r != Err::Ok ? r : map(c);
}

Err Fs::remove(const char* path) {
    if (!mounted_) return Err::NotReady;
    changed();
    return map(lfs_remove(&lfs_, path));
}
Err Fs::rename(const char* from, const char* to) {
    if (!mounted_) return Err::NotReady;
    changed();
    return map(lfs_rename(&lfs_, from, to));
}

Err Fs::mkdir(const char* path) {
    if (!mounted_) return Err::NotReady;
    changed();
    const Err e = map(lfs_mkdir(&lfs_, path));
    return e == Err::Exists ? Err::Ok : e;
}

Err Fs::size_of(const char* path, std::size_t& n) {
    n = 0;
    if (!mounted_) return Err::NotReady;
    lfs_info i;
    const int r = lfs_stat(&lfs_, path, &i);
    if (r < 0) return map(r);
    n = i.size;
    return Err::Ok;
}

bool Fs::exists(const char* path) {
    lfs_info i;
    return mounted_ && lfs_stat(&lfs_, path, &i) >= 0;
}

Err Fs::list_raw(const char* dir, Visit v, void* ctx) {
    if (!mounted_) return Err::NotReady;
    lfs_dir_t d{};
    if (Err e = map(lfs_dir_open(&lfs_, &d, dir)); e != Err::Ok) return e;
    Err r = Err::Ok;
    for (;;) {
        lfs_info li;
        const int g = lfs_dir_read(&lfs_, &d, &li);
        if (g < 0) { r = map(g); break; }
        if (g == 0) break;
        if (!std::strcmp(li.name, ".") || !std::strcmp(li.name, "..") || is_tmp(li.name)) continue;
        Info i{};
        std::memcpy(i.name, li.name, sizeof i.name);
        i.size = li.size;
        i.dir = li.type == LFS_TYPE_DIR;
        if (!v(ctx, i)) break;
    }
    lfs_dir_close(&lfs_, &d);
    return r;
}

Err Fs::clean_tmp(const char* dir) {
    if (!mounted_) return Err::NotReady;
    const std::size_t dl = std::strlen(dir);
    for (;;) {
        char victim[kPathMax];
        bool found = false;
        lfs_dir_t d{};
        if (Err e = map(lfs_dir_open(&lfs_, &d, dir)); e != Err::Ok) return e;
        lfs_info li;
        while (lfs_dir_read(&lfs_, &d, &li) > 0) {
            if (!is_tmp(li.name)) continue;
            const std::size_t nl = std::strlen(li.name);
            const bool root = dl == 1 && dir[0] == '/';
            if (dl + 1 + nl + 1 > kPathMax) continue;
            std::memcpy(victim, dir, dl);
            std::size_t p = dl;
            if (!root) victim[p++] = '/';
            std::memcpy(victim + p, li.name, nl + 1);
            found = true;
            break;
        }
        lfs_dir_close(&lfs_, &d);
        if (!found) return Err::Ok;
        changed();
        if (Err e = map(lfs_remove(&lfs_, victim)); e != Err::Ok) return e;
    }
}

std::uint32_t Fs::used_blocks() {
    if (!mounted_) return 0;
    if (used_known_) return used_;
    const lfs_ssize_t n = lfs_fs_size(&lfs_);
    if (n < 0) return 0;                          // a failed walk is not cached
    used_ = std::uint32_t(n);
    used_known_ = true;
    return used_;
}

}  // namespace k2::fs
