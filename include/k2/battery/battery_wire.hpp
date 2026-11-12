// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The battery recorder over the remote protocol (Cmd::Battery 0x58, layout in k2/core/remote_proto.hpp): pure code over the
// Service, so the host tests drive it the way tools/k2_remote.py does.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::battery {
class Service;
}

namespace k2::battery::wire {

enum Op : std::uint8_t { kStatus = 0, kStart = 1, kEndPhase = 2, kNext = 3, kFinish = 4, kConfig = 5, kList = 6, kRead = 7 };
// What the last op did, in the status block's `result` byte. A refusal is never a protocol error: the block tells why.
enum class Result : std::uint8_t { Ok, Busy, NoMonitor, BadArg, NotNow };

inline constexpr std::size_t kStatusBytes = 40;
inline constexpr std::size_t kChunk = 48;     // record bytes per read
inline constexpr std::uint8_t kFlagWriteFailed = 1, kFlagUnwritten = 2, kFlagMonitor = 4, kFlagFinalEff = 8;

// The request length of each op (the op byte included); 0 for an unknown op. The dispatcher checks it before handle().
constexpr std::size_t req_len(std::uint8_t op) {
    switch (op) {
    case kStatus: case kEndPhase: case kNext: case kFinish: case kList: return 1;
    case kStart: return 3;        // mode wiring
    case kConfig: return 6;       // end_ma(2) end_s(2) wiring
    case kRead: return 5;         // id(2) off(2)
    default: return 0;
    }
}

// One request (length already checked) -> the reply payload in `out` (at least kStatusBytes + 8 bytes); returns its size.
std::size_t handle(Service &s, bool monitor_online, std::span<const std::uint8_t> p, std::span<std::uint8_t> out);

}  // namespace k2::battery::wire
