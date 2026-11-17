// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the remembered cable identity (k2/chargertest/cablepref.hpp): the record format, a blank volume reads as "no record"
// (the list then starts on the default, the EPR spoof), save and reload over a RAM disk, a damaged or impossible record reads as
// "no record" too, and an explicit Off record is the only way to read Off.
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
    CHECK_EQ(cablepref::decode(off) == kOff, 1);   // an explicit Off record is a choice, not damage
}

void test_the_default_is_the_epr_spoof() {
    CHECK_EQ(cablepref::kDefault == kB1, 1);        // the hardware-proven EPR 50 V / 5 A cable
    CHECK_EQ(ctcable::valid(cablepref::kDefault), 1);
}

void test_the_old_builtin_spelling_is_stored_canonical() {
    CHECK_EQ(cablepref::decode(cablepref::encode(kB0)) == kB1, 1);
}

void test_damage_reads_as_no_record() {
    const auto r = cablepref::encode(kS9);
    CHECK_EQ(cablepref::decode(r) == kS9, 1);
    auto bad = r; bad[7] ^= 1;                  // check byte
    CHECK_EQ(cablepref::decode(bad).has_value(), false);
    bad = r; bad[0] = 'X';                      // magic
    CHECK_EQ(cablepref::decode(bad).has_value(), false);
    bad = r; bad[5] ^= 0x40;                    // a flipped payload bit: the check no longer matches
    CHECK_EQ(cablepref::decode(bad).has_value(), false);
    // well-formed record, impossible choice: a clone without its id, a built-in past the table, an unknown kind, Off with an id
    constexpr Choice kNoId{Kind::Saved, 0}, kFar{Kind::Builtin, 40}, kKind3{static_cast<Kind>(3), 1}, kOffId{Kind::Off, 5};
    CHECK_EQ(cablepref::decode(cablepref::encode(kNoId)).has_value(), false);
    CHECK_EQ(cablepref::decode(cablepref::encode(kFar)).has_value(), false);
    CHECK_EQ(cablepref::decode(cablepref::encode(kKind3)).has_value(), false);
    CHECK_EQ(cablepref::decode(cablepref::encode(kOffId)).has_value(), false);
}

// Ids 5, 6 and 7 were sniffed cable identities on unreleased test builds (removed 2026-11-17; 5's words are now id 2's).
// A /cable.cfg that names one reads as the default, the EPR spoof, and is stored as id 1 from then on; the first id past every
// id ever used is still an impossible choice.
void test_removed_xiaomi_ids_read_as_the_epr_spoof() {
    for (std::uint8_t id : {std::uint8_t(5), std::uint8_t(6), std::uint8_t(7)}) {
        const std::array<std::uint8_t, 8> rec{'K', '2', 'C', 'B', 1, id, 0, static_cast<std::uint8_t>(1 ^ id ^ 0xA5)};
        CHECK_EQ(cablepref::decode(rec) == cablepref::kDefault, 1);
        CHECK_EQ(cablepref::encode(Choice{Kind::Builtin, id}) == cablepref::encode(kB1), 1);
        Vol v;
        CHECK_EQ(v.f.write_file(cablepref::kPath, rec) == fs::Err::Ok, true);
        CHECK_EQ(cablepref::load(v.f) == cablepref::kDefault, 1);
    }
    CHECK_EQ(cablepref::decode(cablepref::encode(Choice{Kind::Builtin, 8})).has_value(), false);
}

void test_save_and_reload() {
    Vol v;
    CHECK_EQ(cablepref::load(v.f).has_value(), false);                  // blank volume: no record, the list starts on the default
    CHECK_EQ(cablepref::save(v.f, kS12), true);
    CHECK_EQ(cablepref::load(v.f) == kS12, 1);
    CHECK_EQ(cablepref::save(v.f, kB2), true);
    CHECK_EQ(cablepref::load(v.f) == kB2, 1);
    CHECK_EQ(cablepref::save(v.f, kOff), true);
    CHECK_EQ(cablepref::load(v.f).has_value(), true);                   // an explicit Off stays Off (it is not the default)
    CHECK_EQ(cablepref::load(v.f) == kOff, 1);
    CHECK_EQ(v.f.exists(cablepref::kPath), true);
}

void test_clear_returns_to_no_record() {
    Vol v;
    CHECK_EQ(cablepref::clear(v.f), true);                              // nothing to remove is fine
    CHECK_EQ(cablepref::save(v.f, kS12), true);
    CHECK_EQ(cablepref::clear(v.f), true);
    CHECK_EQ(v.f.exists(cablepref::kPath), false);
    CHECK_EQ(cablepref::load(v.f).has_value(), false);
}

void test_the_prefs_file_is_untouched() {
    Vol v;
    CHECK_EQ(cablepref::save(v.f, kB1), true);
    CHECK_EQ(v.f.exists("/prefs.cfg"), false);                          // own file: the 8-byte prefs format stays as it is
}

void test_a_short_or_long_file_reads_as_no_record() {
    Vol v;
    const std::array<std::uint8_t, 4> shortf{'K', '2', 'C', 'B'};
    CHECK_EQ(v.f.write_file(cablepref::kPath, shortf) == fs::Err::Ok, true);
    CHECK_EQ(cablepref::load(v.f).has_value(), false);
    const auto r = cablepref::encode(kB1);
    std::array<std::uint8_t, 9> longf{};
    for (size_t i = 0; i < r.size(); ++i) longf[i] = r[i];
    CHECK_EQ(v.f.write_file(cablepref::kPath, longf) == fs::Err::Ok, true);
    CHECK_EQ(cablepref::load(v.f).has_value(), false);
}
}  // namespace

int main() {
    test_record_format();
    test_the_default_is_the_epr_spoof();
    test_the_old_builtin_spelling_is_stored_canonical();
    test_damage_reads_as_no_record();
    test_save_and_reload();
    test_removed_xiaomi_ids_read_as_the_epr_spoof();
    test_clear_returns_to_no_record();
    test_the_prefs_file_is_untouched();
    test_a_short_or_long_file_reads_as_no_record();
    return test_report("test_cablepref");
}
