// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Append-only ring log on raw flash sectors, see include/k2/core/ringlog.hpp.
#include "k2/core/ringlog.hpp"

namespace k2::ringlog {

namespace {

std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }
std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
void put16(std::uint8_t* p, std::uint16_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
void put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; i++) p[i] = std::uint8_t(v >> (8 * i)); }

bool header_ok(const std::uint8_t* p, std::uint8_t kind) {
    return get16(p) == kSectorMagic && p[2] == kind && p[3] == kVersion && get16(p + 8) == crc16(0xFFFF, {p, 8});
}

}  // namespace

bool Ring::blank_range(std::uint32_t addr, std::uint32_t len) const {
    const std::uint8_t* p = io_.view(addr);
    for (std::uint32_t k = 0; k < len; k++)
        if (p[k] != 0xFF) return false;
    return true;
}

void Ring::scan(std::uint64_t skip) {
    next_sec_ = 1;
    next_rec_ = 1;
    last_ = 0;
    for (unsigned i = 0; i < n_; i++) {
        fill_[i] = std::uint16_t(kSector);
        seq_[i] = 0;
        if (skip >> i & 1u) { sec_[i] = Sec::Skip; continue; }
        const std::uint8_t* p = io_.view(sector_addr(i));
        if (header_ok(p, kind_)) {
            sec_[i] = Sec::Live;
            seq_[i] = get32(p + 4);
            if (seq_[i] >= next_sec_) next_sec_ = seq_[i] + 1;
            fill_[i] = walk(i, nullptr, nullptr, false);
        } else {
            sec_[i] = blank_range(sector_addr(i), kSector) ? Sec::Blank : Sec::Dirty;
        }
    }
    scanned_ = true;
}

// Walks the records of live sector i, visiting them when v is set; returns the offset of the free space (kSector when
// the sector is full or closed by a bad header). Also keeps next_rec_ above every record seen.
std::uint16_t Ring::walk(unsigned i, Visit v, void* ctx, bool check) const {
    const std::uint32_t a = sector_addr(i);
    const std::uint8_t* p = io_.view(a);
    std::uint32_t pos = kSectorHeader;
    while (pos + kRecordHeader <= kSector) {
        const std::uint8_t* h = p + pos;
        const std::uint16_t m = get16(h);
        if (m == 0xFFFF) return std::uint16_t(pos);
        const std::uint16_t len = get16(h + 4);
        if (m != kRecordMagic || std::uint16_t(len ^ get16(h + 6)) != 0xFFFF || len > kMaxPayload || pos + record_bytes(len) > kSector)
            return std::uint16_t(kSector);
        const std::uint32_t seq = get32(h + 8);
        if (seq >= next_rec_) next_rec_ = seq + 1;
        if (v) {
            const std::uint32_t body = kRecordHeader - 2 + pad2(len);
            const bool ok = !check || get16(h + 2 + body) == crc16(0xFFFF, {h + 2, body});
            if (ok && !v(ctx, Record{a + pos, seq, len, h[2], h + kRecordHeader})) return 0;
        }
        pos += record_bytes(len);
    }
    return std::uint16_t(pos > kSector ? kSector : pos);
}

void Ring::each_raw(Visit v, void* ctx, bool check) const {
    // live sectors by seq, oldest first (insertion sort, kMaxSectors at most)
    std::array<std::uint8_t, kMaxSectors> o{};
    unsigned m = 0;
    for (unsigned i = 0; i < n_; i++) {
        if (sec_[i] != Sec::Live) continue;
        unsigned j = m++;
        for (; j > 0 && seq_[o[j - 1]] > seq_[i]; --j) o[j] = o[j - 1];
        o[j] = std::uint8_t(i);
    }
    for (unsigned k = 0; k < m; k++)
        if (walk(o[k], v, ctx, check) == 0) return;   // the visitor stopped
}

unsigned Ring::count(Sec s) const {
    unsigned c = 0;
    for (unsigned i = 0; i < n_; i++) c += sec_[i] == s;
    return c;
}

int Ring::newest() const {
    int best = -1;
    for (unsigned i = 0; i < n_; i++)
        if (sec_[i] == Sec::Live && (best < 0 || seq_[i] > seq_[unsigned(best)])) best = int(i);
    return best;
}

std::uint32_t Ring::head() const {
    const int h = newest();
    return h < 0 ? 0 : sector_addr(unsigned(h));
}

std::uint32_t Ring::tail() const {
    int best = -1;
    for (unsigned i = 0; i < n_; i++)
        if (sec_[i] == Sec::Live && (best < 0 || seq_[i] < seq_[unsigned(best)])) best = int(i);
    return best < 0 ? 0 : sector_addr(unsigned(best));
}

std::uint32_t Ring::first_dirty() const {
    for (unsigned i = 0; i < n_; i++)
        if (sec_[i] == Sec::Dirty) return sector_addr(i);
    return 0;
}

bool Ring::erase(std::uint32_t sector) {
    const int i = index(sector);
    if (i < 0 || sec_[unsigned(i)] == Sec::Skip) return false;
    const bool ok = io_.erase_sector(sector);
    sec_[unsigned(i)] = ok ? Sec::Blank : Sec::Dirty;
    fill_[unsigned(i)] = std::uint16_t(kSector);
    return ok;
}

std::uint32_t Ring::free_bytes() const {
    const unsigned b = blank();
    std::uint32_t n = (b > reserve_ ? b - reserve_ : 0) * (kSector - kSectorHeader);
    const int h = newest();
    if (h >= 0) n += kSector - fill_[unsigned(h)];
    return n;
}

std::uint32_t Ring::head_free() const {
    const int h = newest();
    return h < 0 ? 0 : kSector - fill_[unsigned(h)];
}

// Opens a blank sector as the new head: the first blank one after the current head, so the ring goes round the range.
bool Ring::open_sector(bool use_reserve) {
    if (blank() <= (use_reserve ? 0u : reserve_)) return false;
    const int h = newest();
    unsigned i = h < 0 ? 0 : (unsigned(h) + 1) % n_;
    for (unsigned k = 0; k < n_ && sec_[i] != Sec::Blank; k++) i = (i + 1) % n_;
    std::array<std::uint8_t, kSectorHeader> hd{};
    put16(&hd[0], kSectorMagic);
    hd[2] = kind_;
    hd[3] = kVersion;
    put32(&hd[4], next_sec_);
    put16(&hd[8], crc16(0xFFFF, {hd.data(), 8}));
    hd[10] = hd[11] = 0xFF;
    if (!io_.program(sector_addr(i), hd)) {
        sec_[i] = Sec::Dirty;
        return false;
    }
    sec_[i] = Sec::Live;
    seq_[i] = next_sec_++;
    fill_[i] = std::uint16_t(kSectorHeader);
    return true;
}

bool Ring::append(std::uint8_t type, std::span<const std::uint8_t> a, std::span<const std::uint8_t> b, bool use_reserve) {
    if (!scanned_ || a.size() % 2 != 0) return false;
    const std::size_t len = a.size() + b.size();
    if (len > kMaxPayload) return false;
    const std::uint32_t need = record_bytes(std::uint32_t(len));
    int h = newest();
    if (h >= 0 && fill_[unsigned(h)] + need <= kSector && !blank_range(sector_addr(unsigned(h)) + fill_[unsigned(h)], need))
        fill_[unsigned(h)] = std::uint16_t(kSector);   // not blank where it should be (an erase cut short): closed
    if (h < 0 || fill_[unsigned(h)] + need > kSector) {
        if (!open_sector(use_reserve)) return false;
        h = newest();
        if (!blank_range(sector_addr(unsigned(h)) + fill_[unsigned(h)], need)) { fill_[unsigned(h)] = std::uint16_t(kSector); return false; }
    }
    const unsigned i = unsigned(h);
    const std::uint32_t at = sector_addr(i) + fill_[i];
    std::array<std::uint8_t, kRecordHeader> hd{};
    put16(&hd[0], kRecordMagic);
    hd[2] = type;
    hd[3] = 0xFF;
    put16(&hd[4], std::uint16_t(len));
    put16(&hd[6], std::uint16_t(~len));
    put32(&hd[8], next_rec_);
    std::uint16_t crc = crc16(0xFFFF, {hd.data() + 2, kRecordHeader - 2});
    crc = crc16(crc, a);
    crc = crc16(crc, b);
    // The odd last byte of b goes out with the pad byte and the CRC: every program is even in address and length.
    const std::size_t b_even = b.size() & ~std::size_t{1};
    std::array<std::uint8_t, 4> tr{};
    std::size_t tn = 0;
    if (b.size() != b_even) {
        tr[0] = b[b_even];
        tr[1] = 0xFF;
        crc = crc16(crc, {&tr[1], 1});
        tn = 2;
    }
    put16(&tr[tn], crc);
    tn += 2;
    fill_[i] = std::uint16_t(fill_[i] + need);   // a failed program below leaves a torn record the scan skips
    std::uint32_t pos = at;
    bool ok = io_.program(pos, hd);
    pos += kRecordHeader;
    if (ok && !a.empty()) { ok = io_.program(pos, a); pos += std::uint32_t(a.size()); }
    if (ok && b_even) { ok = io_.program(pos, b.first(b_even)); pos += std::uint32_t(b_even); }
    if (ok) ok = io_.program(pos, {tr.data(), tn});
    if (!ok) {
        fill_[i] = std::uint16_t(kSector);
        return false;
    }
    next_rec_++;
    last_ = at;
    return true;
}

}  // namespace k2::ringlog
