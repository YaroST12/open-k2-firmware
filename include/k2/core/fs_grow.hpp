// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Growing the littlefs volume from flash::kFsBlocks (90) to flash::kFsVolumeBlocks (111) into the run
// 0x080E4000..0x080FF7FF (k2/drivers/flash.hpp), only on proof. The rest of the run (blocks 111..144) holds the report-card
// and Black Box ring logs (k2/core/ringlog.hpp); the proof covers it too, so the rings start on blank sectors. Pure
// sequence over k2::fs::Fs and a small environment, host-tested on a RAM disk with power cuts (tests/test_fs_grow.cpp);
// the real environment is in src/drivers/fs_flash.cpp.
//
// mount(): mounts a volume of any size the Fs knows. A grown one (111) or a wide one (145, the layout before the rings,
// shrunk to 111 by k2/core/fs_migrate.hpp) opens the run (its blocks are ours: only a proven grow or a proven blank
// format ever wrote such a superblock). When nothing mounts, the format takes 111 blocks only if every page of the run
// reads blank (the run opened first), and writes the legacy marker: a blank run holds none of our old raw stores, and the
// only legacy candidate outside it is the kept top page. Otherwise the format takes 90 blocks.
//
// grow(), once per boot after the legacy import and wipe (k2::fs::wipe_legacy):
//   1. the volume already has 111 blocks (or 145, its shrink pending): open the run, drop a leftover intent, done
//      (AlreadyGrown)
//   2. no legacy marker: the run may still hold our old raw stores, wait for the wipe (LegacyPending)
//   3. no intent yet: every page of the run must read blank, else stop and name the first page that does not
//      (NotBlank); then commit the intent file /.grow (an atomic littlefs write inside the 90 blocks)
//   4. open the run, grow (one superblock commit), remove the intent (Grown)
// A power cut before the intent leaves the run untouched; after it, the next boot finds the intent and grows without
// asking the run again, since the only writes there since the proof were the grow's own (the grow's commit may compact
// the superblock pair into new blocks of the run before the cut). The superblock commit is atomic, so every boot sees
// 90 or 111 blocks and the same files, never a format.
#pragma once
#include <concepts>
#include <cstdint>

#include "k2/core/fs.hpp"
#include "k2/drivers/flash.hpp"

namespace k2::fsgrow {

inline constexpr char kLegacyMarker[] = "/.legacy";   // k2::fs::wipe_legacy() has run
inline constexpr char kIntent[] = "/.grow";           // the run was proven blank and the grow is under way

// bool blank(sector address): the whole 2 KB sector reads 0xFF. void open_run(): the flash guard lets the volume write
// the run from now on (flash::open_fs_run on the device).
template <class E>
concept Env = requires(E& e, std::uint32_t addr) {
    { e.blank(addr) } -> std::same_as<bool>;
    e.open_run();
};

// The first sector of the run (ascending) that does not read blank; 0 when the whole run is blank.
template <Env E>
std::uint32_t first_not_blank(E& e) {
    for (std::uint32_t i = flash::kFsBlocks; i < flash::kFsGrownBlocks; i++) {
        const std::uint32_t a = flash::fs_block_addr(i);
        if (!e.blank(a)) return a;
    }
    return 0;
}

template <Env E>
fs::Err mount(fs::Fs& f, E& e, bool& formatted) {
    formatted = false;
    if (f.mount() == fs::Err::Ok) {
        if (f.grown() || f.wide()) e.open_run();
        return fs::Err::Ok;
    }
    formatted = true;
    const bool blank = first_not_blank(e) == 0;
    if (blank) e.open_run();
    if (const fs::Err r = f.format(blank ? flash::kFsVolumeBlocks : flash::kFsBlocks); r != fs::Err::Ok) return r;
    if (const fs::Err r = f.mount(); r != fs::Err::Ok) return r;
    if (blank) {
        const std::uint8_t one = '1';
        f.write_file(kLegacyMarker, {&one, 1});
    }
    return fs::Err::Ok;
}

enum class Outcome : std::uint8_t { Grown, AlreadyGrown, NotBlank, LegacyPending, Failed, NotMounted };

struct Result {
    Outcome outcome = Outcome::NotMounted;
    std::uint32_t addr = 0;          // NotBlank: the first sector that is not blank
    fs::Err err = fs::Err::Ok;       // Failed: what the intent write or the grow answered
    bool resumed = false;            // Grown: an intent from an interrupted grow was found
};

template <Env E>
Result grow(fs::Fs& f, E& e) {
    Result r;
    if (!f.ready()) return r;
    if (f.grown() || f.wide()) {   // wide: an older grown volume whose move into the ring layout waits (fs_migrate.hpp)
        e.open_run();
        if (f.exists(kIntent)) f.remove(kIntent);
        r.outcome = Outcome::AlreadyGrown;
        return r;
    }
    if (!f.exists(kLegacyMarker)) {
        r.outcome = Outcome::LegacyPending;
        return r;
    }
    r.resumed = f.exists(kIntent);
    if (!r.resumed) {
        if ((r.addr = first_not_blank(e)) != 0) {
            r.outcome = Outcome::NotBlank;
            return r;
        }
        const std::uint8_t one = '1';
        if ((r.err = f.write_file(kIntent, {&one, 1})) != fs::Err::Ok) {
            r.outcome = Outcome::Failed;
            return r;
        }
    }
    e.open_run();
    if ((r.err = f.grow()) != fs::Err::Ok) {
        r.outcome = Outcome::Failed;
        return r;
    }
    f.remove(kIntent);
    r.outcome = Outcome::Grown;
    return r;
}

}  // namespace k2::fsgrow
