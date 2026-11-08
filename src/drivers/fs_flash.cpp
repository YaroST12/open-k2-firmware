// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Block device of the file system on the real flash (see include/k2/drivers/fs_flash.hpp).
#include "k2/core/log.hpp"
#include "k2/drivers/fs_flash.hpp"

#include <cstring>

#include "k2/drivers/flash.hpp"
#include "k2/core/legacy_wipe.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::fs {

static_assert(kFlashBlocks * kBlockSize == flash::kFsEnd - flash::kFsBase, "the volume is exactly the file system range");

namespace {

int status_to_io(flash::Status st) {
    if (st == flash::Status::Ok) return 0;
    return st == flash::Status::VerifyFailed ? kIoCorrupt : kIoFail;   // corrupt: littlefs relocates the block
}

class FlashDevice final : public BlockDevice {
public:
    int read(std::uint32_t block, std::uint32_t off, void* buf, std::uint32_t n) override {
        if (block >= kFlashBlocks || off + n > kBlockSize) return kIoFail;
        std::memcpy(buf, flash::store_view(addr(block) + off), n);
        return 0;
    }
    int prog(std::uint32_t block, std::uint32_t off, const void* buf, std::uint32_t n) override {
        if (block >= kFlashBlocks || off + n > kBlockSize) return kIoFail;
        return status_to_io(flash::store_program(addr(block) + off, {static_cast<const std::uint8_t*>(buf), n}));
    }
    int erase(std::uint32_t block) override {
        if (block >= kFlashBlocks) return kIoFail;
        return status_to_io(flash::store_erase(addr(block)));
    }

private:
    static std::uint32_t addr(std::uint32_t block) { return flash::kFsBase + block * kBlockSize; }
};

FlashDevice g_dev;
Fs g_fs(g_dev, kFlashBlocks);
bool g_formatted = false;

}  // namespace

Fs& instance() { return g_fs; }
bool formatted_at_boot() { return g_formatted; }

Err init(bool& formatted) {
    const Err e = g_fs.mount_or_format(formatted);
    g_formatted = formatted;
    if (e == Err::Ok) {
        g_fs.clean_tmp("/");
        K2LOG("[FS] mounted%s blocks=%u used=%u", formatted ? " (formatted)" : "", unsigned(kFlashBlocks), unsigned(g_fs.used_blocks()));
        if (formatted) K2LOG("[FS] new volume 0x%08X..0x%08X formatted; the old volume (if any) is gone", unsigned(flash::kFsBase), unsigned(flash::kFsEnd));
    } else {
        K2LOG("[FS] mount failed: %s", err_name(e));
    }
    return e;
}

namespace {

// The real flash for legacy::wipe: memory-mapped reads, the guarded erase (which re-checks the proof).
struct LegacyEnv {
    std::uint16_t read16(std::uint32_t a) const { return *reinterpret_cast<const volatile std::uint16_t*>(a); }
    flash::Status erase(std::uint32_t a) { return flash::store_erase(a); }
};

}  // namespace

unsigned wipe_legacy() {
    static constexpr char kMarker[] = "/.legacy";
    if (g_fs.exists(kMarker)) return 0;
    LegacyEnv env;
    const legacy::WipeResult r = legacy::wipe(env);
    if (r.err != flash::Status::Ok) {
        K2LOG("[FS] legacy wipe failed at 0x%08X", unsigned(r.failed_at));
        return r.erased;                    // no marker: tried again next boot
    }
    const std::uint8_t one = '1';
    g_fs.write_file(kMarker, {&one, 1});
    K2LOG("[FS] legacy stores wiped, %u sectors erased (only pages with our own signature)", unsigned(r.erased));
    return r.erased;
}

}  // namespace k2::fs
