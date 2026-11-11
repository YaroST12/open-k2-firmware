// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD world of tests/world_sim.h: virtual clock, FUSB302 / INA226 / ADC fakes, the scenario reader, and the reboot world
// (a charger on one CC line, the K2's supply rail, brown-out and power-on of the MCU and the FUSB302).
#include "world_sim.h"

#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <new>
#include <optional>
#include <sstream>
#include <span>

#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/chargertest/session_store.hpp"
#include "k2/core/fs.hpp"
#include "k2/core/log.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/i2c.hpp"
#include "k2/drivers/pin_mark.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"

namespace world {
namespace {

constexpr std::size_t kRxFifo = 80;
constexpr std::size_t kFlashBytes = std::size_t(k2::fs::kFlashBlocks) * k2::fs::kBlockSize;
constexpr std::size_t kLogBytes = 1u << 20;
constexpr int kHist = 512;           // VBUS samples kept for the INA226's averaging window (reboot world)
constexpr double kStepMs = 0.05;     // reboot-world physics step
constexpr double kSafe5vMv = 4750;   // vSafe5V (min): the charger's PD starts, its detach watch counts
constexpr int kRdMv = 1680, kOpenMv = 3300;   // an Rp 3 A into one Rd / an Rp with no Rd on the line (trace2scenario OPEN_RP_MV)
constexpr double kAdcFirstRingUs = 15000;     // adc::init() to the first full ring (src/core/rd_comeback.cpp host_powered_now)
constexpr int kMaxLives = 20;
constexpr std::size_t kNoinitMax = 1024;      // room for the executable's .noinit (the SRAM probe is 128 B); noinit_found() checks it fits

enum : std::uint8_t {
    kDeviceId = 0x01, kSwitches0 = 0x02, kSwitches1 = 0x03, kMeasure = 0x04, kControl0 = 0x06, kControl1 = 0x07,
    kControl3 = 0x09, kPower = 0x0B, kReset = 0x0C, kStatus0A = 0x3C, kStatus1A = 0x3D, kInterruptA = 0x3E, kInterruptB = 0x3F,
    kStatus0 = 0x40, kStatus1 = 0x41, kInterrupt = 0x42, kFifo = 0x43,
};
constexpr std::uint8_t kIaHardRst = 0x01, kIaTxSent = 0x04, kIaHardSent = 0x08, kIaRetryFail = 0x10, kIbGcrcSent = 0x01;

struct ChargerState {
    enum St : int { Unattached, AttachWait, Attached, HardResetOff } st = Unattached;
    enum Pe : int { Startup, SendCaps, WaitRequest, AcceptDue, PsRdyDue, Ready, NoPd } pe = Startup;
    double since = 0, safe_at = -1, rd_lost_at = -1, pe_at = 0, request_at = 0, not_supported_at = -1;
    bool vbus_on = false;
    int caps_tries = 0, msg_id = 0;
};

// The world's state. In the reboot world it lives in shared memory: every firmware life and the run's process see the same
// bytes, so what a power loss keeps (the flash, the backup registers, a FUSB302 that stayed powered) and the world itself (clock,
// charger, VBUS, log) carry over from one life to the next. In the replay it is a static of the one process. Plain data only.
struct State {
    double t0 = 0, us = 0;   // world ms at firmware 0 (this life's power-on), firmware us
    // FUSB302
    std::uint8_t reg[256] = {};
    std::uint8_t rx[kRxFifo] = {};
    std::uint32_t rx_n = 0;
    std::uint8_t ia = 0, ib = 0;                       // INTERRUPTA / INTERRUPTB, clear on read (reboot world)
    std::uint8_t txf[64] = {};                         // TX FIFO token stream since the last TX_START
    std::uint32_t txf_n = 0;
    std::uint8_t tx_msg[32] = {};                      // the message in flight: header + data objects
    std::uint32_t tx_msg_n = 0;
    double tx_done_at = -1;                            // world ms its TXSENT / RETRYFAIL shows
    std::uint8_t tx_done_bits = 0;
    bool tx_delivered = false;
    unsigned tx_starts = 0;
    bool chip_on = true;
    std::uint16_t ina_cfg = 0;
    // reboot world
    bool reboot = false, mcu = false, cable_in = true, ended = false, alive_at_end = false;
    int life = 0, lives = 0;
    double pt = 0, vbus = 0, rail = 0, vh_last = -1, last_boot_ms = -1;
    double vh_t[kHist] = {}, vh_mv[kHist] = {};
    int vh_i = 0;
    std::size_t next_event = 0;
    ChargerState ch;
    ChargerStats stats;
    // What a power loss does to the rest of the K2: the backup domain (BPR) and SRAM. Each life is a fork of the pristine image, so
    // both are carried here: the session record's 16 registers (DR22..DR37), the pin mark (DR21), and the .noinit bytes.
    std::uint16_t bpr_session[k2::session::kWords] = {};
    std::uint16_t pin_mark = 0;
    std::uint8_t noinit[kNoinitMax] = {};
    std::uint32_t noinit_n = 0;      // the executable's .noinit size (0: not located or too large for the copy)
    bool died = false;               // a life ended in a brown-out (the next boot follows a dark, not a power-on)
    double dark_from = 0;            // world ms of that brown-out
    std::uint32_t log_n = 0;
    std::uint8_t flash[kFlashBytes];
    char log[kLogBytes];
};

State g_local;
State* S = &g_local;
const Scenario* g_sc;           // the replay's recording (null in the reboot world)
const RebootScenario* g_rs;     // the reboot world's scenario (null in the replay)
Timing g_t;
std::size_t g_next;             // next scenario frame to put on the wire
std::vector<FrameFate> g_fates;
std::vector<LogLine> g_logs;
bool g_in_spend;
bool g_in_life;                 // this process is a firmware life (the run's process never runs firmware code in the reboot world)
double g_adc_init_us = -1;      // this life's first adc::init()
std::span<std::uint8_t> g_noinit;
struct Queued { int frame; std::size_t left; };   // a replay frame in the RX FIFO: bytes not yet popped
std::deque<Queued> g_rxq;
std::map<std::pair<unsigned, unsigned>, bool> g_pins;

double world_now() { return S->t0 + S->us / 1000.0; }

void log_put(double t, int life, const char* text) {
    const std::size_t n = std::strlen(text), need = sizeof t + sizeof life + sizeof(std::uint16_t) + n;
    if (n > 0xFFFF || S->log_n + need > kLogBytes) return;
    char* p = S->log + S->log_n;
    const std::uint16_t len = std::uint16_t(n);
    std::memcpy(p, &t, sizeof t); p += sizeof t;
    std::memcpy(p, &life, sizeof life); p += sizeof life;
    std::memcpy(p, &len, sizeof len); p += sizeof len;
    std::memcpy(p, text, n);
    S->log_n += std::uint32_t(need);
}

[[gnu::format(printf, 1, 2)]] void wlog(const char* fmt, ...) {   // a world event, in the firmware log's order
    char b[200];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    std::strcat(b, "\n");
    log_put(S->pt, S->life, b);
}

void on_log(const char* line) {
    if (S->reboot) log_put(world_now(), S->life, line);
    else g_logs.push_back({world_now(), line, 0});
}

void int_n() {   // the chip's INT_N edge: only a running MCU sees it
    if (g_in_life && S->mcu) k2::fusb302::on_int_n();
}

// ---- FUSB302 ----
void chip_defaults() {   // power-on / SW_RES values of the registers the model reads (inferred)
    std::fill(std::begin(S->reg), std::end(S->reg), std::uint8_t{0});
    S->reg[kDeviceId] = 0x91;
    S->reg[kSwitches0] = 0x03;   // Rd on both lines
    S->reg[kSwitches1] = 0x20;
    S->reg[kMeasure] = 0x31;
    S->reg[kControl0] = 0x24;
    S->reg[kPower] = 0x01;
    S->ia = S->ib = 0;
}

void flush_rx(const char* why) {
    for (const Queued& q : g_rxq)
        if (!g_fates[q.frame].read) { g_fates[q.frame].delivered = false; g_fates[q.frame].lost = why; }
    S->rx_n = 0;
    g_rxq.clear();
}

std::uint8_t rx_pop() {
    const std::uint8_t b = S->rx[0];
    std::memmove(S->rx, S->rx + 1, --S->rx_n);
    return b;
}

// The CC analog switch between the connector line and the FUSB302: a GPIO the MCU drives high to close it; a dead MCU drives
// nothing, so the switch is open (k2/core/bootmode.hpp: the K2 has no hardware Rd to fall back on).
bool sw_closed(int line) {
    if (!S->reboot) return true;
    if (!g_in_life || !S->mcu) return false;
    const k2::reg::Pin& p = line == 1 ? k2::pins::cc1_switch : k2::pins::cc2_switch;
    const auto it = g_pins.find({unsigned(p.port), p.n});
    return it != g_pins.end() && it->second;
}

// Why a frame on `line` does not reach the RX FIFO now (empty: it does).
void route_block(int line, std::uint8_t tok, std::size_t n, char* why, std::size_t cap) {
    why[0] = 0;
    const std::uint8_t sw1 = S->reg[kSwitches1], c1 = S->reg[kControl1], pw = S->reg[kPower];
    if (S->reboot && !(S->chip_on && sw_closed(line))) std::snprintf(why, cap, "FUSB302 off CC%d (unpowered or switch open)", line);
    else if (!(sw1 & (line == 1 ? 0x01 : 0x02))) std::snprintf(why, cap, "unrouted: SWITCHES1=%02X (TXCC%d off)", sw1, line);
    else if (!(pw & 0x02)) std::snprintf(why, cap, "receiver off: POWER=%02X", pw);
    else if (tok == 0xC0 && !(c1 & 0x01)) std::snprintf(why, cap, "SOP' not enabled: CONTROL1=%02X", c1);
    else if (tok == 0xA0 && !(c1 & 0x02)) std::snprintf(why, cap, "SOP'' not enabled: CONTROL1=%02X", c1);
    else if (S->rx_n + n > kRxFifo) std::snprintf(why, cap, "RX FIFO overflow (%u bytes queued)", unsigned(S->rx_n));
}

template <class Step> const Step* step_at(const std::vector<Step>& v, double t) {
    const Step* cur = nullptr;
    for (const Step& s : v) { if (s.t > t) break; cur = &s; }
    return cur;
}

double hist_mv(double t) {   // reboot world: VBUS at world time t from the recent samples
    for (int k = 0; k < kHist; k++) {
        const int i = (S->vh_i - 1 - k + 2 * kHist) % kHist;
        if (S->vh_t[i] <= t) return S->vh_mv[i];
    }
    return S->vh_mv[S->vh_i % kHist];
}

double vbus_mv_at(double t) {
    if (S->reboot) return hist_mv(t);
    const auto& v = g_sc->vbus;
    if (v.empty()) return 0;
    if (t <= v.front().t) return v.front().mv;
    if (t >= v.back().t) return v.back().mv;
    const auto it = std::upper_bound(v.begin(), v.end(), t, [](double x, const VbusPoint& p) { return x < p.t; });
    const VbusPoint& b = *it;
    const VbusPoint& a = *(it - 1);
    return b.t == a.t ? b.mv : a.mv + (b.mv - a.mv) * (t - a.t) / (b.t - a.t);
}

// ---- reboot world: the charger, the K2's supply, brown-out ----
bool chip_rd(int line) { return S->chip_on && sw_closed(line) && (S->reg[kSwitches0] & (line == 1 ? 0x01 : 0x02)); }

bool rd_on_charger_line() {
    const int l = g_rs->ch.line;
    return g_rs->hid_loop || (l == 2 && plus_held()) || chip_rd(l);
}

int cc_level(int line) {   // what the FUSB302's measure block and the ADC see on a connector line (mV)
    if (!S->cable_in || line != g_rs->ch.line) return 0;
    return rd_on_charger_line() ? kRdMv : kOpenMv;
}

int line_level(int line, double t) {
    if (S->reboot) return sw_closed(line) ? cc_level(line) : 0;
    const RpStep* s = step_at(g_sc->rp, t);
    return s ? (line == 1 ? s->cc1 : s->cc2) : 0;
}

bool k2_receives(const std::vector<std::uint8_t>& frame) {   // a charger frame at the K2's chip; true = Auto-CRC acknowledged it
    char why[96];
    route_block(g_rs->ch.line, frame[0] & 0xE0, frame.size(), why, sizeof why);
    if (why[0]) return false;
    std::memcpy(S->rx + S->rx_n, frame.data(), frame.size());
    S->rx_n += std::uint32_t(frame.size());
    const bool ack = S->reg[kSwitches1] & 0x04;   // AUTO_CRC
    if (ack) S->ib |= kIbGcrcSent;
    int_n();
    return ack;
}

bool charger_send(std::uint8_t type, std::span<const std::uint8_t> data) {
    ChargerState& c = S->ch;
    const unsigned ndo = unsigned(data.size() / 4);
    const std::uint16_t h = std::uint16_t(type | (2u << 6) | (1u << 5) | (1u << 8) | (unsigned(c.msg_id) << 9) | (ndo << 12));
    c.msg_id = (c.msg_id + 1) & 7;
    std::vector<std::uint8_t> f{0xE0, std::uint8_t(h), std::uint8_t(h >> 8)};
    f.insert(f.end(), data.begin(), data.end());
    f.insert(f.end(), 4, 0);   // CRC: the chip checks it, the driver does not read it
    return k2_receives(f);
}
bool charger_ctl(std::uint8_t type) { return charger_send(type, {}); }

void charger_hard_reset(const char* why) {
    ChargerState& c = S->ch;
    const int l = g_rs->ch.line;
    ++S->stats.hard_resets;
    wlog("[WORLD] charger: Hard Reset (%s), VBUS off for %.0f ms", why, g_rs->ch.hr_off_ms);
    if (S->chip_on && sw_closed(l) && (S->reg[kSwitches1] & (l == 1 ? 0x01 : 0x02)) && (S->reg[kPower] & 0x02)) {
        S->ia |= kIaHardRst;
        int_n();
    }
    c.st = ChargerState::HardResetOff;
    c.since = S->pt;
    c.vbus_on = false;
    c.pe = ChargerState::Startup;
    c.msg_id = 0;
    c.safe_at = -1;
    c.not_supported_at = -1;
}

void charger_hears(std::span<const std::uint8_t> m) {   // a K2 message the charger acknowledged (header + data objects)
    if (m.size() < 2) return;
    ChargerState& c = S->ch;
    const unsigned h = m[0] | m[1] << 8, type = h & 0x1F, ndo = (h >> 12) & 7;
    const bool ext = h & 0x8000;
    if (!ext && ndo && type == 2) {   // Request
        const std::uint32_t rdo = m.size() >= 6 ? std::uint32_t(m[2] | m[3] << 8 | m[4] << 16 | std::uint32_t(m[5]) << 24) : 0;
        ++S->stats.requests;
        wlog("[WORLD] charger: Request RDO 0x%08X (object %u)", unsigned(rdo), unsigned(rdo >> 28) & 0xF);
        c.pe = ChargerState::AcceptDue;
        c.request_at = S->pt;
        c.pe_at = S->pt + g_rs->ch.accept_ms;
        return;
    }
    if (!ext && !ndo && (type == 7 || type == 13)) {   // Get_Source_Cap, Soft_Reset (its Accept, then the capabilities)
        if (type == 13) { charger_ctl(3); c.msg_id = 0; }
        c.pe = ChargerState::SendCaps;
        c.pe_at = S->pt + 2;
        c.caps_tries = 0;
        return;
    }
    if (!ext && !ndo && (type == 1 || type == 3 || type == 4 || type == 16)) return;
    c.not_supported_at = S->pt + 2;   // anything else: Not_Supported (the e-marker look, VDMs, extended asks)
}

void charger_pe(double now) {
    ChargerState& c = S->ch;
    const ChargerModel& m = g_rs->ch;
    if (c.not_supported_at >= 0 && now >= c.not_supported_at) { c.not_supported_at = -1; charger_ctl(16); }
    switch (c.pe) {
    case ChargerState::SendCaps:
        if (now < c.pe_at) break;
        ++S->stats.caps_sent;
        if (charger_send(1, m.caps)) {
            ++S->stats.caps_acked;
            c.pe = ChargerState::WaitRequest;
            c.pe_at = now + m.sender_response_ms;
        } else if (++c.caps_tries >= m.caps_count) {
            c.pe = ChargerState::NoPd;
            wlog("[WORLD] charger: %d capabilities without GoodCRC: no PD, VBUS stays 5 V", c.caps_tries);
        } else c.pe_at = now + m.caps_period_ms;
        break;
    case ChargerState::WaitRequest:
        if (now >= c.pe_at) charger_hard_reset("no Request within tSenderResponse");
        break;
    case ChargerState::AcceptDue:
        if (now < c.pe_at) break;
        if (!charger_ctl(3)) { charger_hard_reset("Accept not acknowledged"); break; }
        c.pe = ChargerState::PsRdyDue;
        c.pe_at = c.request_at + m.ps_rdy_ms;
        break;
    case ChargerState::PsRdyDue:
        if (now < c.pe_at) break;
        if (!charger_ctl(6)) { charger_hard_reset("PS_RDY not acknowledged"); break; }
        c.pe = ChargerState::Ready;
        ++S->stats.contracts;
        wlog("[WORLD] charger: contract, PS_RDY acknowledged");
        break;
    default: break;
    }
}

void charger_step(double now) {
    ChargerState& c = S->ch;
    const ChargerModel& m = g_rs->ch;
    ChargerStats& st = S->stats;
    if (!S->cable_in) return;
    const bool rd = rd_on_charger_line();
    switch (c.st) {
    case ChargerState::Unattached:
        if (rd) { c.st = ChargerState::AttachWait; c.since = now; }
        break;
    case ChargerState::AttachWait:
        if (!rd) c.st = ChargerState::Unattached;
        else if (now - c.since >= m.cc_debounce_ms) {
            c = ChargerState{};
            c.st = ChargerState::Attached;
            c.since = now;
            c.vbus_on = true;
            ++st.attaches;
            if (st.first_attach_ms < 0) st.first_attach_ms = now;
            wlog("[WORLD] charger: Rd for %.0f ms on CC%d: attached, VBUS on", m.cc_debounce_ms, m.line);
        }
        break;
    case ChargerState::HardResetOff:   // tSrcRecover: VBUS comes back without a CC check (inferred)
        if (now - c.since >= m.hr_off_ms) {
            c.st = ChargerState::Attached;
            c.since = now;
            c.vbus_on = true;
            c.rd_lost_at = -1;
            wlog("[WORLD] charger: Hard Reset over, VBUS on again");
        }
        break;
    case ChargerState::Attached:
        if (c.safe_at < 0 && S->vbus >= kSafe5vMv) {
            c.safe_at = now;
            c.pe = ChargerState::SendCaps;
            c.pe_at = now + m.first_caps_ms;
            c.caps_tries = 0;
        }
        if (c.safe_at < 0) break;
        if (rd) c.rd_lost_at = -1;
        else {
            if (c.rd_lost_at < 0) c.rd_lost_at = now;
            const double gap = now - c.rd_lost_at;
            if (gap > st.rd_gap_max_ms) { st.rd_gap_max_ms = gap; st.rd_gap_max_at = c.rd_lost_at; }
            if (gap >= m.pd_debounce_ms) {
                c = ChargerState{};
                ++st.detaches;
                wlog("[WORLD] charger: no Rd for %.1f ms: detached, VBUS off", gap);
                break;
            }
        }
        charger_pe(now);
        break;
    }
}

void apply_event(const RebootEvent& e) {
    switch (e.kind) {
    case RebootEvent::HardReset:
        if (S->ch.st == ChargerState::Attached) charger_hard_reset("the scenario's");
        else wlog("[WORLD] scenario Hard Reset skipped: the charger is not attached");
        break;
    case RebootEvent::Unplug:
        S->cable_in = false;
        S->ch = ChargerState{};
        wlog("[WORLD] cable unplugged");
        break;
    case RebootEvent::Replug:
        S->cable_in = true;
        wlog("[WORLD] cable plugged in");
        break;
    }
}

[[noreturn]] void die() {
    if (S->noinit_n) std::memcpy(S->noinit, g_noinit.data(), S->noinit_n);
    S->died = true;
    S->dark_from = S->pt;
    wlog("[WORLD] brown-out: rail %.0f mV, life %d ends", S->rail, S->life);
    S->us = (S->pt - S->t0) * 1000.0;
    S->mcu = false;
    S->life = 0;
    std::_Exit(kLifeDied);
}

void phys_step(double dt) {
    S->pt += dt;
    const double now = S->pt;
    const PowerModel& pm = g_rs->pm;
    if (S->tx_done_at >= 0 && now >= S->tx_done_at) {   // the K2's transmission: the charger's GoodCRC, or the chip's retries ran out
        S->tx_done_at = -1;
        S->ia |= S->tx_done_bits;
        int_n();
        if (S->tx_delivered) charger_hears({S->tx_msg, S->tx_msg_n});
    }
    while (S->next_event < g_rs->events.size() && g_rs->events[S->next_event].t <= now) apply_event(g_rs->events[S->next_event++]);
    charger_step(now);
    const double target = S->cable_in && S->ch.vbus_on ? 5000.0 : 0.0;
    if (S->vbus < target) S->vbus = std::min(target, S->vbus + g_rs->ch.rise_mv_per_ms * dt);
    else if (S->vbus > target) S->vbus = std::max(target, S->vbus - g_rs->ch.fall_mv_per_ms * dt);
    if (now - S->vh_last >= kStepMs - 1e-9) {
        S->vh_last = now;
        S->vh_t[S->vh_i] = now;
        S->vh_mv[S->vh_i] = S->vbus;
        S->vh_i = (S->vh_i + 1) % kHist;
    }
    const double aux = g_rs->hid_loop ? S->vbus : 0.0;   // the HID port's VBUS (through the loop cable)
    S->rail = std::clamp(std::max(S->vbus, aux) - pm.dropout_mv, 0.0, 3300.0);
    if (S->chip_on && S->rail < pm.fusb_por_mv) {
        S->chip_on = false;
        S->rx_n = 0;
        S->txf_n = 0;
        S->tx_done_at = -1;
        wlog("[WORLD] FUSB302 below its supply minimum: registers lost");
    } else if (!S->chip_on && S->rail >= pm.fusb_por_mv) {
        S->chip_on = true;
        chip_defaults();
    }
    if (S->mcu && S->rail < pm.mcu_bor_mv) die();
}

void phys_to(double t) {
    if (t - S->pt < 0.005) return;
    while (t - S->pt > 1e-9) phys_step(std::min(kStepMs, t - S->pt));
}

void put_on_wire(int i) {
    const Frame& f = g_sc->frames[i];
    FrameFate& fate = g_fates[i];
    const std::uint8_t tok = f.raw.empty() ? 0 : f.raw[0] & 0xE0;
    char why[96];
    route_block(f.line, tok, f.raw.size(), why, sizeof why);
    if (why[0]) { fate.lost = why; return; }
    std::memcpy(S->rx + S->rx_n, f.raw.data(), f.raw.size());
    S->rx_n += std::uint32_t(f.raw.size());
    g_rxq.push_back({i, f.raw.size()});
    fate.delivered = true;
    k2::fusb302::on_int_n();
}

// Everything the world does up to firmware time `until_us`, against the register state as it is now.
void world_until(double until_us) {
    const double until = S->t0 + until_us / 1000.0;
    if (S->reboot) { phys_to(until); return; }
    while (g_next < g_sc->frames.size() && g_sc->frames[g_next].t <= until) put_on_wire(int(g_next++));
}

std::uint8_t status0() {
    const std::uint8_t sw0 = S->reg[kSwitches0];
    const int meas = (sw0 & 0x0C) == 0x04 ? 1 : (sw0 & 0x0C) == 0x08 ? 2 : 0;
    const int mv = meas ? line_level(meas, world_now()) : 0;
    const std::uint8_t bc = mv < 200 ? 0 : mv < 660 ? 1 : mv < 1230 ? 2 : 3;
    const bool comp = mv > (S->reg[kMeasure] & 0x3F) * 42;
    return std::uint8_t(bc | (comp ? 0x20 : 0));   // VBUSOK reads 0 on this board (fusb302.cpp), ACTIVITY / CRC_CHK not modelled
}

// TX_START: the token stream since the last start is one message (SOP tokens, PACKSYM | n and n bytes, JAM_CRC, EOP, TXOFF).
// The charger acknowledges an SOP message the K2's chip can put on its line; anything else ends in RETRYFAIL (no cable
// e-marker in this world).
void tx_start() {
    const std::uint8_t* t = S->txf;
    const bool sop = S->txf_n >= 4 && t[0] == 0x12 && t[1] == 0x12 && t[2] == 0x12 && t[3] == 0x13;
    S->tx_msg_n = 0;
    for (std::uint32_t i = 0; i < S->txf_n;) {
        const std::uint8_t b = t[i++];
        if ((b & 0xE0) != 0x80) continue;
        for (unsigned k = 0; k < (b & 0x1Fu) && i < S->txf_n && S->tx_msg_n < sizeof S->tx_msg; k++) S->tx_msg[S->tx_msg_n++] = t[i++];
    }
    S->txf_n = 0;
    const int l = g_rs->ch.line;
    const bool ok = sop && S->chip_on && sw_closed(l) && (S->reg[kSwitches1] & (l == 1 ? 0x01 : 0x02)) && S->cable_in &&
                    S->ch.st == ChargerState::Attached && S->ch.safe_at >= 0;
    S->tx_delivered = ok;
    S->tx_done_bits = ok ? kIaTxSent : kIaRetryFail;
    S->tx_done_at = world_now() + (ok ? 1.2 : 4.0);   // the GoodCRC ~1 ms after TX_START; 3 retries of ~1 ms each (inferred)
}

std::uint8_t fusb_read(std::uint8_t reg) {
    switch (reg) {
    case kStatus0: return status0();
    case kStatus1: return std::uint8_t((S->rx_n == 0 ? 0x20 : 0x00) | 0x08);   // RX_EMPTY, TX_EMPTY
    case kInterruptA: case kInterruptB: {
        if (!S->reboot) return 0;
        std::uint8_t& r = reg == kInterruptA ? S->ia : S->ib;
        const std::uint8_t v = r;
        r = 0;
        return v;
    }
    case kInterrupt: case kStatus0A: case kStatus1A: return 0;
    case kFifo: {
        if (S->rx_n == 0) return 0;
        const std::uint8_t b = rx_pop();
        if (!S->reboot && !g_rxq.empty()) {
            Queued& q = g_rxq.front();
            FrameFate& f = g_fates[q.frame];
            if (!f.read) { f.read = true; f.fw_read_ms = S->us / 1000.0; }
            if (--q.left == 0) g_rxq.pop_front();
        }
        return b;
    }
    default: return S->reg[reg];
    }
}

void fusb_write(std::uint8_t reg, std::uint8_t v) {
    switch (reg) {
    case kReset:
        if (v & 0x01) { chip_defaults(); flush_rx("flushed: SW_RES"); }
        else if (v & 0x02) flush_rx("flushed: PD_RESET");
        return;
    case kControl0:
        if (v & 0x40) S->txf_n = 0;   // TX_FLUSH
        if (v & 0x01) { ++S->tx_starts; if (S->reboot) tx_start(); }
        S->reg[reg] = std::uint8_t(v & ~0x41);   // TX_START / TX_FLUSH self-clear
        return;
    case kControl1:
        if (v & 0x04) flush_rx("flushed: RX_FLUSH");
        S->reg[reg] = std::uint8_t(v & ~0x04);
        return;
    case kControl3:
        if (S->reboot && (v & 0x40)) {   // SEND_HARD_RESET
            S->ia |= kIaHardSent;
            if (S->ch.st == ChargerState::Attached) charger_hard_reset("sent by the K2");
        }
        S->reg[reg] = std::uint8_t(v & ~0x40);
        return;
    default: S->reg[reg] = v;
    }
}

void fifo_write(std::span<const std::uint8_t> d) {   // TX FIFO bytes (the replay counts them at TX_START only)
    if (!S->reboot) return;
    for (std::uint8_t b : d)
        if (S->txf_n < sizeof S->txf) S->txf[S->txf_n++] = b;
}

// ---- INA226 ----
double g_mv_per_count;
constexpr std::uint16_t kInaDefault = 0x4127;   // power-on configuration
// The bus register holds the average of the last complete conversion period (AVG x (VBUSCT + VSHCT) in continuous mode),
// so a VBUS edge shows up to one period late and smoothed: 4.48 ms at the driver's 0x4407, 280 us in its fast mode.
double ina_bus_mv() {
    static constexpr double kCt[] = {140, 204, 332, 588, 1100, 2116, 4156, 8244};
    static constexpr int kAvg[] = {1, 4, 16, 64, 128, 256, 512, 1024};
    const std::uint16_t cfg = S->ina_cfg;
    const double period_ms = kAvg[(cfg >> 9) & 7] * (kCt[(cfg >> 6) & 7] + kCt[(cfg >> 3) & 7]) / 1000.0;
    const double end = std::floor(world_now() / period_ms) * period_ms;
    double sum = 0;
    for (int i = 0; i < 8; i++) sum += vbus_mv_at(end - period_ms * (i + 0.5) / 8);
    return sum / 8;
}
std::uint16_t ina_read(std::uint8_t reg) {
    switch (reg) {
    case 0xFF: return 0x2260;   // die id: INA226
    case 0x00: return S->ina_cfg;
    case 0x02: return std::uint16_t(std::lround(std::max(0.0, ina_bus_mv()) / g_mv_per_count));
    default: return 0;          // shunt 0 (no load), and no INA228 / INA237 ids
    }
}
void ina_write(std::uint8_t reg, std::span<const std::uint8_t> d) {
    if (reg != 0x00 || d.size() < 2) return;
    const std::uint16_t v = std::uint16_t(d[0] << 8 | d[1]);
    S->ina_cfg = (v & 0x8000) ? kInaDefault : v;
}

// ---- the flash volume ----
class WorldFlash final : public k2::fs::BlockDevice {
public:
    int read(std::uint32_t b, std::uint32_t off, void* buf, std::uint32_t n) override {
        if (b >= k2::fs::kFlashBlocks || off + n > k2::fs::kBlockSize) return k2::fs::kIoFail;
        std::memcpy(buf, &S->flash[std::size_t(b) * k2::fs::kBlockSize + off], n);
        return 0;
    }
    int prog(std::uint32_t b, std::uint32_t off, const void* buf, std::uint32_t n) override {
        if (b >= k2::fs::kFlashBlocks || off + n > k2::fs::kBlockSize) return k2::fs::kIoFail;
        std::uint8_t* d = &S->flash[std::size_t(b) * k2::fs::kBlockSize + off];
        for (std::uint32_t i = 0; i < n; i++) d[i] &= static_cast<const std::uint8_t*>(buf)[i];
        return 0;
    }
    int erase(std::uint32_t b) override {
        if (b >= k2::fs::kFlashBlocks) return k2::fs::kIoFail;
        std::memset(&S->flash[std::size_t(b) * k2::fs::kBlockSize], 0xFF, k2::fs::kBlockSize);
        return 0;
    }
};
WorldFlash g_flash;
k2::fs::Fs g_fs(g_flash, k2::fs::kFlashBlocks);

// The .noinit section of this executable (rd_comeback.cpp's SRAM probe, a diagnostic): its address at run time from the section
// headers and the load bias of the main program.
std::span<std::uint8_t> find_noinit() {
    std::uintptr_t base = 0;
    dl_iterate_phdr([](dl_phdr_info* i, std::size_t, void* d) { *static_cast<std::uintptr_t*>(d) = i->dlpi_addr; return 1; }, &base);
    std::FILE* f = std::fopen("/proc/self/exe", "rb");
    if (!f) return {};
    std::span<std::uint8_t> out;
    Elf64_Ehdr eh;
    if (std::fread(&eh, sizeof eh, 1, f) == 1 && eh.e_shnum && eh.e_shstrndx < eh.e_shnum) {
        std::vector<Elf64_Shdr> sh(eh.e_shnum);
        std::fseek(f, long(eh.e_shoff), SEEK_SET);
        if (std::fread(sh.data(), sizeof(Elf64_Shdr), sh.size(), f) == sh.size()) {
            std::vector<char> names(sh[eh.e_shstrndx].sh_size + 1, 0);
            std::fseek(f, long(sh[eh.e_shstrndx].sh_offset), SEEK_SET);
            if (std::fread(names.data(), 1, names.size() - 1, f) == names.size() - 1)
                for (const Elf64_Shdr& s : sh)
                    if (s.sh_name < names.size() && std::strcmp(&names[s.sh_name], ".noinit") == 0)
                        out = {reinterpret_cast<std::uint8_t*>(base + s.sh_addr), std::size_t(s.sh_size)};
        }
    }
    std::fclose(f);
    return out;
}

LcdTiming g_lcd;
void lcd_spend(double us) { spend_us(us); }
double lcd_now() { return S->us; }

void common_start(const Timing& t) {
    g_t = t;
    std::memset(S->flash, 0xFF, sizeof S->flash);
    S->ina_cfg = kInaDefault;
    g_logs.clear();
    g_rxq.clear();
    g_mv_per_count = k2::power::raw_to_mv(k2::power::Chip::Ina226, k2::power::align_vbus(k2::power::Chip::Ina226, 10000)) / 10000.0;
    g_pins.clear();
    for (const auto& p : {k2::pins::key_plus, k2::pins::key_minus, k2::pins::key_ok, k2::pins::key_back})
        g_pins[{unsigned(p.port), p.n}] = true;   // released (active low)
    g_stub.on_log = on_log;
    g_lcd = {lcd_spend, lcd_now, t.px_us, t.lcd_call_us, t.lcd_window_us, t.px_cpu_us, 468, 2048};
    g_lcd_timing = &g_lcd;
}

}  // namespace

void spend_us(double us) {
    if (us <= 0) return;
    const double target = S->us + us;
    if (!g_in_spend) {
        g_in_spend = true;
        world_until(target);
        g_in_spend = false;
    }
    S->us = target;
}

double world_ms() { return world_now(); }
double fw_ms() { return S->us / 1000.0; }
void wfi() { spend_us(1000.0 - std::fmod(S->us, 1000.0)); }
const std::vector<FrameFate>& fates() { return g_fates; }
const std::vector<LogLine>& logs() {
    if (S->reboot) {   // the shared log of every life
        g_logs.clear();
        for (std::uint32_t at = 0; at < S->log_n;) {
            double t;
            int life;
            std::uint16_t len;
            std::memcpy(&t, S->log + at, sizeof t); at += sizeof t;
            std::memcpy(&life, S->log + at, sizeof life); at += sizeof life;
            std::memcpy(&len, S->log + at, sizeof len); at += sizeof len;
            g_logs.push_back({t, std::string(S->log + at, len), life});
            at += len;
        }
    }
    return g_logs;
}
unsigned tx_starts() { return S->tx_starts; }

void start(const Scenario& sc, double world_t0_ms, const Timing& t) {
    S = new (&g_local) State();
    g_sc = &sc;
    g_rs = nullptr;
    S->t0 = world_t0_ms;
    g_fates.assign(sc.frames.size(), {});
    for (std::size_t i = 0; i < sc.frames.size(); i++) g_fates[i].index = int(i);
    g_next = 0;
    while (g_next < sc.frames.size() && sc.frames[g_next].t < world_t0_ms) g_fates[g_next++].lost = "before the run";
    chip_defaults();
    common_start(t);
}

void reboot_start(const RebootScenario& sc, const Timing& t) {
    void* mem = mmap(nullptr, sizeof(State), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { std::perror("mmap"); std::_Exit(2); }
    S = new (mem) State();
    g_sc = nullptr;
    g_rs = &sc;
    g_fates.clear();
    S->reboot = true;
    S->chip_on = false;   // everything unpowered at world time 0
    S->cable_in = true;
    g_noinit = find_noinit();
    S->noinit_n = g_noinit.size() <= sizeof S->noinit ? std::uint32_t(g_noinit.size()) : 0;
    common_start(t);
    g_stub.usb_configured = false;   // the HID port (looped or not) has no USB host on it
}

bool noinit_found() { return S->noinit_n != 0; }

bool reboot_wait_boot() {
    const PowerModel& pm = g_rs->pm;
    for (;;) {
        if (S->ended || world_now() >= g_rs->end_ms || S->lives >= kMaxLives) { S->ended = true; return false; }
        if (S->rail >= pm.mcu_bor_mv) {
            S->us += pm.boot_ms * 1000.0;   // power-on reset released: the clock start-up to the first instruction
            phys_to(world_now());
            if (S->rail >= pm.mcu_bor_mv) return true;
            continue;
        }
        S->us += kStepMs * 1000.0;
        phys_to(world_now());
    }
}

void life_begin() {
    g_in_life = true;
    const double w = world_now();
    S->t0 = w;
    S->us = 0;
    S->life = ++S->lives;
    S->mcu = true;
    S->last_boot_ms = w;
    // What the dark did. The first life is a power-on: the backup registers read as zeros (an unpowered domain), SRAM as the image.
    // After a brown-out: the backup registers survive a dark up to bpr_hold_ms, else they are zeros; SRAM is lost (power-on
    // garbage, a fixed pattern here) unless the scenario's switch says it held.
    const PowerModel& pm = g_rs->pm;
    const double dark = S->died ? w - S->dark_from : 0;
    const bool bpr_kept = S->died && dark <= pm.bpr_hold_ms;
    const bool sram_kept = S->died && pm.sram_kept;
    if (S->died && !bpr_kept) {
        std::memset(S->bpr_session, 0, sizeof S->bpr_session);
        S->pin_mark = 0;
    }
    if (S->died && S->noinit_n) {
        if (sram_kept) std::memcpy(g_noinit.data(), S->noinit, S->noinit_n);
        else {
            std::uint32_t x = 0x9E3779B9u;
            for (std::uint32_t i = 0; i < S->noinit_n; i++) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;   // xorshift32: unlike the probe's pattern, so the count is not a coincidence
                g_noinit[i] = std::uint8_t(x);
            }
        }
    }
    wlog("[WORLD] life %d: power-on reset at rail %.0f mV, VBUS %.0f mV, dark %.0f ms, backup registers %s, .noinit %s", S->life, S->rail,
         S->vbus, dark, S->died ? (bpr_kept ? "kept" : "lost") : "power-on", S->died ? (sram_kept ? "kept" : "lost") : "power-on");
}

void note(const char* text) {
    char b[256];
    std::snprintf(b, sizeof b, "%s\n", text);
    log_put(S->pt, S->life, b);
}

void life_end() {
    S->alive_at_end = true;
    S->ended = true;
    S->mcu = false;
    wlog("[WORLD] end of the scenario: life %d alive", S->life);
}

bool plus_held() { return S->reboot && world_now() < g_rs->plus_until_ms; }

RebootResult reboot_result() {
    RebootResult r;
    r.lives = S->lives;
    r.alive_at_end = S->alive_at_end;
    r.last_boot_ms = S->last_boot_ms;
    r.ch = S->stats;
    r.ch.attached_at_end = S->cable_in && S->ch.st == ChargerState::Attached;
    return r;
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
std::uint32_t millis() { world::spend_us(world::g_t.clock_read_us); return std::uint32_t(world::S->us / 1000.0); }
std::uint32_t micros() { world::spend_us(world::g_t.clock_read_us); return std::uint32_t(world::S->us); }
std::uint32_t ticks() { world::spend_us(world::g_t.clock_read_us); return std::uint32_t(std::fmod(world::S->us * 240.0, 4294967296.0)); }
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
        if (reg == kFifo) { fifo_write(d); return true; }
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
    if (op == HostPin::Read && p == pins::key_plus.port && n == pins::key_plus.n && world::S->reboot) return !world::plus_held();
    bool& v = world::g_pins[{unsigned(p), n}];
    if (op == HostPin::Set) v = true;
    else if (op == HostPin::Clear) v = false;
    return v;
}
}  // namespace reg

namespace pinmark {
// DR21 in the world's backup domain (State::pin_mark): kept over a dark shorter than PowerModel::bpr_hold_ms, like the session record.
std::uint16_t read() { return world::S->pin_mark; }
void write(std::uint16_t raw) { world::S->pin_mark = raw; }
}  // namespace pinmark

namespace fs {   // the volume on the world's flash bytes (src/drivers/fs_flash.cpp without the log lines and the legacy wipe)
Fs& instance() { return world::g_fs; }
Err init(bool& formatted) { return world::g_fs.mount_or_format(formatted); }
}  // namespace fs

namespace adc {
namespace {
const world::AdcStep* now_step() { return world::step_at(world::g_sc->adc, world::world_now()); }
std::uint16_t raw12(int mv) { return std::uint16_t(std::clamp((mv * 4096 + 1650) / 3300, 0, 4095)); }
// The reboot world: no reading before the first full ring of this life.
bool ring_full() { return world::g_adc_init_us >= 0 && world::S->us - world::g_adc_init_us >= world::kAdcFirstRingUs; }
}  // namespace
bool init() {
    if (world::g_adc_init_us < 0) world::g_adc_init_us = world::S->us;
    return true;
}
void poll() {}
std::optional<Reading> read() {
    Reading r;
    r.vdd_mv = 3300;
    if (world::S->reboot) {
        if (!ring_full()) return std::nullopt;
        r.cc1_mv = world::cc_level(1);
        r.cc2_mv = world::cc_level(2);
        r.aux_mv = world::g_rs->hid_loop ? std::int32_t(world::S->vbus) : 0;
        return r;
    }
    const world::AdcStep* s = now_step();
    if (!s) return std::nullopt;
    r.cc1_mv = s->cc1; r.cc2_mv = s->cc2; r.dp_mv = s->dp; r.dm_mv = s->dm;
    return r;
}
bool latest_cc(std::uint16_t& cc1, std::uint16_t& cc2) {
    if (world::S->reboot) {
        if (!ring_full()) return false;
        cc1 = raw12(world::cc_level(1));
        cc2 = raw12(world::cc_level(2));
        return true;
    }
    const world::AdcStep* s = now_step();
    if (!s) return false;
    cc1 = raw12(s->cc1);
    cc2 = raw12(s->cc2);
    return true;
}
bool latest_dpdm(std::int32_t& dp, std::int32_t& dm) {
    if (world::S->reboot) {
        if (!ring_full()) return false;
        dp = dm = 0;
        return true;
    }
    const world::AdcStep* s = now_step();
    if (!s) return false;
    dp = s->dp;
    dm = s->dm;
    return true;
}
}  // namespace adc

// ---- the parts of the main loop the world leaves out ----
// USB, the drive, the shell and the network have no host on the other end; the stores other than the Rd intent marker (session
// record, black box, boot trace) have no flash here; the D+/D- side (fastcharge, dpsniff, fctrig) is not modelled yet
// (docs/TESTING.md "World replay", sink and QC); the transition capture's poll is the delay_ms() wait hook and records nothing.
namespace usb { void log_stats() {} }
namespace shell_hw { void poll() {} }
// The session record's backup registers (src/chargertest/session_store.cpp reaches BPR at 0x40006C00, which a host cannot): the
// same init / voucher over State::bpr_session. Every boot writes a fresh record, so the record is the voucher of the next one. poll()
// would only rewrite it as the PD state changes: the voucher needs no more than that valid record.
namespace session {
namespace {
std::array<std::uint16_t, kWords> bpr_load() {
    std::array<std::uint16_t, kWords> w;
    std::copy(std::begin(world::S->bpr_session), std::end(world::S->bpr_session), w.begin());
    return w;
}
}  // namespace
bool backup_alive() { return decode(bpr_load()).has_value(); }
void init() {
    const auto prev = decode(bpr_load());
    if (prev) K2LOG("[SESS] previous session #%u\n", static_cast<unsigned>(prev->boots));
    else K2LOG("[SESS] no previous session record\n");
    Record fresh;
    fresh.boots = std::uint16_t((prev ? prev->boots : 0) + 1);
    const auto w = encode(fresh);
    std::copy(w.begin(), w.end(), world::S->bpr_session);
}
void poll() {}
}  // namespace session
namespace power_source { void poll(std::uint32_t) {} }
namespace net { void log_stats() {} }
namespace msc { void poll() {} bool io() { return false; } }
namespace http_server { void service() {} void log_stats() {} }
namespace handback { void poll(std::uint32_t) {} }
namespace fctrig { void service_ufcs() {} void service_hs() {} }
namespace fault { void trigger(std::uint8_t) {} }
namespace fastcharge { unsigned init() { return 0; } unsigned log_idle() { return 0; } void poll() {} }
namespace dpsniff { void poll(std::uint32_t, std::int32_t) {} }
namespace boottrace { void flush_log() {} void poll() {} }   // poll: the self-power bring-up's (src/core/boot_pd.cpp)
namespace bootscreen { void draw_live(std::uint32_t) {} }
namespace bootinfo { bool boot_key_held() { return false; } }
namespace bbpersist { void poll() {} }
namespace transcap { void poll() {} }

}  // namespace k2

extern "C" std::uint32_t k2_stack_free_bytes() { return 4096; }
extern "C" std::uint32_t k2_msp_free_bytes() { return 1024; }
