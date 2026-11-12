// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Passive D+/D- edge sniffer for the Black Box. While armed, every edge on D+ (PA7, EXTI7) and D-
// (PB2, EXTI2) is timestamped with the DWT cycle counter into a 16 KB buffer leased from the
// Net/Drive overlay RAM (k2/drivers/mode_ram.hpp, only free in HID-only mode). When the capture ends (buffer
// full, 500 ms without an edge, VBUS gone, 6 s from the first edge, cancel) the edges are stored
// as a Black Box capture of type "D+/D- edges" (pdtrace::Trigger::Edges, k2/fastcharge/edges.hpp) next to the
// PD messages and V/I samples of the window before the capture, and the usual capture list,
// littlefs persistence, remote export and WebHID view show it.
//
// The sniffer NEVER drives: arming switches D+ to a floating input (PA7) and D- to two floating
// inputs (PB1, PB2) and leaves them so when it ends. It refuses to arm while a charger test or
// trigger owns the ports, and the fast-charge engine and the PA8 probe refuse to start while armed.
// Which of PB1 / PB2 sees the phone's D- on the real switch network is unverified.
#pragma once
#include <cstdint>
#include <span>

#include "k2/fastcharge/edge_guard.hpp"
#include "k2/fastcharge/edges.hpp"
#include "k2/fastcharge/vooc_decode.hpp"

// Live stream (shell `sniff stream`): the same passive arming, but every edge goes through a ring to the
// CDC port as framed records (k2/fastcharge/edges.hpp) until stopped, so the 4096-edge buffer and the 2 KB slot do
// not limit the capture. Overruns (edges dropped because the host or the FIFO fell behind) are counted.
namespace k2::dpsniff {

enum class State : std::uint8_t { Idle = 0, Armed = 1, Recording = 2 };
enum class ArmResult : std::uint8_t { Ok = 0, NoRam, Busy };

struct Status {
    State state = State::Idle;
    edges::End reason = edges::End::None;   // how the last session ended
    std::uint16_t edges = 0;                // edges recorded (current or last session)
    bool overflow = false;
    bool stored = false;                    // the last session produced a Black Box capture
    std::uint32_t total = 0;                // edges seen incl. those dropped after the buffer filled
};

constexpr std::size_t kCapacity = 4096;     // edge words in the buffer

ArmResult arm();
ArmResult arm_stream();                          // passive stream; stays armed until cancel()
ArmResult arm_synth(std::uint32_t edges_per_s);  // NO pins: a synthetic source in the main loop, to measure the stream path
bool streaming();
struct StreamStats { std::uint32_t edges = 0, overruns = 0, bytes = 0; };
StreamStats stream_stats();
// Main loop (shell glue): feeds the synthetic source, then writes whole frames into `out` while `room`
// bytes are free; returns the bytes produced (0 = nothing pending).
std::size_t stream_frame(std::span<std::uint8_t> out);
void stream_service();
void cancel();
// Main loop: ends a session and writes the capture. A VBUS below 3 V counts as a detach only after
// edges started and VBUS was seen at 4.5 V or more since arming (so arm-then-plug-in works).
void poll(std::uint32_t now_ms, std::int32_t vbus_mv);
bool armed();
Status status();
// Test aid, no phone attached: toggles D+ a few times through dpdm so the EXTI path is proven. Only
// while armed; the caller must have checked that nothing is attached.
bool selftest();
// The whole buffer of the last session (up to kCapacity edges, the FIRST ones; the Black Box slot holds
// only what fits 2 KB). Writes one Edges record (k2/fastcharge/edges.hpp) starting at edge `from` into `out` and
// returns its length, 0 when nothing is left or a session is running. `total` = edges kept.
std::size_t read_record(std::size_t from, std::span<std::uint8_t> out, std::size_t& used, std::size_t& total);

// Inline VOOC monitor (k2/fastcharge/vooc_decode.hpp). While a far sink draws through the K2 (a PD contract on CC may
// run at the same time) and nothing else owns D+/D- (vooc::monitor_wanted), the lines are the Sniff floating inputs and the EXTI ISR feeds every edge to the frame
// decoder, which keeps the live summary; no edge buffer, no mode RAM. It never drives: arming only sets the Sniff state,
// ending puts back the passive idle (dpdm::idle(), both lines released) when the lines are still free, and leaves them alone when an engine or a
// Black Box arm has them. A Black Box arm or stream running at the same time feeds the decoder too.
// fastcharge::poll() calls monitor_poll() every ~20 ms; `wanted` from vooc::monitor_wanted, `lines_free` = no engine,
// scan, probe or reset hold owns the pins. Logs one `[VOOC] session N start` / `end` line per session.
//
// Edge-storm guard (k2/fastcharge/edge_guard.hpp), for every user of the ISR: a USB data link through the K2 toggles
// the lines at MHz rates. When the ISR counts more than 32 interrupts in 1 ms (or 4,096 in 1.1 s) it masks its two
// EXTI lines at once. The main loop then ends a Black Box arm or stream (End::Storm, `[SNIFF] stopped: data traffic`)
// and pauses the monitor (`[VOOC] monitor paused: data traffic`): the lines stay floating, nothing is decoded, and the
// lines are unmasked again for a probe after 1 s, 5 s, then every 30 s. monitoring() stays true while paused (the
// monitor still owns the floating lines).
void monitor_poll(bool wanted, bool lines_free);
bool monitoring();
void monitor_enable(bool on);     // the operator's switch (RAM only, on at boot)
bool monitor_enabled();
struct MonitorInfo {
    bool enabled = false, running = false;
    vooc::Summary s{};
    edges::GuardInfo guard{};
};
MonitorInfo monitor_info();       // a copy taken with interrupts masked
vooc::Live monitor_live();        // the screen's view (vooc::live_of), interrupts masked for a few reads

// Edge hook of the guarded SuperVOOC trigger (k2/fastcharge/svooc_trig.hpp), the one user of this ISR that DRIVES: the
// ISR hands it every edge word (after the storm guard, before the recorder) and the hook may set D- through dpdm. The
// trigger owns the hook from hook_claim() to hook_end(); meanwhile arm(), arm_stream() and arm_synth() answer Busy and
// the EXTI lines stay on when a recording or the monitor ends.
//   hook_claim()  the mode RAM for the session's recording: NoRam while the USB drive or net function holds it (the
//                 real reason, reported as such), Busy while a Black Box arm, a stream or the monitor runs. Lines untouched.
//   hook_listen() both lines to the Sniff floating inputs, the hook live, EXTI on; the first call per claim also arms
//                 the recorder, so the session's edges (the adapter's and our own D- drive) become a Black Box capture
//                 of type "D+/D- edges" (it ends on its own: full, 500 ms quiet, 6 s from the first edge).
//   hook_mute()   the ISR stops calling the hook and EXTI goes off (the wake burst drives D+ itself).
//   hook_end()    hook off, the recording ends and is stored, EXTI off. The lines are the caller's to put back.
//   hook_storm()  the edge-storm guard tripped since the last hook_listen() (the ISR masked the lines).
//   line_word()   the current levels as an edge word (edges::pack of now, D-, D+).
using Hook = void (*)(std::uint32_t word);
ArmResult hook_claim();
void hook_listen(Hook h);
void hook_mute();
void hook_end();
bool hooked();
bool hook_storm();
std::uint32_t line_word();

}  // namespace k2::dpsniff
