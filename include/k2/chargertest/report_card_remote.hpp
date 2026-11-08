// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remote command Cmd::Cards (0x62): list, read, rename and delete charger report cards. Pure logic over a
// store-like Api, host-tested by tests/test_report_card_remote.cpp; src/core/remote.cpp binds it to
// report_card::hw. Request validation (lengths, the delete confirmation byte) is in remote_proto.hpp.
//
//   LIST    req [0] or [0, offset]   rsp [err, total, n, card_no u16 * n]     newest first, from `offset`, at most 27
//   GET     req [1, no u16, chunk]   rsp [err, chunk, bytes...]               the kPayload byte card payload
//                                    (k2/chargertest/report_card.hpp "Payload"), kChunk bytes per chunk (4 chunks for v6: 190 bytes)
//   RENAME  req [2, no u16, name]    rsp [err, no u16]                        name 1..20 of [A-Za-z0-9 -_.]
//   DELETE  req [3, no u16, 1]       rsp [err]                                the trailing 1 confirms
// err: report_card::Err (0 Ok, 1 Invalid, 2 NotFound, 3 Flash, 4 Full) or kCardsErrBusy (a charger test runs).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/chargertest/report_card.hpp"
#include "k2/chargertest/report_card_store.hpp"

namespace k2::report_card::remote {

inline constexpr std::uint8_t kList = 0, kGet = 1, kRename = 2, kDelete = 3;
inline constexpr std::uint8_t kErrBusy = 5;
inline constexpr std::size_t kChunk = 48;
inline constexpr std::size_t kListPage = 27;   // card numbers per LIST response: 3 + 2 * 27 = 57 <= 58
inline constexpr unsigned kChunks = (kPayload + kChunk - 1) / kChunk;
static_assert(kChunk + 2 <= 58, "a chunk fits one response payload (kMaxRspPayload)");

// Api: size_t numbers(span<uint16_t>), bool get(uint16_t, Card&), Err rename(uint16_t, string_view), Err remove(uint16_t).
// `p` was length-checked by the dispatcher; `out` holds at least 58 bytes. Returns the response size.
template <class Api>
std::size_t handle(Api& api, std::span<const std::uint8_t> p, std::span<std::uint8_t> out) {
    auto le16 = [](const std::uint8_t* q) { return std::uint16_t(q[0] | (q[1] << 8)); };
    auto err = [&out](unsigned e) { out[0] = std::uint8_t(e); return std::size_t{1}; };
    switch (p[0]) {
    case kList: {
        std::array<std::uint16_t, kMaxCards> all{};
        const std::size_t total = api.numbers(all);
        const std::size_t off = p.size() > 1 ? p[1] : 0;
        const std::size_t n = off >= total ? 0 : (total - off < kListPage ? total - off : kListPage);
        out[0] = 0; out[1] = std::uint8_t(total); out[2] = std::uint8_t(n);
        for (std::size_t i = 0; i < n; ++i) { out[3 + 2 * i] = std::uint8_t(all[off + i]); out[4 + 2 * i] = std::uint8_t(all[off + i] >> 8); }
        return 3 + 2 * n;
    }
    case kGet: {
        Card c;
        if (p[3] >= kChunks) return err(unsigned(Err::Invalid));
        if (!api.get(le16(&p[1]), c)) return err(unsigned(Err::NotFound));
        std::array<std::uint8_t, kMaxRecord> rec{};
        encode_card(rec, c);
        const std::size_t at = std::size_t(p[3]) * kChunk, n = kPayload - at < kChunk ? kPayload - at : kChunk;
        out[0] = 0; out[1] = p[3];
        for (std::size_t i = 0; i < n; ++i) out[2 + i] = rec[4 + at + i];
        return 2 + n;
    }
    case kRename: {
        const Err e = api.rename(le16(&p[1]), std::string_view(reinterpret_cast<const char*>(&p[3]), p.size() - 3));
        out[0] = std::uint8_t(e); out[1] = p[1]; out[2] = p[2];
        return 3;
    }
    default:   // kDelete
        return err(unsigned(api.remove(le16(&p[1]))));
    }
}

}  // namespace k2::report_card::remote
