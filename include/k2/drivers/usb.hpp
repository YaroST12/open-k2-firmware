// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB device glue: TinyUSB (fsdev driver) bring-up and the HID log channel.
//
// Everything runs from the main loop: the ISR only feeds TinyUSB's event queue and
// poll() executes the callbacks, so the log queue is single-context and needs no locks.
// Debug output goes through log()/log_fmt(): lines are queued as 64-byte HID IN reports
// (the VID 0x0716 / PID 0x5060 logger protocol); a full queue drops the oldest line.
#pragma once
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace k2 {
template <std::size_t N>
class BinLog;   // k2/core/binlog.hpp
}

namespace k2::usb {

// Diagnostics shown on the diagnostics page and in the [USB] line.
struct Stats {
    volatile std::uint32_t resets;        // bus resets seen by the ISR
    volatile std::uint32_t isrs;          // USB interrupt entries
    volatile std::uint32_t log_sent;      // log reports handed to the HID IN endpoint
    volatile std::uint32_t log_dropped;   // log lines (or, last resort, frames) dropped by a full queue
    volatile std::uint32_t poll_gap_max;  // longest ms between poll() calls since the last log_stats()
};
extern Stats stats;

// Plain copy of the counters, for readers outside the driver (the Diag page). One call, one moment.
struct StatsSnapshot {
    std::uint32_t resets, isrs, log_sent, log_dropped, poll_gap_max;
};
StatsSnapshot stats_snapshot();

// Detach for 20 ms, start the 48 MHz USB clock, enable the core and connect. Call once.
void init();

// Leave the bus for good before a reset: stop the stack, disable the USB interrupts and core clock,
// drive D+/D- low (SE0) and hold it for `hold_ms` so the host registers a disconnect. After this
// the driver is dead (poll()/service() do nothing) until the next reset. Queued log reports
// that were not flushed first are lost.
void detach(std::uint32_t hold_ms = 300);

// Run the USB stack and push queued log lines. Call at least every few ms.
void poll();

// poll() plus a manual call of the USB interrupt handler, for code that runs with interrupts
// unusable (the fault handler): lets queued log reports actually leave the device.
void pump();

// poll() from inside a busy-wait (systick::delay_ms, LCD streaming); ignored before
// init() and when nested.
void service();

// True once the host has configured the device.
bool configured();

// Queue one log line (truncated to 63 bytes) / a printf-style line (see k2/core/fmt.hpp).
void log(std::string_view msg);
// One observer of every log line (the debug shell's `log on`); nullptr removes it. The callback
// must not call log().
void set_log_tap(void (*fn)(std::string_view));
void log_fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void vlog_fmt(const char* fmt, std::va_list ap);

// Copy the most recent log text (about 2 KB, whole lines, oldest first) into `out`; returns the
// length. Fed from log()/log_fmt() even while log_enable(false); always on.
std::size_t log_text(std::span<char> out);
// The newest whole binary K2LOG entries that fit `out` (k2/core/binlog.hpp), oldest first; returns the length. Fed by every K2LOG
// call (not by plain log()/log_fmt() text); `base_ms` is the uptime of the entry before the first one, `older` is set when
// older entries were left out.
std::size_t log_bin(std::span<std::uint8_t> out, std::uint32_t& base_ms, bool& older);
// The binary ring itself, read-only: the live debug stream (k2/core/dbg_stream.hpp) follows it with an absolute cursor.
inline constexpr std::size_t kLogBinBytes = 1024;
const BinLog<kLogBinBytes>& log_bin_ring();
// Borrowed recent log text, oldest first; invalidated by the next log(). No USB servicing
// or logging may run between taking the view and consuming it.
std::array<std::string_view, 2> log_view();

// Log text on/off. Off drops log()/log_fmt() output; send_report() is unaffected, so a remote
// client can silence the text and keep its request/response channel.
void log_enable(bool on);
bool log_enabled();

// Queue one 64-byte non-log report (a remote-control response) on the HID IN endpoint, in
// order with the log lines. Never gated by log_enable().
void send_report(const std::uint8_t* report);

// Free slots in the IN queue. send_report() overwrites the oldest entry when it is full, so a
// burst sender (screenshot) waits for room with poll() first.
std::uint8_t tx_free();

// Implemented by src/core/remote.cpp: receives each 64-byte HID OUT report (EP1 OUT / SET_REPORT).
// Must only queue the data; it runs from tud_task() inside poll(), never from the ISR.
void on_remote_report(const std::uint8_t* buffer, std::uint16_t length);

// Send queued lines while the HID IN endpoint is ready (also done by poll() and log()).
void flush();

// Emit "[USB] gap=..ms isr=.. rst=.. sent=.. drop=.." and restart the gap maximum.
void log_stats();

}  // namespace k2::usb
