// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The JSON of a saved cable (docs/EMARKER_JSON.md): exact text for a known cable, the escape of
// the user name, and that a too-small buffer is cut off without overrunning.
#include <array>
#include <cstring>
#include <string>

#include "k2/pd/emjson.hpp"
#include "test_util.h"

using namespace k2;

static emlib::Entry entry(const char* name) {
    emlib::Entry e;
    e.seq = 3;
    e.version = 2;
    std::strncpy(e.name.data(), name, emlib::kNameMax);
    // passive cable, Apple VID, cert 0x1234, PID 0x1234 hw 1 fw 1; cable VDO: USB4 Gen3, 5 A, 50 V, latency code 2
    const std::array<std::uint32_t, 5> v = {0xFF00A041, 0x180005AC, 0x00001234, 0x12340101, 0x00004643};
    e.rec = emrec::make(v, 2, "Apple-USB4G3-240W");
    return e;
}

static void test_exact() {
    std::array<char, 2048> buf;
    const std::size_t n = emjson::render(entry("Desk USB4"), buf);
    const std::string got(buf.data(), n);
    const std::string want =
        "{\n"
        "  \"format\": \"k2-emarker\",\n"
        "  \"version\": 1,\n"
        "  \"id\": 3,\n"
        "  \"revision\": 2,\n"
        "  \"name\": \"Desk USB4\",\n"
        "  \"auto_name\": \"Apple-USB4G3-240W\",\n"
        "  \"record\": \"" + [] {
            const emlib::Entry e = entry("x");
            static const char* hex = "0123456789ABCDEF";
            std::string h;
            for (std::uint8_t b : e.rec) { h += hex[b >> 4]; h += hex[b & 15]; }
            return h;
        }() + "\",\n"
        "  \"cable\": {\n"
        "    \"vid\": \"0x05AC\",\n"
        "    \"vendor\": \"Apple\",\n"
        "    \"pid\": \"0x1234\",\n"
        "    \"xid\": \"0x00001234\",\n"
        "    \"type\": \"passive\",\n"
        "    \"speed\": \"USB4 Gen3 (40G)\",\n"
        "    \"current_a\": 5,\n"
        "    \"max_v\": 50,\n"
        "    \"max_w\": 240,\n"
        "    \"latency\": \"10-20 ns (~2 m)\",\n"
        "    \"vconn_required\": false,\n"
        "    \"epr\": true,\n"
        "    \"pd_rev\": 2,\n"
        "    \"vdos\": [\"0xFF00A041\", \"0x180005AC\", \"0x00001234\", \"0x12340101\", \"0x00004643\"]\n"
        "  }\n"
        "}\n";
    CHECK_STR(got.c_str(), want.c_str());
}

static void test_cut() {
    std::array<char, 64> small;
    small.fill('#');
    const std::size_t n = emjson::render(entry("Desk USB4"), small);
    CHECK_EQ(n < small.size(), 1);
    CHECK_EQ(std::strlen(small.data()), n);                    // NUL terminated inside the buffer
}

int main() {
    test_exact();
    test_cut();
    return test_report("test_emjson");
}
