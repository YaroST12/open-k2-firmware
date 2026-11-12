// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remote command Cmd::Cards (k2/chargertest/report_card_remote.hpp) over a real Store on a fake flash.
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "k2/chargertest/report_card_remote.hpp"
#include "k2/chargertest/report_card_store.hpp"
#include "k2/drivers/flash.hpp"
#include "ramflash.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = k2::report_card;
namespace rr = k2::report_card::remote;
namespace ct = k2::charger_test;

// The card ring on a RAM flash with a card store on it.
struct FakeFlash {
    k2test::RamFlash disk;
    ringlog::Ring ring{disk, flash::kCardRingBase, flash::kCardRingSectors, rc::kRingKind, rc::kRingReserve};
    FakeFlash() { ring.scan(); }
};
struct Store : rc::Store {
    explicit Store(FakeFlash& f) : rc::Store(f.ring) {}
};

struct Api {
    Store& s;
    size_t numbers(std::span<uint16_t> o) { return s.numbers(o); }
    bool get(uint16_t n, rc::Card& c) { return s.get(n, c); }
    rc::Err rename(uint16_t n, std::string_view v) { return s.rename(n, v); }
    rc::Err remove(uint16_t n) { return s.remove(n); }
};

static std::vector<uint8_t> call(Api& api, std::vector<uint8_t> req) {
    std::array<uint8_t, 58> out{};
    const size_t n = rr::handle(api, req, out);
    return {out.begin(), out.begin() + n};
}

static rc::Card card(uint32_t fp) {
    rc::Card c;
    c.id.fp = fp; c.id.vid = 0x05AC; c.id.pid = 0x1234;
    c.fw = 0xABCD; c.num_pdos = 5; c.uptime_s = 99;
    c.group[0] = {rc::GV::Pass, 20000, 0, 0};
    c.group[6] = {rc::GV::Fail, 0, 3, 13};
    c.n_ranges = 1; c.range[0] = {uint8_t(ct::Algorithm::Pps), 0, 33, 110, 110, 0};
    return c;
}

static void test_list_newest_first() {
    FakeFlash f; Store s(f); Api api{s};
    CHECK_EQ(call(api, {rr::kList}).size(), 3u);
    rc::Card a = card(1), b = card(2);
    s.put(a); s.put(b);
    const auto r = call(api, {rr::kList});
    CHECK_EQ(r.size(), 7u);
    CHECK_EQ(r[0], 0); CHECK_EQ(r[1], 2); CHECK_EQ(r[2], 2);
    CHECK_EQ(r[3], 2); CHECK_EQ(r[5], 1);
}

// Many cards: LIST pages through them with an offset, newest first.
static void test_list_pages() {
    FakeFlash f; Store s(f); Api api{s};
    for (uint32_t i = 1; i <= 50; ++i) { rc::Card c = card(i); CHECK_EQ(int(s.put(c)), int(rc::Err::Ok)); }
    std::vector<unsigned> got;
    while (got.size() < 50) {
        const auto r = call(api, {rr::kList, uint8_t(got.size())});
        CHECK_EQ(r[0], 0); CHECK_EQ(r[1], 50);
        CHECK_EQ(r[2] > 0 && r[2] <= rr::kListPage, 1);
        for (unsigned i = 0; i < r[2]; ++i) got.push_back(r[3 + 2 * i] | (r[4 + 2 * i] << 8));
        CHECK_EQ(r.size(), 3u + 2u * r[2]);
        if (r[2] == 0) break;
    }
    CHECK_EQ(got.size(), 50u);
    for (unsigned i = 0; i < 50; ++i) CHECK_EQ(got[i], 50u - i);
    CHECK_EQ(call(api, {rr::kList, 60})[2], 0);   // past the end: empty page
}

static void test_get_chunks_reassemble_the_payload() {
    FakeFlash f; Store s(f); Api api{s};
    rc::Card c = card(7);
    s.put(c);
    std::vector<uint8_t> got;
    for (uint8_t k = 0; k < rr::kChunks; ++k) {
        const auto r = call(api, {rr::kGet, 1, 0, k});
        CHECK_EQ(r[0], 0); CHECK_EQ(r[1], k);
        got.insert(got.end(), r.begin() + 2, r.end());
    }
    CHECK_EQ(got.size(), size_t(rc::kPayload));
    rc::Card d;
    CHECK_EQ(rc::decode_card(got, d), true);
    CHECK_EQ(d == c, true);
    CHECK_EQ(call(api, {rr::kGet, 1, 0, rr::kChunks})[0], int(rc::Err::Invalid));
    CHECK_EQ(call(api, {rr::kGet, 9, 0, 0})[0], int(rc::Err::NotFound));
}

static void test_rename_and_delete() {
    FakeFlash f; Store s(f); Api api{s};
    rc::Card c = card(7);
    s.put(c);
    std::vector<uint8_t> req = {rr::kRename, 1, 0};
    for (char ch : std::string("Desk 65W")) req.push_back(uint8_t(ch));
    auto r = call(api, req);
    CHECK_EQ(r.size(), 3u); CHECK_EQ(r[0], 0); CHECK_EQ(r[1], 1);
    rc::Card d;
    s.get(1, d);
    CHECK_STR(d.name, "Desk 65W");
    CHECK_EQ(d.group[0].max_mv, 20000);
    req = {rr::kRename, 1, 0, '!', '!'};
    CHECK_EQ(call(api, req)[0], int(rc::Err::Invalid));
    req = {rr::kRename, 5, 0, 'a'};
    CHECK_EQ(call(api, req)[0], int(rc::Err::NotFound));
    CHECK_EQ(call(api, {rr::kDelete, 1, 0, 1})[0], 0);
    CHECK_EQ(call(api, {rr::kDelete, 1, 0, 1})[0], int(rc::Err::NotFound));
    CHECK_EQ(call(api, {rr::kList})[1], 0);
}

int main() {
    test_list_newest_first();
    test_list_pages();
    test_get_chunks_reassemble_the_payload();
    test_rename_and_delete();
    return test_report(__FILE__);
}
