// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the flash store: the region allow-list in k2::flash and the record/log layer
// in k2::store, over a fake flash that models "program only clears bits, erase sets a page to 0xFF".
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "k2/drivers/flash.hpp"
#include "k2/pd/store.hpp"
#include "test_util.h"

using namespace k2;

namespace {

struct FakeStoreFlash {
    static constexpr std::uint32_t kBase = 0x080B3000;
    std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x08100000 - kBase, 0xFF);
    std::vector<std::uint32_t> erased;
    int programs = 0;
    flash::Status erase_result = flash::Status::Ok;
    flash::Status program_result = flash::Status::Ok;
    bool stuck = false;               // reports Ok, changes nothing

    flash::Status erase(std::uint32_t addr) {
        erased.push_back(addr);
        if (erase_result != flash::Status::Ok) return erase_result;
        if (!stuck) std::memset(&mem[addr - kBase], 0xFF, flash::kSectorSize);
        return flash::Status::Ok;
    }
    flash::Status program(std::uint32_t addr, std::uint16_t v) {
        programs++;
        if (program_result != flash::Status::Ok) return program_result;
        if (!stuck) {
            mem[addr - kBase] &= v & 0xFF;
            mem[addr - kBase + 1] &= v >> 8;
        }
        return flash::Status::Ok;
    }
    std::uint16_t read(std::uint32_t addr) const { return mem[addr - kBase] | (mem[addr - kBase + 1] << 8); }
    const std::uint8_t* view(std::uint32_t addr) const { return &mem[addr - kBase]; }
};

emrec::Record rec(int n, const char* name = "t") {
    std::array<std::uint32_t, 5> v = {0xFF00A041, 0x18002B01, 0, 0x40510000u + n, 0x000A2640};
    return emrec::make(v, 2, name);
}

}  // namespace

static void test_region_policy() {
    CHECK_EQ(flash::kStoreBase, 0x080E4000u);
    CHECK_EQ(flash::kStoreEnd, 0x08100000u);
    CHECK_EQ(flash::store_allowed(flash::kFsBase, 2), 1);
    CHECK_EQ(flash::store_allowed(flash::kFsEnd - 2, 2), 1);
    CHECK_EQ(flash::store_allowed(flash::kFsEnd - 2, 4), 0);       // runs into the native pages
    CHECK_EQ(flash::store_allowed(flash::kFsBase + 1, 2), 0);      // misaligned
    CHECK_EQ(flash::store_allowed(flash::kFsBase, 3), 0);          // odd length
    CHECK_EQ(flash::store_allowed(flash::kFsBase - 2, 2), 0);      // just below
    CHECK_EQ(flash::store_allowed(0x080E4000, 2), 0);              // legacy range: erase only, never programmed
    CHECK_EQ(flash::store_allowed(0x080FFFFE, 2), 0);
    CHECK_EQ(flash::store_allowed(0x080E3000, 2), 0);              // native single slot: read only
    CHECK_EQ(flash::store_allowed(0x080E3800, 2), 0);              // native library: read only
    CHECK_EQ(flash::store_allowed(0x08004010, 2), 0);              // app meta page
    CHECK_EQ(flash::store_allowed(0x08003000, 2), 0);              // bootloader region
    CHECK_EQ(flash::store_allowed(0x08000000, 2), 0);              // bootloader
    CHECK_EQ(flash::store_allowed(0x08003800, 2), 0);
    CHECK_EQ(flash::store_allowed(0xFFFFFFFE, 4), 0);              // wraps around
    CHECK_EQ(flash::store_allowed(0x20000000, 2), 0);
    CHECK_EQ(flash::store_sector_allowed(flash::kFsBase), 1);
    CHECK_EQ(flash::store_sector_allowed(flash::kFsEnd - flash::kSectorSize), 1);
    CHECK_EQ(flash::store_sector_allowed(0x080E4000), 0);          // legacy range: no static grant, needs a content proof
    CHECK_EQ(flash::store_sector_allowed(0x080E4800), 0);
    CHECK_EQ(flash::store_sector_allowed(0x080FF800), 0);
    CHECK_EQ(flash::store_sector_allowed(0x080E4400), 0);          // not sector aligned
    CHECK_EQ(flash::store_sector_allowed(0x080FF000 + 0x1000), 0); // 0x08100000 is outside
    CHECK_EQ(flash::store_sector_allowed(0x080E3000), 0);
    CHECK_EQ(flash::store_sector_allowed(0x080E3800), 0);
    CHECK_EQ(flash::store_sector_allowed(0x08004000), 0);
    CHECK_EQ(flash::store_sector_allowed(0x08000000), 0);
    CHECK_EQ(flash::allowed(0x080E3000, 0), 0);                    // the marker allow-list is unchanged
}

static void test_guarded_erase_and_program() {
    FakeStoreFlash f;
    const std::array<std::uint8_t, 4> data = {1, 2, 3, 4};
    for (std::uint32_t a : {0x08004000u, 0x08000000u, 0x080E3000u, 0x080E3800u})
        CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, a)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(f.erased.size(), 0u);                                  // nothing reached the controller
    for (std::uint32_t a : {0x08003000u, 0x080E3000u, 0x080E3800u, 0x080E4000u, 0x080FFFFEu})
        CHECK_EQ(static_cast<int>(flash::store_program_guarded(f, a, data)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(f.programs, 0);

    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, flash::kFsBase + 0x800)), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(static_cast<int>(flash::store_program_guarded(f, flash::kFsBase + 0x800, data)), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(f.read(flash::kFsBase + 0x800), 0x0201);
    CHECK_EQ(f.read(flash::kFsBase + 0x802), 0x0403);

    // all-0xFF half-words are skipped (the page is erased already)
    FakeStoreFlash g;
    const std::array<std::uint8_t, 6> mixed = {0xFF, 0xFF, 0x12, 0x34, 0xFF, 0xFF};
    CHECK_EQ(static_cast<int>(flash::store_program_guarded(g, flash::kFsBase, mixed)), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(g.programs, 1);

    // odd length: the last byte is padded with 0xFF
    FakeStoreFlash o;
    const std::array<std::uint8_t, 3> odd = {1, 2, 3};
    CHECK_EQ(static_cast<int>(flash::store_program_guarded(o, flash::kFsBase, odd)), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(o.read(flash::kFsBase + 2), 0xFF03);

    // programming over non-erased bits cannot produce the data: verify catches it
    FakeStoreFlash w;
    const std::array<std::uint8_t, 2> a = {0x0F, 0x00}, b = {0xF0, 0x00};
    CHECK_EQ(static_cast<int>(flash::store_program_guarded(w, flash::kFsBase, a)), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(static_cast<int>(flash::store_program_guarded(w, flash::kFsBase, b)), static_cast<int>(flash::Status::VerifyFailed));

    // a controller that reports success and changes nothing
    FakeStoreFlash s;
    s.stuck = true;
    CHECK_EQ(static_cast<int>(flash::store_program_guarded(s, flash::kFsBase, data)), static_cast<int>(flash::Status::VerifyFailed));
    FakeStoreFlash e;
    e.mem[0x800 + 0x100] = 0x00;                                     // second volume sector not blank
    e.stuck = true;
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(e, flash::kFsBase + 0x800)), static_cast<int>(flash::Status::VerifyFailed));

    // controller errors pass through
    FakeStoreFlash p;
    p.program_result = flash::Status::WriteProtect;
    CHECK_EQ(static_cast<int>(flash::store_program_guarded(p, flash::kFsBase, data)), static_cast<int>(flash::Status::WriteProtect));
    p.erase_result = flash::Status::Busy;
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(p, flash::kFsBase)), static_cast<int>(flash::Status::Busy));
}

// The legacy range (0x080E4000..) is erased only when the page carries one of our old store signatures; the
// native area below it never.
static void test_guarded_erase_needs_a_proof() {
    FakeStoreFlash f;
    std::memset(&f.mem[0x080E6800 - FakeStoreFlash::kBase], 0xA5, 0x800);   // someone else's content
    std::memset(&f.mem[0x080E3000 - FakeStoreFlash::kBase], 0xA5, 0x1000);
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E6800)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E4000)), static_cast<int>(flash::Status::Refused));   // blank: nothing proven
    CHECK_EQ(f.erased.size(), 0u);
    // our old USB config sector: magic, flags, ~flags, zero half-word
    const std::uint32_t o = 0x080E6000 - FakeStoreFlash::kBase;
    f.mem[o] = 'K'; f.mem[o + 1] = '2'; f.mem[o + 2] = 'U'; f.mem[o + 3] = 'S';
    f.mem[o + 4] = 0x01; f.mem[o + 5] = 0xFE; f.mem[o + 6] = 0; f.mem[o + 7] = 0;
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E6000)), static_cast<int>(flash::Status::Ok));
    CHECK_EQ(f.erased.size(), 1u);
    // the native pages stay out of reach even with our magic planted in them
    const std::uint32_t q = 0x080E3000 - FakeStoreFlash::kBase;
    f.mem[q] = 'K'; f.mem[q + 1] = '2'; f.mem[q + 2] = 'B'; f.mem[q + 3] = 'B';
    CHECK_EQ(static_cast<int>(flash::store_erase_guarded(f, 0x080E3000)), static_cast<int>(flash::Status::Refused));
    CHECK_EQ(f.erased.size(), 1u);
}

static void test_layout_constants() {
    CHECK_EQ(store::kSingleAddr, 0x080E3000u);
    CHECK_EQ(store::kLibraryAddr, 0x080E3800u);
    CHECK_EQ(store::kLibraryAddr + flash::kSectorSize, flash::kStoreBase);
}

static void test_empty_store() {
    FakeStoreFlash f;
    store::Store<FakeStoreFlash> st(f);
    const store::Info i = st.load();
    CHECK_EQ(i.lib.count, 0);
    CHECK_EQ(i.has_single, 0);
    CHECK_EQ(i.has_native, 0);
}

// The native pages are read, never written: load() leaves flash untouched.
static void test_native_pages_are_read_only() {
    FakeStoreFlash f;
    const emrec::Record r = rec(7, "[copied emark]");
    std::memcpy(&f.mem[0x30000], r.data(), 66);                           // single slot page
    std::uint8_t* pg = &f.mem[0x30800];
    pg[0] = 0x00; pg[1] = 0x12; pg[2] = 0x34; pg[3] = 0x56; pg[4] = 0; pg[5] = 2;
    for (unsigned n = 0; n < 2; n++) std::memcpy(&pg[6 + n * 66], rec(10 + n).data(), 66);
    std::uint8_t s = 0xA5;
    for (unsigned k = 0; k < 6 + 2 * 66; k++) s = static_cast<std::uint8_t>(s + pg[k]);
    pg[6 + 2 * 66] = s;
    const std::vector<std::uint8_t> before = f.mem;
    store::Store<FakeStoreFlash> st(f);
    const store::Info i = st.load();
    CHECK_EQ(i.has_single, 1);
    CHECK_EQ(std::memcmp(i.single.data(), r.data(), 66), 0);
    CHECK_EQ(i.has_native, 1);
    CHECK_EQ(i.native.count, 2); CHECK_EQ(i.native.checksum_ok, 1); CHECK_EQ(i.native.valid_mask, 3);
    CHECK_EQ(f.mem == before, 1);                                    // nothing changed, both native pages byte for byte
    CHECK_EQ(f.programs, 0);
    CHECK_EQ(f.erased.size(), 0u);
}

int main() {
    test_region_policy();
    test_guarded_erase_and_program();
    test_guarded_erase_needs_a_proof();
    test_layout_constants();
    test_empty_store();
    test_native_pages_are_read_only();
    return test_report("test_store");
}
