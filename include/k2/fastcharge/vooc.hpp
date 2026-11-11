// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// OPPO VOOC / SuperVOOC phone-side slave, pure. The adapter is the clock master on D+: 9 low
// pulses (25-40 us, 0.46 / 0.54 ms apart) per frame; D- carries one bit per pulse, latched just before the
// next D+ fall (the last bit 300 us after the last pulse). An adapter command is 8 bits + a trailing 0;
// the phone answers about 9 ms later with a 9 bit word in the clock of the next frame, driving each D- bit
// about 50 us after the D+ rise of its pulse (tests/fixtures/oneplus15_svooc_init.edges, op15_svooc_entry.edges).
// The charger-test engine only listens: it ends the session on the first ident and answers nothing (operator rule
// 2026-11-11). tx_reply serves the lab `vooc answer` op alone.
#pragma once
#include <cstdint>

namespace k2::vooc {

inline constexpr int kFrameBits = 9;
inline constexpr std::uint32_t kFrameGapUs = 700;      // D+ steady this long (> the 0.54 ms in-frame gap): the next edge starts a frame
inline constexpr std::uint32_t kLastBitDelayUs = 300;  // last command bit: this long after the last pulse rises
inline constexpr std::uint32_t kReplyDelayUs = 50;     // phone drives D- this long after the D+ rise
inline constexpr std::uint32_t kReplyWaitUs = 12000;   // the reply slot starts about 9 ms after the command
inline constexpr int kAllOnes = 0x1FF;                 // D- floating high under the clock: nobody drives it

constexpr std::uint8_t frame_cmd(std::uint16_t frame) { return static_cast<std::uint8_t>(frame >> 1); }   // bit 0 is the trailing 0

// A frame the engine counts as heard: one came, and it is not all ones (the no-phone run of 2026-11-11 read 0x1FF in the
// reply slot after a missed ident; an all-ones word carries no adapter bit).
constexpr bool heard(int frame) { return frame >= 0 && frame != kAllOnes; }

// The VOOC-family identification commands: 0xA8 (captured, shared by VOOC / SVOOC / SVOOC65; reverse-engineered) and
// 0xAC / 0xA6 (VOOC; inferred, never captured). One ident proves the family, not the variant.
constexpr bool is_ident(std::uint8_t cmd) { return cmd == 0xA8 || cmd == 0xAC || cmd == 0xA6; }

// Frame start over D+ samples: an edge after D+ held one level for at least `gap` (in the caller's time unit) since
// listening began. A fall after a long high is pulse 1 of a frame in the idle protocol; a rise after a long low is pulse 1
// of the first ident after the wake (cap1 2026-11-11: D+ stays low 539 ms after the phone's burst, then the adapter raises
// D- and D+ 5 us apart). In-frame levels last 25-40 us low and 0.46 / 0.54 ms high, so they never start a frame.
struct FrameStart {
    std::uint32_t since = 0;
    bool level = false, have = false;
    bool feed(std::uint32_t t, bool clk, std::uint32_t gap = kFrameGapUs) {
        if (have && clk == level) return false;
        const bool start = have && t - since >= gap;
        have = true;
        level = clk;
        since = t;
        return start;
    }
};

// Hardware seam of the slave: the firmware implements it over D+ (PA7 or PB0) and D- (PB2 / PB1 drive),
// the host tests over a fake adapter clock.
//   wait_frame(t)  a FrameStart edge (a fall after D+ high, or a rise after D+ low, for kFrameGapUs): true on it,
//                  false after t us
//   wait_fall(t) / wait_rise(t)   next D+ edge, false after t us; wait_fall also latches dm_before_fall()
//   dm_before_fall()  D- level sampled the last time before the fall wait_fall returned on
//   dm()  D- now; delay_us(n); drive_dm(level) takes D- (phone side); release_dm() lets go of it
template <class Hw>
int rx_command(Hw& hw, std::uint32_t first_timeout_us) {
    if (!hw.wait_frame(first_timeout_us)) return -1;
    unsigned v = 0;
    for (int k = 1; k < kFrameBits; ++k) {
        if (!hw.wait_fall(1000)) return -1;
        v = v << 1 | (hw.dm_before_fall() ? 1u : 0u);
    }
    if (!hw.wait_rise(1000)) return -1;
    hw.delay_us(kLastBitDelayUs);
    v = v << 1 | (hw.dm() ? 1u : 0u);
    return static_cast<int>(v);
}

// Reply in the next frame's clock. D- goes high (the phone-side idle) right away, then each bit follows its
// pulse. False when no clock came within kReplyWaitUs or it stopped mid-frame; D- is released either way.
template <class Hw>
bool tx_reply(Hw& hw, std::uint16_t word) {
    hw.drive_dm(true);
    bool ok = hw.wait_frame(kReplyWaitUs);
    for (int i = kFrameBits - 1; i >= 0 && ok; --i) {
        if (i != kFrameBits - 1) ok = hw.wait_fall(1000);
        if (ok) ok = hw.wait_rise(1000);
        if (!ok) break;
        hw.delay_us(kReplyDelayUs);
        hw.drive_dm(((word >> i) & 1) != 0);
    }
    if (ok) hw.delay_us(kLastBitDelayUs + 100);   // the adapter latches the last bit after the last pulse
    hw.release_dm();
    return ok;
}

}  // namespace k2::vooc
