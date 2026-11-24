// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Transparent constant string compression (BPE-157).
//
// K2_P("...") packs an ASCII string literal at compile time (consteval, optimal DP) against the global recursive
// byte-pair table in strpack_table.hpp and places the NUL-terminated bytes in linker section `k2_strpack`, returning
// a plain `const char*`. Sinks (gfx_text, fit_text, text_width, StrBuf::put, diag::Out::s) check `is_packed(s)` in
// 3 instructions via the section bounds `[__start_k2_strpack, __stop_k2_strpack)` and decode on the fly with zero
// type/struct/callsite overhead.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "k2/core/strpack_table.hpp"

namespace k2::strpack {

// Linker-provided bounds of the `k2_strpack` section (link.ld on ARM; GNU ld/lld auto-symbols on host).
extern "C" [[gnu::weak]] const char __start_k2_strpack[];
extern "C" [[gnu::weak]] const char __stop_k2_strpack[];

inline bool is_packed(const char *s) {
    if (!s || !__start_k2_strpack) return false;
    const auto a = reinterpret_cast<std::uintptr_t>(s);
    const auto lo = reinterpret_cast<std::uintptr_t>(__start_k2_strpack);
    const auto hi = reinterpret_cast<std::uintptr_t>(__stop_k2_strpack);
    return a - lo < hi - lo;
}

inline bool is_packed(std::string_view s) { return !s.empty() && is_packed(s.data()); }

constexpr bool is_literal_byte(std::uint8_t b) {
    return static_cast<std::uint8_t>(b - 0x20u) < 0x5Fu || b == '\n' || b == '\t' || b == '\r';
}

constexpr int byte_to_idx(std::uint8_t b) {
    return b >= 0x7Fu ? static_cast<int>(b - 0x7Fu) : (129 + static_cast<int>(b) - 1);
}

constexpr std::uint8_t idx_to_byte(int idx) {
    return idx < 129 ? static_cast<std::uint8_t>(0x7F + idx) : static_cast<std::uint8_t>(idx - 129 + 1);
}

// Streaming byte-by-byte BPE-157 decoder. Uses 16 bytes of local stack; zero heap or static RAM.
// constexpr so compile-time round-trip static_asserts and width checks run the exact same code.
class Reader {
public:
    constexpr explicit Reader(const char *s) : p_(s) {
        if consteval {
            for (auto &x : stk_) x = 0;
        }
    }

    constexpr int next() {
        if consteval {
            return step();
        } else {
            return next_rt();
        }
    }

    constexpr int step() {
        std::uint8_t b;
        if (sp_) {
            b = stk_[--sp_];
        } else {
            if consteval {
                if (!*p_) return -1;
            } else {
                if (!p_ || !*p_) return -1;
            }
            b = static_cast<std::uint8_t>(*p_++);
        }
        while (!is_literal_byte(b)) {
            const int idx = byte_to_idx(b);
            if (idx < 0 || idx >= table::kTableLen || (table::kPair[idx][0] == 0 && table::kPair[idx][1] == 0))
                return -1;
            stk_[sp_++] = table::kPair[idx][1];
            b = table::kPair[idx][0];
        }
        return b;
    }

private:
    int next_rt();

    const char *p_;
    std::uint8_t stk_[16];
    std::uint8_t sp_ = 0;
};

// Decode a packed string into `out[0..cap-1]` (always NUL-terminated when cap > 0). Returns full decoded length.
std::size_t decode(const char *s, char *out, std::size_t cap);

// Out-of-line helpers to keep sinks minimal.
std::string_view unpack_rt(std::string_view s, char *buf, std::size_t cap);
int cells_rt(const char *s);

// Convenience: if `s` is packed, decode into `buf` and return a view of `buf`; otherwise return `s` unchanged.
template <std::size_t N>
inline std::string_view unpack(std::string_view s, char (&buf)[N]) {
    return unpack_rt(s, buf, N);
}

// Decoded cell count (UTF-8 code points) of a BPE-packed or plain ASCII string (constexpr for static_assert width checks).
constexpr int cells_packed(const char *s) {
    if consteval {
    } else {
        if (!s) return 0;
    }
    Reader r(s);
    int n = 0;
    for (int b; (b = r.next()) >= 0;) n += (b & 0xC0) != 0x80;
    return n;
}

namespace detail {

// Compile-time expanded byte strings of every rule in table::kPair, for optimal-DP encoding.
struct ExpTable {
    static constexpr int kMaxRuleBytes = 32;
    std::uint8_t bytes[160][kMaxRuleBytes]{};
    std::uint8_t len[160]{};
    std::uint8_t depth[160]{};
    std::int16_t head[128]{};   // first rule index starting with ASCII byte c, or -1
    std::int16_t next[160]{};   // next rule index with the same first byte, or -1
};

consteval ExpTable build_exp_table() {
    ExpTable t{};
    for (int i = 0; i < 128; i++) t.head[i] = -1;
    for (int i = 0; i < 160; i++) t.next[i] = -1;
    // Populate in multiple passes so topological order is guaranteed regardless of index order.
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < table::kTableLen; i++) {
            if (t.len[i] != 0) continue;
            const std::uint8_t a = table::kPair[i][0];
            const std::uint8_t b = table::kPair[i][1];
            if (a == 0 && b == 0) continue;
            auto ready = [&](std::uint8_t x) -> bool {
                if (is_literal_byte(x)) return true;
                const int j = byte_to_idx(x);
                return j >= 0 && j < table::kTableLen && t.len[j] > 0;
            };
            if (!ready(a) || !ready(b)) continue;
            int out = 0;
            std::uint8_t da = 0, db = 0;
            auto append = [&](std::uint8_t x, std::uint8_t &dx) {
                if (is_literal_byte(x)) {
                    if (out < ExpTable::kMaxRuleBytes) t.bytes[i][out++] = x;
                    dx = 0;
                } else {
                    const int j = byte_to_idx(x);
                    dx = t.depth[j];
                    for (int k = 0; k < t.len[j] && out < ExpTable::kMaxRuleBytes; k++)
                        t.bytes[i][out++] = t.bytes[j][k];
                }
            };
            append(a, da);
            append(b, db);
            t.len[i] = static_cast<std::uint8_t>(out);
            t.depth[i] = static_cast<std::uint8_t>((da > db ? da : db) + 1);
            if (out >= 2 && t.bytes[i][0] < 128) {
                t.next[i] = t.head[t.bytes[i][0]];
                t.head[t.bytes[i][0]] = static_cast<std::int16_t>(i);
            }
            progress = true;
        }
    }
    return t;
}

inline constexpr ExpTable kExp = build_exp_table();

template <std::size_t N>
struct Lit {
    char s[N]{};
    static constexpr std::size_t len = N - 1;
    consteval Lit(const char (&in)[N]) {
        for (std::size_t i = 0; i < N; i++) s[i] = in[i];
    }
};

template <std::size_t N>
struct Encoded {
    char data[N + 1]{};
    std::size_t len = 0;
};

template <std::size_t N>
consteval Encoded<N> encode_dp(const Lit<N> &lit) {
    constexpr std::size_t n = N - 1;
    std::array<std::uint16_t, n + 1> best{};
    std::array<std::uint8_t, n + 1> pick_tok{};
    std::array<std::uint8_t, n + 1> pick_adv{};
    best[n] = 0;
    for (std::size_t i = n; i-- > 0;) {
        const auto c0 = static_cast<std::uint8_t>(lit.s[i]);
        best[i] = static_cast<std::uint16_t>(best[i + 1] + 1);
        pick_tok[i] = c0;
        pick_adv[i] = 1;
        for (int r = c0 < 128 ? kExp.head[c0] : -1; r >= 0; r = kExp.next[r]) {
            const std::uint8_t rl = kExp.len[r];
            if (i + rl > n) continue;
            bool match = true;
            for (std::uint8_t k = 1; k < rl; k++) {
                if (static_cast<std::uint8_t>(lit.s[i + k]) != kExp.bytes[r][k]) {
                    match = false;
                    break;
                }
            }
            if (match && best[i + rl] + 1 < best[i]) {
                best[i] = static_cast<std::uint16_t>(best[i + rl] + 1);
                pick_tok[i] = idx_to_byte(r);
                pick_adv[i] = rl;
            }
        }
    }
    Encoded<N> out{};
    std::size_t pos = 0;
    for (std::size_t i = 0; i < n;) {
        out.data[pos++] = static_cast<char>(pick_tok[i]);
        i += pick_adv[i];
    }
    out.data[pos] = '\0';
    out.len = pos;
    return out;
}

template <Lit S>
consteval auto pack_exact() {
    constexpr auto enc = encode_dp(S);
    std::array<char, enc.len + 1> out{};
    for (std::size_t i = 0; i <= enc.len; i++) out[i] = enc.data[i];
    return out;
}

template <Lit S>
consteval bool verify_roundtrip(const char *packed) {
    Reader r(packed);
    for (std::size_t i = 0; i < S.len; i++) {
        if (r.next() != static_cast<std::uint8_t>(S.s[i])) return false;
    }
    return r.next() < 0;
}

namespace {
template <Lit S>
struct Store {
    [[gnu::section("k2_strpack")]] static constexpr auto bytes = pack_exact<S>();
    static_assert(verify_roundtrip<S>(bytes.data()), "K2_P round-trip mismatch (non-ASCII literal?)");
};
}  // namespace

}  // namespace detail
}  // namespace k2::strpack

// Pack an ASCII string literal into section `k2_strpack`, returning a plain `const char*`.
#define K2_P(lit) (::k2::strpack::detail::Store<::k2::strpack::detail::Lit(lit)>::bytes.data())
