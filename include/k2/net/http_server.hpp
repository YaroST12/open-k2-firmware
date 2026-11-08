// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HTTP server of the USB network link: lwIP raw TCP on port 80, static memory, one request
// executed at a time from the main loop. The lwIP callbacks only read the request head and mark
// the connection pending; routing (k2/net/http_api.hpp), the Remote backend and the screenshot
// rendering all run in service(), never inside the network stack's callbacks.
#pragma once
#include <cstdint>

#include "k2/core/remote.hpp"

namespace k2::http_server {

// Strip-wise screen source for /api/screenshot.bmp (see capture.hpp).
struct Shot {
    bool (*begin)();                          // false while another capture runs
    const std::uint16_t* (*strip)(int index); // 4 rows of RGB565, valid until the next call
    void (*end)();
};

// Open port 80. Needs lwIP up (net::init()). `remote` must outlive the server.
void init(Remote& remote, const Shot& shot);

// Main loop, once per pass: starts a pending request, feeds the send buffer, expires dead peers.
void service();

// One [HTTP] counter line per heartbeat.
void log_stats();

}  // namespace k2::http_server
