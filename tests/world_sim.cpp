// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD world of tests/world_sim.h: virtual clock, FUSB302 / INA226 / ADC fakes and the scenario reader.
#include "world_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <span>

#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/i2c.hpp"
#include "k2/drivers/pin_mark.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"

namespace world {
namespace {

const Scenario* g_sc;
Timing g_t;
double g_us;          // firmware clock, microseconds since the run started
double g_t0;          // recording ms at g_us == 0
std::size_t g_next;   // next scenario frame to put on the wire
std::vector<FrameFate> g_fates;
std::vector<LogLine> g_logs;
unsigned g_tx_starts;
bool g_in_spend;

// ---- FUSB302 ----
std::uint8_t g_reg[256];
struct Queued { int frame; std::size_t left; };   // a frame in the RX FIFO: bytes not yet popped
std::deque<std::uint8_t> g_rx;
std::deque<Queued> g_rxq;
constexpr std::size_t kRxFifo = 80;

enum : std::uint8_t {
    kDeviceId = 0x01, kSwitches0 = 0x02, kSwitches1 = 0x03, kMeasure = 0x04, kControl0 = 0x06, kControl1 = 0x07,
    kPower = 0x0B, kReset = 0x0C, kStatus0A = 0x3C, kStatus1A = 0x3D, kInterruptA = 0x3E, kInterruptB = 0x3F,
    kStatus0 = 0x40, kStatus1 = 0x41, kInterrupt = 0x42, kFifo = 0x43,
};

void chip_defaults() {   // power-on / SW_RES values of the registers the model reads (inferred)
    std::fill(std::begin(g_reg), std::end(g_reg), std::uint8_t{0});
    g_reg[kDeviceId] = 0x91;
    g_reg[kSwitches0] = 0x03;   // Rd on both lines
    g_reg[kSwitches1] = 0x20;
    g_reg[kMeasure] = 0x31;
    g_reg[kControl0] = 0x24;
    g_reg[kPower] = 0x01;
}

void flush_rx(const char* why) {
    for (const Queued& q : g_rxq)
        if (!g_fates[q.frame].read) { g_fates[q.frame].delivered = false; g_fates[q.frame].lost = why; }
    g_rx.clear();
    g_rxq.clear();
}

double world_now() { return g_t0 + g_us / 1000.0; }

template <class Step> const Step* step_at(const std::vector<Step>& v, double t) {
    const Step* cur = nullptr;
    for (const Step& s : v) { if (s.t > t) break; cur = &s; }
    return cur;
}

double vbus_mv_at(double t) {
    const auto& v = g_sc->vbus;
    if (v.empty()) return 0;
    if (t <= v.front().t) return v.front().mv;
    if (t >= v.back().t) return v.back().mv;
    const auto it = std::upper_bound(v.begin(), v.end(), t, [](double x, const VbusPoint& p) { return x < p.t; });
    const VbusPoint& b = *it;
    const VbusPoint& a = *(it - 1);
    return b.t == a.t ? b.mv : a.mv + (b.mv - a.mv) * (t - a.t) / (b.t - a.t);
}

int line_level(int line, double t) {
    const RpStep* s = step_at(g_sc->rp, t);
    return s ? (line == 1 ? s->cc1 : s->cc2) : 0;
}

void put_on_wire(int i) {
    const Frame& f = g_sc->frames[i];
    FrameFate& fate = g_fates[i];
    const std::uint8_t tok = f.raw.empty() ? 0 : f.raw[0] & 0xE0;
    char why[96] = {};
    const std::uint8_t sw1 = g_reg[kSwitches1], c1 = g_reg[kControl1], pw = g_reg[kPower];
    if (!(sw1 & (f.line == 1 ? 0x01 : 0x02)))
        std::snprintf(why, sizeof why, "unrouted: SWITCHES1=%02X (TXCC%d off)", sw1, f.line);
    else if (!(pw & 0x02)) std::snprintf(why, sizeof why, "receiver off: POWER=%02X", pw);
    else if (tok == 0xC0 && !(c1 & 0x01)) std::snprintf(why, sizeof why, "SOP' not enabled: CONTROL1=%02X", c1);
    else if (tok == 0xA0 && !(c1 & 0x02)) std::snprintf(why, sizeof why, "SOP'' not enabled: CONTROL1=%02X", c1);
    else if (g_rx.size() + f.raw.size() > kRxFifo) std::snprintf(why, sizeof why, "RX FIFO overflow (%zu bytes queued)", g_rx.size());
    if (why[0]) { fate.lost = why; return; }
    for (std::uint8_t b : f.raw) g_rx.push_back(b);
    g_rxq.push_back({i, f.raw.size()});
    fate.delivered = true;
    k2::fusb302::on_int_n();
}

// Everything the world does up to firmware time `until_us`, against the register state as it is now.
void world_until(double until_us) {
    const double until = g_t0 + until_us / 1000.0;
    while (g_next < g_sc->frames.size() && g_sc->frames[g_next].t <= until) put_on_wire(int(g_next++));
}

std::uint8_t status0() {
    const std::uint8_t sw0 = g_reg[kSwitches0];
    const int meas = (sw0 & 0x0C) == 0x04 ? 1 : (sw0 & 0x0C) == 0x08 ? 2 : 0;
    const int mv = meas ? line_level(meas, world_now()) : 0;
    const std::uint8_t bc = mv < 200 ? 0 : mv < 660 ? 1 : mv < 1230 ? 2 : 3;
    const bool comp = mv > (g_reg[kMeasure] & 0x3F) * 42;
    return std::uint8_t(bc | (comp ? 0x20 : 0));   // VBUSOK reads 0 on this board (fusb302.cpp), ACTIVITY / CRC_CHK not modelled
}

std::uint8_t fusb_read(std::uint8_t reg) {
    switch (reg) {
    case kStatus0: return status0();
    case kStatus1: return std::uint8_t((g_rx.empty() ? 0x20 : 0x00) | 0x08);   // RX_EMPTY, TX_EMPTY
    case kInterrupt: case kInterruptA: case kInterruptB: case kStatus0A: case kStatus1A: return 0;
    case kFifo: {
        if (g_rx.empty()) return 0;
        const std::uint8_t b = g_rx.front();
        g_rx.pop_front();
        Queued& q = g_rxq.front();
        FrameFate& f = g_fates[q.frame];
        if (!f.read) { f.read = true; f.fw_read_ms = g_us / 1000.0; }
        if (--q.left == 0) g_rxq.pop_front();
        return b;
    }
    default: return g_reg[reg];
    }
}

void fusb_write(std::uint8_t reg, std::uint8_t v) {
    switch (reg) {
    case kFifo: return;   // TX FIFO: what the K2 would send (counted at TX_START)
    case kReset:
        if (v & 0x01) { chip_defaults(); flush_rx("flushed: SW_RES"); }
        else if (v & 0x02) flush_rx("flushed: PD_RESET");
        return;
    case kControl0:
        if (v & 0x01) ++g_tx_starts;
        g_reg[reg] = std::uint8_t(v & ~0x41);   // TX_START / TX_FLUSH self-clear
        return;
    case kControl1:
        if (v & 0x04) flush_rx("flushed: RX_FLUSH");
        g_reg[reg] = std::uint8_t(v & ~0x04);
        return;
    default: g_reg[reg] = v;
    }
}

// ---- INA226 ----
double g_mv_per_count;
constexpr std::uint16_t kInaDefault = 0x4127;   // power-on configuration
std::uint16_t g_ina_cfg = kInaDefault;
// The bus register holds the average of the last complete conversion period (AVG x (VBUSCT + VSHCT) in continuous mode),
// so a VBUS edge shows up to one period late and smoothed: 4.48 ms at the driver's 0x4407, 280 us in its fast mode.
double ina_bus_mv() {
    static constexpr double kCt[] = {140, 204, 332, 588, 1100, 2116, 4156, 8244};
    static constexpr int kAvg[] = {1, 4, 16, 64, 128, 256, 512, 1024};
    const double period_ms = kAvg[(g_ina_cfg >> 9) & 7] * (kCt[(g_ina_cfg >> 6) & 7] + kCt[(g_ina_cfg >> 3) & 7]) / 1000.0;
    const double end = std::floor(world_now() / period_ms) * period_ms;
    double sum = 0;
    for (int i = 0; i < 8; i++) sum += vbus_mv_at(end - period_ms * (i + 0.5) / 8);
    return sum / 8;
}
std::uint16_t ina_read(std::uint8_t reg) {
    switch (reg) {
    case 0xFF: return 0x2260;   // die id: INA226
    case 0x00: return g_ina_cfg;
    case 0x02: return std::uint16_t(std::lround(std::max(0.0, ina_bus_mv()) / g_mv_per_count));
    default: return 0;          // shunt 0 (no load), and no INA228 / INA237 ids
    }
}
void ina_write(std::uint8_t reg, std::span<const std::uint8_t> d) {
    if (reg != 0x00 || d.size() < 2) return;
    const std::uint16_t v = std::uint16_t(d[0] << 8 | d[1]);
    g_ina_cfg = (v & 0x8000) ? kInaDefault : v;
}

std::map<std::pair<unsigned, unsigned>, bool> g_pins;

void on_log(const char* line) { g_logs.push_back({world_now(), line}); }

}  // namespace

void spend_us(double us) {
    if (us <= 0) return;
    const double target = g_us + us;
    if (!g_in_spend) {
        g_in_spend = true;
        world_until(target);
        g_in_spend = false;
    }
    g_us = target;
}

double world_ms() { return world_now(); }
double fw_ms() { return g_us / 1000.0; }
void wfi() { spend_us(1000.0 - std::fmod(g_us, 1000.0)); }
const std::vector<FrameFate>& fates() { return g_fates; }
const std::vector<LogLine>& logs() { return g_logs; }
unsigned tx_starts() { return g_tx_starts; }

namespace {
LcdTiming g_lcd;
void lcd_spend(double us) { spend_us(us); }
double lcd_now() { return g_us; }
}  // namespace

void start(const Scenario& sc, double world_t0_ms, const Timing& t) {
    g_sc = &sc;
    g_t = t;
    g_us = 0;
    g_t0 = world_t0_ms;
    g_fates.assign(sc.frames.size(), {});
    for (std::size_t i = 0; i < sc.frames.size(); i++) g_fates[i].index = int(i);
    g_next = 0;
    while (g_next < sc.frames.size() && sc.frames[g_next].t < world_t0_ms) g_fates[g_next++].lost = "before the run";
    g_logs.clear();
    g_tx_starts = 0;
    chip_defaults();
    g_rx.clear();
    g_rxq.clear();
    g_mv_per_count = k2::power::raw_to_mv(k2::power::Chip::Ina226, k2::power::align_vbus(k2::power::Chip::Ina226, 10000)) / 10000.0;
    g_ina_cfg = kInaDefault;
    g_pins.clear();
    for (const auto& p : {k2::pins::key_plus, k2::pins::key_minus, k2::pins::key_ok, k2::pins::key_back})
        g_pins[{unsigned(p.port), p.n}] = true;   // released (active low)
    g_stub.on_log = on_log;
    g_lcd = {lcd_spend, lcd_now, t.px_us, t.lcd_call_us, t.lcd_window_us, t.px_cpu_us, 468, 2048};
    g_lcd_timing = &g_lcd;
}

// ---- scenario file ----
const char* sop_name(std::uint8_t token) {
    switch (token & 0xE0) { case 0xE0: return "SOP"; case 0xC0: return "SOP'"; case 0xA0: return "SOP''"; default: return "?"; }
}

const char* msg_name(const std::vector<std::uint8_t>& raw) {
    static const char* const ctrl[] = {"?", "GoodCRC", "GotoMin", "Accept", "Reject", "Ping", "PS_RDY", "Get_Source_Cap",
        "Get_Sink_Cap", "DR_Swap", "PR_Swap", "VCONN_Swap", "Wait", "Soft_Reset", "Data_Reset", "Data_Reset_Complete",
        "Not_Supported", "Get_Source_Cap_Extended", "Get_Status", "FR_Swap", "Get_PPS_Status", "Get_Country_Codes",
        "Get_Sink_Cap_Extended", "Get_Source_Info", "Get_Revision"};
    static const char* const data[] = {"?", "Source_Capabilities", "Request", "BIST", "Sink_Capabilities", "Battery_Status",
        "Alert", "Get_Country_Info", "Enter_USB", "EPR_Request", "EPR_Mode", "Source_Info", "Revision", "?", "?", "Vendor_Defined"};
    if (raw.size() < 3) return "?";
    const unsigned h = raw[1] | raw[2] << 8, type = h & 0x1F, n = (h >> 12) & 7;
    if (h & 0x8000) return "Extended";
    if (n) return type < std::size(data) ? data[type] : "Data?";
    return type < std::size(ctrl) ? ctrl[type] : "Control?";
}

bool load_scenario(const std::string& path, Scenario& out, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path; return false; }
    out = {};
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream s(line);
        std::string kw;
        if (!(s >> kw)) continue;
        auto bad = [&](const char* what) { err = path + ":" + std::to_string(n) + ": " + what; return false; };
        if (kw == "scenario") { int v; if (!(s >> v) || v != 1) return bad("scenario version must be 1"); }
        else if (kw == "name") s >> out.name;
        else if (kw == "recording") { std::string fw; s >> out.recording >> fw >> out.fw; }
        else if (kw == "template" || kw == "line") {}
        else if (kw == "mode") s >> out.mode;
        else if (kw == "vbus") { VbusPoint p; if (!(s >> p.t >> p.mv)) return bad("vbus <t> <mv>"); out.vbus.push_back(p); }
        else if (kw == "rp") { RpStep p; if (!(s >> p.t >> p.cc1 >> p.cc2)) return bad("rp <t> <cc1> <cc2>"); out.rp.push_back(p); }
        else if (kw == "adc") { AdcStep p; if (!(s >> p.t >> p.cc1 >> p.cc2 >> p.dp >> p.dm)) return bad("adc <t> cc1 cc2 dp dm"); out.adc.push_back(p); }
        else if (kw == "frame") {
            Frame f; std::string hex, src;
            if (!(s >> f.t >> f.line >> hex >> src) || hex.size() % 2 || (f.line != 1 && f.line != 2)) return bad("frame <t> <line> <hex> rec|tpl");
            for (std::size_t i = 0; i < hex.size(); i += 2) f.raw.push_back(std::uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
            f.tpl = src == "tpl";
            out.frames.push_back(std::move(f));
        } else if (kw == "episode") {
            Episode e; std::string k1, k2;
            if (!(s >> e.name >> e.t0 >> e.t1 >> k1 >> e.line >> k2 >> e.rise) || k1 != "line" || k2 != "rise")
                return bad("episode <name> <t0> <t1> line <n> rise <t>");
            out.episodes.push_back(e);
        } else if (kw == "expect") {
            Expect x; std::string c;
            if (!(s >> x.episode)) return bad("expect <episode> <checks>");
            while (s >> c) {
                if (c.rfind("first_sop=", 0) == 0) x.first_sop = c.substr(10);
                else if (c.rfind("caps>=", 0) == 0) x.caps_min = std::stoi(c.substr(6));
                else if (c.rfind("verdict_caps>=", 0) == 0) x.verdict_caps_min = std::stoi(c.substr(14));
                else if (c.rfind("verdict=", 0) == 0) x.verdict = c.substr(8);
                else return bad("unknown check");
            }
            out.expects.push_back(x);
        } else return bad("unknown keyword");
    }
    auto by_t = [](const auto& a, const auto& b) { return a.t < b.t; };
    std::stable_sort(out.vbus.begin(), out.vbus.end(), by_t);
    std::stable_sort(out.rp.begin(), out.rp.end(), by_t);
    std::stable_sort(out.adc.begin(), out.adc.end(), by_t);
    std::stable_sort(out.frames.begin(), out.frames.end(), by_t);
    return true;
}

}  // namespace world

// ---- the firmware's view of the world ----
namespace k2 {

namespace systick {
namespace { void (*g_wait_hook)(); void (*g_delay_hook)(); void (*g_tick_hook)(); }
void init() {}
void tick() {}
std::uint32_t millis() { world::spend_us(world::g_t.clock_read_us); return std::uint32_t(world::g_us / 1000.0); }
std::uint32_t micros() { world::spend_us(world::g_t.clock_read_us); return std::uint32_t(world::g_us); }
std::uint32_t ticks() { world::spend_us(world::g_t.clock_read_us); return std::uint32_t(std::fmod(world::g_us * 240.0, 4294967296.0)); }
void set_delay_hook(void (*hook)()) { g_delay_hook = hook; }
void set_wait_hook(void (*hook)()) { g_wait_hook = hook; }
void set_tick_hook(void (*hook)()) { g_tick_hook = hook; }
void delay_us(std::uint32_t us) { world::spend_us(us * world::g_t.delay_us_factor); }
void delay_ms(std::uint32_t ms) {   // systick.cpp: per ms, delay_us(1000), usb::service(), the delay and wait hooks
    while (ms--) {
        delay_us(1000);
        if (g_delay_hook) g_delay_hook();
        if (g_wait_hook) g_wait_hook();
    }
}
}  // namespace systick

namespace i2c {
void init() {}
bool write(Bus bus, std::uint8_t dev, std::uint8_t reg, std::span<const std::uint8_t> d) {
    using namespace world;
    if (bus == Bus::Fusb) {
        spend_us((2 + d.size()) * g_t.fusb_byte_us);
        if (reg == kFifo) return true;
        for (std::size_t i = 0; i < d.size(); i++) fusb_write(std::uint8_t(reg + i), d[i]);
        return true;
    }
    if (dev != power::kI2cAddr) { spend_us(g_t.ina_byte_us); return false; }
    spend_us((2 + d.size()) * g_t.ina_byte_us);
    ina_write(reg, d);
    return true;
}
bool read(Bus bus, std::uint8_t dev, std::uint8_t reg, std::span<std::uint8_t> out) {
    using namespace world;
    if (bus == Bus::Fusb) {
        spend_us((3 + out.size()) * g_t.fusb_byte_us);
        for (std::size_t i = 0; i < out.size(); i++) out[i] = fusb_read(reg == kFifo ? reg : std::uint8_t(reg + i));
        return true;
    }
    if (dev != power::kI2cAddr) { spend_us(g_t.ina_byte_us); return false; }
    spend_us((3 + out.size()) * g_t.ina_byte_us);
    const std::uint16_t v = ina_read(reg);
    if (!out.empty()) out[0] = std::uint8_t(v >> 8);
    if (out.size() > 1) out[1] = std::uint8_t(v);
    return true;
}
}  // namespace i2c

namespace reg {
bool host_pin(Port p, std::uint8_t n, HostPin op, unsigned) {
    bool& v = world::g_pins[{unsigned(p), n}];
    if (op == HostPin::Set) v = true;
    else if (op == HostPin::Clear) v = false;
    return v;
}
}  // namespace reg

namespace pinmark {
namespace { std::uint16_t g_mark; }
std::uint16_t read() { return g_mark; }
void write(std::uint16_t raw) { g_mark = raw; }
}  // namespace pinmark

namespace adc {
namespace {
const world::AdcStep* now_step() { return world::step_at(world::g_sc->adc, world::world_now()); }
std::uint16_t raw12(int mv) { return std::uint16_t(std::clamp((mv * 4096 + 1650) / 3300, 0, 4095)); }
}  // namespace
bool init() { return true; }
void poll() {}
std::optional<Reading> read() {
    const world::AdcStep* s = now_step();
    if (!s) return std::nullopt;
    Reading r;
    r.cc1_mv = s->cc1; r.cc2_mv = s->cc2; r.dp_mv = s->dp; r.dm_mv = s->dm;
    r.vdd_mv = 3300;
    return r;
}
bool latest_cc(std::uint16_t& cc1, std::uint16_t& cc2) {
    const world::AdcStep* s = now_step();
    if (!s) return false;
    cc1 = raw12(s->cc1);
    cc2 = raw12(s->cc2);
    return true;
}
bool latest_dpdm(std::int32_t& dp, std::int32_t& dm) {
    const world::AdcStep* s = now_step();
    if (!s) return false;
    dp = s->dp;
    dm = s->dm;
    return true;
}
}  // namespace adc

// ---- the parts of the main loop the world leaves out ----
// USB, the drive, the shell and the network have no host on the other end; the stores (session record, black box, boot trace)
// have no flash; the D+/D- side (fastcharge, dpsniff, fctrig) is not modelled yet (docs/TESTING.md "World replay", sink and QC);
// the transition capture's poll is the delay_ms() wait hook and records nothing here.
namespace usb { void log_stats() {} }
namespace shell_hw { void poll() {} }
namespace session { void poll() {} }
namespace rdcomeback { void poll() {} }
namespace power_source { void poll(std::uint32_t) {} }
namespace net { void log_stats() {} }
namespace msc { void poll() {} bool io() { return false; } }
namespace http_server { void service() {} void log_stats() {} }
namespace handback { void poll(std::uint32_t) {} }
namespace fctrig { void service_ufcs() {} void service_hs() {} }
namespace fault { void trigger(std::uint8_t) {} }
namespace fastcharge { unsigned init() { return 0; } unsigned log_idle() { return 0; } void poll() {} }
namespace dpsniff { void poll(std::uint32_t, std::int32_t) {} }
namespace boottrace { void flush_log() {} }
namespace bootscreen { void draw_live(std::uint32_t) {} }
namespace bootinfo { bool boot_key_held() { return false; } }
namespace bbpersist { void poll() {} }
namespace transcap { void poll() {} }

}  // namespace k2

extern "C" std::uint32_t k2_stack_free_bytes() { return 4096; }
extern "C" std::uint32_t k2_msp_free_bytes() { return 1024; }
