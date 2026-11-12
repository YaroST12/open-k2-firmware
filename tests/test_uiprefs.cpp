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

void test_orientation_byte() {
    using uiprefs::Prefs;
    // Defaults add nothing: byte 7 stays 0 (auto rotation, buttons not rotated), the file older firmware wrote.
    const std::array<std::uint8_t, 8> plain{'K', '2', 'U', 'P', 1, 0xFE, 0, 0};
    CHECK_EQ(Prefs{}.rotation_manual, 0);
    CHECK_EQ(Prefs{}.rotation, 0);
    CHECK_EQ(Prefs{}.rotate_buttons, 0);
    CHECK_EQ(uiprefs::encode(Prefs{.qc_after_pd = true}) == plain, 1);
    const Prefs old = uiprefs::decode(plain);   // an old record (byte 7 = 0) reads as today's defaults
    CHECK_EQ(old.rotation_manual, 0); CHECK_EQ(old.rotation, 0); CHECK_EQ(old.rotate_buttons, 0);
    // byte 7: bit0..1 manual rotation, bit2 manual (0 = auto), bit3 rotate buttons; flags and smoothing stay put
    const std::array<std::uint8_t, 8> m270{'K', '2', 'U', 'P', 1, 0xFE, 0, 0x07};
    CHECK_EQ(uiprefs::encode(Prefs{.qc_after_pd = true, .rotation_manual = true, .rotation = 3}) == m270, 1);
    const std::array<std::uint8_t, 8> btn{'K', '2', 'U', 'P', 1, 0xFE, 0, 0x08};
    CHECK_EQ(uiprefs::encode(Prefs{.qc_after_pd = true, .rotate_buttons = true}) == btn, 1);
    // the rotation value alone (auto mode) is stored but means nothing until manual is set
    const std::array<std::uint8_t, 8> auto2{'K', '2', 'U', 'P', 1, 0xFE, 0, 0x02};
    CHECK_EQ(uiprefs::encode(Prefs{.qc_after_pd = true, .rotation = 2}) == auto2, 1);
    for (unsigned rot = 0; rot < 4; rot++) for (bool manual : {false, true}) for (bool buttons : {false, true}) {
        const Prefs in{.qc_after_pd = true, .screenshot = false, .smoothing = smoothing::Preset::High,
                       .rotation_manual = manual, .rotation = static_cast<std::uint8_t>(rot), .rotate_buttons = buttons};
        const Prefs back = uiprefs::decode(uiprefs::encode(in));
        CHECK_EQ(back.rotation_manual, manual); CHECK_EQ(back.rotation, rot); CHECK_EQ(back.rotate_buttons, buttons);
        CHECK_EQ(back.qc_after_pd, 1); CHECK_EQ(back.screenshot, 0); CHECK_EQ(back.smoothing == smoothing::Preset::High, 1);
    }
    // the unused high bits of byte 7 are ignored, not a reason to drop the record
    std::array<std::uint8_t, 8> odd = btn; odd[7] |= 0xF0;
    CHECK_EQ(uiprefs::decode(odd).rotate_buttons, 1);
    CHECK_EQ(uiprefs::decode(odd).qc_after_pd, 1);
    // survives the file system
    Vol v;
    CHECK_EQ(uiprefs::save(v.f, Prefs{.rotation_manual = true, .rotation = 2, .rotate_buttons = true}) == true, 1);
    const Prefs loaded = uiprefs::load(v.f);
    CHECK_EQ(loaded.rotation_manual, 1); CHECK_EQ(loaded.rotation, 2); CHECK_EQ(loaded.rotate_buttons, 1);
}

// Settings > Wake sleeping port: bit4 of the orientation byte, 0 = off (the default, and what every older file holds).
void test_idle_wake_bit() {
    using uiprefs::Prefs;
    CHECK_EQ(Prefs{}.idle_wake, 0);
    const std::array<std::uint8_t, 8> plain{'K', '2', 'U', 'P', 1, 0xFE, 0, 0};
    CHECK_EQ(uiprefs::decode(plain).idle_wake, 0);
    CHECK_EQ(uiprefs::encode(Prefs{.qc_after_pd = true, .idle_wake = false}) == plain, 1);   // off adds nothing to the file
    const std::array<std::uint8_t, 8> on{'K', '2', 'U', 'P', 1, 0xFE, 0, 0x10};
    CHECK_EQ(uiprefs::encode(Prefs{.qc_after_pd = true, .idle_wake = true}) == on, 1);
    CHECK_EQ(uiprefs::decode(on).idle_wake, 1);
    // the other byte-7 settings ride along, in both directions
    const Prefs in{.rotation_manual = true, .rotation = 3, .rotate_buttons = true, .idle_wake = true};
    const Prefs back = uiprefs::decode(uiprefs::encode(in));
    CHECK_EQ(back.idle_wake, 1); CHECK_EQ(back.rotation, 3); CHECK_EQ(back.rotation_manual, 1); CHECK_EQ(back.rotate_buttons, 1);
    CHECK_EQ(uiprefs::decode(uiprefs::encode(Prefs{.rotate_buttons = true})).idle_wake, 0);
    // survives the file system
    Vol v;
    CHECK_EQ(uiprefs::save(v.f, Prefs{.idle_wake = true}) == true, 1);
    CHECK_EQ(uiprefs::load(v.f).idle_wake, 1);
}

// Home hero (Long Back cycles W -> V -> A): bits 5..6 of the orientation byte, 0 = W (the default, and what every older
// file holds); 3 is not a hero and reads as W.
void test_home_hero_bits() {
    using uiprefs::Prefs;
    CHECK_EQ(Prefs{}.home_hero, 0);
    const std::array<std::uint8_t, 8> old{'K', '2', 'U', 'P', 0, 0xFF, 0, 0x1B};   // an older record: rotation bits only
    CHECK_EQ(uiprefs::decode(old).home_hero, 0);
    CHECK_EQ(uiprefs::encode(Prefs{}) == uiprefs::encode(Prefs{.home_hero = 0}), 1);   // W adds nothing to the file
    const std::array<std::uint8_t, 8> amps{'K', '2', 'U', 'P', 0, 0xFF, 0, 0x40};
    CHECK_EQ(uiprefs::encode(Prefs{.home_hero = 2}) == amps, 1);
    CHECK_EQ(uiprefs::decode(amps).home_hero, 2);
    const std::array<std::uint8_t, 8> bad{'K', '2', 'U', 'P', 0, 0xFF, 0, 0x60};
    CHECK_EQ(uiprefs::decode(bad).home_hero, 0);
    for (std::uint8_t h = 0; h < 3; h++) {
        const Prefs in{.rotation_manual = true, .rotation = 2, .rotate_buttons = true, .idle_wake = true, .home_hero = h};
        const Prefs back = uiprefs::decode(uiprefs::encode(in));
        CHECK_EQ(back == in, 1);
    }
    CHECK_EQ(uiprefs::decode(uiprefs::encode(Prefs{.home_hero = 1})).idle_wake, 0);
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
    test_orientation_byte();
    test_idle_wake_bit();
    test_home_hero_bits();
    test_legacy_page_is_outside_the_volume();
    return test_report("test_uiprefs");
}
