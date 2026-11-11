// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the legacy-page ownership proof (k2/core/legacy_proof.hpp) and for the erase of legacy
// pages (k2/core/legacy_wipe.hpp): a page is erased only when it carries one of this firmware's old raw-store
// magics; native content (settings, e-marker records, watch faces, the activation word) survives.
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "k2/drivers/flash.hpp"
#include "k2/core/legacy_proof.hpp"
#include "k2/core/legacy_wipe.hpp"
#include "test_util.h"

using namespace k2;

namespace {

constexpr std::uint32_t kBase = 0x080E0000;   // the fake covers the native area and every legacy page
constexpr std::uint32_t kEnd = 0x08100000;

// Fake flash: erase sets a sector to 0xFF, program clears bits. Fills the native area with
// deterministic non-blank "native" bytes (never one of our magics at a magic position).
struct Fake {
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(kEnd - kBase, 0xFF);
    std::vector<std::uint32_t> erased;
    flash::Status erase_result = flash::Status::Ok;

    Fake() {
        std::uint32_t x = 0x1234567;
        for (auto& b : mem) {
            x = x * 1664525u + 1013904223u;
            b = static_cast<std::uint8_t>(x >> 24);
        }
    }
    flash::Status erase(std::uint32_t a) {
        erased.push_back(a);
        if (erase_result != flash::Status::Ok) return erase_result;
        std::memset(&mem[a - kBase], 0xFF, flash::kSectorSize);
        return flash::Status::Ok;
    }
    flash::Status program(std::uint32_t, std::uint16_t) { return flash::Status::Refused; }
    std::uint16_t read(std::uint32_t a) const { return static_cast<std::uint16_t>(mem[a - kBase] | (mem[a - kBase + 1] << 8)); }

    void put(std::uint32_t a, std::initializer_list<std::uint8_t> bytes) {
        std::uint32_t i = a - kBase;
        for (auto b : bytes) mem[i++] = b;
    }
    void put_magic(std::uint32_t a, const char (&m)[5]) { put(a, {std::uint8_t(m[0]), std::uint8_t(m[1]), std::uint8_t(m[2]), std::uint8_t(m[3])}); }
    // The log snapshot header: "K2LG" and the u32 text length (the text spills into the second sector above 2040).
    void put_log(std::uint32_t a, std::uint32_t len) {
        put(a, {'K', '2', 'L', 'G', std::uint8_t(len), std::uint8_t(len >> 8), std::uint8_t(len >> 16), std::uint8_t(len >> 24)});
    }
    bool blank(std::uint32_t a) const {
        for (std::uint32_t i = 0; i < flash::kSectorSize; i++)
            if (mem[a - kBase + i] != 0xFF) return false;
        return true;
    }
    auto reader() const { return [this](std::uint32_t a) { return read(a); }; }
    bool ours(std::uint32_t a) const { return legacy::ours(a, reader()); }
};
static_assert(flash::StoreHwOps<Fake>);

// A journal record (k2/chargertest/charger_journal.hpp framing): A5 type len 0 payload crc16(CCITT, init FFFF).
void put_journal_record(Fake& f, std::uint32_t a, std::uint8_t type, std::uint8_t len, bool good_crc = true) {
    std::vector<std::uint8_t> r = {0xA5, type, len, 0};
    for (std::uint8_t i = 0; i < len; i++) r.push_back(static_cast<std::uint8_t>(0x11 + i));
    std::uint16_t crc = 0xFFFF;
    for (std::uint8_t b : r) {
        crc ^= static_cast<std::uint16_t>(b << 8);
        for (int i = 0; i < 8; i++) crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    if (!good_crc) crc ^= 0x0101;
    r.push_back(static_cast<std::uint8_t>(crc));
    r.push_back(static_cast<std::uint8_t>(crc >> 8));
    for (std::uint8_t b : r) f.mem[(a++) - kBase] = b;
}

}  // namespace

static void test_constants() {
    CHECK_EQ(legacy::kBase, flash::kStoreBase);
    CHECK_EQ(legacy::kEnd, flash::kStoreEnd);
    CHECK_EQ(legacy::kSectors, 56u);
    CHECK_EQ(legacy::kLegacyUsbCfg, 0x080E6000u);
    CHECK_EQ(legacy::kLegacyUiPrefs, 0x080E6800u);
}

static void test_native_content_is_never_ours() {
    Fake f;   // every byte of the native area and of the legacy range is pseudo-random "native data"
    for (std::uint32_t a = kBase; a < kEnd; a += flash::kSectorSize) CHECK_EQ(f.ours(a), 0);
    // Blank is not ours either (nothing to erase).
    Fake b;
    std::memset(b.mem.data(), 0xFF, b.mem.size());
    for (std::uint32_t a = kBase; a < kEnd; a += flash::kSectorSize) CHECK_EQ(b.ours(a), 0);
}

static void test_each_store_is_recognised_by_its_magic() {
    Fake f;
    f.put_log(0x080E4000, 1500);
    CHECK_EQ(f.ours(0x080E4000), 1);
    CHECK_EQ(f.ours(0x080E4800), 0);                   // the text fits the first sector: the second was never written
    f.put_log(0x080E4000, 3000);
    CHECK_EQ(f.ours(0x080E4000), 1);
    CHECK_EQ(f.ours(0x080E4800), 1);                   // second sector of the log snapshot: proven by the first
    f.put_log(0x080E4000, 5000);                       // a length no snapshot can have: damaged header
    CHECK_EQ(f.ours(0x080E4000), 0);
    CHECK_EQ(f.ours(0x080E4800), 0);
    f.put_log(0x080E4000, 3000);
    CHECK_EQ(f.ours(0x080E5000), 0);                   // capture area was never written
    f.put(0x080E6000, {'K', '2', 'U', 'S', 0x03, 0xFC, 0, 0});
    CHECK_EQ(f.ours(0x080E6000), 1);
    f.put(0x080E6800, {'K', '2', 'U', 'P', 0x01, 0xFE, 0, 0});
    CHECK_EQ(f.ours(0x080E6800), 1);
    f.put_magic(0x080E7000, "K2RC");
    f.put_magic(0x080E7800, "K2RC");
    CHECK_EQ(f.ours(0x080E7000), 1);
    CHECK_EQ(f.ours(0x080E7800), 1);
    for (std::uint32_t a = 0x080E8000; a < 0x080F8000; a += 0x800) {
        CHECK_EQ(f.ours(a), 0);
        f.put_magic(a, "K2BB");
        CHECK_EQ(f.ours(a), 1);
    }
    for (std::uint32_t a = 0x080FC000; a < 0x08100000; a += 0x800) {
        CHECK_EQ(f.ours(a), 0);
        f.put_magic(a, "K2EL");
        CHECK_EQ(f.ours(a), 1);
    }
}

static void test_wrong_store_magic_in_a_page_is_not_ours() {
    Fake f;
    f.put_magic(0x080E8000, "K2EL");                  // an e-marker magic in a Black Box page
    CHECK_EQ(f.ours(0x080E8000), 0);
    f.put_magic(0x080FC000, "K2BB");
    CHECK_EQ(f.ours(0x080FC000), 0);
    f.put_magic(0x080E7000, "K2LG");
    CHECK_EQ(f.ours(0x080E7000), 0);
    f.put(0x080E6000, {'K', '2', 'U', 'S', 0x03, 0x03, 0, 0});   // flags not complemented: damaged
    CHECK_EQ(f.ours(0x080E6000), 0);
    f.put(0x080E6800, {'K', '2', 'U', 'P', 0x01, 0x01, 0, 0});
    CHECK_EQ(f.ours(0x080E6800), 0);
    f.put_magic(0x080E7800, "K2RX");                   // a near miss of the card magic
    CHECK_EQ(f.ours(0x080E7800), 0);
}

static void test_journal_sectors_need_a_valid_record() {
    Fake f;
    CHECK_EQ(f.ours(0x080F8000), 0);
    put_journal_record(f, 0x080F8000, 1, 30);          // header record
    CHECK_EQ(f.ours(0x080F8000), 1);
    put_journal_record(f, 0x080F8800 + 0x100, 2, 20);  // a step record inside the second sector
    CHECK_EQ(f.ours(0x080F8800), 1);
    put_journal_record(f, 0x080FA000, 2, 20, false);   // bad CRC
    CHECK_EQ(f.ours(0x080FA000), 0);
    put_journal_record(f, 0x080FA800, 9, 20);          // unknown type
    CHECK_EQ(f.ours(0x080FA800), 0);
    put_journal_record(f, 0x080FB000, 3, 5);           // odd length
    CHECK_EQ(f.ours(0x080FB000), 0);
    // A record that straddles into the next sector does not prove the sector (the neighbour may be erased first).
    put_journal_record(f, 0x080FB800 - 10, 2, 20);
    CHECK_EQ(f.ours(0x080FB000), 0);
}

static void test_guard_refuses_unproven_pages() {
    Fake f;
    // Native settings, the two e-marker pages and every legacy page with foreign content: refused, untouched.
    for (std::uint32_t a : {0x080E0000u, 0x080E2800u, 0x080E3000u, 0x080E3800u, 0x080E4000u, 0x080E6000u, 0x080E8000u,
                            0x080F8000u, 0x080FC000u, 0x080FF800u})
        CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, a)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(f.erased.size(), 0u);
    // Our magic at a legacy page: allowed.
    f.put_magic(0x080E8000, "K2BB");
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E8000)), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(f.blank(0x080E8000), 1);
    // The same magic below the legacy range (native settings page) never counts.
    f.put_magic(0x080E0000, "K2BB");
    f.put_magic(0x080E3000, "K2BB");
    f.put_magic(0x080E3800, "K2EL");
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E0000)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E3000)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E3800)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(f.erased.size(), 1u);
}

static void test_wipe_erases_only_our_pages() {
    Fake f;
    f.put(0x080E6000, {'K', '2', 'U', 'S', 0x01, 0xFE, 0, 0});
    f.put_log(0x080E4000, 3000);
    f.put_magic(0x080E8000, "K2BB");
    f.put_magic(0x080F7800, "K2BB");
    put_journal_record(f, 0x080F8000, 1, 30);
    f.put_magic(0x080FC800, "K2EL");
    const std::vector<std::uint8_t> before = f.mem;

    struct Env {
        Fake& f;
        std::uint16_t read16(std::uint32_t a) const { return f.read(a); }
        flash::Status erase(std::uint32_t a) { return flash::store_erase_guarded(f, a); }
    } env{f};
    const legacy::WipeResult r = legacy::wipe(env);
    CHECK_EQ(r.erased, 7u);                            // log 2 sectors, usbcfg, two Black Box, journal, e-marker
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(flash::Status::Ok));
    for (std::uint32_t a : {0x080E4000u, 0x080E4800u, 0x080E6000u, 0x080E8000u, 0x080F7800u, 0x080F8000u, 0x080FC800u})
        CHECK_EQ(f.blank(a), 1);
    // Everything else is byte-for-byte what it was before (the native area, the unknown legacy pages).
    for (std::uint32_t a = kBase; a < kEnd; a += flash::kSectorSize) {
        const bool wiped = (a == 0x080E4000 || a == 0x080E4800 || a == 0x080E6000 || a == 0x080E8000 || a == 0x080F7800 || a == 0x080F8000 || a == 0x080FC800);
        if (wiped) continue;
        CHECK_EQ(std::memcmp(&f.mem[a - kBase], &before[a - kBase], flash::kSectorSize), 0);
    }
    // The log's second sector goes before its first: the first one carries the proof.
    CHECK_EQ(f.erased.front(), 0x080FC800u);
    std::size_t i48 = 0, i40 = 0;
    for (std::size_t i = 0; i < f.erased.size(); i++) { if (f.erased[i] == 0x080E4800) i48 = i; if (f.erased[i] == 0x080E4000) i40 = i; }
    CHECK_EQ(i48 < i40, 1);
    // A second run has nothing left to do.
    Fake& g = f;
    g.erased.clear();
    CHECK_EQ(legacy::wipe(env).erased, 0u);
    CHECK_EQ(g.erased.size(), 0u);
}

static void test_wipe_stops_at_the_first_failed_erase() {
    Fake f;
    f.put_magic(0x080E8000, "K2BB");
    f.put_magic(0x080E8800, "K2BB");
    f.erase_result = flash::Status::Busy;
    struct Env {
        Fake& f;
        std::uint16_t read16(std::uint32_t a) const { return f.read(a); }
        flash::Status erase(std::uint32_t a) { return flash::store_erase_guarded(f, a); }
    } env{f};
    const legacy::WipeResult r = legacy::wipe(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(flash::Status::Busy));
    CHECK_EQ(r.erased, 0u);
    CHECK_EQ(r.failed_at, 0x080E8800u);                // descending: the highest proven page first
    CHECK_EQ(f.erased.size(), 1u);
}

// The kept top page 0x080FF800 is never erased, not even when it carries our e-marker library signature: the wipe
// passes over it (and still wipes the proven page below it) instead of stopping at the guard's refusal.
static void test_wipe_passes_over_the_kept_top_page() {
    Fake f;
    f.put_magic(0x080FF800, "K2EL");
    f.put_magic(0x080FF000, "K2EL");
    struct Env {
        Fake& f;
        std::uint16_t read16(std::uint32_t a) const { return f.read(a); }
        flash::Status erase(std::uint32_t a) { return flash::store_erase_guarded(f, a); }
    } env{f};
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080FF800)), static_cast<int>(flash::Status::Refused));
    const legacy::WipeResult r = legacy::wipe(env);
    CHECK_EQ(static_cast<int>(r.err), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(r.erased, 1u);
    CHECK_EQ(f.blank(0x080FF000), 1);
    CHECK_EQ(f.read(0x080FF800), 0x324B);
}

int main() {
    test_constants();
    test_wipe_passes_over_the_kept_top_page();
    test_native_content_is_never_ours();
    test_each_store_is_recognised_by_its_magic();
    test_wrong_store_magic_in_a_page_is_not_ours();
    test_journal_sectors_need_a_valid_record();
    test_guard_refuses_unproven_pages();
    test_wipe_erases_only_our_pages();
    test_wipe_stops_at_the_first_failed_erase();
    return test_report("test_legacy_proof");
}
