// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the remembered idle-power settings (k2/core/idle_cfg.hpp): record format, defaults for a blank volume,
// save and reload, a damaged record falls back to the defaults, the bench flag is never stored.
#include "k2/core/fs.hpp"
#include "k2/core/idle_cfg.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2;

namespace {
struct Vol {
    k2test::RamDisk d{96};
    fs::Fs f{d, 96};
    Vol() { bool x; f.mount_or_format(x); }
};

void test_blank_is_default() {
    Vol v;
    const idle::Config c = idle_cfg::load(v.f);
    CHECK_EQ(int(c.brightness), 100);
    CHECK_EQ(int(c.dim_brightness), 30);
    CHECK_EQ(int(c.dim_after_s), 60);
    CHECK_EQ(int(c.off_after_s), 300);
}

void test_record_format() {
    idle::Config c;
    c.brightness = 70; c.dim_brightness = 20; c.dim_after_s = 0x0102; c.off_after_s = 0x0E10;
    const auto r = idle_cfg::encode(c);
    CHECK_EQ(int(r[0]), 'K'); CHECK_EQ(int(r[1]), '2'); CHECK_EQ(int(r[2]), 'I'); CHECK_EQ(int(r[3]), 'D');
    CHECK_EQ(int(r[4]), 70); CHECK_EQ(int(r[5]), 20);
    CHECK_EQ(int(r[6]), 0x02); CHECK_EQ(int(r[7]), 0x01); CHECK_EQ(int(r[8]), 0x10); CHECK_EQ(int(r[9]), 0x0E);
    CHECK_EQ(int(r[10]), int(0xA5 ^ 'K' ^ '2' ^ 'I' ^ 'D' ^ 70 ^ 20 ^ 0x02 ^ 0x01 ^ 0x10 ^ 0x0E));
}

void test_save_and_reload() {
    Vol v;
    idle::Config c;
    c.brightness = 40; c.dim_brightness = 10; c.dim_after_s = 0; c.off_after_s = 1800; c.wake_on_remote = false;
    CHECK_COND(idle_cfg::save(v.f, c));
    const idle::Config d = idle_cfg::load(v.f);
    CHECK_COND(idle_cfg::same(c, d));
    CHECK_COND(d.wake_on_remote);   // the bench flag is not stored: it reads back as its default
    c.brightness = 90;
    CHECK_COND(idle_cfg::save(v.f, c));
    CHECK_EQ(int(idle_cfg::load(v.f).brightness), 90);
}

void test_damaged_falls_back() {
    idle::Config c;
    c.brightness = 40;
    auto r = idle_cfg::encode(c);
    r[10] ^= 1;   // bad check
    CHECK_EQ(int(idle_cfg::decode(r).brightness), 100);
    r = idle_cfg::encode(c);
    r[0] = 'X';   // wrong magic
    CHECK_EQ(int(idle_cfg::decode(r).brightness), 100);
    r = idle_cfg::encode(c);
    r[4] = 101;   // a level above 100 with a matching check
    r[10] = idle_cfg::check(r);
    CHECK_EQ(int(idle_cfg::decode(r).brightness), 100);
    Vol v;   // a file of another length
    const std::array<std::uint8_t, 8> shortrec{'K', '2', 'I', 'D', 1, 2, 3, 4};
    CHECK_COND(v.f.write_file(idle_cfg::kPath, shortrec) == fs::Err::Ok);
    CHECK_EQ(int(idle_cfg::load(v.f).dim_after_s), 60);
}

// A saved level below 10 % would boot to a black screen: it never reaches the file and never comes back from one.
void test_saved_levels_stay_visible() {
    idle::Config c;
    c.brightness = 0; c.dim_brightness = 3;
    const auto r = idle_cfg::encode(c);
    CHECK_EQ(int(r[4]), 10); CHECK_EQ(int(r[5]), 10);
    auto z = idle_cfg::encode(idle::Config{});   // a file an older build wrote with 0 %
    z[4] = 0; z[5] = 0;
    z[10] = idle_cfg::check(z);
    CHECK_EQ(int(idle_cfg::decode(z).brightness), 10);
    CHECK_EQ(int(idle_cfg::decode(z).dim_brightness), 10);
    c.brightness = 10; c.dim_brightness = 10;
    CHECK_EQ(int(idle_cfg::encode(c)[4]), 10);
}

// A remote level below the minimum is live only: what gets saved (and compared with what is saved) keeps the stored level.
void test_low_level_is_never_persisted() {
    idle::Config saved;                  // 100 / 30
    idle::Config live = saved;
    live.brightness = 0;
    idle::Config p = idle_cfg::persisted(live, saved);
    CHECK_EQ(int(p.brightness), 100);
    CHECK_COND(idle_cfg::same(p, saved));
    live.dim_brightness = 9;
    live.off_after_s = 600;              // another field changed in the same burst is saved, the brightness is not
    p = idle_cfg::persisted(live, saved);
    CHECK_EQ(int(p.brightness), 100);
    CHECK_EQ(int(p.dim_brightness), 30);
    CHECK_EQ(int(p.off_after_s), 600);
    CHECK_COND(!idle_cfg::same(p, saved));
    live.brightness = 10;                // the minimum itself is a real setting
    live.dim_brightness = 10;
    p = idle_cfg::persisted(live, saved);
    CHECK_EQ(int(p.brightness), 10);
    CHECK_EQ(int(p.dim_brightness), 10);
}
}  // namespace

int main() {
    test_blank_is_default();
    test_record_format();
    test_save_and_reload();
    test_damaged_falls_back();
    test_saved_levels_stay_visible();
    test_low_level_is_never_persisted();
    return test_report("test_idle_cfg");
}
