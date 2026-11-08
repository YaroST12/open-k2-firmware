// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Ring of fixed-size USB IN reports. Two producers share it: stream frames (screenshots, ripple) that
// the sender paces with free(), and log lines that fire at any time, also from the LCD idle hook.
// A log line never evicts anything: when the ring is full it is dropped. Evicting the oldest entry
// instead removed queued screenshot frames and left a strip short.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2 {

template <std::size_t Slots, std::size_t Size>
class TxQueue {
public:
    // Queues a stream frame; callers pace themselves with free(). On a full ring the oldest entry is
    // dropped (last resort, counted).
    bool push_report(std::span<const std::uint8_t, Size> r) {
        if (full()) { tail_ = next(tail_); dropped_++; }
        store(r);
        return true;
    }
    // Queues a log line; dropped (counted) when the ring is full.
    bool push_log(std::span<const std::uint8_t, Size> r) {
        if (full()) { dropped_++; return false; }
        store(r);
        return true;
    }
    const std::uint8_t *front() const { return head_ == tail_ ? nullptr : q_[tail_].data(); }
    void pop() { if (head_ != tail_) tail_ = next(tail_); }
    std::uint8_t free() const { return static_cast<std::uint8_t>((tail_ + Slots - head_ - 1) % Slots); }
    std::uint32_t dropped() const { return dropped_; }

private:
    static constexpr std::uint8_t next(std::uint8_t i) { return static_cast<std::uint8_t>((i + 1) % Slots); }
    bool full() const { return next(head_) == tail_; }
    void store(std::span<const std::uint8_t, Size> r) {
        for (std::size_t i = 0; i < Size; i++) q_[head_][i] = r[i];
        head_ = next(head_);
    }
    std::array<std::array<std::uint8_t, Size>, Slots> q_{};
    std::uint8_t head_ = 0, tail_ = 0;   // next slot to fill / next slot to send
    std::uint32_t dropped_ = 0;
};

}  // namespace k2
