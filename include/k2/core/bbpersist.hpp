// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box persistence: finished captures are compressed into the /bb files on littlefs (k2/core/bbstore.hpp) and
// a boot reloads the newest ones into the RAM capture slots, so the UI shows them again. The class
// takes the volume for host tests; src/core/bbpersist.cpp binds the real one.
//
// Writing rules, all about not stalling PD traffic and not wearing the flash:
//   - only finished captures (the post-trigger window is over);
//   - only once the link has been quiet for kQuietMs (a program or erase blocks the main loop for
//     up to a few tens of ms);
//   - not while idle (no PD traffic or VBUS change for bbstore::kIdleMs): such a capture is dropped;
//   - each capture once: a slot is remembered by its sequence number;
//   - hold_run(): while a charger test runs, the captures that every PD row makes (Contract, and the VBUS dip and edge
//     captures that its 21 V to 5 V steps raise) wait for the end of the run (one save is about 600 flash half-words
//     with interrupts off, 95-290 ms of main loop gap on the device). The rare events (hard reset, detach, refusal) go
//     at once, but only kRareBudget of them per run: a charger that resets on every row would otherwise cost one save
//     (a sector erase and ~600 programs) per row, so the rest wait too. A waiting capture is kept in its RAM slot
//     (pdtrace::keep_capture): a newer capture takes another slot, and only when all three are waiting is the oldest
//     lost, which `dropped` counts. When the hold lapses the waiting captures go out one save per poll(), oldest first.
//     The hold lapses by itself kHoldLapseMs after the last call, so a session that stops renewing it cannot keep it up.
//   - make_room(): the ring gives the journal's blocks back (see bbstore.hpp) at boot and when a run starts.
#pragma once
#include <array>
#include <cstdint>

#include "k2/core/bbstore.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/core/scratch.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::bbpersist {

inline constexpr std::uint32_t kQuietMs = 300;
inline constexpr std::uint32_t kHoldLapseMs = 2000;
inline constexpr std::uint32_t kRareBudget = 2;   // rare-kind captures written during one held run; the rest wait for its end

struct Status {
    bool ready = false;                // the ring was mounted
    bool idle = false;                 // idle gate closed: captures are not written
    bool held = false;                 // a charger test runs: the per-row captures wait
    std::uint32_t waiting = 0;         // finished captures the hold kept back at the last scan
    std::uint32_t dropped = 0;         // waiting captures a newer one overwrote (every RAM slot was waiting)
    std::uint32_t deferred = 0;        // rare-kind captures the run budget made wait (they are written after the run)
    std::array<std::uint32_t, 8> by_kind{};  // captures written, by pdtrace::Trigger
    unsigned last_kind = 0;            // trigger of the last save
    bool last_while_held = false;      // the hold was up when it was written (a held kind written anyway = a bug)
    std::uint32_t made_room = 0;       // captures evicted by make_room()
    int entries = 0;                   // captures in flash
    std::uint32_t used_bytes = 0, free_bytes = 0;
    std::uint32_t skipped = 0;         // captures dropped while idle, too big or without room above the reserve
    std::uint32_t errors = 0;          // flash errors
    std::uint32_t saves = 0;           // captures written
    std::uint32_t last_ms = 0;         // duration of the last save (the main loop was blocked this long)
    std::uint32_t max_ms = 0;          // the longest save since boot
};

class Persister {
public:
    explicit Persister(fs::Fs& f) : ring_(f) { ring_.set_reserve(bbstore::kJournalReserveBlocks); }

    // A charger test runs: the per-row captures wait (see the header). Renew it every tick of the test.
    void hold_run() {
        if (!holding()) rare_run_ = 0;   // a new run: the budget of rare saves starts over
        hold_ms_ = systick::millis();
        hold_set_ = true;
    }

    // The journal's blocks free again (the ring evicts its oldest captures); returns how many went.
    unsigned make_room() {
        const unsigned n = ring_.make_room();
        st_.made_room += n;
        refresh();
        return n;
    }

    // Mounts the ring and reloads the newest stored captures, oldest first.
    void init() {
        ring_.mount();
        make_room();
        // Every stored capture is visited (the entry is valid during the call); the slots keep the newest ones.
        {
            const scratch::Lease lease("bbpersist.init");
            const std::span<std::uint8_t> buf = buffer();
            ring_.each([&](const bbstore::Entry& e) {
                const std::size_t m = ring_.unpack(e, buf);
                if (m == lz::kError || m != e.raw_len) return;
                if (pdtrace::restore_capture(static_cast<pdtrace::Trigger>(e.trigger), {buf.data(), m}, e.pre))
                    mark_saved_newest();
            });
        }
        refresh();
        st_.ready = true;
    }

    // Main loop: saves finished, unsaved captures, oldest first, one flash write per call (the main loop gets a pass
    // between two saves).
    void poll() {
        if (!st_.ready) return;
        const bool hold = holding();
        for (;;) {
            int pick = -1;
            std::uint32_t seq = ~0u, waiting = 0;
            for (std::size_t i = 0; i < pdtrace::kCapSlots; i++) {
                const auto in = pdtrace::capture_info(i);
                if (in.trigger == pdtrace::Trigger::None) { pending_[i] = 0; continue; }
                if (pending_[i] != 0 && pending_[i] != in.seq) { st_.dropped++; pending_[i] = 0; }   // a newer capture took the slot
                if (in.filling || in.stored || in.seq == saved_[i]) continue;
                if (hold && waits(in.trigger)) {
                    waiting++;
                    if (pending_[i] != in.seq) {
                        pending_[i] = in.seq;
                        pdtrace::keep_capture(i, true);
                        if (!held_kind(in.trigger)) st_.deferred++;
                    }
                    continue;
                }
                if (in.seq < seq) { seq = in.seq; pick = int(i); }
            }
            st_.waiting = waiting;
            if (pick < 0) return;
            const std::size_t i = std::size_t(pick);
            if (pdtrace::idle()) { settled(i, seq); st_.skipped++; continue; }
            if (pdtrace::since_activity_ms() < kQuietMs) return;
            const auto in = pdtrace::capture_info(i);
            const auto data = pdtrace::capture_data(i);
            const std::uint32_t at_ms = systick::millis() - in.age_ms;
            bbstore::Err e;
            const std::uint32_t t0 = systick::millis();
            {
                const scratch::Lease lease("bbpersist.poll");
                e = ring_.append({static_cast<unsigned>(in.trigger), in.pre, at_ms / 1000}, data, buffer());
            }
            st_.last_ms = systick::millis() - t0;
            if (st_.last_ms > st_.max_ms) st_.max_ms = st_.last_ms;
            settled(i, seq);
            if (hold && !held_kind(in.trigger)) rare_run_++;   // a rare kind went out mid-run: it counts against the budget
            if (e == bbstore::Err::TooBig || e == bbstore::Err::Full) st_.skipped++;
            else if (e != bbstore::Err::Ok) st_.errors++;
            else {
                st_.saves++;
                const unsigned k = static_cast<unsigned>(in.trigger);
                if (k < st_.by_kind.size()) st_.by_kind[k]++;
                st_.last_kind = k;
                st_.last_while_held = hold;
            }
            refresh();
            return;
        }
    }

    Status status() const {
        Status s = st_;
        s.idle = pdtrace::idle();
        s.held = holding();
        return s;
    }

private:
    bbstore::Ring ring_;
    Status st_;
    std::uint32_t hold_ms_ = 0;
    bool hold_set_ = false;
    std::uint32_t rare_run_ = 0;   // rare-kind captures written since the hold started
    bool holding() const { return hold_set_ && systick::millis() - hold_ms_ < kHoldLapseMs; }
    // What a charger-test row makes by itself: the Contract capture, and the dip and edge captures of its voltage steps.
    static bool held_kind(pdtrace::Trigger t) {
        return t == pdtrace::Trigger::Contract || t == pdtrace::Trigger::VbusDip || t == pdtrace::Trigger::Edges;
    }
    // Waits for the end of the run: the per-row kinds always, the rare kinds once the run has used its budget.
    bool waits(pdtrace::Trigger t) const { return held_kind(t) || rare_run_ >= kRareBudget; }
    std::array<std::uint32_t, pdtrace::kCapSlots> saved_{};
    std::array<std::uint32_t, pdtrace::kCapSlots> pending_{};   // sequence of the capture each slot holds back (0 = none)
    // The capture of slot `i` was written or given up: it is not waiting any more and its slot may be reused.
    void settled(std::size_t i, std::uint32_t seq) {
        saved_[i] = seq;
        pending_[i] = 0;
        pdtrace::keep_capture(i, false);
    }
    // The LZ working buffer of one capture, from the shared scratch arena (k2/core/scratch.hpp); the caller holds the lease.
    static constexpr std::size_t kBufBytes = lz::bound(pdtrace::kCapBytes);
    static_assert(kBufBytes <= scratch::kSize, "scratch arena too small for the Black Box LZ buffer");
    static std::span<std::uint8_t> buffer() { return {scratch::arena(), kBufBytes}; }

    void refresh() {
        const auto s = ring_.stats();
        st_.entries = s.entries;
        st_.used_bytes = s.used_bytes;
        st_.free_bytes = s.free_bytes;
    }
    // The slot restore_capture() just filled is the one with the highest sequence number.
    void mark_saved_newest() {
        std::size_t best = 0;
        std::uint32_t seq = 0;
        for (std::size_t i = 0; i < pdtrace::kCapSlots; i++) {
            const auto in = pdtrace::capture_info(i);
            if (in.seq > seq) { seq = in.seq; best = i; }
        }
        saved_[best] = seq;
    }
};

// The firmware instance over the real flash (src/core/bbpersist.cpp).
void init();
void poll();
void hold_run();
void make_room();   // logs what it evicted
Status status();

}  // namespace k2::bbpersist
