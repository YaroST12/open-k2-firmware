// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the "wipe for factory" sequence (k2/core/factory_wipe.hpp): the sector list (the whole littlefs
// volume plus only the legacy pages that carry one of our old store signatures), the order of the steps, the
// stop rules on an erase or a verify failure, and that the native pages are never touched. The fake flash goes
// through the real guarded erase (k2::flash).
#include <cstring>
#include <vector>

#include "k2/core/factory_wipe.hpp"
#include "test_util.h"

using namespace k2;

namespace {

// Flash from the littlefs base up to the end of the flash, filled with data (not blank): an erase that is
// skipped or lands on the wrong page shows up, and the 0xA5 fill stands in for foreign content (it is no
// signature of any of our old stores).
struct FakeFlash {
    static constexpr std::uint32_t kBase = flash::kFsBase;
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(flash::kStoreEnd - kBase, 0xA5);
    std::vector<std::uint32_t> erased;                 // addresses that reached the "controller"
    std::vector<std::uint32_t> stuck;                  // sectors that report Ok but keep their data
    std::uint32_t fail_at = 0;                         // sector whose erase reports a controller error
    flash::Status fail_status = flash::Status::ProgramError;

    flash::Status erase(std::uint32_t addr) {
        erased.push_back(addr);
        if (addr == fail_at) return fail_status;
        for (std::uint32_t s : stuck)
            if (s == addr) return flash::Status::Ok;
        std::memset(&mem[addr - kBase], 0xFF, flash::kSectorSize);
        return flash::Status::Ok;
    }
    flash::Status program(std::uint32_t, std::uint16_t) { return flash::Status::Refused; }
    std::uint16_t read(std::uint32_t addr) const { return static_cast<std::uint16_t>(mem[addr - kBase] | (mem[addr - kBase + 1] << 8)); }
    bool blank_sector(std::uint32_t addr) const {
        for (std::uint32_t a = addr; a < addr + flash::kSectorSize; a++)
            if (mem[a - kBase] != 0xFF) return false;
        return true;
    }
    void put(std::uint32_t addr, std::initializer_list<std::uint8_t> bytes) {
        std::uint32_t i = addr - kBase;
        for (std::uint8_t b : bytes) mem[i++] = b;
    }
    // Leaves our old raw stores in five legacy sectors: the log snapshot (two sectors), the USB config, one
    // Black Box sector, one e-marker library sector.
    void stage_old_stores() {
        put(0x080E4000, {'K', '2', 'L', 'G', 0xB8, 0x0B, 0, 0});   // length 3000: spills into the second sector
        put(0x080E6000, {'K', '2', 'U', 'S', 0x01, 0xFE, 0, 0});
        put(0x080E8000, {'K', '2', 'B', 'B'});
        put(0x080FC800, {'K', '2', 'E', 'L'});
    }
    std::vector<std::uint8_t> range(std::uint32_t a, std::uint32_t b) const { return {mem.begin() + (a - kBase), mem.begin() + (b - kBase)}; }
};

// The wipe environment over the fake: erase through the guarded store erase, like the firmware.
struct FakeEnv {
    FakeFlash flash;
    std::vector<int> events;                           // 0 unmount, 1 erase, 2 blank check, 3 clear_backup
    int unmounts = 0, backups = 0;

    void unmount() { unmounts++; events.push_back(0); }
    std::uint16_t read16(std::uint32_t addr) const { return flash.read(addr); }
    k2::flash::Status erase(std::uint32_t addr) {
        events.push_back(1);
        return k2::flash::store_erase_guarded(flash, addr);
    }
    bool blank(std::uint32_t addr) { events.push_back(2); return flash.blank_sector(addr); }
    void clear_backup() { backups++; events.push_back(3); }
};

static_assert(factory_wipe::Env<FakeEnv>);

}  // namespace

static void test_sector_list() {
    CHECK_EQ(factory_wipe::kFsSectors, 90u);
    CHECK_EQ(factory_wipe::kLegacySectors, 56u);        // candidates: 0x080E4000..0x08100000 = 112 KB
    CHECK_EQ(factory_wipe::kMaxSectors, 146u);
    CHECK_EQ(factory_wipe::fs_sector_addr(0), 0x080B3000u);
    CHECK_EQ(factory_wipe::fs_sector_addr(89), 0x080DF800u);   // last littlefs sector, right below the native area
    CHECK_EQ(factory_wipe::fs_sector_addr(90), 0u);            // past the end: no address
    for (unsigned i = 0; i < factory_wipe::kFsSectors; i++) {
        const std::uint32_t a = factory_wipe::fs_sector_addr(i);
        CHECK_EQ(a % 0x800, 0u);
        CHECK_EQ(a >= 0x080B3000u && a < 0x080E0000u, 1);   // never the native area, the bootloader or the application
        CHECK_EQ(flash::store_sector_allowed(a), 1);        // the existing guard accepts every one of them
    }
    // Nothing of the native pages is erasable without a proof, whatever the list says.
    for (std::uint32_t a : {0x080E0000u, 0x080E3000u, 0x080E3800u}) CHECK_EQ(flash::store_sector_allowed(a), 0);
}

static void test_clean_run_erases_the_volume_and_our_pages_only() {
    FakeEnv env;
    env.flash.stage_old_stores();
    const std::vector<std::uint8_t> before = env.flash.range(0x080E0000, 0x08100000);
    const factory_wipe::Result r = factory_wipe::run(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(factory_wipe::Err::Ok));
    CHECK_EQ(r.sectors, 95u);                           // 90 volume sectors + log (2), usb cfg, Black Box, e-marker library
    CHECK_EQ(r.erased, 95u);
    CHECK_EQ(r.verified, 95u);
    CHECK_EQ(r.failed, 0u);
    CHECK_EQ(r.first_fail, 0u);
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(env.unmounts, 1);
    CHECK_EQ(env.backups, 1);
    CHECK_EQ(env.flash.erased.size(), 95u);
    CHECK_EQ(env.flash.erased.front(), 0x080B3000u);
    // The legacy pages go last, descending: the log's second sector before the sector that proves it.
    const std::vector<std::uint32_t> tail(env.flash.erased.end() - 5, env.flash.erased.end());
    CHECK_EQ(tail == std::vector<std::uint32_t>({0x080FC800, 0x080E8000, 0x080E6000, 0x080E4800, 0x080E4000}), 1);
    // order: unmount, then all erases, then all blank checks, then the backup registers
    CHECK_EQ(env.events.size(), 1u + 95 + 95 + 1);
    CHECK_EQ(env.events.front(), 0);
    CHECK_EQ(env.events.back(), 3);
    for (std::size_t i = 1; i <= 95; i++) CHECK_EQ(env.events[i], 1);
    for (std::size_t i = 96; i <= 190; i++) CHECK_EQ(env.events[i], 2);
    // The whole volume is 0xFF; the native area and every legacy page that is not ours are byte for byte unchanged.
    for (std::uint32_t a = flash::kFsBase; a < flash::kFsEnd; a++) CHECK_EQ(env.flash.mem[a - flash::kFsBase], 0xFF);
    const std::vector<std::uint8_t> after = env.flash.range(0x080E0000, 0x08100000);
    for (std::uint32_t a = 0x080E0000; a < 0x08100000; a += 0x800) {
        const bool wiped = a == 0x080E4000 || a == 0x080E4800 || a == 0x080E6000 || a == 0x080E8000 || a == 0x080FC800;
        const std::uint32_t o = a - 0x080E0000;
        CHECK_EQ(std::memcmp(&after[o], &before[o], 0x800) == 0, wiped ? 0 : 1);
        if (wiped) CHECK_EQ(env.flash.blank_sector(a), 1);
    }
}

static void test_fresh_unit_loses_only_the_volume() {
    FakeEnv env;                                        // no signature of ours anywhere
    const std::vector<std::uint8_t> before = env.flash.range(0x080E0000, 0x08100000);
    const factory_wipe::Result r = factory_wipe::run(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(factory_wipe::Err::Ok));
    CHECK_EQ(r.sectors, 90u);
    CHECK_EQ(r.erased, 90u);
    CHECK_EQ(env.flash.erased.size(), 90u);
    for (std::uint32_t a : env.flash.erased) CHECK_EQ(a >= flash::kFsBase && a < flash::kFsEnd, 1);
    CHECK_EQ(env.flash.range(0x080E0000, 0x08100000) == before, 1);
}

static void test_erase_failure_stops() {
    FakeEnv env;
    env.flash.stage_old_stores();
    env.flash.fail_at = factory_wipe::fs_sector_addr(10);
    const factory_wipe::Result r = factory_wipe::run(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(factory_wipe::Err::EraseFailed));
    CHECK_EQ(r.sectors, 95u);
    CHECK_EQ(r.erased, 10u);                               // the ten before the failing one
    CHECK_EQ(r.first_fail, factory_wipe::fs_sector_addr(10));
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(flash::Status::ProgramError));
    CHECK_EQ(env.flash.erased.size(), 11u);                // stopped at the failure, nothing after it
    CHECK_EQ(env.unmounts, 1);
    CHECK_EQ(env.backups, 0);                              // the registers stay: the wipe did not finish
    for (int e : env.events) CHECK_EQ(e == 2, 0);          // no verify pass after a failed erase
}

static void test_legacy_erase_failure_stops() {
    FakeEnv env;
    env.flash.stage_old_stores();
    env.flash.fail_at = 0x080E8000;
    const factory_wipe::Result r = factory_wipe::run(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(factory_wipe::Err::EraseFailed));
    CHECK_EQ(r.erased, 91u);                               // the volume and the e-marker sector before it
    CHECK_EQ(r.first_fail, 0x080E8000u);
    CHECK_EQ(env.backups, 0);
}

static void test_verify_failure_is_reported() {
    FakeEnv env;
    // A sector whose erase reports Ok but keeps its data: the guarded erase reads it back itself and
    // answers VerifyFailed, which stops the run as an erase failure.
    env.flash.stuck = {factory_wipe::fs_sector_addr(80)};
    const factory_wipe::Result r = factory_wipe::run(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(factory_wipe::Err::EraseFailed));
    CHECK_EQ(static_cast<int>(r.status), static_cast<int>(flash::Status::VerifyFailed));
    CHECK_EQ(r.erased, 80u);
    CHECK_EQ(r.first_fail, factory_wipe::fs_sector_addr(80));
    CHECK_EQ(env.backups, 0);
}

// An env whose erase claims Ok without checking, so only the separate verify pass can catch it.
struct TrustingEnv {
    FakeFlash flash;
    int backups = 0;
    void unmount() {}
    std::uint16_t read16(std::uint32_t addr) const { return flash.read(addr); }
    k2::flash::Status erase(std::uint32_t addr) { return flash.erase(addr); }
    bool blank(std::uint32_t addr) { return flash.blank_sector(addr); }
    void clear_backup() { backups++; }
};
static_assert(factory_wipe::Env<TrustingEnv>);

static void test_verify_pass_catches_a_lying_erase() {
    TrustingEnv env;
    env.flash.stage_old_stores();
    env.flash.stuck = {factory_wipe::fs_sector_addr(70), 0x080E8000};
    const factory_wipe::Result r = factory_wipe::run(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(factory_wipe::Err::VerifyFailed));
    CHECK_EQ(r.sectors, 95u);
    CHECK_EQ(r.erased, 95u);
    CHECK_EQ(r.verified, 93u);
    CHECK_EQ(r.failed, 2u);
    CHECK_EQ(r.first_fail, factory_wipe::fs_sector_addr(70));
    CHECK_EQ(env.backups, 0);                              // no DFU path after a failed verify
    // Even a trusting controller was only ever asked about the volume and our own pages.
    for (std::uint32_t a : env.flash.erased)
        CHECK_EQ((a >= flash::kFsBase && a < flash::kFsEnd) || a == 0x080E4000 || a == 0x080E4800 || a == 0x080E6000 || a == 0x080E8000 || a == 0x080FC800, 1);
}

int main() {
    test_sector_list();
    test_clean_run_erases_the_volume_and_our_pages_only();
    test_fresh_unit_loses_only_the_volume();
    test_erase_failure_stops();
    test_legacy_erase_failure_stops();
    test_verify_failure_is_reported();
    test_verify_pass_catches_a_lying_erase();
    return test_report("test_factory_wipe");
}
