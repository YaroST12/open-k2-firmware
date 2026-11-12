// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Black Box diagnosis on the HID remote (Cmd 0x72 DIAGNOSE, layout in k2/core/remote_proto.hpp): the report as a
// status block, its text lines in chunks and its findings with their evidence numbers. Pure, host-tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "k2/pd/diag.hpp"

namespace k2::pd::diag::wire {

inline constexpr std::uint8_t kOpStatus = 0, kOpLine = 1, kOpFinding = 2;
inline constexpr std::size_t kStatusBytes = 16;
inline constexpr std::size_t kLineHeader = 5;      // lines(1) kind(1) sev(1) num(1) len(1), then text from `off`
inline constexpr std::size_t kFindingHeader = 24;  // findings(1) sev(1) group(1) code(1) t_ms(4) a b c d (4 each), name
// state: 0 no session yet, 1 a session is open, 2 the last finished one
inline constexpr std::uint8_t kStateNone = 0, kStateOpen = 1, kStateLast = 2;

namespace detail {
inline void put(std::span<std::uint8_t> o, std::size_t at, std::uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; i++) o[at + i] = std::uint8_t(v >> (8 * i));
}
}  // namespace detail

// `req` is the whole request payload, already length-checked by the dispatcher (op 0: 1 byte; op 1: op idx off;
// op 2: op idx). Returns the response size; 0 when `out` is too small or the op is unknown.
inline std::size_t encode(const Report& r, std::uint8_t state, std::uint16_t sessions, std::span<const std::uint8_t> req,
                          std::span<std::uint8_t> out) {
    if (req.empty()) return 0;
    const unsigned lines = state == kStateNone ? 0 : line_count(r);
    switch (req[0]) {
    case kOpStatus: {
        if (out.size() < kStatusBytes) return 0;
        std::uint8_t worst = std::uint8_t(Sev::Ok);
        for (unsigned i = 0; i < r.n; i++) if (std::uint8_t(r.f[i].sev) < worst) worst = std::uint8_t(r.f[i].sev);
        out[0] = state;
        out[1] = r.power;
        out[2] = r.link;
        out[3] = state == kStateNone ? std::uint8_t(Sev::Ok) : worst;
        out[4] = state == kStateNone ? 0 : r.n;
        out[5] = std::uint8_t(lines > 255 ? 255 : lines);
        detail::put(out, 6, sessions, 2);
        detail::put(out, 8, r.start_us, 4);
        detail::put(out, 12, r.dur_ms, 4);
        return kStatusBytes;
    }
    case kOpLine: {
        if (req.size() < 3 || out.size() <= kLineHeader) return 0;
        out[0] = std::uint8_t(lines > 255 ? 255 : lines);
        Line l;
        if (req[1] >= lines || !line(r, req[1], l)) {
            out[1] = out[2] = out[3] = out[4] = 0;
            return kLineHeader;
        }
        const std::size_t len = std::strlen(l.text.data());
        const std::size_t off = req[2] < len ? req[2] : len;
        std::size_t n = len - off;
        if (n > out.size() - kLineHeader) n = out.size() - kLineHeader;
        out[1] = std::uint8_t(l.kind);
        out[2] = std::uint8_t(l.sev);
        out[3] = l.num;
        out[4] = std::uint8_t(len);
        std::memcpy(out.data() + kLineHeader, l.text.data() + off, n);
        return kLineHeader + n;
    }
    case kOpFinding: {
        if (req.size() < 2 || out.size() < kFindingHeader) return 0;
        const unsigned n = state == kStateNone ? 0 : r.n;
        out[0] = std::uint8_t(n);
        if (req[1] >= n) { std::memset(out.data() + 1, 0, kFindingHeader - 1); return kFindingHeader; }
        const Finding& f = r.f[req[1]];
        out[1] = std::uint8_t(f.sev);
        out[2] = std::uint8_t(f.group);
        out[3] = std::uint8_t(f.code);
        detail::put(out, 4, f.t_ms, 4);
        detail::put(out, 8, std::uint32_t(f.a), 4);
        detail::put(out, 12, std::uint32_t(f.b), 4);
        detail::put(out, 16, std::uint32_t(f.c), 4);
        detail::put(out, 20, std::uint32_t(f.d), 4);
        const char* name = code_name(f.code);
        std::size_t k = std::strlen(name);
        if (k > out.size() - kFindingHeader) k = out.size() - kFindingHeader;
        std::memcpy(out.data() + kFindingHeader, name, k);
        return kFindingHeader + k;
    }
    default: return 0;
    }
}

}  // namespace k2::pd::diag::wire
