// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The loop probe's Env and Store on the K2, see k2/loopprobe/loop_probe_hw.hpp.
#include "k2/loopprobe/loop_probe_hw.hpp"

#include "k2/core/log.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/dpdm.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/pd/link.hpp"

namespace k2::loopprobe::hw {
namespace {

std::int32_t vbus_now() {
    const auto v = power::vbus_fast_mv();
    return v ? *v : -1;
}

class HwEnv final : public Env {
public:
    Sample sample() override {
        Sample s;
        if (const auto m = power::sample_now()) {
            s.pm_ok = true;
            s.vbus_mv = m->vbus_mv;
            s.ibus_ma = m->current_ma;
        }
        if (const auto a = adc::read()) {
            s.adc_ok = true;
            s.aux_mv = a->aux_mv;
            s.dp_mv = a->dp_mv;
            s.dm_mv = a->dm_mv;
            s.cc1_mv = a->cc1_mv;
            s.cc2_mv = a->cc2_mv;
        }
        return s;
    }
    std::int32_t vbus_fast_mv() override { return vbus_now(); }
    Hid hid() override {
        const usb::Lines l = usb::lines();
        Hid h;
        h.bits = std::uint8_t((l.core ? kHidCore : 0) | (l.configured ? kHidConfigured : 0) | (l.rx_dp ? kHidRxDp : 0) |
                              (l.rx_dm ? kHidRxDm : 0) | (l.pin_dp ? kHidPinDp : 0) | (l.pin_dm ? kHidPinDm : 0));
        h.frame = l.frame;
        const std::uint32_t r = usb::stats_snapshot().resets;
        h.resets = std::uint16_t(r > 0xFFFF ? 0xFFFF : r);
        return h;
    }
    Link link() override {
        Link k;
        k.active_cc = fusb302::active_cc();
        const pd::SinkState ss = fusb302::sink_status().state;
        k.flags = std::uint8_t((fusb302::inline_mode() ? kLinkInline : 0) | (fusb302::sink_downstream() ? kLinkDownstream : 0) |
                               (fusb302::caps_this_attach() ? kLinkCaps : 0) | (fusb302::contract().valid ? kLinkContract : 0) |
                               (ss != pd::SinkState::Idle && ss != pd::SinkState::Failed ? kLinkSinkContract : 0));
        k.guard = std::uint8_t(link::inline_now());
        return k;
    }
    void load(Load l) override { lcd::set_backlight(l == Load::BlOn || l == Load::BlOnBusy); }
    void spin(std::uint32_t ms) override {
        // CPU busy (no WFI), the PD chip and the USB stack still served every millisecond.
        const std::uint32_t t0 = systick::millis();
        std::uint32_t last = t0;
        while (systick::millis() - t0 < ms) {
            const std::uint32_t now = systick::millis();
            if (now != last) {
                last = now;
                fusb302::service();
                usb::service();
            }
        }
    }
    void drive(DpState s) override {
        dpdm::set_dp(s.dp);
        dpdm::set_dm(s.dm);
    }
    bool bus_dpdm(std::int32_t& dp_mv, std::int32_t& dm_mv) override { return adc::latest_dpdm(dp_mv, dm_mv); }
    void dpdm_release() override { dpdm::start_reset(); }
    PullMethod pull(bool on) override {
        switch (usb::dp_pullup(on)) {
        case usb::Pull::Core: return PullMethod::CorePullup;
        case usb::Pull::Gpio: return PullMethod::GpioPull;
        default:              return PullMethod::None;
        }
    }
    void pull_restore() override { usb::dp_pullup_restore(); }
    Emk emarker() override {
        Emk e;
        const pd::Emarker& m = fusb302::emarker();
        e.valid = m.valid ? 1 : 0;
        e.num_vdos = m.num_vdos > e.vdos.size() ? std::uint8_t(e.vdos.size()) : m.num_vdos;
        for (std::size_t i = 0; i < e.num_vdos; i++) e.vdos[i] = m.raw_vdos[i];
        e.seq = std::uint16_t(fusb302::cable_seq());
        e.read_mode = std::uint8_t(fusb302::last_cable_read().mode);
        return e;
    }
    void cc_hold(std::span<const std::uint8_t> plan, std::uint16_t hold_us, CcResult& out) override {
        fusb302::loop_cc_hold(plan, hold_us, vbus_now, out);
    }
    bool save(const Record& r) override { return loopprobe::save(store(), r); }
};

// A write that held the main loop longer than this is named in the log.
constexpr std::uint32_t kSlowMs = 150;

class FsStore final : public Store {
public:
    bool read(std::span<std::uint8_t> out, std::size_t& n) override {
        n = 0;
        return fs::instance().read_file(kFile, out, n) == fs::Err::Ok;
    }
    bool write(std::span<const std::uint8_t> in) override {
        auto& f = fs::instance();
        const std::uint32_t t0 = systick::millis();
        f.mkdir(kDir);   // an existing directory is fine
        const fs::Err e = f.write_file(kFile, in);
        const std::uint32_t ms = systick::millis() - t0;
        if (e != fs::Err::Ok) {
            K2LOG("[LOOP] write failed fs=%s ms=%u\n", fs::err_name(e), static_cast<unsigned>(ms));
            return false;
        }
        if (ms > kSlowMs) K2LOG("[LOOP] slow write ms=%u\n", static_cast<unsigned>(ms));
        return true;
    }
};

HwEnv g_env;
FsStore g_store;

}  // namespace

Env& env() { return g_env; }
Store& store() { return g_store; }

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

}  // namespace k2::loopprobe::hw
