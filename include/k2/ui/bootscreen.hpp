// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The boot-info screen: in a key-mode boot (PLUS or OK held at reset) the LCD shows the K2B1 text (k2/core/bootinfo.hpp) as a QR
// code with three readable lines under it, so the self-powered boot decision can be read from a photo or a phone scan on a
// K2 that has no USB host and no log.
//
//   - the code is a k2/ui/qrshare.hpp frame (type BootInfo, record = the K2B1 text), alphanumeric mode, ECC level L, version <= 10,
//     laid out by k2/ui/qrview.hpp (black on white, at least 3 px per module, 4-module quiet zone, centred), the readable lines
//     below the box;
//   - redrawn every kRefreshMs while the key is held (the payload carries the uptime and the live CC/VBUS readings), the last
//     frame stays kHoldMs after the key is released, then the Controller says Exit and the caller repaints the normal UI;
//   - drawn from the normal main loop (never from the boot path), so the boot timing and the SWITCHES0 write are untouched.
//
// Pure over the Snapshot: plan(), format_lines() and Controller are host-tested without a display; draw() goes through the lcd
// stream API and the scratch arena (the K2B1 text, the frame text, the QR workspace and one pixel line, about 2.3 KB, leased
// for one draw), so the host test renders it onto the fake panel and reads the code back with an independent decoder.
#pragma once
#include <cstdint>

#include "k2/core/bootinfo.hpp"
#include "k2/ui/qrview.hpp"

namespace k2::bootscreen {

inline constexpr int kPanelPx = qrview::kPanelPx;
inline constexpr int kMinModulePx = qrview::kMinModulePx;
inline constexpr int kQuietModules = qrview::kQuietModules;
inline constexpr int kTextH = 40;               // under the box: three 12 px lines and their gaps
inline constexpr int kMaxVersion = 10;          // the three lines leave room for version 10 at 3 px per module
inline constexpr std::uint32_t kRefreshMs = 500;
inline constexpr std::uint32_t kHoldMs = 30000;

using Plan = qrview::Plan;
inline Plan plan(int n) { return qrview::plan(n, kTextH); }

// The three readable lines (each at most 40 characters, the panel's width in the 6 px font), NUL terminated.
struct Lines {
    char id[48];      // "fw 1.0 fcb3128-dirty p1": the same strings PING answers
    char up[48];      // "up 12.3 s  frame 7"
    char mode[48];    // "plus  Rd CC2>CC1 at 650 ms" / "ok  Rd CC1, no move"
};
void format_lines(const bootinfo::Snapshot& s, std::uint32_t frame, Lines& out);

// One frame onto the panel. Frame 1 (or a symbol whose size changed) clears the panel to white first; the others repaint the
// box and the lines in place, so the code does not flash.
void draw(const bootinfo::Snapshot& s, std::uint32_t frame);

// When to draw and when to stop. Time and the key state go in, an Action comes out; no display, no clock of its own.
class Controller {
public:
    enum class Action : std::uint8_t { None, Redraw, Exit };

    void start(std::uint32_t now_ms);
    // Call every main-loop pass while active(). `key_down`: the key that selected the boot mode is down right now.
    Action tick(std::uint32_t now_ms, bool key_down);
    void dismiss() { active_ = false; }
    bool active() const { return active_; }
    std::uint32_t frame() const { return frame_; }   // frames drawn so far (1 after the first Redraw)

private:
    bool active_ = false;
    bool first_ = false;
    bool released_ = false;
    std::uint32_t last_ms_ = 0;       // the last Redraw
    std::uint32_t released_ms_ = 0;   // when the key was first seen up
    std::uint32_t frame_ = 0;
};

}  // namespace k2::bootscreen
