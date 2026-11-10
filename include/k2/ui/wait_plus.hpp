// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger test's Waiting stage, pure parts. The PLUS key is wired to the CC pins on this board: holding it for a few
// seconds wakes a charger that sleeps with no sink on the line, so the firmware only watches it, it never reads the key as
// navigation there. Two pieces live here so the host tests cover them:
//   footer()    the footer line of the stage (which hint, which tone), cut to the 27-cell footer;
//   PlusWatch   paces the [WAKE] log lines (shell-observable CC1 / CC2 / VBUS while the key is held and for 2 s after).
#pragma once

#include <cstdint>

namespace k2::waitplus {

enum class Tone : std::uint8_t { Ok, Info, Caution };   // the footer tones this stage uses (mapped to the kit's by the caller)

struct Line {
    const char *text;
    Tone tone;
};

// A CC line with nothing pulling it to a source's Rp ladder: ground-ish or above the highest Rp level (-1 = unknown, not open).
constexpr bool cc_open(int mv) { return mv >= 0 && (mv < 200 || mv >= 2200); }

// The footer of Waiting, highest priority first. `waited_ms` = time since START's wait began; a negative reading = unknown.
// "Hold + few secs" only where no VBUS is up (the key helps nothing once the charger has woken); the lines for a charger
// that is already up are the older ones.
constexpr Line footer(bool ready, int cc1_mv, int cc2_mv, std::int32_t vbus_mv, std::uint32_t waited_ms) {
    if (ready) return {"Charger up, starting", Tone::Ok};
    if (vbus_mv < 500) {
        if (cc_open(cc1_mv) && cc_open(cc2_mv)) return {"No charger. Hold + few secs", Tone::Caution};
        return {"No VBUS. Hold + few secs", Tone::Info};   // an Rp is on a line (or the reading is unknown): asleep
    }
    if (vbus_mv < 4500) return {"Standby. Wake the charger", Tone::Info};
    if (waited_ms >= 3000) return {"VBUS up, no PD caps yet", Tone::Info};
    return {"Charger to the top port", Tone::Info};   // up under 3 s: caps may still come
}

// Paces the [WAKE] lines. step() is called once per tick of the session; it says what to log this tick. The window is the
// physical key down (Press, then a Sample every kSampleMs) and kTailMs after it goes up (Release, then Samples), and only
// while the stage is Waiting. A key that is never touched logs nothing; a second press inside the tail starts a new window.
class PlusWatch {
public:
    enum class Act : std::uint8_t { None, Press, Sample, Release };
    static constexpr std::uint32_t kSampleMs = 100;   // ~10 Hz
    static constexpr std::uint32_t kTailMs = 2000;    // after the release

    Act step(std::uint32_t now, bool waiting, bool down) {
        if (!waiting) { active_ = down_ = false; return Act::None; }   // leaving the stage ends the window, silently
        if (down && !down_) {
            down_ = active_ = true;
            down_at_ = last_ = now;
            return Act::Press;
        }
        if (!down && down_) {
            down_ = false;
            until_ = now + kTailMs;
            last_ = now;
            return Act::Release;
        }
        if (!active_) return Act::None;
        if (!down_ && static_cast<std::int32_t>(now - until_) >= 0) { active_ = false; return Act::None; }
        if (now - last_ < kSampleMs) return Act::None;
        last_ = now;
        return Act::Sample;
    }
    bool down() const { return down_; }
    std::uint32_t held_ms(std::uint32_t now) const { return now - down_at_; }   // valid from the Press on; the Release line reads it
    std::uint32_t t0() const { return down_at_; }

private:
    bool active_ = false, down_ = false;
    std::uint32_t down_at_ = 0, last_ = 0, until_ = 0;
};

}  // namespace k2::waitplus
