// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab "listen attach" (remote `lab listen-attach`): the K2 presents Rd on a charger's port with Auto-CRC off and CONTROL1 in
// the passive profile (SOP, SOP' and SOP'' received), for a bounded time, and never transmits or GoodCRCs anything. A source
// that gets no GoodCRC for its Source_Capabilities hard-resets and starts over; the OnePlus 120 W brick queries its own cable
// with SOP' Discover Identity and the vendor command 31 challenge on every such cycle, and the cable answers on the same CC
// wire. A normal sink (Auto-CRC on, SOP' off) never hears either. Every frame reaches the PD trace and the Black Box through
// the ordinary receive path.
//
// The window starts with a forced detach: a source that is in a contract keeps it and sends nothing when our Rd merely blinks
// (hardware 2026-11-18: VBUS stayed at 5.27 V, zero frames in 15 s). So Rd goes off, both CC lines open, until VBUS is below
// vSafe0V (0.8 V) or 1.5 s have passed, whichever comes first; only then Rd comes on and the window opens. A source that is
// already mid-negotiation can skip the detach (no_detach). The wait is a phase of this state machine, never a blocking loop.
//
// This is the pure part: the phases, the time window, the refusals and the end reason. The wire side is in
// src/drivers/fusb302_cold.cpp.
#pragma once

#include <cstdint>
#include "k2/core/strpack.hpp"

namespace k2::pd {

inline constexpr std::uint8_t kListenDefaultSecs = 10;
inline constexpr std::uint8_t kListenMaxSecs = 30;
inline constexpr std::int32_t kListenVSafe0Mv = 800;      // the source's VBUS counts as gone below this (vSafe0V)
inline constexpr std::uint32_t kListenDropMaxMs = 1500;   // the longest the detach waits for it

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

// Why the detach wait ended (None while it still runs).
enum class ListenDrop : std::uint8_t {
    None = 0,
    VSafe0 = 1,     // VBUS fell below vSafe0V
    Timeout = 2,    // 1.5 s passed with VBUS still up (or unknown)
    Skipped = 3,    // no_detach: the source is already mid-negotiation, Rd came on at once
};
constexpr const char* listen_drop_name(ListenDrop d) {
    switch (d) {
    case ListenDrop::None: return K2_P("waiting");
    case ListenDrop::VSafe0: return K2_P("vSafe0V reached");
    case ListenDrop::Timeout: return K2_P("timeout");
    case ListenDrop::Skipped: return K2_P("skipped");
    }
    return "?";
}

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
    case ListenRefusal::BadArg: return K2_P("bad seconds or port");
    case ListenRefusal::Busy: return K2_P("the link is busy");
    case ListenRefusal::Inline: return K2_P("a sink sits behind the K2");
    case ListenRefusal::Pinned: return K2_P("the K2 is powered through this port");
    case ListenRefusal::Spoof: return K2_P("the e-marker spoof is armed");
    case ListenRefusal::Running: return K2_P("already listening");
    }
    return "?";
}
constexpr const char* listen_end_name(ListenEnd e) {
    switch (e) {
    case ListenEnd::None: return "none";
    case ListenEnd::Done: return "done";
    case ListenEnd::Stopped: return K2_P("stopped");
    case ListenEnd::Lost: return K2_P("lost the port");
    }
    return "?";
}

class Listen {
public:
    // Only the refusal is decided here; the caller does the wire work when it is None. With `detach` the run begins in the
    // dropping phase (Rd stays off until settle() says so); without it the window opens at once.
    constexpr ListenRefusal start(std::uint32_t now_ms, std::uint8_t secs, std::uint8_t port, const ListenFacts& f,
                                  bool detach = true) {
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
        drop_started_ = now_ms;
        dropping_ = detach;
        drop_ = detach ? ListenDrop::None : ListenDrop::Skipped;
        return ListenRefusal::None;
    }
    // The dropping phase: true once, when it is over (VBUS below vSafe0V, or the time out; `vbus_mv` < 0 = unknown, which only
    // the time out can end). The window opens at that moment, so the caller puts Rd on now.
    constexpr bool settle(std::uint32_t now_ms, std::int32_t vbus_mv) {
        if (!running_ || !dropping_) return false;
        if (vbus_mv >= 0 && vbus_mv < kListenVSafe0Mv) drop_ = ListenDrop::VSafe0;
        else if (std::uint32_t(now_ms - drop_started_) >= kListenDropMaxMs) drop_ = ListenDrop::Timeout;
        else return false;
        dropping_ = false;
        started_ = now_ms;
        return true;
    }
    // true while the run goes on (dropping or listening); false once it ended (the caller then takes the Rd off).
    constexpr bool tick(std::uint32_t now_ms) {
        if (!running_) return false;
        if (dropping_) return true;
        if (std::uint32_t(now_ms - started_) >= std::uint32_t(secs_) * 1000u) finish(ListenEnd::Done);
        return running_;
    }
    constexpr void stop() { if (running_) finish(ListenEnd::Stopped); }
    constexpr void lost() { if (running_) finish(ListenEnd::Lost); }
    // One received frame (any SOP kind).
    constexpr void frame() { if (frames_ < 0xFFFFu) frames_ = std::uint16_t(frames_ + 1); }

    constexpr bool running() const { return running_; }
    constexpr bool dropping() const { return running_ && dropping_; }
    constexpr ListenDrop drop() const { return drop_; }
    // How long the detach wait took (ms); only meaningful once it ended.
    constexpr std::uint32_t dropped_ms() const { return std::uint32_t(started_ - drop_started_); }
    // The remote status: 0 idle, 1 listening, 2 dropping Rd.
    constexpr std::uint8_t state() const { return !running_ ? 0 : dropping_ ? 2 : 1; }
    constexpr ListenEnd end() const { return end_; }
    constexpr std::uint8_t secs() const { return secs_; }
    constexpr std::uint8_t port() const { return port_; }
    constexpr std::uint16_t frames() const { return frames_; }
    // Whole seconds left in the window (rounded up), 0 when it is not running.
    constexpr std::uint8_t left_s(std::uint32_t now_ms) const {
        if (!running_) return 0;
        if (dropping_) return secs_;
        const std::uint32_t total = std::uint32_t(secs_) * 1000u, gone = std::uint32_t(now_ms - started_);
        return gone >= total ? 0 : std::uint8_t((total - gone + 999u) / 1000u);
    }

private:
    constexpr void finish(ListenEnd e) { running_ = false; end_ = e; }
    bool running_ = false, dropping_ = false;
    ListenDrop drop_ = ListenDrop::None;
    std::uint32_t drop_started_ = 0;
    ListenEnd end_ = ListenEnd::None;
    std::uint8_t secs_ = 0, port_ = 0;
    std::uint16_t frames_ = 0;
    std::uint32_t started_ = 0;
};

}  // namespace k2::pd
