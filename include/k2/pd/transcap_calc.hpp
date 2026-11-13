// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Transition capture, pure part (host-tested by tests/test_transcap.cpp): the sample/marker ring with its
// arm / trigger / post-window state machine, the byte-stream encoder and decoder used for persistence, and the
// thumbnail (min/max columns) the UI draws as a sparkline. No hardware here; src/pd/transcap.cpp feeds it.
//
// What a capture is: VBUS (mV) plus CC1 and CC2 (8-bit ADC, 12.9 mV per count) sampled about every 1 ms, with
// PD events (Request sent, Accept, PS_RDY, Hard Reset, attach, detach ...) as markers on the same microsecond
// timeline. The ring keeps the newest kSamples; a trigger starts a post window of kPostUs after which the
// capture freezes, so a frozen capture is [about 1.5 s before the trigger] + [kPostUs after it].
//
// Wire form (persistence and the remote dump share the sample/marker records, little endian):
//   Sample  8 bytes  t_us u32 | vbus_mv u16 | cc1 u8 | cc2 u8
//   Marker  8 bytes  t_us u32 | code u8 | step u8 | arg u16
// The persisted data file is the delta stream of encode() below.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/math_opt.hpp"

namespace k2::transcap {

inline constexpr std::size_t kSamples = 1920;      // ring: 1.92 s at 1 kHz
inline constexpr std::size_t kMarkers = 64;
inline constexpr std::uint32_t kSampleGapUs = 800;     // minimum spacing the driver keeps between samples
inline constexpr std::uint32_t kPostUs = 400'000;      // recording continues this long after a trigger
inline constexpr std::uint32_t kArmTimeoutMs = 30'000; // an arm nobody triggers lapses (INA226 back to normal)

// Spaces the samples: take() is true when at least kSampleGapUs passed since the last true, and
// stamps the time it was asked (the START of the read). Stamping the end of the read instead would
// leave a poll that comes 1 ms after the last one under the gap, and every other pass would skip.
class Pacer {
public:
    void reset(std::uint32_t now_us) { last_ = now_us - kSampleGapUs; }
    bool take(std::uint32_t now_us) {
        if (now_us - last_ < kSampleGapUs) return false;
        last_ = now_us;
        return true;
    }

private:
    std::uint32_t last_ = 0;
};

struct Sample {
    std::uint32_t t_us = 0;
    std::uint16_t vbus_mv = 0;
    std::uint8_t cc1 = 0, cc2 = 0;      // ADC raw 12 bit >> 4
};
struct Marker {
    std::uint32_t t_us = 0;
    std::uint8_t code = 0, step = 0;
    std::uint16_t arg = 0;
};
static_assert(sizeof(Sample) == 8 && sizeof(Marker) == 8);

enum class Mark : std::uint8_t {
    None, Arm, TxRequest, Accept, PsRdy, Reject, Wait, SoftReset, HardReset, Attach, Detach, Trigger,
    Rx, Tx, Cc,
};
enum class Reason : std::uint8_t { None, Manual, ShutOff, NoPsRdy, HardReset, Detach, Timeout, Voltage, Other };
enum class State : std::uint8_t { Off, Armed, Post, Frozen };

constexpr const char* reason_name(Reason r) {
    switch (r) {
    case Reason::Manual: return "manual";
    case Reason::ShutOff: return "shutoff";
    case Reason::NoPsRdy: return "no-psrdy";
    case Reason::HardReset: return "hardreset";
    case Reason::Detach: return "detach";
    case Reason::Timeout: return "timeout";
    case Reason::Voltage: return "voltage";
    case Reason::Other: return "other";
    default: return "none";
    }
}
constexpr const char* mark_name(Mark m) {
    switch (m) {
    case Mark::Arm: return "arm";
    case Mark::TxRequest: return "request";
    case Mark::Accept: return "accept";
    case Mark::PsRdy: return "ps_rdy";
    case Mark::Reject: return "reject";
    case Mark::Wait: return "wait";
    case Mark::SoftReset: return "soft_reset";
    case Mark::HardReset: return "hard_reset";
    case Mark::Attach: return "attach";
    case Mark::Detach: return "detach";
    case Mark::Trigger: return "trigger";
    case Mark::Rx: return "rx";
    case Mark::Tx: return "tx";
    case Mark::Cc: return "cc";
    default: return "none";
    }
}

// Auto-trigger rule: a Hard Reset (either direction) or a detach while armed freezes the capture on its own.
constexpr Reason auto_reason(Mark m) {
    switch (m) {
    case Mark::HardReset: return Reason::HardReset;
    case Mark::Detach: return Reason::Detach;
    default: return Reason::None;
    }
}

template <std::size_t NS = kSamples, std::size_t NM = kMarkers>
class Recorder {
public:
    State state() const { return state_; }
    Reason reason() const { return reason_; }
    bool triggered() const { return state_ == State::Post || state_ == State::Frozen; }
    std::uint32_t trigger_us() const { return trig_us_; }
    std::size_t sample_count() const { return ns_ < NS ? ns_ : NS; }
    std::size_t marker_count() const { return nm_ < NM ? nm_ : NM; }
    std::uint32_t lost_samples() const { return ns_ > NS ? std::uint32_t(ns_ - NS) : 0; }

    // Starts recording from empty. A frozen capture is dropped.
    void arm() {
        ns_ = nm_ = 0;
        reason_ = Reason::None;
        trig_us_ = 0;
        state_ = State::Armed;
    }
    void stop() { state_ = State::Off; ns_ = nm_ = 0; reason_ = Reason::None; }
    // Back to the all-zero state, storage included (the overlay's next tenant expects zeroed RAM). In place:
    // the object is too big to build as a temporary on the stack.
    void clear() {
        smp_.fill({});
        mrk_.fill({});
        ns_ = nm_ = 0;
        trig_us_ = 0;
        state_ = State::Off;
        reason_ = Reason::None;
    }

    // Appends one sample while Armed or Post. A Post window that has run its time freezes here. Returns true
    // when the capture has just frozen.
    bool push(const Sample& s) {
        if (state_ != State::Armed && state_ != State::Post) return false;
        smp_[ns_ % NS] = s;
        ++ns_;
        if (state_ == State::Post && std::int32_t(s.t_us - trig_us_) >= std::int32_t(kPostUs)) {
            state_ = State::Frozen;
            return true;
        }
        return false;
    }
    // Event marker; kept in Armed and Post. HardReset / Detach trigger on their own while Armed.
    void mark(std::uint32_t t_us, Mark code, std::uint8_t step = 0, std::uint16_t arg = 0) {
        if (state_ != State::Armed && state_ != State::Post) return;
        mrk_[nm_ % NM] = {t_us, std::uint8_t(code), step, arg};
        ++nm_;
        if (state_ == State::Armed) {
            const Reason r = auto_reason(code);
            if (r != Reason::None) trigger(t_us, r);
        }
    }
    // Starts the post window (only from Armed); the reason sticks.
    bool trigger(std::uint32_t t_us, Reason r) {
        if (state_ != State::Armed) return false;
        trig_us_ = t_us;
        reason_ = r;
        state_ = State::Post;
        mrk_[nm_ % NM] = {t_us, std::uint8_t(Mark::Trigger), 0, std::uint16_t(r)};
        ++nm_;
        return true;
    }
    // Chronological access, 0 = oldest kept.
    Sample sample(std::size_t i) const { return smp_[(ns_ - sample_count() + i) % NS]; }
    Marker marker(std::size_t i) const { return mrk_[(nm_ - marker_count() + i) % NM]; }

private:
    std::array<Sample, NS> smp_{};
    std::array<Marker, NM> mrk_{};
    std::size_t ns_ = 0, nm_ = 0;
    std::uint32_t trig_us_ = 0;
    State state_ = State::Off;
    Reason reason_ = Reason::None;
};

// ---- PD message and event classification (pure; the driver feeds it what the PD trace hooks see) ----------
struct PdMark {
    Mark code = Mark::None;
    std::uint8_t step = 0;      // TxRequest: the requested object position
    std::uint16_t arg = 0;      // Rx / Tx: the message header
};

// header: type 4:0, number of data objects 14:12, extended 15. `rx` is true for a message from the source.
// GoodCRC is noise; the interesting ones get their own marker, the rest keep the header for the plot's label.
constexpr PdMark classify_pd(bool rx, std::uint16_t header, std::uint32_t obj0) {
    const unsigned type = header & 0x1F, nobj = (header >> 12) & 7;
    const bool ext = (header & 0x8000) != 0, control = !ext && nobj == 0;
    if (control && type == 1) return {};
    if (control && type == 13) return {Mark::SoftReset, 0, header};
    if (rx && control) {
        switch (type) {
        case 3: return {Mark::Accept, 0, header};
        case 4: return {Mark::Reject, 0, header};
        case 6: return {Mark::PsRdy, 0, header};
        case 12: return {Mark::Wait, 0, header};
        default: break;
        }
    }
    if (!rx && !ext && nobj > 0 && type == 2) return {Mark::TxRequest, std::uint8_t((obj0 >> 28) & 0xF), header};
    return {rx ? Mark::Rx : Mark::Tx, 0, header};
}
constexpr Mark mark_of_reset(bool hard) { return hard ? Mark::HardReset : Mark::SoftReset; }
constexpr Mark mark_of_cc(bool attach) { return attach ? Mark::Attach : Mark::Detach; }

// ---- thumbnail: VBUS min / max per column, scaled to 0..255 of vmax_mv -----------------------------------
inline constexpr std::size_t kThumbCols = 96;
struct Thumb {
    std::array<std::uint8_t, kThumbCols> lo{}, hi{};
    std::uint16_t vmax_mv = 0;
    std::uint8_t trig_col = 0xFF, accept_col = 0xFF;   // column of the trigger / the first Accept marker (0xFF = none)
};

constexpr std::uint8_t scale_u8(std::uint32_t mv, std::uint32_t vmax) {
    if (vmax == 0) return 0;
    const std::uint32_t v = (mv * 255 + vmax / 2) / vmax;
    return std::uint8_t(v > 255 ? 255 : v);
}

// Column of time t on the capture's span [t0, t1].
constexpr std::uint8_t column_of(std::uint32_t t, std::uint32_t t0, std::uint32_t t1) {
    if (std::int32_t(t - t0) <= 0) return 0;
    const std::uint32_t span = t1 - t0;
    if (span == 0 || std::int32_t(t - t1) >= 0) return std::uint8_t(kThumbCols - 1);
    return std::uint8_t(math::mul_div_u32(t - t0, kThumbCols, span + 1));
}

// Min/max decimation by time (not by index, so a gap in sampling shows as an empty column that is filled from
// its neighbour). Peaks survive: every sample lands in exactly one column and the column keeps both extremes.
// In place (the caller keeps `th`, e.g. in the scratch arena): scaling is monotonic, so the scaled min / max of a
// column equal the min / max of the scaled samples and no 16-bit working arrays are needed.
template <class Rec>
void fill_thumb(const Rec& r, Thumb& th) {
    th.lo.fill(0);
    th.hi.fill(0);
    th.vmax_mv = 0;
    th.trig_col = th.accept_col = 0xFF;
    const std::size_t n = r.sample_count();
    if (n == 0) return;
    const std::uint32_t t0 = r.sample(0).t_us, t1 = r.sample(n - 1).t_us;
    std::uint16_t vmax = 1;
    for (std::size_t i = 0; i < n; i++) if (r.sample(i).vbus_mv > vmax) vmax = r.sample(i).vbus_mv;
    th.vmax_mv = vmax;
    std::uint32_t seen[(kThumbCols + 31) / 32] = {};
    for (std::size_t i = 0; i < n; i++) {
        const Sample s = r.sample(i);
        const std::size_t c = column_of(s.t_us, t0, t1);
        const std::uint8_t v = scale_u8(s.vbus_mv, vmax);
        if (!(seen[c / 32] >> (c % 32) & 1)) { seen[c / 32] |= 1u << (c % 32); th.lo[c] = th.hi[c] = v; continue; }
        if (v < th.lo[c]) th.lo[c] = v;
        if (v > th.hi[c]) th.hi[c] = v;
    }
    std::uint8_t prev = scale_u8(r.sample(0).vbus_mv, vmax);
    for (std::size_t c = 0; c < kThumbCols; c++) {
        if (!(seen[c / 32] >> (c % 32) & 1)) th.lo[c] = th.hi[c] = prev;
        prev = th.hi[c];
    }
    for (std::size_t i = 0; i < r.marker_count(); i++) {
        const Marker m = r.marker(i);
        if (m.code == std::uint8_t(Mark::Accept) && th.accept_col == 0xFF && std::int32_t(m.t_us - t0) >= 0)
            th.accept_col = column_of(m.t_us, t0, t1);
        if (m.code == std::uint8_t(Mark::Trigger)) th.trig_col = column_of(m.t_us, t0, t1);
    }
}

template <class Rec>
Thumb make_thumb(const Rec& r) {
    Thumb th;
    fill_thumb(r, th);
    return th;
}

// ---- delta stream ----------------------------------------------------------------------------------------
// Stream = Header(16) + markers (8 each, raw) + samples. Header: "K2TD", n_samples u16, n_markers u8, reason u8,
// trigger_us u32 (absolute, same clock as the records). Samples: the first one raw (8 bytes), then per sample
// a flag byte: bit0 dt changed, bit1 vbus changed, bit2 cc1 changed, bit3 cc2 changed; the changed fields follow
// as zigzag varints of their delta (dt against the previous dt). Flag 0 = identical step, followed by a varint
// count of further identical steps.
inline constexpr std::size_t kHeaderBytes = 12;
inline constexpr std::array<std::uint8_t, 4> kStreamMagic{'K', '2', 'T', 'D'};

namespace detail {
constexpr std::uint32_t zz(std::int32_t v) { return (std::uint32_t(v) << 1) ^ std::uint32_t(v >> 31); }
constexpr std::int32_t unzz(std::uint32_t v) { return std::int32_t(v >> 1) ^ -std::int32_t(v & 1); }
inline void put32(std::uint8_t* p, std::uint32_t v) { for (int i = 0; i < 4; i++) p[i] = std::uint8_t(v >> (8 * i)); }
inline std::uint32_t get32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
}  // namespace detail

// Chunked encoder: read() fills the caller's buffer with the next bytes of the stream, so a capture of any
// size is written to flash through one small buffer. The stream is identical however it is chunked.
template <class Rec>
class Encoder {
public:
    explicit Encoder(const Rec& r) : r_(r), n_(r.sample_count()), m_(r.marker_count()) {}
    bool done() const { return stage_ == Stage::Done && pos_ == len_; }
    // Returns the bytes written to `out` (0 once done).
    std::size_t read(std::span<std::uint8_t> out) {
        std::size_t o = 0;
        while (o < out.size()) {
            if (pos_ == len_) {
                if (stage_ == Stage::Done) break;
                refill();
                continue;
            }
            out[o++] = pend_[pos_++];
        }
        return o;
    }

private:
    enum class Stage : std::uint8_t { Header, Markers, First, Steps, Done };
    const Rec& r_;
    std::size_t n_, m_, i_ = 0;
    Stage stage_ = Stage::Header;
    std::array<std::uint8_t, 24> pend_{};
    std::size_t pos_ = 0, len_ = 0;
    Sample prev_{};
    std::int32_t pdt_ = 0;

    void put(std::uint8_t b) { pend_[len_++] = b; }
    void var(std::uint32_t v) {
        while (v >= 0x80) { put(std::uint8_t(v | 0x80)); v >>= 7; }
        put(std::uint8_t(v));
    }
    void put32(std::uint32_t v) { for (int i = 0; i < 4; i++) put(std::uint8_t(v >> (8 * i))); }
    struct Delta { std::int32_t dt, ddt, dv, d1, d2; std::uint8_t flags; };
    Delta delta(const Sample& s) const {
        Delta d;
        d.dt = std::int32_t(s.t_us - prev_.t_us);
        d.ddt = d.dt - pdt_;
        d.dv = std::int32_t(s.vbus_mv) - prev_.vbus_mv;
        d.d1 = std::int32_t(s.cc1) - prev_.cc1;
        d.d2 = std::int32_t(s.cc2) - prev_.cc2;
        d.flags = std::uint8_t((d.ddt != 0) | (d.dv != 0) << 1 | (d.d1 != 0) << 2 | (d.d2 != 0) << 3);
        return d;
    }
    void refill() {
        pos_ = len_ = 0;
        switch (stage_) {
        case Stage::Header:
            for (auto c : kStreamMagic) put(c);
            put(std::uint8_t(n_)); put(std::uint8_t(n_ >> 8));
            put(std::uint8_t(m_)); put(std::uint8_t(r_.reason()));
            put32(r_.trigger_us());
            stage_ = m_ ? Stage::Markers : n_ ? Stage::First : Stage::Done;
            return;
        case Stage::Markers: {
            const Marker k = r_.marker(i_++);
            put32(k.t_us); put(k.code); put(k.step); put(std::uint8_t(k.arg)); put(std::uint8_t(k.arg >> 8));
            if (i_ == m_) { i_ = 0; stage_ = n_ ? Stage::First : Stage::Done; }
            return;
        }
        case Stage::First:
            prev_ = r_.sample(0);
            put32(prev_.t_us); put(std::uint8_t(prev_.vbus_mv)); put(std::uint8_t(prev_.vbus_mv >> 8));
            put(prev_.cc1); put(prev_.cc2);
            i_ = 1;
            stage_ = n_ > 1 ? Stage::Steps : Stage::Done;
            return;
        case Stage::Steps: {
            Delta d = delta(r_.sample(i_));
            if (d.flags == 0) {   // a run of identical steps
                std::uint32_t run = 0;
                while (i_ < n_ && d.flags == 0) {
                    ++run;
                    pdt_ = d.dt;
                    prev_ = r_.sample(i_++);
                    if (i_ < n_) d = delta(r_.sample(i_));
                }
                put(0); var(run - 1);
            } else {
                put(d.flags);
                if (d.ddt != 0) var(detail::zz(d.ddt));
                if (d.dv != 0) var(detail::zz(d.dv));
                if (d.d1 != 0) var(detail::zz(d.d1));
                if (d.d2 != 0) var(detail::zz(d.d2));
                pdt_ = d.dt;
                prev_ = r_.sample(i_++);
            }
            if (i_ >= n_) stage_ = Stage::Done;
            return;
        }
        case Stage::Done: return;
        }
    }
};

// Whole-capture encoder into `out`. Returns the byte count, 0 when `out` is too small.
template <class Rec>
std::size_t encode(const Rec& r, std::span<std::uint8_t> out) {
    Encoder<Rec> e(r);
    const std::size_t n = e.read(out);
    return e.done() ? n : 0;
}

struct Decoded {
    std::uint16_t n_samples = 0;
    std::uint8_t n_markers = 0;
    Reason reason = Reason::None;
    std::uint32_t trigger_us = 0;
};

// Decodes a stream: markers into `mk`, samples into `sm` (both must hold the counts the header names).
// Returns false on a bad magic, a truncated stream or too small output spans.
inline bool decode(std::span<const std::uint8_t> in, std::span<Sample> sm, std::span<Marker> mk, Decoded& d) {
    using namespace detail;
    if (in.size() < kHeaderBytes) return false;
    for (std::size_t i = 0; i < 4; i++) if (in[i] != kStreamMagic[i]) return false;
    d.n_samples = std::uint16_t(in[4] | in[5] << 8);
    d.n_markers = in[6];
    d.reason = Reason(in[7]);
    d.trigger_us = get32(&in[8]);
    if (sm.size() < d.n_samples || mk.size() < d.n_markers) return false;
    std::size_t o = kHeaderBytes;
    if (in.size() < o + std::size_t(d.n_markers) * 8) return false;
    for (std::size_t i = 0; i < d.n_markers; i++, o += 8)
        mk[i] = {get32(&in[o]), in[o + 4], in[o + 5], std::uint16_t(in[o + 6] | in[o + 7] << 8)};
    if (d.n_samples == 0) return true;
    if (in.size() < o + 8) return false;
    Sample prev{get32(&in[o]), std::uint16_t(in[o + 4] | in[o + 5] << 8), in[o + 6], in[o + 7]};
    o += 8;
    sm[0] = prev;
    auto var = [&](std::uint32_t& v) {
        v = 0;
        for (unsigned sh = 0; sh < 35; sh += 7) {
            if (o >= in.size()) return false;
            const std::uint8_t b = in[o++];
            v |= std::uint32_t(b & 0x7F) << sh;
            if (!(b & 0x80)) return true;
        }
        return false;
    };
    std::int32_t pdt = 0;
    for (std::size_t i = 1; i < d.n_samples;) {
        if (o >= in.size()) return false;
        const std::uint8_t flags = in[o++];
        std::uint32_t cnt = 1;
        std::int32_t ddt = 0, dv = 0, d1 = 0, d2 = 0;
        std::uint32_t v;
        if (flags == 0) {
            if (!var(v)) return false;
            cnt = v + 1;
        } else {
            if ((flags & 1) && !var(v)) return false; else if (flags & 1) ddt = unzz(v);
            if ((flags & 2) && !var(v)) return false; else if (flags & 2) dv = unzz(v);
            if ((flags & 4) && !var(v)) return false; else if (flags & 4) d1 = unzz(v);
            if ((flags & 8) && !var(v)) return false; else if (flags & 8) d2 = unzz(v);
        }
        for (std::uint32_t k = 0; k < cnt; k++, i++) {
            if (i >= d.n_samples) return false;
            const std::int32_t dt = pdt + ddt;
            prev = {prev.t_us + std::uint32_t(dt), std::uint16_t(std::int32_t(prev.vbus_mv) + dv),
                    std::uint8_t(std::int32_t(prev.cc1) + d1), std::uint8_t(std::int32_t(prev.cc2) + d2)};
            sm[i] = prev;
            pdt = dt;
            ddt = dv = d1 = d2 = 0;
        }
    }
    return true;
}

// Is a kept capture due for the flash write now? Quiet link (or the maximum wait elapsed) as before, but never while
// the scratch arena is leased: poll() also runs from the LCD idle hook and from delay_ms(), i.e. inside a page paint
// or a storage call that may hold the lease, and the save takes its own lease. It simply retries at the next poll.
constexpr bool persist_due(bool keep, std::uint32_t saved_seq, std::uint32_t waited_ms, std::uint32_t since_activity_ms,
                           std::uint32_t quiet_ms, std::uint32_t max_wait_ms, bool scratch_busy) {
    if (!keep || saved_seq != 0 || scratch_busy) return false;
    return since_activity_ms >= quiet_ms || waited_ms >= max_wait_ms;
}

// A decoded capture as a Recorder-shaped view (for make_thumb / the host plot code paths).
struct View {
    std::span<const Sample> s;
    std::span<const Marker> m;
    Reason r = Reason::None;
    std::uint32_t trig = 0;
    std::size_t sample_count() const { return s.size(); }
    std::size_t marker_count() const { return m.size(); }
    Sample sample(std::size_t i) const { return s[i]; }
    Marker marker(std::size_t i) const { return m[i]; }
    Reason reason() const { return r; }
    std::uint32_t trigger_us() const { return trig; }
};

}  // namespace k2::transcap
