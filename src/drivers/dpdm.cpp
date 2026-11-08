// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/drivers/dpdm.hpp"

#include <initializer_list>

#include "k2/fastcharge/fc_seq.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/fastcharge/vooc.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::dpdm {

using namespace reg;
namespace p = pins;

namespace {

// Core cycle counter (DWT) as the microsecond time base: SysTick cannot be used while interrupts
// are masked. If the DWT does not run on this part, waits fall back to systick::delay_us.
constexpr std::uintptr_t kDemcr = 0xE000EDFC, kDwtCtrl = 0xE0001000, kDwtCyccnt = 0xE0001004;
constexpr std::uint32_t kCyclesPerUs = systick::kSysclkHz / 1'000'000;
constexpr std::uint32_t kResetHoldMs = 500;
constexpr std::uint32_t kMaskedLimitUs = 20'000;

bool g_dwt;
bool g_resetting;
std::uint32_t g_reset_start_ms;

inline std::uint32_t cyc() { return *reinterpret_cast<volatile std::uint32_t*>(kDwtCyccnt); }

void dwt_init() {
    *reinterpret_cast<volatile std::uint32_t*>(kDemcr) |= 1u << 24;   // TRCENA
    *reinterpret_cast<volatile std::uint32_t*>(kDwtCtrl) |= 1u;       // CYCCNTENA
    const std::uint32_t a = cyc();
    for (volatile int i = 0; i < 16; i++) {}
    g_dwt = cyc() != a;
}

using Irq = Critical;   // masks interrupts for the lifetime of the object

void apply(const Pin& a, const Pin& b, PinPair pp) {
    switch (pp.kind) {
    case Kind::Analog:
        a.mode(Mode::Analog);
        b.mode(Mode::Analog);
        break;
    case Kind::Out:
        a.write(pp.a);
        b.write(pp.b);
        a.mode(Mode::OutPp50);
        b.mode(Mode::OutPp50);
        break;
    case Kind::RxPull:
        a.write(pp.a);
        a.mode(Mode::OutPp50);
        b.clear();             // ODR bit 0 selects the pull-down
        b.mode(Mode::InPull);
        break;
    case Kind::In:
        a.mode(Mode::InFloating);
        b.mode(Mode::Analog);
        break;
    case Kind::InBoth:
        a.mode(Mode::InFloating);
        b.mode(Mode::InFloating);
        break;
    }
}

struct HwIo {
    std::uint32_t deadline;
    void set(Drive dp, Drive dm) {
        set_dp(dp);
        set_dm(dm);
    }
    void wait_us(std::uint32_t us) {
        if (g_dwt) {
            deadline += us * kCyclesPerUs;
            while (static_cast<std::int32_t>(cyc() - deadline) < 0) {}
        } else {
            systick::delay_us(us);
        }
    }
};

}  // namespace

void init() {
    crm().APB2EN |= Crm::APB2EN_GPIOA | Crm::APB2EN_GPIOB;
    // Drive the level before switching to output so the pins never glitch through the old state.
    p::sw_pa7.write(kPassiveIdle.pa7);
    p::sw_pb0.write(kPassiveIdle.pb0);
    p::dp_dm_sw1.write(kPassiveIdle.sw1);
    p::dp_dm_sw2.write(kPassiveIdle.sw2);
    for (const Pin& pin : {p::sw_pa7, p::sw_pb0, p::dp_dm_sw1, p::dp_dm_sw2}) pin.mode(Mode::OutPp50);
    dwt_init();
    g_resetting = false;
}

Levels read() {
    return {p::sw_pa7.read(), p::sw_pb0.read(), p::dp_dm_sw1.read(), p::dp_dm_sw2.read()};
}

namespace { bool g_clk_pb0; }
void set_clock_pin(ClockPin c) { g_clk_pb0 = c == ClockPin::Pb0; }
ClockPin clock_pin() { return g_clk_pb0 ? ClockPin::Pb0 : ClockPin::Pa7; }
void set_dp(Drive d) {
    if (d == Drive::Listen && g_clk_pb0) d = Drive::Sniff;   // both D+ pins floating digital inputs
    apply(p::sw_pa7, p::sw_pb0, dp_pins(d));
}
void set_dm(Drive d) { apply(p::dp_dm_sw1, p::dp_dm_sw2, dm_pins(d)); }

void play(std::span<const Step> steps) {
    const std::uint32_t total = fc::total_us(steps);
    if (total > kMaskedLimitUs || !g_dwt) {     // long lists run with interrupts on
        HwIo io{g_dwt ? cyc() : 0};
        fc::replay(steps, io);
        return;
    }
    {
        Irq mask;
        HwIo io{cyc()};
        fc::replay(steps, io);
    }
    // Masked interrupts lose SysTick ticks beyond the one that stays pending: count them back.
    for (std::uint32_t i = 1; i < total / 1000; i++) systick::tick();
}

std::uint32_t cycles() { return cyc(); }

void delay_us(std::uint32_t us) {
    if (!g_dwt) {
        systick::delay_us(us);
        return;
    }
    const std::uint32_t t0 = cyc(), n = us * kCyclesPerUs;
    while (cyc() - t0 < n) {}
}

bool read_dm_bit() { return p::dp_dm_sw2.read(); }

int wait_dm(bool level, std::uint32_t timeout_us) {
    if (!g_dwt) return -1;
    Irq mask;
    const std::uint32_t t0 = cyc(), limit = timeout_us * kCyclesPerUs;
    while (read_dm_bit() != level) {
        if (cyc() - t0 >= limit) return -1;
    }
    return static_cast<int>((cyc() - t0) / kCyclesPerUs);
}

int wait_dp(bool level, std::uint32_t timeout_us) {
    if (!g_dwt) return -1;
    Irq mask;
    const std::uint32_t t0 = cyc(), limit = timeout_us * kCyclesPerUs;
    while (p::sw_pa7.read() != level) {
        if (cyc() - t0 >= limit) return -1;
    }
    return static_cast<int>((cyc() - t0) / kCyclesPerUs);
}

namespace {
inline bool clk() { return g_clk_pb0 ? p::sw_pb0.read() : p::sw_pa7.read(); }

// The vooc::Hw seam over the real pins. Every wait is bounded by the DWT cycle counter.
struct SlaveHw {
    bool last_dm = false;
    bool wait_frame(std::uint32_t timeout_us) {
        const std::uint32_t t0 = cyc(), limit = timeout_us * kCyclesPerUs, quiet = vooc::kFrameGapUs * kCyclesPerUs;
        std::uint32_t high_since = 0;
        bool high = false, armed = false;
        for (;;) {
            const std::uint32_t t = cyc();
            if (t - t0 >= limit) return false;
            if (clk()) {
                if (!high) { high = true; high_since = t; }
                else if (t - high_since >= quiet) armed = true;
            } else {
                if (armed) return true;
                high = armed = false;
            }
        }
    }
    bool wait_fall(std::uint32_t timeout_us) {
        const std::uint32_t t0 = cyc(), limit = timeout_us * kCyclesPerUs;
        while (!clk()) if (cyc() - t0 >= limit) return false;
        while (clk()) {
            last_dm = read_dm_bit();
            if (cyc() - t0 >= limit) return false;
        }
        return true;
    }
    bool wait_rise(std::uint32_t timeout_us) {
        const std::uint32_t t0 = cyc(), limit = timeout_us * kCyclesPerUs;
        while (!clk()) if (cyc() - t0 >= limit) return false;
        return true;
    }
    bool dm_before_fall() const { return last_dm; }
    bool dm() const { return read_dm_bit(); }
    void delay_us(std::uint32_t us) {
        const std::uint32_t t0 = cyc(), n = us * kCyclesPerUs;
        while (cyc() - t0 < n) {}
    }
    void drive_dm(bool level) { set_dm(level ? Drive::V3p3 : Drive::V0); }
    void release_dm() { set_dm(Drive::Sniff); }
};

// SysTick cannot tick while interrupts are masked: count the lost milliseconds back.
struct MaskedSpan {
    Irq mask;
    std::uint32_t t0 = cyc();
    ~MaskedSpan() { for (std::uint32_t i = 1; i < (cyc() - t0) / (kCyclesPerUs * 1000); i++) systick::tick(); }
};
}  // namespace

int rx_command(std::uint32_t first_timeout_us) {
    if (!g_dwt) return -1;
    MaskedSpan m;
    SlaveHw hw;
    return vooc::rx_command(hw, first_timeout_us);
}

bool tx_reply(std::uint16_t word) {
    if (!g_dwt) return false;
    MaskedSpan m;
    SlaveHw hw;
    return vooc::tx_reply(hw, word);
}

EdgeCount count_dp_edges(std::uint32_t window_us) {
    EdgeCount c{0, 0};
    if (!g_dwt) return c;
    MaskedSpan m;
    const std::uint32_t t0 = cyc(), limit = window_us * kCyclesPerUs;
    bool a = p::sw_pa7.read(), b = p::sw_pb0.read();
    while (cyc() - t0 < limit) {
        const bool na = p::sw_pa7.read(), nb = p::sw_pb0.read();
        if (a && !na) c.pa7++;
        if (b && !nb) c.pb0++;
        a = na; b = nb;
    }
    return c;
}

void capture_dm(std::span<std::uint8_t> out, std::uint32_t period_us) {
    if (!g_dwt) {
        for (auto& b : out) b = 0;
        return;
    }
    Irq mask;
    std::uint32_t deadline = cyc();
    for (auto& b : out) {
        b = read_dm_bit() ? 1 : 0;
        deadline += period_us * kCyclesPerUs;
        while (static_cast<std::int32_t>(cyc() - deadline) < 0) {}
    }
    const std::uint32_t total = static_cast<std::uint32_t>(out.size()) * period_us;
    for (std::uint32_t i = 1; i < total / 1000; i++) systick::tick();
}

void discharge() {
    // Both lines to 0 V at once, D+ first.
    set_dp(Drive::V0);
    set_dm(Drive::V0);
}

void start_reset() {
    discharge();
    g_resetting = true;
    g_reset_start_ms = systick::millis();
}

bool reset_busy() { return g_resetting; }

// A new session owns the lines: the reset hold must not rewrite them to passive idle underneath it.
void cancel_reset() { g_resetting = false; }

void poll() {
    if (g_resetting && systick::millis() - g_reset_start_ms >= kResetHoldMs) {
        g_resetting = false;
        set_dp(Drive::V0p6);          // passive idle: PA7=1 PB0=0 PB1=1 PB2=0
        set_dm(Drive::V0p6);
    }
}

}  // namespace k2::dpdm
