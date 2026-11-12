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
    // Longest ms between poll() calls since the last log_stats() (one 2 s heartbeat window, zeroed there on purpose: the
    // Diag row, GET_STATE part 2 and tools/loop_survey.py read it as "the recent worst gap", not a peak since boot,
    // which the boot-time stalls would pin for good).
    volatile std::uint32_t poll_gap_max;
};
extern Stats stats;

// Plain copy of the counters, for readers outside the driver (the Diag page). One call, one moment.
struct StatsSnapshot {
    std::uint32_t resets, isrs, log_sent, log_dropped, poll_gap_max;
};
StatsSnapshot stats_snapshot();

// First thing at boot (before the clock tree and every slow init step): drive D+/D- low (SE0) until init() connects.
// The HID port's D+ reads high from power-up whatever the USB core does: with PA12 an input with its weak pull-down, and
// the core in reset with its own pull-up off, the line still reads high and the host's 50 ms port resets show on it
// (2026-11-12). Left undriven through the ~1.5 s of boot, the host sees an attached device that never answers: its
// 64-byte descriptor reads fail with -71 and it power-cycles the port before the core is up.
void hold_detached();
// The USB link stays off this boot (no init()): give PA11/PA12 back in the mode hold_detached() found them in.
void release_detached();

// Detach for 150 ms, start the 48 MHz USB clock, enable the core and connect. Call once.
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
// A host configured the device and keeps the bus awake (not suspended). There is no VBUS sensing: after a PC is unplugged the
// device stays "configured" but suspended, so this is the live "a PC is on the HID port" signal (k2/core/loop_guard.hpp).
// False while the USB link is off (the core never started).
bool host_active();

// The HID port's data lines as this side sees them (read-only; the loop probe, k2/loopprobe/loop_probe_hw.hpp). `core`:
// the USB core runs (the link is on); rx_dp / rx_dm and frame (the counter of the last SOF, 11 bits) come from the core
// and are false / 0 without it. pin_dp / pin_dm are the PA12 / PA11 input levels, valid either way.
struct Lines {
    bool core = false, configured = false;
    bool rx_dp = false, rx_dm = false;
    bool pin_dp = false, pin_dm = false;
    std::uint16_t frame = 0;
};
Lines lines();

// The HID port's D+ pull-up, switched for the loop probe's reverse-direction check. With the core running it is the
// core's 1.5k pull-up (the PHY stays powered, so the receivers keep reading); with the link off it is PA12's weak input
// pull-up. The first call remembers what was there and dp_pullup_restore() puts it back (a no-op without a call).
enum class Pull : std::uint8_t { None, Core, Gpio };
Pull dp_pullup(bool on);
void dp_pullup_restore();

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

// HID quiet: while on, log()/log_fmt() text is not queued on the HID IN endpoint (the log ring, the tap and the CDC
// shell still get it), so a push session (k2/core/trace_hid.hpp) owns the 1 kHz pipe. Separate from log_enable(), which
// is the host's own switch and survives the session. send_report() is never gated.
void set_hid_quiet(bool on);
bool hid_quiet();

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
