// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The binary K2LOG ring: every K2LOG call as one small entry, kept for the QR share record "LOG" (docs/QR_SHARE.md).
// Pure and allocation free; usb::log_packed feeds it, the Share log entry reads a snapshot.
//
// An entry is   len u8 | dt varint | id varint | one value per argument
// len is the body length (dt, id and the values), dt the milliseconds since the previous entry (32-bit wrap safe), id the
// K2LOG message id (tools/logdb.json) and the values follow the format's argument kinds (k2/core/log.hpp): a signed argument
// is a zigzag varint, an unsigned one a varint, a string a length byte and up to kStrMax bytes. The host knows each id's
// argument kinds from the version's logdb map, so the entry carries no types. Varints are 7 bits per byte, low group first,
// bit 7 set on every byte but the last. An entry whose arguments would pass kEntryMax stops at the last one that fits; an
// argument takes the room it needs (a varint at most 5 bytes, a string its length up to kStrMax), not a worst case.
//
// The ring drops whole oldest entries when full and keeps the time of the entry it dropped last (base_ms), so the time of
// any entry is base_ms plus the dt of every entry up to and including it.
#pragma once
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/log.hpp"

namespace k2 {

struct BinLogSnapshot {
    std::size_t len;         // bytes copied: whole entries, oldest first
    std::uint32_t base_ms;   // the time of the entry before the first one copied (0 at boot): add each dt to walk the times
    bool older;              // entries older than the first one copied were dropped or did not fit
};

template <std::size_t N>
class BinLog {
public:
    static constexpr std::size_t kEntryMax = 63;   // body bytes: len + body is one 64-byte debug-stream chunk at most
    static constexpr std::size_t kStrMax = 28;     // the longest interrogation words are 27 characters

    void push(std::uint32_t now_ms, std::uint16_t id, std::uint32_t sig, std::va_list ap) {
        std::uint8_t e[kEntryMax + 1];
        std::size_t n = 1;
        const auto varint = [&](std::uint32_t v) {
            for (; v >= 0x80; v >>= 7) e[n++] = static_cast<std::uint8_t>(v | 0x80);
            e[n++] = static_cast<std::uint8_t>(v);
        };
        varint(now_ms - last_ms_);
        varint(id);
        for (; sig; sig >>= 2) {
            if ((sig & 3) == logfmt::kString) {
                const char *s = va_arg(ap, const char *);
                std::size_t len = 0;
                while (s && s[len] && len < kStrMax) len++;
                if (n + 1 + len > sizeof e) break;   // the rest does not fit: the entry stops at the last argument that did
                e[n++] = static_cast<std::uint8_t>(len);
                for (std::size_t i = 0; i < len; i++) e[n++] = static_cast<std::uint8_t>(s[i]);
            } else {
                if (n + 5 > sizeof e) break;   // a varint takes up to 5 bytes
                if ((sig & 3) == logfmt::kSigned) {
                    const int v = va_arg(ap, int);
                    varint((static_cast<std::uint32_t>(v) << 1) ^ static_cast<std::uint32_t>(v >> 31));
                } else {
                    varint(va_arg(ap, std::uint32_t));
                }
            }
        }
        e[0] = static_cast<std::uint8_t>(n - 1);
        last_ms_ = now_ms;
        if (n > N) return;
        while (used_ + n > N) drop_oldest();
        for (std::size_t i = 0; i < n; i++) {
            buf_[head_] = e[i];
            head_ = (head_ + 1) % N;
        }
        used_ += n;
        pushed_++;
    }

    std::size_t size() const { return used_; }

    void clear() {
        base_ms_ = last_ms_;
        tail_abs_ += static_cast<std::uint32_t>(used_);
        dropped_ = pushed_;
        head_ = tail_ = used_ = 0;
        older_ = false;
    }

    // ---- Live reading (the debug stream, k2/core/dbg_stream.hpp). A reader keeps an absolute byte position (counting every byte
    // ever stored, so it survives the wrap and clear()) and the number of the entry there; the ring is the queue and a reader
    // that fell behind finds tail_abs() beyond its position, never stale bytes.
    std::uint32_t head_abs() const { return tail_abs_ + static_cast<std::uint32_t>(used_); }   // after the newest entry
    std::uint32_t tail_abs() const { return tail_abs_; }                                       // the oldest entry kept
    std::uint32_t entries_pushed() const { return pushed_; }
    std::uint32_t entries_dropped() const { return dropped_; }   // = the number of the oldest kept entry
    std::uint32_t base_ms() const { return base_ms_; }           // the time of the entry before the oldest kept one
    std::uint32_t last_ms() const { return last_ms_; }           // the time of the newest entry: the base of the next one

    // Whole entries from the absolute position `abs` (an entry boundary within [tail_abs(), head_abs()]) that fit `out`.
    std::size_t read(std::uint32_t abs, std::span<std::uint8_t> out) const {
        if (abs < tail_abs_ || abs >= head_abs()) return 0;
        std::size_t pos = (tail_ + (abs - tail_abs_)) % N, left = head_abs() - abs, n = 0;
        while (left) {
            const std::size_t whole = 1 + buf_[pos];
            if (n + whole > out.size()) break;
            for (std::size_t i = 0; i < whole; i++) out[n++] = buf_[(pos + i) % N];
            pos = (pos + whole) % N;
            left -= whole;
        }
        return n;
    }

    // The dt varint of a copied entry (`len dt id ...`): the milliseconds since the previous entry.
    static std::uint32_t dt_of(const std::uint8_t* entry) {
        std::uint32_t v = 0;
        for (unsigned shift = 0, i = 1; shift < 35; shift += 7, i++) {
            v |= static_cast<std::uint32_t>(entry[i] & 0x7F) << shift;
            if (!(entry[i] & 0x80)) break;
        }
        return v;
    }

    // Copies the newest whole entries that fit `out`, oldest first.
    BinLogSnapshot snapshot(std::span<std::uint8_t> out) const {
        std::size_t pos = tail_, left = used_;
        std::uint32_t base = base_ms_;
        while (left > out.size()) {
            base += dt_at(pos);
            const std::size_t whole = 1 + buf_[pos];
            pos = (pos + whole) % N;
            left -= whole;
        }
        for (std::size_t i = 0; i < left; i++) out[i] = buf_[(pos + i) % N];
        return {left, base, older_ || left != used_};
    }

private:
    std::uint32_t dt_at(std::size_t pos) const {   // the dt varint of the entry at `pos`
        std::uint32_t v = 0;
        for (unsigned shift = 0, i = 1; shift < 35; shift += 7, i++) {
            const std::uint8_t b = buf_[(pos + i) % N];
            v |= static_cast<std::uint32_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
        }
        return v;
    }
    void drop_oldest() {
        base_ms_ += dt_at(tail_);
        const std::size_t whole = 1 + buf_[tail_];
        tail_ = (tail_ + whole) % N;
        used_ -= whole;
        tail_abs_ += static_cast<std::uint32_t>(whole);
        dropped_++;
        older_ = true;
    }

    std::array<std::uint8_t, N> buf_{};
    std::size_t head_ = 0, tail_ = 0, used_ = 0;
    std::uint32_t base_ms_ = 0, last_ms_ = 0;
    std::uint32_t tail_abs_ = 0, pushed_ = 0, dropped_ = 0;
    bool older_ = false;
};

}  // namespace k2
