// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress saved results (k2/pdstress/pdstress_store.hpp) over littlefs on a RAM disk: put keeps the newest kMaxSaved, list is
// newest first, read returns the bytes put, remove drops one, and foreign files in /ps (the journal, bad names, wrong sizes) are
// never listed.
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <algorithm>

#include "k2/pdstress/pdstress_store.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2::pdstress;

#define CHECK(c) CHECK_COND(c)

namespace {

constexpr std::size_t kBlocks = 64;

struct Vol {
    k2test::RamDisk disk{kBlocks};
    k2::fs::Fs fs{disk, kBlocks};
    store::Store st{fs};
    Vol() { bool fm = false; fs.mount_or_format(fm); }
};

// A blob whose bytes are a function of `tag`, with a valid version byte.
struct Blob {
    std::uint8_t tag;
    std::uint8_t at(std::size_t off) const { return off == 0 ? result::kVersion : std::uint8_t(off * 7 + tag); }
};
void fill(void* ctx, std::size_t off, std::span<std::uint8_t> out) {
    const auto* b = static_cast<const Blob*>(ctx);
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = b->at(off + i);
}
bool same(store::Store& st, std::uint16_t id, std::uint8_t tag) {
    const Blob b{tag};
    std::array<std::uint8_t, 100> buf{};
    for (std::size_t off = 0; off < store::kFileBytes; off += buf.size()) {
        const std::size_t n = st.read(id, off, buf);
        const std::size_t want = std::min(buf.size(), store::kFileBytes - off);
        if (n != want) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (buf[i] != b.at(off + i)) return false;
    }
    return st.read(id, store::kFileBytes, buf) == 0;
}

void test_put_read_round_trip() {
    Vol v;
    Blob b{3};
    std::uint16_t id = 0;
    CHECK(v.st.put(fill, &b, id) == store::Err::Ok);
    CHECK(id == 1);
    std::array<std::uint16_t, store::kMaxSaved> ids{};
    CHECK(v.st.list(ids) == 1);
    CHECK(ids[0] == 1);
    CHECK(same(v.st, 1, 3));
    CHECK(v.st.read(2, 0, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(ids.data()), 4)) == 0);   // no such result
}

void test_keeps_newest_eight() {
    Vol v;
    for (std::uint8_t k = 1; k <= 10; ++k) {
        Blob b{k};
        std::uint16_t id = 0;
        CHECK(v.st.put(fill, &b, id) == store::Err::Ok);
        CHECK(id == k);
    }
    std::array<std::uint16_t, 12> ids{};
    const std::size_t n = v.st.list(ids);
    CHECK(n == store::kMaxSaved);
    for (std::size_t i = 0; i < n; ++i) CHECK(ids[i] == 10 - i);   // newest first
    CHECK(same(v.st, 10, 10));
    CHECK(same(v.st, 3, 3));
    std::array<std::uint8_t, 4> b4{};
    CHECK(v.st.read(1, 0, b4) == 0);   // the two oldest are gone
    CHECK(v.st.read(2, 0, b4) == 0);
    // A short output list gets the newest.
    std::array<std::uint16_t, 3> few{};
    CHECK(v.st.list(few) == 3);
    CHECK(few[0] == 10 && few[1] == 9 && few[2] == 8);
}

void test_remove() {
    Vol v;
    std::uint16_t id = 0;
    for (std::uint8_t k = 1; k <= 3; ++k) { Blob b{k}; v.st.put(fill, &b, id); }
    CHECK(v.st.remove(2) == store::Err::Ok);
    CHECK(v.st.remove(2) == store::Err::NotFound);
    std::array<std::uint16_t, 8> ids{};
    CHECK(v.st.list(ids) == 2);
    CHECK(ids[0] == 3 && ids[1] == 1);
    // Ids are never reused: the next one follows the newest.
    Blob b{9};
    CHECK(v.st.put(fill, &b, id) == store::Err::Ok);
    CHECK(id == 4);
}

void test_refuses_bad_blob() {
    Vol v;
    std::uint16_t id = 77;
    auto bad = [](void*, std::size_t, std::span<std::uint8_t> out) { for (auto& x : out) x = 0xEE; };
    CHECK(v.st.put(bad, nullptr, id) == store::Err::Invalid);
    std::array<std::uint16_t, 8> ids{};
    CHECK(v.st.list(ids) == 0);
}

void test_ignores_foreign_files() {
    Vol v;
    v.fs.mkdir("/ps");
    const std::array<std::uint8_t, 40> junk{};
    CHECK(v.fs.write_file("/ps/run.rec", junk) == k2::fs::Err::Ok);   // the run journal shares the directory
    CHECK(v.fs.write_file("/ps/00zz.psr", junk) == k2::fs::Err::Ok);  // not hex
    CHECK(v.fs.write_file("/ps/0009.psr", junk) == k2::fs::Err::Ok);  // a result name, the wrong size
    std::array<std::uint16_t, 8> ids{};
    CHECK(v.st.list(ids) == 0);
    Blob b{1};
    std::uint16_t id = 0;
    CHECK(v.st.put(fill, &b, id) == store::Err::Ok);
    CHECK(v.st.list(ids) == 1);
    CHECK(ids[0] == id);
    std::array<std::uint8_t, 4> b4{};
    CHECK(v.st.read(9, 0, b4) == 0);   // a wrong-sized file is not a result
    std::size_t n = 0;
    std::array<std::uint8_t, 40> back{};
    CHECK(v.fs.read_file("/ps/run.rec", back, n) == k2::fs::Err::Ok && n == 40);   // put never touches the journal
}

void test_name_format() {
    std::array<char, store::kPathLen> p{};
    store::path_of(0x00AF, p);
    CHECK(std::string_view(p.data()) == "/ps/00af.psr");
    std::uint16_t id = 0;
    CHECK(store::parse_name("00af.psr", id) && id == 0xAF);
    CHECK(!store::parse_name("run.rec", id));
    CHECK(!store::parse_name("00af.ps", id));
    CHECK(!store::parse_name("0000.psr", id));   // id 0 is never used
}

}  // namespace

int main() {
    test_put_read_round_trip();
    test_keeps_newest_eight();
    test_remove();
    test_refuses_bad_blob();
    test_ignores_foreign_files();
    test_name_format();
    return test_report("pdstress_store");
}
