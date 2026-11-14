// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Reassembles remote frames from a byte stream. The WebUSB bulk OUT pipe is a stream (TinyUSB's vendor FIFO hands over
// whatever bytes arrived, in any chunking), while the remote protocol is made of fixed 64-byte frames that start with the
// request magic (A5 4B, k2/core/remote_proto.hpp). FrameAssembler cuts frames out of the stream:
//  - bytes before the magic are dropped (a lone A5 or 4B included);
//  - once the magic is seen, 64 bytes make a frame, handed to the callback;
//  - a partial frame that has been waiting longer than kPartialTimeoutMs is dropped, so a lost tail costs one frame, not
//    the one after it (the host sends whole frames back to back, so a gap means the tail is not coming);
//  - reset() starts clean (the host re-enumerated or the configuration changed).
// Pure code, no hardware: tests/test_frame_rx.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/remote_proto.hpp"

namespace k2::remote {

class FrameAssembler {
public:
    static constexpr std::uint32_t kPartialTimeoutMs = 50;

    // Feed the bytes read at `now_ms`; `on_frame(std::span<const uint8_t, kFrameSize>)` runs once per whole frame.
    template <class OnFrame>
    void feed(std::span<const std::uint8_t> bytes, std::uint32_t now_ms, OnFrame&& on_frame) {
        if (len_ != 0 && now_ms - last_ms_ > kPartialTimeoutMs) len_ = 0;
        last_ms_ = now_ms;
        for (const std::uint8_t b : bytes) {
            if (len_ == 0) {
                if (b == kReqMagic0) buf_[len_++] = b;
                continue;
            }
            if (len_ == 1 && b != kReqMagic1) {
                len_ = 0;
                if (b == kReqMagic0) buf_[len_++] = b;   // A5 A5 4B: the second A5 starts the frame
                continue;
            }
            buf_[len_++] = b;
            if (len_ == kFrameSize) {
                len_ = 0;
                on_frame(std::span<const std::uint8_t, kFrameSize>(buf_, kFrameSize));
            }
        }
    }

    void reset() { len_ = 0; }
    std::size_t pending() const { return len_; }

private:
    std::uint8_t buf_[kFrameSize] = {};
    std::size_t len_ = 0;
    std::uint32_t last_ms_ = 0;
};

}  // namespace k2::remote
