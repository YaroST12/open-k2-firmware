// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// HID-cable loop probe, pure half (k2/loopprobe/loop_probe.hpp): the record layout, the flash file, the boot check and the
// step runner over a fake Env that logs every action with the fake clock.
#include "test_util.h"

#include <cstring>
#include <vector>

#include "k2/loopprobe/loop_probe.hpp"

using namespace k2;
using namespace k2::loopprobe;
using dpdm::Drive;

namespace {

struct DriveEvent {
    std::uint32_t t;
    DpState s;
};

class Fake final : public Env {
public:
    std::uint32_t now = 0;
    // What the K2 would read
    bool pm_ok = true, adc_ok = true;
    std::int32_t vbus = 5050, ibus = 180, aux = 5010, dp = 600, dm = 600, cc1 = 410, cc2 = 0;
    std::int32_t vbus_at_hold = -1;          // VBUS once a D+/D- state other than 0/0 is driven (-1: unchanged)
    Hid hid_state{kHidCore, 100, 0};
    Link link_state{1, kLinkInline, 2};
    Emk emk{};
    std::uint8_t cc_refusal = 0;
    bool save_ok = true;
    // What the runner did
    std::vector<DriveEvent> drives;
    std::vector<Load> loads;
    std::vector<std::uint8_t> saved_flags;
    std::vector<std::uint32_t> spins;
    int releases = 0, pull_restores = 0, cc_calls = 0, samples = 0;
    std::vector<bool> pulls;
    std::size_t saves_before_cc = 0;
    Record last_saved{};

    Sample sample() override {
        samples++;
        Sample s;
        s.pm_ok = pm_ok;
        s.vbus_mv = vbus; s.ibus_ma = ibus + (loads.empty() || loads.back() == Load::BlOn || loads.back() == Load::BlOnBusy ? 40 : 0);
        s.adc_ok = adc_ok;
        s.aux_mv = aux; s.dp_mv = dp; s.dm_mv = dm; s.cc1_mv = cc1; s.cc2_mv = cc2;
        return s;
    }
    std::int32_t vbus_fast_mv() override { return pm_ok ? vbus : -1; }
    bool sof = false;                        // a host sends SOFs: the frame counter follows the clock
    Hid hid() override { Hid h = hid_state; if (sof) h.frame = std::uint16_t((hid_state.frame + now) & 0x7FF); return h; }
    Link link() override { return link_state; }
    void load(Load l) override { loads.push_back(l); }
    void spin(std::uint32_t ms) override { spins.push_back(ms); }
    void drive(DpState s) override {
        drives.push_back({now, s});
        if (vbus_at_hold >= 0 && !(s.dp == Drive::V0 && s.dm == Drive::V0)) vbus = vbus_at_hold;
    }
    bool bus_dpdm(std::int32_t& p, std::int32_t& m) override { p = 3000; m = 7; return true; }
    void dpdm_release() override { releases++; }
    PullMethod pull(bool on) override { pulls.push_back(on); return PullMethod::CorePullup; }
    void pull_restore() override { pull_restores++; }
    Emk emarker() override { return emk; }
    void cc_hold(std::span<const std::uint8_t> plan, std::uint16_t, CcResult& out) override {
        cc_calls++;
        if (plan.size() == kCcPlan.size()) saves_before_cc = saved_flags.size();
        out = {};
        out.refusal = cc_refusal;
        if (cc_refusal) return;
        out.sw0 = 0x03;
        out.flags = kCcRestored;
        for (std::size_t i = 0; i < plan.size(); i++) out.rows[i] = {plan[i], 500, 3, 0x11, 5040};
        out.n = std::uint8_t(plan.size());
    }
    bool save(const Record& r) override {
        saved_flags.push_back(r.flags);
        last_saved = r;
        return save_ok;
    }
};

// Polls every 20 ms (the UI tick) until the run ends; returns the end time.
std::uint32_t run(Runner& r, Fake& f, std::uint32_t limit_ms = 60000) {
    while (r.running() && f.now < limit_ms) {
        f.now += 20;
        r.poll(f.now);
    }
    return f.now;
}

Status status(const Record& r, Step s) { return Status(r.at(s).status); }
Why why(const Record& r, Step s) { return Why(r.at(s).why); }

bool is_zero(DpState s) { return s.dp == Drive::V0 && s.dm == Drive::V0; }

}  // namespace

static void test_acc() {
    Acc a;
    CHECK_EQ(a.stat().avg, 0);   // empty: zeros
    for (std::int32_t v : {10, 11, 12, 15}) a.add(v);
    CHECK_EQ(a.stat().avg, 12);  // 48 / 4
    CHECK_EQ(a.stat().min, 10);
    CHECK_EQ(a.stat().max, 15);
    Acc b;
    b.add(-7); b.add(-8);
    CHECK_EQ(b.stat().avg, -8);  // -7.5 rounds away from zero
    Acc c;
    c.add(40000); c.add(-40000);
    CHECK_EQ(c.stat().max, 32767);
    CHECK_EQ(c.stat().min, -32768);
}

static void test_safe_plan() {
    for (const DpState& s : kDpPlan) CHECK_COND(dp_state_safe(s));
    CHECK_COND(!dp_state_safe({Drive::V3p3, Drive::V0p6}));   // QC2 9 V
    CHECK_COND(!dp_state_safe({Drive::V0p6, Drive::V0p6}));   // QC2 12 V
    CHECK_COND(!dp_state_safe({Drive::V3p3, Drive::V3p3}));   // QC2 20 V
    CHECK_COND(!dp_state_safe({Drive::V0p6, Drive::V3p3}));   // QC3 entry
    CHECK_COND(!dp_state_safe({Drive::V2p0, Drive::V0}));     // not in the documented set
    CHECK_COND(dp_state_safe({Drive::V0p6, Drive::V0}));      // QC2 5 V: allowed
}

static Record sample_record() {
    Record r;
    r.flags = kFlagInProgress;
    std::memcpy(r.fw.data(), "1.0.0-515-67ce114d", 18);
    r.start_ms = 0x01020304;
    for (std::size_t i = 0; i < kSteps; i++) r.step[i] = {std::uint8_t(i % 5), std::uint8_t(i), std::uint32_t(1000 * i + 7), std::uint16_t(300 + i)};
    r.base_n = 16;
    for (std::size_t c = 0; c < kChannels; c++) r.base[c] = {std::int16_t(5000 - int(c)), std::int16_t(-100 - int(c)), std::int16_t(7000 + int(c))};
    r.hid0 = {kHidCore | kHidRxDp, 0x7FF, 3};
    r.hid1 = {kHidCore | kHidPinDm, 0x123, 4};
    r.link = {2, kLinkInline | kLinkCaps, 1};
    r.bc = 0x15;
    r.drop_n = 8;
    for (std::size_t i = 0; i < r.drop.size(); i++)
        r.drop[i] = {std::uint8_t(kDropPlan[i]), 6, {5040, 5030, 5050}, {std::int16_t(-180 - int(i)), -200, -150}, 4990};
    r.dp_n = 6;
    for (std::size_t i = 0; i < r.dp.size(); i++) r.dp[i] = {std::uint8_t(0x12 + i), 3300, -5, std::uint8_t(i), 5061};
    r.pull_method = std::uint8_t(PullMethod::GpioPull);
    r.pull_n = 2;
    r.pull[0] = {0, 12, 13, 0x10};
    r.pull[1] = {1, 3290, 14, 0x14};
    r.emk.valid = 1; r.emk.num_vdos = 5;
    r.emk.vdos = {0xFF00A041, 0, 0x12345678, 0x000A0640, 0xDEADBEEF, 0x80000001};
    r.emk.seq = 0xBEEF; r.emk.read_mode = 2;
    r.cc.refusal = 0; r.cc.sw0 = 0x07; r.cc.pc = 0x06; r.cc.flags = kCcRestored; r.cc.n = 5;
    for (std::size_t i = 0; i < r.cc.rows.size(); i++) r.cc.rows[i] = {kCcPlan[i], std::uint16_t(400 + i), std::uint16_t(4095 - i), 0x1A, 5010};
    return r;
}

static void test_codec_round_trip() {
    const Record r = sample_record();
    std::uint8_t buf[kRecordBytes + 4] = {};
    const std::size_t n = encode(r, buf);
    CHECK_EQ(n, kRecordBytes);
    CHECK_EQ(buf[0], kVersion);
    CHECK_EQ(buf[1], kFlagInProgress);
    CHECK_EQ(buf[2], '1');                                     // fw starts at +2
    CHECK_EQ(buf[32] | buf[33] << 8 | buf[34] << 16 | buf[35] << 24, 0x01020304);
    CHECK_EQ(buf[kRecordBytes - 1], 5010 >> 8);                // the last field: the last CC row's vbus_mv, high byte
    const auto d = decode(std::span<const std::uint8_t>(buf, n));
    CHECK_COND(d.has_value());
    std::uint8_t again[kRecordBytes] = {};
    CHECK_EQ(encode(*d, again), kRecordBytes);
    CHECK_EQ(std::memcmp(buf, again, kRecordBytes), 0);
    CHECK_EQ(d->emk.vdos[5], 0x80000001u);
    CHECK_EQ(d->drop[3].ibus.avg, -183);
    CHECK_EQ(d->pull[1].dp_mv, 3290);
    CHECK_EQ(d->cc.pc, 0x06);
    CHECK_EQ(d->cc.sw0, 0x07);

    CHECK_EQ(encode(r, std::span<std::uint8_t>(buf, kRecordBytes - 1)), 0u);   // too small
    CHECK_COND(!decode(std::span<const std::uint8_t>(buf, n - 1)).has_value());  // wrong length
    buf[0] = 2;
    CHECK_COND(!decode(std::span<const std::uint8_t>(buf, n)).has_value());      // unknown version
}

static void test_file() {
    const Record r = sample_record();
    std::uint8_t f[kFileBytes] = {};
    CHECK_EQ(encode_file(r, f), kFileBytes);
    CHECK_EQ(f[0], 'L'); CHECK_EQ(f[1], 'P'); CHECK_EQ(f[2], 'R'); CHECK_EQ(f[3], kVersion);
    CHECK_EQ(f[4] | f[5] << 8, int(kRecordBytes));
    CHECK_COND(decode_file(f).has_value());
    f[kFileHeader + 40] ^= 1;                                  // one bit of the record
    CHECK_COND(!decode_file(f).has_value());
}

namespace {
class MemStore final : public Store {
public:
    std::vector<std::uint8_t> data;
    bool fail = false;
    bool read(std::span<std::uint8_t> out, std::size_t& n) override {
        n = 0;
        if (data.empty() || data.size() > out.size()) return false;
        std::memcpy(out.data(), data.data(), data.size());
        n = data.size();
        return true;
    }
    bool write(std::span<const std::uint8_t> in) override {
        if (fail) return false;
        data.assign(in.begin(), in.end());
        return true;
    }
};
}  // namespace

static void test_boot_check() {
    MemStore s;
    CHECK_COND(!boot_check(s).has_value());                    // nothing stored
    Record r = sample_record();                                 // in progress, CC "not run"
    r.at(Step::Cc) = {std::uint8_t(Status::NotRun), 0, 5000, 0};
    CHECK_COND(save(s, r));
    const auto st = boot_check(s);
    CHECK_COND(st.has_value() && *st == Step::Cc);
    const auto back = load(s);
    CHECK_COND(back.has_value());
    CHECK_EQ(back->flags & kFlagInProgress, 0);
    CHECK_EQ(int(status(*back, Step::Cc)), int(Status::Interrupted));
    CHECK_EQ(int(why(*back, Step::Cc)), int(Why::Rebooted));
    CHECK_COND(!boot_check(s).has_value());                    // closed: nothing more to do

    s.data[kFileHeader + 10] ^= 0xFF;                          // a damaged file is no record
    CHECK_COND(!load(s).has_value());
    CHECK_COND(!boot_check(s).has_value());
}

static void test_start_refusals() {
    const char* fw = "1.0.0-1-abc";
    {
        Fake f; f.pm_ok = false;
        Runner r(f);
        CHECK_EQ(int(r.start(0, fw)), int(Start::NoMonitor));
        CHECK_COND(!r.running());
    }
    {
        Fake f; f.vbus = 5501;
        Runner r(f);
        CHECK_EQ(int(r.start(0, fw)), int(Start::VbusHigh));
        CHECK_COND(f.drives.empty() && f.loads.empty() && f.saved_flags.empty());
    }
    {
        Fake f; f.aux = 5801;
        Runner r(f);
        CHECK_EQ(int(r.start(0, fw)), int(Start::AuxHigh));
    }
    {
        Fake f;
        Runner r(f);
        CHECK_EQ(int(r.start(0, fw)), int(Start::Ok));
        CHECK_EQ(int(r.start(0, fw)), int(Start::Running));
    }
}

static void test_full_run() {
    Fake f;
    f.emk.valid = 1; f.emk.num_vdos = 4; f.emk.vdos[0] = 0xFF00A041;
    Runner r(f);
    f.now = 1000;
    CHECK_EQ(int(r.start(f.now, "1.0.0-515-67ce114d")), int(Start::Ok));
    run(r, f);
    CHECK_COND(!r.running());
    CHECK_COND(r.finished());
    const Record& rec = r.record();
    for (std::size_t i = 0; i < kSteps; i++) CHECK_EQ(rec.step[i].status, int(Status::Done));
    CHECK_STR(rec.fw.data(), "1.0.0-515-67ce114d");
    CHECK_EQ(rec.start_ms, 1000u);
    // Steps in order, each after the previous one.
    for (std::size_t i = 1; i < kSteps; i++) CHECK_COND(rec.step[i].t_ms >= rec.step[i - 1].t_ms + rec.step[i - 1].dur_ms);
    // Baseline
    CHECK_EQ(rec.base_n, kBaseSamples);
    CHECK_EQ(rec.base[kVbus].avg, 5050);
    CHECK_EQ(rec.base[kAux].avg, 5010);
    CHECK_EQ(rec.base[kCc1].max, 410);
    CHECK_EQ(rec.hid0.bits & kHidCore, kHidCore);
    CHECK_EQ(rec.hid1.frame, rec.hid0.frame);                  // no host: no SOF moves the frame counter
    CHECK_EQ(rec.link.active_cc, 1);
    CHECK_EQ(rec.bc, 0x11);                                    // the as-found CC hold's BC_LVL
    // Drop: the plan, forward then reverse, and the backlight on again at the end
    CHECK_EQ(rec.drop_n, int(kDropPlan.size()));
    CHECK_EQ(f.loads.size(), kDropPlan.size() + 1);
    for (std::size_t i = 0; i < kDropPlan.size(); i++) {
        CHECK_EQ(int(f.loads[i]), int(kDropPlan[i]));
        CHECK_EQ(rec.drop[i].load, int(kDropPlan[i]));
        CHECK_EQ(rec.drop[i].n, kDropSamples);
    }
    CHECK_EQ(int(f.loads.back()), int(Load::BlOn));
    CHECK_EQ(rec.drop[0].ibus.avg, 220);                       // the fake adds 40 mA with the backlight on
    CHECK_EQ(rec.drop[1].ibus.avg, 180);
    CHECK_EQ(f.spins.size(), 4u * kDropSamples);               // four busy levels
    // D+/D-: every state framed by 0/0 for kDischargeMs, held kHoldMs (well under the 1 s HVDCP handshake), never unsafe
    CHECK_EQ(rec.dp_n, int(kDpPlan.size()));
    std::size_t states = 0;
    for (std::size_t i = 0; i < f.drives.size(); i++) {
        const DriveEvent& e = f.drives[i];
        CHECK_COND(dp_state_safe(e.s));
        if (is_zero(e.s)) continue;
        if (e.s.dp == Drive::HiZ && e.s.dm == Drive::HiZ && i + 1 == f.drives.size()) continue;   // the release for the pull rows
        CHECK_COND(i > 0 && is_zero(f.drives[i - 1].s) && e.t - f.drives[i - 1].t >= kDischargeMs);
        if (i + 1 < f.drives.size()) {
            CHECK_COND(f.drives[i + 1].t - e.t >= kHoldMs);
            CHECK_COND(f.drives[i + 1].t - e.t < 1000);
        }
        states++;
    }
    CHECK_EQ(states, kDpPlan.size() - 1);                      // the 0/0 row of the plan is itself a discharge
    for (std::size_t i = 0; i < kDpPlan.size(); i++) {
        CHECK_EQ(rec.dp[i].drive, std::uint8_t(std::uint8_t(kDpPlan[i].dp) << 4 | std::uint8_t(kDpPlan[i].dm)));
        CHECK_EQ(rec.dp[i].dp_mv, 3000);
        CHECK_EQ(rec.dp[i].vbus_max, 5050);
    }
    CHECK_EQ(rec.pull_n, 2);
    CHECK_EQ(f.pulls.size(), 2u);
    CHECK_COND(!f.pulls[0] && f.pulls[1]);
    CHECK_EQ(f.pull_restores, 1);
    CHECK_EQ(f.releases, 1);
    CHECK_EQ(rec.pull_method, int(PullMethod::CorePullup));
    // E-marker
    CHECK_EQ(rec.emk.vdos[0], 0xFF00A041u);
    // CC: the in-progress record was on flash before the hold, the final one after it
    CHECK_EQ(f.cc_calls, 2);                                   // baseline (as found) and the CC step
    CHECK_EQ(f.saves_before_cc, 1u);
    CHECK_EQ(f.saved_flags.size(), 2u);
    CHECK_EQ(f.saved_flags[0], kFlagInProgress);
    CHECK_EQ(f.saved_flags[1], 0);
    CHECK_EQ(rec.cc.n, int(kCcPlan.size()));
    CHECK_EQ(rec.cc.flags, kCcRestored);
    CHECK_EQ(f.last_saved.cc.n, int(kCcPlan.size()));
}

static void test_vbus_rise_ends_the_run() {
    Fake f;
    f.vbus_at_hold = 9000;                                      // a charger that answered a D+/D- state with 9 V
    Runner r(f);
    r.start(0, "x");
    run(r, f);
    const Record& rec = r.record();
    CHECK_EQ(int(status(rec, Step::Baseline)), int(Status::Done));
    CHECK_EQ(int(status(rec, Step::DpDm)), int(Status::Aborted));
    CHECK_EQ(int(why(rec, Step::DpDm)), int(Why::VbusHigh));
    CHECK_EQ(int(status(rec, Step::Emarker)), int(Status::NotRun));
    CHECK_EQ(int(status(rec, Step::Cc)), int(Status::NotRun));
    CHECK_EQ(f.cc_calls, 1);                                   // only the baseline's as-found look
    CHECK_COND(is_zero(f.drives.back().s));                    // both lines to 0 V at once
    CHECK_EQ(f.releases, 1);
    CHECK_EQ(f.saved_flags.size(), 1u);
    CHECK_EQ(f.saved_flags[0], 0);
}

static void test_host_on_hid() {
    Fake f;
    f.hid_state.bits = kHidCore | kHidConfigured;
    Runner r(f);
    r.start(0, "x");
    run(r, f);
    const Record& rec = r.record();
    CHECK_EQ(int(status(rec, Step::Drop)), int(Status::Done));
    CHECK_EQ(int(status(rec, Step::DpDm)), int(Status::Skipped));
    CHECK_EQ(int(why(rec, Step::DpDm)), int(Why::HostOnHid));
    CHECK_EQ(int(status(rec, Step::Cc)), int(Status::Skipped));
    CHECK_EQ(int(why(rec, Step::Cc)), int(Why::HostOnHid));
    CHECK_COND(f.drives.empty());
    CHECK_EQ(f.cc_calls, 1);

    Fake g;                                                     // not configured (yet), but SOFs arrive: a host all the same
    g.sof = true;
    Runner q(g);
    q.start(0, "x");
    run(q, g);
    CHECK_COND(q.record().hid1.frame != q.record().hid0.frame);
    CHECK_EQ(int(why(q.record(), Step::DpDm)), int(Why::HostOnHid));
    CHECK_COND(g.drives.empty());
}

static void test_no_vbus() {
    Fake f;
    f.vbus = 30;                                                // a charger waiting for a sink: no VBUS
    Runner r(f);
    CHECK_EQ(int(r.start(0, "x")), int(Start::Ok));
    run(r, f);
    const Record& rec = r.record();
    CHECK_EQ(int(status(rec, Step::Drop)), int(Status::Skipped));
    CHECK_EQ(int(why(rec, Step::Drop)), int(Why::NoVbus));
    CHECK_EQ(int(status(rec, Step::DpDm)), int(Status::Done));
    CHECK_EQ(int(status(rec, Step::Cc)), int(Status::Skipped));
    CHECK_EQ(int(why(rec, Step::Cc)), int(Why::NoVbus));
    CHECK_COND(f.loads.empty());
}

static void test_store_failure_skips_cc() {
    Fake f;
    f.save_ok = false;
    Runner r(f);
    r.start(0, "x");
    run(r, f);
    CHECK_EQ(int(status(r.record(), Step::Cc)), int(Status::Skipped));
    CHECK_EQ(int(why(r.record(), Step::Cc)), int(Why::StoreFailed));
    CHECK_EQ(f.cc_calls, 1);
}

static void test_cc_refused() {
    Fake f;
    f.cc_refusal = std::uint8_t(CcRefusal::Contract);
    Runner r(f);
    r.start(0, "x");
    run(r, f);
    const Record& rec = r.record();
    CHECK_EQ(int(status(rec, Step::Cc)), int(Status::Skipped));
    CHECK_EQ(int(why(rec, Step::Cc)), int(Why::CcRefused));
    CHECK_EQ(rec.cc.refusal, int(CcRefusal::Contract));
    CHECK_EQ(rec.bc, 0);                                       // the baseline's look was refused as well
    CHECK_EQ(f.saved_flags.back(), 0);
}

static void test_no_emarker() {
    Fake f;
    Runner r(f);
    r.start(0, "x");
    run(r, f);
    CHECK_EQ(int(status(r.record(), Step::Emarker)), int(Status::Skipped));
    CHECK_EQ(int(why(r.record(), Step::Emarker)), int(Why::NoEmarker));
    CHECK_EQ(int(status(r.record(), Step::Cc)), int(Status::Done));
}

static void test_cancel_restores() {
    Fake f;
    Runner r(f);
    r.start(0, "x");
    while (r.running() && r.step() != Step::Drop) { f.now += 20; r.poll(f.now); }
    for (int i = 0; i < 30; i++) { f.now += 20; r.poll(f.now); }   // into the busy levels
    r.cancel(f.now);
    CHECK_COND(!r.running());
    CHECK_EQ(int(status(r.record(), Step::Drop)), int(Status::Aborted));
    CHECK_EQ(int(why(r.record(), Step::Drop)), int(Why::Cancelled));
    CHECK_EQ(int(f.loads.back()), int(Load::BlOn));
    CHECK_EQ(f.saved_flags.size(), 1u);
    r.cancel(f.now);                                           // no-op when idle
    CHECK_EQ(f.saved_flags.size(), 1u);

    Fake g;                                                    // cancel inside the D+/D- step: lines released
    Runner q(g);
    q.start(0, "x");
    while (q.running() && q.step() != Step::DpDm) { g.now += 20; q.poll(g.now); }
    for (int i = 0; i < 8; i++) { g.now += 20; q.poll(g.now); }
    q.cancel(g.now);
    CHECK_EQ(g.releases, 1);
    CHECK_EQ(int(status(q.record(), Step::DpDm)), int(Status::Aborted));
}

static void test_aux_rise_ends_the_run() {
    Fake f;
    Runner r(f);
    r.start(0, "x");
    for (int i = 0; i < 5; i++) { f.now += 20; r.poll(f.now); }
    f.aux = 6000;
    run(r, f);
    CHECK_EQ(int(status(r.record(), Step::Baseline)), int(Status::Aborted));
    CHECK_EQ(int(why(r.record(), Step::Baseline)), int(Why::AuxHigh));
    CHECK_COND(f.drives.empty());
}

static void test_names() {
    CHECK_STR(step_name(Step::DpDm), "D+/D-");
    CHECK_STR(step_name(Step::Cc), "CC");
    CHECK_STR(why_name(Why::HostOnHid), "host on HID");
    CHECK_STR(why_name(Why(200)), "?");
}

// The driver's pure half: the SWITCHES0 value per plan entry and the plan check.
static void test_cc_switches() {
    using namespace k2::pd::phy;
    const std::uint8_t passive = kMeasCc1;                       // inline default: no Rd, measure on CC1
    CHECK_EQ(cc_sw0(passive, kCcAsFound), passive);
    CHECK_EQ(cc_sw0(passive, 0x01), passive | kPdwn1);
    CHECK_EQ(cc_sw0(passive, 0x02), passive | kPdwn2);
    CHECK_EQ(cc_sw0(passive, 0x00), passive);
    const std::uint8_t both = kPdwn1 | kPdwn2 | kMeasCc2;        // a self-powered boot: Rd on both lines
    CHECK_EQ(cc_sw0(both, 0x01), kPdwn1 | kMeasCc2);             // drop CC2's Rd, keep the measure switch
    CHECK_EQ(cc_sw0(both, 0x02), kPdwn2 | kMeasCc2);
    CHECK_EQ(cc_sw0(both, 0x00), kMeasCc2);
    CHECK_EQ(cc_sw0(both, kCcAsFound), both);

    CHECK_COND(cc_plan_ok(kCcPlan));
    const std::uint8_t both_lines[] = {kCcAsFound, 0x03};
    CHECK_COND(!cc_plan_ok(both_lines));                         // Rd on both lines is never asked for
    const std::uint8_t vconn[] = {0x10};
    CHECK_COND(!cc_plan_ok(vconn));
    CHECK_COND(!cc_plan_ok(std::span<const std::uint8_t>{}));
    const std::uint8_t too_long[kCcRows + 1] = {};
    CHECK_COND(!cc_plan_ok(too_long));
}

int main() {
    test_cc_switches();
    test_acc();
    test_safe_plan();
    test_codec_round_trip();
    test_file();
    test_boot_check();
    test_start_refusals();
    test_full_run();
    test_vbus_rise_ends_the_run();
    test_host_on_hid();
    test_no_vbus();
    test_store_failure_skips_cc();
    test_cc_refused();
    test_no_emarker();
    test_cancel_restores();
    test_aux_rise_ends_the_run();
    test_names();
    return test_report("test_loop_probe");
}
