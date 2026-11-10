// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Protocol lab grammar (k2/core/lab.hpp): verbs, positionals, options, ranges and the confirm rule.
#include <string>
#include <vector>

#include "k2/core/lab.hpp"
#include "test_util.h"

using namespace k2;

namespace {
std::vector<std::string_view> split(const char* s) {
    std::vector<std::string_view> v;
    std::string_view r = s;
    while (!r.empty()) {
        const auto e = r.find(' ');
        v.push_back(r.substr(0, e));
        if (e == std::string_view::npos) break;
        r.remove_prefix(e + 1);
    }
    return v;
}
// nullptr = ok
const char* P(const char* line, lab::Cmd& c) {
    const auto t = split(line);
    return lab::parse(t.data(), t.size(), c);
}
bool ok(const char* line) { lab::Cmd c; return P(line, c) == nullptr; }
}  // namespace

int main() {
    lab::Cmd c;
    // accp
    CHECK_EQ(P("accp ping", c) == nullptr && c.id == lab::Id::AccpPing, true);
    CHECK_EQ(P("accp read 0x80", c) == nullptr && c.id == lab::Id::AccpRead && c.a[0] == 0x80 && c.na == 1, true);
    CHECK_EQ(P("accp read 128 4 --retries 5", c) == nullptr && c.a[1] == 4 && c.o.retries == 5, true);
    CHECK_EQ(ok("accp read 0x100"), false);                        // byte range
    CHECK_EQ(ok("accp write 0xA0 0xC0"), false);                   // writes need confirm
    CHECK_EQ(P("accp write 0xA0 0xC0 confirm", c) == nullptr && c.a[0] == 0xA0 && c.a[1] == 0xC0 && c.o.confirm, true);
    CHECK_EQ(P("accp write 160 192 --confirm", c) == nullptr && c.o.confirm, true);
    CHECK_EQ(P("accp dump 0x00 0xFF --json --log raw", c) == nullptr && c.o.json && c.o.raw_log, true);
    CHECK_EQ(ok("accp dump 0x80 0x10"), false);                    // from > to
    CHECK_EQ(ok("accp read"), false);
    CHECK_EQ(ok("accp ping 1"), false);                            // too many positionals
    CHECK_EQ(ok("accp ping --bogus 1"), false);
    CHECK_EQ(ok("accp ping --retries"), false);                    // option without value
    CHECK_EQ(ok("accp ping --retries 99"), false);                 // out of range
    // qc
    CHECK_EQ(ok("qc2 5000"), true);
    CHECK_EQ(ok("qc2 9000"), false);
    CHECK_EQ(ok("qc2 9000 confirm"), true);
    CHECK_EQ(ok("qc2 36001 confirm"), false);
    CHECK_EQ(ok("qc3 set 9000 confirm"), true);
    CHECK_EQ(ok("qc3 up confirm"), true);
    CHECK_EQ(ok("qc3 up"), false);
    CHECK_EQ(P("qc3 down 5 --pulse-us 2250 --gap-us 1000 confirm", c) == nullptr && c.id == lab::Id::Qc3Down &&
              c.a[0] == 5 && c.o.pulse_us == 2250 && c.o.gap_us == 1000, true);
    CHECK_EQ(ok("qc3 down 5 --pulse-us 5 confirm"), false);        // below 20 us
    CHECK_EQ(P("qc3+ up 20 confirm", c) == nullptr && c.id == lab::Id::Qc3pUp, true);
    // protocols that use the engines
    CHECK_EQ(ok("afc 9000 1670 confirm"), true);
    CHECK_EQ(ok("afc 9000"), false);
    CHECK_EQ(ok("fcp 5000"), true);
    CHECK_EQ(ok("scp 9000 3000"), false);
    CHECK_EQ(ok("scp 5000 3000"), true);
    CHECK_EQ(ok("scp 9000 3000 --confirm --hold"), true);
    CHECK_EQ(ok("vooc wake"), false);
    CHECK_EQ(ok("vooc wake confirm"), true);
    CHECK_EQ(ok("vooc listen --timeout-ms 500"), true);
    CHECK_EQ(ok("vooc answer 0xA8 0x0C confirm"), true);
    CHECK_EQ(ok("vooc sniff"), true);
    CHECK_EQ(ok("ufcs ping"), true);
    CHECK_EQ(ok("ufcs request 9000 2000"), false);
    CHECK_EQ(P("ufcs raw 0A1B2C confirm", c) == nullptr && c.nbytes == 3 && c.bytes[0] == 0x0A && c.bytes[2] == 0x2C, true);
    CHECK_EQ(ok("ufcs raw 0A1 confirm"), false);                   // odd hex
    CHECK_EQ(ok("ufcs raw zz confirm"), false);
    CHECK_EQ(ok("vivo request 9000 2000 confirm"), true);
    // dpdm / vbus / cc
    CHECK_EQ(P("dpdm 0 hiz", c) == nullptr && c.id == lab::Id::Dpdm && c.a[0] == 1 && c.a[1] == 0, true);
    CHECK_EQ(ok("dpdm 0.6 0"), false);
    CHECK_EQ(P("dpdm 0.6 3.3 --hold-ms 100 confirm", c) == nullptr && c.o.hold_ms == 100, true);
    CHECK_EQ(ok("dpdm 1.0 0"), false);
    CHECK_EQ(P("vbus --watch 500 --json", c) == nullptr && c.id == lab::Id::Vbus && c.o.watch_ms == 500, true);
    CHECK_EQ(ok("cc rp 100"), true);
    CHECK_EQ(ok("cc rd"), true);
    // pd
    CHECK_EQ(ok("pd caps"), true);
    CHECK_EQ(ok("pd request 2 9000 2000"), false);
    CHECK_EQ(ok("pd request 1"), true);                            // position 1 is vSafe5V
    CHECK_EQ(ok("pd request 2"), false);                           // an unknown PDO may be high
    CHECK_EQ(ok("pd request 2 9000 2000 confirm --sop 0"), true);
    CHECK_EQ(ok("pd pps 9000 2000"), false);
    CHECK_EQ(ok("pd pps 5000 2000"), true);
    CHECK_EQ(ok("pd avs 20000 3000 confirm"), true);
    CHECK_EQ(P("pd vdm discover-identity --sop 1", c) == nullptr && c.word == "discover-identity" && c.o.sop == 1, true);
    CHECK_EQ(ok("pd vdm svids --sop 3"), false);
    CHECK_EQ(P("pd get source-cap", c) == nullptr && c.word == "source-cap", true);
    CHECK_EQ(P("pd reread", c) == nullptr && c.id == lab::Id::PdReread && c.na == 0, true);   // no mask: the firmware default
    CHECK_EQ(P("pd reread 8", c) == nullptr && c.a[0] == 8 && c.na == 1, true);
    CHECK_EQ(P("pd reread 0x1F", c) == nullptr && c.a[0] == 31, true);
    CHECK_EQ(ok("pd reread 0"), false);                            // nothing to do
    CHECK_EQ(ok("pd reread 32"), false);                           // only five steps
    CHECK_EQ(ok("pd reread 1 2"), false);
    CHECK_STR(std::string(lab::name(lab::Id::PdReread)).c_str(), "pd.reread");
    CHECK_EQ(ok("pd hardreset"), true);
    CHECK_EQ(ok("pd softreset"), true);
    CHECK_EQ(ok("pd raw 1161 confirm"), true);
    CHECK_EQ(ok("pd raw 1161"), false);
    CHECK_EQ(ok("pd epr 28000 5000 confirm"), true);
    // ufcs tuner and probe: RAM only, never raises VBUS, so no confirm
    CHECK_EQ(P("ufcs set version 2", c) == nullptr && c.id == lab::Id::UfcsSet && c.a[0] == 2 && c.a[1] == 1 && c.word == "version", true);
    CHECK_EQ(P("ufcs set baud 57600", c) == nullptr && c.a[0] == 57600 && c.word == "baud", true);   // beyond the 36000 mV number cap
    CHECK_EQ(P("ufcs set crcpoly 0x1D", c) == nullptr && c.a[0] == 0x1D, true);
    CHECK_EQ(P("ufcs set opener 0", c) == nullptr && c.a[0] == 0, true);
    CHECK_EQ(ok("ufcs set nope 1"), false);                        // unknown field
    CHECK_EQ(ok("ufcs set version 64"), false);                    // 6-bit field
    CHECK_EQ(ok("ufcs set stop 3"), false);
    CHECK_EQ(ok("ufcs set baud 1199"), false);
    CHECK_EQ(ok("ufcs set delay 201"), false);
    CHECK_EQ(ok("ufcs set version"), false);                       // value missing
    CHECK_EQ(ok("ufcs set"), false);
    CHECK_EQ(ok("ufcs set version x"), false);
    CHECK_EQ(ok("ufcs set version 1 2"), false);
    CHECK_EQ(P("ufcs show", c) == nullptr && c.id == lab::Id::UfcsShow, true);
    CHECK_EQ(P("ufcs reset", c) == nullptr && c.id == lab::Id::UfcsReset, true);
    CHECK_EQ(P("ufcs probe", c) == nullptr && c.id == lab::Id::UfcsProbe && !c.o.confirm, true);
    CHECK_EQ(P("ufcs result", c) == nullptr && c.id == lab::Id::UfcsResult, true);
    CHECK_EQ(ok("ufcs probe 5000"), false);
    CHECK_STR(std::string(lab::name(lab::Id::UfcsSet)).c_str(), "ufcs.set");
    CHECK_STR(std::string(lab::name(lab::Id::UfcsProbe)).c_str(), "ufcs.probe");
    // lcd: the SPI clock switch (4 or 8, never raises VBUS, no confirm)
    CHECK_EQ(P("lcd show", c) == nullptr && c.id == lab::Id::LcdShow && c.na == 0, true);
    CHECK_EQ(P("lcd spi 4", c) == nullptr && c.id == lab::Id::LcdSpi && c.a[0] == 4 && c.na == 1, true);
    CHECK_EQ(P("lcd spi 8 --json", c) == nullptr && c.a[0] == 8 && c.o.json, true);
    CHECK_EQ(ok("lcd spi 2"), false);                              // 60 MHz is not offered
    CHECK_EQ(ok("lcd spi 0"), false);
    CHECK_EQ(ok("lcd spi 16"), false);
    CHECK_EQ(ok("lcd spi"), false);                                // divider missing
    CHECK_EQ(ok("lcd spi 4 8"), false);
    CHECK_EQ(ok("lcd show 4"), false);
    CHECK_EQ(ok("lcd nope"), false);
    CHECK_EQ(lab::is_family("lcd"), true);
    CHECK_STR(std::string(lab::name(lab::Id::LcdSpi)).c_str(), "lcd.spi");
    CHECK_STR(std::string(lab::name(lab::Id::LcdShow)).c_str(), "lcd.show");
    // families and unknown verbs
    CHECK_EQ(lab::is_family("accp"), true);
    CHECK_EQ(lab::is_family("qc3+"), true);
    CHECK_EQ(lab::is_family("log"), false);
    CHECK_EQ(ok("accp nope"), false);
    CHECK_EQ(ok("--confirm"), false);
    CHECK_STR(std::string(lab::name(lab::Id::AccpRead)).c_str(), "accp.read");
    return test_report("test_lab");
}
