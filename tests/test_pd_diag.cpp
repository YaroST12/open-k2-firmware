// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Black Box diagnosis (k2/pd/diag.hpp): one test per rule, on synthetic records, plus the captured fixtures.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../tools/bbdiag_walk.hpp"
#include "k2/pd/diag.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pdtrace.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::pd;
using namespace k2::pd::diag;

namespace {

// ---- record builders (the pdtrace wire format: len type ts_us payload) ----

enum : unsigned {   // message types
    GoodCRC = 1, Accept = 3, Reject = 4, PS_RDY = 6, Get_Sink_Cap = 8, DR_Swap = 9, PR_Swap = 10, VCONN_Swap = 11, Wait = 12,
    Soft_Reset = 13, Not_Supported = 16,
};
enum : unsigned { Source_Cap = 1, Request = 2, Sink_Cap = 4, Enter_USB = 8, EPR_Request = 9, EPR_Mode = 10, VDM = 15 };
constexpr std::uint8_t kSop = 0xE0, kSop1 = 0xC0;

constexpr std::uint32_t fixed(unsigned mv, unsigned ma) { return (mv / 50) << 10 | ma / 10; }
constexpr std::uint32_t rdo(unsigned pos, unsigned op_ma, unsigned max_ma, bool mismatch = false) {
    return pos << 28 | (mismatch ? 1u << 26 : 0) | (op_ma / 10) << 10 | max_ma / 10;
}
constexpr std::uint32_t vdm(unsigned svid, unsigned ctype, unsigned cmd) { return svid << 16 | 1u << 15 | 1u << 13 | ctype << 6 | cmd; }

struct Trace {
    std::vector<std::uint8_t> b;
    std::uint32_t t_us = 1'000'000;
    std::uint32_t crc = 0x1000;
    unsigned id_src = 0, id_snk = 0, id_cable = 0;

    void ms(std::uint32_t ms) { t_us = ms * 1000; }
    void wait(std::uint32_t ms) { t_us += ms * 1000; }
    void put(pdtrace::Type type, const std::vector<std::uint8_t>& p) {
        b.push_back(std::uint8_t(pdtrace::kHeader + p.size()));
        b.push_back(std::uint8_t(type));
        for (int i = 0; i < 4; i++) b.push_back(std::uint8_t(t_us >> 8 * i));
        b.insert(b.end(), p.begin(), p.end());
    }
    void cc(bool attach) { put(pdtrace::Type::Cc, {std::uint8_t(attach ? 1 : 0), 1, 0}); }
    void vbus(unsigned mv, int ma) { put(pdtrace::Type::Vbus, {std::uint8_t(mv), std::uint8_t(mv >> 8), std::uint8_t(ma), std::uint8_t(ma >> 8)}); }
    void boot() { put(pdtrace::Type::Mark, {pdtrace::kMarkBoot, 0, 0, 0, 0}); }
    void reset(unsigned kind) { put(pdtrace::Type::Reset, {std::uint8_t(kind)}); }
    void tx_result(unsigned res) { put(pdtrace::Type::TxResult, {std::uint8_t(res), 0, 0}); }
    void rx_bad() { put(pdtrace::Type::RxBad, {0x00}); }

    static std::uint16_t hdr(unsigned type, unsigned n, unsigned id, bool src, bool dfp, bool ext = false) {
        return std::uint16_t(type | (dfp ? 1u << 5 : 0) | 2u << 6 | (src ? 1u << 8 : 0) | id << 9 | n << 12 | (ext ? 1u << 15 : 0));
    }
    // A message on the wire as the passive K2 records it (Rx), with the other end's GoodCRC unless `ack` is false.
    std::uint32_t rx(bool src, bool dfp, unsigned type, const std::vector<std::uint32_t>& objs, bool ack = true,
                     std::uint8_t token = kSop, int same_crc = -1, int id_force = -1) {
        unsigned& idc = token != kSop ? id_cable : src ? id_src : id_snk;
        const unsigned id = id_force >= 0 ? unsigned(id_force) : idc;
        const std::uint16_t h = hdr(type, unsigned(objs.size()), id, src, dfp);
        const std::uint32_t c = same_crc >= 0 ? std::uint32_t(same_crc) : crc++;
        std::vector<std::uint8_t> p{token, std::uint8_t(h), std::uint8_t(h >> 8)};
        for (auto o : objs) for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(o >> 8 * i));
        for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(c >> 8 * i));
        put(pdtrace::Type::Rx, p);
        if (ack) {
            wait(1);
            const std::uint16_t g = hdr(GoodCRC, 0, id, token == kSop ? !src : true, !dfp);
            const std::uint32_t gc = crc++;
            std::vector<std::uint8_t> q{token, std::uint8_t(g), std::uint8_t(g >> 8)};
            for (int i = 0; i < 4; i++) q.push_back(std::uint8_t(gc >> 8 * i));
            put(pdtrace::Type::Rx, q);
        }
        if (id_force < 0 && same_crc < 0) idc = (idc + 1) & 7;
        wait(2);
        return c;
    }
    // The K2 itself sends (active): a Tx record.
    void tx(bool src, bool dfp, unsigned type, const std::vector<std::uint32_t>& objs) {
        const std::uint16_t h = hdr(type, unsigned(objs.size()), id_snk, src, dfp);
        id_snk = (id_snk + 1) & 7;
        std::vector<std::uint8_t> p{std::uint8_t(h), std::uint8_t(h >> 8)};
        for (auto o : objs) for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(o >> 8 * i));
        p.push_back(kSop);
        put(pdtrace::Type::Tx, p);
        wait(2);
    }
    // Source caps, Request, Accept, PS_RDY between a passive pair. The sink is a host (dfp) unless told otherwise.
    void contract(const std::vector<std::uint32_t>& caps, std::uint32_t req, bool snk_dfp = true, unsigned accept_ms = 5,
                  unsigned psrdy_ms = 40) {
        rx(true, !snk_dfp, Source_Cap, caps);
        rx(false, snk_dfp, Request, {req});
        wait(accept_ms);
        rx(true, !snk_dfp, Accept, {});
        wait(psrdy_ms);
        rx(true, !snk_dfp, PS_RDY, {});
    }
    // VBUS samples every `step` ms for `dur` ms.
    void hold(unsigned mv, int ma, unsigned dur, unsigned step = 100) {
        for (unsigned t = 0; t < dur; t += step) { vbus(mv, ma); wait(step); }
    }
    // Power-monitor windows (the fast min/max input): one per `span` ms for `dur` ms, ending at each step.
    std::vector<bbdiag::TimedWindow> wins;
    void win(unsigned mv, int ma, unsigned vmin, unsigned vmax, int imin, int imax, unsigned span = 100) {
        wait(span);
        bbdiag::TimedWindow w;
        w.ts_us = t_us;
        w.w.span_ms = std::uint16_t(span);
        w.w.n = std::uint16_t(span / 50 ? span / 50 : 1);
        w.w.mv = std::uint16_t(mv);
        w.w.ma = std::int16_t(ma);
        w.w.vmin = std::uint16_t(vmin);
        w.w.vmax = std::uint16_t(vmax);
        w.w.imin = std::int16_t(imin);
        w.w.imax = std::int16_t(imax);
        wins.push_back(w);
    }
    void wins_for(unsigned dur, unsigned mv, int ma, unsigned vmin, unsigned vmax, int imin, int imax, unsigned span = 100) {
        for (unsigned t = 0; t < dur; t += span) win(mv, ma, vmin, vmax, imin, imax, span);
    }
};

struct Run {
    Analyzer a;
    Report r;
    std::vector<std::string> lines;
    void go(const Trace& t, const TwinHint* twin = nullptr, bool finished = false) {
        if (t.wins.empty()) a.feed(t.b);
        else bbdiag::walk(a, t.b, t.wins, [] {});
        a.advance(t.t_us);
        r = evaluate(finished ? a.last() : a.shown(), twin, a.now_ms());
        render();
    }
    void render() {
        lines.clear();
        const unsigned n = line_count(r);
        for (unsigned i = 0; i < n; i++) {
            Line l;
            if (!line(r, i, l)) continue;
            lines.emplace_back(l.text.data());
            // a line that fills its buffer was cut: every text must fit (no truncation on the K2 either)
            CHECK_COND(lines.back().size() < kLineMax - 1);
            if (lines.back().size() >= kLineMax - 1) std::printf("  truncated: %s\n", lines.back().c_str());
        }
    }
    const Finding* find(Code c) const {
        for (unsigned i = 0; i < r.n; i++) if (r.f[i].code == c) return &r.f[i];
        return nullptr;
    }
    bool has_line(const char* s) const {
        for (const auto& l : lines) if (l.find(s) != std::string::npos) return true;
        return false;
    }
    void dump(const char* name) const {
        std::printf("--- %s\n", name);
        for (const auto& l : lines) std::printf("  %s\n", l.c_str());
    }
};

std::vector<std::uint8_t> load(const char* path) {
    std::vector<std::uint8_t> v;
    FILE* f = std::fopen(path, "rb");
    if (!f) return v;
    std::uint8_t buf[4096];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) v.insert(v.end(), buf, buf + n);
    std::fclose(f);
    return v;
}

const std::vector<std::uint32_t> kHubCaps{fixed(5000, 3000), fixed(9000, 3000), fixed(15000, 3000), fixed(20000, 3000)};
const std::uint32_t kLegionPdos[] = {0x3601912C, 0x9902D145};   // fixed 5 V 3 A, variable 9-20 V 3.25 A (65 W)

#define EXPECT_CODE(run, code, sev_) do { \
    const Finding* f_ = (run).find(code); \
    CHECK_COND(f_ != nullptr); \
    if (f_) CHECK_EQ(int(f_->sev), int(sev_)); \
    if (!f_) (run).dump(#code); \
} while (0)
#define EXPECT_NO(run, code) do { CHECK_COND((run).find(code) == nullptr); if ((run).find(code)) (run).dump("unexpected " #code); } while (0)

// ---- the acceptance case: a laptop behind a 60 W hub, 65 W need from an assumed twin ----

void test_case1_fixture() {
    const auto raw = load("tests/fixtures/bbdiag/hub_rear_60w.raw");
    CHECK_COND(raw.size() > 1000);
    TwinHint twin{"Legion rear port", kLegionPdos, true, true};
    Run run;
    run.a.feed(raw);
    run.r = evaluate(run.a.shown(), &twin, run.a.now_ms());
    run.render();
    run.dump("case 1 (hub rear 60 W)");
    CHECK_STR(run.lines[0].c_str(), "PD link healthy, power short");
    CHECK_EQ(int(run.r.f[0].code), int(Code::PowerShort));
    CHECK_EQ(run.r.need_mw, 65000u);
    CHECK_EQ(run.r.offer_mw, 60000u);
    CHECK_EQ(int(run.r.need_from), int(NeedFrom::AssumedTwin));
    CHECK_STR(run.lines[1].c_str(), "Laptop needs 65 W, source gives 60 W max");
    CHECK_STR(run.lines[2].c_str(), "need: assumed twin \"Legion rear port\", 20 V 3.25 A");
    EXPECT_CODE(run, Code::NoMismatchFlag, Sev::Info);
    EXPECT_CODE(run, Code::Draw, Sev::Info);
    CHECK_COND(run.has_line("Contract 20 V 3 A, draws"));
    CHECK_COND(run.has_line("No resets, no retries, no rejects"));
    CHECK_COND(run.has_line("Data lanes pass through the K2, not observed"));
    CHECK_COND(run.has_line("agreed over PD: DP alt mode"));
    EXPECT_NO(run, Code::SwapRefused);   // the rear port accepted the hub's PR_Swap
    EXPECT_NO(run, Code::SoftReset);     // the Soft_Reset was on SOP' (the cable plug), not the contract
    EXPECT_NO(run, Code::OverDraw);
    for (const auto& l : run.lines) CHECK_COND(l.find("negotiat") == std::string::npos);
}

// Without a twin the need is unknown: the K2 says so instead of guessing.
void test_case1_no_twin() {
    const auto raw = load("tests/fixtures/bbdiag/hub_rear_60w.raw");
    Run run;
    run.a.feed(raw);
    run.r = evaluate(run.a.shown(), nullptr, run.a.now_ms());
    run.render();
    CHECK_STR(run.lines[0].c_str(), "PD link healthy, need not known");
    EXPECT_NO(run, Code::PowerShort);
}

// ---- captured fixtures: every session in a file, finished ones and the open one last (as bbdiag prints them) ----

std::vector<Run> sessions(const char* path, const TwinHint* twin = nullptr, const char* power = nullptr) {
    const auto b = load(path);
    CHECK_COND(b.size() > 100);
    std::vector<bbdiag::TimedWindow> wins;
    if (power) {
        const auto pb = load(power);
        CHECK_COND(pb.size() >= bbdiag::kWindowRecord);
        wins = bbdiag::parse_windows(pb);
    }
    std::vector<Run> out;
    static Analyzer a;
    a = Analyzer();
    bbdiag::walk(a, b, wins, [&] {
        out.emplace_back();
        out.back().r = evaluate(a.last(), twin, a.now_ms());
        out.back().render();
    });
    if (a.current().open) {
        out.emplace_back();
        out.back().r = evaluate(a.current(), twin, a.now_ms());
        out.back().render();
    }
    return out;
}

// The Anker splitter: the rear leg (laptop on a contract made before the K2 attached), then the left port, where the
// laptop is the 5 V source and refuses the hub's PR_Swap.
void test_fixture_splitter() {
    const std::uint32_t pdos[] = {0x3601912C, 0x9902D145};
    TwinHint twin{"Legion rear port", pdos, false, true, 0x17EF, 0xA4BE};
    auto ss = sessions("tests/fixtures/bbdiag/splitter_rear_left.raw", &twin);
    CHECK_EQ(ss.size(), 6u);
    if (ss.size() < 6) return;
    Run& rear = ss[0];
    rear.dump("splitter rear");
    CHECK_STR(rear.lines[0].c_str(), "PD link healthy, contract not observed");
    EXPECT_CODE(rear, Code::ContractUnseen, Sev::Info);
    CHECK_COND(rear.has_line("VBUS 19.4 V when the K2 saw it"));
    EXPECT_CODE(rear, Code::Draw, Sev::Info);
    EXPECT_CODE(rear, Code::DrawReduced, Sev::Info);
    CHECK_COND(rear.has_line("reduced its draw, contract unchanged"));
    CHECK_COND(rear.has_line("no PD message, VBUS kept"));
    EXPECT_CODE(rear, Code::CableSilent, Sev::Info);
    EXPECT_CODE(rear, Code::VdmLoop, Sev::Info);
    CHECK_COND(rear.has_line("Not_Supported"));
    EXPECT_NO(rear, Code::VbusLost);     // the unplug at 1108.4 s: VBUS fell, then CC detached
    EXPECT_NO(rear, Code::VbusLow);
    EXPECT_NO(rear, Code::PowerShort);   // the contract is not on this leg: no offer to compare
    for (int i : {1, 2, 4}) {
        Run& left = ss[i];
        EXPECT_CODE(left, Code::SwapRefused, Sev::Info);
        CHECK_COND(left.has_line("refuses to take power on this port"));
        CHECK_COND(left.has_line("PR_Swap rejected"));
        CHECK_COND(left.has_line("it stays the 5 V source"));
        CHECK_COND(left.has_line("peak 0 mA"));   // no INA tail from the unplug before it
        EXPECT_NO(left, Code::VbusLost);
    }
    EXPECT_CODE(ss[3], Code::ContractUnseen, Sev::Info);
    CHECK_STR(ss[5].lines[0].c_str(), "No PD seen, VBUS 0 V");
    for (auto& s : ss) for (const auto& l : s.lines) CHECK_COND(l.find("negotiat") == std::string::npos);
}

// The first boot of the laptop-hub run: nothing happened, and the K2 says only that.
void test_fixture_laptop_hub_idle() {
    auto ss = sessions("tests/fixtures/bbdiag/laptop_hub_idle.raw");
    CHECK_EQ(ss.size(), 1u);
    if (ss.empty()) return;
    CHECK_STR(ss[0].lines[0].c_str(), "No PD seen, VBUS 0 V");
    CHECK_EQ(ss[0].r.n, 0u);
}

// The Legion inline on a charger: PR_Swap answered Not_Supported, and the unplug ramp at the end is not a dip.
void test_fixture_legion_inline() {
    auto ss = sessions("tests/fixtures/bbdiag/legion_inline.raw");
    CHECK_COND(ss.size() >= 1);
    if (ss.empty()) return;
    EXPECT_CODE(ss[0], Code::SwapRefused, Sev::Info);
    CHECK_COND(ss[0].has_line("PR_Swap answered Not_Supported"));
    CHECK_COND(ss[0].has_line("Contract 20 V 3.25 A"));
    EXPECT_NO(ss[0], Code::VbusLow);
    EXPECT_NO(ss[0], Code::VbusLost);
}

// PD Stress: ten Soft_Resets sent by the K2 itself; its own Requests are not counted against the source.
void test_fixture_stress_soft_resets() {
    auto ss = sessions("tests/fixtures/bbdiag/stress_soft_resets.raw");
    CHECK_EQ(ss.size(), 1u);
    if (ss.empty()) return;
    EXPECT_CODE(ss[0], Code::SoftReset, Sev::Fail);
    CHECK_COND(ss[0].has_line("10 sent by the K2"));
    EXPECT_NO(ss[0], Code::ManyRequests);
    EXPECT_CODE(ss[0], Code::ActiveSink, Sev::Info);
}

// The K2 as sink on the FH22 brick, the offer outside the captured window.
void test_fixture_fh22() {
    auto ss = sessions("tests/fixtures/bbdiag/fh22_request_5a.raw");
    CHECK_EQ(ss.size(), 1u);
    if (ss.empty()) return;
    EXPECT_CODE(ss[0], Code::ActiveSink, Sev::Info);
    CHECK_COND(!ss[0].has_line("0 V 0 mA"));
}

// OnePlus brick with K2 re-reads: each release drops VBUS by design (no cut-out, no dip, no Request count).
void test_fixture_oneplus() {
    auto ss = sessions("tests/fixtures/bbdiag/oneplus_usbc.raw");
    CHECK_COND(ss.size() >= 1);
    if (ss.empty()) return;
    ss[0].dump("oneplus");
    CHECK_STR(ss[0].lines[0].c_str(), "PD link healthy, K2 is the sink");
    EXPECT_NO(ss[0], Code::VbusLost);
    EXPECT_NO(ss[0], Code::VbusLow);
    EXPECT_NO(ss[0], Code::VbusHigh);
    EXPECT_NO(ss[0], Code::ManyRequests);
}

// The usb3-left text dump: its last session is a real "contract not observed" with no VBUS sample in it.
void test_fixture_usb3_left_unseen() {
    auto ss = sessions("tests/fixtures/bbdiag/usb3_left_flipped.raw");
    CHECK_EQ(ss.size(), 3u);
    if (ss.size() < 3) return;
    CHECK_STR(ss[2].lines[0].c_str(), "PD link healthy, contract not observed");
    CHECK_COND(ss[2].has_line("no VBUS reading in this capture"));
    EXPECT_CODE(ss[2], Code::SwapRefused, Sev::Info);
}

// ---- power shortfall ----

void test_need_from_sink_caps() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.rx(true, false, Get_Sink_Cap, {});
    t.rx(false, true, Sink_Cap, {fixed(5000, 3000) | 1u << 28, fixed(20000, 5000)});
    t.hold(20000, 2900, 12000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::PowerShort, Sev::Fail);
    CHECK_EQ(int(run.r.need_from), int(NeedFrom::SinkCaps));
    CHECK_EQ(run.r.need_mw, 100000u);
    CHECK_COND(run.has_line("its sink caps on the wire"));
}

void test_sink_caps_from_source_are_not_the_need() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.rx(false, true, Get_Sink_Cap, {});
    t.rx(true, false, Sink_Cap, {fixed(5000, 0)});   // the source's own sink side
    t.hold(20000, 100, 2000);
    Run run;
    run.go(t);
    CHECK_EQ(int(run.r.need_from), int(NeedFrom::None));
    EXPECT_NO(run, Code::PowerShort);
}

void test_need_from_skedb() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    // Sink_Capabilities_Extended: ext header (chunked, chunk 0, size 24) then the SKEDB; PDP op at byte 19, max at 20
    std::vector<std::uint8_t> ext(26, 0);
    ext[0] = 24; ext[1] = 0x80;
    ext[2 + 19] = 90; ext[2 + 20] = 100;
    const std::uint16_t h = Trace::hdr(15, 7, 3, false, true, true);
    std::vector<std::uint8_t> p{kSop, std::uint8_t(h), std::uint8_t(h >> 8)};
    p.insert(p.end(), ext.begin(), ext.end());
    p.resize(3 + 28, 0);
    for (int i = 0; i < 4; i++) p.push_back(0x55);
    t.put(pdtrace::Type::Rx, p);
    t.hold(20000, 1000, 2000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::PowerShort, Sev::Fail);
    CHECK_EQ(int(run.r.need_from), int(NeedFrom::Skedb));
    CHECK_EQ(run.r.need_mw, 90000u);
}

void test_mismatch_flag() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000, true));
    t.hold(20000, 2000, 2000);
    TwinHint twin{"Big laptop", kLegionPdos, false, true};
    Run run;
    run.go(t, &twin);
    EXPECT_CODE(run, Code::MismatchFlag, Sev::Warn);
    EXPECT_NO(run, Code::NoMismatchFlag);
    CHECK_COND(run.has_line("twin \"Big laptop\""));
}

void test_pinned() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.hold(20000, 2950, 15000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::Pinned, Sev::Warn);
    EXPECT_NO(run, Code::OverDraw);
}

// The device draws more than its contract (the Legion's reported "20 V 3.25 A contract, draws more").
void test_over_draw_warn() {
    Trace t;
    t.cc(true);
    const std::vector<std::uint32_t> caps{fixed(5000, 3000), fixed(20000, 5000)};
    t.contract(caps, rdo(2, 3250, 3250));
    t.hold(20000, 3000, 5000);
    t.hold(20000, 3600, 8000);   // 3.6 A > 3.25 A * 1.05 + 50 mA = 3.46 A
    t.hold(20000, 3100, 3000);
    Run run;
    run.go(t);
    run.dump("over draw");
    EXPECT_CODE(run, Code::OverDraw, Sev::Warn);
    CHECK_EQ(over_limit_ma(3250), 3462u);
    CHECK_STR(run.lines[0].c_str(), "PD link healthy, draw over contract");
    CHECK_COND(run.has_line("Host draws above its contract: up to 3.6 A on a 3.25 A contract"));
    CHECK_COND(run.has_line("above 3.46 A for 7.9 s from 0:05"));
    EXPECT_NO(run, Code::Pinned);
}

void test_over_draw_within_margin_is_quiet() {
    Trace t;
    t.cc(true);
    const std::vector<std::uint32_t> caps{fixed(5000, 3000), fixed(20000, 5000)};
    t.contract(caps, rdo(2, 3250, 3250));
    t.hold(20000, 3450, 12000);   // under the 3.46 A limit
    Run run;
    run.go(t);
    EXPECT_NO(run, Code::OverDraw);
}

void test_over_draw_brief_is_quiet() {
    Trace t;
    t.cc(true);
    const std::vector<std::uint32_t> caps{fixed(5000, 3000), fixed(20000, 5000)};
    t.contract(caps, rdo(2, 3250, 3250));
    t.hold(20000, 3000, 20000);
    t.hold(20000, 3900, 400);    // a spike: 0.3 s first to last sample, under kOverBurstMs and the share
    t.hold(20000, 3000, 1000);
    Run run;
    run.go(t);
    EXPECT_NO(run, Code::OverDraw);
}

// Short bursts over the contract in a long session (the Legion on a 5 A charger): Warn on the bursts, though their
// share of the time is small. One low sample inside a burst does not split it.
void test_over_draw_bursts() {
    Trace t;
    t.cc(true);
    const std::vector<std::uint32_t> caps{fixed(5000, 3000), fixed(20000, 5000)};
    t.contract(caps, rdo(2, 3250, 3250));
    t.hold(19700, 1960, 60000);
    t.hold(19600, 3540, 1500);
    t.hold(19700, 1960, 40000);
    t.hold(19500, 3790, 1200);
    t.hold(19500, 3300, 100);    // one sample under the limit
    t.hold(19500, 3790, 1200);
    t.hold(19700, 1960, 30000);
    Run run;
    run.go(t);
    run.dump("over draw bursts");
    EXPECT_CODE(run, Code::OverDraw, Sev::Warn);
    CHECK_COND(run.has_line("up to 3.79 A on a 3.25 A contract"));
    CHECK_COND(run.has_line("2 bursts, 1.4-2.4 s, from 1:00"));
    EXPECT_NO(run, Code::VbusLow);
}

// ---- power windows: the fast min/max input (the ring's VBUS records average the extremes away) ----

void test_window_acc() {
    WindowAcc acc;
    Window w;
    CHECK_COND(!acc.add(1000, 20000, 2000, w));
    CHECK_COND(!acc.add(1050, 19500, 0, w));
    CHECK_COND(acc.add(1100, 20100, 4000, w));   // kWindowMs after the first sample: the window closes
    CHECK_EQ(w.n, 3u);
    CHECK_EQ(w.span_ms, 100u);
    CHECK_EQ(w.mv, 19866u);
    CHECK_EQ(int(w.ma), 2000);
    CHECK_EQ(w.vmin, 19500u);
    CHECK_EQ(w.vmax, 20100u);
    CHECK_EQ(int(w.imin), 0);
    CHECK_EQ(int(w.imax), 4000);
    CHECK_COND(!acc.add(1150, 20000, 1000, w));   // a fresh window
}

const std::vector<std::uint32_t> kRgCaps{fixed(5000, 3000), fixed(9000, 3000), fixed(20000, 3500)};

// The device's input drops to 0 inside windows while VBUS holds: the device lets go, the source does not. The ring
// records (rate limited) only ever saw the mean.
void test_windows_input_cuts() {
    Trace t;
    t.cc(true);
    t.contract(kRgCaps, rdo(3, 3250, 3250));
    t.vbus(20000, 2000);
    t.wins_for(5000, 20000, 2000, 19900, 20100, 1900, 2100);
    t.wins_for(2000, 20300, 900, 19900, 20450, 0, 2600);   // 20 windows, each with a cut-out
    t.wins_for(3000, 20000, 2000, 19900, 20100, 1900, 2100);
    Run run;
    run.go(t);
    run.dump("input cuts");
    EXPECT_CODE(run, Code::InputCuts, Sev::Warn);
    CHECK_COND(run.has_line("Host's input cuts out repeatedly"));
    CHECK_COND(run.has_line("0 mA in 20 of 96 windows from 0:05; VBUS stayed up: the device let go"));
    EXPECT_NO(run, Code::VbusLost);
    EXPECT_NO(run, Code::VbusLow);
    EXPECT_NO(run, Code::DrawReduced);
    EXPECT_NO(run, Code::OverDraw);
}

// The same 0 mA, but VBUS goes with it and CC stays attached: the source cut, not the device.
void test_windows_vbus_lost_is_the_source() {
    Trace t;
    t.cc(true);
    t.contract(kRgCaps, rdo(3, 3250, 3250));
    t.vbus(20000, 2000);
    t.wins_for(5000, 20000, 2000, 19900, 20100, 1900, 2100);
    t.wins_for(3000, 0, 0, 0, 0, 0, 0);
    Run run;
    run.go(t);
    run.dump("windows vbus lost");
    EXPECT_CODE(run, Code::VbusLost, Sev::Fail);
    EXPECT_NO(run, Code::InputCuts);
}

// Bursts above even the offer's own maximum: a Fail with VBUS holding. The ring saw nothing above the contract.
void test_windows_far_above_offer() {
    Trace t;
    t.cc(true);
    t.contract(kRgCaps, rdo(3, 3250, 3250));
    t.vbus(19700, 2900);
    t.wins_for(10000, 19700, 2900, 19500, 19900, 2700, 3100);
    t.wins_for(1500, 19300, 3300, 19100, 19800, 2900, 5620);
    t.wins_for(10000, 19700, 2900, 19500, 19900, 2700, 3100);
    Run run;
    run.go(t);
    run.dump("far above offer");
    EXPECT_CODE(run, Code::OverDraw, Sev::Fail);
    CHECK_COND(run.has_line("Host draws far above its contract: up to 5.62 A on a 3.25 A contract"));
    CHECK_COND(run.has_line("offer 3.5 A; above 3.46 A for 1.5 s from 0:10"));
    EXPECT_NO(run, Code::VbusLow);
}

// The band is the spec's 5 %: 18.99 V on a 20 V contract is out, and the note gives the percentage.
void test_windows_band_five_percent() {
    Trace t;
    t.cc(true);
    t.contract(kRgCaps, rdo(3, 3000, 3000));
    t.vbus(20000, 2000);
    t.wins_for(5000, 20000, 2000, 19900, 20100, 1900, 2100);
    t.win(19500, 2900, 18986, 20000, 1900, 3000);
    t.wins_for(5000, 20000, 2000, 19900, 20100, 1900, 2100);
    Run run;
    run.go(t);
    run.dump("band 5 %");
    EXPECT_CODE(run, Code::VbusLow, Sev::Warn);
    CHECK_COND(run.has_line("VBUS fell to 18.99 V on a 20 V contract"));
    CHECK_COND(run.has_line("-5 %, at 3 A, 0:05"));
}

// The real capture: NOVOO RG100 -> K2 -> Legion rear port, 20 V 3.25 A contract. Ring records alone: the bursts show
// only as their means.
void test_fixture_novoo_overdraw_ring_only() {
    auto ss = sessions("tests/fixtures/bbdiag/novoo_legion_overdraw.raw");
    CHECK_EQ(ss.size(), 1u);
    if (ss.empty()) return;
    Run& r = ss[0];
    r.dump("novoo legion overdraw, ring only");
    EXPECT_CODE(r, Code::OverDraw, Sev::Fail);   // a ring sample at 18.81 V after the first over-draw
    CHECK_COND(r.has_line("up to 3.97 A on a 3.25 A contract"));   // the 5.62 A peak is averaged away
    EXPECT_NO(r, Code::InputCuts);   // invisible without the windows
    EXPECT_NO(r, Code::NoGoodCrc);   // the caps before the sink listened are not a link fault
    EXPECT_NO(r, Code::HardReset);
}

// The same capture with its power windows (power.jsonl): the coordinator's readings, 538-637 s under CPU stress
// peak 5620 mA on a 3.5 A offer, 0 mA windows with VBUS up, VBUS down to 18986 mV.
void test_fixture_novoo_overdraw() {
    auto ss = sessions("tests/fixtures/bbdiag/novoo_legion_overdraw.raw", nullptr,
                       "tests/fixtures/bbdiag/novoo_legion_overdraw.power");
    CHECK_EQ(ss.size(), 1u);
    if (ss.empty()) return;
    Run& r = ss[0];
    r.dump("novoo legion overdraw, power windows");
    EXPECT_CODE(r, Code::OverDraw, Sev::Fail);   // above the offer's own 3.5 A
    CHECK_COND(r.has_line("draws far above its contract: up to 5.62 A on a 3.25 A contract"));
    CHECK_COND(r.has_line("offer 3.5 A; "));
    EXPECT_CODE(r, Code::InputCuts, Sev::Warn);
    CHECK_COND(r.has_line("input cuts out repeatedly"));
    CHECK_COND(r.has_line("VBUS stayed up: the device let go"));
    // the lowest reading is a ring sample (18.81 V at 547.6 s, between the windows' 20 Hz samples, whose lowest
    // is 18.99 V): both are real readings
    EXPECT_CODE(r, Code::VbusLow, Sev::Warn);
    CHECK_COND(r.has_line("VBUS fell to 18.81 V on a 20 V contract"));
    CHECK_COND(r.has_line("-6 %"));
    EXPECT_NO(r, Code::VbusLost);                // VBUS stayed up through the cut-outs
    EXPECT_NO(r, Code::DrawReduced);
    CHECK_COND(r.has_line("Contract 20 V 3.25 A"));
    EXPECT_NO(r, Code::NoGoodCrc);
    EXPECT_NO(r, Code::HardReset);
    CHECK_COND(r.has_line("Charger refused a VCONN swap"));
    CHECK_COND(r.has_line("Cable plug refused DP alt mode"));
    EXPECT_CODE(r, Code::VdmLoop, Sev::Info);
    CHECK_COND(r.has_line("Discover_SVIDs asked 2x"));
    EXPECT_NO(r, Code::SoftReset);               // the Soft_Reset was to the cable plug (SOP')
}

void test_over_draw_fail_when_vbus_drops() {
    Trace t;
    t.cc(true);
    const std::vector<std::uint32_t> caps{fixed(5000, 3000), fixed(20000, 5000)};
    t.contract(caps, rdo(2, 3250, 3250));
    t.hold(20000, 3700, 6000);
    t.hold(17500, 3700, 2000);   // below 20 V - 5 % = 19 V
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::OverDraw, Sev::Fail);
    EXPECT_CODE(run, Code::VbusLow, Sev::Warn);
    CHECK_COND(run.has_line("% of the time above 3.46 A from 0:00; then VBUS fell"));
    CHECK_COND(run.has_line("VBUS fell to 17.5 V on a 20 V contract"));
    CHECK_COND(run.has_line("-13 %, at 3.7 A"));
}

void test_over_draw_fail_when_source_resets() {
    Trace t;
    t.cc(true);
    const std::vector<std::uint32_t> caps{fixed(5000, 3000), fixed(20000, 5000)};
    t.contract(caps, rdo(2, 3250, 3250));
    t.hold(20000, 3700, 6000);
    t.reset(0);
    t.hold(5000, 0, 1000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::OverDraw, Sev::Fail);
    EXPECT_CODE(run, Code::HardReset, Sev::Fail);
    CHECK_COND(run.has_line("then a hard reset"));
}

void test_five_volt_only() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(1, 2000, 3000));
    t.rx(true, false, Get_Sink_Cap, {});
    t.rx(false, true, Sink_Cap, {fixed(5000, 3000) | 1u << 28, fixed(9000, 2000)});
    t.hold(5000, 1500, 3000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::FiveVoltOnly, Sev::Warn);
}

void test_draw_reduced_quiet_is_the_device() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.hold(19500, 2100, 6000);
    t.hold(19600, 700, 3000);
    t.hold(19700, 60, 10000);
    t.hold(19400, 2100, 5000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::DrawReduced, Sev::Info);
    CHECK_COND(run.has_line("reduced its draw, contract unchanged"));
    CHECK_COND(run.has_line("2.1 A to 60 mA"));
    CHECK_COND(run.has_line("no PD message, VBUS kept"));
    EXPECT_NO(run, Code::VbusLost);
    CHECK_EQ(run.r.link, 0);
}

// ---- contract not observed: the K2 attached after the contract was made ----

void test_contract_unseen() {
    Trace t;
    t.vbus(4930, 0);
    t.wait(100);
    t.vbus(19740, 0);
    t.wait(700);
    t.cc(true);
    for (int i = 0; i < 3; i++) t.rx(false, false, VDM, {vdm(0xFF00, 0, 1)}, false, kSop1, 0x777, 0);   // retried, never answered
    t.hold(19300, 1950, 8000);
    Run run;
    run.go(t);
    run.dump("contract unseen");
    CHECK_STR(run.lines[0].c_str(), "PD link healthy, contract not observed");
    EXPECT_CODE(run, Code::ContractUnseen, Sev::Info);
    CHECK_COND(run.has_line("Contract not observed (the K2 attached after it)"));
    CHECK_COND(run.has_line("Draws 3"));
    EXPECT_CODE(run, Code::CableSilent, Sev::Info);
    EXPECT_NO(run, Code::NoGoodCrc);
    EXPECT_NO(run, Code::Retries);
}

// ---- contract instability ----

void test_hard_reset() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(2, 2000, 3000));
    t.hold(9000, 500, 1000);
    t.reset(1);
    t.reset(0);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::HardReset, Sev::Fail);
    CHECK_EQ(run.find(Code::HardReset)->a, 2);
    CHECK_COND(run.lines[0].find("unstable") != std::string::npos);
}

void test_soft_reset_sop_only() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(2, 2000, 3000));
    t.rx(true, false, Soft_Reset, {}, true, kSop1);   // the cable plug: not counted
    Run a;
    a.go(t);
    EXPECT_NO(a, Code::SoftReset);
    t.rx(false, true, Soft_Reset, {});
    Run b;
    b.go(t);
    EXPECT_CODE(b, Code::SoftReset, Sev::Warn);
}

void test_soft_reset_by_k2_many_is_fail() {
    Trace t;
    t.cc(true);
    t.rx(true, false, Source_Cap, kHubCaps);
    for (int i = 0; i < 10; i++) { t.tx(false, false, Soft_Reset, {}); t.wait(50); }
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::SoftReset, Sev::Fail);
    CHECK_EQ(run.find(Code::SoftReset)->b, 10);
    CHECK_COND(run.has_line("10 sent by the K2"));
    CHECK_COND(run.r.active);
}

void test_reattach() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(1, 1000, 3000));
    t.cc(false);
    t.wait(500);
    t.cc(true);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::Reattach, Sev::Warn);
    CHECK_EQ(run.a.finished(), 0);
}

void test_detach_finishes_session() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(1, 1000, 3000));
    t.hold(5000, 500, 2000);
    t.cc(false);
    t.wait(4000);
    Run run;
    run.go(t);
    CHECK_EQ(run.a.finished(), 1);
    CHECK_COND(!run.a.current().open);
    CHECK_COND(run.a.has_last());
    CHECK_COND(run.a.last().contract_seen);
}

void test_rejected_and_wait() {
    Trace t;
    t.cc(true);
    t.rx(true, false, Source_Cap, kHubCaps);
    t.rx(false, true, Request, {rdo(4, 3000, 3000)});
    t.rx(true, false, Reject, {});
    for (int i = 0; i < 3; i++) {
        t.rx(false, true, Request, {rdo(4, 3000, 3000)});
        t.rx(true, false, Wait, {});
        t.wait(100);
    }
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::Rejected, Sev::Warn);
    EXPECT_CODE(run, Code::WaitLoop, Sev::Warn);
}

void test_many_requests() {
    Trace t;
    t.cc(true);
    for (int i = 0; i < 7; i++) { t.contract(kHubCaps, rdo(1 + i % 4, 1000, 3000)); t.wait(200); }
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::ManyRequests, Sev::Warn);
}

void test_no_accept_and_late() {
    Trace t;
    t.cc(true);
    t.rx(true, false, Source_Cap, kHubCaps);
    t.rx(false, true, Request, {rdo(2, 1000, 3000)});
    t.wait(1500);   // never answered
    t.contract(kHubCaps, rdo(2, 1000, 3000), true, 120, 700);   // late Accept, late PS_RDY
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::NoAccept, Sev::Warn);
    EXPECT_CODE(run, Code::LateAccept, Sev::Warn);
    EXPECT_CODE(run, Code::LatePsRdy, Sev::Warn);
}

void test_no_psrdy() {
    Trace t;
    t.cc(true);
    t.rx(true, false, Source_Cap, kHubCaps);
    t.rx(false, true, Request, {rdo(2, 1000, 3000)});
    t.rx(true, false, Accept, {});
    t.wait(2500);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::NoPsRdy, Sev::Warn);
}

void test_caps_unanswered() {
    Trace t;
    t.cc(true);
    for (int i = 0; i < 4; i++) { t.rx(true, false, Source_Cap, kHubCaps, false); t.wait(150); }
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::CapsUnanswered, Sev::Fail);
}

// ---- power quality ----

void test_vbus_high() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(2, 1000, 3000));
    t.hold(9000, 500, 1000);
    t.hold(10200, 500, 300);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::VbusHigh, Sev::Warn);
}

void test_vbus_in_band_healthy_line() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(2, 1000, 3000));
    t.hold(8700, 900, 3000);   // 9 V - 5 % - 0.5 V = 8.05 V: in band
    Run run;
    run.go(t);
    EXPECT_NO(run, Code::VbusLow);
    CHECK_COND(run.has_line("VBUS within the contract band"));
}

// VBUS gone with CC still attached: the source cut out (here while drawing more than the contract).
void test_vbus_lost_is_a_cut_out() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.hold(20000, 3300, 1000);
    t.hold(300, 0, 3000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::VbusLost, Sev::Fail);
    CHECK_COND(run.has_line("CC still attached; drew 3.3 A on 3 A: over-current signature"));
}

// VBUS falls, then CC detaches within kUnplugMs: an unplug, not a cut-out.
void test_unplug_is_not_a_cut_out() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.hold(19200, 2000, 2000);
    t.vbus(1300, 0);
    t.wait(570);
    t.cc(false);
    t.wait(4000);
    Run run;
    run.go(t, nullptr, true);
    EXPECT_NO(run, Code::VbusLost);
    CHECK_EQ(run.a.finished(), 1);
}

// VBUS falls through the band over a few samples, then CC detaches: an unplug ramp, not a dip. The monitor's IBUS
// lags below 3.5 V (1.5 A shown at 1.9 V), which must not count as a peak.
void test_unplug_ramp_is_not_a_dip() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.hold(19500, 2000, 2000);
    t.vbus(12000, 1800);
    t.wait(20);
    t.vbus(7000, 1700);
    t.wait(20);
    t.vbus(1900, 2600);
    t.wait(300);
    t.cc(false);
    t.wait(4000);
    Run run;
    run.go(t, nullptr, true);
    EXPECT_NO(run, Code::VbusLow);
    EXPECT_NO(run, Code::VbusLost);
    CHECK_EQ(run.r.peak_ma, 2000);
}

// The same dip with CC kept: a real dip, reported once kUnplugMs has passed.
void test_dip_with_cc_kept_is_reported() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    t.hold(19500, 2000, 2000);
    t.hold(16000, 2000, 300);
    t.hold(19500, 2000, 2000);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::VbusLow, Sev::Warn);
}

// Between a new Request and its PS_RDY VBUS moves to the new level: no band breach against the old contract.
void test_transition_is_not_a_breach() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(1, 2000, 3000));
    t.hold(5000, 100, 2000);
    t.rx(false, true, Request, {rdo(4, 3000, 3000)});
    t.wait(5);
    t.rx(true, false, Accept, {});
    t.vbus(7500, 100);
    t.wait(60);
    t.vbus(14000, 100);
    t.wait(60);
    t.vbus(20800, 100);
    t.wait(60);
    t.rx(true, false, PS_RDY, {});
    t.hold(20000, 100, 3000);
    Run run;
    run.go(t);
    EXPECT_NO(run, Code::VbusLow);
    EXPECT_NO(run, Code::VbusHigh);
    CHECK_COND(run.has_line("VBUS within the contract band"));
}

// The K2's own sink goes idle (a re-read released the contract): the source dropping VBUS then is by design.
void test_k2_release_is_not_a_cut_out() {
    Trace t;
    t.cc(true);
    t.rx(true, false, Source_Cap, kHubCaps);
    t.tx(false, false, Request, {rdo(1, 3000, 3000)});
    t.rx(true, false, Accept, {});
    t.wait(40);
    t.rx(true, false, PS_RDY, {});
    t.hold(5000, 0, 2000);
    t.put(pdtrace::Type::Sink, {std::uint8_t(SinkState::Idle), 0, 0, 0});
    t.hold(300, 0, 2000);
    Run run;
    run.go(t);
    EXPECT_NO(run, Code::VbusLost);
    EXPECT_NO(run, Code::VbusLow);
    EXPECT_CODE(run, Code::ActiveSink, Sev::Info);
}

// The K2's own Requests (an active script) are not the device asking again and again.
void test_k2_requests_are_not_many_requests() {
    Trace t;
    t.cc(true);
    for (int i = 0; i < 8; i++) {
        t.rx(true, false, Source_Cap, kHubCaps);
        t.tx(false, false, Request, {rdo(1, 3000, 3000)});
        t.rx(true, false, Accept, {});
        t.wait(40);
        t.rx(true, false, PS_RDY, {});
        t.hold(5000, 0, 300);
    }
    Run run;
    run.go(t);
    EXPECT_NO(run, Code::ManyRequests);
}

// An active contract on a PDO the capture never saw (the offer fell out of the ring): name the PDO, not "0 V".
void test_active_sink_pdo_not_captured() {
    Trace t;
    t.cc(true);
    t.tx(false, false, Request, {rdo(3, 5000, 5000)});
    t.rx(true, false, Accept, {});
    t.wait(40);
    t.rx(true, false, PS_RDY, {});
    t.hold(20000, 0, 2000);
    Run run;
    run.go(t);
    CHECK_COND(run.has_line("K2 is the sink: contract on PDO 3"));
    CHECK_COND(!run.has_line("0 V 0 mA"));
}

// PD traffic and no VBUS sample at all: say so instead of "VBUS 0 V".
void test_contract_unseen_without_vbus() {
    Trace t;
    t.cc(true);
    t.rx(false, true, PR_Swap, {});
    t.rx(true, false, Reject, {});
    t.wait(500);
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::ContractUnseen, Sev::Info);
    CHECK_COND(run.has_line("no VBUS reading in this capture"));
    CHECK_COND(!run.has_line("VBUS 0 V when"));
}

// ---- link quality ----

void test_no_goodcrc_and_retries() {
    Trace t;
    t.cc(true);
    const auto c = t.rx(true, false, Source_Cap, kHubCaps, false, kSop, -1, 0);
    t.rx(true, false, Source_Cap, kHubCaps, false, kSop, int(c), 0);   // retry: same header and CRC within 20 ms
    t.rx(true, false, Source_Cap, kHubCaps, true, kSop, int(c), 0);
    t.wait(200);
    t.rx(false, true, Request, {rdo(1, 1000, 3000)}, false);   // no GoodCRC ...
    t.rx(false, true, Request, {rdo(1, 900, 3000)}, true);     // ... and a different frame next
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::Retries, Sev::Info);
    CHECK_EQ(run.find(Code::Retries)->a, 2);
    EXPECT_CODE(run, Code::NoGoodCrc, Sev::Info);
    EXPECT_NO(run, Code::CapsUnanswered);
}

void test_tx_fail_and_rx_bad() {
    Trace t;
    t.cc(true);
    t.rx(true, false, Source_Cap, kHubCaps);
    t.tx(false, false, Request, {rdo(1, 1000, 3000)});
    t.tx_result(1);
    for (int i = 0; i < 3; i++) t.rx_bad();
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::TxFail, Sev::Warn);
    EXPECT_CODE(run, Code::RxBad, Sev::Warn);
}

// ---- roles and modes ----

// The laptop's left port (source, DFP) rejects the hub's PR_Swap: it stays the 5 V source.
void test_pr_swap_refused_by_source() {
    Trace t;
    t.vbus(5080, 0);
    t.cc(true);
    t.contract({fixed(5000, 3000) | 1u << 29 | 1u << 26 | 1u << 25}, rdo(1, 0, 0), false);
    t.rx(false, false, PR_Swap, {});
    t.rx(true, true, Reject, {});
    t.hold(5080, 0, 2000);
    Run run;
    run.go(t);
    run.dump("PR_Swap refused");
    EXPECT_CODE(run, Code::SwapRefused, Sev::Info);
    CHECK_COND(run.has_line("Host refuses to take power on this port"));
    CHECK_COND(run.has_line("PR_Swap rejected at 0:00; it stays the 5 V source"));
    CHECK_EQ(run.r.link, 0);
}

void test_pr_swap_accepted_is_quiet() {
    Trace t;
    t.cc(true);
    t.rx(true, true, PR_Swap, {});
    t.rx(false, false, Accept, {});
    t.contract(kHubCaps, rdo(4, 3000, 3000));
    Run run;
    run.go(t);
    EXPECT_NO(run, Code::SwapRefused);
    CHECK_COND(run.has_line("No refused swaps or modes"));
}

void test_swap_unanswered() {
    Trace t;
    t.cc(true);
    t.rx(false, true, DR_Swap, {});
    t.wait(1500);
    t.rx(true, false, VCONN_Swap, {});
    t.rx(false, true, Not_Supported, {});
    Run run;
    run.go(t);
    int n = 0;
    for (unsigned i = 0; i < run.r.n; i++) n += run.r.f[i].code == Code::SwapRefused;
    CHECK_EQ(n, 2);
}

void test_mode_nak() {
    Trace t;
    t.cc(true);
    t.rx(true, true, VDM, {vdm(0xFF01, 0, 4) | 1u << 8});
    t.rx(false, false, VDM, {vdm(0xFF01, 2, 4) | 1u << 8});
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::ModeNak, Sev::Warn);
    CHECK_COND(run.has_line("DP alt mode refused"));
}

void test_enter_usb_refused() {
    Trace t;
    t.cc(true);
    t.rx(true, true, Enter_USB, {2u << 28});
    t.rx(false, false, Reject, {});
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::EnterUsbRefused, Sev::Warn);
    CHECK_COND(!run.r.lanes.eusb);
}

void test_epr_failed() {
    Trace t;
    t.cc(true);
    t.rx(false, false, EPR_Mode, {1u << 24 | 140u << 16});
    t.rx(true, false, EPR_Mode, {4u << 24 | 3u << 16});
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::EprFailed, Sev::Warn);
}

void test_vdm_loop_not_supported() {
    Trace t;
    t.cc(true);
    for (int i = 0; i < 10; i++) {
        t.rx(false, false, VDM, {vdm(0xFF00, 0, 2)});
        t.rx(true, true, Not_Supported, {});
        t.wait(300);
    }
    Run run;
    run.go(t);
    EXPECT_CODE(run, Code::VdmLoop, Sev::Info);
    CHECK_COND(run.has_line("Discover_SVIDs asked 10x"));
    CHECK_COND(run.has_line("answered Not_Supported"));
    CHECK_EQ(run.r.link, 0);
}

// ---- headers, empty and passive-only states ----

void test_no_session() {
    Analyzer a;
    const Report r = evaluate(a.shown(), nullptr, 0);
    CHECK_COND(r.empty);
    Line l;
    CHECK_COND(line(r, 0, l));
    CHECK_STR(l.text.data(), "No session yet");
    CHECK_EQ(line_count(r), 1u);
    char v[40];
    verdict(r, v);
    CHECK_STR(v, "No session yet");
}

void test_no_pd() {
    Trace t;
    t.cc(true);
    t.hold(5100, 400, 3000);
    Run run;
    run.go(t);
    CHECK_STR(run.lines[0].c_str(), "No PD seen, VBUS 5.1 V");
}

void test_boot_mark_finishes() {
    Trace t;
    t.cc(true);
    t.contract(kHubCaps, rdo(1, 1000, 3000));
    t.boot();
    Run run;
    run.go(t);
    CHECK_EQ(run.a.finished(), 1);
}

void test_lines_fit_and_codes_named() {
    for (int c = 0; c <= int(Code::CableSilent); c++) CHECK_COND(std::strcmp(code_name(Code(c)), "?") != 0);
    Report r;
    r.empty = false;
    r.n = 0;
    for (int c = 0; c <= int(Code::CableSilent) && r.n < kMaxFindings; c++) r.f[r.n++] = Finding{Sev::Warn, Group::Power, Code(c), 3600000, 99999, 99999, 99999, 3};
    for (unsigned i = 0; i < line_count(r); i++) {
        Line l;
        if (line(r, i, l)) CHECK_COND(std::strlen(l.text.data()) < kLineMax);
    }
}

void test_ram_budget() {
    std::printf("sizeof Analyzer=%zu Session=%zu Report=%zu\n", sizeof(Analyzer), sizeof(Session), sizeof(Report));
    // two Sessions (the running one and the last finished one) are most of it; the window input added 8 B + state
    CHECK_COND(sizeof(Session) <= 304);
    CHECK_COND(sizeof(Analyzer) <= 748);
}

}  // namespace

int main() {
    test_case1_fixture();
    test_case1_no_twin();
    test_fixture_splitter();
    test_fixture_laptop_hub_idle();
    test_fixture_legion_inline();
    test_fixture_stress_soft_resets();
    test_fixture_fh22();
    test_fixture_oneplus();
    test_fixture_usb3_left_unseen();
    test_need_from_sink_caps();
    test_sink_caps_from_source_are_not_the_need();
    test_need_from_skedb();
    test_mismatch_flag();
    test_pinned();
    test_over_draw_warn();
    test_over_draw_within_margin_is_quiet();
    test_over_draw_brief_is_quiet();
    test_over_draw_bursts();
    test_window_acc();
    test_windows_input_cuts();
    test_windows_vbus_lost_is_the_source();
    test_windows_far_above_offer();
    test_windows_band_five_percent();
    test_fixture_novoo_overdraw_ring_only();
    test_fixture_novoo_overdraw();
    test_over_draw_fail_when_vbus_drops();
    test_over_draw_fail_when_source_resets();
    test_five_volt_only();
    test_draw_reduced_quiet_is_the_device();
    test_contract_unseen();
    test_hard_reset();
    test_soft_reset_sop_only();
    test_soft_reset_by_k2_many_is_fail();
    test_reattach();
    test_detach_finishes_session();
    test_rejected_and_wait();
    test_many_requests();
    test_no_accept_and_late();
    test_no_psrdy();
    test_caps_unanswered();
    test_vbus_high();
    test_vbus_in_band_healthy_line();
    test_vbus_lost_is_a_cut_out();
    test_unplug_is_not_a_cut_out();
    test_unplug_ramp_is_not_a_dip();
    test_dip_with_cc_kept_is_reported();
    test_transition_is_not_a_breach();
    test_k2_release_is_not_a_cut_out();
    test_k2_requests_are_not_many_requests();
    test_active_sink_pdo_not_captured();
    test_contract_unseen_without_vbus();
    test_no_goodcrc_and_retries();
    test_tx_fail_and_rx_bad();
    test_pr_swap_refused_by_source();
    test_pr_swap_accepted_is_quiet();
    test_swap_unanswered();
    test_mode_nak();
    test_enter_usb_refused();
    test_epr_failed();
    test_vdm_loop_not_supported();
    test_no_session();
    test_no_pd();
    test_boot_mark_finishes();
    test_lines_fit_and_codes_named();
    test_ram_budget();
    return test_report("test_pd_diag");
}
