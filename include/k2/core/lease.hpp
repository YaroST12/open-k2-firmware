// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The host dead-man lease: pure timing and clear rules, no hardware. A host command that can leave the output above 5 V
// under the host's control (`pd request` / `pd attach` above 5 V, `fc trigger` above 5 V, `fc scan`) ARMS the lease; any
// later request from that host (any transport: HID or the HTTP bridge) REFRESHES it. When the host goes quiet for the lease
// length, or the USB link drops, the firmware runs the same `stop` path as the remote `stop` command (Remote::stop).
//
// What clears the lease without a stop: the host's own stop / pd release / pd reset / fc reset (Remote calls clear()), a
// charger test starting (a test is not host-owned voltage: it owns the ports itself, and a stop must never cancel it), and
// a verified 5 V: nothing holds or negotiates above 5 V and VBUS reads 5 V for kSafeHoldMs without a break (an attach that
// is still waiting for the source's capabilities does not count: tick() is told `safe_5v` only from an idle sink at 5 V, and
// the run restarts on any sample that is not).
//
// The wire (Cmd::Lease): `[0]` status, `[1, seconds u16]` set the length. The length applies to the NEXT arm and only
// within kPendingMs of the set (a refused command must not leave a long lease for an unrelated arm later); while armed it
// changes the running lease at once. Seconds 0 = the default (30 s); above kMaxSeconds the dispatcher answers BadArg.
//
// All times are the 32-bit millisecond clock; every comparison is a modular difference, so the clock wrap is harmless.
#pragma once

#include <cstdint>

namespace k2::lease {

inline constexpr std::uint32_t kDefaultMs = 30000;
inline constexpr std::uint16_t kDefaultSeconds = 30;
inline constexpr std::uint16_t kMaxSeconds = 600;
inline constexpr std::uint32_t kPendingMs = 5000;    // a set-length applies to an arm at most this long afterwards
inline constexpr std::uint32_t kSafeHoldMs = 5000;   // an unbroken run of verified 5 V before the lease clears itself

// Why the last lease ended (wire byte of the status reply).
enum class End : std::uint8_t { None = 0, Expired = 1, HostGone = 2, Released = 3, Settled = 4, TestRunning = 5 };
// What tick() asks the caller to do: stop everything (Expired: the host went quiet; HostGone: the USB link dropped).
enum class Event : std::uint8_t { None, Expired, HostGone };

struct Info {
    bool armed = false;
    std::uint16_t left_s = 0;      // seconds until expiry, rounded up (never 0 while armed)
    std::uint16_t length_s = 0;    // the length of the running lease (armed) or of the next one (pending / default)
    End ended = End::None;
};

class Lease {
public:
    // A host command that can hold the output above 5 V was accepted.
    void arm(std::uint32_t now) {
        if (pending_valid(now)) len_ms_ = pending_ms_;
        else if (!armed_) len_ms_ = kDefaultMs;
        pending_set_ = false;
        armed_ = true;
        last_ = now;
        safe_ = false;
    }
    // Any request from the host. Refreshes a running lease; arms nothing.
    void touch(std::uint32_t now) { if (armed_) last_ = now; }
    // The length for the next arm (not armed), or for the running lease (armed). 0 = the default. Counts as a request.
    void set_length(std::uint32_t now, std::uint16_t seconds) {
        if (seconds > kMaxSeconds) seconds = kMaxSeconds;
        const std::uint32_t ms = seconds == 0 ? kDefaultMs : std::uint32_t{seconds} * 1000u;
        if (armed_) { len_ms_ = ms; last_ = now; return; }
        pending_ms_ = ms;
        pending_at_ = now;
        pending_set_ = true;
    }
    // The host (or the operator) released the output itself: stop, pd release, pd reset, fc reset.
    void clear(End why = End::Released) {
        if (armed_) { armed_ = false; ended_ = why; }
        pending_set_ = false;
    }
    // Once per main-loop pass. host_present: the USB link is configured. safe_5v: an idle sink, no FC, VBUS at 5 V.
    // test_running: a charger test owns the ports.
    Event tick(std::uint32_t now, bool host_present, bool safe_5v, bool test_running) {
        if (!armed_) return Event::None;
        // A running charger test owns the ports: the lease ends without a stop, even if the cable was pulled meanwhile.
        if (test_running) { armed_ = false; ended_ = End::TestRunning; return Event::None; }
        if (!host_present) { armed_ = false; ended_ = End::HostGone; return Event::HostGone; }
        if (safe_5v) {
            if (!safe_) { safe_ = true; safe_since_ = now; }
            else if (now - safe_since_ >= kSafeHoldMs) { armed_ = false; ended_ = End::Settled; return Event::None; }
        } else {
            safe_ = false;
        }
        if (now - last_ >= len_ms_) { armed_ = false; ended_ = End::Expired; return Event::Expired; }
        return Event::None;
    }
    Info info(std::uint32_t now) const {
        Info i;
        i.armed = armed_;
        i.ended = ended_;
        if (armed_) {
            const std::uint32_t left = len_ms_ - (now - last_);
            i.left_s = static_cast<std::uint16_t>((left + 999u) / 1000u);
            i.length_s = static_cast<std::uint16_t>(len_ms_ / 1000u);
        } else {
            i.length_s = static_cast<std::uint16_t>((pending_valid(now) ? pending_ms_ : kDefaultMs) / 1000u);
        }
        return i;
    }

private:
    bool pending_valid(std::uint32_t now) const { return pending_set_ && now - pending_at_ < kPendingMs; }

    bool armed_ = false;
    bool safe_ = false;
    bool pending_set_ = false;
    End ended_ = End::None;
    std::uint32_t len_ms_ = kDefaultMs;
    std::uint32_t last_ = 0;
    std::uint32_t safe_since_ = 0;
    std::uint32_t pending_ms_ = kDefaultMs;
    std::uint32_t pending_at_ = 0;
};

}  // namespace k2::lease
