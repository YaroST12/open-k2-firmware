// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Debug shell glue: connects the CDC-ACM port (USB link modes HID and HID+Drive) to the line parser
// in k2/core/shell.hpp and the existing engines. Commands reuse Remote::fc_cmd, Remote::pd_hard_reset,
// fusb302 and dpdm; nothing here re-implements their safety checks.
//
// Session rule: the shell is a session while the host holds the port open (DTR). When that ends, or
// the USB link drops (unplug, suspend), the shell returns the board to 5 V and passive idle: a trigger
// it started is reset, `pd silent` ends, a raw D+/D- drive goes to HiZ and the lines are reset.
#pragma once

namespace k2 {
class Remote;
class Ui;
}

namespace k2::shell_hw {

// Takes the objects the commands act on; call once after usb::init(). No-op without the CDC function.
void init(Remote& remote, Ui& ui);

// Call every main-loop iteration: reads typed bytes, runs complete lines, supervises the session.
void poll();

}  // namespace k2::shell_hw
