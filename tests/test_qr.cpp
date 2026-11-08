// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR wrapper (k2/ui/qr.hpp over third_party/qrcodegen): fit limits, level, and a layout check through an
// independent minimal reader (tests/qr_decode.hpp).
#include <cstdio>
#include <string>

#include "k2/ui/qr.hpp"
#include "qr_decode.hpp"
#include "test_util.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;

static qrdec::Result read_back(const qr::Workspace& w, int n) {
    return qrdec::decode(n, [&](int x, int y) { return qr::dark(w, x, y); });
}

static std::string payload(std::size_t len) {
    std::string s;
    for (std::size_t i = 0; i < len; i++) s.push_back(static_cast<char>('a' + i * 7 % 26));
    return s;
}

int main() {
    static qr::Workspace w;

    // A short text: version 1, level L, decodes back to itself.
    {
        const int n = qr::encode("K2B1 hi", w);
        CHECK_EQ(n, 21);
        const auto r = read_back(w, n);
        CHECK(r.ok);
        CHECK_EQ(r.version, 1);
        CHECK_EQ(r.ecl, 0);
        CHECK(r.text == "K2B1 hi");
    }
    // Every length up to the version-10 byte-mode capacity of level L (271) round-trips; versions change on the way.
    int last_version = 0;
    int versions_seen = 0;
    for (std::size_t len = 1; len <= qr::kMaxBytes; len += (len < 40 ? 1 : 13)) {
        const std::string s = payload(len);
        const int n = qr::encode(s, w);
        CHECK(n >= 21);
        CHECK(n <= qr::kMaxModules);
        const auto r = read_back(w, n);
        CHECK(r.ok);
        CHECK(r.text == s);
        CHECK_EQ(r.ecl, 0);
        if (r.version != last_version) { last_version = r.version; versions_seen++; }
    }
    CHECK(versions_seen >= 8);
    // Exactly the capacity fits, one more does not (no silent truncation).
    {
        const std::string s = payload(qr::kMaxBytes);
        const int n = qr::encode(s, w);
        CHECK_EQ(n, qr::kMaxModules);
        CHECK(read_back(w, n).text == s);
        CHECK_EQ(qr::encode(payload(qr::kMaxBytes + 1), w), 0);
        CHECK_EQ(qr::encode("", w), 0);
    }
    // Alphanumeric mode: an upper-case text of N characters picks the alphanumeric segment, reads back, and the capacity
    // per version (level L) is exactly the standard's table; a one-character-longer text needs the next version.
    {
        static const int cap[13] = {0, 25, 47, 77, 114, 154, 195, 224, 279, 335, 395, 468, 535};   // alphanumeric, level L
        static const char kAl[] = "K2:EMK1 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ$%*+-./";
        for (int v = 1; v <= 12; v++) {
            for (int len : {cap[v] - 1, cap[v]}) {
                std::string s;
                for (int i = 0; i < len; i++) s.push_back(kAl[(i * 5 + i / 7) % (sizeof kAl - 1)]);
                const int n = qr::encode_text(s.c_str(), w);
                CHECK_EQ(n, 4 * v + 17);
                const auto r = read_back(w, n);
                CHECK(r.ok);
                CHECK(r.text == s);
                CHECK_EQ(r.version, v);
            }
            if (v < 12) {
                std::string s(static_cast<std::size_t>(cap[v] + 1), 'A');
                CHECK_EQ(qr::encode_text(s.c_str(), w), 4 * (v + 1) + 17);
            }
        }
        CHECK_EQ(qr::encode_text(std::string(536, 'A').c_str(), w), 0);          // past version 12
        CHECK_EQ(qr::encode_text(std::string(396, 'A').c_str(), w, 10), 0);      // past a lower cap
        CHECK_EQ(qr::encode_text(std::string(395, 'A').c_str(), w, 10), 57);
        CHECK_EQ(qr::encode_text("", w), 0);
        CHECK_EQ(qr::encode_text(nullptr, w), 0);
        CHECK_EQ(qr::encode_text("lower case is byte mode", w), 25);             // not in the alphanumeric set: byte segment
    }
    // The finder pattern sits at the three corners (a reader's anchor).
    {
        const int n = qr::encode("anchor", w);
        for (int i = 0; i < 7; i++) {
            CHECK(qr::dark(w, i, 0) && qr::dark(w, 0, i));
            CHECK(qr::dark(w, n - 1 - i, 0) && qr::dark(w, n - 1, i));
            CHECK(qr::dark(w, i, n - 1) && qr::dark(w, 0, n - 1 - i));
        }
        CHECK(!qr::dark(w, -1, 0) && !qr::dark(w, n, 0));   // outside reads light
    }
    return test_report("test_qr");
}
