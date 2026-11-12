// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store: the newest card per charger, as records of the report-card ring log on raw flash
// (k2/core/ringlog.hpp, 14 sectors at flash::kCardRingBase). Pure logic over a ringlog::Ring, host-tested by
// tests/test_report_card_store.cpp on a RAM flash that can lose power at any program or erase.
//
// Records (ring type byte):
//   kRecCard  stamp u32, then the card as k2/chargertest/report_card.hpp frames it (4 byte header, kPayload, CRC16):
//             260 bytes, 274 with the ring's framing, 7 to a sector
//   kRecGone  fp u32: the charger's card was deleted (tombstone)
// The latest record of a fingerprint wins: a save, a re-test, a repeat or a rename appends a new kRecCard (a rename keeps
// the old stamp, so the card keeps its place), a delete appends a kRecGone. The stamp is a counter that grows with every
// save: newest card = highest stamp. kMaxCards = 64 live cards; saving a new charger when full appends the new card, then a
// kRecGone for the one with the lowest stamp. More live cards than that (a power cut between the two): the index keeps the
// 64 highest stamps, the rest are never listed and are not carried forward. Two cards with one number: a current-format
// card keeps it over an older-format one, then the higher stamp; the other is shown with one above the highest number in
// use, written only by its own next save (renumber()).
// A run whose results are identical to the stored card (same_results(), rule in report_card.hpp) is not stored as a
// second copy: the stored card gets its repeat count (times) and the time of the latest repeat, with a new stamp.
//
// No write here erases: an append that finds no room fails (Err::Full). maintain() makes the room, one sector erase per
// call, and only when the caller says erasing is fine now (no charger test, no PD Stress run, a quiet PD link): it erases
// a dirty sector, or, when fewer than kSpare sectors are blank, copies the live cards of one sector to the head (into the
// reserve if need be) and erases it. That sector is the oldest one when its cards fit the free room with one blank sector
// to spare, else the one with the fewest bytes to carry (then its tombstones go along too, see keep()). A power cut
// anywhere leaves the old record or the new one: a copy cut short is a torn record the scan skips, or a torn record
// header that closes the sector it was written into (the spare blank sector is where the next pass goes), a finished
// copy and its original are the same card. Two cuts in a row inside passes of a full ring can leave no room at all:
// saves are refused then (Err::Full, logged) until a factory wipe.
// The in-RAM index (fingerprint, stamp, record address, number per card, 768 bytes) is built on the first use and kept
// up to date by every write of this object.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/chargertest/report_card.hpp"
#include "k2/core/ringlog.hpp"

namespace k2::report_card {

inline constexpr unsigned kMaxCards = 64;
inline constexpr std::uint8_t kRingKind = 'C';                             // the ring's sector kind byte
inline constexpr std::uint8_t kRecCard = 1, kRecGone = 2;
inline constexpr std::size_t kRecBytes = 4 + kMaxRecord;                   // stamp + framed card: 260
// Blank sectors only maintain() writes: one to carry cards forward into, one it never fills, because a power cut that
// tears a record header closes the sector being written (see ringlog.hpp) and the next pass needs somewhere to go.
inline constexpr unsigned kRingReserve = 2;
inline constexpr unsigned kSpare = kRingReserve + 1;                        // blank sectors maintain() keeps when it may
static_assert(kRecBytes <= ringlog::kMaxPayload);
static_assert(ringlog::per_sector(kRecBytes) == 7, "seven cards to a sector");

enum class Err : std::uint8_t { Ok, Invalid, NotFound, Flash, Full };

class Store {
public:
    explicit Store(ringlog::Ring& r) : ring_(r) {}

    // Newest first. Returns the number of cards written to out (at most out.size()).
    std::size_t list(std::span<Card> out) const {
        ensure();
        Order o = order();
        std::size_t n = 0;
        for (unsigned i = 0; i < o.n && n < out.size(); ++i)
            if (load(idx_[o.at[i]], out[n])) ++n;
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
        const int i = by_fp(fp);
        return i >= 0 && load(idx_[unsigned(i)], c);
    }
    bool get(std::uint16_t card_no, Card& c) const {
        ensure();
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no == card_no) return load(idx_[i], c);
        return false;
    }
    std::size_t count() const { ensure(); return n_; }
    // Saves a finished run: keeps the number, user name and run count of an earlier card of the same
    // charger (see inherit()). c comes back with card_no/runs/name filled in.
    Err put(Card& c) {
        Card prev;
        bool had = false;
        return put(c, prev, had);
    }
    // The same save with the caller's buffer for the card it replaces: prev holds the stored card of the same
    // charger when `had` comes back true (read before the write, never modified by it), so a caller that wants
    // that card for a delta does not read it a second time or keep a second Card on the stack beside this frame.
    Err put(Card& c, Card& prev, bool& had) {
        had = false;
        if (c.id.fp == 0) return Err::Invalid;
        ensure();
        if (const int i = by_fp(c.id.fp); i >= 0) had = load(idx_[unsigned(i)], prev);
        c.times = 1; c.last_boots = 0; c.last_uptime_s = 0;      // a new run is one run, never a repeat of itself
        std::uint32_t top_stamp = 0;
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].stamp > top_stamp) top_stamp = idx_[i].stamp;
        if (had && same_results(prev, c)) {                       // identical to the stored run: count it, keep one copy
            fold_repeat(prev, c);
            return write(c, top_stamp + 1);
        }
        if (had) {
            c.card_no = prev.card_no;
            c.runs = prev.runs == 0xFFFF ? prev.runs : std::uint16_t(prev.runs + 1);
            inherit(c, prev);
            return write(c, top_stamp + 1);
        }
        std::uint16_t top = 0;
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no > top) top = idx_[i].card_no;
        c.card_no = std::uint16_t(top + 1) ? std::uint16_t(top + 1) : std::uint16_t(1);
        c.runs = 1;
        // Full: the new card first, then the tombstone of the card saved longest ago (a cut between the two leaves 65
        // live records, of which the index lists the newest 64).
        const bool full = n_ >= kMaxCards;
        const std::uint32_t oldest = full ? idx_[lowest()].fp : 0;
        if (const Err e = write(c, top_stamp + 1); e != Err::Ok) return e;
        if (full) gone(oldest);
        return Err::Ok;
    }
    Err rename(std::uint16_t card_no, std::string_view name) {
        if (!valid_name(name)) return Err::Invalid;
        Card c;
        if (!get(card_no, c)) return Err::NotFound;
        set_name(c, name);
        c.name_auto = false;
        return write(c, idx_[unsigned(by_fp(c.id.fp))].stamp);
    }
    Err remove(std::uint16_t card_no) {
        Card c;
        if (!get(card_no, c)) return Err::NotFound;
        return gone(c.id.fp);
    }

    // One maintenance step (see the header); `may_erase`: no charger test, no PD Stress run, a quiet link. Returns true
    // when it erased a sector.
    bool maintain(bool may_erase) {
        if (!may_erase || !ring_.scanned()) return false;
        if (const std::uint32_t d = ring_.first_dirty()) return ring_.erase(d);
        if (ring_.blank() >= kSpare) return false;
        ensure();
        const std::uint32_t v = victim();
        if (v == 0) return false;                       // nothing can be given back without losing a card
        const bool tail = v == ring_.tail();
        bool ok = true;
        ring_.each_in(v, [&](const ringlog::Record& r) {
            const int i = keep(r, tail);
            if (i == kDrop) return true;
            if (!ring_.append(r.type, r.payload(), {}, true)) { ok = false; return false; }
            if (i >= 0) idx_[unsigned(i)].at = ring_.last();
            return true;
        });
        if (!ok) { valid_ = false; return false; }   // no room even in the reserve: nothing erased
        return ring_.erase(v);
    }
    // Blank sectors short of kSpare, or a dirty sector: maintain() has work.
    bool needs_maintenance() const { return ring_.scanned() && (ring_.first_dirty() != 0 || ring_.blank() < kSpare); }
    // Forget the index: the ring was rescanned or erased underneath (the factory wipe).
    void invalidate() { valid_ = false; }

private:
    struct Ent { std::uint32_t fp = 0, stamp = 0, at = 0; std::uint16_t card_no = 0; bool current = false; };   // current: kFormat
    struct Order { std::array<std::uint8_t, kMaxCards> at{}; unsigned n = 0; };

    ringlog::Ring& ring_;
    mutable std::array<Ent, kMaxCards> idx_{};
    mutable unsigned n_ = 0;
    mutable bool valid_ = false;

    int by_fp(std::uint32_t fp) const {
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].fp == fp) return int(i);
        return -1;
    }

    // What maintain() carries forward out of a sector it erases: a card the index points at (returns its index entry),
    // and, out of any sector but the oldest, the tombstone of a charger with no card now (kGone), since an older record
    // of that charger may sit in an older sector. Anything else is dropped (kDrop).
    static constexpr int kDrop = -2, kGone = -1;
    int keep(const ringlog::Record& r, bool tail) const {
        if (r.type == kRecCard && r.len == kRecBytes) {
            for (unsigned i = 0; i < n_; ++i) if (idx_[i].at == r.at) return int(i);
            return kDrop;
        }
        return !tail && r.type == kRecGone && r.len == 4 && by_fp(detail::get32(r.data)) < 0 ? kGone : kDrop;
    }
    // Whether what sector `s` carries forward fits the head's rest and the blank sectors (the reserve too), records
    // packed in order as append() packs them; `slack`: one blank sector left untouched, the room for the next pass when
    // a power cut closes the sector this one writes.
    bool fits(std::uint32_t s, bool slack, std::uint32_t& need) const {
        constexpr std::uint32_t kCap = ringlog::kSector - ringlog::kSectorHeader;
        std::uint32_t room = ring_.head_free();
        unsigned blank = ring_.blank();
        if (slack) {
            if (blank == 0) return false;
            --blank;
        }
        bool ok = true;
        need = 0;
        const auto place = [&](std::uint32_t b) {
            need += b;
            if (b > room) {
                if (blank == 0) { ok = false; return; }
                --blank;
                room = kCap;
            }
            room -= b;
        };
        const bool tail = s == ring_.tail();
        ring_.each_in(s, [&](const ringlog::Record& r) {
            if (keep(r, tail) != kDrop) place(ringlog::record_bytes(r.len));
            return ok;
        }, false);
        return ok;
    }
    // The sector maintain() gives back: the oldest live one when its cards fit with slack, else the one that carries the
    // fewest bytes and fits with slack, else (after a power cut used the slack) the same without slack; 0 when none
    // fits. The head is never one.
    std::uint32_t victim() const {
        const std::uint32_t head = ring_.head(), tail = ring_.tail();
        std::uint32_t need = 0;
        if (tail == 0 || tail == head) return 0;   // one live sector: nothing older to give back
        if (fits(tail, true, need)) return tail;
        for (const bool slack : {true, false}) {
            std::uint32_t best = 0, best_need = 0;
            for (unsigned i = 0; i < ring_.sectors(); ++i) {
                const std::uint32_t s = ring_.sector_addr(i);
                if (ring_.state(i) != ringlog::Sec::Live || s == head || !fits(s, slack, need)) continue;
                if (best == 0 || need < best_need) { best = s; best_need = need; }
            }
            if (best) return best;
        }
        return 0;
    }
    unsigned lowest() const {
        unsigned lo = 0;
        for (unsigned i = 1; i < n_; ++i) if (idx_[i].stamp < idx_[lo].stamp) lo = i;
        return lo;
    }
    static const std::uint8_t* record_of(const std::uint8_t* payload) { return payload + 4; }

    bool load(const Ent& e, Card& c) const {
        const std::uint8_t* p = ring_view(e.at);
        if (!p) return false;
        if (!decode_card({record_of(p) + 4, kPayload}, c) || c.id.fp != e.fp) return false;
        migrate_to_current(c);
        c.card_no = e.card_no;   // the number the index gives it (renumber()), which the card's next save stores
        return true;
    }
    // The payload of the card record at `at` (mapped flash), nullptr when it is not one any more.
    const std::uint8_t* ring_view(std::uint32_t at) const {
        const std::uint8_t* p = nullptr;
        ring_.each_in(at - (at - ring_.base()) % ringlog::kSector, [&](const ringlog::Record& r) {
            if (r.at != at) return true;
            if (r.type == kRecCard && r.len == kRecBytes) p = r.data;
            return false;
        }, false);
        return p;
    }

    // Builds the index from the whole ring, oldest record first: the latest record per fingerprint, tombstones remove.
    void ensure() const {
        if (valid_) return;
        n_ = 0;
        ring_.each([this](const ringlog::Record& r) {
            if (r.type == kRecGone && r.len == 4) drop_ent(detail::get32(r.data));
            else if (r.type == kRecCard && r.len == kRecBytes) index_card(r);
            return true;
        });
        renumber();
        valid_ = true;
    }
    // One card record into the index: replaces the fingerprint's entry; while the index is full the lowest stamp of the
    // index and the new card is left out. noinline: the decoded Card lives on this frame only.
    [[gnu::noinline]] void index_card(const ringlog::Record& r) const {
        Card c;
        if (!decode_card({record_of(r.data) + 4, kPayload}, c) || c.id.fp == 0) return;
        const Ent e{c.id.fp, detail::get32(r.data), r.at, c.card_no, c.format >= kFormat};
        if (const int i = by_fp(e.fp); i >= 0) { idx_[unsigned(i)] = e; return; }
        if (n_ < kMaxCards) { idx_[n_++] = e; return; }
        const unsigned lo = lowest();
        if (e.stamp > idx_[lo].stamp) idx_[lo] = e;
    }
    void drop_ent(std::uint32_t fp) const {
        const int i = by_fp(fp);
        if (i < 0) return;
        idx_[unsigned(i)] = idx_[--n_];
    }
    // Two cards with one card number (an older build handed the number out again while it could not read an older-format
    // card): a current-format card keeps it over an older-format one, then the newer stamp wins; the other card is shown
    // with one above the highest number in use. In RAM only: the card's next save stores it (load() hands the number out).
    void renumber() const {
        std::uint16_t top = 0;
        for (unsigned i = 0; i < n_; ++i) if (idx_[i].card_no > top) top = idx_[i].card_no;
        const Order o = order();
        std::array<std::uint8_t, kMaxCards> r{};               // claim order: current format newest first, then the rest
        unsigned m = 0;
        for (const bool cur : {true, false})
            for (unsigned i = 0; i < o.n; ++i) if (idx_[o.at[i]].current == cur) r[m++] = o.at[i];
        for (unsigned i = 1; i < m; ++i) {
            Ent& e = idx_[r[i]];
            for (unsigned j = 0; j < i; ++j)
                if (idx_[r[j]].card_no == e.card_no) {
                    top = std::uint16_t(top + 1) ? std::uint16_t(top + 1) : top;
                    e.card_no = top;
                    break;
                }
        }
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
        std::array<std::uint8_t, kRecBytes> b{};
        detail::put32(&b[0], stamp);
        encode_card(std::span<std::uint8_t>(b).subspan(4), c);
        if (!ring_.append(kRecCard, b)) return ring_.scanned() ? Err::Full : Err::Flash;
        const Ent e{c.id.fp, stamp, ring_.last(), c.card_no, true};
        if (const int i = by_fp(c.id.fp); i >= 0) idx_[unsigned(i)] = e;
        else if (n_ < kMaxCards) idx_[n_++] = e;
        else idx_[lowest()] = e;
        return Err::Ok;
    }
    Err gone(std::uint32_t fp) {
        std::array<std::uint8_t, 4> b{};
        detail::put32(b.data(), fp);
        if (!ring_.append(kRecGone, b)) return ring_.scanned() ? Err::Full : Err::Flash;
        drop_ent(fp);
        return Err::Ok;
    }
};

}  // namespace k2::report_card
