// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The e-marker scan as a wire sequence: the real src/drivers/fusb302.cpp runs against a fake FUSB302 (register
// file, TX/RX FIFOs, an ordered event log), a fake GPIO log (K2_HOST_PINS seam in k2/drivers/regs.hpp) and a
// fake cable that only answers when the physical conditions hold. The expected Vconn sequence is the
// one of the C implementation at b79ce3d (src/fusb302.c emarker_read / scan_orientation), with one
// deliberate difference: the second attempt asks again as PD 2.0.
//
//   - Vconn mode (nothing attached): PC14/PC15/PC13 high, POWER 0x0F, SWITCHES0/1 per orientation,
//     40 ms of VCONN, then two Discover Identity attempts; afterwards PC13 low, Rd back on.
//   - Sniff mode (a source is attached): CC switches, VCONN and Rd are never touched.
#include "test_util.h"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/drivers/i2c.hpp"
#include "k2/drivers/adc.hpp"
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

using namespace k2;

namespace {

std::vector<std::string> g_log;        // ordered: GPIO and chip register events
std::uint32_t g_ms = 1000;
bool g_pin[3];                          // PC13, PC14, PC15
std::uint8_t g_reg[256];
int g_line_mv = 1600;   // level of the measured line when STATUS0.COMP is armed (a source Rp over a sink Rd)
int g_line_mv_cc[2] = {-1, -1};   // per-line override (index 0 = CC1, 1 = CC2, selected by SWITCHES0 MEAS); -1 = use g_line_mv
// A source that shows its Rp only to a sink (g_rd_model on): the level of each line depends on whether the K2's own Rd (SWITCHES0
// PDWN1 = CC1, PDWN2 = CC2) is on it. Overrides both of the above while on.
bool g_rd_model;
int g_lvl_rd[2] = {0, 0};     // line level with the K2's Rd on that line
int g_lvl_nord[2] = {0, 0};   // line level with no Rd on it (a floating or coupled net)
std::vector<std::uint8_t> g_txfifo;
std::deque<std::uint8_t> g_rx;
std::deque<std::uint8_t> g_ia_script;   // INTERRUPTA (0x3E) values handed out one per read (clear-on-read), then g_reg[0x3E]
bool g_auto_txsent;                    // every TX_START reports TXSENT once (a source that GoodCRCs everything)
std::uint32_t g_vconn_since;           // ms when VCONN started flowing (0 = not)
std::vector<std::string> g_usb;
std::uint16_t g_mark;                  // the pin mark (backup register DR21): survives the "warm resets" the tests simulate

// The cable under test.
struct Cable {
    int cc = 1;                         // wire the e-marker sits on (1 or 2); VCONN must go on the other
    bool answers_pd3 = true;
    bool answers_pd2 = true;
    int tx_seen = 0;
    int answered = 0;
    bool vendor = false;                // also GoodCRCs other SOP' VDMs; the vendor command 22D9:31 gets an ACK with two VDOs
    std::vector<unsigned> vendor_ids;   // MessageID of every such request
    int vendor_vconn_ms = -1;           // VCONN age at the last such request
    bool vendor_drop = false;           // ... but then no GoodCRC at all: RETRYFAIL
} g_cable;

std::string hex(unsigned v) { char b[16]; std::snprintf(b, sizeof b, "%02X", v); return b; }

void log(const std::string& s) { g_log.push_back(s); }

void update_vconn() {
    const bool on = g_pin[0] && g_pin[1] && g_pin[2] && (g_reg[0x02] & 0x30) != 0;
    if (on && !g_vconn_since) g_vconn_since = g_ms;
    if (!on) g_vconn_since = 0;
}

void push_frame(std::uint8_t token, std::uint16_t header, const std::vector<std::uint32_t>& dos) {
    g_rx.push_back(token);
    g_rx.push_back(std::uint8_t(header));
    g_rx.push_back(std::uint8_t(header >> 8));
    for (std::uint32_t d : dos)
        for (int i = 0; i < 4; i++) g_rx.push_back(std::uint8_t(d >> (8 * i)));
    for (int i = 0; i < 4; i++) g_rx.push_back(0xC5);   // CRC
}

// What the cable does when TX_START is written.
void tx_start() {
    ++g_cable.tx_seen;
    const bool is_di = g_txfifo.size() == 14 && g_txfifo[10] == 0xFF && g_txfifo[9] == 0x00 && (g_txfifo[8] & 0x1F) == 0 &&
                       g_txfifo[7] == 0x01;   // the VDM header 0xFF00xx01: Discover Identity
    const bool pd3 = is_di && g_txfifo[5] == 0x8F;
    const bool pd2 = is_di && g_txfifo[5] == 0x4F;
    const unsigned vconn_line = g_cable.cc == 1 ? 0x20 : 0x10;       // VCONN on the other CC
    const unsigned tx_line = g_cable.cc == 1 ? 0x01 : 0x02;          // BMC to the cable's wire
    const bool powered = g_vconn_since && g_ms - g_vconn_since >= 25;
    const bool routed = (g_reg[0x02] & 0x30) == vconn_line && (g_reg[0x03] & 0x03) == tx_line;
    if (!powered || !routed) return;
    if (!pd3 && !pd2 && g_cable.vendor && g_txfifo.size() >= 11) {   // a later request of a cable session
        const std::uint32_t vdm = g_txfifo[7] | (g_txfifo[8] << 8) | (g_txfifo[9] << 16) | (std::uint32_t(g_txfifo[10]) << 24);
        g_cable.vendor_ids.push_back((g_txfifo[6] >> 1) & 7u);
        g_cable.vendor_vconn_ms = int(g_ms - g_vconn_since);
        if (g_cable.vendor_drop) {                                    // nothing comes back: the chip's retries run out
            g_ia_script.push_back(0x10);                              // INTERRUPTA RETRYFAIL
            return;
        }
        ++g_cable.answered;
        g_ia_script.push_back(0x04);                                  // INTERRUPTA TXSENT: the chip saw the GoodCRC
        push_frame(0xC0, 0x0101, {});                                 // GoodCRC
        if ((vdm >> 16) == 0x22D9 && (vdm & 0x1F) == 31)
            push_frame(0xC0, std::uint16_t(0x0F | 0x100 | (2u << 6) | (3u << 12)),
                       {(vdm & ~(3u << 6)) | (1u << 6), 0xAB01CD02u, 7u});
        return;
    }
    if ((pd3 && !g_cable.answers_pd3) || (pd2 && !g_cable.answers_pd2) || !(pd3 || pd2)) return;
    ++g_cable.answered;
    push_frame(0xC0, 0x0101, {});                                     // GoodCRC from the cable
    const std::uint16_t rev = pd3 ? (2u << 6) : (1u << 6);
    push_frame(0xC0, std::uint16_t(0x0F | 0x100 | rev | (5u << 12)),        // Discover Identity ACK
               {0xFF00A041u, (3u << 27) | 0x05ACu, 0, 0x12340005u, (3u << 9) | (2u << 5) | 2u});
}

}  // namespace

namespace k2 {

namespace reg {
bool host_pin(Port p, std::uint8_t n, HostPin op, unsigned) {
    if (p != Port::C || n < 13 || n > 15) {
        log(std::string("PIN other ") + std::to_string(unsigned(p)) + "." + std::to_string(n));
        return false;
    }
    const char* name = n == 13 ? "PC13" : n == 14 ? "PC14" : "PC15";
    bool& v = g_pin[n - 13];
    switch (op) {
        case HostPin::Set: v = true; log(std::string(name) + "=1"); break;
        case HostPin::Clear: v = false; log(std::string(name) + "=0"); break;
        case HostPin::Read: return v;
        case HostPin::Mode: break;
    }
    update_vconn();
    return v;
}
}  // namespace reg

namespace i2c {
bool write(Bus, std::uint8_t, std::uint8_t reg, std::span<const std::uint8_t> d) {
    if (reg == 0x43) {
        g_txfifo.assign(d.begin(), d.end());
        std::string s = "FIFO";
        for (std::uint8_t b : d) s += " " + hex(b);
        log(s);
        return true;
    }
    const std::uint8_t v = d.empty() ? 0 : d[0];
    log("W " + hex(reg) + "=" + hex(v));
    g_reg[reg] = v;
    if (reg == 0x0C && (v & 0x01)) g_reg[0x02] = 0x03;   // SW_RES: the chip is back at its power-on SWITCHES0, Rd on both lines
    if (reg == 0x06 && (v & 0x01)) { tx_start(); if (g_auto_txsent) g_reg[0x3E] |= 0x04; }
    if (reg == 0x07 && (v & 0x04)) g_rx.clear();
    update_vconn();
    return true;
}
bool read(Bus, std::uint8_t, std::uint8_t reg, std::span<std::uint8_t> out) {
    if (reg == 0x43) {
        for (auto& b : out) {
            b = g_rx.empty() ? 0 : g_rx.front();
            if (!g_rx.empty()) g_rx.pop_front();
        }
        return true;
    }
    if (reg == 0x41) { out[0] = g_rx.empty() ? 0x20 : 0x00; return true; }   // RX_EMPTY
    log("R " + hex(reg));
    for (auto& b : out) b = g_reg[reg];
    if (reg == 0x3E && !g_ia_script.empty()) { out[0] = g_ia_script.front(); g_ia_script.pop_front(); }
    if (reg == 0x3E && g_auto_txsent) g_reg[0x3E] = 0;   // clear-on-read
    if (reg == 0x40 && (g_reg[0x40] & 0x20)) {   // COMP: the measured line is above the MDAC threshold (MEASURE[5:0] x 42 mV)
        const int thr = (g_reg[0x04] & 0x3F) * 42;
        const int meas = (g_reg[0x02] & 0x0C) == 0x08 ? 1 : (g_reg[0x02] & 0x0C) == 0x04 ? 0 : -1;   // MEAS_CC2 / MEAS_CC1 (SWITCHES0 bits 3:2)
        int mv = meas == 0 && g_line_mv_cc[0] >= 0 ? g_line_mv_cc[0]
               : meas == 1 && g_line_mv_cc[1] >= 0 ? g_line_mv_cc[1] : g_line_mv;
        if (g_rd_model && meas >= 0) mv = (g_reg[0x02] & (1u << meas)) ? g_lvl_rd[meas] : g_lvl_nord[meas];
        if (mv <= thr) for (auto& b : out) b &= std::uint8_t(~0x23);   // no Rp: COMP and BC_LVL both read 0
    }
    return true;
}
}  // namespace i2c

namespace systick {
std::uint32_t millis() { return g_ms; }
std::uint32_t micros() { return g_ms * 1000u; }
void delay_ms(std::uint32_t ms) { g_ms += ms; }
void delay_us(std::uint32_t us) { g_ms += (us + 999u) / 1000u; }
}  // namespace systick

namespace usb {
void log(std::string_view m) { g_usb.emplace_back(m); }
void log_fmt(const char* fmt, ...) {
    char b[160];
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_usb.emplace_back(b);
}
}  // namespace usb

namespace adc {
bool latest_cc(std::uint16_t&, std::uint16_t&) { return false; }   // the VCONN glue reads the CC nets; no ADC here
}  // namespace adc

namespace pinmark {   // the backup register that survives a warm reset
std::uint16_t read() { return g_mark; }
void write(std::uint16_t raw) { g_mark = raw; log("MARK " + hex(raw >> 8) + hex(raw & 0xFF)); }
}  // namespace pinmark

}  // namespace k2

namespace {

void reset_world() {
    g_log.clear(); g_usb.clear(); g_rx.clear(); g_txfifo.clear(); g_ia_script.clear(); g_auto_txsent = false;
    for (auto& r : g_reg) r = 0;
    g_pin[0] = false; g_pin[1] = true; g_pin[2] = true;      // idle: PC13 low, PC14/PC15 high
    g_vconn_since = 0;
    g_ms = 1000;
    g_line_mv = 1600;
    g_line_mv_cc[0] = g_line_mv_cc[1] = -1;
    g_rd_model = false;
    g_lvl_rd[0] = g_lvl_rd[1] = g_lvl_nord[0] = g_lvl_nord[1] = 0;
    g_cable = Cable{};
    g_reg[0x02] = 0x03;                                      // PDWN1|PDWN2 (Rd), nothing measured
    g_reg[0x03] = 0x20;                                      // passive: SPECREV=01, Auto-CRC off
}

bool usb_has(const char* needle) {
    for (const auto& s : g_usb) if (s.find(needle) != std::string::npos) return true;
    return false;
}

// Position of `s` in the log at or after `from`, or -1.
int find_from(const std::string& s, std::size_t from = 0) {
    for (std::size_t i = from; i < g_log.size(); i++) if (g_log[i] == s) return int(i);
    return -1;
}

// The sequence of one orientation as b79ce3d wrote it, up to the first TX_START.
const char* kDi3 = "FIFO 12 12 1B 1B 86 8F 10 01 A0 00 FF FF 14 FE";
const char* kDi2 = "FIFO 12 12 1B 1B 86 4F 10 01 80 00 FF FF 14 FE";

std::vector<std::string> orientation_prefix(const char* sw0, const char* sw1) {
    // read_emarker() closes the CC switches and starts VCONN (old: one BSRR), then each orientation
    // repeats it (old: the BSRR at the top of scan_orientation) before POWER and the two SWITCHES.
    // Before any of that the pre-scan POWER / SWITCHES0 / SWITCHES1 are read back for the restore.
    return {"R 0B", "R 02", "R 03", "PC14=1", "PC15=1", "PC13=1", "PC14=1", "PC15=1", "PC13=1", "W 0B=0F", sw0, sw1};
}

bool starts_with(const std::vector<std::string>& hay, std::size_t at, const std::vector<std::string>& seq) {
    if (at + seq.size() > hay.size()) return false;
    for (std::size_t i = 0; i < seq.size(); i++) if (hay[at + i] != seq[i]) return false;
    return true;
}

// Attempt: TX_FLUSH, RX_FLUSH with the passive CONTROL1 (0x67), three interrupt reads, FIFO write, TX_START.
std::vector<std::string> attempt(const char* di) {
    return {"W 06=40", "W 07=67", "R 42", "R 3E", "R 3F", di, "W 06=01"};
}

void test_cable_on_cc1_orientation_a() {
    reset_world();
    g_cable.cc = 1;                                          // VCONN on CC2, BMC on CC1: orientation A
    const bool found = fusb302::read_emarker(false, pd::EmarkerMode::Vconn);
    CHECK_EQ(found, true);
    CHECK_EQ(g_cable.answered, 1);                           // the PD 3.0 attempt of A already works
    std::vector<std::string> want = orientation_prefix("W 02=24", "W 03=25");
    for (const auto& s : attempt(kDi3)) want.push_back(s);
    CHECK_EQ(starts_with(g_log, 0, want), true);
    // Restore: PC13 low, Rd on both lines (no active CC: nothing measured), Auto-CRC off (passive), FIFOs flushed.
    const int pc13_off = find_from("PC13=0");
    CHECK_EQ(pc13_off > 0, true);
    CHECK_EQ(find_from("W 02=03", pc13_off) > 0, true);
    CHECK_EQ(find_from("W 03=20", pc13_off) > 0, true);
    CHECK_EQ(g_pin[0], false);
    CHECK_EQ(g_pin[1], true);
    CHECK_EQ(g_pin[2], true);
    CHECK_EQ(usb_has("Response on Orientation A"), true);
    CHECK_EQ(usb_has("E-Marker detected: VID=0x05AC"), true);
    CHECK_EQ(fusb302::emarker().valid, true);
    CHECK_EQ(fusb302::emarker().max_current_a, 5);
}

void test_cable_on_cc2_orientation_b() {
    reset_world();
    g_cable.cc = 2;                                          // A fails (no answer), B works
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), true);
    // A: both attempts, nobody answered.
    std::vector<std::string> a = orientation_prefix("W 02=24", "W 03=25");
    CHECK_EQ(starts_with(g_log, 0, a), true);
    const int a1 = find_from(kDi3);
    const int a2 = find_from(kDi2, a1 + 1);                  // the PD 2.0 retry (deliberate change)
    CHECK_EQ(a1 > 0, true);
    CHECK_EQ(a2 > a1, true);
    // B follows A with its own switches, still with PC13 high.
    const int b = find_from("W 02=18", a2);
    CHECK_EQ(b > a2, true);
    CHECK_STR(g_log[b + 1].c_str(), "W 03=26");
    CHECK_STR(g_log[b - 1].c_str(), "W 0B=0F");
    CHECK_EQ(g_cable.answered >= 1, true);
    CHECK_EQ(usb_has("Response on Orientation B"), true);
}

void test_pd2_only_cable_answers_the_retry() {
    reset_world();
    g_cable.cc = 1;
    g_cable.answers_pd3 = false;
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), true);
    CHECK_EQ(g_cable.tx_seen, 2);                            // PD 3.0 unanswered, PD 2.0 answered
    CHECK_EQ(find_from(kDi2) > find_from(kDi3), true);
}

void test_no_cable_scans_both_orientations_and_restores() {
    reset_world();
    g_cable.answers_pd3 = false;
    g_cable.answers_pd2 = false;
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), false);
    CHECK_EQ(g_cable.tx_seen, 4);                            // 2 orientations x 2 attempts
    CHECK_EQ(usb_has("No E-Marker detected"), true);
    CHECK_EQ(g_pin[0], false);                               // VCONN off again
    CHECK_EQ(g_reg[0x02], 0x03);
    CHECK_EQ(g_reg[0x03], 0x20);
    // Two orientations, each at least 40 ms plus two 80 ms windows of polling.
    CHECK_EQ(g_ms - 1000 >= 2 * (40 + 2 * 80), true);
}

void test_vconn_needs_pc13_and_the_far_line() {
    // Mutation guards for the fake itself: a cable does not answer without VCONN on the right line.
    reset_world();
    g_cable.cc = 1;
    g_pin[0] = true; g_pin[1] = true; g_pin[2] = true;
    g_reg[0x02] = 0x18;                                      // VCONN on CC1 = the cable's own wire
    g_reg[0x03] = 0x25;
    g_vconn_since = 1;
    g_txfifo = {0x12, 0x12, 0x1B, 0x1B, 0x86, 0x8F, 0x10, 0x01, 0xA0, 0x00, 0xFF, 0xFF, 0x14, 0xFE};
    i2c::write_reg(i2c::Bus::Fusb, 0x22, 0x06, 0x01);
    CHECK_EQ(g_cable.answered, 0);
}

void test_inline_gate_refuses_unconfirmed_vconn() {
    reset_world();
    fusb302::set_inline(true);
    g_log.clear();
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), false);
    CHECK_EQ(g_cable.tx_seen, 0);
    CHECK_EQ(find_from("PC13=1"), -1);
    CHECK_EQ(usb_has("Refused"), true);
    fusb302::set_inline(false);
}

void test_sniff_never_touches_cc_or_vconn() {
    reset_world();
    g_cable.cc = 1;
    g_log.clear();
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Sniff), false);   // nothing heard yet
    CHECK_EQ(g_cable.tx_seen, 0);                            // never transmits
    CHECK_EQ(g_pin[0], false);                               // PC13 (VCONN) stays low
    CHECK_EQ(find_from("PC13=1"), -1);
    CHECK_EQ(find_from("PC14=0"), -1);
    CHECK_EQ(find_from("PC15=0"), -1);
    CHECK_EQ(g_reg[0x02], 0x03);                             // Rd still on
    for (const auto& s : g_log) {
        CHECK_EQ(s.rfind("W 02", 0) == 0 || s.rfind("W 03", 0) == 0 || s.rfind("W 0B", 0) == 0 ||
                 s.rfind("W 06", 0) == 0 || s.rfind("FIFO", 0) == 0, false);
    }
    CHECK_EQ(g_log.size(), 0u);                              // passive CONTROL1 already receives SOP': no write at all
    CHECK_EQ(usb_has("no VCONN injection"), true);
}

}  // namespace

// A failed scan leaves the chip exactly as it found it: the SWITCHES0 value of an attached sink
// (Rd on both lines, CC1 measured) and BMC routing come back, not a reconstruction from a guess.
void test_failed_scan_restores_the_pre_scan_state() {
    reset_world();
    g_reg[0x02] = 0x07;                                      // PDWN1|PDWN2|MEAS_CC1: attached on CC1
    g_reg[0x03] = 0x21;                                      // SPECREV=01 | TX on CC1, Auto-CRC off
    g_reg[0x0B] = 0x0F;                                      // all blocks powered
    g_cable.answers_pd3 = false;
    g_cable.answers_pd2 = false;
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), false);
    CHECK_EQ(int(g_reg[0x02]), 0x07);
    CHECK_EQ(int(g_reg[0x03]), 0x21);
    CHECK_EQ(int(g_reg[0x0B]), 0x0F);
    CHECK_EQ(g_pin[0], false);                               // VCONN off
    // And a second scan after the failed one still works.
    g_cable.answers_pd3 = true;
    g_cable.answers_pd2 = true;
    g_cable.cc = 1;
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), true);
    CHECK_EQ(int(g_reg[0x02]), 0x07);
    CHECK_EQ(int(g_reg[0x03]), 0x21);                        // TX back on CC1, not left on the scan's VCONN pin
    CHECK_EQ(int(g_reg[0x0B]), 0x0F);
    CHECK_EQ(g_pin[0], false);                               // VCONN off after a successful scan too
}

// START asks the driver whether an awake source's Rp is on the wire before it talks on CC: the C135 hard-reset
// 180 ms after Rd when the VCONN scan had put SOP' traffic on its CC mid Rp cycle (hardware 2026-11-03).
void test_rp_on_line_sees_a_source_and_leaves_the_chip_alone() {
    reset_world();
    fusb302::init();
    g_line_mv = 0;                                           // nothing drives either line
    g_reg[0x40] = 0x20;
    const std::uint8_t sw0 = g_reg[0x02], mdac = g_reg[0x04];
    CHECK_EQ(fusb302::rp_on_line(), false);
    CHECK_EQ(int(g_reg[0x02]), int(sw0));                    // measure switch and threshold back as they were
    CHECK_EQ(int(g_reg[0x04]), int(mdac));
    g_line_mv = 3300;                                        // an unloaded source Rp (the C135 before Rd)
    CHECK_EQ(fusb302::rp_on_line(), true);
    CHECK_EQ(int(g_reg[0x02]), int(sw0));
    CHECK_EQ(int(g_reg[0x04]), int(mdac));
    CHECK_EQ(g_pin[0], false);                               // never any VCONN or TX from this check
    CHECK_EQ(find_from("W 06=01"), -1);
}

// Passive detach: no Rp for 500 ms, or VBUS below 3.0 V for 1 s. The order on the wire is
// RESET=0x03, SWITCHES0=0, then the passive profile again.
void attach_on_cc1() {
    reset_world();
    g_reg[0x40] = 0x20;                                      // STATUS0.COMP: Rp on the measured line
    for (int i = 0; i < 60 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }  // 2 Hz detect
}

void test_detach_on_cc_loss_after_500ms() {
    attach_on_cc1();
    CHECK_EQ(usb_has("Active CC detected: CC1"), true);
    g_reg[0x40] = 0x00;
    g_log.clear();
    for (int i = 0; i < 20; i++) { g_ms += 20; fusb302::poll(); }      // 400 ms: still attached
    CHECK_EQ(find_from("W 0C=02"), -1);
    for (int i = 0; i < 6; i++) { g_ms += 20; fusb302::poll(); }       // past 500 ms
    const int rst = find_from("W 0C=02");
    CHECK_EQ(rst >= 0, true);
    const int sw0 = find_from("W 02=00", std::size_t(rst));
    CHECK_EQ(sw0 > rst, true);
    CHECK_EQ(find_from("W 07=67", std::size_t(sw0)) > sw0, true);      // passive CONTROL1 again
    CHECK_EQ(usb_has("Detach"), true);
}

void test_detach_on_low_vbus_after_1s() {
    attach_on_cc1();
    fusb302::sink_vbus(5000); fusb302::poll();   // VBUS was there: only then does losing it mean a detach
    g_log.clear();
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::sink_vbus(2000); fusb302::poll(); }   // 800 ms
    CHECK_EQ(find_from("W 0C=02"), -1);
    for (int i = 0; i < 15; i++) { g_ms += 20; fusb302::sink_vbus(2000); fusb302::poll(); }   // > 1 s
    CHECK_EQ(find_from("W 0C=02") >= 0, true);
}

void test_vbus_zero_throughout_is_no_detach() {
    // QA: a charger on the top port, VBUS read 0 mV the whole time, flapped attach/detach. With Rp on
    // the line and VBUS never seen, nothing may detach.
    attach_on_cc1();
    g_log.clear();
    for (int i = 0; i < 400; i++) { g_ms += 20; fusb302::sink_vbus(0); fusb302::poll(); }   // 8 s
    CHECK_EQ(find_from("W 0C=02"), -1);
    CHECK_EQ(usb_has("Detach"), false);
}

// loop.txt: laptop (Type-C current only) -> K2 -> Pixel. Inline starts, the line never reads in the sink
// band (Rd not visible, BC_LVL=3): no detach, the Pixel keeps charging.
void test_inline_never_seeing_a_sink_is_not_a_detach() {
    attach_on_cc1();
    g_log.clear();
    fusb302::set_inline(true);
    g_reg[0x40] = 0x23;                                  // COMP + BC_LVL=3
    for (int i = 0; i < 250; i++) { g_ms += 20; fusb302::sink_vbus(4850); fusb302::sink_cc(3300, 100); fusb302::poll(); }
    CHECK_EQ(find_from("W 0C=02"), -1);
    CHECK_EQ(fusb302::inline_mode(), true);
}

// No PD caps 1 s after attach: a plain Type-C source, current from the Rp level (BC_LVL).
void test_typec_only_source_after_one_second_without_caps() {
    reset_world();
    fusb302::init();                                     // earlier tests leave Passive latched; only a detach/init clears it
    fusb302::set_inline(false);
    g_reg[0x40] = 0x22;                                  // COMP + BC_LVL=2 (our Rd on, Pixel's Rd in parallel)
    std::uint32_t t0 = 0, found = 0;
    for (int i = 0; i < 200 && !found; i++) {
        g_ms += 20;
        if (i == 30) g_reg[0x40] = 0x23;                 // Rd off inline: the divider reads BC_LVL=3
        fusb302::sink_vbus(4850);
        fusb302::poll();
        if (!t0 && fusb302::active_cc()) t0 = g_ms;
        if (fusb302::typec_ma()) found = g_ms;
        else if (t0) CHECK_EQ(g_ms - t0 < 3100, true);   // decided within about 3 s of attach
    }
    CHECK_EQ(t0 != 0, true);
    CHECK_EQ(int(fusb302::typec_ma()), 3000);            // highest level since attach
}

// The laptop sent its caps before the K2 listened and a Pixel is behind it: PD traffic or inline means a PD
// source, never "Type-C only", however long the caps stay unseen.
void test_no_typec_only_with_pd_traffic_or_inline() {
    for (int mode = 1; mode >= 0; mode--) {   // 1: inline, nothing seen; 0: a PS_RDY seen, no caps (the attach carries over)
        reset_world();
        fusb302::set_inline(false);
        g_reg[0x40] = 0x23;
        std::uint32_t t0 = 0;
        for (int i = 0; i < 400; i++) {
            g_ms += 20;
            fusb302::sink_vbus(4850);
            fusb302::poll();
            if (!t0 && fusb302::active_cc()) {
                t0 = g_ms;
                if (mode == 0) { const std::uint8_t none[1] = {0}; fusb302::feed(0, 0x0006 | (2 << 6), std::span<const std::uint8_t>(none, 0)); }
                else fusb302::set_inline(true);
            }
        }
        CHECK_EQ(t0 != 0, true);
        CHECK_EQ(int(fusb302::typec_ma()), 0);
        CHECK_EQ(fusb302::pd_traffic(), mode == 0);
    }
}

void test_inline_drops_rd_and_detects_the_sink_leaving() {
    attach_on_cc1();
    g_log.clear();
    fusb302::set_inline(true);
    CHECK_EQ(find_from("W 02=04") >= 0, true);          // measure only: no Rd of ours on the line
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(1500, 100); fusb302::poll(); }
    CHECK_EQ(find_from("W 0C=02"), -1);                  // the sink's Rd is there: attached
    for (int i = 0; i < 20; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(3300, 100); fusb302::poll(); }
    CHECK_EQ(find_from("W 0C=02"), -1);                  // 400 ms without a sink: still waiting
    for (int i = 0; i < 8; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(3300, 100); fusb302::poll(); }
    CHECK_EQ(find_from("W 0C=02") >= 0, true);           // past 500 ms: detach
    CHECK_EQ(usb_has("the sink left"), true);
    CHECK_EQ(fusb302::inline_mode(), true);              // the latch is the Link's (held kInlineHoldMs): a detach does not clear it
    fusb302::set_inline(false);
}

// Inline (a real sink on the other port): after the sink left, the line stays at SWITCHES0 = 0 (no Rd in
// plain boot) and a charger that shows up is found by the next scan, with no hold delaying it.
void test_inline_rd_stays_off_after_a_detach_then_comes_back() {
    attach_on_cc1();
    fusb302::set_inline(true);
    for (int i = 0; i < 10; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(1500, 100); fusb302::poll(); }
    g_log.clear();
    g_reg[0x40] = 0x00;                                  // the charger side goes quiet with the sink
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(3300, 100); fusb302::poll(); }
    const int rst = find_from("W 0C=02");
    CHECK_EQ(rst >= 0, true);
    CHECK_EQ(find_from("W 0C=03"), -1);                                 // never SW_RES
    CHECK_EQ(find_from("W 02=03"), -1);                                 // no Rd
    g_reg[0x40] = 0x20;                                  // a charger shows up right away
    g_log.clear(); g_usb.clear();
    for (int i = 0; i < 30; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    CHECK_EQ(find_from("W 02=03"), -1);
    fusb302::set_inline(false);
}

void test_inline_hold_ends_with_rd_back_even_when_nothing_is_there() {
    attach_on_cc1();
    fusb302::set_inline(true);
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(3300, 100); fusb302::poll(); }
    g_reg[0x40] = 0x00;                                  // nothing on the line at all
    g_log.clear();
    for (int i = 0; i < 85; i++) { g_ms += 20; fusb302::poll(); }      // 1.7 s: the hold is over
    CHECK_EQ(find_from("W 02=03"), -1);                  // plain boot: Rd never comes back (passive)
    fusb302::set_inline(false);                          // the Link's latch outlives the detach: this test ends it
}

// Plain boot is the passive state: after a detach SWITCHES0 is left at 0 (no Rd); a cheap charger
// that needs Rd is served by the sink boot modes (PLUS / OK held at power-up).
void test_source_alone_gets_rd_back_after_a_detach() {
    attach_on_cc1();
    CHECK_EQ(fusb302::inline_mode(), false);
    g_reg[0x40] = 0x00;                                  // charger unplugged
    g_log.clear();
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); }
    const int rst = find_from("W 0C=02");
    CHECK_EQ(rst >= 0, true);
    std::string last_sw0;                                // what the line is left at after the detach
    for (std::size_t i = std::size_t(rst); i < g_log.size(); i++)
        if (g_log[i].rfind("W 02=", 0) == 0) last_sw0 = g_log[i];
    CHECK_STR(last_sw0.c_str(), "W 02=00");              // plain boot: no Rd after a detach either, listen only
    g_reg[0x40] = 0x20;                                  // charger plugged in again
    g_usb.clear();
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); }      // 0.8 s: the scan finds it, no hold
    CHECK_EQ(usb_has("Active CC detected"), true);
}

void test_good_vbus_and_rp_keep_the_attach() {
    attach_on_cc1();
    g_log.clear();
    for (int i = 0; i < 200; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::poll(); }
    CHECK_EQ(find_from("W 0C=02"), -1);
}

// ---- Rd-window re-check on fresh ADC levels (the cold-boot case) ----
// The attach scan runs ~1 s after reset, before the UI has fed sink_cc() any ADC levels, so its
// cross-check saw -1/-1 and latched whatever the comparator saw. Attach on CC2 with unknown levels.
void attach_cc2_unknown_levels() {
    reset_world();
    fusb302::sink_cc(-1, -1);                        // cold boot: no ADC levels yet
    g_line_mv_cc[0] = 100; g_line_mv_cc[1] = 1600;   // the comparator sees Rp only on CC2 at scan time
    g_reg[0x40] = 0x23;                               // COMP + BC_LVL=3
    fusb302::init();
    for (int i = 0; i < 60 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    g_ms = 100000;   // the driver's re-check rate-limit timestamp survives this reset_world (g_ms restarts at 1000): jump past it
    CHECK_EQ(usb_has("Active CC detected: CC2"), true);
    CHECK_EQ(int(fusb302::active_cc()), 2);
    CHECK_EQ(usb_has("CC override"), false);          // unknown levels: the scan could not cross-check
    g_log.clear(); g_usb.clear();
}

void detach_and_forget_levels() {
    g_line_mv_cc[0] = g_line_mv_cc[1] = 100;
    g_reg[0x40] = 0x00;                               // no Rp anywhere: the 500 ms rule detaches
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    fusb302::sink_cc(-1, -1);
}

// Fresh ADC levels arrive while CC2 is attached and no PD message was decoded on it yet: the Rd window
// names CC1 (1678 mV against an open 3307 mV), so the attach moves to CC1, applied like a fresh attach
// (switches, SWITCHES0 measure bit, SWITCHES1 routing, a new session), and the override is logged.
void test_rd_window_recheck_moves_a_wrongly_latched_line_on_fresh_levels() {
    attach_cc2_unknown_levels();
    const auto session0 = fusb302::session();
    g_line_mv_cc[0] = 1600; g_line_mv_cc[1] = 3300;   // the comparator agrees now: CC1 live, CC2 open Rp
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::sink_cc(1678, 3307); fusb302::poll(); fusb302::service(); }
    CHECK_EQ(int(fusb302::active_cc()), 1);
    CHECK_EQ(usb_has("[FUSB302] CC override"), true);
    CHECK_EQ(int(g_reg[0x02]), 0x04);                 // SWITCHES0: MEAS_CC1, no Rd (plain passive boot)
    CHECK_EQ(int(g_reg[0x03] & 0x03), 0x01);          // SWITCHES1: BMC/TX routed to CC1 (phy::sw1)
    CHECK_EQ(fusb302::session() != session0, true);   // a new attach session: the PD state was reset
    // stable: the same levels no longer name another line, the override fired once
    g_usb.clear();
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::sink_cc(1678, 3307); fusb302::poll(); fusb302::service(); }
    CHECK_EQ(int(fusb302::active_cc()), 1);
    CHECK_EQ(usb_has("CC override"), false);
    CHECK_EQ(usb_has("Detach"), false);               // Rp on the new line keeps the attach
    detach_and_forget_levels();
}

// Negative: ambiguous levels (408, 1) name no line, and a forced debug line is an explicit choice.
void test_rd_window_recheck_ignores_ambiguous_levels_and_a_forced_line() {
    attach_cc2_unknown_levels();
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::sink_cc(408, 1); fusb302::poll(); fusb302::service(); }
    CHECK_EQ(int(fusb302::active_cc()), 2);
    CHECK_EQ(usb_has("CC override"), false);
    fusb302::set_force_line(2);                       // pd-reread --line 2: never overridden
    g_line_mv_cc[0] = 1600; g_line_mv_cc[1] = 3300;
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::sink_cc(1678, 3307); fusb302::poll(); fusb302::service(); }
    CHECK_EQ(int(fusb302::active_cc()), 2);
    CHECK_EQ(usb_has("CC override"), false);
    fusb302::set_force_line(0);
    detach_and_forget_levels();
}

// Negative: once a PD message was decoded on the attached line, CC is never touched again.
void test_rd_window_recheck_never_runs_after_pd_traffic() {
    attach_cc2_unknown_levels();
    push_frame(0xE0, std::uint16_t(0x1001 | (2u << 6)), {0x0801912Cu});   // Source_Capabilities: 5 V 3 A
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(fusb302::pd_traffic(), true);
    g_line_mv_cc[0] = 1600; g_line_mv_cc[1] = 3300;
    g_usb.clear();
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::sink_cc(1678, 3307); fusb302::poll(); fusb302::service(); }
    CHECK_EQ(int(fusb302::active_cc()), 2);
    CHECK_EQ(int(g_reg[0x02] & 0x0C), 0x08);          // still measuring CC2
    CHECK_EQ(usb_has("CC override"), false);
    detach_and_forget_levels();
}

void test_rescan_is_only_the_cc_switch_cycle() {
    attach_on_cc1();
    g_log.clear();
    fusb302::rescan();
    int chip = 0;                            // PCxx = the CC switches; anything else is a chip access
    for (const auto& e : g_log) if (e[0] != 'P') chip++;
    CHECK_EQ(chip, 0);
    CHECK_EQ(find_from("PC14=0") >= 0 && find_from("PC14=1") > find_from("PC14=0"), true);
    CHECK_EQ(usb_has("Rescan requested"), true);
}

void test_service_drains_the_fifo_past_the_four_packet_pass() {
    reset_world();
    for (int i = 0; i < 6; i++) push_frame(0xE0, 0x1041, {0x0001912C});   // 6 x Source_Capabilities (5 V 3 A)
    fusb302::on_int_n();
    fusb302::service();
    CHECK_EQ(g_rx.empty(), true);   // 4 per pass, a second pass because the FIFO was not empty
}

// A sink Request goes to the source: SOP is Sync-1 Sync-1 Sync-1 Sync-2 (0x12 0x12 0x12 0x13). The
// Sync-1 Sync-1 Sync-3 Sync-3 the driver used is SOP', the cable's address, so a source never ACKed it.
void test_sink_request_goes_out_as_sop_not_sop_prime() {
    reset_world();
    fusb302::init();
    attach_on_cc1();
    const std::uint8_t pdo5v3a[4] = {0x2C, 0x91, 0x01, 0x08};                  // fixed 5 V 3 A
    const std::uint16_t caps_hdr = std::uint16_t(0x1001 | (2u << 6));
    fusb302::feed(0, caps_hdr, pdo5v3a);
    for (int i = 0; i < 5; i++) { g_ms += 2; fusb302::poll(); }
    g_txfifo.clear();
    const pd::Err e = fusb302::sink_request({.pos = 1}, true);
    CHECK_EQ(int(e), int(pd::Err::Ok));
    bool refed = false;
    for (int i = 0; i < 120 && g_txfifo.empty(); i++) {
        g_ms += 5;
        fusb302::poll();
        if (!refed && usb_has("CC closed")) { refed = true; fusb302::feed(0, caps_hdr, pdo5v3a); }   // the source re-announces
    }
    CHECK_EQ(g_txfifo.size() > 8, true);
    if (g_txfifo.size() > 8) {
        CHECK_EQ(int(g_txfifo[0]), 0x12);
        CHECK_EQ(int(g_txfifo[1]), 0x12);
        CHECK_EQ(int(g_txfifo[2]), 0x12);
        CHECK_EQ(int(g_txfifo[3]), 0x13);
    }
}

// The source already has a contract with a device on the other port: its Request was sniffed, but the
// link has not latched inline yet. A trigger must not put a Request on the wire (a hard reset follows).
void test_trigger_refused_after_a_foreign_request_without_inline() {
    reset_world();
    fusb302::init();
    attach_on_cc1();
    const std::uint8_t pdo5v3a[4] = {0x2C, 0x91, 0x01, 0x08};
    fusb302::feed(0, std::uint16_t(0x1001 | (2u << 6)), pdo5v3a);
    const std::uint8_t rdo[4] = {0x2C, 0x91, 0x01, 0x10};                      // pos 1, someone else's Request
    fusb302::feed(0, std::uint16_t(0x1002 | (2u << 6)), rdo);
    for (int i = 0; i < 5; i++) { g_ms += 2; fusb302::poll(); }
    CHECK_EQ(fusb302::inline_mode(), false);
    g_txfifo.clear();
    g_log.clear();
    CHECK_EQ(int(fusb302::sink_request({.pos = 1}, true)), int(pd::Err::Inline));
    for (int i = 0; i < 120; i++) { g_ms += 5; fusb302::poll(); }
    CHECK_EQ(g_txfifo.empty(), true);                                           // no TX at all
    CHECK_EQ(usb_has("[PD] trigger refused: inline (request from another sink)"), true);
    CHECK_EQ(int(fusb302::trigger_refusal()), int(pd::TriggerRefusal::ForeignRequest));
    // A fresh attach is a new session: the old Request no longer counts.
    g_reg[0x40] = 0x00;
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(int(fusb302::trigger_refusal()), int(pd::TriggerRefusal::None));
}

void test_passive_30v_physical_identity() {
    reset_world();
    CHECK_EQ(fusb302::init(), true);
    attach_on_cc1();
    const std::vector<std::uint32_t> identity{
        0xff00a041, 0x180022d9, 0, 0x11960000, 0x60082a40
    };
    std::vector<std::uint8_t> body;
    for (auto word : identity) for (unsigned n = 0; n < 4; ++n) body.push_back(word >> (n * 8));
    fusb302::feed(1, 0x518F, body);
    CHECK_EQ(fusb302::physical_cable().observed, false);
    push_frame(0xC0, 0x518F, identity);
    fusb302::service();
    CHECK_EQ(fusb302::physical_cable().observed, true);
    CHECK_EQ(fusb302::physical_cable().max_mv, 30000);
    CHECK_EQ(fusb302::physical_cable().max_ma, 5000);
    fusb302::set_sop_prime_hook(+[](std::uint8_t, std::uint16_t, std::span<const std::uint8_t>) {});
    auto other = identity;
    other[4] |= 2u << 9;
    push_frame(0xC0, 0x518F, other);
    fusb302::service();
    CHECK_EQ(fusb302::physical_cable().max_mv, 30000);
    fusb302::set_sop_prime_hook(nullptr);
    g_reg[0x40] = 0;
    for (unsigned i = 0; i < 40; ++i) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(fusb302::physical_cable().observed, false);
    CHECK_EQ(fusb302::physical_cable().max_mv, 20000);
}

void test_passive_physical_identity_and_source_revision_provenance() {
    reset_world();
    CHECK_EQ(fusb302::init(), true);
    attach_on_cc1();
    push_frame(0xE0, 0x1181, {0x0001912C});
    fusb302::service();
    CHECK_EQ(fusb302::caps().spec_rev, 2);
    CHECK_EQ(fusb302::physical_cable().observed, false);
    const std::vector<std::uint32_t> identity{
        0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)
    };
    std::vector<std::uint8_t> body;
    for (auto word : identity) for (unsigned n = 0; n < 4; ++n) body.push_back(word >> (n * 8));
    fusb302::feed(1, 0x518F, body);
    CHECK_EQ(fusb302::emarker().spec_rev, 2);
    CHECK_EQ(fusb302::physical_cable().observed, false);
    CHECK_EQ(fusb302::physical_cable().max_mv, 20000);
    CHECK_EQ(fusb302::physical_cable().max_ma, 3000);
    push_frame(0xC0, 0x518F, identity);
    fusb302::service();
    CHECK_EQ(fusb302::physical_cable().observed, true);
    CHECK_EQ(fusb302::physical_cable().max_mv, 50000);
    CHECK_EQ(fusb302::physical_cable().max_ma, 5000);

    const auto revision = pd::make_header(pd::kDataRevision, 1, 2);
    push_frame(0xE0, revision, {0x32170000});
    fusb302::service();
    CHECK_EQ(fusb302::source_revision().valid, false);
    push_frame(0xC0, revision | (1u << 8), {0x32170000});
    fusb302::service();
    CHECK_EQ(fusb302::source_revision().valid, false);
    push_frame(0xE0, revision | (1u << 8), {0x32170000});
    fusb302::service();
    const auto& report = fusb302::source_revision();
    CHECK_EQ(report.valid, true); CHECK_EQ(report.rev_major, 3); CHECK_EQ(report.rev_minor, 2);
    CHECK_EQ(report.ver_major, 1); CHECK_EQ(report.ver_minor, 7);
    CHECK_EQ(g_cable.tx_seen, 0); CHECK_EQ(g_pin[0], false);

    const auto session = fusb302::session();
    g_reg[0x40] = 0;
    for (unsigned i = 0; i < 40; ++i) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(fusb302::session() != session, true);
    CHECK_EQ(fusb302::physical_cable().observed, false);
    CHECK_EQ(fusb302::source_revision().valid, false);
    attach_on_cc1();
    fusb302::set_sop_prime_hook(+[](std::uint8_t, std::uint16_t, std::span<const std::uint8_t>) {});
    push_frame(0xC0, 0x518F, identity);
    fusb302::service();
    CHECK_EQ(fusb302::physical_cable().observed, false);
    fusb302::set_sop_prime_hook(nullptr);
    CHECK_EQ(fusb302::physical_cable().observed, false); // disarm cannot promote cached spoof-era data
    push_frame(0xC0, 0x51CF, identity);
    fusb302::service();
    CHECK_EQ(fusb302::emarker().spec_rev, 3);
    CHECK_EQ(fusb302::physical_cable().observed, false);
    push_frame(0xC0, 0x518F, identity);
    fusb302::service();
    CHECK_EQ(fusb302::physical_cable().observed, true);
    CHECK_EQ(g_cable.tx_seen, 0); CHECK_EQ(g_pin[0], false);

    reset_world();
    CHECK_EQ(fusb302::init(), true);
    CHECK_EQ(fusb302::physical_cable().observed, false);
    CHECK_EQ(fusb302::physical_cable().max_mv, 20000);
    CHECK_EQ(fusb302::physical_cable().max_ma, 3000);
}

void test_vconn_level_line_is_not_an_active_cc_and_detach_never_soft_resets() {
    reset_world();
    fusb302::init();                                     // forget the previous test's attach
    g_log.clear(); g_usb.clear();
    g_line_mv = 3300;                                    // VCONN / unloaded Rp level, above 2.6 V
    g_reg[0x40] = 0x20;
    g_usb.clear();
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), false);
    g_line_mv = 1595;                                    // a real Rp
    for (int i = 0; i < 60 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_reg[0x40] = 0x00;                                  // detach
    g_log.clear();
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(find_from("W 0C=03"), -1);                  // SW_RES would put Rd back on both lines
    CHECK_EQ(find_from("W 0C=01"), -1);
}

// A run start presents Rd itself (plain boot has none): on the line that sees Rp, both lines while no partner
// is detected, never while a sink is inline; off again when the run ends.
void test_run_start_presents_rd() {
    attach_on_cc1();
    CHECK_EQ(fusb302::rd_for_run_on(), false);
    g_log.clear();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    CHECK_EQ(find_from("W 02=05"), 0);                   // PDWN1 + MEAS_CC1: Rd on the Rp line only
    CHECK_EQ(fusb302::rd_for_run_on(), true);
    g_log.clear();
    fusb302::rd_for_run(false);
    CHECK_EQ(find_from("W 02=04"), 0);                   // measure only again
    CHECK_EQ(fusb302::rd_for_run_on(), false);
    fusb302::set_inline(true);
    g_log.clear();
    CHECK_EQ(fusb302::rd_for_run(true), false);          // a sink downstream: refused, nothing written
    CHECK_EQ(find_from("W 02="), -1);
    CHECK_EQ(fusb302::rd_for_run_on(), false);
    fusb302::set_inline(false);
    // nothing attached yet: both lines
    g_reg[0x40] = 0x00;
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); }
    g_log.clear();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    CHECK_EQ(find_from("W 02=03"), 0);
    fusb302::rd_for_run(false);
    CHECK_EQ(find_from("W 02=00") >= 0, true);
}

// Plain boot, nothing attached, the passive scan running at 2 Hz: START presents Rd and no scan write may wipe it.
void test_run_rd_survives_the_passive_scan() {
    reset_world();
    fusb302::init();
    g_reg[0x40] = 0x00;
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::poll(); }
    g_log.clear();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    CHECK_EQ(find_from("W 02=03"), 0);
    g_line_mv = 3300;                                    // a lone charger's open Rp: scan rejects it (VCONN filter)
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::poll(); }
    for (const std::string& e : g_log)
        if (e.rfind("W 02=", 0) == 0) CHECK_EQ(int(std::stoi(e.substr(5), nullptr, 16) & 3), 3);   // PDWN on every SWITCHES0 write
    g_line_mv = 1600;                                    // Rp over our Rd: below 2.6 V, the scan finds it
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; g_reg[0x40] = 0x20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    fusb302::rd_for_run(false);
}

// The START path scans the cable (VCONN from us) before Rd goes on. A charger that stays silent gets Rd off for a
// second and on again (the C135 after its hard reset): that pause must not forget the scan, the same cable is still
// plugged in. Only a run ending (rd_for_run(false)) drops it. Hardware 2026-11-03: the result said "20 V/3 A
// (assumed, no e-marker)" although the C135's own cable is a 5 A e-marker.
void test_run_start_rd_pause_keeps_the_scanned_cable() {
    reset_world();
    g_cable.cc = 1;
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), true);
    CHECK_EQ(fusb302::rd_for_run(true), true);
    CHECK_EQ(fusb302::physical_cable().observed, true);
    CHECK_EQ(fusb302::physical_cable().max_ma, 5000);
    fusb302::rd_for_run_pause();
    CHECK_EQ(fusb302::rd_for_run_on(), false);
    CHECK_EQ(fusb302::rd_for_run(true), true);
    CHECK_EQ(fusb302::physical_cable().observed, true);
    CHECK_EQ(fusb302::physical_cable().max_ma, 5000);
    fusb302::rd_for_run(false);                              // the run is over: the scan goes with it
    CHECK_EQ(fusb302::rd_for_run(true), true);
    CHECK_EQ(fusb302::physical_cable().observed, false);
    fusb302::rd_for_run(false);
}

// The charger attaches once Rd is on (a new session, its own SOP' traffic may or may not follow): the scan made before Rd went on
// is still the cable the run is judged on.
void test_scanned_cable_survives_the_attach_after_rd() {
    reset_world();
    fusb302::init();
    g_cable.cc = 1;
    CHECK_EQ(fusb302::read_emarker(false, pd::EmarkerMode::Vconn), true);
    CHECK_EQ(fusb302::rd_for_run(true), true);
    const auto session = fusb302::session();
    g_reg[0x40] = 0x20;                                      // Rp on the measured line: the charger is there
    for (int i = 0; i < 100 && fusb302::session() == session; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(fusb302::session() != session, true);
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(fusb302::physical_cable().observed, true);
    CHECK_EQ(fusb302::physical_cable().max_ma, 5000);
    fusb302::rd_for_run(false);
}

// Lenovo C135 (hardware 2026-11-03): while START holds Rd, the source's Source_Capabilities must be answered with a
// Request for PDO 1 straight from the PD engine, a few ms after the caps and with no VBUS reading in the way. The
// source's SenderResponseTimer (~30 ms, the C135 allows ~57) ends in a Hard Reset otherwise.
bool request_on_the_wire() {
    for (std::size_t i = 0; i + 1 < g_txfifo.size(); i++)
        if (g_txfifo[i] == 0x12 && g_txfifo[i + 1] == 0x12) return g_txfifo.size() > 8;   // SOP token run, then the Request frame
    return false;
}

// The charger leaves: the sink engine forgets this attach (its release Request and retries would reach the next test).
void end_start_test() {
    fusb302::rd_for_run(false);
    g_reg[0x40] = 0x00;
    for (int i = 0; i < 80; i++) { g_ms += 20; fusb302::poll(); }
}

void send_caps_5v3a() {
    push_frame(0xE0, std::uint16_t(0x1001 | (2u << 6)), {0x0801912Cu});      // Source_Capabilities: fixed 5 V 3 A
}

// The attach happens in the main loop's poll(); the caps follow 27 ms later (trace c135_trace4). VBUS is not read at all.
void test_start_requests_pdo1_a_few_ms_after_the_caps_when_poll_attached() {
    reset_world();
    fusb302::init();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_txfifo.clear();
    send_caps_5v3a();
    int passes = 0;                                          // service() passes, 1 ms apart, until the Request is on the wire
    while (passes < 10 && !request_on_the_wire()) { g_ms += 1; fusb302::service(); ++passes; }
    CHECK_EQ(request_on_the_wire(), true);
    CHECK_EQ(passes <= 5, true);
    end_start_test();
}

// START blocks the main loop in its VBUS wait (it only calls service()): the wait loop itself has to notice the
// source's Rp, or the attach waits for the next 2 Hz scan after the wait.
void test_start_wait_loop_attaches_and_requests_without_poll() {
    reset_world();
    fusb302::init();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_reg[0x40] = 0x20;                                      // the charger's Rp shows up under our Rd
    for (int i = 0; i < 5 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::attach_scan(); fusb302::service(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_txfifo.clear();
    send_caps_5v3a();
    int passes = 0;                                          // service() passes, 1 ms apart, until the Request is on the wire
    while (passes < 10 && !request_on_the_wire()) { g_ms += 1; fusb302::service(); ++passes; }
    CHECK_EQ(request_on_the_wire(), true);
    CHECK_EQ(passes <= 5, true);
    end_start_test();
}

// Attach through poll() like the first START test and leave the chip ready for the caps.
void attach_for_start() {
    reset_world();
    fusb302::init();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_txfifo.clear();
    g_log.clear();
}

int count_log(const std::string& s, std::size_t from) {
    int n = 0;
    for (std::size_t i = from; i < g_log.size(); i++) n += g_log[i] == s;
    return n;
}

// Baseus 140W (hardware 2026-11-04, Black Box): the source's Hard Reset was latched in INTERRUPTA by the read that
// clear_irqs() makes right after the caps were pulled from the FIFO. The bit is clear-on-read, so the next service()
// pass did not see it: no PD_RESET, no engine reset, and the Request for the (stale) caps went out on a link the source
// had already torn down. The Hard Reset must survive that read.
void test_hard_reset_latched_by_the_post_packet_read_is_handled() {
    attach_for_start();
    send_caps_5v3a();
    g_ia_script = {0x00, 0x01};                              // pass-top read clean, the clear_irqs() after the packet reads I_HARDRST
    for (int i = 0; i < 6; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(find_from("W 0C=02") >= 0, true);               // PD_RESET: the Hard Reset was acted on
    CHECK_EQ(request_on_the_wire(), false);                  // no Request for the caps that the Hard Reset made stale
    end_start_test();
}

// The same bit read inside transmit()'s wait for TXSENT: the wait ends at once (no 40 x 1.9 ms of blind polling while
// the source power-cycles) and the next pass resets the PD engine.
void test_hard_reset_seen_while_waiting_for_txsent_ends_the_wait() {
    attach_for_start();
    send_caps_5v3a();
    // service(): pass-top, after-packet clear_irqs(), transmit's clear_irqs(), then the wait reads; the 3rd wait read has I_HARDRST
    g_ia_script = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    g_ms += 1; fusb302::service();
    const int tx = find_from("W 06=01");
    CHECK_EQ(tx >= 0, true);
    CHECK_EQ(count_log("R 3E", std::size_t(tx)) < 10, true); // the wait stopped at the Hard Reset, not after 40 reads
    g_ms += 1; fusb302::service();
    CHECK_EQ(find_from("W 0C=02", std::size_t(tx)) >= 0, true);
    end_start_test();
}

// The charger test's handover leaves the source with no PD contract: its QC / AFC pins stay dead while one exists (the
// Lenovo C135, hardware 2026-11-03: legacy failed at 4.9 V with the sink answering its caps again and again). After
// legacy_only(true) the K2 is PD-silent: no Request for re-announced caps, none after a Hard Reset, no GoodCRC.
void test_legacy_only_is_pd_silent() {
    reset_world();
    fusb302::init();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    g_txfifo.clear();
    send_caps_5v3a();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);                   // the run start's Request: a contract at 5 V
    push_frame(0xE0, std::uint16_t(0x0103 | (2u << 6)), {});               // Accept
    push_frame(0xE0, std::uint16_t(0x0106 | (2u << 6) | (1u << 9)), {});   // PS_RDY
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    g_reg[0x03] |= 0x04;                                     // Auto-CRC is on while the start holds Rd
    fusb302::legacy_only(true);
    // 1: caps re-announced after the detach
    g_txfifo.clear();
    push_frame(0xE0, std::uint16_t(0x1001 | (2u << 6) | (2u << 9)), {0x0801912Cu});
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), false);
    // 2: a Hard Reset, then caps again: the "ask PDO 1 after a hard reset" shortcut stays off
    g_reg[0x3E] = 0x01;                                      // I_HARDRST
    g_ms += 1; fusb302::service();
    g_reg[0x3E] = 0x00;
    g_txfifo.clear();
    push_frame(0xE0, std::uint16_t(0x1001 | (2u << 6) | (3u << 9)), {0x0801912Cu});
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), false);
    // 3: no GoodCRC: Auto-CRC drops once the run start's demand is gone
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(int(g_reg[0x03] & 0x04), 0);
    fusb302::legacy_only(false);
    end_start_test();
}

// The re-read's own attach (no CC yet: Rd presented, Request for PDO 1) must end clean: after the run, or at once on an
// abort, the sink goes back to PDO 1 and the contract is dropped, Rd and Auto-CRC return to the boot state. Hardware
// 2026-11-03: the X865 contract and Rd stayed on until a reboot, because a Ready sink keeps Auto-CRC and the sink demand.
struct AttachState { unsigned sw0; unsigned ctl3; };

AttachState reread_attach_to_ready(bool fresh_world = true) {
    if (fresh_world) reset_world();
    fusb302::init();
    fusb302::set_proactive_discover(false);   // the reread runs its own SVIDs step; keep these tests deterministic
    for (int i = 0; i < 20; i++) { g_ms += 20; fusb302::poll(); }
    const AttachState boot{g_reg[0x02], g_reg[0x03]};
    CHECK_EQ(fusb302::reread_start(1), true);                // attach: no CC, so Rd goes up
    g_reg[0x40] = 0x20;                                      // the charger's Rp answers
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_txfifo.clear();
    send_caps_5v3a();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);
    push_frame(0xE0, std::uint16_t(0x0103 | (2u << 6)), {});               // Accept
    push_frame(0xE0, std::uint16_t(0x0106 | (2u << 6) | (1u << 9)), {});   // PS_RDY
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    return boot;
}

void check_back_to_boot(const AttachState& boot) {
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));   // no contract held
    CHECK_EQ(int(g_reg[0x03] & 0x04), 0);                    // Auto-CRC off again
    CHECK_EQ(int(g_reg[0x02] & 0x03), int(boot.sw0 & 0x03)); // Rd as at boot
    CHECK_EQ(int(g_reg[0x03] & ~0x03), int(boot.ctl3 & ~0x03));   // SPECREV / Auto-CRC / retries as at boot (the TX line select stays)
    CHECK_EQ(usb_has("[REREAD] attach released"), true);
}

void test_reread_abort_drops_the_contract_and_rd() {
    const AttachState boot = reread_attach_to_ready();
    fusb302::reread_abort();
    for (int i = 0; i < 200; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    check_back_to_boot(boot);
    end_start_test();
}

void test_reread_end_drops_the_contract_and_rd_after_the_linger() {
    const AttachState boot = reread_attach_to_ready();
    for (int i = 0; i < 400 && fusb302::reread_status().state == pd::RereadState::Running; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(int(fusb302::reread_status().state) != int(pd::RereadState::Running), true);
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }      // inside the linger: the contract stays readable
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    for (int i = 0; i < 2000; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }    // 40 s: past the 30 s linger
    check_back_to_boot(boot);
    end_start_test();
}

// A user trigger that took the sink over during the linger keeps its contract: the re-read never releases it.
void test_reread_release_leaves_a_held_contract_alone() {
    (void)reread_attach_to_ready();
    fusb302::reread_abort();
    CHECK_EQ(fusb302::rd_for_run(true), true);               // a START takes the Rd over
    for (int i = 0; i < 200; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(usb_has("[REREAD] attach released"), false);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    end_start_test();
}

// UGREEN X865 (hardware 2026-11-04, Black Box): its GoodCRC for our Get_Source_Cap_Extended came 118 ms after the TX_START,
// just past transmit()'s loop (40 iterations of 1 ms delay plus the ~1.9 ms bit-banged read = 117 ms), and its
// Source_Capabilities_Extended 3 ms after that. A TX timeout is ambiguous (the message may be delivered): the step keeps
// waiting for its answer. The fake chip never reports TXSENT, so every TX here ends in that timeout.
void test_reread_step_survives_a_tx_timeout_when_the_answer_arrives() {
    (void)reread_attach_to_ready();
    g_txfifo.clear();
    for (int i = 0; i < 600 && !request_on_the_wire(); i++) { g_ms += 5; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);                   // Get_Source_Cap_Extended is out (and its TX timed out)
    CHECK_EQ(int(fusb302::reread_status().skipped_mask & pd::kRereadCapsExt), 0);
    push_frame(0xE0, std::uint16_t(0x8000 | (7u << 12) | (2u << 6) | 1u), {0x00000019u, 0, 0, 0, 0, 0, 0});   // ext header: 25 B, chunk 0
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::reread_status().ok_mask & pd::kRereadCapsExt), int(pd::kRereadCapsExt));
    CHECK_EQ(int(fusb302::reread_status().skipped_mask & pd::kRereadCapsExt), 0);
    fusb302::reread_abort();
    for (int i = 0; i < 200; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    end_start_test();
}

// Ra mode (hardware 2026-11-06: PC13 high is an Ra on CC1): PC13 goes high before Rd appears, Rd sits on CC2 alone, PC13 is never
// driven low while the mode is on, and the mode ends with the run (or ra_set(false)) with PC13 low and Rd back to the idle rule.
void test_ra_mode_holds_pc13_high_with_rd_on_cc2_only() {
    reset_world();
    fusb302::init();
    g_reg[0x40] = 0x00;
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); }
    g_log.clear();
    CHECK_EQ(fusb302::ra_set(true), true);
    CHECK_EQ(fusb302::ra_active(), true);
    CHECK_EQ(g_pin[0], true);                                // PC13 high: the Ra on CC1
    CHECK_EQ(find_from("PC13=1") >= 0, true);
    CHECK_EQ(fusb302::rd_for_run(true), true);
    const int pc13 = find_from("PC13=1");
    const int rd = find_from("W 02=02");                     // PDWN2: Rd on CC2 alone
    CHECK_EQ(rd > pc13, true);                               // the Ra is there before Rd is
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(find_from("PC13=0"), -1);                       // never low while on: no attach, scan, detach or poll path drops it
    for (std::size_t i = std::size_t(rd); i < g_log.size(); i++) {   // from the run's Rd on
        const std::string& e = g_log[i];
        if (e.rfind("W 02=", 0) != 0) continue;
        const int v = std::stoi(e.substr(5), nullptr, 16);
        CHECK_EQ(v & 3, 2);                                 // PDWN2 only, never PDWN1
        CHECK_EQ(v & 0x30, 0);                               // the VCONN switches stay open
    }
    CHECK_EQ(g_pin[0], true);
    fusb302::rd_for_run(false);                              // the run owns the mode: it ends with it
    CHECK_EQ(fusb302::ra_active(), false);
    CHECK_EQ(g_pin[0], false);
    CHECK_EQ(find_from("PC13=0") >= 0, true);
}

void test_ra_mode_refusals_and_explicit_off() {
    reset_world();
    fusb302::init();
    g_reg[0x40] = 0x00;
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); }
    fusb302::set_inline(true);
    g_log.clear();
    CHECK_EQ(fusb302::ra_set(true), false);                  // a sink may be downstream: nothing driven
    CHECK_EQ(fusb302::ra_active(), false);
    CHECK_EQ(g_pin[0], false);
    CHECK_EQ(find_from("PC13=1"), -1);
    fusb302::set_inline(false);
    CHECK_EQ(fusb302::ra_set(true), true);
    CHECK_EQ(fusb302::ra_set(true), true);                   // idempotent
    CHECK_EQ(fusb302::ra_set(false), true);
    CHECK_EQ(fusb302::ra_active(), false);
    CHECK_EQ(g_pin[0], false);
    CHECK_EQ(fusb302::ra_set(false), true);                  // off again is a no-op
}

// A charger-test unit detach is Rd off, whatever keeps Auto-CRC demanded: the armed e-marker spoof holds Auto-CRC (and with it the
// active-operation Rd) for as long as it is armed, so the unit's detach left Rd on and the charger never saw it (hardware 2026-11-06,
// charger test with the spoof armed: every unit "no PD after re-attach", VBUS 5.12 V throughout).
void test_unit_detach_drops_rd_with_the_spoof_armed() {
    reset_world();
    fusb302::init();
    fusb302::set_sop_prime_hook(+[](std::uint8_t, std::uint16_t, std::span<const std::uint8_t>) {});
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_txfifo.clear();
    send_caps_5v3a();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);
    for (int i = 0; i < 400; i++) { g_ms += 1; fusb302::service(); }   // the contract settles, Auto-CRC is on
    g_log.clear();
    fusb302::unit_detach();
    for (int i = 0; i < 400; i++) { g_ms += 5; fusb302::service(); }   // the passive linger is long over
    int last = -1;
    for (std::size_t i = 0; i < g_log.size(); i++)
        if (g_log[i].rfind("W 02=", 0) == 0) last = std::stoi(g_log[i].substr(5), nullptr, 16);
    CHECK_EQ(last >= 0, true);
    CHECK_EQ(last & 3, 0);                                   // no Rd on either line
    fusb302::set_sop_prime_hook(nullptr);
    end_start_test();
}

// A Legacy unit under a selected cable identity must behave exactly like Cable OFF: the page (charger_test Env::identity(false))
// drops the spoof and the Ra mode at the unit's attach, right before unit_attach(legacy). The spoof's Auto-CRC demand used to end
// only there, and the 300 ms Auto-CRC linger then ran on into the attach: the Baseus 70 W ACKed its first caps, kept PD and
// ignored every D+/D- handshake (hardware 2026-11-07, runs 3 and 4). With the spoof gone at the detach, Auto-CRC is already off
// when the unit attaches. Cable OFF is the reference: both cases must show the same GoodCRC state at the first caps.
void legacy_attach_after_a_run(bool spoofed) {
    reset_world();
    fusb302::init();
    const auto hook = +[](std::uint8_t, std::uint16_t, std::span<const std::uint8_t>) {};
    if (spoofed) { CHECK_EQ(fusb302::ra_set(true), true); fusb302::set_sop_prime_hook(hook); }
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_txfifo.clear();
    send_caps_5v3a();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);
    push_frame(0xE0, std::uint16_t(0x0103 | (2u << 6)), {});               // Accept
    push_frame(0xE0, std::uint16_t(0x0106 | (2u << 6) | (1u << 9)), {});   // PS_RDY
    for (int i = 0; i < 400; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    // The unit boundary, as the runner drives the page: detach (the identity still armed), a dark wait, then the attach
    // with the identity dropped first.
    fusb302::unit_detach();
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }   // 2 s dark
    if (spoofed) { fusb302::ra_set(false); fusb302::set_force_line(0); fusb302::set_sop_prime_hook(nullptr); }
    fusb302::unit_attach(true);
    for (int i = 0; i < 40; i++) {   // the first 400 ms of the Legacy attach: the charger's first caps come within it
        g_ms += 10; fusb302::poll(); fusb302::service();
        CHECK_EQ(int(g_reg[0x03] & 0x04), 0);                // Auto-CRC off: the charger's caps would be GoodCRC'd otherwise
    }
    g_txfifo.clear();
    push_frame(0xE0, std::uint16_t(0x1001 | (2u << 6)), {0x0801912Cu});    // the charger's caps
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), false);
    CHECK_EQ(int(g_reg[0x03] & 0x04), 0);
    fusb302::legacy_only(false);
    end_start_test();
}
void test_legacy_unit_attach_is_pd_silent_with_cable_off() { legacy_attach_after_a_run(false); }
void test_legacy_unit_attach_is_pd_silent_under_a_spoofed_run() { legacy_attach_after_a_run(true); }

// ---- cable_session / cable_exchange (`emarker learn`) ----

struct SessionProbe {
    int calls = 0;
    bool pc13 = false;
    bool ra = false, rb = false, rc = false, rd = false;
    emk_learn::Reply a, b, c, d;
};

void session_body(void* p) {
    auto& s = *static_cast<SessionProbe*>(p);
    s.calls++;
    s.pc13 = g_pin[0];
    emk_learn::Request r;
    r.spec_rev = 2;
    r.n = 1;
    r.w[0] = 0x22D9A01Fu;                                   // the OPPO vendor command
    s.ra = fusb302::cable_exchange(r, s.a, 50);
    r.w[0] = 0x22D9A011u;                                   // a command the cable only GoodCRCs
    s.rb = fusb302::cable_exchange(r, s.b, 50);
    g_cable.vendor_drop = true;                             // RETRYFAIL: no GoodCRC
    s.rc = fusb302::cable_exchange(r, s.c, 50);
    g_cable.vendor_drop = false;
    s.rd = fusb302::cable_exchange(r, s.d, 50);
}

void test_cable_session_exchanges_run_with_vconn_up_and_restore_after() {
    reset_world();
    g_cable.cc = 1;
    g_cable.vendor = true;
    SessionProbe s;
    CHECK_EQ(fusb302::cable_session(false, &session_body, &s), true);
    CHECK_EQ(s.calls, 1);
    CHECK_EQ(s.pc13, true);                                  // VCONN still on while the body runs
    CHECK_EQ(g_cable.vendor_vconn_ms >= 25, true);
    // The vendor request is acknowledged and answered: the ACK's header and two VDOs.
    CHECK_EQ(s.ra, true);
    CHECK_EQ(s.a.got, true);
    CHECK_EQ(s.a.n, 3);
    CHECK_EQ(s.a.header & 0x1F, 0x0F);
    CHECK_EQ(s.a.w[0], 0x22D9A05Fu);
    CHECK_EQ(s.a.w[1], 0xAB01CD02u);
    CHECK_EQ(s.a.w[2], 7u);
    // A request the cable does not answer: acknowledged, nothing after the GoodCRC.
    CHECK_EQ(s.rb, true);
    CHECK_EQ(s.b.got, false);
    // No GoodCRC: false, and the MessageID is reused by the next request (RETRYFAIL does not advance it).
    CHECK_EQ(s.rc, false);
    CHECK_EQ(s.c.got, false);
    CHECK_EQ(s.rd, true);
    CHECK_EQ(g_cable.vendor_ids.size(), 4u);
    CHECK_EQ(g_cable.vendor_ids[0], 1u);                     // the scan's Discover Identity was id 0
    CHECK_EQ(g_cable.vendor_ids[1], 2u);
    CHECK_EQ(g_cable.vendor_ids[2], 3u);
    CHECK_EQ(g_cable.vendor_ids[3], 3u);
    // The scan state is back: VCONN off, Rd on, Auto-CRC as before.
    CHECK_EQ(g_pin[0], false);
    const int pc13_off = find_from("PC13=0");
    CHECK_EQ(pc13_off > 0, true);
    CHECK_EQ(find_from("W 02=03", pc13_off) > 0, true);
    CHECK_EQ(find_from("W 03=20", pc13_off) > 0, true);
    CHECK_EQ(usb_has("E-Marker detected"), true);
    // The session is over: an exchange now is refused.
    emk_learn::Reply rep;
    emk_learn::Request r;
    r.n = 1;
    CHECK_EQ(fusb302::cable_exchange(r, rep, 50), false);
}

void test_cable_session_without_a_cable_never_calls_the_body() {
    reset_world();
    g_cable.answers_pd3 = false;
    g_cable.answers_pd2 = false;
    SessionProbe s;
    CHECK_EQ(fusb302::cable_session(false, &session_body, &s), false);
    CHECK_EQ(s.calls, 0);
    CHECK_EQ(g_pin[0], false);                               // restored like a failed scan
    emk_learn::Reply rep;
    emk_learn::Request r;
    r.n = 1;
    CHECK_EQ(fusb302::cable_exchange(r, rep, 50), false);    // no session open
    CHECK_EQ(rep.got, false);
}

void test_cable_session_is_refused_like_the_scan() {
    reset_world();
    SessionProbe s;
    g_cable.vendor = true;
    // Inline with a live sink and no confirmation: refused before any wire activity, body not called.
    fusb302::set_inline(true);
    g_log.clear();
    CHECK_EQ(fusb302::cable_session(false, &session_body, &s), false);
    CHECK_EQ(s.calls, 0);
    CHECK_EQ(g_cable.tx_seen, 0);
    CHECK_EQ(find_from("PC13=1"), -1);
    CHECK_EQ(usb_has("Refused"), true);
    fusb302::set_inline(false);
}

// ---- Rd lifetime (k2/rd-release) ----
// Hardware 2026-11-06: after pd-reread (finished or aborted), `pd release`, `pd attach` + release, `--line 2`, `--ra on` or a spoof-armed
// session the K2 sometimes kept Rd on CC2 (SWITCHES0 0x0A, VBUS 5.2 V) until a reboot: the charger never saw a detach.
// Contract (every case below ends in it): once no owner is left, Rd is off within kSinkIdleLingerMs + the Auto-CRC linger.
// Owners: the boot-mode pin, a run's Rd (rd_for_run / unit_attach / the re-read's own attach), an operation in flight (sink
// negotiating or held, Identify, probe, re-read, prepare, Get_Source_Cap, VCONN swap) and a released contract for the linger only.
// The armed e-marker spoof needs Auto-CRC (SOP' GoodCRC), never Rd.
unsigned sw0_rd() { return g_reg[0x02] & 3u; }
bool auto_crc() { return (g_reg[0x03] & 0x04) != 0; }
void settle_ms(unsigned ms) { for (unsigned t = 0; t < ms; t += 20) { g_ms += 20; fusb302::poll(); fusb302::service(); } }
std::uint16_t ctl_msg(unsigned type, unsigned id) { return std::uint16_t(0x0100 | type | (2u << 6) | (id << 9)); }
void send_caps_5v_9v(unsigned id) {
    push_frame(0xE0, std::uint16_t(0x2001 | (2u << 6) | (id << 9)), {0x0801912Cu, 0x0002D12Cu});   // fixed 5 V 3 A, fixed 9 V 3 A
}
void reread_style_boot() {
    reset_world();
    fusb302::init();
    fusb302::set_proactive_discover(false);
    g_auto_txsent = true;
    for (int i = 0; i < 20; i++) { g_ms += 20; fusb302::poll(); }
}
// A charger's Rp on the wire, no owner of Rd: the passive scan attaches on its own.
void passive_attach() {
    reread_style_boot();
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
}
// The START's own attach: Rd, caps (5 V / 9 V), the automatic Request for PDO 1, Accept, PS_RDY: Ready at 5 V.
void start_attach_to_ready() {
    reread_style_boot();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    for (int i = 0; i < 5; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }   // Auto-CRC is on before the caps
    g_txfifo.clear();
    send_caps_5v_9v(0);
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);
    push_frame(0xE0, ctl_msg(3, 1), {});     // Accept
    push_frame(0xE0, ctl_msg(6, 2), {});     // PS_RDY
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
}
// `pd attach 9 V` (held).
void trigger_9v() {
    fusb302::sink_set_cap(20000);
    settle_ms(6000);                         // the e-marker Identify the attach started ends (nobody answers)
    CHECK_EQ(int(fusb302::sink_request({2, 9000, 3000}, true)), int(pd::Err::Ok));
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    push_frame(0xE0, ctl_msg(3, 3), {});
    push_frame(0xE0, ctl_msg(6, 4), {});
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(int(fusb302::sink_status().pos), 2);
    CHECK_EQ(fusb302::sink_status().held, true);
}
// `pd release`: back to 5 V, not held; the source answers the Request for PDO 1.
void release_to_5v() {
    fusb302::sink_release();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    push_frame(0xE0, ctl_msg(3, 5), {});
    push_frame(0xE0, ctl_msg(6, 6), {});
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(int(fusb302::sink_status().pos), 1);
    CHECK_EQ(fusb302::sink_status().held, false);
}
void check_rd_gone(bool spoof_armed) {
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    CHECK_EQ(auto_crc(), spoof_armed);       // the spoof keeps Auto-CRC on, nothing else may
}
void clean_up_rd_tests() {
    fusb302::set_sop_prime_hook(nullptr);
    fusb302::set_force_line(0);
    fusb302::ra_set(false);
    fusb302::reread_abort();
    end_start_test();
    g_auto_txsent = false;
}
void spoof_hook_on() { fusb302::set_sop_prime_hook(+[](std::uint8_t, std::uint16_t, std::span<const std::uint8_t>) {}); }

// The armed spoof with nobody sinking: Auto-CRC on (SOP' needs the GoodCRC), Rd never. Before: Auto-CRC demand was Rd demand.
void test_armed_spoof_alone_never_presents_rd() {
    passive_attach();
    spoof_hook_on();
    settle_ms(2000);
    CHECK_EQ(auto_crc(), true);
    CHECK_EQ(sw0_rd(), 0u);
    fusb302::set_sop_prime_hook(nullptr);
    settle_ms(1000);
    CHECK_EQ(auto_crc(), false);
    CHECK_EQ(sw0_rd(), 0u);
    clean_up_rd_tests();
}

// `pd release` after `pd attach`: the contract is back at 5 V and unheld; it stays readable for the linger, then Rd goes.
void test_released_contract_lingers_then_drops_rd() {
    start_attach_to_ready();
    fusb302::rd_for_run(false);              // the run is over; the contract at 5 V is all that is left
    trigger_9v();
    settle_ms(2000);
    CHECK_EQ(sw0_rd() != 0, true);           // a held trigger contract owns Rd
    release_to_5v();
    settle_ms(20000);                        // inside the linger: still readable
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(sw0_rd() != 0, true);
    settle_ms(15000 + 2000);                 // linger + Auto-CRC linger + scan slack
    check_rd_gone(false);
    CHECK_EQ(usb_has("[PD] Contract released: Rd off"), true);
    clean_up_rd_tests();
}

void test_released_contract_with_the_spoof_armed_drops_rd_keeps_crc() {
    spoof_hook_on();
    start_attach_to_ready();
    fusb302::rd_for_run(false);
    trigger_9v();
    release_to_5v();
    settle_ms(36000);
    check_rd_gone(true);
    clean_up_rd_tests();
}

// A held trigger contract is the user's: it never lingers out, however long it runs.
void test_held_contract_keeps_rd() {
    start_attach_to_ready();
    fusb302::rd_for_run(false);
    trigger_9v();
    settle_ms(90000);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(sw0_rd() != 0, true);
    clean_up_rd_tests();
}

// The re-read's own attach on a forced line, up to a contract at 5 V; `spoof`: the armed e-marker spoof as well.
void reread_attach_forced_to_ready() {
    reread_style_boot();
    fusb302::set_force_line(2);
    CHECK_EQ(fusb302::reread_start(1), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    for (int i = 0; i < 5; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    g_txfifo.clear();
    send_caps_5v3a();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);
    push_frame(0xE0, ctl_msg(3, 1), {});
    push_frame(0xE0, ctl_msg(6, 2), {});
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
}

// pd-reread --line 2 then --abort: the forced line goes, the contract and the Rd the attach presented go with it.
void test_reread_abort_after_a_forced_line_releases_rd() {
    reread_attach_forced_to_ready();
    fusb302::reread_abort();
    settle_ms(6000);
    check_rd_gone(false);
    clean_up_rd_tests();
}

// The same with the spoof armed (the Ra experiments): Rd still goes, Auto-CRC stays for the spoof.
void test_reread_abort_with_the_spoof_armed_releases_rd() {
    spoof_hook_on();
    reread_attach_forced_to_ready();
    fusb302::reread_abort();
    settle_ms(6000);
    check_rd_gone(true);
    clean_up_rd_tests();
}

// ---- A trigger after a re-read (k2/rd-held-fix) ----
// Hardware 2026-11-06 (build d0ad00b): `pd-reread --wait 20` leaves a 5 V contract the re-read's attach made, then `pd attach 5000
// 3000` (held). At +39 s Rd went and VBUS fell: the re-read's own "release 30 s after the run" still fired on the held contract.
// Contract: a held trigger contract keeps Rd and the contract until `pd release`, however long since the re-read ended; the
// re-read's attach housekeeping starts its linger over after the release.
// The re-read's attach on a source offering 5 V and 9 V, to Ready at 5 V, then the run to its end (the steps get no answers).
void reread_attach_then_finished() {
    reread_style_boot();
    CHECK_EQ(fusb302::reread_start(1), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    for (int i = 0; i < 5; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    g_txfifo.clear();
    send_caps_5v_9v(0);
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);
    push_frame(0xE0, ctl_msg(3, 1), {});
    push_frame(0xE0, ctl_msg(6, 2), {});
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    for (int i = 0; i < 400 && fusb302::reread_status().state == pd::RereadState::Running; i++) settle_ms(20);
    CHECK_EQ(int(fusb302::reread_status().state) != int(pd::RereadState::Running), true);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));   // inside the re-read linger: the contract is still there
}
// `pd attach` of PDO `pos` after the re-read, answered by the source (Accept, PS_RDY); held.
void attach_after_reread(std::uint8_t pos, std::uint32_t mv) {
    fusb302::sink_set_cap(20000);
    settle_ms(3000);
    CHECK_EQ(int(fusb302::sink_request({pos, mv, 3000}, true)), int(pd::Err::Ok));
    CHECK_EQ(fusb302::sink_status().held, true);
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    push_frame(0xE0, ctl_msg(3, 3), {});
    push_frame(0xE0, ctl_msg(6, 4), {});
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(int(fusb302::sink_status().pos), int(pos));
    CHECK_EQ(fusb302::sink_status().held, true);
}
void check_held_after_reread(std::uint8_t pos) {
    settle_ms(70000);                        // far past the re-read's 30 s and the 30 s contract linger
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(int(fusb302::sink_status().pos), int(pos));
    CHECK_EQ(fusb302::sink_status().held, true);
    CHECK_EQ(sw0_rd() != 0, true);
    CHECK_EQ(usb_has("[REREAD] attach released"), false);
    CHECK_EQ(usb_has("[PD] Contract released"), false);
}
void check_released_then_drops(std::uint32_t after_ms) {
    fusb302::sink_release();
    if (fusb302::sink_status().pos != 1 || fusb302::sink_status().state != pd::SinkState::Ready) {
        for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
        push_frame(0xE0, ctl_msg(3, 5), {});
        push_frame(0xE0, ctl_msg(6, 6), {});
        for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    }
    CHECK_EQ(int(fusb302::sink_status().pos), 1);
    CHECK_EQ(fusb302::sink_status().held, false);
    settle_ms(10000);
    CHECK_EQ(sw0_rd() != 0, true);           // the 5 V contract stays readable for a while after `pd release`
    settle_ms(after_ms);
    check_rd_gone(false);
}
// pd-reread, then `pd attach 5000 3000` on the PDO the re-read's contract already holds.
void test_trigger_on_the_rereads_own_pdo_stays_held() {
    reread_attach_then_finished();
    attach_after_reread(1, 5000);
    check_held_after_reread(1);
    check_released_then_drops(36000);
    clean_up_rd_tests();
}
// pd-reread, then `pd attach 9000 3000`: another PDO (passes on hardware; kept).
void test_trigger_on_another_pdo_after_a_reread_stays_held() {
    reread_attach_then_finished();
    attach_after_reread(2, 9000);
    check_held_after_reread(2);
    check_released_then_drops(36000);
    clean_up_rd_tests();
}

// pd-reread --ra on ... then `--ra off --line 0` and --abort: PC13 low, Rd off, contract gone.
void test_ra_off_then_abort_releases_rd() {
    spoof_hook_on();
    reread_style_boot();
    CHECK_EQ(fusb302::ra_set(true), true);
    CHECK_EQ(fusb302::reread_start(1), true);
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(sw0_rd(), 2u);                  // Rd on CC2 alone beside the Ra
    for (int i = 0; i < 5; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    g_txfifo.clear();
    send_caps_5v3a();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    push_frame(0xE0, ctl_msg(3, 1), {});
    push_frame(0xE0, ctl_msg(6, 2), {});
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    fusb302::ra_set(false);
    fusb302::set_force_line(0);
    fusb302::reread_abort();
    settle_ms(6000);
    check_rd_gone(true);
    CHECK_EQ(g_pin[0], false);
    clean_up_rd_tests();
}

// debug detach: a CC-level detach. Every owner is dropped and Rd stays off for the hold (longer than tPDDebounce), never
// a display-only override.
void test_cc_detach_drops_rd_for_longer_than_the_debounce() {
    start_attach_to_ready();
    CHECK_EQ(sw0_rd() != 0, true);
    g_log.clear();
    const std::uint32_t t0 = g_ms;
    CHECK_EQ(fusb302::cc_detach(), true);
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    while (g_ms - t0 < 1400) { g_ms += 20; fusb302::poll(); fusb302::service(); }   // > 100..200 ms debounce, inside the hold
    for (const std::string& e : g_log)
        if (e.rfind("W 02=", 0) == 0) CHECK_EQ(std::stoi(e.substr(5), nullptr, 16) & 3, 0);
    settle_ms(3000);
    CHECK_EQ(sw0_rd(), 0u);                  // nothing owns Rd afterwards either
    clean_up_rd_tests();
}

void test_cc_detach_refused_inline() {
    passive_attach();
    fusb302::set_inline(true);
    CHECK_EQ(fusb302::cc_detach(), false);
    fusb302::set_inline(false);
    clean_up_rd_tests();
}

// ---- HID-powered vs charger-powered boot (k2/self-power-hid-gate) ----
// Hardware 2026-11-06 (K2 on HID power, charger brick on the top plug): the self-power merge left Rd on both lines after a
// flash, kept Rd on CC2 through `pd-reread --abort`, and inherited Rd across `k2_remote reboot`. A K2 powered over HID has
// no pin and no pin mark: its boot is passive on every path (cold start, flash, reboot), and the chip's own SWITCHES0 (power-on
// 0x03, a reread's or a run's Rd) is never a reason to keep Rd. Only a mark written by a pinned boot (key mode / charger-powered
// resume) survives a warm reset.
bool wrote_pdwn_to_sw0() {
    for (const std::string& s : g_log)
        if (s.rfind("W 02=", 0) == 0 && (std::stoul(s.substr(5), nullptr, 16) & 3u) != 0) return true;
    return false;
}
int first_log(const char* prefix) {
    for (std::size_t i = 0; i < g_log.size(); i++)
        if (g_log[i].rfind(prefix, 0) == 0) return int(i);
    return -1;
}
void power_cycle_forget_pin() {   // leaves no pin, no mark behind (the tests share the driver's statics)
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::Plain, false);
}

// (a) HID-powered cold boot: SWITCHES0 never gets a PDWN bit before the main loop, and the passive write comes before the CC
// switches close, so the chip's power-on Rd on both lines never reaches the connector.
void test_hid_cold_boot_is_passive_before_the_main_loop() {
    reset_world();                       // chip at its power-on default: SWITCHES0 0x03
    g_mark = bootmode::pin_mark(0x02);   // a stale mark (battery-backed registers) means nothing after a power-on reset
    fusb302::self_power_early(bootmode::Mode::Plain, false);
    CHECK_EQ(int(g_reg[0x02] & 3), 0);
    CHECK_EQ(wrote_pdwn_to_sw0(), false);
    CHECK_EQ(fusb302::self_powered(), false);
    CHECK_EQ(fusb302::rd_inherited(), false);
    CHECK_EQ(int(g_mark), 0);
    const int w = first_log("W 02=00"), c1 = first_log("PC14=1"), c2 = first_log("PC15=1");
    CHECK_EQ(w >= 0, true);
    CHECK_EQ(c1 < 0 || w < c1, true);
    CHECK_EQ(c2 < 0 || w < c2, true);
    fusb302::init();                     // the main loop's quiet start keeps it passive
    for (int i = 0; i < 50; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(wrote_pdwn_to_sw0(), false);
    CHECK_EQ(int(g_reg[0x02] & 3), 0);
}

// (b) HID-powered warm reset (a flash, `k2_remote reboot`, a watchdog): a detach, whatever the chip still says.
void test_hid_warm_reset_inherits_no_rd() {
    for (unsigned leftover : {0x03u, 0x02u, 0x01u}) {
        reset_world();
        g_reg[0x02] = std::uint8_t(leftover);   // the power-on default after a chip reset, a reread's or a run's Rd
        g_mark = 0;                             // never pinned: HID power
        fusb302::self_power_early(bootmode::Mode::Plain, true);
        CHECK_EQ(int(g_reg[0x02] & 3), 0);
        CHECK_EQ(wrote_pdwn_to_sw0(), false);
        CHECK_EQ(fusb302::self_powered(), false);
        CHECK_EQ(fusb302::rd_inherited(), false);
        CHECK_EQ(int(fusb302::pin_rd()), 0);
    }
}

// (c) pd-reread on a HID-powered boot: the abort gives Rd back (it used to stay on CC2 and hold the brick at 5 V), and a reboot
// in the middle of it is a detach too. The boot state itself must be Rd off: check_back_to_boot only compares with it.
void test_hid_reread_abort_releases_rd() {
    reset_world();
    g_reg[0x02] = 0x02;                  // Rd on CC2 left over from before the reset
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::Plain, true);
    const AttachState boot = reread_attach_to_ready(false);
    CHECK_EQ(int(boot.sw0 & 3), 0);
    CHECK_EQ(sw0_rd() != 0, true);       // the reread's own attach holds Rd now
    fusb302::reread_abort();
    settle_ms(4000);
    check_back_to_boot(boot);
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(int(fusb302::pin_rd()), 0);
    end_start_test();
}

void test_hid_reboot_during_a_reread_is_a_detach() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::Plain, false);
    (void)reread_attach_to_ready(false);
    CHECK_EQ(sw0_rd() != 0, true);       // the reread presents Rd; VBUS from the brick hangs on it
    g_log.clear();
    fusb302::self_power_early(bootmode::Mode::Plain, true);   // `k2_remote reboot`: warm reset, chip keeps SWITCHES0
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(wrote_pdwn_to_sw0(), false);
    CHECK_EQ(fusb302::rd_inherited(), false);
    fusb302::reread_abort();             // the simulated reboot kept the driver's statics: end the attach the way the others do
    settle_ms(4000);
    end_start_test();
    power_cycle_forget_pin();
}

// The charger-powered side stays as designed: a key mode pins its line and marks it, a plain warm reset keeps the marked pin
// (even when the chip itself says nothing), releasing it clears the mark, and a run's detach (leave / restore) keeps the mark.
void test_charger_powered_pin_is_marked_and_survives_a_warm_reset() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);
    CHECK_EQ(int(fusb302::pin_rd()), 2);
    CHECK_EQ(int(g_reg[0x02] & 3), 2);
    CHECK_EQ(int(g_mark), int(bootmode::pin_mark(0x02)));
    CHECK_EQ(fusb302::rd_inherited(), false);
    fusb302::self_power_leave();         // a run's detach drops the pin ...
    CHECK_EQ(int(g_mark), int(bootmode::pin_mark(0x02)));   // ... but the K2 is still charger-powered: the mark stays
    fusb302::self_power_restore();
    CHECK_EQ(int(fusb302::pin_rd()), 2);
    // power glitch / watchdog: warm reset without a key, the chip lost its register (default after its own reset)
    g_reg[0x02] = 0x00;
    g_log.clear();
    fusb302::self_power_early(bootmode::Mode::Plain, true);
    CHECK_EQ(int(fusb302::pin_rd()), 2);
    CHECK_EQ(fusb302::rd_inherited(), true);
    CHECK_EQ(int(g_reg[0x02] & 3), 2);
    CHECK_EQ(first_log("W 02=02") >= 0, true);
    CHECK_EQ(int(g_mark), int(bootmode::pin_mark(0x02)));
    fusb302::self_power_release();       // no journal to resume: Rd goes, the mark with it
    CHECK_EQ(int(g_reg[0x02] & 3), 0);
    CHECK_EQ(int(g_mark), 0);
    CHECK_EQ(fusb302::self_powered(), false);
    fusb302::self_power_early(bootmode::Mode::Plain, true);   // the next warm reset is passive
    CHECK_EQ(fusb302::self_powered(), false);
    fusb302::self_power_early(bootmode::Mode::OkRdCc1, true);
    CHECK_EQ(int(g_mark), int(bootmode::pin_mark(0x01)));
    power_cycle_forget_pin();
    CHECK_EQ(int(g_mark), 0);
}

// ---- No Rd on both lines in a key mode (k2/plus-boot-rd) ----
// Hardware 2026-11-06 (OnePlus 120W brick on the top plug, K2 on HID power, PLUS held at boot): 300 ms after reset the widen put
// Rd on both lines. Rd/Rd is the Type-C Debug Accessory signature: the brick turned its Rp off on both pins and stayed off until
// the plug was pulled. A key mode presents ONE Rd (mode 3 = CC2), and moves it at most once.
bool wrote_rd_pair_to_sw0() {
    for (const std::string& s : g_log)
        if (s.rfind("W 02=", 0) == 0 && (std::stoul(s.substr(5), nullptr, 16) & 3u) == 3u) return true;
    return false;
}

void test_key_mode_never_presents_rd_on_both_lines() {
    for (bootmode::Mode m : {bootmode::Mode::PlusRdCc2, bootmode::Mode::OkRdCc1}) {
        const unsigned pin = bootmode::sw0(m);
        reset_world();
        g_mark = 0;
        fusb302::self_power_early(m, false);
        g_log.clear();
        fusb302::init();
        // On a pinned boot the boot reset is PD_RESET only (SW_RES would restore the power-on Rd/Rd for ~0.13 ms),
        // so SWITCHES0 never leaves the pin; the write right after the reset restates it anyway.
        const int r = first_log("W 0C=");
        CHECK_EQ(r >= 0 && r + 1 < int(g_log.size()), true);
        if (r >= 0 && r + 1 < int(g_log.size())) CHECK_EQ(g_log[std::size_t(r + 1)] == "W 02=" + hex(pin), true);
        settle_ms(1200);
        CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
        CHECK_EQ(auto_crc(), false);          // key-mode idle is PD-silent: nothing ACKs, nothing transmits
        CHECK_EQ(g_txfifo.empty(), true);
        // a run start with no CC known yet used to add both lines (start_rd_lines), pin included
        CHECK_EQ(fusb302::rd_for_run(true), true);
        CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
        fusb302::rd_for_run(false);
        // the charger test's unit cycle: detach drops the pin, attach brings it back before presenting Rd
        fusb302::unit_detach();
        CHECK_EQ(sw0_rd(), 0u);
        fusb302::unit_attach(false);
        CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
        settle_ms(200);
        CHECK_EQ(wrote_rd_pair_to_sw0(), false);
        CHECK_EQ(int(fusb302::pin_rd()), int(pin));
        fusb302::unit_detach();
        fusb302::rd_for_run(false);
        end_start_test();
        power_cycle_forget_pin();
    }
}

// A mark with both bits (an older build's widened pin) is no mark: the K2 boots passive instead of re-presenting Rd/Rd.
void test_a_stale_pair_mark_boots_passive() {
    reset_world();
    g_mark = std::uint16_t(bootmode::kPinMarkMagic | 0x03);
    fusb302::self_power_early(bootmode::Mode::Plain, true);
    CHECK_EQ(int(fusb302::pin_rd()), 0);
    CHECK_EQ(fusb302::rd_inherited(), false);
    CHECK_EQ(wrote_pdwn_to_sw0(), false);
    power_cycle_forget_pin();
}

// The single Rd moves to the other line at most once per boot; the mark and the run-detach restore follow it.
void test_self_power_move_moves_the_single_rd_once() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::Plain, false);
    CHECK_EQ(fusb302::self_power_move(1), false);            // no pin, nothing to move
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);
    fusb302::init();
    CHECK_EQ(fusb302::self_power_move(2), false);            // already there
    CHECK_EQ(fusb302::self_power_move(3), false);            // never both
    CHECK_EQ(fusb302::self_power_move(0), false);
    CHECK_EQ(int(fusb302::pin_rd()), 2);
    g_log.clear();
    CHECK_EQ(fusb302::self_power_move(1), true);
    CHECK_EQ(int(fusb302::pin_rd()), 1);
    CHECK_EQ(first_log("W 02=01") >= 0, true);
    CHECK_EQ(int(g_reg[0x02] & 3), 1);
    CHECK_EQ(int(g_mark), int(bootmode::pin_mark(0x01)));
    g_log.clear();
    CHECK_EQ(fusb302::self_power_move(2), false);            // once: no ping-pong
    CHECK_EQ(int(fusb302::pin_rd()), 1);
    CHECK_EQ(first_log("W 02="), -1);
    fusb302::self_power_leave();                             // a run's detach, then the restore: the moved line comes back
    CHECK_EQ(int(g_reg[0x02] & 3), 0);
    fusb302::self_power_restore();
    CHECK_EQ(int(fusb302::pin_rd()), 1);
    CHECK_EQ(int(g_reg[0x02] & 3), 1);
    CHECK_EQ(wrote_rd_pair_to_sw0(), false);
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);   // the next boot may move again
    CHECK_EQ(fusb302::self_power_move(1), true);
    power_cycle_forget_pin();
}

// Hardware 2026-11-06 (OnePlus 120W): the pd-reread's attach on an already known line (the passive scan had taken it) turned
// Auto-CRC on through its Identify demand, but made no Request (a Request needs caps of this attach, and none had arrived):
// the first Source_Capabilities got our GoodCRC and nothing followed, the source's SenderResponseTimer ran out and it sent a
// Hard Reset 27 ms later (VBUS to 0.37 V for ~650 ms: harmless on HID, fatal on a charger-powered K2). Caps the chip
// ACKed always get a Request within the timer.
bool pdo_request_on_the_wire() {
    for (std::size_t i = 0; i + 6 < g_txfifo.size(); i++)
        if (g_txfifo[i] == 0x12 && g_txfifo[i + 1] == 0x12 && g_txfifo[i + 2] == 0x12 && g_txfifo[i + 3] == 0x13) {   // SOP
            const unsigned h = g_txfifo[i + 5] | (unsigned(g_txfifo[i + 6]) << 8);
            return (h & 0x1F) == 2 && ((h >> 12) & 7) == 1;   // Request, one data object
        }
    return false;
}

void test_acked_caps_get_a_request_when_a_reread_attaches_on_a_known_line() {
    passive_attach();                                        // the passive scan took the line; no caps in this attach yet
    CHECK_EQ(fusb302::reread_start(1), true);                // active CC known: sink_request refuses (no caps of this attach)
    for (int i = 0; i < 5; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(auto_crc(), true);                              // the reread's Identify demand: the chip GoodCRCs the caps
    g_txfifo.clear();
    send_caps_5v_9v(0);
    int passes = 0;                                          // 1 ms service() passes: the source's timer is 24-30 ms
    while (passes < 10 && !pdo_request_on_the_wire()) { g_ms += 1; fusb302::service(); ++passes; }
    CHECK_EQ(pdo_request_on_the_wire(), true);
    CHECK_EQ(passes <= 5, true);
    CHECK_EQ(int(fusb302::sink_status().state) != int(pd::SinkState::Idle), true);
    clean_up_rd_tests();
}

// The same caps with Auto-CRC off (the passive idle path) get no GoodCRC and no Request: PD-silent stays silent.
void test_unacked_caps_stay_unanswered_in_the_passive_idle_path() {
    passive_attach();
    CHECK_EQ(auto_crc(), false);
    g_txfifo.clear();
    send_caps_5v_9v(0);
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(pdo_request_on_the_wire(), false);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    clean_up_rd_tests();
}

// ---- unit_hold: the unit boundary of a self-powered run (k2/selfpower-run) ----
// A K2 powered by the brick under test through its pinned Rd must never show a detach: unit_detach() opened the CC lines and the
// OnePlus 120W cut VBUS (hardware 2026-11-06). unit_hold() changes nothing on the wire but the protocol state.
bool soft_reset_on_the_wire() {
    for (std::size_t i = 0; i + 6 < g_txfifo.size(); i++)
        if (g_txfifo[i] == 0x12 && g_txfifo[i + 1] == 0x12 && g_txfifo[i + 2] == 0x12 && g_txfifo[i + 3] == 0x13) {   // SOP
            const unsigned h = g_txfifo[i + 5] | (unsigned(g_txfifo[i + 6]) << 8);
            if ((h & 0x1F) == 13 && ((h >> 12) & 7) == 0) return true;   // Soft_Reset, no data objects
        }
    return false;
}

bool wrote_rd_change_to_sw0(unsigned pin) {   // any SWITCHES0 write that takes the pinned Rd away or adds the other line
    for (const std::string& s : g_log)
        if (s.rfind("W 02=", 0) == 0 && (std::stoul(s.substr(5), nullptr, 16) & 3u) != pin) return true;
    return false;
}

void test_unit_hold_keeps_the_pinned_rd_and_never_opens_cc() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::OkRdCc1, false);
    const unsigned pin = 1;
    fusb302::init();
    fusb302::set_proactive_discover(false);
    settle_ms(200);
    CHECK_EQ(fusb302::rd_for_run(true), true);               // the run start
    g_reg[0x40] = 0x20;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_txfifo.clear();
    send_caps_5v3a();
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), true);                   // the run start's contract at 5 V
    push_frame(0xE0, std::uint16_t(0x0103 | (2u << 6)), {});               // Accept
    push_frame(0xE0, std::uint16_t(0x0106 | (2u << 6) | (1u << 9)), {});   // PS_RDY
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(fusb302::self_powered(), true);

    // PD unit: Soft_Reset on the wire, the pinned Rd stays, the source's fresh caps get PDO 1 again
    g_log.clear();
    g_txfifo.clear();
    fusb302::unit_hold(false);
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(soft_reset_on_the_wire(), true);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    CHECK_EQ(int(fusb302::sink_status().pos), 0);
    CHECK_EQ(wrote_rd_change_to_sw0(pin), false);
    CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
    CHECK_EQ(usb_has("CC open"), false);
    CHECK_EQ(fusb302::self_powered(), true);                 // the pin is never given up
    g_txfifo.clear();
    push_frame(0xE0, std::uint16_t(0x0103 | (2u << 6)), {});               // Accept for the Soft_Reset
    send_caps_5v3a();
    int passes = 0;
    while (passes < 10 && !request_on_the_wire()) { g_ms += 1; fusb302::service(); ++passes; }
    CHECK_EQ(request_on_the_wire(), true);                   // PDO 1 again, inside the source's response timer
    CHECK_EQ(passes <= 5, true);
    push_frame(0xE0, std::uint16_t(0x0103 | (2u << 6)), {});               // Accept
    push_frame(0xE0, std::uint16_t(0x0106 | (2u << 6) | (1u << 9)), {});   // PS_RDY
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(wrote_rd_change_to_sw0(pin), false);

    // Legacy unit: PD-silent, the Rd stays, no CC open; re-announced caps get neither a GoodCRC nor a Request
    g_log.clear();
    g_txfifo.clear();
    fusb302::unit_hold(true);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(int(g_reg[0x03] & 0x04), 0);                    // Auto-CRC off
    push_frame(0xE0, std::uint16_t(0x1001 | (2u << 6) | (2u << 9)), {0x0801912Cu});
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(request_on_the_wire(), false);
    CHECK_EQ(soft_reset_on_the_wire(), false);
    CHECK_EQ(wrote_rd_change_to_sw0(pin), false);
    CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
    CHECK_EQ(usb_has("CC open"), false);
    CHECK_EQ(fusb302::self_powered(), true);

    // back to a PD unit after the legacy one: PD-active again, Soft_Reset out, still no Rd change
    g_log.clear();
    g_txfifo.clear();
    fusb302::unit_hold(false);
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(soft_reset_on_the_wire(), true);
    CHECK_EQ(wrote_rd_change_to_sw0(pin), false);

    fusb302::unit_detach();                                  // a real detach ends the test the way the others do
    fusb302::rd_for_run(false);
    end_start_test();
    power_cycle_forget_pin();
}

// ---- self-hold: a self-powered K2 takes a plain 5 V contract as soon as caps arrive (k2/selfpower-pd-contract) ----
// Hardware 2026-11-06 (OnePlus 120W brick on the top plug, PLUS boot): the key-mode idle is PD-silent (Auto-CRC off), so the
// brick's Source_Capabilities were never GoodCRC'd. It retried for 7-10 s, then went to PE_SRC_Disabled for the attach, and the
// run's evidence read "pdo_count 0": every PD row "not advertised". Reviving PD needs a detach or Hard Reset, either one cuts VBUS
// and kills a K2 that lives on it. So a pinned-Rd K2 answers from the start: Auto-CRC on while attached, PDO 1 at a modest
// current within tSenderResponse, Ready, answering every re-announce; and it never sends a Hard Reset itself.
bool hard_reset_written() {   // CONTROL3 with SEND_HARD_RESET (0x40)
    for (const std::string& s : g_log)
        if (s.rfind("W 09=", 0) == 0 && (std::stoul(s.substr(5), nullptr, 16) & 0x40u)) return true;
    return false;
}
// The data object of the Request the last transmit left in the TX FIFO; 0 when it is none.
std::uint32_t request_rdo() {
    if (!pdo_request_on_the_wire()) return 0;
    for (std::size_t i = 0; i + 10 < g_txfifo.size(); i++)
        if (g_txfifo[i] == 0x12 && g_txfifo[i + 1] == 0x12 && g_txfifo[i + 2] == 0x12 && g_txfifo[i + 3] == 0x13)
            return g_txfifo[i + 7] | (std::uint32_t(g_txfifo[i + 8]) << 8) | (std::uint32_t(g_txfifo[i + 9]) << 16) |
                   (std::uint32_t(g_txfifo[i + 10]) << 24);
    return 0;
}
// A source that announces its caps every 150 ms and gives the attach up after `give_up_after` announcements nobody GoodCRC'd
// (the brick: ~7-10 s). One whose announcement was GoodCRC'd waits for the Request (its SenderResponseTimer) and says no more.
struct RetrySource {
    unsigned give_up_after;
    std::uint32_t next_ms;
    unsigned unacked = 0, sent = 0, id = 0;
    bool gave_up = false, waiting = false;
    RetrySource(unsigned n, std::uint32_t first_ms) : give_up_after(n), next_ms(first_ms) {}
    void tick() {   // once per 1 ms pass, before service()
        if (gave_up || waiting || std::int32_t(g_ms - next_ms) < 0) return;
        next_ms = g_ms + 150;
        const bool acked = auto_crc();   // what the chip does with this frame
        send_caps_5v_9v(id++ & 7);
        ++sent;
        if (acked) waiting = true;
        else if (++unacked >= give_up_after) gave_up = true;
    }
    void reannounce(std::uint32_t at_ms) { waiting = false; gave_up = false; unacked = 0; next_ms = at_ms; }
};
void ms_pass(RetrySource& src, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        g_ms += 1;
        src.tick();
        fusb302::service();
        if ((g_ms % 20) == 0) fusb302::poll();
    }
}
std::uint32_t g_hr_base = 0;
// Boot a pinned K2 (key mode, or the pin inherited by a warm reset) and attach a source to it. Returns the pin.
unsigned self_hold_boot(int how) {   // 0 PLUS, 1 OK, 2 inherited pin (CC2) after a warm reset
    reset_world();
    g_mark = how == 2 ? bootmode::pin_mark(0x02) : std::uint16_t(0);
    if (how == 2) fusb302::self_power_early(bootmode::Mode::Plain, true);
    else fusb302::self_power_early(how == 0 ? bootmode::Mode::PlusRdCc2 : bootmode::Mode::OkRdCc1, false);
    const unsigned pin = fusb302::pin_rd();
    fusb302::init();
    fusb302::set_proactive_discover(false);
    g_auto_txsent = true;
    settle_ms(200);
    CHECK_EQ(auto_crc(), false);             // nothing attached: PD-silent, the passive idle
    CHECK_EQ(g_txfifo.empty(), true);
    g_reg[0x40] = 0x20;                      // the brick's Rp shows up
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_hr_base = fusb302::sink_status().hard_resets;   // the counter outlives detaches (earlier cases leave theirs in it)
    return pin;
}
unsigned hard_resets_since_boot() { return unsigned(fusb302::sink_status().hard_resets - g_hr_base); }
void self_hold_end() {
    fusb302::unit_detach();
    end_start_test();
    power_cycle_forget_pin();
}

void test_self_hold_boots_take_the_5v_contract_as_soon_as_caps_arrive() {
    for (int how = 0; how < 3; how++) {
        const unsigned pin = self_hold_boot(how);
        CHECK_EQ(pin != 0, true);
        for (int i = 0; i < 3; i++) { g_ms += 1; fusb302::service(); }
        CHECK_EQ(auto_crc(), true);          // on from the attach, before any caps: the first announcement is GoodCRC'd
        g_log.clear();
        g_txfifo.clear();
        RetrySource src(4, g_ms + 27);       // the first caps ~27 ms after the attach; the brick gives up after 4 unacked
        unsigned passes = 0;
        while (passes < 120 && !pdo_request_on_the_wire()) { ms_pass(src, 1); ++passes; }
        CHECK_EQ(pdo_request_on_the_wire(), true);
        CHECK_EQ(passes <= 27 + 5, true);    // the Request leaves within ~5 ms of the caps (tSenderResponse is 24-30 ms)
        CHECK_EQ(src.sent, 1u);
        const std::uint32_t rdo = request_rdo();
        CHECK_EQ(int((rdo >> 28) & 7), 1);   // PDO 1
        CHECK_EQ(int((rdo >> 10) & 0x3FF), 100);   // operating current 1 A (modest), not the PDO's 3 A
        CHECK_EQ(int(rdo & 0x3FF), 100);           // maximum current 1 A as well: no capability mismatch
        CHECK_EQ(int((rdo >> 25) & 1), 1);   // USB communications capable, as every Request
        push_frame(0xE0, ctl_msg(3, 1), {});     // Accept
        push_frame(0xE0, ctl_msg(6, 2), {});     // PS_RDY
        ms_pass(src, 40);
        CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
        CHECK_EQ(int(fusb302::sink_status().pos), 1);
        CHECK_EQ(fusb302::self_powered(), true);
        CHECK_EQ(int(g_reg[0x02] & 3), int(pin));    // the pinned Rd never moved
        // a re-announce (the source's own restart) is answered again, inside the response window
        g_txfifo.clear();
        src.reannounce(g_ms + 5);
        passes = 0;
        while (passes < 30 && !pdo_request_on_the_wire()) { ms_pass(src, 1); ++passes; }
        CHECK_EQ(pdo_request_on_the_wire(), true);
        CHECK_EQ(passes <= 5 + 5, true);
        push_frame(0xE0, ctl_msg(3, 3), {});
        push_frame(0xE0, ctl_msg(6, 4), {});
        ms_pass(src, 40);
        CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
        // the contract is never given up: well past the 30 s linger of an unheld one, Ready, Auto-CRC on, Rd on its pin
        for (int i = 0; i < 70; i++) { g_ms += 500; fusb302::poll(); fusb302::service(); }
        CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
        CHECK_EQ(auto_crc(), true);
        CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
        CHECK_EQ(hard_reset_written(), false);   // nothing in this path ever sends a Hard Reset
        CHECK_EQ(hard_resets_since_boot(), 0u);
        CHECK_EQ(fusb302::pd_traffic(), true);   // and the evidence has the caps
        self_hold_end();
    }
}

// The brick's caps retries are GoodCRC'd from the attach on, so it never reaches its give-up: the contrast with the old PD-silent
// idle is the source's own count of unacked announcements.
void test_self_hold_source_never_reaches_its_give_up() {
    self_hold_boot(0);
    for (int i = 0; i < 3; i++) { g_ms += 1; fusb302::service(); }
    RetrySource src(2, g_ms + 27);
    ms_pass(src, 400);
    CHECK_EQ(src.unacked, 0u);
    CHECK_EQ(src.gave_up, false);
    CHECK_EQ(src.waiting, true);
    self_hold_end();
}

// A source that does not answer our Request (no Accept): the sink times out and gives up, without a Hard Reset, and the next
// announcement is asked again.
void test_self_hold_unanswered_request_never_sends_a_hard_reset() {
    self_hold_boot(0);
    for (int i = 0; i < 3; i++) { g_ms += 1; fusb302::service(); }
    g_log.clear();
    RetrySource src(4, g_ms + 27);
    ms_pass(src, 40);
    CHECK_EQ(pdo_request_on_the_wire(), true);
    ms_pass(src, 2000);                              // no Accept, no PS_RDY: the sink's own timer runs out (kAcceptMs)
    CHECK_EQ(hard_reset_written(), false);
    CHECK_EQ(hard_resets_since_boot(), 0u);
    CHECK_EQ(int(fusb302::sink_status().result), int(pd::SinkResult::Timeout));
    CHECK_EQ(fusb302::self_powered(), true);
    g_txfifo.clear();
    src.reannounce(g_ms + 5);
    unsigned passes = 0;
    while (passes < 30 && !pdo_request_on_the_wire()) { ms_pass(src, 1); ++passes; }
    CHECK_EQ(pdo_request_on_the_wire(), true);       // a re-announce after the failure is asked again
    CHECK_EQ(hard_reset_written(), false);
    self_hold_end();
}

// A source that sends no caps at all leaves the K2 PD-silent on the wire: Auto-CRC on, nothing transmitted.
void test_self_hold_source_without_caps_leaves_the_k2_silent() {
    self_hold_boot(1);
    g_txfifo.clear();
    g_log.clear();
    for (int i = 0; i < 200; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(g_txfifo.empty(), true);
    CHECK_EQ(hard_reset_written(), false);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    CHECK_EQ(int(g_reg[0x02] & 3), 1);
    self_hold_end();
}

// HID-powered (plain boot) is unchanged: passive scan, Auto-CRC off, caps unanswered, and a Request that goes unanswered still
// ends in the Hard Reset it always did (nothing there lives on the source's VBUS).
void test_plain_boot_is_unchanged_by_self_hold() {
    passive_attach();
    CHECK_EQ(fusb302::self_powered(), false);
    for (int i = 0; i < 5; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(auto_crc(), false);
    g_txfifo.clear();
    send_caps_5v_9v(0);
    for (int i = 0; i < 40; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(pdo_request_on_the_wire(), false);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// The PD-silent legacy unit of a self-powered run overrides the hold: no GoodCRC, no Request, then back to PD.
void test_self_hold_yields_to_a_legacy_unit() {
    self_hold_boot(0);
    for (int i = 0; i < 3; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(auto_crc(), true);
    fusb302::unit_hold(true);
    for (int i = 0; i < 100; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(auto_crc(), false);
    g_txfifo.clear();
    send_caps_5v_9v(1);
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(pdo_request_on_the_wire(), false);
    fusb302::unit_hold(false);
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(auto_crc(), true);
    self_hold_end();
}

// START finds the K2 already in its self-hold contract (no run-start Rd yet): the PD unit's hold is a Soft_Reset from that live
// contract, the source re-announces and the K2 asks PDO 1 again; the pinned Rd never moves and nothing sends a Hard Reset.
void test_self_hold_contract_then_unit_hold_soft_resets_from_the_live_contract() {
    const unsigned pin = self_hold_boot(0);
    for (int i = 0; i < 3; i++) { g_ms += 1; fusb302::service(); }
    RetrySource src(4, g_ms + 27);
    ms_pass(src, 40);
    CHECK_EQ(pdo_request_on_the_wire(), true);
    push_frame(0xE0, ctl_msg(3, 1), {});
    push_frame(0xE0, ctl_msg(6, 2), {});
    ms_pass(src, 40);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(fusb302::rd_for_run_on(), false);
    g_txfifo.clear();
    fusb302::unit_hold(false);
    ms_pass(src, 10);
    CHECK_EQ(soft_reset_on_the_wire(), true);
    CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
    g_txfifo.clear();
    push_frame(0xE0, ctl_msg(3, 3), {});     // Accept for the Soft_Reset
    send_caps_5v3a();
    unsigned passes = 0;
    while (passes < 10 && !request_on_the_wire()) { g_ms += 1; fusb302::service(); ++passes; }
    CHECK_EQ(request_on_the_wire(), true);
    CHECK_EQ(passes <= 5, true);
    push_frame(0xE0, ctl_msg(3, 4), {});
    push_frame(0xE0, ctl_msg(6, 5), {});
    ms_pass(src, 40);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(hard_reset_written(), false);
    CHECK_EQ(hard_resets_since_boot(), 0u);
    self_hold_end();
}

// A key-mode boot whose page arms a cable identity puts the Ra on (PC13 high, Rd on CC2 alone) before the source's first
// attach completes, and the 5 V contract still follows as soon as caps arrive: the boot notes show PC13 in the CC routing
// before any caps, the Ra and the pin never move, no Hard Reset.
void test_boot_ra_before_the_attach_then_the_5v_contract() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);
    fusb302::init();
    fusb302::set_proactive_discover(false);
    g_auto_txsent = true;
    CHECK_EQ(fusb302::ra_set(true), true);               // the page's arm at boot: nothing attached yet
    CHECK_EQ(fusb302::ra_active(), true);
    CHECK_EQ(g_pin[0], true);                            // PC13 high: the Ra on CC1
    // (The boot-note ring that K2B1 "n=" shows records the PC13 routing, bit 2 of a kind-1 note, but it is a 24-slot static that
    // the earlier cases of this file have filled, so the note itself is asserted in test_bootinfo.)
    CHECK_EQ(int(g_reg[0x02] & 3), 2);                  // Rd on CC2 alone: the pin the key chose
    settle_ms(200);
    CHECK_EQ(g_pin[0], true);                            // the passive idle never drops it
    CHECK_EQ(auto_crc(), false);
    g_reg[0x40] = 0x20;                                  // the brick's Rp shows up
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_hr_base = fusb302::sink_status().hard_resets;
    for (int i = 0; i < 3; i++) { g_ms += 1; fusb302::service(); }
    CHECK_EQ(auto_crc(), true);
    g_txfifo.clear();
    RetrySource src(4, g_ms + 27);
    unsigned passes = 0;
    while (passes < 120 && !pdo_request_on_the_wire()) { ms_pass(src, 1); ++passes; }
    CHECK_EQ(pdo_request_on_the_wire(), true);
    CHECK_EQ(passes <= 27 + 5, true);
    push_frame(0xE0, ctl_msg(3, 1), {});
    push_frame(0xE0, ctl_msg(6, 2), {});
    ms_pass(src, 40);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Ready));
    CHECK_EQ(fusb302::ra_active(), true);
    CHECK_EQ(g_pin[0], true);
    CHECK_EQ(int(g_reg[0x02] & 3), 2);
    CHECK_EQ(hard_reset_written(), false);
    CHECK_EQ(hard_resets_since_boot(), 0u);
    self_hold_end();
    CHECK_EQ(fusb302::ra_active(), false);
}

// S7: a cable read started while Auto-CRC is on (the armed spoof keeps it on) used to run with CONTROL1 0x05, so the chip
// never listened for the cable's reply on SOP' / SOP''. The scan itself now writes the listening value (0x67) before every
// request and puts the live value back afterwards.
void test_cable_read_listens_on_sop_prime_and_sop_pp_with_auto_crc_on() {
    passive_attach();
    spoof_hook_on();
    settle_ms(2000);
    CHECK_EQ(auto_crc(), true);
    CHECK_EQ(int(g_reg[0x07]), 0x05);
    g_cable.cc = 1;
    g_log.clear();
    CHECK_EQ(fusb302::read_emarker(true, pd::EmarkerMode::Vconn), true);
    const int first_tx = find_from("W 06=01");
    CHECK_EQ(first_tx > 0, true);
    int last_ctl1 = -1;
    for (int i = 0; i < first_tx; i++) if (g_log[i].rfind("W 07=", 0) == 0) last_ctl1 = i;
    CHECK_EQ(last_ctl1 >= 0, true);
    CHECK_EQ(g_log[last_ctl1] == "W 07=67", true);
    CHECK_EQ(int(g_reg[0x07]), 0x05);                        // restored: Auto-CRC on, SOP' only for the armed spoof
    clean_up_rd_tests();
    g_reg[0x40] = 0;
    for (unsigned i = 0; i < 40; ++i) { g_ms += 20; fusb302::poll(); }
}

// S7: the same hook that serves the spoof's SOP' now also gets SOP'' (passive CONTROL1 listens on both), with the kind
// as its first argument; SOP never reaches it.
static std::uint8_t g_hook_sop[8];
static std::uint16_t g_hook_hdr[8];
static int g_hook_n = 0;
void test_sop_pp_frames_reach_the_hook_with_their_sop() {
    passive_attach();
    g_hook_n = 0;
    fusb302::set_sop_prime_hook(+[](std::uint8_t sop, std::uint16_t h, std::span<const std::uint8_t>) {
        if (g_hook_n < 8) { g_hook_sop[g_hook_n] = sop; g_hook_hdr[g_hook_n] = h; g_hook_n++; }
    });
    const std::uint16_t di = 0x118F;
    fusb302::feed(2, di, {});
    CHECK_EQ(g_hook_n, 1);
    CHECK_EQ(int(g_hook_sop[0]), 2);
    fusb302::feed(1, di, {});
    CHECK_EQ(g_hook_n, 2);
    CHECK_EQ(int(g_hook_sop[1]), 1);
    fusb302::feed(0, di, {});
    CHECK_EQ(g_hook_n, 2);                                   // SOP is the source's own traffic
    // Off the wire: the FIFO token 0xA0 is SOP''.
    push_frame(0xA0, 0x118F, {});
    fusb302::service();
    CHECK_EQ(g_hook_n, 3);
    CHECK_EQ(int(g_hook_sop[2]), 2);
    CHECK_EQ(int(g_hook_hdr[2]), 0x118F);
    CHECK_EQ(usb_has("[PD] SOP'' "), true);
    fusb302::set_sop_prime_hook(nullptr);
    push_frame(0xA0, 0x118F, {});
    fusb302::service();
    CHECK_EQ(g_hook_n, 3);                                   // no hook, no call
    g_reg[0x40] = 0;
    for (unsigned i = 0; i < 40; ++i) { g_ms += 20; fusb302::poll(); }   // ends with a detach
}

// ---- A passive K2 never answers by itself (k2/passive-after-hr) ----
// Hardware 2026-11-07 (Baseus 140W -> K2 bottom port, K2 top plug -> a Kowsi tester as the sink; plain boot, nothing pressed): the
// Kowsi starting its test made the charger send two Hard Resets, and the K2 answered the re-announced caps with a Request for
// PDO 1, Rd on CC2 and Auto-CRC on, then held the contract: the Kowsi's own PD test never got a line. A received Hard Reset
// marked the sink to "resume", whether or not it ever had a contract. A passive K2 presents no Rd, turns no Auto-CRC on and
// transmits nothing unless an operation asked for it.
// True while no write in g_log[from..] presents Rd (SWITCHES0 PDWN1/2), turns Auto-CRC on or transmits (a FIFO load, TX_START).
bool passive_stayed_silent(std::size_t from) {
    bool ok = true;
    for (std::size_t i = from; i < g_log.size(); i++) {
        const std::string& s = g_log[i];
        const unsigned v = s.size() >= 7 && s[4] == '=' ? unsigned(std::stoul(s.substr(5), nullptr, 16)) : 0u;
        const bool bad = (s.rfind("W 02=", 0) == 0 && (v & 3u) != 0) || (s.rfind("W 03=", 0) == 0 && (v & 4u) != 0) ||
                         (s.rfind("W 06=", 0) == 0 && (v & 1u) != 0) || s.rfind("FIFO", 0) == 0;
        if (bad) { std::fprintf(stderr, "passive K2 broke silence: %s\n", s.c_str()); ok = false; }
    }
    return ok && sw0_rd() == 0u && !auto_crc();
}
// The charger's side of that evening: two Hard Resets 1 ms apart, VBUS down and back (~1.1 s), the caps twice, a few ms apart.
void hard_reset_x2_then_caps_x2() {
    g_ia_script = {0x01};
    g_ms += 1; fusb302::service();
    g_ia_script = {0x01};
    g_ms += 1; fusb302::service();
    for (int i = 0; i < 55; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    g_txfifo.clear();
    send_caps_5v_9v(0);
    send_caps_5v_9v(0);
    for (int i = 0; i < 60; i++) { g_ms += 1; fusb302::service(); }
    for (int i = 0; i < 20; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
}
// The invariant, after whatever got the K2 passive again: Hard Reset, caps, and not a sound from the K2.
void expect_silent_through_a_hard_reset() {
    CHECK_EQ(auto_crc(), false);
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
    const std::size_t from = g_log.size();
    hard_reset_x2_then_caps_x2();
    CHECK_EQ(passive_stayed_silent(from), true);
    CHECK_EQ(pdo_request_on_the_wire(), false);
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));
}

void test_passive_idle_stays_silent_through_a_source_hard_reset() {
    passive_attach();
    settle_ms(2000);
    expect_silent_through_a_hard_reset();
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
void test_finished_run_stays_silent_through_a_source_hard_reset() {
    start_attach_to_ready();
    fusb302::rd_for_run(false);              // the run's end releases to 5 V: a Request the charger answers
    for (int i = 0; i < 10; i++) { g_ms += 1; fusb302::service(); }
    push_frame(0xE0, ctl_msg(3, 3), {});
    push_frame(0xE0, ctl_msg(6, 4), {});
    for (int i = 0; i < 30; i++) { g_ms += 1; fusb302::service(); }
    settle_ms(36000);                        // the 5 V contract nobody holds lingers out
    check_rd_gone(false);
    expect_silent_through_a_hard_reset();
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
void test_released_trigger_stays_silent_through_a_source_hard_reset() {
    start_attach_to_ready();
    fusb302::rd_for_run(false);
    trigger_9v();
    release_to_5v();
    settle_ms(36000);
    check_rd_gone(false);
    expect_silent_through_a_hard_reset();
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
void test_finished_reread_stays_silent_through_a_source_hard_reset() {
    reread_attach_forced_to_ready();
    fusb302::reread_abort();
    settle_ms(6000);
    check_rd_gone(false);
    expect_silent_through_a_hard_reset();
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
// A K2 that never made a contract and saw a source's Hard Reset while an Identify ran on its own: nothing to resume either.
void test_aborted_reread_before_any_contract_stays_silent_through_a_source_hard_reset() {
    passive_attach();
    CHECK_EQ(fusb302::reread_start(1), true);
    fusb302::reread_abort();
    settle_ms(6000);
    expect_silent_through_a_hard_reset();
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
// The other side of the rule: a trigger the user holds is a contract the K2 owns, so a Hard Reset under it is still answered.
void test_held_trigger_still_resumes_after_a_source_hard_reset() {
    start_attach_to_ready();
    fusb302::rd_for_run(false);
    trigger_9v();
    settle_ms(2000);
    g_ia_script = {0x01};
    g_ms += 1; fusb302::service();
    for (int i = 0; i < 55; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    g_txfifo.clear();
    send_caps_5v_9v(0);
    int passes = 0;
    while (passes < 10 && !pdo_request_on_the_wire()) { g_ms += 1; fusb302::service(); ++passes; }
    CHECK_EQ(pdo_request_on_the_wire(), true);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// ... and once the user lets go (pd release) before the caps are back, the pending resume goes with the hold.
void test_release_after_a_source_hard_reset_cancels_the_resume() {
    start_attach_to_ready();
    fusb302::rd_for_run(false);
    trigger_9v();
    settle_ms(2000);
    g_ia_script = {0x01};
    g_ms += 1; fusb302::service();
    fusb302::sink_release();
    for (int i = 0; i < 30; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    settle_ms(36000);
    check_rd_gone(false);
    expect_silent_through_a_hard_reset();
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// ---- A K2 inline between a source and another sink sends no SOP message (k2/inline-silent) ----
// Hardware 2026-11-07 (recordings op15/rec1 Xiaomi 20K, op15/rec2-svooc OnePlus SUPERVOOC; the K2 inline, a OnePlus 15 on the
// top plug): the phone, a sink behind the K2, sent Discover Identity and then Soft_Reset to the source, and the K2 answered both
// (a 4-object Vendor_Defined 5.8 ms after the identity request, an Accept 5.9 ms after the Soft_Reset) although the link was
// already latched inline: the Sink's replies to a partner's VDMs and Soft_Reset had no inline gate. The source took the extra
// SOP traffic as a protocol error and sent Hard Resets. What the K2 can see: the phone's Request (power-role bit clear) and,
// later, the Link's inline latch. A passive K2 (Auto-CRC off, no Rd) must stay on RX only.
constexpr std::uint16_t kRev3 = 2u << 6;                          // PD 3.0 in the header's spec-revision field
constexpr std::uint16_t kFromSource = 1u << 8;                    // Port Power Role = source
constexpr std::uint32_t kDiscoverIdentityReq = 0xFF008001u;       // structured VDM, command 1, REQ
std::uint16_t hdr(unsigned type, unsigned num_do, bool from_source, unsigned id = 0) {
    return std::uint16_t(type | (num_do << 12) | kRev3 | (from_source ? kFromSource : 0u) | (id << 9));
}
// A source that Source_Capabilities, a far sink that Requests: the K2 sees both on the wire and sends nothing.
void far_sink_negotiates() {
    push_frame(0xE0, hdr(1, 1, true), {0x0801912Cu});            // Source_Capabilities: fixed 5 V 3 A
    push_frame(0xC0, hdr(1, 0, false), {});                      // the phone's GoodCRC for them
    push_frame(0xE0, hdr(2, 1, false), {0x1001912Cu});           // the phone's Request, PDO 1
    push_frame(0xC0, hdr(1, 0, true), {});                       // the source's GoodCRC for it
    push_frame(0xE0, hdr(3, 0, true, 1), {});                    // Accept
    push_frame(0xE0, hdr(6, 0, true, 2), {});                    // PS_RDY
    for (int i = 0; i < 20; i++) { g_ms += 1; fusb302::service(); }
}
int fifo_loads(std::size_t from) {   // TX FIFO loads since g_log[from]: every K2 transmission starts with one
    int n = 0;
    for (std::size_t i = from; i < g_log.size(); i++) n += g_log[i].rfind("FIFO", 0) == 0;
    return n;
}
void service_ms(int n) { for (int i = 0; i < n; i++) { g_ms += 1; fusb302::service(); } }
// Header of the SOP message in the last FIFO load (0 = nothing loaded).
unsigned sop_tx_header() {
    for (std::size_t i = 0; i + 6 < g_txfifo.size(); i++)
        if (g_txfifo[i] == 0x12 && g_txfifo[i + 1] == 0x12 && g_txfifo[i + 2] == 0x12 && g_txfifo[i + 3] == 0x13)
            return g_txfifo[i + 5] | (unsigned(g_txfifo[i + 6]) << 8);
    return 0;
}

// rec1 2188543..2188561: the phone's Discover Identity, then its Soft_Reset. Before the fix: a Vendor_Defined and an Accept.
void test_a_far_sinks_identity_request_and_soft_reset_get_no_sop_from_the_passive_k2() {
    passive_attach();
    settle_ms(100);
    far_sink_negotiates();
    CHECK_EQ(fusb302::inline_mode(), false);                    // the Link has not latched yet: the wire alone has to tell
    const std::size_t from = g_log.size();
    push_frame(0xE0, hdr(0x0F, 1, false), {kDiscoverIdentityReq});   // the phone asks the source for its identity
    service_ms(10);
    push_frame(0xE0, hdr(0x0D, 0, false, 1), {});                    // the phone's Soft_Reset
    service_ms(10);
    push_frame(0xE0, hdr(0x0F, 1, true), {kDiscoverIdentityReq});    // the source's own identity request, to the phone
    service_ms(10);
    CHECK_EQ(fifo_loads(from), 0);
    CHECK_EQ(passive_stayed_silent(from), true);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// Same traffic with the Link's inline latch set and no Request on the wire (the K2 plugged in after the contract).
void test_the_inline_latch_alone_silences_the_identity_and_soft_reset_replies() {
    passive_attach();
    settle_ms(100);
    fusb302::set_inline(true);
    const std::size_t from = g_log.size();
    push_frame(0xE0, hdr(0x0F, 1, true), {kDiscoverIdentityReq});
    service_ms(10);
    push_frame(0xE0, hdr(0x0D, 0, true, 1), {});
    service_ms(10);
    CHECK_EQ(fifo_loads(from), 0);
    CHECK_EQ(passive_stayed_silent(from), true);
    fusb302::set_inline(false);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// The K2 as the only sink keeps answering its charger: identity (4 objects) and Soft_Reset (Accept).
void test_the_k2_as_the_only_sink_still_answers_identity_and_soft_reset() {
    start_attach_to_ready();
    g_txfifo.clear();
    push_frame(0xE0, hdr(0x0F, 1, true), {kDiscoverIdentityReq});
    service_ms(10);
    CHECK_EQ(sop_tx_header() & 0x1F, 0x0Fu);
    CHECK_EQ((sop_tx_header() >> 12) & 7, 4u);                  // the identity ACK: header, ID header, cert stat, product
    g_txfifo.clear();
    push_frame(0xE0, hdr(0x0D, 0, true, 3), {});
    service_ms(10);
    CHECK_EQ(sop_tx_header() & 0x1F, 3u);                       // Accept
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// A far sink belongs to one attach: the next charger plugged straight into the K2 is answered again.
void test_a_new_attach_forgets_the_far_sink() {
    passive_attach();
    settle_ms(100);
    far_sink_negotiates();
    g_reg[0x40] = 0x00;                                          // the source leaves
    settle_ms(2000);
    start_attach_to_ready();
    g_txfifo.clear();
    push_frame(0xE0, hdr(0x0F, 1, true), {kDiscoverIdentityReq});
    service_ms(10);
    CHECK_EQ(sop_tx_header() & 0x1F, 0x0Fu);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// ---- A passive K2 never transmits on a live link, pre-inline window included ----
// Cold boot, laptop - K2 - dock (the K2's HID cable on the dock's hub): in the ~1 s between "attach CC1" and the
// inline decision sole_sink() is true (no inline latch, no far sink's Request on the wire yet), so the sink policy
// armed its answers - PR_Swap -> Reject, Soft_Reset -> Accept, Get_Sink_Cap -> Sink_Capabilities - and transmit()
// sent them, forcing a Soft_Reset on the laptop - dock link. Passive mode must refuse every TX at the choke point:
// zero TX FIFO loads / TX_STARTs, Auto-CRC off in SWITCHES1 throughout, until an active feature demands the wire.
void test_passive_cold_boot_never_transmits_on_a_live_link() {
    passive_attach();                                            // cold-boot attach on CC1
    fusb302::sink_vbus(5000);                                    // VBUS present (the dock's port)
    settle_ms(100);
    CHECK_EQ(fusb302::inline_mode(), false);                     // the pre-inline window: sole_sink() is true
    CHECK_EQ(auto_crc(), false);                                 // Auto-CRC off before the traffic
    const std::size_t from = g_log.size();
    push_frame(0xE0, hdr(10, 0, false), {});                     // PR_Swap from the dock (snk/ufp asks to swap)
    service_ms(10);
    CHECK_EQ(auto_crc(), false);
    push_frame(0xC0, hdr(0x0F, 1, true), {kDiscoverIdentityReq});   // SOP' Discover Identity REQ (laptop to the cable)
    service_ms(10);
    push_frame(0xE0, hdr(13, 0, false, 1), {});                  // Soft_Reset from the laptop (snk/dfp)
    service_ms(10);
    push_frame(0xE0, hdr(8, 0, true), {});                       // Get_Sink_Cap from the source
    service_ms(10);
    settle_ms(200);
    CHECK_EQ(fifo_loads(from), 0);                               // zero TX records
    CHECK_EQ(find_from("W 06=01", from) == -1, true);            // no TX_START either
    CHECK_EQ(passive_stayed_silent(from), true);                 // no Rd, no Auto-CRC write, no FIFO, no TX_START
    CHECK_EQ(auto_crc(), false);                                 // SWITCHES1 still has Auto-CRC off
    CHECK_EQ(usb_has("TX refused (passive)"), true);             // the refusal is logged, not silent
    // Positive control: an active feature (the run start's Rd) demands the wire, TX is allowed again.
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_txfifo.clear();
    const std::size_t from2 = g_log.size();
    send_caps_5v3a();
    int passes = 0;
    while (passes < 10 && !pdo_request_on_the_wire()) { g_ms += 1; fusb302::service(); ++passes; }
    CHECK_EQ(pdo_request_on_the_wire(), true);                   // the run start's Request for PDO 1 went out
    CHECK_EQ(fifo_loads(from2) > 0, true);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// A planned re-attach's comeback boot (k2/core/rd_intent.hpp): Rd on CC2 from the first write, PC13 (Ra) high from the same moment,
// and nothing on the boot path writes a Rd-less SWITCHES0 (hardware, a comeback trace: init used to write 0x00 and the pin again
// 1.3 ms later, a detach the charger can see). The chip's power-on default (Rd on both, 0x03) is the chip's, not a write, and
// the boot path never SW_RESes a pinned boot, so no write of ours ever carries it either.
void test_comeback_boot_never_writes_a_rd_less_switches0_and_has_the_ra_first() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);
    fusb302::self_power_early_ra();
    CHECK_EQ(g_pin[0], true);                                // the Ra is on before init
    CHECK_EQ(int(g_reg[0x02] & 3), 2);
    const std::size_t from = g_log.size();                   // (the pin's own first drive, low, is the line-up of the CC switches)
    fusb302::init();
    fusb302::set_proactive_discover(false);
    settle_ms(200);
    CHECK_EQ(g_pin[0], true);
    CHECK_EQ(find_from("PC13=0", from), -1);                 // never low on the way
    for (std::size_t i = from; i < g_log.size(); i++)
        if (g_log[i].rfind("W 02=", 0) == 0) CHECK_EQ(int(std::stoul(g_log[i].substr(5), nullptr, 16) & 3u), 2);
    CHECK_EQ(int(g_reg[0x02] & 3), 2);
    CHECK_EQ(fusb302::self_powered(), true);
    // The page's spoof takes the early Ra over; ra_set(false) with the Ra only early (nothing else asked for it) just drops it.
    CHECK_EQ(fusb302::ra_set(true), true);
    CHECK_EQ(g_pin[0], true);
    CHECK_EQ(fusb302::ra_set(false), true);
    CHECK_EQ(g_pin[0], false);
    CHECK_EQ(int(g_reg[0x02] & 3), 2);                       // the Rd stays until the verdict says otherwise
    self_hold_end();
}

// The comeback is released (no source caps in time): Ra and the pin go, Rd goes with them, nothing resumes.
void test_comeback_release_gives_the_pin_and_the_early_ra_back() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);
    fusb302::self_power_early_ra();
    fusb302::init();
    settle_ms(50);
    CHECK_EQ(g_pin[0], true);
    CHECK_EQ(fusb302::ra_set(false), true);                  // the early Ra alone can be dropped
    CHECK_EQ(g_pin[0], false);
    fusb302::self_power_early_ra();                          // (a second early Ra is harmless)
    fusb302::self_power_drop();
    CHECK_EQ(g_pin[0], false);
    CHECK_EQ(fusb302::self_powered(), false);
    CHECK_EQ(int(g_reg[0x02] & 3), 0);
    power_cycle_forget_pin();
}

// The planned Rd glitch (lab op 8 and the charger test's planned reboot): the Rd goes for the gap with the Ra kept, comes back to
// the pin, and the sink starts fresh.
void test_rd_glitch_drops_the_rd_keeps_the_ra_and_restores_the_pin() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);
    fusb302::init();
    fusb302::set_proactive_discover(false);
    CHECK_EQ(fusb302::ra_set(true), true);
    settle_ms(50);
    g_log.clear();
    fusb302::rd_glitch_begin();
    CHECK_EQ(int(g_reg[0x02] & 3), 0);
    CHECK_EQ(g_pin[0], true);                                // the Ra stays through the gap
    fusb302::rd_glitch_end();
    CHECK_EQ(int(g_reg[0x02] & 3), 2);
    CHECK_EQ(g_pin[0], true);
    CHECK_EQ(find_from("PC13=0"), -1);
    fusb302::ra_set(false);
    self_hold_end();
}

// ---- the Rd seek: a passive pick on the wrong line (hardware 2026-11-09, an Anker 2-way splitter behind a 140 W charger) ----
// The passive scan (no Rd anywhere) reads CC2 high and CC1 low and attaches on CC2. The splitter shows its Rp only once it sees an
// Rd, and only on CC1. A re-read (or a run start, or a PD trigger) puts the K2's Rd on the active line, CC2, which reads 0 V, so
// the attach ended in a 500 ms "no Rp" detach and the other line was never tried. The Rd now moves, one line at a time.
void seek_world() {
    power_cycle_forget_pin();                                // the earlier tests may leave a boot-mode pin behind
    fusb302::set_force_line(0);                              // ... or a forced debug line
    reread_style_boot();                                   // reset_world + init, passive (Rd off), nothing attached
    g_rd_model = true;
    g_lvl_nord[1] = 1600;                                    // CC2 floats high with nothing behind it (a coupled net)
    g_lvl_rd[1] = 0;                                         // ... and reads nothing once our Rd is on it
    g_lvl_nord[0] = 0;                                       // CC1 is dead to a passive scan
    g_lvl_rd[0] = 1600;                                      // ... and carries the Rp only over our Rd
    g_reg[0x40] = 0x23;                                      // COMP armed, BC_LVL 3 while a line is above the threshold
}
void seek_attach_cc2() {
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected: CC2"), true);
    CHECK_EQ(sw0_rd(), 0u);                                  // the scan was passive: no Rd on any line
}
// Every SWITCHES0 write from `from` on keeps at most one Rd line (two is the Debug Accessory signature).
bool never_rd_on_both_lines(std::size_t from, std::size_t to = std::size_t(-1)) {
    for (std::size_t i = from; i < g_log.size() && i < to; i++)
        if (g_log[i].rfind("W 02=", 0) == 0 && (std::stoul(g_log[i].substr(5), nullptr, 16) & 3u) == 3u) return false;
    return true;
}
int seek_moves_logged() {
    int n = 0;
    for (const auto& s : g_usb) if (s.find("Rd seek:") != std::string::npos) ++n;
    return n;
}

void test_seek_moves_the_rd_to_the_line_that_answers_and_keeps_the_attach() {
    seek_world();
    seek_attach_cc2();
    g_log.clear();
    CHECK_EQ(fusb302::rd_for_run(true), true);               // the re-read's Rd goes on CC2
    CHECK_EQ(sw0_rd(), 2u);
    const std::uint32_t t0 = g_ms;
    std::uint32_t moved_at = 0;
    for (int i = 0; i < 100 && !usb_has("Active CC detected: CC1"); i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    moved_at = g_ms - t0;
    CHECK_EQ(usb_has("Active CC detected: CC1"), true);
    CHECK_EQ(moved_at >= 200u, true);                     // not before the dwell ...
    CHECK_EQ(moved_at < 400u, true);                         // ... and far inside the old 500 ms give-up
    CHECK_EQ(sw0_rd(), 1u);                                  // the single Rd followed
    CHECK_EQ(never_rd_on_both_lines(0), true);
    CHECK_EQ(seek_moves_logged(), 1);
    settle_ms(3000);                                         // the Rp is there now: no detach, no second move, no Rd pair
    CHECK_EQ(usb_has("Detach"), false);
    CHECK_EQ(seek_moves_logged(), 1);
    CHECK_EQ(sw0_rd(), 1u);
    CHECK_EQ(never_rd_on_both_lines(0), true);
    clean_up_rd_tests();
}

// The chip's own signature: the move is a new attach for the sink side (session, PD reset) but never a detach for the source.
void test_seek_move_is_an_attach_not_a_detach() {
    seek_world();
    seek_attach_cc2();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    g_log.clear();
    settle_ms(400);
    CHECK_EQ(find_from("W 0C=02") >= 0, true);               // the new attach resets the PD logic (an ordinary attach does) ...
    CHECK_EQ(find_from("W 0C=01"), -1);                      // ... but the chip is never soft-reset by a move (SW_RES)
    CHECK_EQ(usb_has("Detach"), false);
    clean_up_rd_tests();
}

// Nothing answers on either line: three moves, then the ordinary no-Rp detach; about a second in all, never an Rd pair.
void test_seek_is_bounded_and_ends_in_the_ordinary_detach() {
    seek_world();
    g_lvl_rd[0] = 0;                                         // no line ever shows an Rp
    seek_attach_cc2();
    g_log.clear();
    CHECK_EQ(fusb302::rd_for_run(true), true);
    const std::uint32_t t0 = g_ms;
    std::size_t before_detach = 0;                           // the log up to the poll that detached (a detach's own rescan is the START's)
    for (int i = 0; i < 200 && !usb_has("Detach"); i++) {
        before_detach = g_log.size();
        g_ms += 20; fusb302::poll(); fusb302::service();
    }
    const std::uint32_t gave_up = g_ms - t0;
    CHECK_EQ(usb_has("Detach"), true);
    CHECK_EQ(seek_moves_logged(), 3);
    CHECK_EQ(gave_up >= 1000u, true);                        // a real sink's debounce plus the source's VBUS-on
    CHECK_EQ(gave_up <= 1300u, true);
    CHECK_EQ(never_rd_on_both_lines(0, before_detach), true);
    fusb302::rd_for_run(false);
    settle_ms(100);
    clean_up_rd_tests();
}

// The seek only starts from a passive pick: a line the scan took while our Rd was on it was verified by its Rp already.
void test_seek_ignores_an_attach_that_was_picked_with_rd_on() {
    seek_world();
    g_lvl_rd[1] = 1600;                                      // an ordinary source: its Rp shows over our Rd on CC2
    CHECK_EQ(fusb302::rd_for_run(true), true);               // Rd on from the start (both lines while none is known)
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    g_lvl_rd[0] = g_lvl_rd[1] = 0;                           // the source drops its Rp (a real detach)
    const std::uint32_t t0 = g_ms;
    for (int i = 0; i < 100 && !usb_has("Detach"); i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(usb_has("Detach"), true);
    CHECK_EQ(g_ms - t0 < 700u, true);                        // the plain 500 ms rule, not stretched by a seek
    CHECK_EQ(seek_moves_logged(), 0);
    fusb302::rd_for_run(false);
    settle_ms(100);
    clean_up_rd_tests();
}

// Never inline (a real sink's Rd must stay alone on the line), never in the Ra mode (CC2 Rd by design), never on a forced line.
void test_seek_never_runs_inline_in_the_ra_mode_or_on_a_forced_line() {
    {   // inline
        seek_world();
        seek_attach_cc2();
        CHECK_EQ(fusb302::rd_for_run(true), true);
        fusb302::set_inline(true);                           // a sink arrived: our Rd comes off, active features refuse
        g_log.clear();
        settle_ms(2000);
        CHECK_EQ(seek_moves_logged(), 0);
        CHECK_EQ(sw0_rd(), 0u);
        CHECK_EQ(usb_has("Active CC detected: CC1"), false);
        fusb302::set_inline(false);
        fusb302::rd_for_run(false);
        settle_ms(100);
        clean_up_rd_tests();
    }
    {   // the Ra mode: Rd on CC2 alone, the Ra on CC1
        seek_world();
        CHECK_EQ(fusb302::ra_set(true), true);
        CHECK_EQ(fusb302::rd_for_run(true), true);
        g_lvl_rd[1] = 1600;                                  // the source answers on CC2 so the attach happens
        for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
        CHECK_EQ(usb_has("Active CC detected: CC2"), true);
        g_lvl_rd[1] = 0;                                     // then it goes quiet
        g_log.clear();
        settle_ms(440);                                      // inside the 500 ms no-Rp rule: a seek (200 ms dwell) would have moved by now
        CHECK_EQ(seek_moves_logged(), 0);
        for (const auto& e : g_log)
            if (e.rfind("W 02=", 0) == 0) CHECK_EQ(int(std::stoul(e.substr(5), nullptr, 16) & 3u), 2);   // PDWN2 alone, always
        fusb302::rd_for_run(false);
        settle_ms(100);
        clean_up_rd_tests();
    }
    {   // a forced line (pd-reread --line 2)
        seek_world();
        fusb302::set_force_line(2);
        seek_attach_cc2();
        CHECK_EQ(fusb302::rd_for_run(true), true);
        g_log.clear();
        settle_ms(800);
        CHECK_EQ(seek_moves_logged(), 0);
        CHECK_EQ(usb_has("Active CC detected: CC1"), false);
        fusb302::rd_for_run(false);
        settle_ms(100);
        clean_up_rd_tests();
    }
}

// A key mode's pinned Rd (self-powered boot) is the charger's own supply: it only ever moves through self_power_move.
void test_seek_never_moves_a_pinned_rd() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::PlusRdCc2, false);
    fusb302::init();
    fusb302::set_proactive_discover(false);
    g_rd_model = true;
    g_lvl_rd[1] = 1600;                                      // the charger answers the pinned Rd on CC2
    g_lvl_nord[1] = 1600;
    g_reg[0x40] = 0x23;
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(usb_has("Active CC detected: CC2"), true);
    g_lvl_rd[1] = g_lvl_nord[1] = 0;                         // quiet: the ordinary 500 ms rule applies, no seek
    g_lvl_rd[0] = 1600;
    g_log.clear();
    settle_ms(800);
    CHECK_EQ(seek_moves_logged(), 0);
    CHECK_EQ(int(fusb302::pin_rd()), 2);
    CHECK_EQ(never_rd_on_both_lines(0), true);
    self_hold_end();
    power_cycle_forget_pin();
}

// ---- A sink behind the K2 is the end of the K2's own Rd and Requests (hardware 2026-11-09, build b00178c) ----
// Hub - K2 - phone: the K2 booted with Rd on CC2 from the first write (a stale record), Requested PDO 1 at 272 ms, and went on
// Requesting on every re-announce (12 times over 3 s) until the phone's own Request collided with one. The inline latch alone was
// too slow and too narrow: the pinned boot's Rd, the self-hold Request, the VDM replies and the Rescan's CC open each looked at
// the latch only. One rule now: a sink downstream (the latch, or another sink's Request in this attach) means no Rd of ours, no
// Request of ours, no reply for the device behind us, and nothing that opens the lines.
void pixel_requests() {
    push_frame(0xE0, hdr(2, 1, false, 0), {0x1001912Cu});          // the phone's own Request (power-role bit clear), PDO 1
    push_frame(0xC0, hdr(1, 0, true, 0), {});                      // the source's GoodCRC for it
    service_ms(5);
}

void test_a_far_sinks_request_takes_the_pinned_rd_off_and_ends_the_k2s_requests() {
    const unsigned pin = self_hold_boot(0);                         // PLUS: Rd on CC2, self-hold on
    CHECK_EQ(pin, 2u);
    service_ms(3);
    g_txfifo.clear();
    send_caps_5v_9v(0);
    int passes = 0;
    while (passes < 10 && !pdo_request_on_the_wire()) { service_ms(1); ++passes; }
    CHECK_EQ(pdo_request_on_the_wire(), true);                      // before any evidence the first Request goes out: unavoidable
    CHECK_EQ(sw0_rd(), pin);
    CHECK_EQ(fusb302::sink_downstream(), false);
    pixel_requests();                                               // the evidence: another sink asks the source
    settle_ms(60);
    CHECK_EQ(fusb302::sink_downstream(), true);
    CHECK_EQ(sw0_rd(), 0u);                                         // our Rd comes off: the phone's Rd alone is on the line
    CHECK_EQ(int(fusb302::sink_status().state), int(pd::SinkState::Idle));   // the Request is the phone's now, not ours
    const std::size_t from = g_log.size();
    g_txfifo.clear();
    for (unsigned id = 1; id <= 4; ++id) {                          // the source re-announces its caps (12 Requests of ours in the capture)
        send_caps_5v_9v(id);
        service_ms(40);
    }
    CHECK_EQ(pdo_request_on_the_wire(), false);
    CHECK_EQ(fifo_loads(from), 0);
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(usb_has("[PD] Sink downstream"), true);                // the decision is in the log
    self_hold_end();
}

// Same with the inline latch alone (the Link got there first): the pinned boot's Rd is already off, and nothing is answered.
void test_a_far_sink_downstream_keeps_the_k2_off_the_line_for_vdms_and_resets() {
    self_hold_boot(0);
    service_ms(3);
    g_txfifo.clear();
    send_caps_5v_9v(0);
    service_ms(10);
    pixel_requests();
    settle_ms(60);
    const std::size_t from = g_log.size();
    g_txfifo.clear();
    push_frame(0xE0, hdr(0x0F, 1, false), {kDiscoverIdentityReq});   // the phone asks the source for its identity
    service_ms(10);
    push_frame(0xE0, hdr(0x0D, 0, false, 1), {});                    // the phone's Soft_Reset
    service_ms(10);
    push_frame(0xE0, hdr(0x0F, 1, true), {kDiscoverIdentityReq});    // the source's identity request, to the phone
    service_ms(10);
    push_frame(0xE0, hdr(8, 0, true), {});                           // Get_Sink_Cap from the source
    service_ms(10);
    CHECK_EQ(fifo_loads(from), 0);
    CHECK_EQ(sw0_rd(), 0u);
    self_hold_end();
}

// The Link holds the inline latch across a VBUS gap (kInlineHoldMs), so the driver must not drop it at a detach either: a detach
// that wrote the pin's Rd back and cleared the latch is what made the capture flap (Rd, Request, VBUS back with load, inline, Rd off,
// VBUS collapses, again). While the latch holds, the pinned Rd stays off through the detach and the scans after it.
void test_inline_survives_a_detach_and_the_pinned_rd_stays_off_until_the_link_clears_it() {
    const unsigned pin = self_hold_boot(0);
    CHECK_EQ(pin, 2u);
    service_ms(3);
    send_caps_5v_9v(0);                                             // the hub's caps: the K2 is attached
    service_ms(10);
    fusb302::set_inline(true);                                      // the Link: a load, a foreign Request
    CHECK_EQ(sw0_rd(), 0u);
    for (int i = 0; i < 10; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(100, 1500); fusb302::poll(); }   // the hub is on CC2
    g_log.clear();
    g_reg[0x40] = 0x00;                                             // the source side collapses (the hub browned out)
    for (int i = 0; i < 60; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::sink_cc(100, 3300); fusb302::poll(); }
    CHECK_EQ(usb_has("[FUSB302] Detach:"), true);                   // the detach was processed ...
    CHECK_EQ(fusb302::inline_mode(), true);                         // ... and the latch is the Link's to clear
    CHECK_EQ(sw0_rd(), 0u);
    for (const std::string& s : g_log)                              // no write in the gap ever put the pin's Rd back
        if (s.rfind("W 02=", 0) == 0) CHECK_EQ(int(std::stoul(s.substr(5), nullptr, 16) & 3u), 0);
    g_reg[0x40] = 0x20;                                             // the source is back
    for (int i = 0; i < 30; i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    CHECK_EQ(sw0_rd(), 0u);                                         // the re-attach under the latch: still no Rd
    fusb302::set_inline(false);                                     // the Link's proof: the phone is gone
    CHECK_EQ(sw0_rd(), pin);                                        // the pinned boot's Rd is back
    self_hold_end();
}

// A Refresh (rescan) or a CC open on a K2 that a sink sits behind would drop the cable's CC for 150 ms: the phone's source sees a
// detach, and a K2 living on that VBUS dies. With another sink's Request on the wire it stays passive, latch or no latch.
void test_rescan_and_cc_open_are_passive_with_a_far_sink() {
    passive_attach();
    settle_ms(100);
    far_sink_negotiates();
    CHECK_EQ(fusb302::inline_mode(), false);
    CHECK_EQ(fusb302::sink_downstream(), true);
    g_log.clear();
    fusb302::rescan();
    CHECK_EQ(find_from("PC14=0"), -1);
    CHECK_EQ(find_from("PC15=0"), -1);
    CHECK_EQ(usb_has("Rescan (inline)"), true);
    CHECK_EQ(fusb302::cc_open(150), false);
    CHECK_EQ(find_from("PC14=0"), -1);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// A far sink belongs to one attach: after the detach the next charger straight into the K2 is a sole-sink case again (the pinned Rd
// follows), unless the Link's latch is still held.
void test_the_far_sink_belongs_to_one_attach_for_the_rd_policy() {
    const unsigned pin = self_hold_boot(0);
    service_ms(3);
    send_caps_5v_9v(0);
    service_ms(10);
    pixel_requests();
    settle_ms(60);
    CHECK_EQ(fusb302::sink_downstream(), true);
    g_reg[0x40] = 0x00;                                             // everything leaves
    for (int i = 0; i < 80; i++) { g_ms += 20; fusb302::sink_vbus(0); fusb302::sink_cc(0, 0); fusb302::poll(); }
    CHECK_EQ(fusb302::sink_downstream(), false);                    // the latch was never set: nothing downstream any more
    CHECK_EQ(sw0_rd(), pin);                                        // the pinned boot's Rd is back for the next charger
    self_hold_end();
}

// A sink's Rd in the sink band on a CC line while the K2 pulls none down: the evidence the Link turns into "possibly inline" when a
// dock - K2 - laptop session is silent (hardware 2026-11-09: cc1 1676 mV against an open 3303 mV). The K2's own Rd gives no verdict.
void test_a_sink_band_level_without_rd_of_our_own_is_a_far_sink() {
    reset_world();
    g_mark = 0;
    fusb302::init();                                   // plain boot: SWITCHES0 without Rd
    fusb302::sink_cc(3303, 3303);
    CHECK_EQ(fusb302::sink_cc_far(), false);           // open Rp on both lines: a source alone
    fusb302::sink_cc(1676, 3303);
    CHECK_EQ(fusb302::sink_cc_far(), true);            // Rp 3 A into the laptop's Rd
    fusb302::sink_cc(3303, 1676);
    CHECK_EQ(fusb302::sink_cc_far(), true);            // either line
    fusb302::sink_cc(410, 3303);
    CHECK_EQ(fusb302::sink_cc_far(), false);           // default Rp into an Rd is out of the band: not enough to call it
    fusb302::sink_cc(-1, -1);
    CHECK_EQ(fusb302::sink_cc_far(), false);           // unknown is not evidence
    const unsigned pin = self_hold_boot(0);            // PLUS boot, the brick attached: the K2's own Rd on CC2
    CHECK_EQ(sw0_rd(), pin);
    fusb302::sink_cc(3303, 1676);
    CHECK_EQ(fusb302::sink_cc_far(), false);           // that level is the K2's own divider
    self_hold_end();
}

// ---- No SW_RES on a pinned or comeback boot: both Rd for 0.13 ms (roadmap 2026-11-09) ----
// init() wrote SW_RES unconditionally: on a pinned boot the CC switches are already closed and SWITCHES0 holds the
// pin (self_power_early wrote it before routing), so the soft reset restored the chip's power-on 0x03 - Rd on BOTH
// lines, the Debug Accessory signature - for the ~0.13 ms until the next SWITCHES0 write; a detach on a pinned boot
// did the same (RESET = SW_RES|PD_RESET). Walk the wire log as the chip holds it: every "W 02=" write sets SWITCHES0,
// every RESET write with SW_RES (bit 0) restores the power-on 0x03. After the first SWITCHES0 write (the power-on
// 0x03 before it is the unavoidable boot default, the CC switches still open) no value with both PDWN bits may be
// held - on the boot path, the attach and the detach alike.
bool sw0_held_rd_pair_after_first_write() {
    bool seen_write = false;
    unsigned sw0 = 0x03;                       // the chip's power-on default until the first SWITCHES0 write
    for (const std::string& s : g_log) {
        if (s.rfind("W 02=", 0) == 0) { sw0 = std::stoul(s.substr(5), nullptr, 16); seen_write = true; }
        else if (s.rfind("W 0C=", 0) == 0 && (std::stoul(s.substr(5), nullptr, 16) & 1u) != 0) sw0 = 0x03;
        if (seen_write && (sw0 & 3u) == 3u) return true;
    }
    return false;
}

void test_pinned_boot_never_soft_resets_so_no_rd_pair_at_sw_res() {
    for (int how : {0, 1, 2}) {   // 0 PLUS key mode, 1 OK key mode, 2 the pin inherited across a warm reset (a comeback)
        const unsigned pin = self_hold_boot(how);   // reset_world + self_power_early + init + an attach, log kept
        CHECK_EQ(pin == 1u || pin == 2u, true);
        // (a) boot path and attach: no SW_RES anywhere on a pinned boot, SWITCHES0 never holds both PDWN bits
        CHECK_EQ(find_from("W 0C=01"), -1);         // no SW_RES write ...
        CHECK_EQ(find_from("W 0C=03"), -1);         // ... and no SW_RES|PD_RESET either
        CHECK_EQ(sw0_held_rd_pair_after_first_write(), false);
        CHECK_EQ(int(g_reg[0x02] & 3), int(pin));
        // (b) a detach on the pinned boot: the detach's chip reset is PD_RESET only, the pin never leaves
        const std::size_t from = g_log.size();
        g_reg[0x40] = 0x00;                         // the charger goes quiet: no Rp on the active line
        for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::sink_vbus(5000); fusb302::poll(); fusb302::service(); }
        CHECK_EQ(usb_has("[FUSB302] Detach:"), true);
        CHECK_EQ(find_from("W 0C=01", from), -1);
        CHECK_EQ(find_from("W 0C=03", from), -1);
        CHECK_EQ(sw0_held_rd_pair_after_first_write(), false);   // the whole log: boot, attach and detach
        CHECK_EQ(int(g_reg[0x02] & 3), int(pin));   // the pinned Rd is what the chip holds after the detach
        self_hold_end();                            // ends with a detach and no pin (the statics leak between cases)
    }
}

// The over-load gate's far-PD-message half (k2/pps-overload review, finding 1): a far sink's control
// message on SOP (power-role bit clear - its Get_Source_Cap, its GoodCRC) is a PD message from it, so
// over_load_facts().far_pd_message must be true and pd::over_load_allowed() must refuse. The source's
// own control messages (bit 8 set) are not the sink's, and a new attach forgets the old sink's messages.
void test_a_far_sinks_control_message_is_a_far_pd_message_for_the_over_load_gate() {
    passive_attach();
    settle_ms(100);
    fusb302::sink_vbus(5000);
    fusb302::sink_cc(1676, 3303);              // a sink's Rd on CC1, none of ours on any line
    fusb302::poll();
    pd::OverLoadFacts f = fusb302::over_load_facts();
    CHECK_EQ(f.far_sink, true);
    CHECK_EQ(f.far_pd_message, false);         // nothing from the sink yet: a passive load
    CHECK_EQ(int(pd::over_load_allowed(f)), 1);
    push_frame(0xE0, hdr(7, 0, false), {});    // the far sink's Get_Source_Cap (control, power-role clear)
    service_ms(10);
    f = fusb302::over_load_facts();
    CHECK_EQ(f.far_sink, true);
    CHECK_EQ(f.far_pd_message, true);          // RED before the fix: control messages were recorded nowhere
    CHECK_EQ(int(pd::over_load_allowed(f)), 0);
    push_frame(0xE0, hdr(1, 0, false), {});    // its GoodCRC counts too
    service_ms(10);
    CHECK_EQ(fusb302::over_load_facts().far_pd_message, true);
    g_reg[0x40] = 0x00;                        // the source leaves: a detach
    settle_ms(2000);
    CHECK_EQ(fusb302::sink_cc_far(), false);   // the far-sink CC reading is not stale-true after the detach
    g_reg[0x40] = 0x20;                        // a charger is back: a new attach
    for (int i = 0; i < 100 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    fusb302::sink_vbus(5000);
    fusb302::sink_cc(1676, 3303);
    fusb302::poll();
    push_frame(0xE0, hdr(1, 0, true), {});     // the source's GoodCRC (power-role bit set)
    push_frame(0xE0, hdr(3, 0, true, 1), {});  // the source's Accept
    service_ms(10);
    f = fusb302::over_load_facts();
    CHECK_EQ(f.far_sink, true);
    CHECK_EQ(f.far_pd_message, false);         // a new attach forgot the old sink's messages
    CHECK_EQ(int(pd::over_load_allowed(f)), 1);
    clean_up_rd_tests();
}

// ---- The lab Rd pin (pd-reread --pin) and the far end's detach ----
// The pin is a lab option and does what it says; the gap is that nothing showed it was still set, and it
// outlived the far end. When the inline sink (the far end) detaches, the LAB pin clears with it, pin mark
// included, so the next attach is passive. A boot / comeback pin is not a lab pin: it keeps its Rd (it is
// what powers the K2) and must not fight the comeback marker.
void test_lab_pin_clears_when_the_far_end_detaches() {
    reset_world();
    g_mark = 0;
    fusb302::self_power_early(bootmode::Mode::Plain, false);   // HID-powered plain boot
    fusb302::init();
    CHECK_EQ(fusb302::debug_pin(1), true);                     // pd-reread --pin 1: the lab pin
    CHECK_EQ(int(fusb302::pin_rd()), 1);
    CHECK_EQ(int(g_mark), int(bootmode::pin_mark(0x01)));      // the pin mark, so a warm reset inherits it
    g_reg[0x40] = 0x20;                                        // STATUS0.COMP: Rp on the measured line
    for (int i = 0; i < 60 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(usb_has("Active CC detected"), true);
    const int cc = int(fusb302::active_cc());
    CHECK_EQ(cc == 1 || cc == 2, true);
    auto sink_on_active = [&](int mv) { if (cc == 1) fusb302::sink_cc(mv, 100); else fusb302::sink_cc(100, mv); };
    fusb302::set_inline(true);                                 // the far end (a real sink) is attached
    for (int i = 0; i < 10; i++) { g_ms += 20; fusb302::sink_vbus(5000); sink_on_active(1500); fusb302::poll(); }
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::sink_vbus(5000); sink_on_active(3300); fusb302::poll(); }
    CHECK_EQ(usb_has("the sink left"), true);                  // DetachReason::Sink: the far end detached
    CHECK_EQ(int(fusb302::pin_rd()), 0);                       // the lab pin cleared with it
    CHECK_EQ(int(g_mark), 0);                                  // and its mark, so no warm reset re-inherits it
    CHECK_EQ(usb_has("lab pin cleared"), true);
    fusb302::set_inline(false);
    end_start_test();
    power_cycle_forget_pin();
}

void test_a_boot_pin_survives_the_far_end_detaching() {
    const unsigned pin = self_hold_boot(0);                    // PLUS boot: a boot pin on CC2, not a lab pin
    CHECK_EQ(pin, 2u);
    CHECK_EQ(int(fusb302::pin_rd()), 2);
    const int cc = int(fusb302::active_cc());
    auto sink_on_active = [&](int mv) { if (cc == 1) fusb302::sink_cc(mv, 100); else fusb302::sink_cc(100, mv); };
    fusb302::set_inline(true);
    for (int i = 0; i < 10; i++) { g_ms += 20; fusb302::sink_vbus(5000); sink_on_active(1500); fusb302::poll(); }
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::sink_vbus(5000); sink_on_active(3300); fusb302::poll(); }
    CHECK_EQ(usb_has("the sink left"), true);
    CHECK_EQ(int(fusb302::pin_rd()), 2);                       // a boot / comeback pin keeps its Rd
    CHECK_EQ(int(g_mark), int(bootmode::pin_mark(0x02)));      // and its mark (the comeback marker is untouched)
    fusb302::set_inline(false);
    end_start_test();
    power_cycle_forget_pin();
}

// ---- inline TX refusals: an inline K2 never transmits, confirmed or not ----
// 2026-11-10 (ROADMAP "Sink Identify inline drops the phone"): a confirmed inline Identify spoke as a
// second sink on a wire whose partners already had a contract; both answered, GoodCRC/MessageIDs
// collided and one side Hard Reset the link, dropping the phone to 5 V. Every active path (Identify,
// Get_Source_Cap, the VCONN cable read over SOP', the VDM probe) must refuse inline even with
// confirm=true, and nothing may reach the TX FIFO. These cases go last: they end with a detach.

// A clean attach on CC1 with the inline latch set, and nothing logged yet.
void attach_inline() {
    fusb302::set_inline(false);
    reset_world();
    fusb302::init();
    g_reg[0x40] = 0x00;                                  // nothing on the line: detach whatever the last case left
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    fusb302::set_inline(false);
    reset_world();
    fusb302::init();
    g_reg[0x40] = 0x20;                                  // a source's Rp on CC1
    for (int i = 0; i < 60 && !usb_has("Active CC detected"); i++) { g_ms += 20; fusb302::poll(); }
    CHECK_EQ(fusb302::active_cc() != 0, true);
    fusb302::set_inline(true);
    g_log.clear(); g_usb.clear();
}

void end_inline_case() {
    g_reg[0x40] = 0x00;                                  // the line goes quiet: the 500 ms rule detaches
    for (int i = 0; i < 40; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    fusb302::set_inline(false);
}

// No frame on the wire after 600 ms of polling: neither a TX-FIFO write nor TX_START.
void check_no_tx() {
    for (int i = 0; i < 30; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(find_from("FIFO"), -1);                     // no frame was ever queued
    CHECK_EQ(find_from("W 06=01"), -1);                  // ... and TX_START never went out
}

// The lab-only `pd reset --inline --confirm` (remote, pd::lab_reset_verdict): its one Hard Reset reaches the wire while inline;
// the plain tool stays refused inline. Hardware 2026-11-10 (CUKTECH -> K2 -> Pixel): the remote said ok, VBUS never moved -
// the driver dropped the lab reset at its own inline guard.
void test_inline_hard_reset_only_with_the_lab_flag() {
    attach_inline();
    CHECK_EQ(fusb302::sink_hard_reset(), false);         // the plain tool: refused inline, nothing on the wire
    for (int i = 0; i < 10; i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(hard_reset_written(), false);
    CHECK_EQ(fusb302::sink_hard_reset(true), true);      // the lab override: exactly this one goes out
    for (int i = 0; i < 10 && !hard_reset_written(); i++) { g_ms += 20; fusb302::poll(); fusb302::service(); }
    CHECK_EQ(hard_reset_written(), true);
    end_inline_case();
}

void test_inline_identify_refused_even_confirmed() {
    attach_inline();
    CHECK_EQ(fusb302::identify_start(true), false);      // confirm must not override the inline refusal
    CHECK_EQ(usb_has("refused"), true);
    check_no_tx();
    end_inline_case();
}

void test_inline_get_source_caps_refused_even_confirmed() {
    attach_inline();
    CHECK_EQ(fusb302::get_source_caps(true), false);
    check_no_tx();
    CHECK_EQ(usb_has("sending Get_Source_Cap"), false);
    end_inline_case();
}

void test_inline_vconn_scan_refused_even_confirmed() {
    attach_inline();
    g_cable.cc = 1;
    CHECK_EQ(fusb302::read_emarker(true, pd::EmarkerMode::Vconn), false);
    CHECK_EQ(g_cable.tx_seen, 0);
    CHECK_EQ(find_from("PC13=1"), -1);                   // VCONN never went on
    CHECK_EQ(usb_has("Refused"), true);
    end_inline_case();
}

void test_inline_cable_session_refused_even_confirmed() {
    attach_inline();
    SessionProbe s;
    g_cable.vendor = true;
    CHECK_EQ(fusb302::cable_session(true, &session_body, &s), false);   // the cable read over SOP' is refused too
    CHECK_EQ(s.calls, 0);
    CHECK_EQ(g_cable.tx_seen, 0);
    CHECK_EQ(find_from("PC13=1"), -1);
    end_inline_case();
}

void test_inline_vdm_probe_refused() {
    attach_inline();
    CHECK_EQ(fusb302::probe_start(), false);             // the VDM probe sequencer: already refused inline, locked in
    check_no_tx();
    end_inline_case();
}

void test_sniffed_caps_while_inline_are_stored_and_a_capless_request_is_never_guessed();
void test_request_period_is_tracked_and_reset_with_the_attach();
// ---- Settings > Wake sleeping port (pattern slow-rp-wake, idle path): opt-in, default off ----
// A sleeping source's port reads ~3.3 V open on one line (CC2 here), nothing on the other, VBUS dark. With the setting off the
// idle K2 stays exactly what the invariants say (I1/H2: no Rd at idle, a waiting charger stays passive); only with it on does it
// hold its one Rd on that line for the dwell.
void idle_wake_world() {
    power_cycle_forget_pin();
    fusb302::set_force_line(0);
    reset_world();
    g_line_mv = 0;                                           // nothing the passive scan could attach to
    fusb302::init();
    fusb302::set_proactive_discover(false);
    g_auto_txsent = true;
}
// Idle for `ms` with the standby level on CC2; returns the union of the Rd bits SWITCHES0 showed after any tick.
unsigned idle_rd_union(unsigned ms) {
    unsigned seen = 0;
    for (unsigned t = 0; t < ms; t += 20) {
        g_ms += 20; fusb302::sink_vbus(78); fusb302::sink_cc(0, 3300); fusb302::poll(); fusb302::service();
        seen |= sw0_rd();
    }
    return seen;
}
void test_idle_wake_off_by_default_never_presents_rd_on_a_standby_line() {
    idle_wake_world();
    CHECK_EQ(fusb302::idle_wake(), false);
    CHECK_EQ(idle_rd_union(6000), 0u);                       // I1 / H2: no Rd on either line, for as long as it idles
    CHECK_EQ(find_from("Idle wake"), -1);
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(never_rd_on_both_lines(0), true);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
void test_idle_wake_on_holds_one_rd_on_the_standby_line_and_off_releases_it() {
    idle_wake_world();
    fusb302::set_idle_wake(true);
    CHECK_EQ(fusb302::idle_wake(), true);
    const unsigned seen = idle_rd_union(500);
    CHECK_EQ(seen, 2u);                                      // Rd on CC2 only, never a pair
    for (int i = 0; i < 90; i++) {                           // a steady hold: the Rd is not blinking inside the dwell
        g_ms += 20; fusb302::sink_vbus(78); fusb302::sink_cc(0, 3300); fusb302::poll(); fusb302::service();
        CHECK_EQ(sw0_rd(), 2u);
    }
    CHECK_EQ(never_rd_on_both_lines(0), true);
    CHECK_EQ(usb_has("Idle wake: Rd on CC2"), true);
    fusb302::set_idle_wake(false);                           // off again: the hold ends at once, no reboot needed
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(idle_rd_union(3000), 0u);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
void test_idle_wake_off_after_on_leaves_nothing_behind() {
    idle_wake_world();
    fusb302::set_idle_wake(true);
    idle_rd_union(400);
    fusb302::set_idle_wake(false);
    idle_rd_union(3000);
    CHECK_EQ(sw0_rd(), 0u);
    CHECK_EQ(never_rd_on_both_lines(0), true);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

int main() {
    test_run_rd_survives_the_passive_scan();
    test_trigger_refused_after_a_foreign_request_without_inline();   // first: g_sink keeps state across tests
    test_cable_on_cc1_orientation_a();
    test_cable_on_cc2_orientation_b();
    test_pd2_only_cable_answers_the_retry();
    test_no_cable_scans_both_orientations_and_restores();
    test_vconn_needs_pc13_and_the_far_line();
    test_inline_gate_refuses_unconfirmed_vconn();
    test_sniff_never_touches_cc_or_vconn();
    test_failed_scan_restores_the_pre_scan_state();
    test_detach_on_cc_loss_after_500ms();
    test_detach_on_low_vbus_after_1s();
    test_vbus_zero_throughout_is_no_detach();
    test_inline_drops_rd_and_detects_the_sink_leaving();
    test_inline_never_seeing_a_sink_is_not_a_detach();
    test_typec_only_source_after_one_second_without_caps();
    test_no_typec_only_with_pd_traffic_or_inline();
    test_inline_rd_stays_off_after_a_detach_then_comes_back();
    test_inline_hold_ends_with_rd_back_even_when_nothing_is_there();
    test_source_alone_gets_rd_back_after_a_detach();
    test_good_vbus_and_rp_keep_the_attach();
    test_rd_window_recheck_moves_a_wrongly_latched_line_on_fresh_levels();
    test_rd_window_recheck_ignores_ambiguous_levels_and_a_forced_line();
    test_rd_window_recheck_never_runs_after_pd_traffic();
    test_vconn_level_line_is_not_an_active_cc_and_detach_never_soft_resets();
    test_rescan_is_only_the_cc_switch_cycle();
    test_service_drains_the_fifo_past_the_four_packet_pass();
    test_passive_30v_physical_identity();
    test_passive_physical_identity_and_source_revision_provenance();
    test_sink_request_goes_out_as_sop_not_sop_prime();
    test_run_start_presents_rd();
    test_run_start_rd_pause_keeps_the_scanned_cable();
    test_scanned_cable_survives_the_attach_after_rd();
    test_rp_on_line_sees_a_source_and_leaves_the_chip_alone();
    test_start_requests_pdo1_a_few_ms_after_the_caps_when_poll_attached();
    test_start_wait_loop_attaches_and_requests_without_poll();
    test_hard_reset_latched_by_the_post_packet_read_is_handled();
    test_hard_reset_seen_while_waiting_for_txsent_ends_the_wait();
    test_legacy_only_is_pd_silent();
    test_reread_abort_drops_the_contract_and_rd();
    test_reread_end_drops_the_contract_and_rd_after_the_linger();
    test_reread_release_leaves_a_held_contract_alone();
    test_reread_step_survives_a_tx_timeout_when_the_answer_arrives();
    test_ra_mode_holds_pc13_high_with_rd_on_cc2_only();
    test_ra_mode_refusals_and_explicit_off();
    test_unit_detach_drops_rd_with_the_spoof_armed();
    test_armed_spoof_alone_never_presents_rd();
    test_released_contract_lingers_then_drops_rd();
    test_released_contract_with_the_spoof_armed_drops_rd_keeps_crc();
    test_held_contract_keeps_rd();
    test_reread_abort_after_a_forced_line_releases_rd();
    test_reread_abort_with_the_spoof_armed_releases_rd();
    test_ra_off_then_abort_releases_rd();
    test_cc_detach_drops_rd_for_longer_than_the_debounce();
    test_cc_detach_refused_inline();
    test_trigger_on_the_rereads_own_pdo_stays_held();
    test_trigger_on_another_pdo_after_a_reread_stays_held();
    test_cable_session_exchanges_run_with_vconn_up_and_restore_after();
    test_cable_session_without_a_cable_never_calls_the_body();
    test_cable_session_is_refused_like_the_scan();
    test_hid_cold_boot_is_passive_before_the_main_loop();      // last: they start from self_power_early and end with no pin
    test_hid_warm_reset_inherits_no_rd();
    test_hid_reread_abort_releases_rd();
    test_hid_reboot_during_a_reread_is_a_detach();
    test_charger_powered_pin_is_marked_and_survives_a_warm_reset();
    test_key_mode_never_presents_rd_on_both_lines();           // last: they start from self_power_early and end with no pin
    test_a_stale_pair_mark_boots_passive();
    test_self_power_move_moves_the_single_rd_once();
    test_acked_caps_get_a_request_when_a_reread_attaches_on_a_known_line();
    test_unacked_caps_stay_unanswered_in_the_passive_idle_path();
    test_unit_hold_keeps_the_pinned_rd_and_never_opens_cc();
    test_self_hold_boots_take_the_5v_contract_as_soon_as_caps_arrive();
    test_self_hold_source_never_reaches_its_give_up();
    test_self_hold_unanswered_request_never_sends_a_hard_reset();
    test_self_hold_source_without_caps_leaves_the_k2_silent();
    test_plain_boot_is_unchanged_by_self_hold();
    test_self_hold_yields_to_a_legacy_unit();
    test_self_hold_contract_then_unit_hold_soft_resets_from_the_live_contract();
    test_boot_ra_before_the_attach_then_the_5v_contract();
    test_cable_read_listens_on_sop_prime_and_sop_pp_with_auto_crc_on();
    test_sop_pp_frames_reach_the_hook_with_their_sop();
    test_legacy_unit_attach_is_pd_silent_with_cable_off();
    test_legacy_unit_attach_is_pd_silent_under_a_spoofed_run();
    test_passive_idle_stays_silent_through_a_source_hard_reset();
    test_finished_run_stays_silent_through_a_source_hard_reset();
    test_released_trigger_stays_silent_through_a_source_hard_reset();
    test_finished_reread_stays_silent_through_a_source_hard_reset();
    test_aborted_reread_before_any_contract_stays_silent_through_a_source_hard_reset();
    test_held_trigger_still_resumes_after_a_source_hard_reset();
    test_release_after_a_source_hard_reset_cancels_the_resume();
    test_a_far_sinks_identity_request_and_soft_reset_get_no_sop_from_the_passive_k2();
    test_the_inline_latch_alone_silences_the_identity_and_soft_reset_replies();
    test_the_k2_as_the_only_sink_still_answers_identity_and_soft_reset();
    test_a_new_attach_forgets_the_far_sink();
    test_passive_cold_boot_never_transmits_on_a_live_link();
    test_comeback_boot_never_writes_a_rd_less_switches0_and_has_the_ra_first();
    test_comeback_release_gives_the_pin_and_the_early_ra_back();
    test_rd_glitch_drops_the_rd_keeps_the_ra_and_restores_the_pin();
    test_seek_moves_the_rd_to_the_line_that_answers_and_keeps_the_attach();   // last: each one ends with a detach / Rd off
    test_seek_move_is_an_attach_not_a_detach();
    test_seek_is_bounded_and_ends_in_the_ordinary_detach();
    test_seek_ignores_an_attach_that_was_picked_with_rd_on();
    test_seek_never_runs_inline_in_the_ra_mode_or_on_a_forced_line();
    test_seek_never_moves_a_pinned_rd();
    test_a_far_sinks_request_takes_the_pinned_rd_off_and_ends_the_k2s_requests();   // last: each one ends with a detach / Rd off
    test_a_far_sink_downstream_keeps_the_k2_off_the_line_for_vdms_and_resets();
    test_inline_survives_a_detach_and_the_pinned_rd_stays_off_until_the_link_clears_it();
    test_rescan_and_cc_open_are_passive_with_a_far_sink();
    test_the_far_sink_belongs_to_one_attach_for_the_rd_policy();
    test_a_sink_band_level_without_rd_of_our_own_is_a_far_sink();
    test_pinned_boot_never_soft_resets_so_no_rd_pair_at_sw_res();   // last: ends with a detach / Rd off
    test_a_far_sinks_control_message_is_a_far_pd_message_for_the_over_load_gate();   // last: ends with a detach / reset
    test_lab_pin_clears_when_the_far_end_detaches();            // last: each one ends with a detach / Rd off
    test_a_boot_pin_survives_the_far_end_detaching();
    test_inline_identify_refused_even_confirmed();          // last: each inline case ends with a detach
    test_inline_hard_reset_only_with_the_lab_flag();
    test_inline_get_source_caps_refused_even_confirmed();
    test_inline_vconn_scan_refused_even_confirmed();
    test_inline_cable_session_refused_even_confirmed();
    test_inline_vdm_probe_refused();
    test_sniffed_caps_while_inline_are_stored_and_a_capless_request_is_never_guessed();   // last: ends with a detach
    test_request_period_is_tracked_and_reset_with_the_attach();                           // last: ends with a detach
    test_idle_wake_off_by_default_never_presents_rd_on_a_standby_line();   // last: each one ends with the setting off, a detach / Rd off
    test_idle_wake_on_holds_one_rd_on_the_standby_line_and_off_releases_it();
    test_idle_wake_off_after_on_leaves_nothing_behind();
    return test_report("test_fusb302_scan");
}

// ROADMAP "PD 0V5.82A" row: Source_Capabilities sniffed on an inline link are stored where the home
// model and state.num_pdos read them, and a Request decoded without its caps is never guessed (no
// voltage, no current - on hardware the PPS bits read as a 5.82 A "max current").
void test_sniffed_caps_while_inline_are_stored_and_a_capless_request_is_never_guessed() {
    passive_attach();
    settle_ms(100);
    fusb302::set_inline(true);
    push_frame(0xE0, hdr(2, 1, false), {0x1001912Cu});                     // the phone's Request, no caps yet
    service_ms(10);
    CHECK_EQ(fusb302::contract().valid, true);
    CHECK_EQ(fusb302::contract().mv, 0);                                   // unknown: never a guessed layout
    CHECK_EQ(fusb302::contract().max_ma, 0);
    push_frame(0xE0, hdr(1, 2, true), {0x0001912Cu, 0x0002914Bu});         // Source_Capabilities sniffed inline
    service_ms(10);
    CHECK_EQ(fusb302::caps().valid, true);                                 // stored where state.num_pdos reads it
    CHECK_EQ(fusb302::caps().num_pdos, 2);
    push_frame(0xE0, hdr(2, 1, false), {0x1001912Cu});                     // the next Request decodes with the caps
    service_ms(10);
    CHECK_EQ(fusb302::contract().mv, 5000);
    CHECK_EQ(fusb302::contract().max_ma, 3000);
    fusb302::set_inline(false);
    clean_up_rd_tests();
    power_cycle_forget_pin();
}

// The Request period feeds the PPS keep-alive label on the home footer (<= 10 s with no caps).
void test_request_period_is_tracked_and_reset_with_the_attach() {
    passive_attach();
    settle_ms(100);
    push_frame(0xE0, hdr(2, 1, false), {0x1001912Cu});
    service_ms(1);
    CHECK_EQ(fusb302::request_period_ms(), 0);                             // one Request: no period yet
    g_ms += 500;
    push_frame(0xE0, hdr(2, 1, false), {0x1001912Cu});
    service_ms(1);
    CHECK_EQ(fusb302::request_period_ms() >= 500 && fusb302::request_period_ms() <= 520, true);
    g_reg[0x40] = 0x00;                                                    // the source leaves: detach
    settle_ms(2000);
    CHECK_EQ(fusb302::request_period_ms(), 0);                             // a new attach starts over
    clean_up_rd_tests();
    power_cycle_forget_pin();
}
