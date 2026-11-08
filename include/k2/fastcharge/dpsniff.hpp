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

#include "k2/fastcharge/edges.hpp"

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

}  // namespace k2::dpsniff
