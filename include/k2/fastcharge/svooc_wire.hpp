// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Guarded SuperVOOC trigger, part 2 of 3: the bit layer, pure (tests/test_svooc_wire.cpp replays the captured sessions
// through it). It eats the D+/D- edge words of the dpsniff ISR (k2/fastcharge/edges.hpp: cycles & ~63 | dm << 1 | dp, the
// state after the edge) and answers each with an Action for the ISR's thin hardware part: take D- (drive it high), set
// D- to a bit at a given cycle time, or let D- go. It owns the responder (svooc_script.hpp) and the log of the pairs
// (kLogPairs of them, each stamped in ms since the wire began).
// It reaches no pin, register or driver.
//
// Framing (tools/vooc_edges.py, the ground truth; reverse-engineered from the OnePlus 15 + SUPERVOOC 120 W captures):
//   - the adapter clocks every frame on D+ with 9 low pulses (25-40 us wide, 0.46 / 0.54 ms apart); a fall more than
//     kGroupGapUs after the last rise starts a frame, and so does a rise after D+ was low over kLongPulseUs (the first
//     ident after the wake: D+ held low ~540 ms, then pulse 1 is only its rise);
//   - a command frame: bit k is D- just before pulse k+1 falls, i.e. the D- state the edge word before that fall left
//     (the adapter moves D- ~30 us after a fall, so the ISR may run late without reading the next bit); bits 1..8 are
//     the command, the trailing bit 9 (always 0) is not sampled;
//   - the reply slot: the next frame, starting within kReplyWaitUs of the command's start (measured 8.8-9.4 ms). The
//     phone raised D- before pulse 1 and changed it kBitDelayUs (median 50 us, 45-66 us) after each rise, MSB first;
//     the adapter latches each bit just before the next fall (0.43-0.51 ms after the rise) and the last one after the
//     last rise. The phone then held the last bit 2.3-2.6 ms before pulling D- low until the next command.
// So the wire: at the slot's first fall Take (D- high, the phone-side idle); at rise k + kBitDelayUs bit k of the word;
// after the 9th bit it keeps D- driven and tick() lets go kHoldUs after the last rise, or the next frame's first edge
// does (the adapter drives D- in its own command frame). A slot whose clock stops is let go kCutUs after its last edge.
// Nothing is ever driven in a command frame. Once the responder aborts (svooc_script.hpp: off-script), stopped() is true
// and no slot is taken again.
//
// Time is the 32-bit cycle count of the words (240 MHz, wraps in 17.9 s), compared as unsigned differences; the main loop
// calls tick() often (it closes frames and expires slots), so no gap the wire compares comes near the wrap.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/fastcharge/svooc_script.hpp"
#include "k2/fastcharge/vooc.hpp"

namespace k2::svooc {

inline constexpr std::uint32_t kCyclesPerUs = 240;               // the DWT cycle counter at 240 MHz
inline constexpr std::uint32_t kBitDelayUs = vooc::kReplyDelayUs;   // 50: D- moves this long after the D+ rise
inline constexpr std::uint32_t kGroupGapUs = 2000;               // tool FRAME_GAP_US
inline constexpr std::uint32_t kLongPulseUs = 1000;              // tool LONG_PULSE_US
inline constexpr std::uint32_t kReplyWaitUs = vooc::kReplyWaitUs;   // 12 ms: the slot starts within this of the command
inline constexpr std::uint32_t kHoldUs = 2400;                   // the last bit stays on D- this long after the last rise
inline constexpr std::uint32_t kCutUs = 1000;                    // a slot whose clock stopped: let go this long after its last edge
inline constexpr std::uint16_t kNoWord = 0xFFFF;                 // a command whose slot never came or was not answered
inline constexpr std::size_t kLogPairs = 512;                    // a whole run: the handshake (25), the ramp, then ~20 s of polls

struct Action {
    enum class Kind : std::uint8_t { None, Take, Bit, Release };
    Kind kind = Kind::None;
    bool level = false;      // Bit: the D- level
    std::uint32_t at = 0;    // Bit: the cycle time to set it at (the ISR waits until then; set at once if already past)
};

struct LogPair {
    std::uint8_t cmd = 0;
    std::uint16_t word = kNoWord;
    std::uint16_t t_ms = 0;   // the command's frame start, ms since the wire began (saturates at 65 s)
};

// One row of the run's pair list as the remote reads it: the wire's LogPair plus the VBUS the glue sampled when the pair
// was closed (0 = none).
struct PairRow {
    std::uint8_t cmd = 0;
    std::uint16_t word = kNoWord;
    std::uint16_t t_ms = 0;
    std::uint16_t vbus_mv = 0;
};

struct Log {
    std::uint32_t commands = 0;   // complete command frames
    std::uint32_t replies = 0;    // reply slots driven through all nine bits
    std::uint32_t missed = 0;     // commands whose slot did not come within kReplyWaitUs
    std::uint32_t cut = 0;        // slots whose clock stopped before the ninth pulse
    std::uint32_t partials = 0;   // frames that were no command (not 9 pulses: a wake burst, a cut frame)
    std::uint32_t pairs_total = 0;   // pairs seen, also those beyond the log
    std::uint8_t last_cmd = 0;
    std::uint16_t n = 0;          // pairs kept, the first kLogPairs (read them with Wire::pair)
};

// Everything the wire resets at begin(); the pair array stays out of it so that reset never builds a 3 KB temporary.
struct WireState {
    enum class Role : std::uint8_t { Command, Slot };
    Responder resp_;
    Log log_;
    bool have_ = false;
    std::uint8_t st_ = 0;
    std::uint32_t t0_ = 0, fall_t_ = 0, last_rise_ = 0;
    // the frame in progress
    bool in_frame_ = false;
    Role role_ = Role::Command;
    std::uint32_t start_t_ = 0;
    int rises_ = 0, nbits_ = 0;
    std::uint16_t bits_ = 0;
    // the command waiting for its slot
    bool pend_ = false;
    std::uint8_t pend_cmd_ = 0;
    std::uint32_t pend_t_ = 0;
    // the slot being driven
    std::uint16_t word_ = 0;
    bool driving_ = false, done_ = false, taken_ = false, stopped_ = false;
    std::uint32_t drive_t_ = 0;
};

class Wire : private WireState {
public:
    // The line state when the ISR starts feeding the wire (an edge word of the current levels). The pair slots past
    // log().n are stale; only pair(i) for i < log().n is meaningful.
    void begin(std::uint32_t word) {
        static_cast<WireState&>(*this) = WireState{};
        have_ = true;
        st_ = std::uint8_t(word & 3u);
        t0_ = fall_t_ = last_rise_ = word & ~63u;
    }

    Action edge(std::uint32_t word) {
        const std::uint32_t t = word & ~63u;
        const std::uint8_t st = std::uint8_t(word & 3u);
        if (!have_) { begin(word); return {}; }
        if (st == st_) return {};
        const bool was_dp = st_ & 1u, dp = st & 1u, was_dm = st_ & 2u;
        st_ = st;
        if (was_dp == dp) return {};   // D- alone (the adapter's bits, or our own drive)
        if (!dp) {                     // a D+ fall
            fall_t_ = t;
            if (!in_frame_ || t - last_rise_ > us(kGroupGapUs)) return start_at_fall(t);
            if (role_ == Role::Command && rises_ > 0) {
                bits_ = std::uint16_t(bits_ << 1 | (was_dm ? 1u : 0u));
                ++nbits_;
            }
            if (role_ == Role::Slot && driving_ && rises_ >= kFrameBits) return release(t);   // a tenth pulse
            return {};
        }
        // a D+ rise
        if (!in_frame_ || t - fall_t_ > us(kLongPulseUs)) return start_at_rise(t);
        ++rises_;
        last_rise_ = t;
        if (role_ != Role::Slot || !driving_) return {};
        drive_t_ = t;
        if (rises_ > kFrameBits) return release(t);
        if (rises_ == kFrameBits) { ++log_.replies; done_ = true; }
        return {Action::Kind::Bit, ((word_ >> (kFrameBits - rises_)) & 1u) != 0, t + us(kBitDelayUs)};
    }

    // Main loop, interrupts masked, `now` in the words' cycle time: closes a frame whose clock went quiet, expires a
    // slot that never came, and lets go of D- after the hold (or a stopped clock).
    Action tick(std::uint32_t now) {
        if (in_frame_ && !low() && now - last_rise_ > us(kGroupGapUs)) close_frame();
        if (pend_ && now - pend_t_ > us(kReplyWaitUs)) miss();
        if (driving_ && now - drive_t_ >= us(done_ ? kHoldUs : kCutUs)) return release(now);
        return {};
    }

    // The trigger ends: no slot is taken again; D- goes if it is held.
    Action stop() {
        stopped_ = true;
        pend_ = false;
        return driving_ ? release(0) : Action{};
    }

    bool driving() const { return driving_; }
    bool stopped() const { return stopped_; }
    const Responder& responder() const { return resp_; }
    const Log& log() const { return log_; }
    const LogPair& pair(std::size_t i) const { return pairs_[i < kLogPairs ? i : kLogPairs - 1]; }

private:
    static constexpr int kFrameBits = vooc::kFrameBits;
    static constexpr std::uint32_t us(std::uint32_t v) { return v * kCyclesPerUs; }
    bool low() const { return (st_ & 1u) == 0; }

    Action release(std::uint32_t) {
        driving_ = false;
        return {Action::Kind::Release, false, 0};
    }

    void record(std::uint8_t cmd, std::uint16_t word) {
        ++log_.pairs_total;
        if (log_.n >= kLogPairs) return;
        const std::uint32_t ms = (pend_t_ - t0_) / (kCyclesPerUs * 1000u);
        pairs_[log_.n++] = {cmd, word, std::uint16_t(ms > 0xFFFFu ? 0xFFFFu : ms)};
    }

    void miss() {
        pend_ = false;
        ++log_.missed;
        record(pend_cmd_, kNoWord);
    }

    // The frame in progress is over (the next one starts, or the clock went quiet).
    void close_frame() {
        if (!in_frame_) return;
        in_frame_ = false;
        if (role_ == Role::Slot) {
            if (taken_ && rises_ < kFrameBits) ++log_.cut;
            return;
        }
        if (rises_ != kFrameBits || nbits_ != kFrameBits - 1) { ++log_.partials; return; }
        ++log_.commands;
        if (pend_) miss();   // two commands in a row: the first one's slot never came
        pend_ = true;
        pend_cmd_ = std::uint8_t(bits_ & 0xFFu);
        pend_t_ = start_t_;
    }

    void open(Role r, std::uint32_t t) {
        in_frame_ = true;
        role_ = r;
        start_t_ = t;
        rises_ = nbits_ = 0;
        bits_ = 0;
        done_ = taken_ = false;
    }

    // A fall after a long high: a command frame, or the reply slot of the pending command.
    Action start_at_fall(std::uint32_t t) {
        close_frame();
        Action a = driving_ ? release(t) : Action{};
        if (pend_ && t - pend_t_ > us(kReplyWaitUs)) miss();
        if (!pend_) { open(Role::Command, t); return a; }
        pend_ = false;
        open(Role::Slot, t);
        log_.last_cmd = pend_cmd_;
        if (stopped_) { record(pend_cmd_, kNoWord); return a; }
        const Answer ans = resp_.on_command(pend_cmd_);
        if (resp_.abort()) {   // the third off-script command in a row: no answer, the trigger stops
            stopped_ = true;
            record(pend_cmd_, kNoWord);
            return a;
        }
        record(pend_cmd_, ans.word);
        word_ = ans.word;
        driving_ = taken_ = true;
        drive_t_ = t;
        return {Action::Kind::Take, true, t};
    }

    // A rise after D+ was held low: pulse 1 of a command frame (the first ident after the wake).
    Action start_at_rise(std::uint32_t t) {
        close_frame();
        Action a = driving_ ? release(t) : Action{};
        if (pend_ && t - pend_t_ > us(kReplyWaitUs)) miss();
        open(Role::Command, t);
        rises_ = 1;
        last_rise_ = t;
        return a;
    }

    std::array<LogPair, kLogPairs> pairs_{};
};

}  // namespace k2::svooc
