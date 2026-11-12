// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Block device of the file system on the real flash (see include/k2/drivers/fs_flash.hpp).
#include "k2/core/log.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/drivers/fs_flash.hpp"

#include <cstring>

#include "k2/chargertest/report_card_store.hpp"
#include "k2/core/bbstore.hpp"
#include "k2/drivers/flash.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/core/fs_grow.hpp"
#include "k2/core/fs_migrate.hpp"
#include "k2/core/legacy_wipe.hpp"
#include "k2/core/scratch.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::fs {

static_assert(kFlashBlocks * kBlockSize == flash::kFsEnd - flash::kFsBase, "the base volume is exactly the file system range");
static_assert(kBlockSize == flash::kSectorSize, "one block is one flash sector (flash::fs_block_addr)");

namespace {

int status_to_io(flash::Status st) {
    if (st == flash::Status::Ok) return 0;
    return st == flash::Status::VerifyFailed ? kIoCorrupt : kIoFail;   // corrupt: littlefs relocates the block
}

class FlashDevice final : public BlockDevice {
public:
    // Blocks 90..144 live in the run; k2::flash refuses their erase and program until the run is opened on proof. A volume
    // of 111 blocks never asks for more; only a wide one (145, its shrink pending) reads the rest.
    int read(std::uint32_t block, std::uint32_t off, void* buf, std::uint32_t n) override {
        if (block >= kWideBlocks || off + n > kBlockSize) return kIoFail;
        std::memcpy(buf, flash::store_view(addr(block) + off), n);
        return 0;
    }
    int prog(std::uint32_t block, std::uint32_t off, const void* buf, std::uint32_t n) override {
        if (block >= kWideBlocks || off + n > kBlockSize) return kIoFail;
        const std::uint32_t t0 = systick::ticks();
        const int rc = status_to_io(flash::store_program(addr(block) + off, {static_cast<const std::uint8_t*>(buf), n}));
        paint::meter.add_fs(systick::ticks() - t0);   // time inside the flash write (the main loop waits for it)
        return rc;
    }
    int erase(std::uint32_t block) override {
        if (block >= kWideBlocks) return kIoFail;
        const std::uint32_t t0 = systick::ticks();
        const int rc = status_to_io(flash::store_erase(addr(block)));
        paint::meter.add_fs(systick::ticks() - t0);
        return rc;
    }

private:
    static std::uint32_t addr(std::uint32_t block) { return flash::fs_block_addr(block); }
};

// The ring seam on the real flash: memory-mapped reads, the guarded program and erase (the run must be open).
class FlashIo final : public ringlog::Io {
public:
    const std::uint8_t* view(std::uint32_t a) override { return flash::store_view(a); }
    bool program(std::uint32_t a, std::span<const std::uint8_t> d) override {
        const std::uint32_t t0 = systick::ticks();
        const bool ok = flash::store_program(a, d) == flash::Status::Ok;
        paint::meter.add_fs(systick::ticks() - t0);
        return ok;
    }
    bool erase_sector(std::uint32_t a) override {
        const std::uint32_t t0 = systick::ticks();
        const bool ok = flash::store_erase(a) == flash::Status::Ok;
        paint::meter.add_fs(systick::ticks() - t0);
        return ok;
    }
};

FlashDevice g_dev;
Fs g_fs(g_dev, kFlashBlocks, kGrownBlocks, kWideBlocks);
bool g_formatted = false;
FlashIo g_io;
ringlog::Ring g_cards(g_io, flash::kCardRingBase, flash::kCardRingSectors, report_card::kRingKind, report_card::kRingReserve);
ringlog::Ring g_bb(g_io, flash::kBbRingBase, flash::kBbRingSectors, bbstore::kRingKind);
bool g_rings = false;

void init_rings() {
    if (g_rings || !g_fs.ready() || !g_fs.grown() || !flash::fs_run_open()) return;
    g_cards.scan();
    g_bb.scan();
    g_rings = true;
    K2LOG("[RING] cards: live=%u blank=%u dirty=%u  bb: live=%u blank=%u dirty=%u", g_cards.live(), g_cards.blank(), g_cards.dirty(),
          g_bb.live(), g_bb.blank(), g_bb.dirty());
}

// The real flash for the grow sequence: memory-mapped blank check, the guard's run switch.
struct GrowEnv {
    bool blank(std::uint32_t sector) {
        for (std::uint32_t a = sector; a < sector + flash::kSectorSize; a += 4)
            if (*reinterpret_cast<const volatile std::uint32_t*>(a) != 0xFFFFFFFFu) return false;
        return true;
    }
    void open_run() { flash::open_fs_run(); }
};

}  // namespace

Fs& instance() { return g_fs; }
bool formatted_at_boot() { return g_formatted; }
bool grown() { return flash::fs_run_open(); }
bool legacy_done() { return g_fs.exists(fsgrow::kLegacyMarker); }
ringlog::Ring& card_ring() { return g_cards; }
ringlog::Ring& bb_ring() { return g_bb; }
bool rings_ready() { return g_rings; }

namespace {

void log_migration(const fsmigrate::Result& r) {
    static constexpr const char* kName[] = {"none", "done", "deferred", "stuck", "failed"};
    K2LOG("[FS] ring layout move %s%s: used=%u above=%u passes=%u files=%u erased=%u%s err=%s", kName[unsigned(r.outcome)],
          r.resumed ? " (resumed)" : "", unsigned(r.used_blocks), unsigned(r.above), r.passes, r.files, r.erased,
          r.tight ? " tcap+log dropped" : "", err_name(r.err));
}

}  // namespace

Err init(bool& formatted) {
    GrowEnv env;
    Err e = fsgrow::mount(g_fs, env, formatted);
    if (e == Err::Ok && g_fs.wide()) {   // the layout before the rings: emptied above 111 blocks and shrunk, once
        const scratch::Lease lease("fs.migrate");
        const fsmigrate::Result r = fsmigrate::migrate(g_fs, g_io, {scratch::arena(), scratch::kSize});
        log_migration(r);
        if (!g_fs.ready()) e = g_fs.mount();
    }
    g_formatted = formatted;
    if (e == Err::Ok) {
        g_fs.clean_tmp("/");
        K2LOG("[FS] mounted%s blocks=%u used=%u", formatted ? " (formatted)" : "", unsigned(g_fs.block_count()), unsigned(g_fs.used_blocks()));
        if (formatted) K2LOG("[FS] new volume of %u blocks from 0x%08X formatted; the old volume (if any) is gone", unsigned(g_fs.block_count()),
                             unsigned(flash::kFsBase));
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
    static constexpr const char* kMarker = fsgrow::kLegacyMarker;
    if (g_fs.exists(kMarker)) return 0;
    if (grown()) return 0;                  // the run holds volume blocks: no page there is an old store any more
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

void grow() {
    GrowEnv env;
    const fsgrow::Result r = fsgrow::grow(g_fs, env);
    switch (r.outcome) {
    case fsgrow::Outcome::Grown:
        K2LOG("[FS] grown %u -> %u blocks%s used=%u", unsigned(kFlashBlocks), unsigned(g_fs.block_count()),
              r.resumed ? " (resumed after a cut)" : "", unsigned(g_fs.used_blocks()));
        break;
    case fsgrow::Outcome::AlreadyGrown:
        if (g_fs.wide()) K2LOG("[FS] %u blocks, the move into the ring layout waits: rings off", unsigned(g_fs.block_count()));
        break;
    case fsgrow::Outcome::NotBlank:
        K2LOG("[FS] grow skipped: 0x%08X not blank", unsigned(r.addr));
        break;
    case fsgrow::Outcome::LegacyPending:
        K2LOG("[FS] grow skipped: legacy wipe pending");
        break;
    case fsgrow::Outcome::Failed:
        K2LOG("[FS] grow failed: %s (stays at %u blocks, retried next boot)", err_name(r.err), unsigned(g_fs.block_count()));
        break;
    case fsgrow::Outcome::NotMounted:
        break;
    }
    init_rings();
}

}  // namespace k2::fs
