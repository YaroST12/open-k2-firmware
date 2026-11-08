// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Transition capture store: the failed-step captures of the charger test, kept on the littlefs volume
// (k2/core/fs.hpp) in /tcap, at most kMaxStored of them, oldest dropped. Pure logic over k2::fs::Fs, host-tested by
// tests/test_transcap_store.cpp on a RAM disk that can lose power at any operation.
//
// Two files per capture, <seq as 8 hex digits>:
//   .tcd  the delta stream of k2/pd/transcap_calc.hpp (a few KB), written through a small buffer to ".tcd.tmp" and
//         renamed, so a power cut never leaves a half file under the final name
//   .tci  kMetaBytes (inline file, ~212 bytes of flash): which run and step it belongs to, why it froze, and the
//         96-column VBUS thumbnail the UI draws. It is written last: a capture without its .tci does not exist
//         (the orphaned .tcd is removed by the next save).
// seq counts up for ever (highest = newest). The run is identified by the charger-test journal's run id plus
// the step index, which is how the step's detail screen finds its capture.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "k2/core/fs.hpp"
#include "k2/core/scratch.hpp"
#include "k2/pd/transcap_calc.hpp"

namespace k2::transcap {

inline constexpr unsigned kMaxStored = 4;
inline constexpr const char* kDir = "/tcap";
inline constexpr std::size_t kMetaBytes = 20 + 2 * kThumbCols;
inline constexpr std::size_t kWriteChunk = 512;
inline constexpr std::array<std::uint8_t, 4> kMetaMagic{'K', '2', 'T', 'I'};

struct Meta {
    std::uint32_t seq = 0, run_id = 0;
    std::uint16_t step = 0;
    Reason reason = Reason::None;
    Thumb thumb;
};

inline void pack_meta(const Meta& m, std::span<std::uint8_t, kMetaBytes> out) {
    using detail::put32;
    for (std::size_t i = 0; i < 4; i++) out[i] = kMetaMagic[i];
    put32(&out[4], m.seq);
    put32(&out[8], m.run_id);
    out[12] = std::uint8_t(m.step); out[13] = std::uint8_t(m.step >> 8);
    out[14] = std::uint8_t(m.reason);
    out[15] = m.thumb.accept_col; out[16] = m.thumb.trig_col; out[17] = 0;
    out[18] = std::uint8_t(m.thumb.vmax_mv); out[19] = std::uint8_t(m.thumb.vmax_mv >> 8);
    for (std::size_t c = 0; c < kThumbCols; c++) { out[20 + c] = m.thumb.lo[c]; out[20 + kThumbCols + c] = m.thumb.hi[c]; }
}
inline bool unpack_meta(std::span<const std::uint8_t> in, Meta& m) {
    if (in.size() != kMetaBytes) return false;
    for (std::size_t i = 0; i < 4; i++) if (in[i] != kMetaMagic[i]) return false;
    m.seq = detail::get32(&in[4]);
    m.run_id = detail::get32(&in[8]);
    m.step = std::uint16_t(in[12] | in[13] << 8);
    m.reason = Reason(in[14]);
    m.thumb.accept_col = in[15]; m.thumb.trig_col = in[16];
    m.thumb.vmax_mv = std::uint16_t(in[18] | in[19] << 8);
    for (std::size_t c = 0; c < kThumbCols; c++) { m.thumb.lo[c] = in[20 + c]; m.thumb.hi[c] = in[20 + kThumbCols + c]; }
    return true;
}

class Store {
public:
    explicit Store(fs::Fs& f) : fs_(f) {}

    // Writes one frozen capture. Drops the oldest beyond kMaxStored. `seq` is the new capture's number.
    template <class Rec>
    fs::Err save(const Rec& r, std::uint32_t run_id, std::uint16_t step, std::uint32_t& seq) {
        fs::Err e = fs_.mkdir(kDir);
        if (e != fs::Err::Ok) return e;
        fs_.clean_tmp(kDir);
        seq = next_seq();
        char tmp[fs::kPathMax], dat[fs::kPathMax], inf[fs::kPathMax];
        path(dat, seq, ".tcd");
        path(inf, seq, ".tci");
        path(tmp, seq, ".tcd.tmp");
        fs_.remove(tmp);
        // The write buffer, the info record and its thumbnail are ~1 KB: leased from the scratch arena, not stacked.
        struct Work {
            std::array<std::uint8_t, kWriteChunk> buf;
            std::array<std::uint8_t, kMetaBytes> mb;
            Meta m;
        };
        const scratch::Lease lease("transcap.save");
        Work& w = scratch::make<Work>();
        Encoder<Rec> enc(r);
        while (!enc.done()) {
            const std::size_t n = enc.read(w.buf);
            if (n == 0) break;
            e = fs_.append_file(tmp, {w.buf.data(), n});
            if (e != fs::Err::Ok) { fs_.remove(tmp); return e; }
        }
        e = fs_.rename(tmp, dat);
        if (e != fs::Err::Ok) { fs_.remove(tmp); return e; }
        w.m.seq = seq; w.m.run_id = run_id; w.m.step = step; w.m.reason = r.reason();
        fill_thumb(r, w.m.thumb);
        pack_meta(w.m, w.mb);
        e = fs_.write_file(inf, w.mb);
        if (e != fs::Err::Ok) { fs_.remove(dat); return e; }
        prune();
        return fs::Err::Ok;
    }

    // Stored captures, newest first.
    std::size_t list(std::span<Meta> out) {
        std::array<std::uint32_t, kScan> seqs;
        const std::size_t n = scan(seqs, ".tci");
        std::size_t k = 0;
        for (std::size_t i = 0; i < n && k < out.size(); i++)
            if (load(seqs[i], out[k])) ++k;
        return k;
    }
    // The newest capture of this run and step.
    bool find(std::uint32_t run_id, std::uint16_t step, Meta& m) {
        // One Meta at a time, over the same first kMaxStored + 1 readable entries list() would return (a whole
        // array of them was a 1 KB frame).
        std::array<std::uint32_t, kScan> seqs;
        const std::size_t n = scan(seqs, ".tci");
        Meta t;
        std::size_t seen = 0;
        for (std::size_t i = 0; i < n && seen < kMaxStored + 1; i++) {
            if (!load(seqs[i], t)) continue;
            ++seen;
            if (t.run_id == run_id && t.step == step) { m = t; return true; }
        }
        return false;
    }
    bool get(std::uint32_t seq, Meta& m) { return load(seq, m); }
    // A piece of the data stream: n bytes at off (0 at the end).
    fs::Err read(std::uint32_t seq, std::uint32_t off, std::span<std::uint8_t> out, std::size_t& n) {
        char p[fs::kPathMax];
        path(p, seq, ".tcd");
        return fs_.read_at(p, off, out, n);
    }
    fs::Err size(std::uint32_t seq, std::size_t& n) {
        char p[fs::kPathMax];
        path(p, seq, ".tcd");
        return fs_.size_of(p, n);
    }
    // Deletes everything.
    void clear() {
        std::array<std::uint32_t, kScan> seqs;
        for (const char* ext : {".tci", ".tcd"}) {
            const std::size_t n = scan(seqs, ext);
            for (std::size_t i = 0; i < n; i++) { char p[fs::kPathMax]; path(p, seqs[i], ext); fs_.remove(p); }
        }
    }
    // Keeps the newest kMaxStored; removes orphaned data files (no .tci).
    void prune() {
        std::array<std::uint32_t, kScan> info, data;
        const std::size_t ni = scan(info, ".tci"), nd = scan(data, ".tcd");
        char p[fs::kPathMax];
        for (std::size_t i = kMaxStored; i < ni; i++) { path(p, info[i], ".tci"); fs_.remove(p); }
        for (std::size_t i = 0; i < nd; i++) {
            bool keep = false;
            for (std::size_t k = 0; k < ni && k < kMaxStored; k++) keep |= info[k] == data[i];
            if (!keep) { path(p, data[i], ".tcd"); fs_.remove(p); }
        }
    }

private:
    static constexpr std::size_t kScan = 16;   // directory entries looked at (a clean volume holds kMaxStored)
    fs::Fs& fs_;

    static void path(char (&out)[fs::kPathMax], std::uint32_t seq, const char* ext) {
        std::size_t o = 0;
        for (const char* d = kDir; *d; ) out[o++] = *d++;
        out[o++] = '/';
        for (int i = 7; i >= 0; i--) out[o++] = "0123456789ABCDEF"[(seq >> (4 * i)) & 15];
        for (const char* d = ext; *d; ) out[o++] = *d++;
        out[o] = 0;
    }
    static bool parse(const char* name, const char* ext, std::uint32_t& seq) {
        std::uint32_t v = 0;
        for (int i = 0; i < 8; i++) {
            const char c = name[i];
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            v = v << 4 | std::uint32_t(d);
        }
        if (std::strcmp(name + 8, ext) != 0) return false;
        seq = v;
        return true;
    }
    // Sequence numbers of the files with this extension, highest first.
    std::size_t scan(std::span<std::uint32_t> out, const char* ext) {
        std::size_t n = 0;
        fs_.list(kDir, [&](const fs::Info& i) {
            std::uint32_t s;
            if (!i.dir && parse(i.name, ext, s) && n < out.size()) out[n++] = s;
            return true;
        });
        for (std::size_t i = 1; i < n; i++)   // insertion sort, descending
            for (std::size_t j = i; j > 0 && out[j] > out[j - 1]; j--) { const auto t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
        return n;
    }
    std::uint32_t next_seq() {
        std::array<std::uint32_t, kScan> a;
        std::uint32_t top = 0;
        for (const char* ext : {".tci", ".tcd"}) {
            const std::size_t n = scan(a, ext);
            if (n && a[0] > top) top = a[0];
        }
        return top + 1;
    }
    bool load(std::uint32_t seq, Meta& m) {
        char p[fs::kPathMax];
        path(p, seq, ".tci");
        std::array<std::uint8_t, kMetaBytes> b;
        std::size_t n = 0;
        if (fs_.read_file(p, b, n) != fs::Err::Ok) return false;
        return unpack_meta({b.data(), n}, m) && m.seq == seq;
    }
};

}  // namespace k2::transcap
