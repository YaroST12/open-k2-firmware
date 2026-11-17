// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The debug-shell line parser (k2/core/shell.hpp) against a recording backend: grammar, the confirm
// rule for anything that can raise VBUS above 5 V, argument limits and line editing.
#include <cstdio>
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
    const char* stop() override { calls += "stop;"; return fail; }
    const char* lab(const lab::Cmd& c) override {
        calls += std::string("lab:") + std::string(lab::name(c.id)) + ":" + std::to_string(c.na ? c.a[0] : 0) + (c.o.confirm ? "c" : "") + ";";
        return fail;
    }
    const char* sniff(int mode, std::uint32_t rate) override { calls += "sniff" + std::to_string(mode) + "," + std::to_string(rate) + ";"; return fail; }
    const char* trace(int mode) override { calls += "trace" + std::to_string(mode) + ";"; return fail; }
    const char* vooc_monitor(int mode) override { calls += "vooc" + std::to_string(mode) + ";"; return fail; }
    const char* stream(unsigned mask, unsigned hz, bool history) override {
        calls += "stream" + std::to_string(mask) + "," + std::to_string(hz) + (history ? ",old" : "") + ";";
        return fail;
    }
    const char* twin(const shell::TwinCmd& c) override {
        calls += "twin" + std::to_string(int(c.op)) + "," + std::to_string(c.seq) + (c.all ? ",all" : "") + (c.confirm ? ",c" : "") +
                 (c.dry ? ",dry" : "") + (c.spoof ? ",spoof" + std::to_string(int(c.spoof_kind)) + ":" + std::to_string(c.spoof_id) : "") +
                 ",'" + std::string(c.name) + "';";
        return fail;
    }
    const char* diag() override { calls += "diag;"; return fail; }
    const char* memory() override { calls += "mem;"; return fail; }
    const char* locked = nullptr;   // the D+/D- lock reason (the guarded SuperVOOC trigger runs)
    const char* lines_locked() override { return locked; }
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
    // stop: the same one-call path as the HID `stop` command; no arguments, no confirm
    CHECK_STR((run(f, "stop")).c_str(), "stop;");
    CHECK_EQ(said(f, "ok"), true);
    CHECK_STR((run(f, "STOP")).c_str(), "stop;");
    CHECK_STR((run(f, "stop now")).c_str(), "");
    CHECK_EQ(said(f, "usage: stop"), true);
    CHECK_STR((run(f, "help")).c_str(), "");
    CHECK_EQ(said(f, "stop "), true);
    // inline VOOC monitor readout: passive, no confirm; on / off is the RAM switch, 2 = read only
    CHECK_STR((run(f, "fc vooc")).c_str(), "vooc2;");
    CHECK_STR((run(f, "FC VOOC on")).c_str(), "vooc1;");
    CHECK_STR((run(f, "fc vooc off")).c_str(), "vooc0;");
    CHECK_STR((run(f, "fc vooc maybe")).c_str(), "");
    CHECK_EQ(said(f, "usage: fc vooc"), true);
    CHECK_STR((run(f, "fc vooc on now")).c_str(), "");
    CHECK_EQ(run(f, "help").empty(), true);
    CHECK_EQ(said(f, "fc vooc [on|off]"), true);
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
    // device twins: op numbers are shell::TwinOp (List 0, Predict 1, Run 2, Status 3, Learn 4, LearnSniff 5, Delete 6, Rename 7, Cancel 8,
    // LearnInline 9)
    CHECK_STR((run(f, "twin list")).c_str(), "twin0,0,'';");
    CHECK_STR((run(f, "twin status")).c_str(), "twin3,0,'';");
    CHECK_STR((run(f, "twin cancel")).c_str(), "twin8,0,'';");
    CHECK_STR((run(f, "twin predict 3")).c_str(), "twin1,3,'';");
    CHECK_STR((run(f, "twin predict")).c_str(), "twin1,0,all,'';");
    CHECK_STR((run(f, "twin predict all")).c_str(), "twin1,0,all,'';");
    CHECK_STR((run(f, "twin predict 0")).c_str(), "");
    CHECK_STR((run(f, "twin predict 13")).c_str(), "");
    CHECK_EQ(said(f, "err: usage: twin predict"), true);
    // a run is a live negotiation with the charger: it needs the trailing confirm
    CHECK_STR((run(f, "twin run 2")).c_str(), "");
    CHECK_EQ(said(f, "needs a trailing confirm"), true);
    CHECK_STR((run(f, "twin run 2 confirm")).c_str(), "twin2,2,c,'';");
    CHECK_STR((run(f, "twin run all confirm")).c_str(), "twin2,0,all,c,'';");
    CHECK_STR((run(f, "twin run confirm")).c_str(), "twin2,0,all,c,'';");
    CHECK_STR((run(f, "twin run 2 3 confirm")).c_str(), "");
    // learning: the name is the rest of the line, `dry` previews without saving
    CHECK_STR((run(f, "twin learn identify Pixel 9")).c_str(), "twin4,0,'Pixel 9';");
    CHECK_STR((run(f, "twin learn sniff dry Camera")).c_str(), "twin5,0,dry,'Camera';");
    CHECK_STR((run(f, "twin learn identify")).c_str(), "twin4,0,'';");
    CHECK_STR((run(f, "twin learn inline")).c_str(), "");        // the interrogation resets the link twice: confirm first
    CHECK_EQ(said(f, "err: an inline interrogation"), true);
    CHECK_STR((run(f, "twin learn inline confirm")).c_str(), "twin9,0,c,'';");
    CHECK_STR((run(f, "twin learn inline confirm dry Canon R5")).c_str(), "twin9,0,c,dry,'Canon R5';");
    CHECK_STR((run(f, "twin learn inline dry confirm")).c_str(), "");
    // the cable spoof for the interrogation only: a saved library number (`spoof lib N`) or a built-in (`spoof builtin N`)
    CHECK_STR((run(f, "twin learn inline confirm spoof lib 3")).c_str(), "twin9,0,c,spoof1:3,'';");
    CHECK_STR((run(f, "twin learn inline confirm spoof builtin 2 dry Canon R5")).c_str(), "twin9,0,c,dry,spoof0:2,'Canon R5';");
    CHECK_STR((run(f, "twin learn inline spoof lib 3 confirm")).c_str(), "");           // confirm stays first
    CHECK_STR((run(f, "twin learn inline confirm spoof")).c_str(), "");
    CHECK_EQ(said(f, "err: usage: twin learn inline"), true);
    CHECK_STR((run(f, "twin learn inline confirm spoof lib")).c_str(), "");
    CHECK_STR((run(f, "twin learn inline confirm spoof lib 0")).c_str(), "");
    CHECK_STR((run(f, "twin learn inline confirm spoof nope 1")).c_str(), "");
    CHECK_STR((run(f, "twin learn identify spoof lib 3")).c_str(), "twin4,0,'spoof lib 3';");   // only the inline run takes it (a name here)
    CHECK_STR((run(f, "twin learn frob")).c_str(), "");
    CHECK_STR((run(f, "twin learn identify 0123456789012345678901234")).c_str(), "");   // 25 characters
    CHECK_EQ(said(f, "err: name"), true);
    CHECK_STR((run(f, "twin rename 4 My phone")).c_str(), "twin7,4,'My phone';");
    CHECK_STR((run(f, "twin rename 4")).c_str(), "");
    CHECK_STR((run(f, "twin delete 4")).c_str(), "");
    CHECK_EQ(said(f, "needs a trailing confirm"), true);
    CHECK_STR((run(f, "twin delete 4 confirm")).c_str(), "twin6,4,c,'';");
    CHECK_STR((run(f, "twin")).c_str(), "");
    CHECK_STR((run(f, "twin frob")).c_str(), "");
    CHECK_EQ(said(f, "err: usage: twin"), true);
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
    // mem: read-only, no arguments; the backend prints the rows itself
    f.fail = nullptr;
    CHECK_STR((run(f, "mem")).c_str(), "mem;");
    CHECK_STR((run(f, "mem 1")).c_str(), "");
    CHECK_EQ(said(f, "usage: mem"), true);
    f.fail = "mem broke";
    run(f, "mem");
    CHECK_EQ(said(f, "mem broke"), true);
    f.fail = nullptr;

    // diag: read-only, no arguments; the backend prints the lines itself and only a failure adds an err line
    CHECK_STR((run(f, "diag")).c_str(), "diag;");
    CHECK_EQ(said(f, "err"), false);
    CHECK_EQ(said(f, "ok"), false);
    CHECK_STR((run(f, "DIAG")).c_str(), "diag;");
    CHECK_STR((run(f, "diag now")).c_str(), "");
    CHECK_EQ(said(f, "usage: diag"), true);
    f.fail = "busy";
    run(f, "diag");
    CHECK_EQ(said(f, "err: busy"), true);
    f.fail = nullptr;
    run(f, "help");
    CHECK_EQ(said(f, "diag "), true);

    // D+/D- locked (the guarded SuperVOOC trigger drives D-): every command that drives or claims D+/D- or the sniffer is
    // refused with the backend's reason and never reaches it, whatever level or mode; stop, fc reset and the read-only
    // commands still go through.
    {
        Fake g;
        g.locked = "busy (SuperVOOC trigger running)";
        const char* refused[] = {
            "dp hiz", "dm hiz", "dm 0", "dp 3.3 confirm",
            "fc trigger qc2 5000", "fc trigger qc2 9000 confirm", "fc vooc on", "fc vooc off",
            "sniff stream", "sniff stop", "sniff synth 100",
            "accp ping", "accp read 1", "qc2 5000", "qc3 set 5000", "qc3+ set 5000", "afc 5000", "fcp 5000", "scp 5000 1000",
            "vooc wake confirm", "vooc listen", "vooc answer 1 2 confirm", "vooc sniff",
            "ufcs ping", "ufcs caps", "ufcs probe", "ufcs request 5000 1000", "vivo request 5000 1000", "dpdm hiz hiz",
        };
        for (const char* line : refused) {
            const std::string calls = run(g, line);
            CHECK_STR(calls.c_str(), "");
            if (!said(g, "err: busy (SuperVOOC trigger running)")) std::printf("  not refused: %s -> %s\n", line, g.out.c_str());
            CHECK_EQ(said(g, "err: busy (SuperVOOC trigger running)"), true);
        }
        CHECK_STR((run(g, "stop")).c_str(), "stop;");
        CHECK_STR((run(g, "fc reset")).c_str(), "fcreset;");
        CHECK_STR((run(g, "fc vooc")).c_str(), "vooc2;");                 // the summary read
        CHECK_STR((run(g, "state")).c_str(), "state;");
        CHECK_STR((run(g, "ufcs show")).c_str(), "lab:ufcs.show:0;");     // RAM tuner, no bus
        CHECK_STR((run(g, "vbus")).c_str(), "lab:vbus:0;");
        CHECK_STR((run(g, "pd caps")).c_str(), "lab:pd.caps:0;");         // CC, not D+/D-
        // unlocked again: the same commands reach the backend
        g.locked = nullptr;
        CHECK_STR((run(g, "dm hiz")).c_str(), "dm0;");
        CHECK_STR((run(g, "sniff stop")).c_str(), "sniff0,0;");
        CHECK_STR((run(g, "vooc listen")).c_str(), "lab:vooc.listen:0;");
    }
    // Which lab commands drive or claim D+/D- (the lock's list): every fast-charge family and dpdm, none of the PD, CC,
    // VBUS, LCD, PD Stress or UFCS tuner ones.
    {
        using lab::Id;
        for (Id id : {Id::AccpPing, Id::AccpRead, Id::AccpWrite, Id::AccpDump, Id::Qc2, Id::Qc3Set, Id::Qc3Up, Id::Qc3Down,
                      Id::Qc3pSet, Id::Qc3pUp, Id::Qc3pDown, Id::Afc, Id::Fcp, Id::Scp, Id::VoocWake, Id::VoocListen,
                      Id::VoocAnswer, Id::VoocSniff, Id::UfcsPing, Id::UfcsCaps, Id::UfcsRequest, Id::UfcsRaw, Id::UfcsProbe,
                      Id::VivoRaw, Id::VivoRequest, Id::Dpdm})
            CHECK_EQ(lab::drives_lines(id), true);
        for (Id id : {Id::UfcsSet, Id::UfcsShow, Id::UfcsReset, Id::UfcsResult, Id::CcRp, Id::CcRd, Id::Vbus, Id::PdCaps,
                      Id::PdRequest, Id::PdPps, Id::PdAvs, Id::PdEpr, Id::PdVdm, Id::PdGet, Id::PdReread, Id::PdHardReset,
                      Id::PdSoftReset, Id::PdRaw, Id::LcdShow, Id::LcdSpi, Id::PdStressStart, Id::PdStressStatus,
                      Id::PdStressResult, Id::PdStressStop})
            CHECK_EQ(lab::drives_lines(id), false);
    }
    // Golden output: the exact reply text of the debug shell, captured from the table-less original, so the host tools
    // that parse it (tools/k2_remote.py, proto_lab) see the same bytes after any refactor of the printers.
    {
        struct Gold { const char* line; const char* out; };
        static const Gold kGold[] = {
        {"", ""},
        {"unknown", "err: unknown command (help)\r\n"},
        {"state extra", ""},
        {"log maybe", "err: usage: log on|off\r\n"},
        {"dp 0.6", "err: this level needs a trailing confirm\r\n"},
        {"dp xyz", "err: level is hiz, 0, 0.6 or 3.3\r\n"},
        {"cc open 0", "err: usage: cc open <1..1000 ms>\r\n"},
        {"cc open", "err: usage: cc open <1..1000 ms>\r\n"},
        {"pd silent 9999", "err: usage: pd silent <1..600 s>\r\n"},
        {"pd foo", "err: unknown verb (help lab)\r\n"},
        {"fc trigger qc3 9000", "err: above 5000 mV needs a trailing confirm\r\n"},
        {"fc trigger nope 5000", "err: unknown protocol\r\n"},
        {"fc", "err: usage: fc trigger <proto> <mV> [confirm] | fc reset\r\n"},
        {"stop now", "err: usage: stop\r\n"},
        {"sniff", "err: usage: sniff stream | sniff stop | sniff synth <1..100000 edges/s>\r\n"},
        {"trace x", "err: usage: trace stream [all] | trace stop\r\n"},
        {"vooc x", "err: unknown verb (help lab)\r\n"},
        {"stream x 5", "err: usage: stream on [log|trace|power[=1..100 Hz]|run|all] [old] | stream off\r\n"},
        {"twin x", "err: usage: twin list | predict [n|all] | run [n|all] confirm | status | cancel | learn identify|sniff [dry] [name] | learn inline confirm [spoof lib|builtin n] [dry] [name] | rename n name | delete n confirm\r\n"},
        {"twin rename 2", "err: usage: twin rename 1..12 <name>\r\n"},
        {"twin delete", "err: usage: twin delete 1..12 confirm\r\n"},
        {"accp read 0x100", "err: argument must be a byte (0..255 or 0x00..0xFF)\r\n"},
        {"accp write 0xA0 0xC0", "err: this can raise VBUS: add --confirm or a trailing confirm\r\n"},
        {"accp nope", "err: unknown verb (help lab)\r\n"},
        {"pdstress start 5 confirm", "err: too many arguments\r\n"},
        {"ufcs request 5000", "err: missing argument\r\n"},
        {"ufcs raw", "err: missing argument\r\n"},
        {"vivo raw zz", "err: argument must be an even-length hex string (max 32 bytes)\r\n"},
        {"vooc answer 1", "err: missing argument\r\n"},
        {"qc2 36001 confirm", "err: argument must be a number (max 36000)\r\n"},
        {"pd request", "err: missing argument\r\n"},
        {"pd pps 1", "err: missing argument\r\n"},
        {"lcd show", "ok\r\n"},
        {"lcd spi", "err: missing argument\r\n"},
        {"cc rp", "ok\r\n"},
        {"vbus extra", "err: too many arguments\r\n"},
        {"dpdm", "err: missing argument\r\n"},
        {"accp ping --bogus", "err: unknown option\r\n"},
        {"accp ping --retries", "err: option needs a number\r\n"},
        {"accp read 1 --json --json", "ok\r\n"},
        {"ufcs show 1 2 3 4 5 6 7 8 9", "err: too many arguments\r\n"},
        };
        Fake g;
        for (const Gold& e : kGold) {
            run(g, e.line);
            if (g.out != e.out) std::printf("  golden mismatch for '%s': '%s'\n", e.line, g.out.c_str());
            CHECK_EQ(g.out == e.out, true);
        }
        // the help text and the replies of every lab verb (bare, family-only and with a spread of arguments and options,
        // backend ok / failing / D+D- locked) as length + FNV-1a, since they are too long to list
        auto fnv = [](const std::string& s, std::uint32_t h) {
            for (unsigned char ch : s) { h ^= ch; h *= 16777619u; }
            return h;
        };
        run(g, "help");
#if K2_PDSTRESS   // the help text lists pdstress only in builds that have it (the host tests always do)
        CHECK_EQ(g.out.size(), 2089);
        CHECK_EQ(fnv(g.out, 2166136261u), 0x9c0e1d7fu);
#endif
        static const char* const kSfx[] = {"", " 1", " 1 2", " 5000 9000", " 0x10 0xFF", " --json", " --retries 3", " --retries",
                                           " --bogus", " confirm", " 1 2 3 4 5 6 7 8", " zz"};
        std::uint32_t h = 2166136261u;
        std::size_t total = 0;
        for (int mode = 0; mode < 3; mode++) {
            g.fail = mode == 1 ? "reason-x" : nullptr;
            g.locked = mode == 2 ? "locked-y" : nullptr;
            for (int id = 0; id < int(lab::Id::Count); id++) {
                std::string nm(lab::name(lab::Id(id)));
                for (char& ch : nm) if (ch == '.') ch = ' ';   // "accp.read" is typed "accp read"
                const std::string fam = nm.substr(0, nm.find(' '));
                for (const char* s : kSfx)
                    for (const std::string& base : {nm, fam}) {
                        run(g, (base + s).c_str());
                        h = fnv(g.out, h);
                        total += g.out.size();
                    }
            }
        }
        CHECK_EQ(total, 99036);
        CHECK_EQ(h, 0x6a139103u);
    }

    return test_report("test_shell");
}
