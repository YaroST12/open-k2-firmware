// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Minimal printf-style formatter for the log lines (no heap, no locale, no float).
// Supported: %d %u %x %X %s %c %%, with an optional '0' flag and width digits for the
// integer conversions ("%02X", "%04d", "%5d"). %x prints upper-case like %X: the log
// parsers in tools/ expect upper-case hex. Anything else prints the character after '%'.
#pragma once
#include <cstdarg>
#include <cstddef>

#include <span>

namespace k2::fmt {

// Writes at most out.size()-1 characters plus a NUL; returns the number of characters
// written (without the NUL). Output that does not fit is cut off. With out.size()==0
// nothing is written.
std::size_t vformat(std::span<char> out, const char* fmt, std::va_list ap);
std::size_t format(std::span<char> out, const char* fmt, ...);

}  // namespace k2::fmt
