// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The WebUSB BOS descriptor, the landing-page URL descriptor and the Microsoft OS 2.0 set (k2/drivers/usb_webusb.hpp):
// every length field agrees with the bytes, the capabilities announce the vendor codes control_reply() answers, and the
// OS 2.0 set binds the right interface to WinUSB.
#include <cstring>
#include <string_view>

#include "k2/drivers/usb_webusb.hpp"
#include "test_util.h"

using namespace k2::webusb;

namespace {

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

void test_bos() {
    const auto& b = kBos;
    CHECK_EQ(b.size(), 57u);
    CHECK_EQ(b[0], 5); CHECK_EQ(b[1], 0x0F); CHECK_EQ(le16(&b[2]), 57); CHECK_EQ(b[4], 2);   // two capabilities
    // WebUSB platform capability: 24 bytes, device capability (0x10), platform (5), the WebUSB UUID, version 1.0.
    const std::uint8_t* w = &b[5];
    CHECK_EQ(w[0], 24); CHECK_EQ(w[1], 0x10); CHECK_EQ(w[2], 5); CHECK_EQ(w[3], 0);
    static const std::uint8_t uuid_w[16] = {0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47, 0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65};
    CHECK_EQ(std::memcmp(w + 4, uuid_w, 16), 0);
    CHECK_EQ(le16(w + 20), 0x0100);
    CHECK_EQ(w[22], kReqWebusb); CHECK_EQ(w[23], kLandingIndex);
    // Microsoft OS 2.0 capability: 28 bytes, Windows 8.1+, set length and vendor code.
    const std::uint8_t* m = &b[5 + 24];
    CHECK_EQ(m[0], 28); CHECK_EQ(m[1], 0x10); CHECK_EQ(m[2], 5);
    static const std::uint8_t uuid_m[16] = {0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C, 0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F};
    CHECK_EQ(std::memcmp(m + 4, uuid_m, 16), 0);
    CHECK_EQ(m[20], 0x00); CHECK_EQ(m[21], 0x00); CHECK_EQ(m[22], 0x03); CHECK_EQ(m[23], 0x06);   // 0x06030000 little endian
    CHECK_EQ(le16(m + 24), kMsosSetLen);
    CHECK_EQ(m[26], kReqMsos); CHECK_EQ(m[27], 0);
    // Walking the capabilities by their own lengths lands exactly on the end.
    unsigned off = 5;
    int caps = 0;
    while (off < b.size()) { off += b[off]; caps++; }
    CHECK_EQ(off, b.size()); CHECK_EQ(caps, 2);
}

void test_url() {
    const auto& u = kUrl;
    CHECK_EQ(u[0], u.size()); CHECK_EQ(u[1], 3); CHECK_EQ(u[2], 1);     // https
    CHECK_EQ(std::memcmp(&u[3], kLandingUrl.data(), kLandingUrl.size()), 0);
    CHECK_EQ(u.size() <= 255, 1);
    // The URL has no scheme in it: the scheme byte carries it.
    CHECK_EQ(kLandingUrl.find("://"), std::string_view::npos);
    // The published dashboard (GitHub Pages site root + k2.html), spelled out independently of the constant: the descriptor
    // is 3 + 43 = 46 bytes and carries exactly these characters after the scheme byte.
    static const char kPage[] = "yarost12.github.io/open-k2-firmware/k2.html";
    CHECK_EQ(sizeof(kPage) - 1, 43u);
    CHECK_EQ(u.size(), 46u); CHECK_EQ(u[0], 46);
    CHECK_EQ(std::memcmp(&u[3], kPage, 43), 0);
    CHECK_EQ(u[3], 'y'); CHECK_EQ(u[45], 'l');
    CHECK_EQ(kLandingUrl == std::string_view(kPage), 1);
}

// Walks the OS 2.0 set: every descriptor's wLength is in range and the subsets nest exactly.
void test_msos(std::uint8_t itf) {
    const auto& s = itf == 0 ? kMsos0 : kMsos1;
    CHECK_EQ(le16(&s[0]), 10); CHECK_EQ(le16(&s[2]), 0); CHECK_EQ(le16(&s[8]), s.size());
    CHECK_EQ(s[4], 0x00); CHECK_EQ(s[5], 0x00); CHECK_EQ(s[6], 0x03); CHECK_EQ(s[7], 0x06);
    unsigned off = 10;
    // configuration subset
    CHECK_EQ(le16(&s[off]), 8); CHECK_EQ(le16(&s[off + 2]), 1);
    CHECK_EQ(off + le16(&s[off + 6]), s.size());                  // the subset ends where the set ends
    const unsigned func = off + 8;
    CHECK_EQ(le16(&s[func]), 8); CHECK_EQ(le16(&s[func + 2]), 2);
    CHECK_EQ(s[func + 4], itf);                                   // bound to the vendor interface
    CHECK_EQ(func + le16(&s[func + 6]), s.size());
    const unsigned compat = func + 8;
    CHECK_EQ(le16(&s[compat]), 20); CHECK_EQ(le16(&s[compat + 2]), 3);
    CHECK_EQ(std::memcmp(&s[compat + 4], "WINUSB\0\0", 8), 0);
    for (int i = 12; i < 20; i++) CHECK_EQ(s[compat + i], 0);     // no sub-compatible ID
    const unsigned prop = compat + 20;
    CHECK_EQ(prop + le16(&s[prop]), s.size());
    CHECK_EQ(le16(&s[prop + 2]), 4);                              // registry property
    CHECK_EQ(le16(&s[prop + 4]), 7);                              // REG_MULTI_SZ
    const unsigned nlen = le16(&s[prop + 6]);
    CHECK_EQ(nlen, 42u);                                          // "DeviceInterfaceGUIDs" + NUL in UTF-16
    for (unsigned i = 0; i + 2 < nlen; i += 2) {
        CHECK_EQ(s[prop + 8 + i], static_cast<std::uint8_t>(kGuidPropName[i / 2]));
        CHECK_EQ(s[prop + 8 + i + 1], 0);
    }
    const unsigned dlen_at = prop + 8 + nlen;
    const unsigned dlen = le16(&s[dlen_at]);
    CHECK_EQ(dlen, (kInterfaceGuid.size() + 1) * 2 + 2);
    CHECK_EQ(dlen_at + 2 + dlen, s.size());
    for (unsigned i = 0; i < kInterfaceGuid.size(); i++) CHECK_EQ(s[dlen_at + 2 + 2 * i], static_cast<std::uint8_t>(kInterfaceGuid[i]));
    CHECK_EQ(s[s.size() - 1], 0); CHECK_EQ(s[s.size() - 2], 0); CHECK_EQ(s[s.size() - 3], 0); CHECK_EQ(s[s.size() - 4], 0);
    // The GUID is braced and 36 characters inside.
    CHECK_EQ(kInterfaceGuid.size(), 38u); CHECK_EQ(kInterfaceGuid[0], '{'); CHECK_EQ(kInterfaceGuid[37], '}');
    // A random (version 4) GUID, not the old placeholder: the version nibble is 4, the variant nibble 8..b.
    CHECK_EQ(kInterfaceGuid == std::string_view("{81bdae64-520b-4114-aba4-de8eedd252bf}"), 1);
    CHECK_EQ(kInterfaceGuid[15], '4'); CHECK_EQ(std::string_view("89ab").find(kInterfaceGuid[20]) != std::string_view::npos, 1);
    CHECK_EQ(kInterfaceGuid.find("5b0d3d6e"), std::string_view::npos);
}

void test_control_reply() {
    Reply r = control_reply(kReqWebusb, kLandingIndex, kIndexGetUrl, 0);
    CHECK_EQ(r.data == kUrl.data(), 1); CHECK_EQ(r.len, kUrl.size());
    r = control_reply(kReqWebusb, 2, kIndexGetUrl, 0);               // no such landing page
    CHECK_EQ(r.data == nullptr, 1);
    r = control_reply(kReqWebusb, kLandingIndex, 3, 0);              // not GET_URL
    CHECK_EQ(r.data == nullptr, 1);
    r = control_reply(kReqMsos, 0, kIndexMsos, 0);
    CHECK_EQ(r.data == kMsos0.data(), 1); CHECK_EQ(r.len, kMsos0.size());
    r = control_reply(kReqMsos, 0, kIndexMsos, 1);                   // HID first: the vendor interface is 1
    CHECK_EQ(r.data == kMsos1.data(), 1);
    r = control_reply(kReqMsos, 0, 8, 0);
    CHECK_EQ(r.data == nullptr, 1);
    r = control_reply(0x55, 0, 0, 0);
    CHECK_EQ(r.data == nullptr, 1);
    CHECK_EQ(kReqWebusb != kReqMsos, 1);
}

}  // namespace

int main() {
    test_bos();
    test_url();
    test_msos(0);
    test_msos(1);
    test_control_reply();
    return test_report("test_usb_webusb");
}
