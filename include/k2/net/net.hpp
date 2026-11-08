// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB network link (HID+Net mode): an lwIP netif on top of TinyUSB's CDC-NCM
// driver. Device address 192.168.7.1/24, a DHCP server that hands out 192.168.7.2/.3 with no
// router and no DNS option, so the host keeps its own default route.
#pragma once

namespace k2::net {

// Bring up lwIP, the netif and the DHCP server. Called from usb::init().
void init();

// Run lwIP timers. Called from usb::poll() right after tud_task().
void poll();

// Emit the per-hop [NET] counters (four 64-byte log slots). Called from the heartbeat.
void log_stats();

}  // namespace k2::net
