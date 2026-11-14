// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the remembered USB link setting (k2/drivers/usbcfg.hpp): record format, blank flash = off,
// save and reload over a fake flash, a damaged record falls back to off.
#include <cstring>

#include "k2/core/fs.hpp"
#include "ramdisk.hpp"
#include "k2/drivers/usbcfg.hpp"
#include "test_util.h"

using namespace k2;

namespace {
struct Vol {
    k2test::RamDisk d{96};
    fs::Fs f{d, 96};
    Vol() { bool x; f.mount_or_format(x); }
};

void test_blank_flash_is_off() {
    Vol v_f; fs::Fs& f = v_f.f;
    const usbcfg::Setting s = usbcfg::load(f);
    CHECK_EQ(s.hid, 0); CHECK_EQ(s.drive, 0);
    CHECK_EQ(usbcfg::any(s), 0);
}

void test_save_and_reload() {
    Vol v_f; fs::Fs& f = v_f.f;
    CHECK_EQ(usbcfg::save(f, {true, false}) == true, 1);
    usbcfg::Setting s = usbcfg::load(f);
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 0);
    CHECK_EQ(usbcfg::save(f, {false, true}) == true, 1);
    s = usbcfg::load(f);
    CHECK_EQ(s.hid, 0); CHECK_EQ(s.drive, 1);
    CHECK_EQ(usbcfg::save(f, {true, true}) == true, 1);
    s = usbcfg::load(f);
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 1);
    CHECK_EQ(f.exists(usbcfg::kPath), 1);
}

void test_damaged_record_is_off() {
    Vol v_f; fs::Fs& f = v_f.f;
    usbcfg::save(f, {true, true});
    auto bad = usbcfg::encode({true, true});
    bad[4] ^= 0x01;                                                // flags no longer match their complement
    f.write_file(usbcfg::kPath, bad);
    CHECK_EQ(usbcfg::any(usbcfg::load(f)), 0);
    Vol v_g; fs::Fs& g = v_g.f;
    usbcfg::save(g, {true, true});
    auto bad2 = usbcfg::encode({true, true});
    bad2[0] = 'X';                                                 // wrong magic
    g.write_file(usbcfg::kPath, bad2);
    CHECK_EQ(usbcfg::any(usbcfg::load(g)), 0);
}

void test_encode_decode() {
    const auto r = usbcfg::encode({true, false});
    CHECK_EQ(r[4], 1); CHECK_EQ(r[5], 0xFE);
    const usbcfg::Setting s = usbcfg::decode(r);
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 0);
}

void test_text() {
    CHECK_STR(usbcfg::describe({false, false}), "off");
    CHECK_STR(usbcfg::describe({true, false}), "HID");
    CHECK_STR(usbcfg::describe({false, true}), "Drive");
    CHECK_STR(usbcfg::describe({true, true}), "HID+Drive");
    CHECK_STR(usbcfg::describe({true, false, true}), "HID+Net");
}

void test_net_mode() {
    const auto r = usbcfg::encode({true, false, true});
    CHECK_EQ(r[4], 5); CHECK_EQ(r[5], 0xFA);
    const usbcfg::Setting s = usbcfg::decode(r);
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 0); CHECK_EQ(s.net, 1);
    // Net without HID is stored as HID+Net (net implies hid).
    const usbcfg::Setting n = usbcfg::normalize({false, false, true});
    CHECK_EQ(n.hid, 1); CHECK_EQ(n.net, 1);
    // Net and Drive do not fit the 512 B packet memory together: invalid, decodes to off.
    auto bad = usbcfg::encode({true, false, true});
    bad[4] = 7; bad[5] = static_cast<std::uint8_t>(~7);
    CHECK_EQ(usbcfg::any(usbcfg::decode(bad)), 0);
    CHECK_EQ(usbcfg::any(usbcfg::normalize({true, true, true})), 0);
    // Flash round trip.
    Vol v_f; fs::Fs& f = v_f.f;
    usbcfg::save(f, {true, false, true});
    CHECK_EQ(usbcfg::load(f).net, 1);
    // Old two-flag records still decode.
    CHECK_EQ(usbcfg::decode(usbcfg::encode({true, true})).drive, 1);
}

// The USB link page flips one row at a time; the result is always a valid mode.
void test_toggle_rows() {
    using usbcfg::Row;
    usbcfg::Setting s{};
    s = usbcfg::toggle(s, Row::Hid);   CHECK_EQ(s.hid, 1);  CHECK_EQ(s.drive, 0); CHECK_EQ(s.net, 0);
    s = usbcfg::toggle(s, Row::Drive); CHECK_EQ(s.hid, 1);  CHECK_EQ(s.drive, 1); CHECK_EQ(s.net, 0);
    s = usbcfg::toggle(s, Row::Net);   // Net needs HID and cannot share the PMA with the Drive
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 0); CHECK_EQ(s.net, 1);
    s = usbcfg::toggle(s, Row::Drive); // Drive on turns Net off
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 1); CHECK_EQ(s.net, 0);
    s = usbcfg::toggle(usbcfg::toggle(s, Row::Drive), Row::Net);
    CHECK_EQ(s.net, 1);
    s = usbcfg::toggle(s, Row::Hid);   // HID off takes Net with it
    CHECK_EQ(s.hid, 0); CHECK_EQ(s.net, 0);
    s = usbcfg::toggle(s, Row::Net);   // from off: Net on brings HID
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.net, 1);
    s = usbcfg::toggle(s, Row::Net);
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.net, 0);
    // Every reachable state is already normalized.
    for (int i = 0; i < 3; i++) {
        usbcfg::Setting t{(i & 1) != 0, (i & 2) != 0, false};
        for (Row r : {Row::Hid, Row::Drive, Row::Net}) {
            const usbcfg::Setting u = usbcfg::toggle(t, r);
            const usbcfg::Setting v = usbcfg::normalize(u);
            CHECK_EQ(u.hid == v.hid && u.drive == v.drive && u.net == v.net, 1);
        }
    }
}

// K2_NET=0 (the default firmware build): the network function does not exist, so a stored request for it
// runs as the rest of the record (HID+Net -> HID), the page has no Network row, and the row cannot be toggled.
// The functions take the build flag as a defaulted argument, so this host build (K2_NET=1) covers the off case too.
void test_net_not_built() {
    using usbcfg::Row;
    const usbcfg::Setting hn = usbcfg::normalize({true, false, true}, false);
    CHECK_EQ(hn.hid, 1); CHECK_EQ(hn.drive, 0); CHECK_EQ(hn.net, 0);
    CHECK_EQ(usbcfg::any(usbcfg::normalize({false, false, true}, false)), 0);   // net alone was never storable
    const usbcfg::Setting hdn = usbcfg::normalize({true, true, true}, false);   // Net+Drive: Drive survives
    CHECK_EQ(hdn.hid, 1); CHECK_EQ(hdn.drive, 1); CHECK_EQ(hdn.net, 0);
    // A stored HID+Net record decodes to HID; the record on flash is left alone (a K2_NET=1 build still reads it).
    const auto rec = usbcfg::encode({true, false, true});
    const usbcfg::Setting d = usbcfg::decode(rec, false);
    CHECK_EQ(d.hid, 1); CHECK_EQ(d.net, 0);
    CHECK_EQ(usbcfg::requests_net(rec), 1);
    CHECK_EQ(usbcfg::requests_net(usbcfg::encode({true, false})), 0);
    auto bad = rec; bad[5] ^= 1;
    CHECK_EQ(usbcfg::requests_net(bad), 0);                                     // a damaged record asks for nothing
    CHECK_EQ(usbcfg::decode(rec, true).net, 1);
    // The Net row does nothing; the other rows behave as before and never leave net set.
    usbcfg::Setting s{true, false};
    const usbcfg::Setting t = usbcfg::toggle(s, Row::Net, false);
    CHECK_EQ(t.hid, 1); CHECK_EQ(t.drive, 0); CHECK_EQ(t.net, 0);
    CHECK_EQ(usbcfg::toggle(usbcfg::Setting{}, Row::Net, false).hid, 0);       // not even HID comes on
    CHECK_EQ(usbcfg::toggle(s, Row::Drive, false).drive, 1);
    CHECK_EQ(usbcfg::rows(false, false), 2);
    CHECK_EQ(usbcfg::rows(true, false), 3);
    // Without Net the shell rides on HID in every HID mode.
    CHECK_EQ(usbcfg::shell_wanted(usbcfg::normalize({true, false, true}, false)), 1);
}

// WebUSB (a vendor bulk interface carrying the remote protocol) replaces the HID remote channel and the Net web path.
// Flag bit 3 in the record; the shell rides on it like on HID; it never shares the 512 B packet memory with HID and
// the Drive together; without K2_WEBUSB the request is dropped and the row does not exist.
void test_webusb_mode() {
    using usbcfg::Row;
    const auto r = usbcfg::encode({false, false, false, false, true});
    CHECK_EQ(r[4], 8); CHECK_EQ(r[5], 0xF7);
    const usbcfg::Setting s = usbcfg::decode(r);
    CHECK_EQ(s.webusb, 1); CHECK_EQ(s.hid, 0); CHECK_EQ(s.drive, 0); CHECK_EQ(s.net, 0);
    CHECK_EQ(usbcfg::any(s), 1);
    CHECK_EQ(usbcfg::valid_record(r), 1);
    // Records with bits above bit 3 are still garbage.
    auto bad = r; bad[4] = 0x18; bad[5] = static_cast<std::uint8_t>(~0x18);
    CHECK_EQ(usbcfg::valid_record(bad), 0);
    // WebUSB+Drive and HID+WebUSB are modes; HID+WebUSB+Drive does not fit (576 B) and WebUSB wins.
    const usbcfg::Setting wd = usbcfg::normalize({false, true, false, false, true});
    CHECK_EQ(wd.webusb, 1); CHECK_EQ(wd.drive, 1); CHECK_EQ(wd.hid, 0);
    const usbcfg::Setting hw = usbcfg::normalize({true, false, false, false, true});
    CHECK_EQ(hw.webusb, 1); CHECK_EQ(hw.hid, 1);
    const usbcfg::Setting all = usbcfg::normalize({true, true, false, false, true});
    CHECK_EQ(all.webusb, 1); CHECK_EQ(all.drive, 1); CHECK_EQ(all.hid, 0);
    // WebUSB replaces the Net web path: asking for both keeps WebUSB and drops Net.
    const usbcfg::Setting wn = usbcfg::normalize({false, false, true, false, true});
    CHECK_EQ(wn.webusb, 1); CHECK_EQ(wn.net, 0);
    // The shell rides on WebUSB and on HID, never on Net.
    CHECK_EQ(usbcfg::shell_wanted({false, false, false, false, true}), 1);
    CHECK_EQ(usbcfg::shell_wanted({false, true, false, false, true}), 1);
    CHECK_EQ(usbcfg::shell_wanted({false, true}), 0);
    CHECK_EQ(usbcfg::with_shell({false, true, false, false, true}).shell, 1);
    // Text.
    CHECK_STR(usbcfg::describe({false, false, false, false, true}), "WebUSB");
    CHECK_STR(usbcfg::describe({false, true, false, false, true}), "WebUSB+Drive");
    CHECK_STR(usbcfg::describe({true, false, false, false, true}), "HID+WebUSB");
    // Not built: the request is dropped, the rest of the record stays.
    const usbcfg::Setting nb = usbcfg::normalize({true, true, false, false, true}, usbcfg::kNetBuilt, false);
    CHECK_EQ(nb.webusb, 0); CHECK_EQ(nb.hid, 1); CHECK_EQ(nb.drive, 1);
    CHECK_EQ(usbcfg::any(usbcfg::decode(r, usbcfg::kNetBuilt, false)), 0);
    CHECK_EQ(usbcfg::requests_webusb(r), 1);
    CHECK_EQ(usbcfg::requests_webusb(usbcfg::encode({true, true})), 0);
    // Rows: HID, Drive, [Net], [WebUSB].
    CHECK_EQ(usbcfg::rows(false, false), 2);
    CHECK_EQ(usbcfg::rows(true, false), 3);
    CHECK_EQ(usbcfg::rows(false, true), 3);
    CHECK_EQ(usbcfg::rows(true, true), 4);
    CHECK_EQ(usbcfg::row_at(0, true, true) == Row::Hid, 1);
    CHECK_EQ(usbcfg::row_at(1, true, true) == Row::Drive, 1);
    CHECK_EQ(usbcfg::row_at(2, true, true) == Row::Net, 1);
    CHECK_EQ(usbcfg::row_at(3, true, true) == Row::Webusb, 1);
    CHECK_EQ(usbcfg::row_at(2, false, true) == Row::Webusb, 1);
    CHECK_EQ(usbcfg::row_at(2, true, false) == Row::Net, 1);
    // Flash round trip.
    Vol v_f; fs::Fs& f = v_f.f;
    usbcfg::save(f, {false, true, false, false, true});
    const usbcfg::Setting l = usbcfg::load(f);
    CHECK_EQ(l.webusb, 1); CHECK_EQ(l.drive, 1); CHECK_EQ(l.hid, 0);
}

void test_webusb_toggle() {
    using usbcfg::Row;
    usbcfg::Setting s{};
    s = usbcfg::toggle(s, Row::Webusb);                 // from off: WebUSB alone, no HID
    CHECK_EQ(s.webusb, 1); CHECK_EQ(s.hid, 0);
    s = usbcfg::toggle(s, Row::Drive);                  // WebUSB + Drive
    CHECK_EQ(s.webusb, 1); CHECK_EQ(s.drive, 1); CHECK_EQ(s.hid, 0);
    s = usbcfg::toggle(s, Row::Hid);                    // HID on beside Drive: the fallback replaces WebUSB
    CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 1); CHECK_EQ(s.webusb, 0);
    s = usbcfg::toggle(s, Row::Webusb);                 // WebUSB on beside HID+Drive: HID goes
    CHECK_EQ(s.webusb, 1); CHECK_EQ(s.drive, 1); CHECK_EQ(s.hid, 0);
    s = usbcfg::toggle(s, Row::Drive);                  // Drive off: HID can join
    s = usbcfg::toggle(s, Row::Hid);
    CHECK_EQ(s.webusb, 1); CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 0);
    s = usbcfg::toggle(s, Row::Drive);                  // Drive on with HID+WebUSB: HID goes
    CHECK_EQ(s.webusb, 1); CHECK_EQ(s.hid, 0); CHECK_EQ(s.drive, 1);
    s = usbcfg::toggle(s, Row::Net);                    // Net replaces WebUSB and drops the Drive
    CHECK_EQ(s.net, 1); CHECK_EQ(s.hid, 1); CHECK_EQ(s.drive, 0); CHECK_EQ(s.webusb, 0);
    s = usbcfg::toggle(s, Row::Webusb);                 // WebUSB on turns Net off
    CHECK_EQ(s.webusb, 1); CHECK_EQ(s.net, 0);
    // Not built: the row does nothing.
    CHECK_EQ(usbcfg::any(usbcfg::toggle(usbcfg::Setting{}, Row::Webusb, usbcfg::kNetBuilt, false)), 0);
    // Every reachable state is already normalized.
    for (int i = 0; i < 16; i++) {
        usbcfg::Setting t{(i & 1) != 0, (i & 2) != 0, (i & 4) != 0, false, (i & 8) != 0};
        t = usbcfg::normalize(t);
        for (Row r : {Row::Hid, Row::Drive, Row::Net, Row::Webusb}) {
            const usbcfg::Setting u = usbcfg::toggle(t, r);
            const usbcfg::Setting v = usbcfg::normalize(u);
            CHECK_EQ(u.hid == v.hid && u.drive == v.drive && u.net == v.net && u.webusb == v.webusb, 1);
        }
    }
}
}  // namespace

int main() {
    test_webusb_mode();
    test_webusb_toggle();
    test_net_not_built();
    test_blank_flash_is_off();
    test_save_and_reload();
    test_damaged_record_is_off();
    test_encode_decode();
    test_text();
    test_net_mode();
    test_toggle_rows();
    return test_report("test_usbcfg");
}
