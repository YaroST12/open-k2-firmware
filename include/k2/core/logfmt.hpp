// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Compact log line encoder (see k2/core/log.hpp for the line format). Pure: no USB, host-tested.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::logfmt {

// Writes "msg#<id> args\n" into `out` (NUL-terminated, cut to fit, the newline always kept);
// `sig` lists the argument kinds (logfmt::signature). Returns the length without the NUL.
std::size_t vencode(std::span<char> out, std::uint16_t id, std::uint32_t sig, std::va_list ap);
std::size_t encode(std::span<char> out, std::uint16_t id, std::uint32_t sig, ...);

}  // namespace k2::logfmt
