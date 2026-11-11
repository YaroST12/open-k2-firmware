// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress journal: what a run must remember across a reboot of a K2 that the charger under test powers. A charger that cuts VBUS under
// stress also kills the K2; when the charger comes back the K2 boots, finds this record and carries on at the interrupted sequence
// instead of starting over (the same idea as the charger test's /jr/*.rec, but one small fixed record).
//
// One record, kFrameBytes: a header, the run's options, the reboot budget (per sequence and per run) and the per-sequence counters (the
// transition count and one counter per event kind). The distributions (dist.hpp) are not kept: a sequence restarts with empty histograms
// after a reboot, its counters carry on. The record is written only at a sequence boundary or right after a resume, on a quiet link
// (a flash write holds the main loop for up to a quarter of a second, which a PD-active link must not see; the session checks
// fusb302::pd_quiet_ms()). Wear: one write per sequence plus one per reboot, about a dozen per run, to a file that is rewritten in
// place by the file system's wear levelling.
//
// A loss is counted against the sequence that was running: its ContractLost counter goes up by one (the contract did not survive), the
// sequence is repeated once, and after kMaxSeqLosses losses in the same sequence it is given up. kMaxRunLosses reboots end the run.
//
// Pure code over buffers and a Store seam (the firmware binds it to the file system); host-tested in tests/test_pdstress_journal.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/bbstore.hpp"
#include "k2/pdstress/engine.hpp"

namespace k2::pdstress::journal {

inline constexpr std::uint8_t kMagic = 0xA5;
inline constexpr std::uint8_t kType = 'S';
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 6;     // magic, type, version, 0, payload length (LE16)
inline constexpr std::size_t kPayloadBytes = 46 + kMaxSeqs * kCountWords * 2;
inline constexpr std::size_t kFrameBytes = kHeaderBytes + kPayloadBytes + 2;   // + CRC16 over everything before it
inline constexpr std::size_t kMaxBytes = 384;
static_assert(kFrameBytes <= kMaxBytes, "one journal frame must fit its buffer");

// Everything the journal holds. `counts[seq]` is the Record's count words of that sequence (transitions, then one per Event).
struct Image {
    Options opt{};
    Resume resume{};                  // next (order index), losses, elapsed_ms, per-sequence losses and given-up flags
    std::uint8_t next_seq = 0;        // the Seq id at order index `resume.next` (what a loss is counted against)
    std::uint16_t caps_crc = 0;       // the source list the run started on (caps_crc below)
    bool finished = false;
    Stop stop = Stop::None;
    std::array<std::array<std::uint16_t, kCountWords>, kMaxSeqs> counts{};
};

namespace detail {
inline void put16(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v); p[1] = std::uint8_t(v >> 8); }
inline void put32(std::uint8_t* p, std::uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
inline std::uint16_t get16(const std::uint8_t* p) { return std::uint16_t(p[0] | p[1] << 8); }
inline std::uint32_t get32(const std::uint8_t* p) { return get16(p) | std::uint32_t(get16(p + 2)) << 16; }
}  // namespace detail

// Frame: header, payload, CRC16 (bbstore::crc16, seed 0xFFFF) of header + payload. Returns the frame length, 0 when `out` is too small.
inline std::size_t encode(const Image& im, std::span<std::uint8_t> out) {
    if (out.size() < kFrameBytes) return 0;
    std::uint8_t* f = out.data();
    f[0] = kMagic; f[1] = kType; f[2] = kVersion; f[3] = 0;
    detail::put16(f + 4, kPayloadBytes);
    std::uint8_t* p = f + kHeaderBytes;
    p[0] = im.finished ? 1 : 0;
    p[1] = static_cast<std::uint8_t>(im.stop);
    p[2] = im.resume.next;
    p[3] = im.resume.losses;
    p[4] = im.next_seq;
    p[5] = im.opt.pace;
    detail::put16(p + 6, im.opt.mask);
    detail::put16(p + 8, im.opt.cycles_pct);
    detail::put16(p + 10, im.opt.max_transitions);
    detail::put16(p + 12, im.caps_crc);
    detail::put32(p + 14, im.opt.max_ms);
    detail::put32(p + 18, im.resume.elapsed_ms);
    for (std::size_t i = 0; i < kMaxSeqs; ++i) { p[22 + i] = im.resume.seq_losses[i]; p[34 + i] = im.resume.given_up[i]; }
    std::uint8_t* c = p + 46;
    for (const auto& seq : im.counts)
        for (std::uint16_t w : seq) { detail::put16(c, w); c += 2; }
    const std::uint16_t crc = bbstore::crc16(0xFFFF, std::span<const std::uint8_t>(f, kHeaderBytes + kPayloadBytes));
    detail::put16(f + kHeaderBytes + kPayloadBytes, crc);
    return kFrameBytes;
}

// false for a short, damaged or foreign frame (any single flipped byte, another version, another length).
inline bool decode(std::span<const std::uint8_t> in, Image& im) {
    if (in.size() != kFrameBytes) return false;
    const std::uint8_t* f = in.data();
    if (f[0] != kMagic || f[1] != kType || f[2] != kVersion || detail::get16(f + 4) != kPayloadBytes) return false;
    if (detail::get16(f + kHeaderBytes + kPayloadBytes) != bbstore::crc16(0xFFFF, in.first(kHeaderBytes + kPayloadBytes))) return false;
    const std::uint8_t* p = f + kHeaderBytes;
    im = Image{};
    im.finished = p[0] & 1;
    im.stop = static_cast<Stop>(p[1]);
    im.resume.next = p[2];
    im.resume.losses = p[3];
    im.next_seq = p[4];
    im.opt.pace = p[5];
    im.opt.mask = detail::get16(p + 6);
    im.opt.cycles_pct = detail::get16(p + 8);
    im.opt.max_transitions = detail::get16(p + 10);
    im.caps_crc = detail::get16(p + 12);
    im.opt.max_ms = detail::get32(p + 14);
    im.resume.elapsed_ms = detail::get32(p + 18);
    for (std::size_t i = 0; i < kMaxSeqs; ++i) { im.resume.seq_losses[i] = p[22 + i]; im.resume.given_up[i] = p[34 + i]; }
    const std::uint8_t* c = p + 46;
    for (auto& seq : im.counts)
        for (std::uint16_t& w : seq) { w = detail::get16(c); c += 2; }
    return true;
}

// The counters of a run <-> the image. apply_counts leaves empty (but valid) distributions: min at the top, no samples.
inline void capture_counts(Image& im, const Record& rec) {
    for (std::size_t s = 0; s < kMaxSeqs; ++s) {
        const Counts c = counts_of(rec.rec(s));
        im.counts[s][0] = c.transitions;
        for (std::size_t e = 0; e < kEventCount; ++e) im.counts[s][1 + e] = c.n[e];
    }
}
inline void apply_counts(const Image& im, Record& rec) {
    rec.clear();
    for (std::size_t s = 0; s < kMaxSeqs; ++s) {
        Rec r = rec.rec(s);
        put(r, 0, im.counts[s][0]);
        for (std::size_t e = 0; e < kEventCount; ++e) put(r, 1 + e, im.counts[s][1 + e]);
    }
}

// A fingerprint of the charger's list: a journal only resumes on the source it started on. The Source_Capabilities count is left out, so
// a re-announce of the same list is the same charger.
inline std::uint16_t caps_crc(const pd::SourceCaps& c) {
    std::array<std::uint8_t, 1 + 4 * pd::kMaxPdos> b{};
    std::size_t n = 0;
    b[n++] = c.num_pdos;
    for (std::size_t i = 0; i < c.num_pdos && i < c.pdos.size(); ++i) {
        detail::put32(b.data() + n, c.pdos[i].raw);
        n += 4;
    }
    return bbstore::crc16(0xFFFF, std::span<const std::uint8_t>(b.data(), n));
}

enum class Verdict : std::uint8_t {
    Resume,    // carry on: the image now holds the new loss and Resume
    RunLost,   // the run's reboot budget is spent: start the engine with this image and it reports TooManyLosses
    Stale      // not ours to resume: finished, another charger, a foreign sequence id, or BACK held at power-up; the image is untouched
};

// A boot found a journal of a run that never finished: account for the loss and decide.
inline Verdict derive(Image& im, std::uint16_t caps_now, bool cancel) {
    if (cancel || im.finished || im.caps_crc != caps_now || im.next_seq >= kSeqCount) return Verdict::Stale;
    const std::size_t q = im.next_seq;
    if (im.resume.losses != 0xFF) ++im.resume.losses;
    if (im.resume.seq_losses[q] != 0xFF) ++im.resume.seq_losses[q];
    std::uint16_t& lost = im.counts[q][1 + static_cast<std::size_t>(Event::ContractLost)];
    if (lost != 0xFFFF) ++lost;
    if (im.resume.seq_losses[q] >= kMaxSeqLosses) im.resume.given_up[q] = 1;
    return im.resume.losses >= kMaxRunLosses ? Verdict::RunLost : Verdict::Resume;
}

// Where the record lives. The firmware binds it to the file system; tests use a fake that can fail.
class Store {
public:
    virtual ~Store() = default;
    virtual bool read(std::span<std::uint8_t> out, std::size_t& n) = 0;   // false = no record
    virtual bool write(std::span<const std::uint8_t> in) = 0;             // atomic replace; false = failed
    virtual void remove() = 0;
};

class Journal {
public:
    explicit Journal(Store& s) : st_(&s) {}
    bool save(const Image& im) {
        std::array<std::uint8_t, kMaxBytes> b{};
        const std::size_t n = encode(im, b);
        return n && st_->write(std::span<const std::uint8_t>(b.data(), n));
    }
    bool load(Image& im) {
        std::array<std::uint8_t, kMaxBytes> b{};
        std::size_t n = 0;
        if (!st_->read(b, n)) return false;
        return decode(std::span<const std::uint8_t>(b.data(), n), im);
    }
    void clear() { st_->remove(); }

private:
    Store* st_;
};

}  // namespace k2::pdstress::journal
