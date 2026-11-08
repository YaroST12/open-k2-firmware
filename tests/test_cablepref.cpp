// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the remembered cable identity (k2/chargertest/cablepref.hpp): the record format, a blank volume reads as Off, save and
// reload over a RAM disk, a damaged or impossible record reads as Off.
#include <array>
#include <cstdint>

#include "k2/chargertest/cablepref.hpp"
#include "k2/core/fs.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;
using ctcable::Choice;
using ctcable::Kind;

namespace {
constexpr Choice kOff{};
constexpr Choice kB1{Kind::Builtin, 1}, kB2{Kind::Builtin, 2}, kB3{Kind::Builtin, 3}, kB4{Kind::Builtin, 4}, kB0{Kind::Builtin, 0};
constexpr Choice kS12{Kind::Saved, 12}, kS9{Kind::Saved, 9}, kS1234{Kind::Saved, 0x1234};

struct Vol {
    k2test::RamDisk d{96};
    fs::Fs f{d, 96};
    Vol() { bool x; f.mount_or_format(x); }
};

void test_record_format() {
    // "K2CB", kind, seq low, seq high, check = kind ^ seq low ^ seq high ^ 0xA5
    const std::array<std::uint8_t, 8> off{'K', '2', 'C', 'B', 0, 0, 0, 0xA5};
    CHECK_EQ(cablepref::encode(kOff) == off, 1);
    const std::array<std::uint8_t, 8> builtin{'K', '2', 'C', 'B', 1, 4, 0, 0xA0};
    CHECK_EQ(cablepref::encode(kB4) == builtin, 1);
    const std::array<std::uint8_t, 8> saved{'K', '2', 'C', 'B', 2, 0x34, 0x12, 0xA5 ^ 2 ^ 0x34 ^ 0x12};
    CHECK_EQ(cablepref::encode(kS1234) == saved, 1);
    CHECK_EQ(cablepref::decode(saved) == kS1234, 1);
    CHECK_EQ(cablepref::decode(builtin) == kB4, 1);
    // A /cable.cfg written before id 3 (OnePlus 100W mag) was retired reads as OnePlus 100W, and is stored as id 4 from then on.
    const std::array<std::uint8_t, 8> old_mag{'K', '2', 'C', 'B', 1, 3, 0, 0xA7};
    CHECK_EQ(cablepref::decode(old_mag) == kB4, 1);
    CHECK_EQ(cablepref::encode(kB3) == builtin, 1);
    CHECK_EQ(cablepref::decode(off) == kOff, 1);
}

void test_the_old_builtin_spelling_is_stored_canonical() {
    CHECK_EQ(cablepref::decode(cablepref::encode(kB0)) == kB1, 1);
}

void test_damage_reads_as_off() {
    const auto r = cablepref::encode(kS9);
    CHECK_EQ(cablepref::decode(r) == kS9, 1);
    auto bad = r; bad[7] ^= 1;                  // check byte
    CHECK_EQ(cablepref::decode(bad) == kOff, 1);
    bad = r; bad[0] = 'X';                      // magic
    CHECK_EQ(cablepref::decode(bad) == kOff, 1);
    bad = r; bad[5] ^= 0x40;                    // a flipped payload bit: the check no longer matches
    CHECK_EQ(cablepref::decode(bad) == kOff, 1);
    // well-formed record, impossible choice: a clone without its id, a built-in past the table, an unknown kind, Off with an id
    constexpr Choice kNoId{Kind::Saved, 0}, kFar{Kind::Builtin, 40}, kKind3{static_cast<Kind>(3), 1}, kOffId{Kind::Off, 5};
    CHECK_EQ(cablepref::decode(cablepref::encode(kNoId)) == kOff, 1);
    CHECK_EQ(cablepref::decode(cablepref::encode(kFar)) == kOff, 1);
    CHECK_EQ(cablepref::decode(cablepref::encode(kKind3)) == kOff, 1);
    CHECK_EQ(cablepref::decode(cablepref::encode(kOffId)) == kOff, 1);
}

void test_save_and_reload() {
    Vol v;
    CHECK_EQ(cablepref::load(v.f) == kOff, 1);                          // blank volume
    CHECK_EQ(cablepref::save(v.f, kS12), true);
    CHECK_EQ(cablepref::load(v.f) == kS12, 1);
    CHECK_EQ(cablepref::save(v.f, kB2), true);
    CHECK_EQ(cablepref::load(v.f) == kB2, 1);
    CHECK_EQ(cablepref::save(v.f, kOff), true);
    CHECK_EQ(cablepref::load(v.f) == kOff, 1);
    CHECK_EQ(v.f.exists(cablepref::kPath), true);
}

void test_the_prefs_file_is_untouched() {
    Vol v;
    CHECK_EQ(cablepref::save(v.f, kB1), true);
    CHECK_EQ(v.f.exists("/prefs.cfg"), false);                          // own file: the 8-byte prefs format stays as it is
}

void test_a_short_or_long_file_reads_as_off() {
    Vol v;
    const std::array<std::uint8_t, 4> shortf{'K', '2', 'C', 'B'};
    CHECK_EQ(v.f.write_file(cablepref::kPath, shortf) == fs::Err::Ok, true);
    CHECK_EQ(cablepref::load(v.f) == kOff, 1);
    const auto r = cablepref::encode(kB1);
    std::array<std::uint8_t, 9> longf{};
    for (size_t i = 0; i < r.size(); ++i) longf[i] = r[i];
    CHECK_EQ(v.f.write_file(cablepref::kPath, longf) == fs::Err::Ok, true);
    CHECK_EQ(cablepref::load(v.f) == kOff, 1);
}
}  // namespace

int main() {
    test_record_format();
    test_the_old_builtin_spelling_is_stored_canonical();
    test_damage_reads_as_off();
    test_save_and_reload();
    test_the_prefs_file_is_untouched();
    test_a_short_or_long_file_reads_as_off();
    return test_report("test_cablepref");
}
