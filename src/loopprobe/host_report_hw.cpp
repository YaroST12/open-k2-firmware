// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The loop probe's Host report on the K2, see k2/loopprobe/host_report_hw.hpp. Built with K2_HOST_REPORT=1 only.
#include "k2/loopprobe/host_report_hw.hpp"

#include <array>

#include "k2/core/log.hpp"
#include "k2/core/version.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::loopprobe::host::hw {
namespace {

constexpr const char* kDir = "/lp";

class LineEnv final : public Env {
public:
    bool send(const pd::TxMsg& m) override { return fusb302::loop_host_send(m); }
    void set_dfp(bool dfp) override { fusb302::loop_host_set_dfp(dfp); }
    bool soft_reset() override { return fusb302::loop_host_soft_reset(); }
};

LineEnv g_env;
Seq g_seq(g_env);
usbfp::Recorder g_usb;
Report g_rep;
std::array<std::uint8_t, kFileBytes> g_buf;

enum class St : std::uint8_t { Boot, Idle, Waiting, Running };
St g_st = St::Boot;
bool g_resume = false;          // the session to start continues the journal's facts
bool g_detached = false;        // the driver's on_detach (the line went away)
bool g_suspended = false;       // the last bus event was a suspend (ISTR keeps the flag until the stack clears it)
std::uint32_t g_boot_ms = 0;
std::uint16_t g_frame = 0;      // SOF: the frame counter moves
bool g_frame_known = false;

class Line final : public fusb302::LoopHost {
public:
    void on_message(std::uint16_t header, std::span<const std::uint8_t> p, std::uint32_t now) override {
        g_seq.on_message(header, p, now);
    }
    void on_hard_reset(std::uint32_t now) override {
        K2LOG("[LPHR] host Hard Reset (role back to UFP)\n");
        g_seq.on_hard_reset(now);
    }
    void on_detach(std::uint32_t) override { g_detached = true; }
};
Line g_line;

// From the USB interrupt: copy only.
void tap(usb::BusEvent e, const std::uint8_t* setup8) {
    const std::uint32_t now = systick::millis();
    switch (e) {
    case usb::BusEvent::Setup: {
        std::uint8_t p[8];
        for (int i = 0; i < 8; i++) p[i] = setup8[i];
        g_usb.setup(p, now);
        g_suspended = false;
        break;
    }
    case usb::BusEvent::Reset:
        g_usb.bus_reset(now);
        g_suspended = false;
        break;
    case usb::BusEvent::Suspend:
        if (!g_suspended) g_usb.suspend(now);
        g_suspended = true;
        break;
    case usb::BusEvent::Resume:
        if (g_suspended) g_usb.resume(now);
        g_suspended = false;
        break;
    }
}

bool save() {
    g_rep.usb = g_usb.trace();
    const std::size_t n = encode_file(g_rep, g_buf);
    if (n == 0) return false;
    auto& f = fs::instance();
    const std::uint32_t t0 = systick::millis();
    f.mkdir(kDir);
    const fs::Err e = f.write_file(kFile, {g_buf.data(), n});
    K2LOG("[LPHR] saved state=%u end=%s ok=%d ms=%u\n", unsigned(g_rep.state), end_name(End(g_rep.end)),
          e == fs::Err::Ok ? 1 : 0, unsigned(systick::millis() - t0));
    return e == fs::Err::Ok;
}

void load() {
    std::size_t n = 0;
    if (fs::instance().read_file(kFile, g_buf, n) != fs::Err::Ok) return;
    if (!decode_file({g_buf.data(), n}, g_rep)) g_rep = {};
}

LoopFacts facts() {
    LoopFacts f;
    const usb::Lines l = usb::lines();
    f.usb_core = l.core;
    if (l.core) {
        if (g_frame_known && l.frame != g_frame) g_usb.flag(usbfp::kFlagSof);
        g_frame = l.frame;
        g_frame_known = true;
    }
    f.hid_host = l.configured || (g_usb.trace().flags & usbfp::kFlagSof);
    if (const auto v = power::vbus_fast_mv()) f.vbus_mv = *v;
    if (const auto m = power::sample_now()) f.ibus_ma = m->current_ma;
    f.active_cc = fusb302::active_cc();
    f.far_request = fusb302::far_sink_seen();
    f.own_rd = fusb302::pin_rd() != 0 || fusb302::rd_for_run_on() || fusb302::ra_active();
    f.busy = fusb302::run_owner() || fusb302::interrogate_running() || fusb302::sink_status().held;
    f.contract = !g_seq.running() && fusb302::sink_status().state != pd::SinkState::Idle;
    return f;
}

void close(End why, Refuse r) {
    g_rep.state = std::uint8_t(JState::Done);
    g_rep.end = std::uint8_t(why);
    g_rep.refuse = std::uint8_t(r);
    g_rep.os = std::uint8_t(usbfp::classify(g_usb.trace()));
    g_st = St::Idle;
    save();
}

void begin(std::uint32_t now) {
    std::int32_t dp = -1, dm = -1;
    adc::latest_dpdm(dp, dm);   // passive: the host's idle D+/D- (a BC1.2 hint only)
    const auto vb = power::vbus_fast_mv();
    g_rep.start_ms = now;
    g_rep.cc = fusb302::active_cc();
    g_rep.rp = fusb302::loop_host_rp();
    g_rep.vbus_mv = std::int16_t(vb ? *vb : -1);
    g_rep.dp_mv = std::int16_t(dp);
    g_rep.dm_mv = std::int16_t(dm);
    g_rep.refuse = 0;
    g_detached = false;
    fusb302::loop_host_begin(&g_line);
    g_seq.start(now, g_resume ? &g_rep.pd : nullptr);
    g_st = St::Running;
    K2LOG("[LPHR] start boot=%u resume=%d cc=%u rp=%u vbus=%d\n", unsigned(g_rep.boots), g_resume ? 1 : 0, unsigned(g_rep.cc),
          unsigned(g_rep.rp), int(g_rep.vbus_mv));
}

void on_boot(std::uint32_t now) {
    g_boot_ms = now;
    load();
    const Boot b = boot_decide(g_rep);
    if (b == Boot::Nothing) { g_st = St::Idle; return; }
    K2LOG("[LPHR] boot: %s (boots=%u)\n", b == Boot::Start ? "start" : b == Boot::Resume ? "resume" : "closed",
          unsigned(g_rep.boots));
    if (b == Boot::Closed) {
        g_st = St::Idle;
        save();
        return;
    }
    g_resume = b == Boot::Resume;
    if (!g_resume) {
        g_rep.pd = {};
        for (std::size_t i = 0; i < kFwLen - 1 && kFwVersion[i]; i++) g_rep.fw[i] = kFwVersion[i];
    }
    g_usb.clear();
    usb::set_tap(tap);   // the enumeration of this boot, from its first request
    save();
    g_st = St::Waiting;
}

void finish(std::uint32_t now) {
    g_rep.pd = g_seq.facts();
    close(g_seq.end(), Refuse::None);
    K2LOG("[LPHR] done end=%s os=%s id=%u svids=%u hr=%u sr=%u\n", end_name(g_seq.end()), usbfp::os_name(usbfp::Os(g_rep.os)),
          unsigned(g_rep.pd.n_id), unsigned(g_rep.pd.n_svids), unsigned(g_rep.pd.hard_resets), unsigned(g_rep.pd.soft_resets));
    (void)now;
}

}  // namespace

void poll(std::uint32_t now) {
    switch (g_st) {
    case St::Boot: on_boot(now); return;
    case St::Idle: return;
    case St::Waiting: {
        const Refuse r = refusal(facts());
        if (r == Refuse::None) return begin(now);
        if (transient(r) && now - g_boot_ms < kStartWaitMs) return;
        K2LOG("[LPHR] not started: %s\n", refuse_name(r));
        return close(End::NoLoop, r);
    }
    case St::Running: break;
    }
    if (g_detached) g_seq.stop(End::Detached, now);
    else if (fusb302::far_sink_seen()) g_seq.stop(End::ForeignSink, now);
    else {
        facts();   // the SOF flag
        g_seq.on_contract(fusb302::sink_status().state == pd::SinkState::Ready, now);
        const bool settled = now - g_usb.last_ms() >= kUsbQuietMs || g_usb.trace().total == 0;
        g_seq.tick(now, fusb302::loop_host_idle(), settled);
        if (g_seq.take_checkpoint()) {   // before the DR_Swap: the swap goes out on the next tick
            g_rep.pd = g_seq.facts();
            save();
        }
    }
    if (!g_seq.running()) finish(now);
}

Refuse arm() {
    if (g_st == St::Waiting || g_st == St::Running) return Refuse::Busy;
    const Refuse r = refusal(facts());
    if (r != Refuse::None) return r;
    g_rep = {};
    g_rep.state = std::uint8_t(JState::Armed);
    for (std::size_t i = 0; i < kFwLen - 1 && kFwVersion[i]; i++) g_rep.fw[i] = kFwVersion[i];
    g_usb.clear();
    save();
    K2LOG("[LPHR] armed: replug the host end of the loop\n");
    return Refuse::None;
}

void cancel() {
    if (g_st == St::Running) {
        g_seq.stop(End::Cancelled, systick::millis());
        finish(systick::millis());
    } else if (g_st == St::Waiting || JState(g_rep.state) == JState::Armed) {
        close(End::Cancelled, Refuse::None);
    }
}

const Report& report() { return g_rep; }
bool active() { return g_st == St::Waiting || g_st == St::Running || JState(g_rep.state) == JState::Armed; }
bool running() { return g_st == St::Running; }

std::uint16_t file_len() {
    std::size_t n = 0;
    if (fs::instance().size_of(kFile, n) != fs::Err::Ok || n > kFileBytes) return 0;
    return std::uint16_t(n);
}

std::size_t file_read(std::uint16_t off, std::span<std::uint8_t> out) {
    std::size_t n = 0;
    if (fs::instance().read_at(kFile, off, out, n) != fs::Err::Ok) return 0;
    return n;
}

}  // namespace k2::loopprobe::host::hw
