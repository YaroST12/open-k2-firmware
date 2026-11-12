// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box store: compressed PD captures that survive power-off, one record each in the Black Box ring log on raw flash
// (k2/core/ringlog.hpp, 20 sectors at flash::kBbRingBase), apart from the littlefs volume and from the report cards. Pure
// logic over a ringlog::Ring, host-tested by tests/test_bbstore.cpp on a RAM flash that can lose power at any call.
//
// Record (ring type kRecCapture), payload:
//   0  u16 raw_len     bytes after decompression
//   2  u8  trigger     pdtrace::Trigger
//   3  u8  reserved    0xFF
//   4  u16 pre         bytes of the capture that precede the trigger
//   6  u16 boot        boot counter of the run that recorded it (newest boot + 1 at mount)
//   8  u32 at_s        seconds since that boot, at the trigger
//   12 ...             packed data (LZ4 block, k2/core/lz.hpp), at most kMaxPacked bytes (the ring's CRC covers it)
// A capture is never split: it takes the rest of the head sector or a fresh one. The ring keeps as many as fit (about two
// per sector at a typical 0.9 KB, one at the worst case); the newest kMaxEntries are the ones a boot reloads and the UI
// counts. append() never erases: with no blank sector left and no room in the head it returns Err::Full. maintain()
// erases one sector per call, only when the caller says erasing is fine now: a dirty sector, or the oldest live one
// (its captures are gone) while fewer than kSpare sectors are blank, so after an idle moment one capture always fits.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "k2/core/lz.hpp"
#include "k2/core/ringlog.hpp"

namespace k2::bbstore {

inline constexpr unsigned kMaxEntries = 32;
inline constexpr std::uint8_t kRingKind = 'B';
inline constexpr std::uint8_t kRecCapture = 1;
inline constexpr std::uint32_t kHeader = 12;
inline constexpr std::uint32_t kMaxPacked = ringlog::kMaxPayload - kHeader;   // 2010: one record fills a sector
inline constexpr unsigned kSpare = 1;                                         // blank sectors maintain() keeps

// No writes after this long without PD traffic or a VBUS change.
inline constexpr std::uint32_t kIdleMs = 10u * 60u * 1000u;

// Full: no blank sector and no room in the head (nothing is written; maintain() makes room when it may erase).
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
    std::span<const std::uint8_t> packed{};   // memory-mapped flash, valid until its sector is erased
};

struct Stats {
    int entries = 0;                 // stored captures, at most kMaxEntries counted
    std::uint32_t used_bytes = 0;    // bytes of all stored captures
    std::uint32_t free_bytes = 0;    // bytes the ring can still take without an erase
};

// The CRC of every ring record (k2/core/ringlog.hpp); other stores of this firmware use it under this name too.
using ringlog::crc16;

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
    explicit Ring(ringlog::Ring& r) : ring_(r) {}

    // Counts the captures and finds the boot number (the ring is scanned by its owner first).
    void mount() {
        boot_ = 0;
        bool any = false;
        std::uint16_t max_boot = 0;
        each_all([&](const Entry& e) {
            if (!any || e.boot > max_boot) { any = true; max_boot = e.boot; }
        });
        boot_ = any ? std::uint16_t(max_boot + 1) : std::uint16_t(0);
        recount();
    }
    bool ready() const { return ring_.scanned(); }

    std::uint16_t next_boot() const { return boot_; }

    // Compresses `raw` (through `scratch`, at least lz::bound(raw.size()) bytes) and stores it as the newest.
    Err append(const Meta& m, std::span<const std::uint8_t> raw, std::span<std::uint8_t> scratch) {
        if (raw.empty() || scratch.size() < lz::bound(raw.size())) return Err::TooBig;
        if (!ring_.scanned()) return Err::Flash;
        // No room for even a small capture: Full before the compression.
        if (ring_.free_bytes() < ringlog::record_bytes(kHeader + 16)) return Err::Full;
        const std::size_t packed = lz::compress(raw, scratch);
        if (packed == 0 || packed > kMaxPacked) return Err::TooBig;
        std::array<std::uint8_t, kHeader> h{};
        put16(&h[0], std::uint16_t(raw.size()));
        h[2] = std::uint8_t(m.trigger);
        h[3] = 0xFF;
        put16(&h[4], std::uint16_t(m.pre));
        put16(&h[6], boot_);
        for (int i = 0; i < 4; i++) h[8 + i] = std::uint8_t(m.at_s >> (8 * i));
        if (!ring_.append(kRecCapture, h, scratch.first(packed))) return Err::Full;
        count_++;
        used_ += std::uint32_t(kHeader + packed);
        return Err::Ok;
    }

    // Visits the newest kMaxEntries valid captures from the oldest to the newest; e.packed points into the flash.
    template <class F>
    void each(F&& f) const {
        unsigned skip = count_ > kMaxEntries ? count_ - kMaxEntries : 0;
        each_all([&](const Entry& e) {
            if (skip) { skip--; return; }
            f(e);
        });
    }

    // Decompresses an entry into `out`; returns the byte count or lz::kError.
    std::size_t unpack(const Entry& e, std::span<std::uint8_t> out) const { return lz::decompress(e.packed, out); }

    Stats stats() const {
        Stats st;
        st.entries = int(count_ < kMaxEntries ? count_ : kMaxEntries);
        st.used_bytes = used_;
        st.free_bytes = ring_.scanned() ? ring_.free_bytes() : 0;
        return st;
    }

    // One maintenance step (see the header); returns the captures it dropped, or -1 when it erased nothing.
    int maintain(bool may_erase) {
        if (!may_erase || !ring_.scanned()) return -1;
        if (const std::uint32_t d = ring_.first_dirty()) { ring_.erase(d); return 0; }
        if (ring_.blank() >= kSpare) return -1;
        const std::uint32_t t = ring_.tail();
        if (t == 0 || t == ring_.head()) return -1;
        unsigned n = 0;
        ring_.each_in(t, [&](const ringlog::Record& r) { n += r.type == kRecCapture; return true; }, false);
        ring_.erase(t);
        recount();
        return int(n);
    }
    bool needs_maintenance() const { return ring_.scanned() && (ring_.first_dirty() != 0 || ring_.blank() < kSpare); }

private:
    ringlog::Ring& ring_;
    std::uint16_t boot_ = 0;
    unsigned count_ = 0;
    std::uint32_t used_ = 0;

    static void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
    static std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }

    static bool decode(const ringlog::Record& r, Entry& e) {
        if (r.type != kRecCapture || r.len <= kHeader) return false;
        const std::uint8_t* p = r.data;
        e.raw_len = get16(p);
        e.trigger = p[2];
        e.pre = get16(p + 4);
        e.boot = get16(p + 6);
        e.at_s = std::uint32_t(p[8]) | std::uint32_t(p[9]) << 8 | std::uint32_t(p[10]) << 16 | std::uint32_t(p[11]) << 24;
        e.packed_len = std::uint16_t(r.len - kHeader);
        e.packed = {p + kHeader, e.packed_len};
        return true;
    }
    template <class F>
    void each_all(F&& f) const {
        if (!ring_.scanned()) return;
        ring_.each([&](const ringlog::Record& r) {
            Entry e;
            if (decode(r, e)) f(e);
            return true;
        });
    }
    void recount() {
        count_ = 0;
        used_ = 0;
        each_all([&](const Entry& e) { count_++; used_ += kHeader + e.packed_len; });
    }
};

}  // namespace k2::bbstore
