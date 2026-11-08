// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Compact views over black-box records for the K2 screen: one row per interesting event with the
// time since the previous row, and a mini VBUS strip. Pure code over a record buffer (a capture
// slot), no hardware, host-tested (tests/test_pdtrace_view.cpp). The host tool tools/pd_trace.py is
// the full decoder; this keeps only what fits 240 px.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::pdtrace::view {

struct Row {
    std::uint32_t dt_us = 0;   // since the previous row (0 for the first)
    std::uint32_t t_us = 0;    // record timestamp
    char dir = ' ';            // '<' received, '>' sent, '!' event
    char name[12] = {};        // NUL terminated, at most 11 characters
};

// Rows are made from: Rx / Tx messages (GoodCRC skipped), failed TX results, hard resets, CC attach
// and detach. Everything else is a register-level detail left to the host tool.
std::size_t count_rows(std::span<const std::uint8_t> records);
bool row_at(std::span<const std::uint8_t> records, std::size_t index, Row& out);

// "<SrcCap      12.3ms": direction, name padded to 12, the delta right-aligned. Returns the length.
std::size_t format_row(const Row& r, std::span<char> out);

// Capture summary for the list and header, absolute so nothing ticks while the screen is up:
// format_duration "850ms", "2.5s", "12s", "1m15s"; format_capture "#7 2.5s 5 msg" (sequence number, time
// span of the records, message rows); format_capture_long "#7  2.5s  5 msgs". Return the length.
std::size_t format_duration(std::uint32_t us, std::span<char> out);
std::size_t format_capture(std::span<const std::uint8_t> records, std::uint32_t seq, std::span<char> out);
std::size_t format_capture_long(std::span<const std::uint8_t> records, std::uint32_t seq, std::span<char> out);

// The VBUS polyline: one height (0..h) per column over the capture's time span, 0xFF where no
// sample is known yet. mv_min / mv_max are widened to at least 1000 mV so noise does not fill the strip.
struct Strip {
    std::uint16_t mv_min = 0, mv_max = 0;
    std::uint32_t t0_us = 0, t1_us = 0;
    bool valid = false;
};
Strip vbus_strip(std::span<const std::uint8_t> records, std::span<std::uint8_t> heights, std::uint8_t h);

// Column of time `t_us` in a strip of `w` columns (clamped), for the trigger marker.
std::size_t column_of(const Strip& s, std::uint32_t t_us, std::size_t w);
// Timestamp of the last whole record inside the first `bytes` bytes (the trigger moment of a capture).
std::uint32_t time_at(std::span<const std::uint8_t> records, std::size_t bytes);

// Who was on the wire during a capture, read from the records alone (passive: nothing is stored
// while recording). A Discover Identity ACK on SOP gives VID:PID of its sender; the header's power
// role bit says which side answered: `dev` = a sink-role sender, `src` = a source-role sender.
// `fp` is the fingerprint of the first Source_Capabilities ("67W 5PDO+2PPS": best PDO power, number
// of fixed/battery/variable PDOs, PPS and AVS APDOs), the grouping key when no identity was seen.
struct Identity {
    bool has_dev = false, has_src = false;
    std::uint16_t dev_vid = 0, dev_pid = 0, src_vid = 0, src_pid = 0;
    char fp[24] = {};
};
Identity identify(std::span<const std::uint8_t> records);
// Grouping key text: "05AC:1234" (the device, else the source), else the fingerprint, else "?".
std::size_t format_key(const Identity& id, std::span<char> out);
bool same_key(const Identity& a, const Identity& b);
// The VID of the key when it is an identity, else 0 (for the vendor name lookup).
std::uint16_t key_vid(const Identity& id);

// One capture as fixed-width JSON lines for the USB drive (BLACKBOX/CAPn.JSN). Every line is
// kJsonLine bytes (space padded, newline last) so a file offset maps straight to a record:
//   {"t":<us>,"y":<type>,"d":"<payload hex>"}
// kJsonRows rows cover a whole 2 KB slot (shortest record is 6 bytes) plus the meta and link lines. Rows past
// the last record are blank lines, so the file is always kJsonRows * kJsonLine bytes.
// Line 0 is the meta line (the capture's identity, see json_meta), line 1 the link line (see json_link in
// k2/pd/pdtrace_link.hpp); records start at line 2.
inline constexpr std::size_t kJsonLine = 128;
inline constexpr std::size_t kJsonRows = 2048 / 6 + 3;
inline constexpr std::size_t kJsonSize = kJsonLine * kJsonRows;
// Row `index` (0-based) into out[0..kJsonLine); a blank line when there is no such record.
void json_row(std::span<const std::uint8_t> records, std::size_t index, std::span<char, kJsonLine> out);
// {"meta":{"key":"05AC:1234","dev":"..","src":"..","fp":".."}}: key is the grouping key, dev / src
// / fp appear only when seen.
void json_meta(std::span<const std::uint8_t> records, std::span<char, kJsonLine> out);
// Line `index` of the file: the meta line, the link line, then json_row(index - 2).
void json_line(std::span<const std::uint8_t> records, std::size_t index, std::span<char, kJsonLine> out);

}  // namespace k2::pdtrace::view
