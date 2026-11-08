// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS probe, pure: the handshake plus the opener exchange at 5 V and nothing else, so frame and timing
// variants (k2/fastcharge/ufcs_tune.hpp) can be tried on a real source without reflashing. The probe is a state
// machine over a small environment (the hardware glue is in fctrig.cpp); it never goes through the
// fast-charge engine, so it cannot raise the voltage and it never builds a Request.
//
// Safety, all checked here:
//  - start() refuses unless VBUS reads 4.5 to 5.5 V ("5 V only") and no PD contract or load is inline;
//  - every poll re-checks VBUS (above 5.5 V or below 2 V) and the inline state and aborts on a change;
//  - the only frames sent are the tuner's opener (retransmitted) and an Ack for each frame the source
//    sent; an Ack/Nack is never acknowledged;
//  - every end (window over, answer, abort, stop) closes the UART and starts the bus reset, so the
//    source sees the lines released and starts over for the next try.
//
// What it records (read back through blob()): handshake timings and class, every TX frame and every raw
// RX byte run with its time since the first frame, the UART error counts, VBUS start / min / max.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/drivers/uart2.hpp"
#include "k2/fastcharge/ufcs.hpp"
#include "k2/fastcharge/ufcs_diag.hpp"
#include "k2/fastcharge/ufcs_tune.hpp"

namespace k2::ufcs {

using UartErrors = uart2::Errors;

// Resetting is the glue's own refusal (a bus reset hold from the previous probe is still running, the
// source has not seen the lines released long enough); the Prober itself never returns it.
enum class ProbeStart : std::uint8_t { Ok, Busy, Inline, NotFiveVolts, NoMonitor, Resetting };

// Outcome of the last probe. Answered = at least one decodable frame; Garbage = RX bytes but none
// decodable with the tuned format (a lone 0x00 with a framing error is the break the K2's own line
// release makes, the sweep tool tells it apart); Silent = nothing received.
enum class ProbeVerdict : std::uint8_t { None, NoHandshake, Silent, Garbage, Answered, Overvoltage, SourceLost, Inline, Stopped };

constexpr const char* name(ProbeVerdict v) {
    switch (v) {
    case ProbeVerdict::NoHandshake: return "no-handshake";
    case ProbeVerdict::Silent: return "silent";
    case ProbeVerdict::Garbage: return "garbage";
    case ProbeVerdict::Answered: return "answered";
    case ProbeVerdict::Overvoltage: return "overvoltage";
    case ProbeVerdict::SourceLost: return "source-lost";
    case ProbeVerdict::Inline: return "inline";
    case ProbeVerdict::Stopped: return "stopped";
    case ProbeVerdict::None: break;
    }
    return "none";
}

constexpr const char* name(ProbeStart s) {
    switch (s) {
    case ProbeStart::Ok: return "ok";
    case ProbeStart::Busy: return "busy";
    case ProbeStart::Inline: return "inline";
    case ProbeStart::NotFiveVolts: return "not-5v";
    case ProbeStart::NoMonitor: return "no-monitor";
    case ProbeStart::Resetting: return "resetting";
    }
    return "?";
}

// Limits (millivolts, milliseconds).
inline constexpr int kProbeStartMinMv = 4500, kProbeStartMaxMv = 5500;   // "5 V only": the window start() accepts
inline constexpr int kProbeAbortHighMv = 5500, kProbeAbortLowMv = 2000;  // a running probe aborts outside these
inline constexpr std::uint32_t kProbeAnswerGraceMs = 100;                // keep listening this long after the last frame
inline constexpr std::uint32_t kProbeMaxMs = 3000;                       // a chatty source cannot extend the probe past this

// Result blob (little endian), read by the remote in chunks:
//   0 u8 format version (1)      1 u8 state (0 idle, 1 running, 2 done)   2 u8 verdict   3 u8 handshake class
//   4 i16 x5 handshake us: rise1, fall1, rise2, fall2, final (-1 = D+ never moved)
//   14 u8 tx frames   15 u8 rx frames   16 u16 rx bytes   18..24 u16 framing, noise, overrun, parity errors
//   26 u16 vbus at start   28 u16 vbus min   30 u16 vbus max (mV)   32 u16 total ms   34 u16 log bytes   36 u8 flags (bit 0 log truncated)
// followed by the log: events { u16 ms since the first frame, u8 kind (0 TX frame, 1 RX bytes), u8 n, n bytes }.
inline constexpr std::size_t kProbeHeader = 38;
inline constexpr std::size_t kProbeLogMax = 288;   // 3 opener frames + ~5 retransmits + RX chunks fit; flags bit 0 marks a cut log (RAM: K2_NET=1 has little to spare)
inline constexpr std::size_t kProbeBlobMax = kProbeHeader + kProbeLogMax;
inline constexpr std::uint8_t kProbeBlobVersion = 1;
inline constexpr std::uint8_t kEventTx = 0, kEventRx = 1;

struct ProbeResult {
    ProbeVerdict verdict = ProbeVerdict::None;
    HsClass hs_class = HsClass::NoAnswer;
    Handshake hs{{-1, -1}, {-1, -1}, -1};
    int tx_frames = 0, rx_frames = 0, rx_bytes = 0;
    UartErrors err{};
    int vbus_start = 0, vbus_min = 0, vbus_max = 0;
    int total_ms = 0;
    bool truncated = false;
};

struct ProbeEnv {
    virtual std::uint32_t now_ms() = 0;
    virtual int vbus_mv() = 0;          // latest fast sample, -1 when there is no monitor
    virtual bool inline_now() = 0;      // a PD contract or a load behind the K2
    virtual bool open(Handshake& hs) = 0;   // handshake, then the UART with the tuner's line settings; false = no handshake
    virtual bool send(std::span<const std::uint8_t> bytes) = 0;
    virtual std::size_t recv(std::span<std::uint8_t> out) = 0;
    virtual UartErrors errors() = 0;
    virtual void close() = 0;           // UART off, lines to the tuner's idle
    virtual void reset_bus() = 0;       // start the bus reset hold

protected:
    ~ProbeEnv() = default;   // not virtual: a virtual destructor would link operator delete and the heap (no allocation here)
};

class Prober {
public:
    explicit Prober(ProbeEnv& env) : env_(env) {}

    void set_tuner(const Tuner& t) { tuner_ = t; }
    const Tuner& tuner() const { return tuner_; }

    ProbeStart start() {
        if (running_) return ProbeStart::Busy;
        if (env_.inline_now()) return ProbeStart::Inline;
        const int v = env_.vbus_mv();
        if (v < 0) return ProbeStart::NoMonitor;
        if (v < kProbeStartMinMv || v > kProbeStartMaxMv) return ProbeStart::NotFiveVolts;
        res_ = {};
        res_.vbus_start = res_.vbus_min = res_.vbus_max = v;
        log_n_ = 0;
        used_tuner_ = tuner_;
        running_ = true;
        opened_ = false;
        done_ = false;
        return ProbeStart::Ok;
    }

    bool running() const { return running_; }
    bool done() const { return done_; }
    const ProbeResult& result() const { return res_; }

    // Main-loop step: the first call runs the (blocking) handshake, later calls are short.
    void poll() {
        if (!running_) return;
        if (!opened_) {
            open_session();
            return;
        }
        const std::uint32_t t = env_.now_ms() - t0_;
        const int v = env_.vbus_mv();
        if (v >= 0) {
            if (v < res_.vbus_min) res_.vbus_min = v;
            if (v > res_.vbus_max) res_.vbus_max = v;
            if (v > kProbeAbortHighMv) return finish(ProbeVerdict::Overvoltage, t);
            if (v < kProbeAbortLowMv) return finish(ProbeVerdict::SourceLost, t);
        }
        if (env_.inline_now()) return finish(ProbeVerdict::Inline, t);

        std::array<std::uint8_t, 64> buf;
        const std::size_t n = env_.recv(buf);
        if (n > 0) {
            res_.rx_bytes += static_cast<int>(n);
            add_event(kEventRx, t, {buf.data(), n});
            feed({buf.data(), n}, t);
        }
        const bool answered = res_.rx_frames > 0;
        if (answered ? t >= last_rx_ms_ + kProbeAnswerGraceMs || t >= kProbeMaxMs : t >= used_tuner_.listen_ms) {
            return finish(answered ? ProbeVerdict::Answered : res_.rx_bytes > 0 ? ProbeVerdict::Garbage : ProbeVerdict::Silent, t);
        }
        if (!answered && t >= due_ms_) {
            ++tries_;
            send_opener(t);
            due_ms_ = t + open_interval_ms(used_tuner_, tries_);
        }
    }

    // Abort now (remote reset, leaving the page): same release as a normal end. No effect when idle.
    void stop() {
        if (!running_) return;
        if (opened_) return finish(ProbeVerdict::Stopped, env_.now_ms() - t0_);
        running_ = false;   // started but the handshake has not run: no line was touched, nothing to release
        done_ = true;
        res_.verdict = ProbeVerdict::Stopped;
    }

    // The result as one blob (layout above); valid at any time, refreshed on each call.
    std::span<const std::uint8_t> blob() {
        auto& b = blob_;
        std::size_t o = 0;
        auto put8 = [&](unsigned v) { b[o++] = static_cast<std::uint8_t>(v); };
        auto put16 = [&](unsigned v) { b[o++] = static_cast<std::uint8_t>(v); b[o++] = static_cast<std::uint8_t>(v >> 8); };
        auto sat = [](int v) { return static_cast<unsigned>(v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v); };
        put8(kProbeBlobVersion);
        put8(running_ ? 1 : done_ ? 2 : 0);
        put8(static_cast<unsigned>(res_.verdict));
        put8(static_cast<unsigned>(res_.hs_class));
        const int hs[5] = {res_.hs.rise_us[0], res_.hs.fall_us[0], res_.hs.rise_us[1], res_.hs.fall_us[1], res_.hs.final_us};
        for (const int us : hs) put16(static_cast<std::uint16_t>(static_cast<std::int16_t>(us < -1 ? -1 : us > 32767 ? 32767 : us)));
        put8(static_cast<unsigned>(res_.tx_frames > 255 ? 255 : res_.tx_frames));
        put8(static_cast<unsigned>(res_.rx_frames > 255 ? 255 : res_.rx_frames));
        put16(sat(res_.rx_bytes));
        put16(sat(static_cast<int>(res_.err.framing)));
        put16(sat(static_cast<int>(res_.err.noise)));
        put16(sat(static_cast<int>(res_.err.overrun)));
        put16(sat(static_cast<int>(res_.err.parity)));
        put16(sat(res_.vbus_start));
        put16(sat(res_.vbus_min));
        put16(sat(res_.vbus_max));
        put16(sat(res_.total_ms));
        put16(static_cast<unsigned>(log_n_));
        put8(res_.truncated ? 1 : 0);
        put8(0);
        return {b.data(), kProbeHeader + log_n_};
    }

private:
    void open_session() {
        Handshake hs{{-1, -1}, {-1, -1}, -1};
        const bool ok = env_.open(hs);
        res_.hs = hs;
        res_.hs_class = classify(hs);
        if (!ok) {
            running_ = false;
            done_ = true;
            res_.verdict = ProbeVerdict::NoHandshake;
            env_.reset_bus();
            return;
        }
        opened_ = true;
        t0_ = env_.now_ms();
        tries_ = 0;
        acc_n_ = 0;
        last_rx_ms_ = 0;
        send_opener(0);
        due_ms_ = used_tuner_.retry_ms;
    }

    void send_opener(std::uint32_t t) {
        std::array<std::uint8_t, 8> f{};
        const std::size_t n = encode_control(f, used_tuner_.opener, 0, used_tuner_);
        send_frame({f.data(), n}, t);
    }

    void send_frame(std::span<const std::uint8_t> f, std::uint32_t t) {
        env_.send(f);
        res_.tx_frames++;
        add_event(kEventTx, t, f);
    }

    // Accumulate bytes and decode frames with the tuner's format. A decodable frame counts as an answer
    // and is acknowledged (never an Ack or Nack); nothing else is ever sent in reply.
    void feed(std::span<const std::uint8_t> in, std::uint32_t t) {
        for (const std::uint8_t byte : in) {
            if (acc_n_ == acc_.size()) acc_n_ = 0;   // a full buffer without a frame is noise
            acc_[acc_n_++] = byte;
            for (;;) {
                Frame f;
                std::size_t used = 0;
                const Parse p = decode({acc_.data(), acc_n_}, f, used, nullptr, used_tuner_);
                if (p == Parse::Ok) {
                    res_.rx_frames++;
                    last_rx_ms_ = t;
                    if (f.data || (f.cmd != kCtlAck && f.cmd != kCtlNack)) {
                        std::array<std::uint8_t, 8> a{};
                        const std::size_t n = encode_control(a, kCtlAck, f.id, used_tuner_);
                        send_frame({a.data(), n}, t);
                    }
                }
                if (used > 0) {
                    for (std::size_t i = used; i < acc_n_; ++i) acc_[i - used] = acc_[i];
                    acc_n_ -= used;
                }
                if (p == Parse::Need || acc_n_ == 0) break;
            }
        }
    }

    void add_event(std::uint8_t kind, std::uint32_t t, std::span<const std::uint8_t> data) {
        if (res_.truncated) return;
        const std::size_t n = data.size() > 255 ? 255 : data.size();
        if (log_n_ + 4 + n > kProbeLogMax) {
            res_.truncated = true;   // latched: no later event is written, so the log stays in order
            return;
        }
        std::uint8_t* o = blob_.data() + kProbeHeader + log_n_;
        const unsigned ms = t > 0xFFFF ? 0xFFFF : t;
        o[0] = static_cast<std::uint8_t>(ms);
        o[1] = static_cast<std::uint8_t>(ms >> 8);
        o[2] = kind;
        o[3] = static_cast<std::uint8_t>(n);
        for (std::size_t i = 0; i < n; ++i) o[4 + i] = data[i];
        log_n_ += 4 + n;
    }

    void finish(ProbeVerdict v, std::uint32_t t) {
        res_.verdict = v;
        res_.total_ms = static_cast<int>(t);
        res_.err = env_.errors();
        if (opened_) env_.close();
        env_.reset_bus();
        running_ = false;
        done_ = true;
    }

    ProbeEnv& env_;
    Tuner tuner_{};
    Tuner used_tuner_{};
    ProbeResult res_{};
    bool running_ = false, opened_ = false, done_ = false;
    std::uint32_t t0_ = 0, due_ms_ = 0, last_rx_ms_ = 0;
    int tries_ = 0;
    std::array<std::uint8_t, kFrameMax> acc_{};
    std::size_t acc_n_ = 0;
    std::array<std::uint8_t, kProbeBlobMax> blob_{};
    std::size_t log_n_ = 0;
};

}  // namespace k2::ufcs
