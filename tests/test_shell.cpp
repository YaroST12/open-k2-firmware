// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The debug-shell line parser (k2/core/shell.hpp) against a recording backend: grammar, the confirm
// rule for anything that can raise VBUS above 5 V, argument limits and line editing.
#include <string>

#include "k2/core/shell.hpp"
#include "test_util.h"

using namespace k2;

namespace {

struct Fake : shell::Backend {
    std::string out, calls;
    const char* fail = nullptr;   // reason every action returns
    void write(std::string_view s) override { out += s; }
    void state() override { calls += "state;"; }
    void log_stream(bool on) override { calls += on ? "log1;" : "log0;"; }
    const char* drive(bool dp, dpdm::Drive d) override {
        calls += std::string(dp ? "dp" : "dm") + std::to_string(int(d)) + ";";
        return fail;
    }
    const char* cc_open(std::uint32_t ms) override { calls += "cc" + std::to_string(ms) + ";"; return fail; }
    const char* pd_silent(std::uint32_t s) override { calls += "silent" + std::to_string(s) + ";"; return fail; }
    const char* pd_reset() override { calls += "pdreset;"; return fail; }
    const char* pd_rescan() override { calls += "rescan;"; return fail; }
    const char* fc_trigger(fc::Proto p, int mv, bool confirm) override {
        calls += "fc" + std::to_string(int(p)) + "," + std::to_string(mv) + (confirm ? "c" : "") + ";";
        return fail;
    }
    const char* fc_reset() override { calls += "fcreset;"; return fail; }
    const char* lab(const lab::Cmd& c) override {
        calls += std::string("lab:") + std::string(lab::name(c.id)) + ":" + std::to_string(c.na ? c.a[0] : 0) + (c.o.confirm ? "c" : "") + ";";
        return fail;
    }
    const char* sniff(int mode, std::uint32_t rate) override { calls += "sniff" + std::to_string(mode) + "," + std::to_string(rate) + ";"; return fail; }
    const char* trace(int mode) override { calls += "trace" + std::to_string(mode) + ";"; return fail; }
    const char* stream(unsigned mask, unsigned hz, bool history) override {
        calls += "stream" + std::to_string(mask) + "," + std::to_string(hz) + (history ? ",old" : "") + ";";
        return fail;
    }
};

// Run one line; returns what the backend was asked to do, `out` keeps the text.
std::string run(Fake& f, const char* line) {
    f.calls.clear();
    f.out.clear();
    shell::Shell sh(f);
    for (const char* p = line; *p; p++) sh.feed(*p);
    sh.feed('\r');
    return f.calls;
}

bool said(const Fake& f, const char* s) { return f.out.find(s) != std::string::npos; }

}  // namespace

int main() {
    Fake f;
    // help and state
    CHECK_STR((run(f, "help")).c_str(), "");
    CHECK_EQ(said(f, "fc trigger"), true);
    CHECK_STR((run(f, "state")).c_str(), "state;");
    CHECK_STR((run(f, "  state  ")).c_str(), "state;");
    // log
    CHECK_STR((run(f, "log on")).c_str(), "log1;");
    CHECK_STR((run(f, "log off")).c_str(), "log0;");
    CHECK_STR((run(f, "log maybe")).c_str(), "");
    CHECK_EQ(said(f, "err"), true);
    // raw lines: hiz and 0 V are free, 0.6 and 3.3 V need confirm
    CHECK_STR((run(f, "dp hiz")).c_str(), "dp0;");
    CHECK_STR((run(f, "dm 0")).c_str(), "dm1;");
    CHECK_STR((run(f, "dp 0.6")).c_str(), "");
    CHECK_EQ(said(f, "confirm"), true);
    CHECK_STR((run(f, "dp 0.6 confirm")).c_str(), "dp2;");
    CHECK_STR((run(f, "dm 3.3 confirm")).c_str(), "dm4;");
    CHECK_STR((run(f, "dm 3.3")).c_str(), "");
    CHECK_STR((run(f, "dm 2.0 confirm")).c_str(), "");
    CHECK_STR((run(f, "dp 0 confirm")).c_str(), "dp1;");                 // harmless extra confirm
    CHECK_STR((run(f, "dp 0.6 confirm extra")).c_str(), "");
    // cc open
    CHECK_STR((run(f, "cc open 300")).c_str(), "cc300;");
    CHECK_STR((run(f, "cc open 0")).c_str(), "");
    CHECK_STR((run(f, "cc open 1001")).c_str(), "");
    CHECK_STR((run(f, "cc open abc")).c_str(), "");
    CHECK_STR((run(f, "cc open")).c_str(), "");
    // pd
    CHECK_STR((run(f, "pd silent 30")).c_str(), "silent30;");
    CHECK_STR((run(f, "pd silent 0")).c_str(), "");
    CHECK_STR((run(f, "pd silent 601")).c_str(), "");
    CHECK_STR((run(f, "pd reset")).c_str(), "pdreset;");
    CHECK_STR((run(f, "pd rescan")).c_str(), "rescan;");
    // fast charge: above 5 V needs confirm, nothing above the 36 V ceiling
    CHECK_STR((run(f, "fc trigger qc2 5000")).c_str(), "fc0,5000;");
    CHECK_STR((run(f, "fc trigger qc3 9000")).c_str(), "");
    CHECK_EQ(said(f, "confirm"), true);
    CHECK_STR((run(f, "fc trigger qc3 9000 confirm")).c_str(), "fc1,9000c;");
    CHECK_STR((run(f, "fc trigger QC3+ 12000 confirm")).c_str(), "fc2,12000c;");
    CHECK_STR((run(f, "fc trigger vivo 9000 confirm")).c_str(), "fc10,9000c;");
    CHECK_STR((run(f, "fc trigger svooc65 5000")).c_str(), "fc8,5000;");
    CHECK_STR((run(f, "fc trigger qc3 36000 confirm")).c_str(), "fc1,36000c;");
    CHECK_STR((run(f, "fc trigger qc3 36001 confirm")).c_str(), "");
    CHECK_STR((run(f, "fc trigger qc3 0 confirm")).c_str(), "");
    CHECK_STR((run(f, "fc trigger nope 5000")).c_str(), "");
    CHECK_STR((run(f, "fc trigger qc3")).c_str(), "");
    CHECK_STR((run(f, "sniff stream")).c_str(), "sniff1,0;");
    CHECK_STR((run(f, "sniff stop")).c_str(), "sniff0,0;");
    CHECK_STR((run(f, "sniff synth 3000")).c_str(), "sniff2,3000;");
    CHECK_STR((run(f, "sniff synth 0")).c_str(), "");
    CHECK_STR((run(f, "sniff")).c_str(), "");
    CHECK_STR((run(f, "fc reset")).c_str(), "fcreset;");
    // PD black box stream: from now, from the oldest record, stop; nothing else is accepted
    CHECK_STR((run(f, "trace stream")).c_str(), "trace1;");
    CHECK_STR((run(f, "TRACE Stream all")).c_str(), "trace2;");
    CHECK_STR((run(f, "trace stop")).c_str(), "trace0;");
    CHECK_STR((run(f, "trace stream everything")).c_str(), "");
    CHECK_EQ(said(f, "usage: trace"), true);
    CHECK_STR((run(f, "trace")).c_str(), "");
    CHECK_STR((run(f, "trace stop now")).c_str(), "");
    f.fail = "busy";
    CHECK_STR((run(f, "trace stream")).c_str(), "trace1;");
    CHECK_EQ(said(f, "err: busy"), true);
    f.fail = nullptr;
    // multiplexed debug stream: kinds are bits (log 1, trace 2, power 4, run 8), power takes an optional =Hz, `old` replays history
    CHECK_STR((run(f, "stream on")).c_str(), "stream15,0;");
    CHECK_STR((run(f, "stream on all")).c_str(), "stream15,0;");
    CHECK_STR((run(f, "STREAM ON Log Trace")).c_str(), "stream3,0;");
    CHECK_STR((run(f, "stream on power")).c_str(), "stream4,0;");
    CHECK_STR((run(f, "stream on power=50 run")).c_str(), "stream12,50;");
    CHECK_STR((run(f, "stream on run old")).c_str(), "stream8,0,old;");
    CHECK_STR((run(f, "stream on all old")).c_str(), "stream15,0,old;");
    CHECK_STR((run(f, "stream on old")).c_str(), "stream15,0,old;");
    CHECK_STR((run(f, "stream on power=100")).c_str(), "stream4,100;");
    CHECK_STR((run(f, "stream off")).c_str(), "stream0,0;");
    CHECK_STR((run(f, "stream on power=0")).c_str(), "");
    CHECK_EQ(said(f, "usage: stream"), true);
    CHECK_STR((run(f, "stream on power=101")).c_str(), "");
    CHECK_STR((run(f, "stream on power=x")).c_str(), "");
    CHECK_STR((run(f, "stream on bogus")).c_str(), "");
    CHECK_STR((run(f, "stream on log log")).c_str(), "");   // a kind named twice is a typo
    CHECK_STR((run(f, "stream off now")).c_str(), "");
    CHECK_STR((run(f, "stream")).c_str(), "");
    f.fail = "already streaming (stream off first)";
    CHECK_STR((run(f, "stream on")).c_str(), "stream15,0;");
    CHECK_EQ(said(f, "err: already streaming"), true);
    f.fail = nullptr;
    // protocol lab: dispatched after parsing, the confirm gate sits in front of the backend
    CHECK_STR((run(f, "accp read 0x80")).c_str(), "lab:accp.read:128;");
    CHECK_STR((run(f, "accp write 0xB0 0x13 confirm")).c_str(), "lab:accp.write:176c;");
    CHECK_STR((run(f, "accp write 0xB0 0x13")).c_str(), "");
    CHECK_EQ(said(f, "confirm"), true);
    CHECK_STR((run(f, "qc3 up 3 --pulse-us 2250 --confirm")).c_str(), "lab:qc3.up:3c;");
    CHECK_STR((run(f, "qc3 up 3")).c_str(), "");
    CHECK_STR((run(f, "vbus --json")).c_str(), "lab:vbus:0;");
    CHECK_STR((run(f, "accp read zz")).c_str(), "");
    CHECK_EQ(said(f, "err: argument"), true);
    CHECK_STR((run(f, "cc open 300")).c_str(), "cc300;");              // legacy verbs unchanged
    CHECK_STR((run(f, "pd reset")).c_str(), "pdreset;");
    CHECK_STR((run(f, "cc rp 50")).c_str(), "lab:cc.rp:50;");
    // ufcs tuner: no confirm, the value is the first number the backend sees (57600 is beyond a mV number)
    CHECK_STR((run(f, "ufcs set baud 57600")).c_str(), "lab:ufcs.set:57600;");
    CHECK_STR((run(f, "ufcs probe")).c_str(), "lab:ufcs.probe:0;");
    CHECK_STR((run(f, "ufcs show")).c_str(), "lab:ufcs.show:0;");
    CHECK_STR((run(f, "ufcs set baud 5")).c_str(), "");
    CHECK_EQ(said(f, "err: value out of range"), true);
    CHECK_STR((run(f, "ufcs set nope 1")).c_str(), "");
    CHECK_EQ(said(f, "err: unknown tuner field"), true);
    CHECK_EQ(run(f, "help").empty(), true);
    CHECK_EQ(said(f, "ufcs show|set"), true);
    // unknown / empty lines
    CHECK_STR((run(f, "frobnicate")).c_str(), "");
    CHECK_EQ(said(f, "unknown"), true);
    CHECK_STR((run(f, "")).c_str(), "");
    CHECK_EQ(f.out.empty(), true);
    // the backend's refusal is shown
    f.fail = "busy";
    CHECK_STR((run(f, "fc reset")).c_str(), "fcreset;");
    CHECK_EQ(said(f, "err: busy"), true);
    f.fail = nullptr;
    CHECK_STR((run(f, "fc reset")).c_str(), "fcreset;");
    CHECK_EQ(said(f, "ok"), true);
    // line editing: backspace; an overlong line is refused whole, the next one is fine
    {
        Fake g;
        shell::Shell sh(g);
        for (char c : std::string("stax\bte\r")) sh.feed(c);
        CHECK_STR((g.calls).c_str(), "state;");
        g.calls.clear();
        for (int i = 0; i < 200; i++) sh.feed('a');
        sh.feed('\n');
        CHECK_STR((g.calls).c_str(), "");
        CHECK_EQ(said(g, "too long"), true);
        for (char c : std::string("state\n")) sh.feed(c);
        CHECK_STR((g.calls).c_str(), "state;");
        // CRLF is one line end, not an empty command's error
        g.out.clear();
        for (char c : std::string("fc reset\r\n")) sh.feed(c);
        CHECK_STR((g.calls).c_str(), "state;fcreset;");
        CHECK_EQ(said(g, "unknown"), false);
    }
    return test_report("test_shell");
}
