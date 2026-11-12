// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Firmware side of the Black Box push mode over HID (k2/core/trace_hid.hpp, remote command 0x73): the one Session over the
// live ring, the HID quiet switch of the USB driver, and the per-pass service that queues reports on the HID IN endpoint.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::trace_hid_hw {

// Handles one validated Cmd::TraceHid request (remote_proto.hpp lays it out); returns the response payload size (13).
std::size_t command(std::span<const std::uint8_t> req, std::span<std::uint8_t> out);

// One main-loop pass: queues the next reports while the IN queue has room (a few slots stay free for command responses),
// and ends a session whose host stopped reading. Does nothing while the mode is off.
void service();

bool active();

}  // namespace k2::trace_hid_hw
