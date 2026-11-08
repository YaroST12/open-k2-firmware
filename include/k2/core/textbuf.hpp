// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Append-only text builder over a caller-owned buffer, on top of k2::fmt (no heap, no stdio).
// Output that does not fit is cut off; truncated() tells. Pure, host-tested through its users.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <span>
#include <string_view>

#include "k2/core/fmt.hpp"

namespace k2 {

class TextBuf {
public:
    explicit TextBuf(std::span<char> buf) : buf_(buf) {}

    void put(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        std::va_list ap;
        va_start(ap, fmt);
        const std::size_t room = n_ < buf_.size() ? buf_.size() - n_ : 0;
        const std::size_t w = fmt::vformat(buf_.subspan(n_ < buf_.size() ? n_ : buf_.size()), fmt, ap);
        va_end(ap);
        n_ += w;
        if (w + 1 >= room) cut_ = true;   // vformat keeps one byte for the NUL
    }
    void put_raw(std::string_view s) {
        for (char c : s) {
            if (n_ + 1 < buf_.size()) buf_[n_++] = c;
            else cut_ = true;
        }
        if (n_ < buf_.size()) buf_[n_] = 0;
    }
    std::string_view view() const { return {buf_.data(), n_}; }
    std::size_t size() const { return n_; }
    bool truncated() const { return cut_; }

private:
    std::span<char> buf_;
    std::size_t n_ = 0;
    bool cut_ = false;
};

}  // namespace k2
