// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab "listen attach" (remote `lab listen-attach`): the K2 presents Rd on a charger's port with Auto-CRC off and CONTROL1 in
// the passive profile (SOP, SOP' and SOP'' received), for a bounded time, and never transmits or GoodCRCs anything. A source
// that gets no GoodCRC for its Source_Capabilities hard-resets and starts over; the OnePlus 120 W brick queries its own cable
// with SOP' Discover Identity and the vendor command 31 challenge on every such cycle, and the cable answers on the same CC
// wire. A normal sink (Auto-CRC on, SOP' off) never hears either. Every frame reaches the PD trace and the Black Box through
// the ordinary receive path.
//
// This is the pure part: the time window, the refusals and the end reason. The wire side is in src/drivers/fusb302_cold.cpp.
#pragma once

#include <cstdint>

namespace k2::pd {

inline constexpr std::uint8_t kListenDefaultSecs = 10;
inline constexpr std::uint8_t kListenMaxSecs = 30;

// Which port the charger sits on. Only a label: the top plug and the bottom receptacle share their CC nets (K2_PORTS_SPEC.md),
// so one Rd listens on both; the log line and the status carry what the operator said.
inline constexpr std::uint8_t kListenPortAuto = 0, kListenPortTop = 1, kListenPortBottom = 2;

enum class ListenRefusal : std::uint8_t {
    None = 0,
    BadArg = 1,     // seconds above the maximum, or an unknown port label
    Busy = 2,       // a charger test, PD Stress, PD Spec, a twin run, a trigger or an identify / probe owns the link
    Inline = 3,     // a sink sits behind the K2: Rd off and silence would cut it off
    Pinned = 4,     // the K2 runs off this port's VBUS (boot-pinned Rd or self-powered): dropping Rd kills it
    Spoof = 5,      // the e-marker spoof is armed and its Auto-CRC profile would answer the cable
    Running = 6,    // a listen is already running
};

enum class ListenEnd : std::uint8_t {
    None = 0,       // never started, or still running
    Done = 1,       // the window ran out
    Stopped = 2,    // the operator stopped it
    Lost = 3,       // another owner took the unit's Rd (a charger-test START, an attach) before the window ran out
};

struct ListenFacts {
    bool ports_busy = false;
    bool inline_mode = false;
    bool rd_pinned = false;
    bool spoof_armed = false;
};

// 0 = the default window.
constexpr std::uint8_t listen_secs(std::uint8_t requested) { return requested == 0 ? kListenDefaultSecs : requested; }

constexpr const char* listen_refusal_name(ListenRefusal r) {
    switch (r) {
    case ListenRefusal::None: return "ok";
    case ListenRefusal::BadArg: return "bad seconds or port";
    case ListenRefusal::Busy: return "the link is busy";
    case ListenRefusal::Inline: return "a sink sits behind the K2";
    case ListenRefusal::Pinned: return "the K2 is powered through this port";
    case ListenRefusal::Spoof: return "the e-marker spoof is armed";
    case ListenRefusal::Running: return "already listening";
    }
    return "?";
}
constexpr const char* listen_end_name(ListenEnd e) {
    switch (e) {
    case ListenEnd::None: return "none";
    case ListenEnd::Done: return "done";
    case ListenEnd::Stopped: return "stopped";
    case ListenEnd::Lost: return "lost the port";
    }
    return "?";
}

class Listen {
public:
    // Only the refusal is decided here; the caller does the wire work when it is None.
    constexpr ListenRefusal start(std::uint32_t now_ms, std::uint8_t secs, std::uint8_t port, const ListenFacts& f) {
        const std::uint8_t s = listen_secs(secs);
        if (s > kListenMaxSecs || port > kListenPortBottom) return ListenRefusal::BadArg;
        if (running_) return ListenRefusal::Running;
        if (f.ports_busy) return ListenRefusal::Busy;
        if (f.inline_mode) return ListenRefusal::Inline;
        if (f.rd_pinned) return ListenRefusal::Pinned;
        if (f.spoof_armed) return ListenRefusal::Spoof;
        running_ = true;
        end_ = ListenEnd::None;
        secs_ = s;
        port_ = port;
        frames_ = 0;
        started_ = now_ms;
        return ListenRefusal::None;
    }
    // true while the window is open; false once it ended (the caller then takes the Rd off).
    constexpr bool tick(std::uint32_t now_ms) {
        if (!running_) return false;
        if (std::uint32_t(now_ms - started_) >= std::uint32_t(secs_) * 1000u) finish(ListenEnd::Done);
        return running_;
    }
    constexpr void stop() { if (running_) finish(ListenEnd::Stopped); }
    constexpr void lost() { if (running_) finish(ListenEnd::Lost); }
    // One received frame (any SOP kind).
    constexpr void frame() { if (frames_ < 0xFFFFu) frames_ = std::uint16_t(frames_ + 1); }

    constexpr bool running() const { return running_; }
    constexpr ListenEnd end() const { return end_; }
    constexpr std::uint8_t secs() const { return secs_; }
    constexpr std::uint8_t port() const { return port_; }
    constexpr std::uint16_t frames() const { return frames_; }
    // Whole seconds left in the window (rounded up), 0 when it is not running.
    constexpr std::uint8_t left_s(std::uint32_t now_ms) const {
        if (!running_) return 0;
        const std::uint32_t total = std::uint32_t(secs_) * 1000u, gone = std::uint32_t(now_ms - started_);
        return gone >= total ? 0 : std::uint8_t((total - gone + 999u) / 1000u);
    }

private:
    constexpr void finish(ListenEnd e) { running_ = false; end_ = e; }
    bool running_ = false;
    ListenEnd end_ = ListenEnd::None;
    std::uint8_t secs_ = 0, port_ = 0;
    std::uint16_t frames_ = 0;
    std::uint32_t started_ = 0;
};

}  // namespace k2::pd
