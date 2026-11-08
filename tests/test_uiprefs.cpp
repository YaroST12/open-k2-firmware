// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the remembered user preferences (k2/ui/uiprefs.hpp): record format, blank flash =
// defaults, save and reload over a fake flash, a damaged record falls back to the defaults.
#include <cstring>

#include "k2/drivers/flash.hpp"
#include "k2/core/fs.hpp"
#include "ramdisk.hpp"
#include "k2/ui/uiprefs.hpp"
#include "test_util.h"

using namespace k2;

namespace {
struct Vol {
    k2test::RamDisk d{96};
    fs::Fs f{d, 96};
    Vol() { bool x; f.mount_or_format(x); }
};

void test_blank_is_default() {
    Vol v; fs::Fs& f = v.f;
    CHECK_EQ(uiprefs::load(f).qc_after_pd, 0);
}

void test_save_and_reload() {
    Vol v; fs::Fs& f = v.f;
    CHECK_EQ(uiprefs::save(f, {true}) == true, 1);
    CHECK_EQ(uiprefs::load(f).qc_after_pd, 1);
    CHECK_EQ(uiprefs::save(f, {false}) == true, 1);
    CHECK_EQ(uiprefs::load(f).qc_after_pd, 0);
    CHECK_EQ(uiprefs::save(f, {true}) == true, 1);
    CHECK_EQ(uiprefs::load(f).qc_after_pd, 1);
}

void test_record_format() {
    const auto r = uiprefs::encode({true});
    const std::array<std::uint8_t, 8> want{'K', '2', 'U', 'P', 1, 0xFE, 0, 0};
    CHECK_EQ(r == want, 1);
    CHECK_EQ(uiprefs::decode(r).qc_after_pd, 1);
}

void test_damage_falls_back() {
    auto r = uiprefs::encode({true});
    r[5] ^= 1;                                  // complement mismatch
    CHECK_EQ(uiprefs::decode(r).qc_after_pd, 0);
    r = uiprefs::encode({true});
    r[0] = 'X';                                 // wrong magic
    CHECK_EQ(uiprefs::decode(r).qc_after_pd, 0);
    r = uiprefs::encode({true});
    r[4] = 4; r[5] = 0xFB;                      // unknown flag bit
    CHECK_EQ(uiprefs::decode(r).qc_after_pd, 0);
}

void test_screenshot_switch() {
    CHECK_EQ(uiprefs::Prefs{}.screenshot, 1);                          // default on
    CHECK_EQ(uiprefs::decode(uiprefs::encode({false, true})).screenshot, 1);
    const auto off = uiprefs::encode({false, false});
    const std::array<std::uint8_t, 8> want{'K', '2', 'U', 'P', 2, 0xFD, 0, 0};
    CHECK_EQ(off == want, 1);
    CHECK_EQ(uiprefs::decode(off).screenshot, 0);
    CHECK_EQ(uiprefs::decode(off).qc_after_pd, 0);
    const std::array<std::uint8_t, 8> old{'K', '2', 'U', 'P', 1, 0xFE, 0, 0};   // record from before the switch
    CHECK_EQ(uiprefs::decode(old).screenshot, 1);
    CHECK_EQ(uiprefs::decode(old).qc_after_pd, 1);
    std::array<std::uint8_t, 8> bad = off; bad[5] ^= 1;               // damaged = defaults = on
    CHECK_EQ(uiprefs::decode(bad).screenshot, 1);
}

void test_smoothing_preset() {
    using smoothing::Preset;
    CHECK_EQ(uiprefs::Prefs{}.smoothing == Preset::Medium, 1);          // default
    // the default adds nothing to the record: byte 6 stays 0, so the file is the one older firmware wrote
    const std::array<std::uint8_t, 8> plain{'K', '2', 'U', 'P', 1, 0xFE, 0, 0};
    CHECK_EQ(uiprefs::encode({true, true, Preset::Medium}) == plain, 1);
    CHECK_EQ(uiprefs::decode(plain).smoothing == Preset::Medium, 1);    // an old record = the default
    // the other presets are stored as 1 + index in byte 6, flags untouched
    const std::array<std::uint8_t, 8> off{'K', '2', 'U', 'P', 1, 0xFE, 1, 0};
    CHECK_EQ(uiprefs::encode({true, true, Preset::Off}) == off, 1);
    const std::array<std::uint8_t, 8> high{'K', '2', 'U', 'P', 2, 0xFD, 4, 0};
    CHECK_EQ(uiprefs::encode({false, false, Preset::High}) == high, 1);
    for (unsigned i = 0; i < smoothing::kPresetCount; i++) {
        const Preset p = static_cast<Preset>(i);
        const auto back = uiprefs::decode(uiprefs::encode({true, false, p}));
        CHECK_EQ(back.smoothing == p, 1);
        CHECK_EQ(back.qc_after_pd, 1);
        CHECK_EQ(back.screenshot, 0);
    }
    // an unknown byte 6 keeps the flags and falls back to the default preset
    std::array<std::uint8_t, 8> odd = plain; odd[6] = 9;
    CHECK_EQ(uiprefs::decode(odd).smoothing == Preset::Medium, 1);
    CHECK_EQ(uiprefs::decode(odd).qc_after_pd, 1);
    // survives the file system
    Vol v;
    CHECK_EQ(uiprefs::save(v.f, {false, true, Preset::Low}) == true, 1);
    CHECK_EQ(uiprefs::load(v.f).smoothing == Preset::Low, 1);
}

void test_legacy_page_is_outside_the_volume() {
    CHECK_EQ(flash::store_sector_allowed(uiprefs::kLegacyAddr), 0);   // no static grant: erasable only with our signature on it
    CHECK_EQ(flash::legacy_candidate(uiprefs::kLegacyAddr), 1);
    CHECK_EQ(uiprefs::kLegacyAddr % flash::kSectorSize, 0u);
}
}  // namespace

int main() {
    test_blank_is_default();
    test_save_and_reload();
    test_record_format();
    test_damage_falls_back();
    test_screenshot_switch();
    test_smoothing_preset();
    test_legacy_page_is_outside_the_volume();
    return test_report("test_uiprefs");
}
