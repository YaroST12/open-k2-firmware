// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store: the newest card per charger, one binary file each in /cards on the littlefs volume
// (k2/core/fs.hpp). Pure logic over k2::fs::Fs, host-tested by tests/test_report_card_store.cpp on a RAM disk
// that can lose power at any operation.
//
// File /cards/<fingerprint, 8 hex digits>.card, kFileSize = 210 bytes with v7 (older cards are 204 bytes for v6, 192 for v5,
// 176 for v4 and 158 for v3; all are read, zero-padded, and rewritten as v7 by the next save). Small file, so it lives inside
// the directory metadata and costs about 210 bytes of flash:
//     +0   "K2C1"   magic and format version
//     +4   stamp    u32, a counter that grows with every save: newest card = highest stamp
//     +8   record   the card as k2/chargertest/report_card.hpp frames it (4 byte header, kPayload=196 bytes, or the shorter payload of an older format, CRC16)
// A save replaces the file atomically (littlefs commit + rename), so a power cut leaves the old card or the
// new one. A card keeps its number (id), run count and name through re-tests; a rename keeps its place in
// the order. kMaxCards = 64 live cards; saving a new charger when full drops the one with the lowest stamp.
// A run whose results are identical to the stored card (same_results(), rule in report_card.hpp) is not stored
// as a second copy: the stored card gets its repeat count (times) and the time of the latest repeat, and the
// file is rewritten with a new stamp, so the charger still counts as the most recently tested.
// The in-RAM index (fingerprint, number, stamp per card, 768 bytes) is built on the first use and after each
// write by this object.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/core/fs.hpp"
#include "k2/chargertest/report_card.hpp"

namespace k2::report_card {

inline constexpr unsigned kMaxCards = 64;
inline constexpr std::size_t kFileSize = 8 + kMaxRecord;
inline constexpr std::size_t kFileSizeV6 = 204;  // v6 cards (payload 190) are 204 bytes
inline constexpr std::size_t kFileSizeV5 = 192;  // v5 cards (payload 178) are 192 bytes
inline constexpr std::size_t kFileSizeV4 = 176;  // v4 cards (payload 162) are 176 bytes
inline constexpr std::size_t kFileSizeV3 = 158;  // old v3 cards are 158 bytes
inline constexpr std::array<std::uint8_t, 4> kFileMagic{'K', '2', 'C', '1'};
inline constexpr const char* kDir = "/cards";

enum class Err : std::uint8_t { Ok, Invalid, NotFound, Flash, Full };

class Store {
public:
    explicit Store(fs::Fs& f) : fs_(f) {}

    // Newest first. Returns the number of cards written to out (at most out.size()).
    std::size_t list(std::span<Card> out) const {
        ensure();
        Order o = order();
        std::size_t n = 0;
        for (unsigned i = 0; i < o.n && n < out.size(); ++i)
            if (load(idx_[o.at[i]].fp, out[n])) ++n;
        return n;
    }
    // The card numbers, newest first (the order of list), without loading the cards.
    std::size_t numbers(std::span<std::uint16_t> out) const {
        ensure();
        Order o = order();
        std::size_t n = 0;
        for (unsigned i = 0; i < o.n && n < out.size(); ++i) out[n++] = idx_[o.at[i]].card_no;
        return n;
    }
    bool find(std::uint32_t fp, Card& c) const {
        ensure();
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].fp == fp) return load(fp, c);
        return false;
    }
    bool get(std::uint16_t card_no, Card& c) const {
        ensure();
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no == card_no) return load(idx_[i].fp, c);
        return false;
    }
    // Saves a finished run: keeps the number, user name and run count of an earlier card of the same
    // charger (see inherit()). c comes back with card_no/runs/name filled in.
    Err put(Card& c) {
        if (c.id.fp == 0) return Err::Invalid;
        ensure();
        Card prev;
        bool had = false;
        for (unsigned i = 0; i < n_ && !had; ++i) if (idx_[i].fp == c.id.fp) had = load(c.id.fp, prev);
        c.times = 1; c.last_boots = 0; c.last_uptime_s = 0;      // a new run is one run, never a repeat of itself
        std::uint32_t top_stamp = 0;
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].stamp > top_stamp) top_stamp = idx_[i].stamp;
        if (had && same_results(prev, c)) {                       // identical to the stored run: count it, keep one copy
            c = repeat_of(prev, c);
            return write(c, top_stamp + 1);
        }
        if (had) {
            c.card_no = prev.card_no;
            c.runs = prev.runs == 0xFFFF ? prev.runs : std::uint16_t(prev.runs + 1);
            inherit(c, prev);
        } else {
            if (n_ >= kMaxCards) {                           // full: the card saved longest ago makes room
                unsigned oldest = 0;
                for (unsigned i = 1; i < n_; ++i) if (idx_[i].stamp < idx_[oldest].stamp) oldest = i;
                if (Err e = drop(idx_[oldest].fp); e != Err::Ok) return e;
            }
            std::uint16_t top = 0;
            for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no > top) top = idx_[i].card_no;
            c.card_no = std::uint16_t(top + 1) ? std::uint16_t(top + 1) : std::uint16_t(1);
            c.runs = 1;
        }
        return write(c, top_stamp + 1);
    }
    Err rename(std::uint16_t card_no, std::string_view name) {
        if (!valid_name(name)) return Err::Invalid;
        Card c;
        if (!get(card_no, c)) return Err::NotFound;
        set_name(c, name);
        c.name_auto = false;
        return write(c, stamp_of(c.id.fp));
    }
    Err remove(std::uint16_t card_no) {
        Card c;
        if (!get(card_no, c)) return Err::NotFound;
        return drop(c.id.fp);
    }

    // Helper to create file paths for manual file operations
    static constexpr std::size_t kPathLen = 24;                    // "/cards/XXXXXXXX.card"
    static void make_path(std::uint32_t fp, char (&p)[kPathLen]) {
        std::memcpy(p, "/cards/00000000.card", 21);
        for (int k = 14; k >= 7; k--) { p[k] = "0123456789abcdef"[fp & 15]; fp >>= 4; }
    }

private:
    struct Ent { std::uint32_t fp = 0, stamp = 0; std::uint16_t card_no = 0; };
    struct Order { std::array<std::uint8_t, kMaxCards> at{}; unsigned n = 0; };

    fs::Fs& fs_;
    mutable std::array<Ent, kMaxCards> idx_{};
    mutable unsigned n_ = 0;
    mutable bool valid_ = false;
    static constexpr bool known_size(std::size_t n) { return n == kFileSize || n == kFileSizeV6 || n == kFileSizeV5 || n == kFileSizeV4 || n == kFileSizeV3; }
    static bool parse_name(const char* n, std::uint32_t& fp) {
        if (std::strlen(n) != 13 || std::memcmp(n + 8, ".card", 5) != 0) return false;
        std::uint32_t v = 0;
        for (int k = 0; k < 8; k++) {
            const char c = n[k];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return false;
            v = (v << 4) | std::uint32_t(d);
        }
        fp = v;
        return v != 0;
    }

    // Reads and checks the file of fp; the record bytes (header, payload, CRC) go to rec.
    bool read(std::uint32_t fp, std::array<std::uint8_t, kFileSize>& b) const {
        char path[kPathLen];
        make_path(fp, path);
        std::size_t n = 0;
        if (fs_.read_file(path, b, n) != fs::Err::Ok) return false;
        // Accept current (210 bytes), v6 (204), v5 (192), v4 (176) and old v3 (158) card files
        if (!known_size(n)) return false;
        if (std::memcmp(b.data(), kFileMagic.data(), 4) != 0) return false;
        // Zero-pad old v3..v6 cards to kFileSize; decode_card then migrates by the format byte (33)
        if (n != kFileSize) std::memset(b.data() + n, 0, kFileSize - n);
        return true;
    }

    bool load(std::uint32_t fp, Card& c) const {
        std::array<std::uint8_t, kFileSize> b{};
        if (!read(fp, b)) return false;
        // Old v3/v4 payloads are zero-padded to kPayload; decode migrates them and migrate_to_current
        // raises the in-memory card to the current format (fields the old format lacked stay zeroed)
        return decode_card({b.data() + 8 + 4, kPayload}, c) && (migrate_to_current(c), true) && c.id.fp == fp;
    }
    std::uint32_t stamp_of(std::uint32_t fp) const {
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].fp == fp) return idx_[i].stamp;
        return 1;
    }

    void ensure() const {
        if (valid_) return;
        n_ = 0;
        std::array<std::uint32_t, kMaxCards> fps{};
        unsigned nf = 0;
        fs_.list(kDir, [&](const fs::Info& i) {
            std::uint32_t fp;
            // Accept every card file size ever written (v3 158, v4 176, v5 192, v6 204, current 210 bytes)
            if (i.dir || !known_size(i.size) || !parse_name(i.name, fp)) return true;
            if (nf < fps.size()) fps[nf++] = fp;
            return nf < fps.size();
        });
        for (unsigned k = 0; k < nf; ++k) {
            Card c;
            std::array<std::uint8_t, kFileSize> b{};
            if (!read(fps[k], b) || !decode_card({b.data() + 8 + 4, kPayload}, c) || c.id.fp != fps[k]) continue;
            idx_[n_++] = {fps[k], detail::get32(&b[4]), c.card_no};
        }
        valid_ = true;
    }
    // Indexes into idx_, newest first (insertion sort, 64 entries at most).
    Order order() const {
        Order o;
        for (unsigned i = 0; i < n_; ++i) {
            unsigned j = o.n++;
            for (; j > 0 && idx_[o.at[j - 1]].stamp < idx_[i].stamp; --j) o.at[j] = o.at[j - 1];
            o.at[j] = std::uint8_t(i);
        }
        return o;
    }

    Err write(const Card& c, std::uint32_t stamp) {
        std::array<std::uint8_t, kFileSize> b{};
        std::memcpy(b.data(), kFileMagic.data(), 4);
        detail::put32(&b[4], stamp);
        encode_card(std::span<std::uint8_t>(b).subspan(8), c);
        valid_ = false;
        if (fs_.mkdir(kDir) != fs::Err::Ok) return Err::Flash;
        char path[kPathLen];
        make_path(c.id.fp, path);
        const fs::Err e = fs_.write_file(path, b);
        return e == fs::Err::Ok ? Err::Ok : e == fs::Err::NoSpace ? Err::Full : Err::Flash;
    }
    Err drop(std::uint32_t fp) {
        char path[kPathLen];
        make_path(fp, path);
        valid_ = false;
        const fs::Err e = fs_.remove(path);
        return e == fs::Err::Ok ? Err::Ok : e == fs::Err::NotFound ? Err::NotFound : Err::Flash;
    }
};

}  // namespace k2::report_card
