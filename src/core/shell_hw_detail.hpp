// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Internal declarations shared between shell_hw.cpp (cold) and shell_hw_hot.cpp (K2_HOT).
#pragma once

#include <cstdint>
#include <string_view>

#include "k2/core/shell.hpp"
#include "tusb.h"

namespace k2 {
class Remote;
class Ui;
}  // namespace k2

namespace k2::shell_hw::detail {

inline constexpr int kLabAbortMv = 12000;   // lab commands (no `fc trigger` running) drop the lines above this

extern Remote* g_remote;
extern Ui* g_ui;
extern bool g_session;
extern bool g_fc_started;
extern bool g_raw_dp;
extern bool g_raw_dm;
extern std::uint32_t g_silent_until;
extern shell::Shell* const g_shell_ptr;

inline bool session_up() { return tud_mounted() && !tud_suspended() && tud_cdc_connected(); }

void put(std::string_view s);
void raw_to_hiz();
void silent_end();
void session_end();

}  // namespace k2::shell_hw::detail
