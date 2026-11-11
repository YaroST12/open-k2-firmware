// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box store: compressed PD captures that survive power-off, one file each in /bb on the littlefs
// volume (k2/core/fs.hpp). Pure logic over k2::fs::Fs, host-tested by tests/test_bbstore.cpp on a RAM disk.
//
// File /bb/NNNNNNNN.bbe (NNNNNNNN = capture number, 8 decimal digits, counts up, so the directory order is
// the age order). At most kMaxEntries files; saving the next one deletes the oldest, also when the volume
// runs out of blocks. One file = at most 2044 bytes = one flash page of data, so the Black Box costs at
// most kMaxEntries pages (64 KB) of the volume. A save also keeps the volume's free blocks at or above the
// reserve (kJournalReserveBlocks, for the charger journal): it evicts its own oldest captures to get there, and
// returns Err::Full, writing nothing, when none is left to give back; that refusal comes before the compression and reads
// the free figure Fs caches, so a volume full of other data costs no LZ pass and no volume walk per capture. File layout:
//   0  "K2B1"          magic and format version
//   4  u16 packed_len  compressed bytes
//   6  u16 raw_len     bytes after decompression
//   8  u8  trigger     pdtrace::Trigger
//   9  u8  reserved    0xFF
//   10 u16 pre         bytes of the capture that precede the trigger
//   12 u16 boot        boot counter of the run that recorded it (newest boot + 1 at mount)
//   14 u32 at_s        seconds since that boot, at the trigger
//   18 u16 crc         CRC-16/CCITT over bytes 4..17 and the packed data (a format check; littlefs
//                      already makes the write atomic)
//   20 ...             packed data (LZ4 block, k2/core/lz.hpp)
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "k2/core/fs.hpp"
#include "k2/core/lz.hpp"

namespace k2::bbstore {

inline constexpr unsigned kMaxEntries = 32;
inline constexpr std::uint32_t kFileHeader = 20;
inline constexpr std::uint32_t kMaxFile = fs::kBlockSize - 4;                 // 2044: fits one data block
inline constexpr std::uint32_t kMaxPacked = kMaxFile - kFileHeader;           // 2024
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'B', '1'};
inline constexpr const char* kDir = "/bb";

// No writes after this long without PD traffic or a VBUS change.
inline constexpr std::uint32_t kIdleMs = 10u * 60u * 1000u;

// Free blocks of the volume the Black Box leaves alone: the charger journal writes its rows (up to about 14 blocks, plus
// the metadata copies of a compaction) while a run is live, and a journal call that finds no block is both slow (it
// works through the whole volume before giving up) and fatal for the run record (Err::Broken from then on).
inline constexpr std::uint32_t kJournalReserveBlocks = 20;
// A capture needs its data block and the metadata commit that names it.
inline constexpr std::uint32_t kWriteBlocks = 2;

// Full: the volume has no room above the reserve and there is no older capture left to give back (nothing is written).
enum class Err : std::uint8_t { Ok, TooBig, Flash, Full };

struct Meta {
    unsigned trigger = 0;
    unsigned pre = 0;
    std::uint32_t at_s = 0;
};

struct Entry {
    std::uint8_t trigger = 0;
    std::uint16_t pre = 0, raw_len = 0, packed_len = 0, boot = 0;
    std::uint32_t at_s = 0;
    std::span<const std::uint8_t> packed{};   // in flash
};

struct Stats {
    int entries = 0;
    std::uint32_t used_bytes = 0;    // bytes of the stored captures
    std::uint32_t free_bytes = 0;    // free blocks of the whole volume
};

constexpr std::uint16_t crc16(std::uint16_t crc, std::span<const std::uint8_t> d) {
    for (std::uint8_t b : d) {
        crc ^= std::uint16_t(b) << 8;
        for (int i = 0; i < 8; i++) crc = std::uint16_t(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    return crc;
}

// Tracks "nothing is happening": touch() on PD traffic or a VBUS change, idle() when the gap
// since the last one exceeds kIdleMs. Wrap safe.
class IdleGate {
public:
    void touch(std::uint32_t now_ms) { last_ = now_ms; }
    bool idle(std::uint32_t now_ms) const { return now_ms - last_ > kIdleMs; }
    std::uint32_t since(std::uint32_t now_ms) const { return now_ms - last_; }
private:
    std::uint32_t last_ = 0;
};

class Ring {
public:
    explicit Ring(fs::Fs& f) : fs_(f) {}

    // Counts the files, finds the next capture number and the boot number.
    void mount() {
        next_ = 1; boot_ = 0;
        bool any_boot = false;
        std::uint16_t max_boot = 0;
        std::array<std::uint32_t, kScan> q{};
        const std::size_t n = list(q);
        for (std::size_t i = 0; i < n; i++) {
            Entry e;
            if (q[i] >= next_) next_ = q[i] + 1;
            if (!load(q[i], e)) continue;
            if (!any_boot || e.boot > max_boot) { any_boot = true; max_boot = e.boot; }
        }
        boot_ = any_boot ? std::uint16_t(max_boot + 1) : std::uint16_t(0);
    }

    std::uint16_t next_boot() const { return boot_; }

    // Free blocks that a save never takes (default: the journal's room).
    void set_reserve(std::uint32_t blocks) { reserve_ = blocks; }
    std::uint32_t reserve() const { return reserve_; }

    // Evicts the oldest captures until the volume has the reserve free; returns how many went. append() does the same before
    // its own write, but only when a capture is saved: a ring that filled the volume under an older build (no reserve) stayed
    // full until then, and the charger journal met it full (hardware 2026-11-08: used=90/90 at a run's begin).
    unsigned make_room() {
        unsigned n = 0;
        while (free_blocks() < reserve_ && drop_oldest()) n++;
        return n;
    }

    // Compresses `raw` (through `scratch`, at least lz::bound(raw.size()) bytes) and stores it as the newest.
    Err append(const Meta& m, std::span<const std::uint8_t> raw, std::span<std::uint8_t> scratch) {
        if (raw.empty() || scratch.size() < lz::bound(raw.size())) return Err::TooBig;
        // No room above the reserve and no capture of ours to give back: Full before the compression. Cheap on purpose (the
        // free figure is Fs's cached one, plus one listing of /bb): a volume full of other data refuses every capture.
        if (free_blocks() < reserve_ + kWriteBlocks && count() == 0) return Err::Full;
        const std::size_t packed = lz::compress(raw, scratch);
        if (packed == 0 || packed > kMaxPacked) return Err::TooBig;
        std::memcpy(rd_.data(), kMagic.data(), 4);
        put16(&rd_[4], std::uint16_t(packed));
        put16(&rd_[6], std::uint16_t(raw.size()));
        rd_[8] = std::uint8_t(m.trigger);
        rd_[9] = 0xFF;
        put16(&rd_[10], std::uint16_t(m.pre));
        put16(&rd_[12], boot_);
        for (int i = 0; i < 4; i++) rd_[14 + i] = std::uint8_t(m.at_s >> (8 * i));
        std::memcpy(&rd_[kFileHeader], scratch.data(), packed);
        put16(&rd_[18], crc16(crc16(0xFFFF, {&rd_[4], 14}), {&rd_[kFileHeader], packed}));
        // The reserve first: the oldest captures go until the write leaves it free. Nothing to give back = Full, the disk untouched.
        while (free_blocks() < reserve_ + kWriteBlocks)
            if (!drop_oldest()) return Err::Full;
        if (fs_.mkdir(kDir) != fs::Err::Ok) return Err::Flash;
        if (count() >= kMaxEntries) drop_oldest();
        char path[kPathLen];
        make_path(next_, path);
        const std::span<const std::uint8_t> file{rd_.data(), kFileHeader + packed};
        for (;;) {
            const fs::Err e = fs_.write_file(path, file);
            if (e == fs::Err::Ok) { next_++; return Err::Ok; }
            if (e != fs::Err::NoSpace || !drop_oldest()) return Err::Flash;
        }
    }

    // Visits the valid entries from the oldest to the newest; e.packed is valid during the call only.
    template <class F>
    void each(F&& f) {
        std::array<std::uint32_t, kScan> q{};
        const std::size_t n = list(q);
        for (std::size_t i = 0; i < n; i++) {
            Entry e;
            if (load(q[i], e)) f(e);
        }
    }

    // Decompresses an entry into `out`; returns the byte count or lz::kError.
    std::size_t unpack(const Entry& e, std::span<std::uint8_t> out) const { return lz::decompress(e.packed, out); }

    Stats stats() {
        Stats st;
        std::array<std::uint32_t, kScan> q{};
        const std::size_t n = list(q);
        for (std::size_t i = 0; i < n; i++) {
            char path[kPathLen];
            make_path(q[i], path);
            std::size_t sz = 0;
            if (fs_.size_of(path, sz) == fs::Err::Ok) { st.entries++; st.used_bytes += std::uint32_t(sz); }
        }
        const std::uint32_t used = fs_.used_blocks();
        st.free_bytes = (fs_.block_count() > used ? fs_.block_count() - used : 0) * fs::kBlockSize;
        return st;
    }

    // Deletes every capture.
    Err clear() {
        Err e = Err::Ok;
        std::array<std::uint32_t, kScan> q{};
        const std::size_t n = list(q);
        for (std::size_t i = 0; i < n; i++) {
            char path[kPathLen];
            make_path(q[i], path);
            const fs::Err r = fs_.remove(path);
            if (r != fs::Err::Ok && r != fs::Err::NotFound) e = Err::Flash;
        }
        mount();
        return e;
    }

private:
    static constexpr std::size_t kScan = kMaxEntries + 8;       // more files than this are ignored (never written)
    static constexpr std::size_t kPathLen = 24;                  // "/bb/00000000.bbe"
    fs::Fs& fs_;
    std::array<std::uint8_t, kMaxFile> rd_{};
    std::uint32_t next_ = 1;
    std::uint16_t boot_ = 0;
    std::uint32_t reserve_ = kJournalReserveBlocks;

    std::uint32_t free_blocks() {
        const std::uint32_t used = fs_.used_blocks();
        return fs_.block_count() > used ? fs_.block_count() - used : 0;
    }
    static void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
    static std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }
    static void make_path(std::uint32_t seq, char (&p)[kPathLen]) {
        std::memcpy(p, "/bb/00000000.bbe", 17);
        for (int k = 11; k >= 4; k--) { p[k] = char('0' + seq % 10); seq /= 10; }
    }
    static bool parse_name(const char* n, std::uint32_t& seq) {
        if (std::strlen(n) != 12 || std::memcmp(n + 8, ".bbe", 4) != 0) return false;
        std::uint32_t v = 0;
        for (int k = 0; k < 8; k++) { if (n[k] < '0' || n[k] > '9') return false; v = v * 10 + std::uint32_t(n[k] - '0'); }
        seq = v;
        return v != 0;
    }

    // The capture numbers in ascending order; returns how many (at most q.size()).
    std::size_t list(std::span<std::uint32_t> q) {
        std::size_t n = 0;
        fs_.list(kDir, [&](const fs::Info& i) {
            std::uint32_t s;
            if (i.dir || !parse_name(i.name, s)) return true;
            if (n < q.size()) q[n++] = s;
            return n < q.size();
        });
        return n;
    }
    unsigned count() { std::array<std::uint32_t, kScan> q{}; return unsigned(list(q)); }

    bool drop_oldest() {
        std::array<std::uint32_t, kScan> q{};
        if (list(q) == 0) return false;
        char path[kPathLen];
        make_path(q[0], path);
        return fs_.remove(path) == fs::Err::Ok;
    }

    // Reads and checks one file into rd_; e.packed points into rd_.
    bool load(std::uint32_t seq, Entry& e) {
        char path[kPathLen];
        make_path(seq, path);
        std::size_t n = 0;
        if (fs_.read_file(path, rd_, n) != fs::Err::Ok || n < kFileHeader) return false;
        if (std::memcmp(rd_.data(), kMagic.data(), 4) != 0) return false;
        const std::uint16_t len = get16(&rd_[4]);
        if (len == 0 || len > kMaxPacked || kFileHeader + len != n) return false;
        if (crc16(crc16(0xFFFF, {&rd_[4], 14}), {&rd_[kFileHeader], len}) != get16(&rd_[18])) return false;
        e.packed_len = len;
        e.raw_len = get16(&rd_[6]);
        e.trigger = rd_[8];
        e.pre = get16(&rd_[10]);
        e.boot = get16(&rd_[12]);
        e.at_s = std::uint32_t(rd_[14]) | std::uint32_t(rd_[15]) << 8 | std::uint32_t(rd_[16]) << 16 | std::uint32_t(rd_[17]) << 24;
        e.packed = {&rd_[kFileHeader], len};
        return true;
    }
};

}  // namespace k2::bbstore
