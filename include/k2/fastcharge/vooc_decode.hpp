// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Inline VOOC / SuperVOOC frame decoder, pure (host-tested against cap1 slices, tests/test_vooc_decode.cpp). It eats the
// D+/D- edge words of the passive sniffer (k2/fastcharge/edges.hpp: cycles & ~63 | dm << 1 | dp, the state AFTER the
// edge) one at a time and keeps a small live summary of what a phone and its adapter say to each other. It only reads
// words: it has no pin, register or driver in reach (tests/test_vooc_decode_isolation.py checks the includes).
//
// The framing rules are tools/vooc_edges.py's (reverse-engineered from runs/2026-11-11-op15-svooc/cap1.edges):
//   - edges that repeat the previous (dm, dp) state are dropped (the two EXTI lines race);
//   - a pulse is a D+ fall then a rise; pulses whose rises are at most kGroupGapUs apart form one frame;
//   - bit k of a frame is D- just before pulse k+1 falls, the last bit D- kLastBitDelayUs after the last rise;
//   - the idle is D- before pulse 1 falls; a first pulse longer than kLongPulseUs (D+ held low before the frame, the
//     first ident after the wake) is timed from its rise and its idle is D- kLongPulseUs before that rise;
//   - 9 pulses are a complete frame; more are a wake burst, fewer a partial;
//   - a frame is a reply when its idle is 1 or it starts within kPairMaxUs of a command, otherwise a command (8 bits and
//     a trailing 0); a command and the reply after it are one pair;
//   - a session runs from the first complete frame after kSessionGapMs of silence to its last frame;
//   - its handshake: when the first pair is an ident, every pair up to the first pause over kHandshakeGapMs after the
//     ident was answered (a non-zero reply); the pairs after it are the steady state, where kSteadyCmd (0x82) polls.
// The command names the docs give (0x82 "current limit", 0x84 "voltage ramp" ...) are guesses; the summary keeps raw
// command bytes and 9-bit reply words only.
//
// Time is the 32-bit cycle count of the words, compared as unsigned differences: the producer (the ISR) never goes
// back. Silence longer than the 17.9 s the cycle counter takes to wrap would alias, so the caller calls tick(now) from
// the main loop: it closes the frame, pair and session whose gap has passed, and forgets old timestamps.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/fastcharge/vooc.hpp"

namespace k2::vooc {

inline constexpr std::uint32_t kCyclesPerUs = 240;      // the DWT counter at 240 MHz
inline constexpr std::uint32_t kGroupGapUs = 2000;      // tool FRAME_GAP_US
inline constexpr std::uint32_t kLongPulseUs = 1000;     // tool LONG_PULSE_US
inline constexpr std::uint32_t kPairMaxUs = kReplyWaitUs;   // tool PAIR_MAX_US (12 ms)
inline constexpr std::uint32_t kSessionGapMs = 1000;    // tool SESSION_GAP_MS
inline constexpr std::uint32_t kHandshakeGapMs = 60;    // tool HANDSHAKE_GAP_MS
inline constexpr std::uint32_t kSteadyMaxMs = 200;      // tool STEADY_MAX_MS: longer 0x82 gaps are no period
inline constexpr std::uint8_t kSteadyCmd = 0x82;
inline constexpr std::size_t kMaxCmds = 16;             // distinct commands kept; cap1 shows 13
inline constexpr std::uint16_t kNoReply = 0xFFFF;

enum Event : unsigned { kSessionStart = 1, kSessionEnd = 2 };

struct CmdStat {
    std::uint8_t cmd = 0;
    std::uint16_t count = 0;
    std::uint16_t last = kNoReply;   // the 9-bit reply word of the latest pair that had one
};

// Counters since the decoder was reset (edges .. partials, sessions) and of the current or last session (the rest).
struct Summary {
    std::uint32_t edges = 0;          // edges that changed the line state
    std::uint32_t frames = 0;         // complete frames, commands and replies
    std::uint16_t sessions = 0;
    std::uint16_t wakes = 0;          // frames of more than 9 clocks (the phone's wake burst)
    std::uint16_t partials = 0;       // frames of 1..8 clocks
    bool active = false;              // a session runs now
    // --- the current / last session
    bool handshake = false;           // it opened with an ident the phone answered
    std::uint8_t ident = 0;           // that ident command and the reply word
    std::uint16_t ident_reply = 0;
    std::uint8_t hs_pairs = 0;        // pairs in the handshake
    std::uint32_t pairs = 0;          // command / reply pairs (a command without reply counts too)
    std::uint16_t r82_last = kNoReply, r82_min = kNoReply, r82_max = 0, r82_n = 0;   // steady 0x82 replies
    std::uint32_t p82_last_us = 0, p82_sum_us = 0;   // steady 0x82 -> 0x82 gaps under kSteadyMaxMs
    std::uint16_t p82_n = 0;
    std::uint8_t ncmds = 0;
    std::uint16_t other = 0;          // pairs of commands beyond the table
    std::array<CmdStat, kMaxCmds> cmds{};
    std::uint32_t p82_mean_us() const { return p82_n ? p82_sum_us / p82_n : 0; }
};

// The inline monitor may hold D+ / D- as floating inputs when the operator left it on, a data device draws current
// through the K2 (link::Reason::DataDevice: inline without a PD contract) and nothing else owns the lines (no fast-charge
// engine, scan, probe, reset hold or Black Box arm of its own).
constexpr bool monitor_wanted(bool enabled, bool inline_data, bool pd_contract, bool lines_free) {
    return enabled && inline_data && !pd_contract && lines_free;
}

// What the screen may show of a running monitor: `seen` while a session is live and proven VOOC (the phone answered
// an ident, or the steady 0x82 exchange runs: a monitor started mid-charge never sees the handshake), with the last
// raw 0x82 reply word (kNoReply before the first). Nothing once the session ended or the monitor stopped.
struct Live {
    bool seen = false;
    std::uint16_t r82 = kNoReply;
};
constexpr Live live_of(bool running, const Summary& s) {
    Live l;
    l.seen = running && s.active && (s.handshake || s.r82_n > 0);
    if (l.seen) l.r82 = s.r82_last;
    return l;
}

class Decoder {
public:
    void reset() { *this = Decoder{}; }
    const Summary& summary() const { return s_; }

    // One edge word. Returns the Event bits it caused.
    unsigned push(std::uint32_t word) {
        const std::uint32_t t = word & ~63u;
        const std::uint8_t st = std::uint8_t(word & 3u);
        unsigned ev = expire(t);
        if (have_ && st == st_) return ev;
        const bool dp = st & 1u, dm = st & 2u;
        const bool was_dp = st_ & 1u, was_dm = st_ & 2u;
        if (!have_) { have_ = true; st_ = st; return ev; }
        ++s_.edges;
        if (dm != was_dm && low_) { dm_prev_ = was_dm; dm_chg_t_ = t; dm_chg_ = true; }
        if (was_dp && !dp) {                      // a fall: the pulse's idle / previous bit is D- just before it
            low_ = true; long_ = false; fall_t_ = t; fall_dm_ = was_dm; dm_chg_ = false;
        } else if (!was_dp && dp && low_) {       // a rise ends the pulse
            low_ = false;
            ev |= rise(t, was_dm, dm);
        }
        st_ = st;
        if (n_ && t - last_rise_ <= kLastBitDelayUs * kCyclesPerUs) last_dm_ = dm;
        return ev;
    }

    // Main loop: `now` in the words' cycle time, read after the edges up to it were pushed.
    unsigned tick(std::uint32_t now) { return expire(now); }
    // The monitor stops: close the frame, pair and session in progress as if the line went quiet at `now`.
    unsigned end(std::uint32_t now) { return expire(now + us(2 * kSessionGapMs * 1000)); }

private:
    static constexpr std::uint32_t us(std::uint32_t v) { return v * kCyclesPerUs; }

    unsigned rise(std::uint32_t t, bool dm_before, bool dm_after) {
        unsigned ev = 0;
        if (n_ && t - last_rise_ > us(kGroupGapUs)) ev |= close_frame();
        if (!n_) {
            n_ = 1; bits_ = 0;
            g_fall_ = fall_t_; g_idle_ = fall_dm_;
            if (long_ || t - fall_t_ > us(kLongPulseUs)) {   // D+ held low before the frame: time it from the rise
                g_fall_ = t;
                g_idle_ = (dm_chg_ && t - dm_chg_t_ < us(kLongPulseUs)) ? dm_prev_ : dm_before;
            }
        } else {
            bits_ = std::uint16_t(bits_ << 1 | (fall_dm_ ? 1u : 0u));   // the bit of the previous pulse
            ++n_;
        }
        last_rise_ = t;
        last_dm_ = dm_after;
        return ev;
    }

    unsigned expire(std::uint32_t t) {
        unsigned ev = 0;
        if (n_ && t - last_rise_ > us(kGroupGapUs)) ev |= close_frame();
        if (low_ && t - fall_t_ > us(kLongPulseUs)) long_ = true;
        if (dm_chg_ && t - dm_chg_t_ >= us(kLongPulseUs)) dm_chg_ = false;
        // A frame starts at its first fall (or at the rise of a long first pulse), so the role and the pairing of the
        // next frame are decided only once no frame is open and no short pulse is in progress.
        const bool between = !n_ && (!low_ || long_);
        if (prev_ && between && t - prev_t_ >= us(kPairMaxUs)) prev_ = false;
        if (pend_ && between && t - pend_t_ >= us(kPairMaxUs)) pair(pend_t_, pend_cmd_, kNoReply);
        if (sess_ && !n_ && !pend_ && t - last_t_ > us(kSessionGapMs * 1000)) ev |= end_session();
        if (hs_t_ok_ && t - pair_t_ > us(kSteadyMaxMs * 1000)) hs_t_ok_ = false, steady_prev_ = false;
        return ev;
    }

    unsigned close_frame() {
        const int n = n_;
        const std::uint16_t v = std::uint16_t(bits_ << 1 | (last_dm_ ? 1u : 0u));
        n_ = 0;
        const std::uint32_t t = g_fall_;
        const bool reply = g_idle_ || (prev_ && prev_cmd_ && t - prev_t_ < us(kPairMaxUs));
        prev_ = true; prev_cmd_ = !reply; prev_t_ = t;
        if (n > kFrameBits) { ++s_.wakes; return 0; }
        if (n < kFrameBits) { ++s_.partials; return 0; }
        unsigned ev = 0;
        if (sess_ && t - last_t_ > us(kSessionGapMs * 1000)) ev |= end_session();
        if (!sess_) ev |= start_session();
        last_t_ = t;
        ++s_.frames;
        if (pend_ && (!reply || t - pend_t_ >= us(kPairMaxUs))) pair(pend_t_, pend_cmd_, kNoReply);
        if (!reply) { pend_ = true; pend_cmd_ = std::uint8_t(v >> 1); pend_t_ = t; }
        else if (pend_) pair(pend_t_, pend_cmd_, v);
        return ev;
    }

    unsigned start_session() {
        const Summary keep = s_;
        s_ = Summary{};
        s_.edges = keep.edges; s_.frames = keep.frames; s_.wakes = keep.wakes; s_.partials = keep.partials;
        s_.sessions = std::uint16_t(keep.sessions + 1);
        s_.active = true;
        sess_ = true; hs_ = Hs::First; answered_ = false; steady_prev_ = false; hs_t_ok_ = false;
        return kSessionStart;
    }

    unsigned end_session() {
        if (pend_) pair(pend_t_, pend_cmd_, kNoReply);
        sess_ = false;
        s_.active = false;
        return kSessionEnd;
    }

    void pair(std::uint32_t t, std::uint8_t cmd, std::uint16_t reply) {
        pend_ = false;
        ++s_.pairs;
        count(cmd, reply);
        const bool first = hs_ == Hs::First;
        if (first) hs_ = is_ident(cmd) ? Hs::Open : Hs::Done;
        if (hs_ == Hs::Open) {
            if (answered_ && !first && t - pair_t_ > us(kHandshakeGapMs * 1000)) {
                hs_ = Hs::Done;
            } else {
                if (s_.hs_pairs < 0xFF) ++s_.hs_pairs;
                if (is_ident(cmd) && reply != kNoReply && reply != 0) {
                    answered_ = true; s_.handshake = true; s_.ident = cmd; s_.ident_reply = reply;
                }
            }
        }
        if (hs_ == Hs::Done) steady(t, cmd, reply);
        pair_t_ = t; hs_t_ok_ = true;
    }

    void steady(std::uint32_t t, std::uint8_t cmd, std::uint16_t reply) {
        if (cmd == kSteadyCmd) {
            if (reply != kNoReply) {
                s_.r82_last = reply;
                if (s_.r82_min == kNoReply || reply < s_.r82_min) s_.r82_min = reply;
                if (reply > s_.r82_max) s_.r82_max = reply;
                if (s_.r82_n < 0xFFFF) ++s_.r82_n;
            }
            const std::uint32_t gap = t - steady_t_;
            if (steady_prev_ && steady_cmd_ == kSteadyCmd && gap < us(kSteadyMaxMs * 1000)) {
                s_.p82_last_us = gap / kCyclesPerUs;
                if (s_.p82_n < 0xFFFF) { s_.p82_sum_us += s_.p82_last_us; ++s_.p82_n; }
            }
        }
        steady_prev_ = true; steady_cmd_ = cmd; steady_t_ = t;
    }

    void count(std::uint8_t cmd, std::uint16_t reply) {
        for (std::size_t i = 0; i < s_.ncmds; ++i)
            if (s_.cmds[i].cmd == cmd) {
                if (s_.cmds[i].count < 0xFFFF) ++s_.cmds[i].count;
                if (reply != kNoReply) s_.cmds[i].last = reply;
                return;
            }
        if (s_.ncmds == kMaxCmds) { if (s_.other < 0xFFFF) ++s_.other; return; }
        s_.cmds[s_.ncmds++] = {cmd, 1, reply};
    }

    enum class Hs : std::uint8_t { First, Open, Done };

    Summary s_;
    // edge level
    bool have_ = false;
    std::uint8_t st_ = 0;
    // the pulse in progress (D+ low)
    bool low_ = false, long_ = false, fall_dm_ = false;
    std::uint32_t fall_t_ = 0;
    bool dm_chg_ = false, dm_prev_ = false;   // the last D- change while D+ is low, for a long pulse's idle
    std::uint32_t dm_chg_t_ = 0;
    // the frame in progress
    int n_ = 0;
    std::uint16_t bits_ = 0;
    bool g_idle_ = false, last_dm_ = false;
    std::uint32_t g_fall_ = 0, last_rise_ = 0;
    // the previous frame (any length), for the reply role
    bool prev_ = false, prev_cmd_ = false;
    std::uint32_t prev_t_ = 0;
    // the command waiting for its reply
    bool pend_ = false;
    std::uint8_t pend_cmd_ = 0;
    std::uint32_t pend_t_ = 0;
    // the session
    bool sess_ = false, answered_ = false, steady_prev_ = false, hs_t_ok_ = false;
    Hs hs_ = Hs::First;
    std::uint8_t steady_cmd_ = 0;
    std::uint32_t last_t_ = 0, pair_t_ = 0, steady_t_ = 0;
};

}  // namespace k2::vooc
