// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Firmware side of the multiplexed live debug stream (k2/core/dbg_stream.hpp, docs/DEBUG_STREAMING.md): the one Mux instance,
// the probe that feeds it the clock, the power/CC readings and the boot header, and the start/stop/service entry points the
// CDC shell calls. It writes nothing itself; the shell hands service() a function that puts a whole frame into the CDC FIFO.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/dbg_stream.hpp"

namespace k2 {
class Remote;
class Ui;
}

namespace k2::dbg_hw {

// The Remote supplies the boot header (the K2B1 line), the Ui the page number for the heartbeat. Call once.
void init(Remote& remote, Ui& ui);

// Begin streaming; renders the boot header first. `hz` is the power sample rate (0 = default).
void start(std::uint8_t mask, std::uint16_t hz, bool history);
// End the stream; returns its counters.
dbg::Stats stop();
bool running();
std::uint8_t stream_mask();   // the kinds of the running (or last) stream
dbg::Stats stats();

// One main-loop pass. `room` is the free space in the CDC TX FIFO; `write` takes one whole frame.
unsigned service(std::size_t room, void (*write)(std::span<const std::uint8_t>));

}  // namespace k2::dbg_hw
