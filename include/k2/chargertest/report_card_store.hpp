// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card store: the newest card per charger, as records of the report-card ring log on raw flash
// (k2/core/ringlog.hpp, 22 sectors at flash::kCardRingBase). Pure logic over a ringlog::Ring, host-tested by
// tests/test_report_card_store.cpp on a RAM flash that can lose power at any program or erase.
//
// Records (ring type byte):
//   kRecCard  stamp u32, then the card as k2/chargertest/report_card.hpp frames it (4 byte header, kPayload, CRC16):
//             260 bytes, 274 with the ring's framing, 7 to a sector. A Deep card may carry its Deep block (the spec summary,
//             the SVOOC ceiling and the per-range sweep graphs, k2/chargertest/report_card_deep.hpp, at most 412 bytes) after
//             the card: 260 + the block, 2 to a sector at the most. The block is what the share link carries; it never
//             enters the 256 byte card, so every reader of the card is unchanged.
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
// Deep blocks: a rename or a repeat carries the block along (a repeat that brings a new one replaces it); a re-test without
// one drops it. The kMaxDeep newest blocks are never shed: maintain() carries an older card forward without its block, so
// the blocks give way to the cards when the ring needs room (12 blocks and 52 plain cards are 22 KB of the 34 KB the ring
// keeps in use, so that always fits); a save that finds no room for its block is retried without it (the card matters more).
// The in-RAM index (fingerprint, stamp, record address, number per card, 1 KB) is built on the first use and kept
// up to date by every write of this object.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/chargertest/report_card.hpp"
#include "k2/chargertest/report_card_deep.hpp"
#include "k2/core/ringlog.hpp"

namespace k2::report_card {

inline constexpr unsigned kMaxCards = 64;
inline constexpr std::uint8_t kRingKind = 'C';                             // the ring's sector kind byte
inline constexpr std::uint8_t kRecCard = 1, kRecGone = 2;
inline constexpr std::size_t kRecBytes = 4 + kMaxRecord;                   // stamp + framed card: 260 (+ the Deep block when it has one)
inline constexpr unsigned kMaxDeep = 12;                                    // cards that keep their Deep block (the newest)
// Blank sectors only maintain() writes: one to carry cards forward into, the rest it never fills, because a power cut that
// tears a record header closes the sector being written (see ringlog.hpp) and the next pass needs somewhere to go. Four, so
// three cuts in a row inside carries of a full ring still leave room (two cuts could exhaust a reserve of 2; user,
// 2026-11-12: bigger reserve).
inline constexpr unsigned kRingReserve = 4;
inline constexpr unsigned kSpare = kRingReserve + 1;                        // blank sectors maintain() keeps when it may
static_assert(kRecBytes <= ringlog::kMaxPayload);
static_assert(ringlog::per_sector(kRecBytes) == 7, "seven cards to a sector");
static_assert(kRecBytes + deep::kMaxBlob <= ringlog::kMaxPayload, "a card with its worst Deep block is one record");

enum class Err : std::uint8_t { Ok, Invalid, NotFound, Flash, Full };

class Store {
public:
    explicit Store(ringlog::Ring& r) : ring_(r) {}

    // Newest first. Returns the number of cards written to out (at most out.size()).
    std::size_t list(std::span<Card> out) const;
    // The card numbers, newest first (the order of list), without loading the cards.
    std::size_t numbers(std::span<std::uint16_t> out) const;
    bool find(std::uint32_t fp, Card& c) const;
    bool get(std::uint16_t card_no, Card& c) const;
    std::size_t count() const;
    // Saves a finished run: keeps the number, user name and run count of an earlier card of the same
    // charger (see inherit()). c comes back with card_no/runs/name filled in.
    Err put(Card& c);
    // The Deep block stored with card `card_no` (a view of the mapped flash: use it before the next write or maintenance),
    // empty when the card has none.
    std::span<const std::uint8_t> ext(std::uint16_t card_no) const;
    // The same save with the caller's buffer for the card it replaces: prev holds the stored card of the same
    // charger when `had` comes back true (read before the write, never modified by it), so a caller that wants
    // that card for a delta does not read it a second time or keep a second Card on the stack beside this frame.
    // `blk`: the run's Deep block (report_card::deep::build), empty for a run without one; see the header for what
    // becomes of it on a repeat.
    Err put(Card& c, Card& prev, bool& had, std::span<const std::uint8_t> blk = {});
    Err rename(std::uint16_t card_no, std::string_view name);
    Err remove(std::uint16_t card_no);

    // One maintenance step (see the header); `may_erase`: no charger test, no PD Stress run, a quiet link. Returns true
    // when it erased a sector.
    bool maintain(bool may_erase);
    // Blank sectors short of kSpare, or a dirty sector: maintain() has work.
    bool needs_maintenance() const;
    // Forget the index: the ring was rescanned or erased underneath (the factory wipe).
    void invalidate() { valid_ = false; }

private:
    // current: kFormat; deep: the record carries a Deep block
    struct Ent { std::uint32_t fp, stamp, at; std::uint16_t card_no; bool current, deep; };
    struct Order { std::array<std::uint8_t, kMaxCards> at; unsigned n = 0; };

    ringlog::Ring& ring_;
    mutable std::array<Ent, kMaxCards> idx_;
    mutable unsigned n_ K2_BSS_INIT(0);
    mutable bool valid_ K2_BSS_INIT(false);

    int by_fp(std::uint32_t fp) const;

    // What maintain() carries forward out of a sector it erases: a card the index points at (returns its index entry),
    // and, out of any sector but the oldest, the tombstone of a charger with no card now (kGone), since an older record
    // of that charger may sit in an older sector. Anything else is dropped (kDrop).
    static constexpr int kDrop = -2, kGone = -1;
    // A card record: the stamp and the card, then nothing or a whole Deep block.
    static bool is_card(const ringlog::Record& r);
    // Whether card `i` keeps its Deep block when carried forward: only the kMaxDeep newest of those that have one.
    bool block_kept(int i) const;
    // The bytes of record `r` (index entry `i`, see keep()) that maintain() carries forward.
    std::size_t carried(const ringlog::Record& r, int i) const;
    int keep(const ringlog::Record& r, bool tail) const;
    // Whether what sector `s` carries forward fits the head's rest and the blank sectors (the reserve too), records
    // packed in order as append() packs them; `slack`: one blank sector left untouched, the room for the next pass when
    // a power cut closes the sector this one writes.
    bool fits(std::uint32_t s, bool slack, std::uint32_t& need) const;
    // The sector maintain() gives back: the oldest live one when its cards fit with slack, else the one that carries the
    // fewest bytes and fits with slack, else (after a power cut used the slack) the same without slack; 0 when none
    // fits. The head is never one.
    std::uint32_t victim() const;
    unsigned lowest() const;
    static const std::uint8_t* record_of(const std::uint8_t* payload) { return payload + 4; }

    bool load(const Ent& e, Card& c) const;
    // The payload of the card record at `at` (mapped flash) and its length, p == nullptr when it is not one any more.
    struct View { const std::uint8_t* p = nullptr; std::size_t len = 0; };
    View ring_view(std::uint32_t at) const;
    std::span<const std::uint8_t> ext_of(const Ent& e) const;

    // Builds the index from the whole ring, oldest record first: the latest record per fingerprint, tombstones remove.
    void ensure() const;
    // One card record into the index: replaces the fingerprint's entry; while the index is full the lowest stamp of the
    // index and the new card is left out. noinline: the decoded Card lives on this frame only.
    [[gnu::noinline]] void index_card(const ringlog::Record& r) const;
    void drop_ent(std::uint32_t fp) const;
    // Two cards with one card number (an older build handed the number out again while it could not read an older-format
    // card): a current-format card keeps it over an older-format one, then the newer stamp wins; the other card is shown
    // with one above the highest number in use. In RAM only: the card's next save stores it (load() hands the number out).
    void renumber() const;
    // Indexes into idx_, newest first (insertion sort, 64 entries at most).
    Order order() const;

    // `blk` may be a view of the card's own old record (a rename, a repeat): the old record stays on flash until a sector
    // erase, which only maintain() does.
    [[gnu::noinline]] Err write(const Card& c, std::uint32_t stamp, std::span<const std::uint8_t> blk = {});
    Err gone(std::uint32_t fp);
};

}  // namespace k2::report_card
