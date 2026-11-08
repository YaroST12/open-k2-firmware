// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD session record that survives a reset (pure part: layout, CRC, summary text).
//
// The last partner state, written to 16 backup registers (src/session.cpp) when it changes,
// read back at boot. It does not resume the interrogation by itself; the boot log shows what was
// running, so a restart in the middle of Identify / hard reset / trigger is no longer blind.
//
// Layout, 16 halfwords: [0] magic, [1] CRC-16/CCITT over bytes of [2..15] (little endian),
// [2] boots, [3] cc | flags<<8, [4] id_state | id_step<<8, [5] id_ok_mask | pos<<8, [6] mv,
// [7] op_ma, [8] max_ma, [9] num_pdos | spec_rev<<8, [10..15] first three raw PDOs (lo, hi).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "k2/pd/pd_identify.hpp"

namespace k2::session {

inline constexpr std::uint16_t kMagic = 0x4B32;  // "K2"
inline constexpr std::size_t kWords = 16;
inline constexpr std::size_t kPdos = 3;

struct Record {
    std::uint16_t boots = 0;          // boots that found a valid record (wraps)
    std::uint8_t cc = 0;              // 0 none, 1, 2
    bool caps_valid = false;
    bool contract_valid = false;
    bool contract_ready = false;
    bool identify_running = false;
    std::uint8_t id_state = 0;        // pd::IdState
    std::uint8_t id_step = 0;         // pd::IdStep
    std::uint8_t id_ok_mask = 0;
    std::uint8_t pos = 0;             // contract object position (1-based)
    std::uint16_t mv = 0, op_ma = 0, max_ma = 0;
    std::uint8_t num_pdos = 0, spec_rev = 0;
    std::array<std::uint32_t, kPdos> pdo{};
    bool operator==(const Record&) const = default;
};

constexpr std::uint16_t crc16(const std::array<std::uint16_t, kWords>& w) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 2; i < kWords; i++) {
        for (int byte = 0; byte < 2; byte++) {
            crc = std::uint16_t(crc ^ (std::uint16_t(w[i] >> (8 * byte) & 0xFF) << 8));
            for (int b = 0; b < 8; b++) crc = std::uint16_t((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
        }
    }
    return crc;
}

constexpr std::array<std::uint16_t, kWords> encode(const Record& r) {
    std::array<std::uint16_t, kWords> w{};
    w[0] = kMagic;
    w[2] = r.boots;
    const unsigned flags = (r.caps_valid ? 1u : 0u) | (r.contract_valid ? 2u : 0u) | (r.contract_ready ? 4u : 0u) |
                           (r.identify_running ? 8u : 0u);
    w[3] = std::uint16_t(r.cc | flags << 8);
    w[4] = std::uint16_t(r.id_state | unsigned(r.id_step) << 8);
    w[5] = std::uint16_t(r.id_ok_mask | unsigned(r.pos) << 8);
    w[6] = r.mv;
    w[7] = r.op_ma;
    w[8] = r.max_ma;
    w[9] = std::uint16_t(r.num_pdos | unsigned(r.spec_rev) << 8);
    for (std::size_t i = 0; i < kPdos; i++) {
        w[10 + 2 * i] = std::uint16_t(r.pdo[i] & 0xFFFF);
        w[11 + 2 * i] = std::uint16_t(r.pdo[i] >> 16);
    }
    w[1] = crc16(w);
    return w;
}

constexpr std::optional<Record> decode(const std::array<std::uint16_t, kWords>& w) {
    if (w[0] != kMagic || w[1] != crc16(w)) return std::nullopt;
    Record r;
    r.boots = w[2];
    r.cc = std::uint8_t(w[3] & 0xFF);
    const unsigned flags = w[3] >> 8;
    r.caps_valid = flags & 1u;
    r.contract_valid = flags & 2u;
    r.contract_ready = flags & 4u;
    r.identify_running = flags & 8u;
    r.id_state = std::uint8_t(w[4] & 0xFF);
    r.id_step = std::uint8_t(w[4] >> 8);
    r.id_ok_mask = std::uint8_t(w[5] & 0xFF);
    r.pos = std::uint8_t(w[5] >> 8);
    r.mv = w[6];
    r.op_ma = w[7];
    r.max_ma = w[8];
    r.num_pdos = std::uint8_t(w[9] & 0xFF);
    r.spec_rev = std::uint8_t(w[9] >> 8);
    for (std::size_t i = 0; i < kPdos; i++) r.pdo[i] = w[10 + 2 * i] | std::uint32_t(w[11 + 2 * i]) << 16;
    return r;
}

// One line for the boot log, e.g. "cc=2 caps=5 pos=2 9000mV 3000mA ready id=run:Sink caps ok=07".
// NUL terminated, truncated to `n`; returns the length.
inline std::size_t summarize(const Record& r, char* out, std::size_t n) {
    if (n == 0) return 0;
    std::size_t len = 0;
    auto put = [&](char c) { if (len + 1 < n) out[len++] = c; };
    auto str = [&](const char* s) { while (*s) put(*s++); };
    auto num = [&](unsigned v) {
        char t[10];
        int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    };
    str("cc="); num(r.cc);
    str(" caps="); num(r.caps_valid ? r.num_pdos : 0);
    if (r.contract_valid) {
        str(" pos="); num(r.pos);
        put(' '); num(r.mv); str("mV ");
        num(r.op_ma); str("mA");
        if (r.contract_ready) str(" ready");
    } else {
        str(" no-contract");
    }
    str(" id=");
    switch (static_cast<pd::IdState>(r.id_state)) {
        case pd::IdState::Running:
            str("run:");
            str(pd::id_step_short(static_cast<pd::IdStep>(r.id_step)));
            break;
        case pd::IdState::Done: str("done"); break;
        case pd::IdState::Aborted: str("aborted"); break;
        default: str("idle"); break;
    }
    if (r.id_state != 0 && r.id_ok_mask) {
        const char* hex = "0123456789ABCDEF";
        str(" ok="); put(hex[r.id_ok_mask >> 4 & 15]); put(hex[r.id_ok_mask & 15]);
    }
    out[len] = '\0';
    return len;
}

}  // namespace k2::session
