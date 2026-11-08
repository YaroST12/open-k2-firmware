// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fixed-size byte ring for the recent USB log text, pure and allocation free. usb::log feeds it
// (before the on-device log gate, so the drive's LOG.TXT shows lines even while HID logging is
// off); the drive and `store save-log` read a snapshot on request.
#pragma once
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace k2 {

template <std::size_t N>
class LogRing {
public:
    void push(std::string_view s) {
        for (char c : s) {
            buf_[head_] = c;
            head_ = (head_ + 1) % N;
            if (size_ < N) size_++;
        }
    }
    std::size_t size() const { return size_; }

    // Copy the newest bytes that fit into `out`, oldest first. When bytes were dropped (ring wrapped
    // or `out` too small) the copy starts after the first newline so no partial line is shown;
    // if there is no newline at all the raw tail is returned.
    std::size_t snapshot(std::span<char> out) const {
        const auto parts = view(out.size());
        std::size_t n = 0;
        for (auto part : parts)
            for (char c : part) out[n++] = c;
        return n;
    }

    // Borrow the same line-aligned snapshot without copying. Valid until the next push().
    std::array<std::string_view, 2> view(std::size_t capacity = N) const {
        std::size_t n = size_ < capacity ? size_ : capacity;
        const bool cut = n < size_ || wrapped();
        std::size_t start = (head_ + N - n) % N;
        if (cut) {
            for (std::size_t i = 0; i < n; i++) {
                if (buf_[(start + i) % N] == '\n' && i + 1 < n) {
                    start = (start + i + 1) % N;
                    n -= i + 1;
                    break;
                }
            }
        }
        const std::size_t first = n < N - start ? n : N - start;
        return {{{buf_.data() + start, first}, {buf_.data(), n - first}}};
    }

private:
    bool wrapped() const { return size_ == N; }
    std::array<char, N> buf_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

}  // namespace k2
