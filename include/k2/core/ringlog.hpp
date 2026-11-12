// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Append-only ring log on raw flash sectors: the store of the report cards and the Black Box captures (docs/ARCHITECTURE.md
// "Ring logs"). Pure logic over a three-call seam (Io: memory-mapped view, half-word program, sector erase), so the host
// tests run it on a RAM image that can lose power at any call (tests/test_ringlog.cpp); the real seam is the guarded
// k2::flash store API.
//
// Sector (2 KB), header 12 bytes:
//   +0 u16 magic kSectorMagic   +2 u8 kind (one letter per ring)   +3 u8 kVersion   +4 u32 seq (counts up per opened sector)
//   +8 u16 CRC-16/CCITT of bytes 0..7   +10 u16 0xFFFF
// Records follow back to back, each 12 bytes of header, the payload padded to even, then a CRC:
//   +0 u16 magic kRecordMagic   +2 u8 type   +3 u8 0xFF   +4 u16 len   +6 u16 ~len   +8 u32 seq (counts up per record)
//   +12 payload (len bytes, one 0xFF pad byte when odd)   then u16 CRC-16/CCITT of bytes 2..end of the padded payload
// The boot scan reads every sector: a valid header of this ring's kind = live (ordered by its seq, the lowest is the
// tail, the highest the head); all 0xFF = blank; anything else = dirty (a header torn by a power cut, an erase cut short,
// another ring's or another owner's data) and only ever erased. Inside a live sector a 0xFFFF magic is the free space; a
// header whose len does not match ~len closes the sector (nothing behind it is trusted); a CRC that does not match is a
// torn record, skipped by its length. append() never erases: it writes into the head, or opens a blank sector, and fails
// cleanly when there is none (beyond the reserve the caller keeps for itself). Erasing is the caller's maintenance, at
// moments it chooses (erase(), tail(), first_dirty()).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::ringlog {

// CRC-16/CCITT-FALSE step (init 0xFFFF by the caller), the CRC of every record on the volume.
constexpr std::uint16_t crc16(std::uint16_t crc, std::span<const std::uint8_t> d) {
    for (std::uint8_t b : d) {
        crc ^= std::uint16_t(b) << 8;
        for (int i = 0; i < 8; i++) crc = std::uint16_t(crc & 0x8000 ? crc << 1 ^ 0x1021 : crc << 1);
    }
    return crc;
}

inline constexpr std::uint32_t kSector = 0x800;
inline constexpr std::uint16_t kSectorMagic = 0x4C52;   // "RL"
inline constexpr std::uint16_t kRecordMagic = 0x4352;   // "RC"
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::uint32_t kSectorHeader = 12;
inline constexpr std::uint32_t kRecordHeader = 12;
inline constexpr std::uint32_t kRecordOverhead = kRecordHeader + 2;
inline constexpr std::uint32_t kMaxPayload = kSector - kSectorHeader - kRecordOverhead;   // 2022: one record per sector
inline constexpr unsigned kMaxSectors = 40;

constexpr std::uint32_t pad2(std::uint32_t n) { return (n + 1) & ~1u; }
constexpr std::uint32_t record_bytes(std::uint32_t len) { return kRecordOverhead + pad2(len); }
// Records of `len` payload bytes that fit one sector.
constexpr unsigned per_sector(std::uint32_t len) { return (kSector - kSectorHeader) / record_bytes(len); }

class Io {
public:
    virtual const std::uint8_t* view(std::uint32_t addr) = 0;                          // memory-mapped read
    virtual bool program(std::uint32_t addr, std::span<const std::uint8_t> d) = 0;     // even addr and length
    virtual bool erase_sector(std::uint32_t sector) = 0;                                // 2 KB, reads blank after
protected:
    ~Io() = default;
};

struct Record {
    std::uint32_t at = 0;                  // flash address of the record header
    std::uint32_t seq = 0;
    std::uint16_t len = 0;
    std::uint8_t type = 0;
    const std::uint8_t* data = nullptr;    // the payload, memory-mapped (valid until the sector is erased)
    std::span<const std::uint8_t> payload() const { return {data, len}; }
};

enum class Sec : std::uint8_t { Blank, Live, Dirty, Skip };

class Ring {
public:
    // `reserve`: blank sectors append() leaves alone unless asked (append(..., true)); the caller's room to carry live
    // records forward before it erases the tail.
    Ring(Io& io, std::uint32_t base, unsigned sectors, std::uint8_t kind, unsigned reserve = 0)
        : io_(io), base_(base), n_(sectors <= kMaxSectors ? sectors : kMaxSectors), kind_(kind), reserve_(reserve) {}

    // Reads every sector; bit i of `skip` set: sector i is not looked at and never used (Sec::Skip).
    void scan(std::uint64_t skip = 0);
    bool scanned() const { return scanned_; }

    // Appends one record of payload a + b (a.size() even). False when it does not fit (the reserve is not touched unless
    // use_reserve), the payload is too long, or a program failed (the sector is closed then, the record lost).
    bool append(std::uint8_t type, std::span<const std::uint8_t> a, std::span<const std::uint8_t> b = {}, bool use_reserve = false);
    std::uint32_t last() const { return last_; }   // address of the record the last successful append wrote

    // Visits the records oldest first; f(const Record&) returns false to stop. check = verify each CRC (a torn record is
    // skipped); false only for a second pass over records already checked.
    using Visit = bool (*)(void* ctx, const Record& r);
    void each_raw(Visit v, void* ctx, bool check = true) const;
    template <class F>
    void each(F&& f, bool check = true) const {
        each_raw([](void* c, const Record& r) { return (*static_cast<F*>(c))(r); }, &f, check);
    }
    // The records of one live sector only.
    template <class F>
    void each_in(std::uint32_t sector, F&& f, bool check = true) const {
        const int i = index(sector);
        if (i >= 0 && sec_[i] == Sec::Live) walk(unsigned(i), [](void* c, const Record& r) { return (*static_cast<F*>(c))(r); }, &f, check);
    }

    unsigned blank() const { return count(Sec::Blank); }
    unsigned live() const { return count(Sec::Live); }
    unsigned dirty() const { return count(Sec::Dirty); }
    std::uint32_t tail() const;          // address of the oldest live sector, 0 when none
    std::uint32_t head() const;          // address of the newest live sector, 0 when none
    std::uint32_t first_dirty() const;   // 0 when none
    // Erases one sector of the ring (blank after; dirty when the erase failed). The caller has carried forward what it
    // keeps from it.
    bool erase(std::uint32_t sector);
    // Bytes appends can still take: the blank sectors beyond the reserve and the rest of the head.
    std::uint32_t free_bytes() const;
    // Bytes left in the head sector (0 when there is none).
    std::uint32_t head_free() const;
    Sec state(unsigned i) const { return i < n_ ? sec_[i] : Sec::Skip; }
    std::uint32_t base() const { return base_; }
    unsigned sectors() const { return n_; }
    std::uint32_t sector_addr(unsigned i) const { return base_ + i * kSector; }

private:
    Io& io_;
    std::uint32_t base_;
    unsigned n_;
    std::uint8_t kind_;
    unsigned reserve_;
    bool scanned_ = false;
    std::array<Sec, kMaxSectors> sec_{};
    std::array<std::uint32_t, kMaxSectors> seq_{};
    std::array<std::uint16_t, kMaxSectors> fill_{};   // offset of the free space in a live sector (kSector = closed)
    std::uint32_t next_sec_ = 1, last_ = 0;
    mutable std::uint32_t next_rec_ = 1;   // above every record seq a walk has seen

    int index(std::uint32_t addr) const {
        if (addr < base_ || (addr - base_) % kSector != 0 || (addr - base_) / kSector >= n_) return -1;
        return int((addr - base_) / kSector);
    }
    unsigned count(Sec s) const;
    int newest() const;
    std::uint16_t walk(unsigned i, Visit v, void* ctx, bool check) const;
    bool blank_range(std::uint32_t addr, std::uint32_t len) const;
    bool open_sector(bool use_reserve);
};

}  // namespace k2::ringlog
