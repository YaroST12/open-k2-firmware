// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The battery efficiency recorder on the K2: see k2/battery/battery_hw.hpp.
#include "k2/battery/battery_hw.hpp"

#include <new>

#include "k2/core/log.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/power_monitor.hpp"

namespace k2::battery::hw {

namespace {
// Not zeroed at reset (.noinit, link.ld): a warm reset finds the last checkpoint here; its magic and CRC say whether it is one.
// Raw bytes, not a Checkpoint object: Recorder's member initializers would give that a static constructor, which runs before
// main and would wipe the checkpoint it is meant to keep.
__attribute__((section(".noinit"))) alignas(Service::Checkpoint) std::uint8_t g_ckpt_raw[sizeof(Service::Checkpoint)];
alignas(Service) std::uint8_t g_service_raw[sizeof(Service)];
bool g_service_inited;
State g_logged = State::Idle;
bool g_failed_logged = false;

Service &service_obj() {
    if (!g_service_inited) {
        new (g_service_raw) Service();
        g_service_inited = true;
    }
    return *std::launder(reinterpret_cast<Service *>(g_service_raw));
}

const char *state_name(State s) {
    switch (s) {
    case State::Idle: return "idle";
    case State::Measuring: return "measuring";
    case State::Between: return "between";
    case State::Done: return "done";
    }
    return "?";
}
}  // namespace

Service &service() { return service_obj(); }

bool monitor_online() { return power::read_raw().has_value(); }

void init(bool warm_reset) {
    fs::Fs &f = fs::instance();
    auto *ckpt = std::launder(reinterpret_cast<Service::Checkpoint *>(g_ckpt_raw));   // trivially copyable: bytes are the object
    Service &svc = service_obj();
    const Service::Boot b = svc.boot(&f, ckpt, warm_reset);
    static constexpr const char *kBoot[] = {"nothing", "resumed", "waiting", "lost", "closed"};
    const Recorder &r = svc.rec();
    K2LOG("[BATT] boot %s run=%u resumes=%u\n", kBoot[static_cast<int>(b)], static_cast<unsigned>(r.id()),
          static_cast<unsigned>(r.resumes()));
    g_logged = r.state();
}

void poll(std::uint32_t now_ms, bool may_write) {
    const auto m = power::read_raw();
    Service &svc = service_obj();
    svc.poll(now_ms, m ? m->vbus_mv : 0, m ? m->current_ma : 0, m.has_value(), may_write);
    const Recorder &r = svc.rec();
    if (r.state() != g_logged) {
        g_logged = r.state();
        const Phase &p = r.phase(r.current());
        K2LOG("[BATT] run=%u %s phase=%d end=%u s=%u in_uwh=%u out_uwh=%u\n", static_cast<unsigned>(r.id()), state_name(g_logged),
              r.phases(), static_cast<unsigned>(p.end), static_cast<unsigned>(p.ms / 1000u),
              static_cast<unsigned>(math::div_u64_to_u32(p.uj_in, 3600u)),
              static_cast<unsigned>(math::div_u64_to_u32(p.uj_out, 3600u)));
    }
    if (svc.write_failed() != g_failed_logged) {
        g_failed_logged = svc.write_failed();
        K2LOG("[BATT] record write %s run=%u\n", g_failed_logged ? "failed" : "ok", static_cast<unsigned>(r.id()));
    }
}

}  // namespace k2::battery::hw
