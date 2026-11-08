// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Compact black-box views: event rows with deltas, the VBUS strip, trigger time.
#include <cstring>
#include <string>
#include <vector>

#include "k2/pd/pdtrace.hpp"
#include "k2/pd/pdtrace_view.hpp"
#include "test_util.h"

using namespace k2::pdtrace;
namespace v = k2::pdtrace::view;

namespace {
using Buf = std::vector<std::uint8_t>;
void add(Buf& b, Type t, std::uint32_t ts, std::vector<std::uint8_t> p) {
    b.push_back(std::uint8_t(6 + p.size()));
    b.push_back(std::uint8_t(t));
    for (int i = 0; i < 4; i++) b.push_back(std::uint8_t(ts >> (8 * i)));
    b.insert(b.end(), p.begin(), p.end());
}
void rx(Buf& b, std::uint32_t ts, unsigned hdr) { add(b, Type::Rx, ts, {0xE0, std::uint8_t(hdr), std::uint8_t(hdr >> 8), 0, 0, 0, 0}); }
void tx(Buf& b, std::uint32_t ts, unsigned hdr) { add(b, Type::Tx, ts, {std::uint8_t(hdr), std::uint8_t(hdr >> 8), 1, 2, 3, 4}); }
std::string text(const v::Row& r) {
    char o[32];
    return std::string(o, v::format_row(r, o));
}
}  // namespace

int main() {
    Buf b;
    rx(b, 1000, 0x0041);                                   // GoodCRC: no row
    rx(b, 2000, 0x1181);                                   // Source_Capabilities, 1 object
    tx(b, 11800, 0x1282);                                  // Request 9.8 ms later
    add(b, Type::TxResult, 12500, {1, 5, 0x10});           // RETRYFAIL
    add(b, Type::Reset, 512500, {0});                      // hard reset received
    add(b, Type::Reset, 512600, {3});                      // soft reset sent: no row
    add(b, Type::Vbus, 513000, {0x88, 0x13, 0, 0});        // not a row
    add(b, Type::Cc, 2512500, {0, 0, 0});                  // detach
    CHECK_EQ(v::count_rows(b), 5u);

    v::Row r;
    CHECK_EQ(v::row_at(b, 0, r), 1);
    CHECK_STR(r.name, "SrcCap"); CHECK_EQ(r.dir, '<'); CHECK_EQ(r.dt_us, 0u);
    CHECK_STR(text(r).c_str(), ("<SrcCap     " + std::string(9, ' ')).c_str());
    CHECK_EQ(v::row_at(b, 1, r), 1);
    CHECK_STR(r.name, "Request"); CHECK_EQ(r.dir, '>'); CHECK_EQ(r.dt_us, 9800u);
    CHECK_STR(text(r).c_str(), ">Request       +9.8ms");
    CHECK_EQ(v::row_at(b, 2, r), 1);
    CHECK_STR(r.name, "TX nocrc"); CHECK_EQ(r.dir, '!'); CHECK_EQ(r.dt_us, 700u);
    CHECK_STR(text(r).c_str(), "!TX nocrc      +0.7ms");
    CHECK_EQ(v::row_at(b, 3, r), 1);
    CHECK_STR(r.name, "HardRst rx"); CHECK_EQ(r.dt_us, 500000u);
    CHECK_STR(text(r).c_str(), "!HardRst rx    +500ms");
    CHECK_EQ(v::row_at(b, 4, r), 1);
    CHECK_STR(r.name, "DETACH"); CHECK_EQ(r.dt_us, 2000000u);
    CHECK_STR(text(r).c_str(), "!DETACH        +2.00s");
    CHECK_EQ(v::row_at(b, 5, r), 0);

    {   // a truncated tail (a capture cut mid-record) is ignored, not read past
        Buf t = b;
        t.resize(t.size() - 3);
        CHECK_EQ(v::count_rows(t), 4u);
    }
    {   // unknown / extended messages
        Buf e;
        rx(e, 0, 0x8001);
        rx(e, 10, 0x1002);               // Data type 2 = Request
        v::Row q;
        CHECK_EQ(v::row_at(e, 0, q), 1); CHECK_STR(q.name, "Ext");
        CHECK_EQ(v::row_at(e, 1, q), 1); CHECK_STR(q.name, "Request");
    }
    {   // strip: heights scale between the observed min and max, held between samples
        Buf s;
        add(s, Type::Vbus, 0, {0x88, 0x13, 0, 0});        // 5000 mV
        add(s, Type::Vbus, 1000, {0x28, 0x23, 0, 0});     // 9000 mV
        std::uint8_t h[5];
        const auto st = v::vbus_strip(s, h, 10);
        CHECK_EQ(st.valid, 1); CHECK_EQ(st.mv_min, 5000); CHECK_EQ(st.mv_max, 9000);
        const std::uint8_t want[5] = {0, 0, 0, 0, 10};
        CHECK_EQ(std::memcmp(h, want, 5), 0);
        CHECK_EQ(v::column_of(st, 500, 5), 2u);
        CHECK_EQ(v::column_of(st, 5000, 5), 4u);
        // flat trace: range widened to 1000 mV around the value, drawn mid-height
        Buf f;
        add(f, Type::Vbus, 0, {0x88, 0x13, 0, 0});
        add(f, Type::Vbus, 100, {0x88, 0x13, 0, 0});
        const auto sf = v::vbus_strip(f, h, 10);
        CHECK_EQ(sf.mv_min, 4500); CHECK_EQ(sf.mv_max, 5500); CHECK_EQ(h[0], 5);
        // no samples at all
        Buf n;
        rx(n, 5, 0x1181);
        CHECK_EQ(v::vbus_strip(n, h, 10).valid, 0);
    }
    {   // strip: timestamps out of order (restored or edge-stamped capture) stay inside the strip
        Buf s;
        add(s, Type::Vbus, 100, {0x88, 0x13, 0, 0});      // 5000 mV
        add(s, Type::Vbus, 5000, {0x28, 0x23, 0, 0});     // later than the last record
        add(s, Type::Vbus, 50, {0x28, 0x23, 0, 0});       // earlier than the first record
        add(s, Type::Rx, 100, {0xE0, 0x81, 0x11, 0, 0, 0, 0});
        std::uint8_t guard[16];
        std::memset(guard, 0xAA, sizeof guard);
        std::uint8_t* h = guard + 4;                      // 8 columns, 4 guard bytes each side
        const auto st = v::vbus_strip(s, std::span<std::uint8_t>(h, 8), 10);
        CHECK_EQ(st.valid, 1);
        for (int i = 0; i < 4; i++) { CHECK_EQ(guard[i], 0xAA); CHECK_EQ(guard[12 + i], 0xAA); }
    }
    {   // time_at: timestamp of the last record that is wholly inside the byte count
        CHECK_EQ(v::time_at(b, b[0] + b[b[0]]), 2000u);
        CHECK_EQ(v::time_at(b, 5), 0u);
    }
    {   // json_row: fixed-width lines, one per record, blank past the end
        std::array<char, v::kJsonLine> ln;
        v::json_row(b, 0, ln);
        CHECK_EQ(ln[v::kJsonLine - 1], '\n');
        CHECK_EQ(std::memcmp(ln.data(), "{\"t\":", 5), 0);
        CHECK_EQ(std::strstr(ln.data(), "\"y\":") != nullptr, 1);
        const std::size_t n = v::count_rows(b);
        v::json_row(b, 1000, ln);
        CHECK_EQ(ln[0], ' ');
        CHECK_EQ(ln[v::kJsonLine - 1], '\n');
        v::json_row({}, 0, ln);
        CHECK_EQ(ln[0], ' ');
        CHECK_EQ(n > 0, 1);
    }
    {   // Capture summary: a steady "#seq duration messages", never a relative age that ticks
        auto dur = [](std::uint32_t us) {
            char o[16];
            return std::string(o, v::format_duration(us, o));
        };
        CHECK_STR(dur(0).c_str(), "0ms");
        CHECK_STR(dur(850000).c_str(), "850ms");
        CHECK_STR(dur(2511500).c_str(), "2.5s");
        CHECK_STR(dur(12000000).c_str(), "12s");
        CHECK_STR(dur(75000000).c_str(), "1m15s");
        char o[24];
        CHECK_STR(std::string(o, v::format_capture(b, 7, o)).c_str(), "#7 2.5s 5 msg");
        CHECK_STR(std::string(o, v::format_capture({}, 12, o)).c_str(), "#12 0ms 0 msg");
        CHECK_STR(std::string(o, v::format_capture_long(b, 7, o)).c_str(), "#7  2.5s  5 msgs");
        CHECK_STR(std::string(o, v::format_capture_long({}, 1, o)).c_str(), "#1  0ms  0 msgs");
    }
    return test_report("pdtrace_view");
}
