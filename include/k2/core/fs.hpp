// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::fs: a thin C++ wrapper over littlefs (third_party/littlefs, v2.11.0) with static buffers only.
// One Fs owns one lfs_t and its caches over a BlockDevice (the real flash in src/drivers/fs_flash.cpp, a RAM
// disk in the host tests). The helpers are whole-file operations, so no caller keeps a file open:
//   read_file    whole file into a caller buffer
//   write_file   atomic replace: written to "<path>.tmp", then renamed over the target
//   append_file  one append, committed (synced) before it returns
//   list         directory walk (".tmp" leftovers skipped), callback(info) returns false to stop
// littlefs commits metadata with a checksum and a file is replaced only by its rename, so a power cut
// leaves the old content or the new one, never a mix. Main loop only, never from an ISR.
#pragma once
#include "k2/core/lfs_cfg.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

extern "C" {
#include "lfs.h"
}

namespace k2::fs {

inline constexpr std::uint32_t kBlockSize = 2048;   // one flash page
inline constexpr std::uint32_t kCache = 256;        // read, program and file cache; also the inline file limit
inline constexpr std::uint32_t kLookahead = 16;     // allocator bitmap: 128 blocks
inline constexpr std::size_t kPathMax = 48;

enum class Err : std::uint8_t { Ok, NotFound, NoSpace, Corrupt, Io, Invalid, TooBig, Exists, NotReady };
const char* err_name(Err e);

// Block device seam. Return 0 on success, kIoFail or kIoCorrupt (a bad block: littlefs relocates it).
inline constexpr int kIoFail = -5, kIoCorrupt = -84;
class BlockDevice {
public:
    virtual int read(std::uint32_t block, std::uint32_t off, void* buf, std::uint32_t n) = 0;
    virtual int prog(std::uint32_t block, std::uint32_t off, const void* buf, std::uint32_t n) = 0;
    virtual int erase(std::uint32_t block) = 0;
protected:
    ~BlockDevice() = default;
};

struct Info {
    char name[LFS_NAME_MAX + 1];
    std::size_t size;
    bool dir;
};

class Fs {
public:
    Fs(BlockDevice& dev, std::uint32_t block_count);
    Fs(const Fs&) = delete;
    Fs& operator=(const Fs&) = delete;

    Err mount();
    Err format();
    // Mounts; when that fails formats and mounts again. `formatted` tells which happened.
    Err mount_or_format(bool& formatted);
    void unmount();
    bool ready() const { return mounted_; }

    // Ok only when the whole file was read (n = its size); a short read is Err::Corrupt (n = bytes got).
    Err read_file(const char* path, std::span<std::uint8_t> out, std::size_t& n);
    // Reads up to out.size() bytes starting at `off`; n = bytes got (0 at or past the end of the file).
    Err read_at(const char* path, std::uint32_t off, std::span<std::uint8_t> out, std::size_t& n);
    Err write_file(const char* path, std::span<const std::uint8_t> data);
    // write_file for content built on the way: `total` bytes, asked from `fill(off, out)` in contiguous ranges of at most
    // kChunk bytes, in order, then committed and renamed over the target like write_file. For a file too big to keep in RAM
    // beside everything else (a journal section of ~1.8 KB): nothing but the chunk lives on the stack.
    static constexpr std::size_t kChunk = 64;
    using Fill = void (*)(void* ctx, std::size_t off, std::span<std::uint8_t> out);
    Err write_chunks(const char* path, std::size_t total, Fill fill, void* ctx);
    Err append_file(const char* path, std::span<const std::uint8_t> data);
    Err remove(const char* path);
    Err rename(const char* from, const char* to);
    Err mkdir(const char* path);                 // an existing directory is fine
    Err size_of(const char* path, std::size_t& n);
    bool exists(const char* path);

    template <class F>
    Err list(const char* dir, F&& f) {
        return list_raw(dir, [](void* c, const Info& i) { return (*static_cast<F*>(c))(i); }, &f);
    }
    // Removes every ".tmp" leftover of an interrupted write_file in dir.
    Err clean_tmp(const char* dir);

    std::uint32_t block_count() const { return blocks_; }
    // Blocks in use. The first call after a change walks the whole tree (every metadata block, tens of ms on a full volume);
    // later calls answer from memory until a write, append, remove, rename, mkdir, format or remount through this object.
    std::uint32_t used_blocks();

    lfs_t& raw() { return lfs_; }

private:
    using Visit = bool (*)(void*, const Info&);
    Err list_raw(const char* dir, Visit v, void* ctx);
    Err open(lfs_file_t& f, const char* path, int flags);
    void changed() { used_known_ = false; }      // every mutating call: the cached used_blocks() figure is stale

    BlockDevice& dev_;
    std::uint32_t blocks_;
    bool mounted_ = false;
    bool used_known_ = false;
    std::uint32_t used_ = 0;
    lfs_t lfs_{};
    lfs_config cfg_{};
    lfs_file_config fcfg_{};
    alignas(4) std::uint8_t rcache_[kCache]{};
    alignas(4) std::uint8_t pcache_[kCache]{};
    alignas(4) std::uint8_t fcache_[kCache]{};
    alignas(4) std::uint8_t look_[kLookahead]{};
};

}  // namespace k2::fs
