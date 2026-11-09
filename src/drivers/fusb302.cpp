// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// FUSB302 driver, see k2/drivers/fusb302.hpp. Register values follow the ON Semiconductor datasheet;
// the e-marker read is SOP' Discover Identity with VCONN.
//
// Boot is passive: Auto-CRC off, no Rd
// change, SOP/SOP'/SOP'' receive on (pd_phy.hpp). Auto-CRC and transmit are switched on only
// while an active operation runs (AutoCrcGate), then the chip goes back to passive. The frames
// this driver can put on CC, all on explicit request from the UI or the remote and never while
// inline without a confirm:
//  - the e-marker SOP' Discover Identity (scan_orientation(), with VCONN applied);
//  - what the pure sink engine (k2/pd/pd_sink.hpp) asks for: Request / EPR_Request, EPR_Mode, chunk
//    requests, keep-alives, Get_Source_Cap, hard reset;
//  - what the pure Identify sequencer (k2/pd/pd_identify.hpp) asks for: Discover Identity / SVIDs /
//    Modes, Get_Sink_Cap, Get_Status, Get_Revision.
// This file only owns the wire: token stream, MessageID, TXSENT / RETRYFAIL, hard reset ordered set.
// Inline (set_inline(true), decided by pd::Link from the measured current and the sink's Request)
// only gates the active tools: they refuse, or need a confirm. It does not touch the chip: the
// passive profile never ACKs anyway.
// The auto fast-charge detector is owned by the fast-charge work (docs/FASTCHARGE_PROTOCOLS.md), not by this driver.
#include "k2/core/log.hpp"
#include "k2/drivers/fusb302.hpp"

#include <array>
#include <span>

#include "k2/drivers/i2c.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/caps_log_gate.hpp"
#include "k2/pd/emk_ra.hpp"
#include "k2/pd/pd_ccscan.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_probe.hpp"
#include "k2/pd/pd_reread.hpp"
#include "k2/pd/pd_passive.hpp"
#include "k2/pd/pd_phy.hpp"
#include "k2/pd/pd_rescan.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_vconn.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/pin_mark.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::fusb302 {

namespace {

using i2c::Bus;
using systick::delay_ms;

constexpr std::uint8_t kAddr = 0x22;

namespace phy = pd::phy;
using namespace pd::phy;

constexpr std::uint8_t kIntHardRst = 1u << 0, kIntTxSent = 1u << 2, kIntHardSent = 1u << 3,
                       kIntRetryFail = 1u << 4;                       // INTERRUPTA
constexpr std::uint8_t kComp = 1u << 5;                              // STATUS0: CC above MDAC threshold
constexpr std::uint8_t kRxEmpty = 1u << 5;                           // STATUS1

std::uint8_t g_device_id, g_active_cc;
// INTERRUPTA clears on read, and clear_irqs() / transmit() read it for other reasons. A Hard Reset from the source that
// they happen to catch is kept here for service() (Baseus 140W, Black Box 2026-11-04: it fell in the read after the caps,
// was lost, and the Request that followed went out on a link the source had already torn down).
bool g_hr_latched = false;
std::uint8_t g_cc_mask = 3; // CC analog switches closed outside VCONN scans: bit0 = PC14, bit1 = PC15
pd::DetachTracker g_detach;
std::uint32_t g_requests_base = 0;
bool g_pd_traffic = false;           // any PD message (any SOP) seen since attach
bool g_pd_seen_attach = false;       // the same, but only a new attach clears it (a unit hold / start-up does not)
std::uint32_t g_ident_session = 0;   // attach session of the latest identify_start (its Sink_Caps belong to it)   // contract.requests at attach: later ones came from a sink on the far side
std::uint32_t g_attach_ms = 0;   // millis() at the attach of the current partner
std::uint8_t g_bc_max = 0;       // highest BC_LVL (Rp level) read since attach
volatile bool g_irq = false;  // set by the EXTI12 ISR (INT_N fell), cleared by service()
std::int32_t g_vbus_mv = -1;  // last VBUS reading from sink_vbus(); -1 = unknown
bool g_new_source = false;  // next Source_Capabilities on SOP starts a fresh source
std::int32_t g_cc_sink_mv = -1;  // the CC voltage closest to the sink-Rd band (-1 unknown)
std::int32_t g_cc1_mv = -1, g_cc2_mv = -1;  // latest ADC net levels from sink_cc(); -1 = unknown
std::array<pd::CcSample, 6> g_scan_samples{};   // the last attach scan's comparator samples, for the Rd-window re-check
unsigned g_scan_sample_n = 0;
std::uint32_t g_cc_recheck_at = 0;   // rate limit of the Rd-window re-check (kCcRecheckMinMs)
constexpr std::uint32_t kCcRecheckMinMs = 500;
std::uint8_t g_pin_rd = 0;  // self-powered mode: the Rd bits SWITCHES0 keeps while no partner is attached (0 = free)
std::uint8_t g_boot_pin_rd = 0;   // the boot mode's Rd bits, restored after a run's detach (self_power_restore)
bool g_rd_inherited = false;      // the pin was kept across a warm reset by the pin mark (charger-powered K2), not set by a key mode
bool g_pin_moved = false;         // the key mode's single Rd already moved to the other line this boot (self_power_move)
// The boot pin and its mark change together: the mark (a backup register) is what tells the next boot after a warm reset that
// this K2 hangs on its own Rd (charger-powered). Whoever changes g_boot_pin_rd goes through here.
void set_boot_pin(std::uint8_t pin) {
    g_boot_pin_rd = pin;
    pinmark::write(bootmode::pin_mark(pin));
}
pd::RdHold g_rd_hold;       // Rd off after a detach
bool g_rd_op = false;     // an explicit active feature presents Rd on the active line (plain boot: otherwise never)
bool g_start_requested = false;   // the run start already asked the source for PDO 1 (once per Rd presentation)
bool g_rd_start = false;    // a run start presents Rd (no sink inline): the line with Rp, both while none is detected
bool g_inline = false;      // a real sink is attached: active operations refuse or need a confirm
pd::AutoCrcGate g_crc;      // Auto-CRC only while an active operation runs (the armed spoof counts)
pd::AutoCrcGate g_rd_gate;  // Rd on the active line only while a sink operation runs (the armed spoof does not count)
pd::ContractLinger g_contract_linger;   // a contract nobody holds times out after kSinkIdleLingerMs
bool g_legacy_only = false;  // charger test: no PD sink policy, no rescan
bool g_crc_on = false;      // what SWITCHES1 holds
bool g_tx_allowed = false;  // passive by default: transmit() / send_sop_prime_frame() refuse every frame while no
                            // active feature demands the wire (updated from the pd::CrcDemand in active_tick())
bool g_hr_tool_armed = false;  // the explicit sink_hard_reset() tool: one Hard Reset may go out even from idle
bool g_cable_read = false;  // e-marker read in progress: CONTROL1 listens on SOP' and SOP'' even with Auto-CRC on
pd::TriggerPrep g_prep;     // re-announce before a trigger when the last caps went un-ACKed
pd::Choice g_prep_choice;   // the trigger waiting for the prepare to finish
pd::CapsSession g_session;         // attach/detach counter; caps are valid only in the session they arrived in
pd::CapsLogGate g_caps_log;        // keeps a repeating source's caps from flooding the K2LOG ring
std::uint32_t g_caps_count = 0;  // Source_Capabilities messages received since boot
bool g_caps_acked = false;       // the last of them arrived with Auto-CRC on (the source got our GoodCRC)
pd::Engine g_engine;  // all frame decoding lives in the pure engine
pd::CableLimits g_physical_cable;
std::uint32_t g_physical_session = 0;
pd::CableLimits g_scanned_cable;   // the VCONN scan before a run start (no source was there to ask the cable)
bool g_scanned_valid = false;
pd::Sink g_sink;      // transmit-side state machines are pure too; this file only sends their messages
pd::Identify g_ident;
bool g_probe_logged = true;
pd::Probe g_probe;   // report-card source probe: never retries, ends within kProbeTotalMs
bool g_reread_logged = true;
pd::Reread g_reread;   // source-cap re-read (after-contract conversation); never retries, ends within kRereadTotalMs
bool g_reread_owns_rd = false;            // the re-read's attach presented Rd (rd_for_run) and gives it back; any other owner clears this
bool g_reread_made_contract = false;      // the re-read's attach made the contract (Request PDO 1) and ends it; any other owner clears this
bool g_trigger_took_over = false;         // a user trigger (sink_request) holds the re-read's contract: its Rd stays until the trigger ends
std::uint32_t g_reread_release_at = 0;    // when the owned Rd / contract goes back after a finished run (linger: the contract stays readable)
std::uint8_t g_force_cc = 0;              // debug (pd-reread --line N): the only line the attach scan may take; 0 = normal; stays until cleared (pd-reread --abort / --line 0) or reboot
constexpr std::uint32_t kRereadLingerMs = 30000;
SopPrimeHook g_sop_prime_hook = nullptr;  // k2::emk_spoof, null when off
pd::Vconn g_vconn;                // VCONN_Swap experiment (remote only): the K2 takes the VCONN source role
bool g_vconn_applied = false;     // PC13 high and the chip's VCONN switch closed on the line opposite the active one
bool g_ra = false;                // Ra mode (k2/pd/emk_ra.hpp): PC13 high = Ra on CC1, Rd on CC2 alone; ends on detach, abort, `ra off`, reboot
bool g_ra_run = false;            // a charger-test run owns the Ra mode: its units detach and re-attach, the mode survives them
std::uint32_t g_vconn_on_ms = 0;  // when it went on; the settled reading is logged ~5 ms later
bool g_vconn_logged = true;
std::uint8_t g_msg_id;            // MessageID of the next message we send (3 bits)
std::uint32_t g_last_rx_ms;       // last frame heard: the line counts as idle ~50 ms later
bool g_rx_empty = true;
bool g_getcap_force = false;      // the Get_Source_Cap was asked for, not just a rescan fallback
bool g_getcap_armed = false;      // send one Get_Source_Cap when due (on request)
std::uint32_t g_getcap_at;
constexpr std::uint32_t kIdleQuietMs = 50;

// The first kBootNotes SWITCHES0 writes and CC-switch routings after reset, with millis() (u16, wraps at 65 s, plenty for the
// boot window): the black box does not exist yet when self_power_early() runs, so this is the only record of what the charger
// was shown in the first milliseconds. Logged once by poll() and returned by boot_note_at() (k2/core/bootinfo.hpp).
// BootNote kind 0 = SWITCHES0 write, 1 = CC routing (bit0 PC14, bit1 PC15, bit2 PC13).
constexpr unsigned kBootNotes = 24;
constexpr std::uint32_t kBootNoteSpanMs = 3000;
std::array<BootNote, kBootNotes> g_boot_notes{};
unsigned g_boot_note_n = 0, g_boot_note_lost = 0;
// SWITCHES0 as the chip holds it, read at ~50 ms, ~600 ms and ~3 s after reset (kind 2 notes): the write record says what we
// sent, this says what the chip kept (a brown-out or a chip reset between the two would show here).
constexpr std::array<std::uint16_t, kBootReadbacks> kReadbackAtMs = {50, 600, 3000};
std::array<BootNote, kBootReadbacks> g_readbacks{};
unsigned g_readback_n = 0;
std::uint8_t g_initial_pin_rd = 0;   // the Rd bits self_power_early() wrote first
bool g_mark_read = false;            // the pin mark was consulted (plain boot after a warm reset)
std::uint16_t g_mark_raw = 0;
void boot_note(std::uint8_t kind, std::uint8_t val) {
    const std::uint32_t t = systick::millis();
    if (t >= kBootNoteSpanMs) return;
    if (g_boot_note_n > 0 && g_boot_notes[g_boot_note_n - 1].kind == kind && g_boot_notes[g_boot_note_n - 1].val == val) return;
    if (g_boot_note_n < kBootNotes) g_boot_notes[g_boot_note_n++] = {std::uint16_t(t), kind, val};
    else g_boot_note_lost++;
}

// Every register write goes to the black box (k2/pd/pdtrace.hpp) with its value and bus result.
bool wr(Reg r, std::uint8_t v) {
    const bool ok = i2c::write_reg(Bus::Fusb, kAddr, r, v);
    pdtrace::reg_write(r, v, ok);
    if (r == kSwitches0) boot_note(0, v);
    return ok;
}
bool rd(Reg r, std::uint8_t& v) { return i2c::read_reg(Bus::Fusb, kAddr, r, v); }
bool fifo_read(std::span<std::uint8_t> out) { return i2c::read(Bus::Fusb, kAddr, kFifos, out); }
// The only path that loads the TX FIFO (transmit() and scan_orientation()): the black box records
// the PD header and data objects given here, then the token stream goes to the chip.
// SOP kind for the trace record, from the leading FIFO tokens: 0x12 0x12 0x12 0x13 = SOP,
// 0x12 0x12 0x1B 0x1B = SOP' (the e-marker reads and the spoof replies). RX-style token values,
// the same encoding the records of received frames carry (0xE0 SOP, 0xC0 SOP').
std::uint8_t tx_sop_token(std::span<const std::uint8_t> stream) {
    if (stream.size() >= 4 && stream[2] == 0x1B && stream[3] == 0x1B) return 0xC0;
    return 0xE0;
}
bool fifo_write_tx(std::uint16_t header, std::span<const std::uint8_t> objs, std::span<const std::uint8_t> stream) {
    pdtrace::tx(header, objs, tx_sop_token(stream));
    return i2c::write(Bus::Fusb, kAddr, kFifos, stream);
}

void flush_rx();  // after g_crc_on
void flush_fifos() { wr(kControl0, kTxFlush); flush_rx(); }
void clear_irqs() {
    std::uint8_t i = 0, a = 0, b = 0;
    rd(kInterrupt, i); rd(kInterruptA, a); rd(kInterruptB, b);
    if (a & kIntHardRst) g_hr_latched = true;   // service() acts on it
    pdtrace::irq(a, b, i);   // what the read cleared, when it was not zero
}

// CC analog switches (high = closed). PC13 high puts an Ra (~1 kohm to ground) on CC1 (hardware 2026-11-06); `vconn` is the
// experimental VCONN role, and the Ra mode (g_ra) holds it high whatever the caller asks, so no routing change drops it.
void cc_switches(bool vconn, std::uint8_t mask = 3) {
    pins::cc1_switch.write(mask & 1);
    pins::cc2_switch.write(mask & 2);
    const bool pc13 = emkra::pc13_high(vconn, g_ra);
    pins::cc_vconn_gnd.write(pc13);
    boot_note(1, std::uint8_t((mask & 3) | (pc13 ? 4 : 0)));
}

// FIFO token to SOP kind: 0xE0 SOP, 0xC0 SOP', 0xA0 SOP''. Anything else (debug SOPs) counts as
// SOP so the caller drops it with the source traffic.
pd::Sop sop_from_token(std::uint8_t token) {
    switch (token & 0xE0) {
        case 0xC0: return pd::Sop::SopP;
        case 0xA0: return pd::Sop::SopPP;
        default: return pd::Sop::Sop;
    }
}

// ---- transmit (active PD) ----

// Send `m` as an SOP message: token stream SOP1 SOP1 SOP1 SOP2 (0x12 0x12 0x12 0x13; SOP1 SOP1 SOP3 SOP3 is SOP', the cable), PACKSYM|len, header, objects,
// JAM_CRC, EOP, TXOFF, then TX_START. True when INTERRUPTA reports TXSENT (the chip checked the
// partner's GoodCRC itself, with the CONTROL3 auto retries); false on RETRYFAIL or no answer.
// Set by every transmit(): the last TX ended without TXSENT or RETRYFAIL. That is ambiguous, the message may have been
// delivered (UGREEN X865: GoodCRC 118 ms after TX_START, past the loop below, its answer 3 ms later), so a caller that
// waits for an answer anyway (the re-read) treats it as sent.
bool g_tx_timed_out = false;

bool transmit(const pd::TxMsg& m) {
    g_tx_timed_out = false;
    if (!g_tx_allowed) {
        // Passive mode: no frame ever goes on CC - no GoodCRC, no Reject/Accept/Soft_Reset answer, not even a Hard
        // Reset - until an active user feature demands the wire (see the CrcDemand in active_tick()).
        K2LOG("[FUSB302] TX refused (passive): type %d%s\n", m.header & 0x1F, m.hard_reset ? " (hard reset)" : "");
        return false;
    }
    if (m.hard_reset) {
        pdtrace::reset(pdtrace::ResetKind::HardTx);
        wr(kControl3, kRetries3Auto | kSendHardReset);  // self-clearing
        delay_ms(5);
        wr(kReset, kPdReset);
        flush_fifos();
        g_msg_id = 0;
        g_new_source = true;
        K2LOG("[PD] t=%u Hard reset sent\n", unsigned(systick::millis()));
        return true;
    }
    if (m.restart_id) g_msg_id = 0;
    const std::uint16_t header = pd::with_msg_id(m.header, g_msg_id);
    std::array<std::uint8_t, 4 + 1 + 2 + 28 + 3> buf;
    unsigned n = 0;
    for (std::uint8_t b : {0x12, 0x12, 0x12, 0x13}) buf[n++] = b;
    buf[n++] = std::uint8_t(0x80 | (2 + m.len));
    buf[n++] = std::uint8_t(header);
    buf[n++] = std::uint8_t(header >> 8);
    for (unsigned i = 0; i < m.len && i < m.data.size(); i++) buf[n++] = m.data[i];
    buf[n++] = 0xFF;  // JAM_CRC
    buf[n++] = 0x14;  // EOP
    buf[n++] = 0xFE;  // TXOFF
    clear_irqs();
    if ((header & 0x1F) == pd::kCtlSoftReset && m.len == 0) pdtrace::reset(pdtrace::ResetKind::SoftTx);
    fifo_write_tx(header, {m.data.data(), m.len < m.data.size() ? m.len : m.data.size()}, {buf.data(), n});
    wr(kControl0, kTxStart);
    bool ok = false;
    pdtrace::TxResult res = pdtrace::TxResult::Timeout;
    std::uint8_t last_ia = 0;
    int waited = 0;
    for (int t = 0; t < 40; t++) {  // 40 passes of 1 ms plus the ~1.9 ms bit-banged read each (~117 ms on the Black Box); the chip's 3 retries end far sooner
        delay_ms(1);
        waited = t + 1;
        std::uint8_t ia = 0;
        if (!rd(kInterruptA, ia)) continue;
        last_ia |= ia;
        if (ia & kIntHardRst) g_hr_latched = true;
        if (ia & kIntTxSent) { ok = true; res = pdtrace::TxResult::TxSent; break; }
        if (ia & kIntRetryFail) { res = pdtrace::TxResult::RetryFail; break; }
        if (ia & kIntHardRst) break;   // the source is power-cycling: nothing will GoodCRC this, service() resets the engine next
    }
    g_tx_timed_out = res == pdtrace::TxResult::Timeout;
    pdtrace::tx_result(res, std::uint32_t(waited), last_ia);
    // A timeout (neither TXSENT nor RETRYFAIL seen) is ambiguous: the source may have GoodCRC'd it (the black box showed
    // exactly that after a Get_Source_Cap_Extended). Reusing the id then makes the source drop our next message as a
    // retransmission (GoodCRC, no Accept), so only a RETRYFAIL keeps the id.
    if (ok || res == pdtrace::TxResult::Timeout) g_msg_id = (g_msg_id + 1) & 7;
    K2LOG("[TX] type %d header 0x%04X len %d: %s\n", m.header & 0x1F, header, m.len,
                ok ? "GoodCRC" : "no GoodCRC");
    K2LOG("[PD] t=%u TX type %d id %d specrev-field %d: %s (%d ms)\n", unsigned(systick::millis()), m.header & 0x1F,
                (header >> 9) & 7, (header >> 6) & 3, ok ? "GoodCRC" : "no GoodCRC", waited);
    return ok;
}

// ---- e-marker scan ----

// Discover Identity request to SOP' as a FIFO token stream: SOP1 SOP1 SOP3 SOP3, PACKSYM|6,
// PD header 0x108F, VDM header 0xFF00A001, JAM_CRC, EOP, TXOFF.
constexpr std::array<std::uint8_t, 14> kDiscoverIdentity{
    0x12, 0x12, 0x1B, 0x1B, 0x86, 0x8F, 0x10, 0x01, 0xA0, 0x00, 0xFF, 0xFF, 0x14, 0xFE};

// The same request as PD 2.0 (header 0x104F, VDM 0xFF008001): old cables answer only that.
constexpr std::array<std::uint8_t, 14> kDiscoverIdentityPd2{
    0x12, 0x12, 0x1B, 0x1B, 0x86, 0x4F, 0x10, 0x01, 0x80, 0x00, 0xFF, 0xFF, 0x14, 0xFE};

// One orientation: `sw0` powers VCONN on one CC, `sw1` routes BMC to the other.
bool scan_orientation(std::uint8_t sw0, std::uint8_t sw1) {
    cc_switches(true);
    wr(kPower, kPwrAll);
    wr(kSwitches0, sw0);
    wr(kSwitches1, sw1);
    delay_ms(40);  // VCONN must power up the cable's e-marker before it can answer

    for (int attempt = 0; attempt < 2; attempt++) {
        flush_fifos();
        delay_ms(2);
        clear_irqs();
        const auto& di = attempt == 0 ? kDiscoverIdentity : kDiscoverIdentityPd2;  // PD 3.0, then PD 2.0
        fifo_write_tx(std::uint16_t(di[5] | (di[6] << 8)), {di.data() + 7, 4}, di);  // SOP' Discover Identity: header + VDM header
        wr(kControl0, kTxStart);
        // [EMK] probe: INTERRUPTA TXSENT / RETRYFAIL tell whether the cable's GoodCRC came back.
        bool tx_seen = false;

        for (int t = 0; t < 40; t++) {  // up to 80 ms for the answer
            delay_ms(2);
            std::uint8_t ia = 0;
            if (!tx_seen && rd(kInterruptA, ia) && (ia & (kIntTxSent | kIntRetryFail))) {
                tx_seen = true;
                K2LOG("[EMK] sw0=0x%02X attempt %d (PD %d.0): SOP' %s after %d ms\n", sw0, attempt,
                            attempt == 0 ? 3 : 2, (ia & kIntTxSent) ? "GoodCRC" : "RETRYFAIL (no GoodCRC)", (t + 1) * 2);
            }
            std::uint8_t status1;
            if (!rd(kStatus1, status1) || (status1 & kRxEmpty)) continue;

            std::array<std::uint8_t, 3> pre;  // token, PD header
            if (!fifo_read(pre)) continue;
            const pd::Sop sop = sop_from_token(pre[0]);
            const std::uint16_t header = pre[1] | (pre[2] << 8);
            const unsigned num_do = (header >> 12) & 7;
            std::array<std::uint8_t, 36> body;  // data objects + 4 CRC bytes
            if (!fifo_read({body.data(), num_do * 4 + 4})) continue;
            pdtrace::rx(pre[0], header, {body.data(), num_do * 4 + 4});
            K2LOG("[EMK] rx sop=%d type=%d num_do=%d\n", int(sop), int(header & 0x1F), int(num_do));
            if (sop == pd::Sop::Sop || num_do == 0) continue;  // our GoodCRC / the source: keep waiting
            g_engine.on_message(sop, header, {body.data(), num_do * 4}, g_active_cc);
            if (g_engine.cable.valid) return true;
        }
    }
    return false;
}

std::uint8_t ctl1() { return phy::control1(g_crc_on, g_sop_prime_hook != nullptr, g_cable_read); }

// SWITCHES1 for the active line, Auto-CRC on or off (`g_crc_on`).
void write_sw1() {
    wr(kSwitches1, phy::sw1(g_active_cc, g_crc_on));
    wr(kControl1, ctl1());   // SOP' receive follows Auto-CRC (except while the spoof is armed)
}
void flush_rx() { wr(kControl1, ctl1()); }

// Back to the passive profile after init and every detach: nothing ACKs, nothing transmits.
void apply_rd();
std::uint8_t idle_rd();

void configure_passive() {
    g_crc_on = false;
    g_tx_allowed = false;
    g_hr_tool_armed = false;
    g_rd_op = false;
    g_crc.reset();
    g_rd_gate.reset();
    g_contract_linger.reset();
    for (const RegWrite& w : phy::passive_config(g_active_cc)) wr(w.reg, w.value);
    if (g_active_cc == 0) wr(kSwitches0, idle_rd());   // plain boot: 0, no Rd (unless a run start presents it)
    else apply_rd();
    clear_irqs();
}

// Auto-CRC and Rd follow the active operations, each with its own demand (pd::CrcDemand): the chip may ACK and transmit while
// `crc_busy`; the K2 presents Rd on the active line only while `rd_busy` (a sink operation). The armed e-marker spoof is
// CRC demand without Rd demand: it used to set Rd too and kept it for good (hardware 2026-11-06).
void update_crc(bool crc_busy, bool rd_busy) {
    const std::uint32_t now = systick::millis();
    const bool want = g_crc.update(now, crc_busy);
    const bool rd = g_rd_gate.update(now, rd_busy);
    if (rd != g_rd_op) {
        g_rd_op = rd;
        apply_rd();
        K2LOG("[PD] Rd %s (sink operation)\n", rd ? "on" : "off");
    }
    if (want == g_crc_on) return;
    g_crc_on = want;
    write_sw1();
    K2LOG("[PD] Auto-CRC %s\n", want ? "on (active operation)" : "off (passive)");
}

// Rd bits while no partner is attached: only the boot-mode pin (plain boot: none, the passive state). A run start
// adds its own presentation (both lines while no CC is known), but never a second Rd next to a key mode's pin: Rd on both lines
// is the Debug Accessory signature (pd::key_rd_lines).
std::uint8_t idle_rd() {
    const std::uint8_t run = g_rd_start ? pd::key_rd_lines(emkra::run_rd_lines(g_ra, g_inline, 0), g_pin_rd, 0) : std::uint8_t(0);
    return std::uint8_t(g_pin_rd | run) & (kPdwn1 | kPdwn2);
}

// Inline for every gate: the link logic latched it (a real sink downstream).
bool inline_now() { return g_inline; }

// A sink other than the K2 asked the source for power in this attach (a Request with the power-role bit clear: the K2 never
// hears its own TX). The wire shows it a UI tick before the Link latches inline, and it also covers a K2 plugged in after
// the contract was made. Cleared with the attach (g_requests_base).
bool far_sink_seen() { return g_engine.contract.requests != g_requests_base; }

// The K2 answers its partner (SOP replies, Sink_Capabilities, swaps) only when it is the one sink on the link.
bool sole_sink() { return !inline_now() && !far_sink_seen(); }

// SWITCHES0 for the active line: the Rd the policy allows (pd_ccmode.hpp) plus the measured line.
void apply_rd() {
    if (g_active_cc == 0) { wr(kSwitches0, idle_rd()); return; }
    std::uint8_t lines = pd::rd_lines(g_pin_rd, g_rd_op, g_inline, g_active_cc);
    if (g_rd_start) lines |= pd::key_rd_lines(emkra::run_rd_lines(g_ra, g_inline, g_active_cc), g_pin_rd, g_active_cc);
    // VCONN (experiment): the chip's supply goes onto the line opposite the active one, never onto a line we pull down.
    std::uint8_t vconn = 0;
    if (g_vconn_applied) {
        const std::uint8_t other_bit = g_active_cc == 1 ? 2 : 1;
        if (!(lines & other_bit)) vconn = g_active_cc == 1 ? kVconnCc2 : kVconnCc1;
    }
    wr(kSwitches0, std::uint8_t(((lines & 1) ? kPdwn1 : 0) | ((lines & 2) ? kPdwn2 : 0)) | phy::sw0_measure(g_active_cc) | vconn);
}

// ---- CC orientation ----

// The attach half of a CC choice, shared by the scan (detect_cc_orientation) and the Rd-window re-check
// (cc_window_recheck): route the chosen line to the chip, measure it, and reset the PD session exactly as
// on a fresh attach (switches, SWITCHES0 measure bit, SWITCHES1 via write_sw1(), chip PD reset, FIFOs).
void apply_cc_choice(const pd::CcChoice& c, bool pinned, std::uint8_t rd_both) {
    g_cc_mask = pd::self_power_mask(c.mask, pinned);   // pinned: both closed, the Rd must reach the connector
    cc_switches(false, g_cc_mask);
    const int i = c.line - 1;
    g_active_cc = std::uint8_t(c.line);
    if (pinned) apply_rd();   // the pinned Rd follows the active line (pd::rd_lines)
    else wr(kSwitches0, std::uint8_t(rd_both | (i == 0 ? kMeasCc1 : kMeasCc2)));
    delay_ms(2);
    std::uint8_t status0 = 0;
    rd(kStatus0, status0);
    g_session.attach();
    g_detach.reset();
    g_attach_ms = systick::millis();
    g_requests_base = g_engine.contract.requests;
    g_pd_traffic = false;
    g_pd_seen_attach = false;
    g_engine.sink = {};   // the sniffed Sink_Capabilities belong to the previous partner
    g_bc_max = std::uint8_t(status0 & 3);
    g_vbus_mv = -1;  // a stale reading of the previous partner must not count
    write_sw1();
    // New attach: the PD logic and FIFOs still hold the previous partner's state, so
    // reset them or the first Source_Capabilities of this partner can be lost.
    wr(kReset, kPdReset);
    flush_fifos();
    clear_irqs();
    g_hr_latched = false;   // the previous partner's Hard Reset is not this one's
    K2LOG("[FUSB302] Active CC detected: CC%d (switches 0x%X: PC14 %s, PC15 %s)\n", c.line, c.mask,
                 (c.mask & 1) ? "closed" : "open", (c.mask & 2) ? "closed" : "open");
    pdtrace::cc(pdtrace::CcEvent::Attach, std::uint8_t(c.line), status0);
}

}  // namespace

static void vconn_mark();   // defined with the VCONN glue below; handle_frame needs it earlier

bool rp_on_line() {
    if (g_active_cc != 0) return true;
    std::uint8_t sw0 = 0, mdac = 0;
    if (!rd(kSwitches0, sw0) || !rd(kMeasure, mdac)) return false;
    const std::uint8_t pd_bits = std::uint8_t(sw0 & (kPdwn1 | kPdwn2));
    wr(kMeasure, kMdac210mV);
    bool seen = false;
    for (const std::uint8_t meas : {kMeasCc1, kMeasCc2}) {
        wr(kSwitches0, std::uint8_t(pd_bits | meas));
        delay_ms(2);
        std::uint8_t st = 0;
        if (rd(kStatus0, st) && (st & kComp)) seen = true;
    }
    wr(kSwitches0, sw0);
    wr(kMeasure, mdac);
    return seen;
}

namespace {

// Measure CC1 then CC2 against the ~210 mV MDAC threshold: the line that sees the source's
// Rp is the active one. Routes BMC receive and transmit to it.
void detect_cc_orientation() {
    wr(kMeasure, kMdac210mV);

    // Every switch setting x every FUSB302 line, both Rd on: the top male port and the bottom female
    // port reach the chip through different switches, so one fixed routing can miss a charger.
    const std::uint8_t rd_both = idle_rd();
    std::array<pd::CcSample, 6> samples{};
    int n = 0;
    // Plain (passive) boot keeps both CC switches closed: opening one while a
    // host and a dock attach through the K2 broke their USB3 setup (2026-11-03, mask 0x1 -> USB2).
    // A pinned (self-powered) boot sweeps, but only when the closed pair cannot name one live line: opening a switch opens the
    // Rd the charger holds VBUS for (pd::sweep_needed).
    // A key mode whose pinned line sees the Rp never sweeps and never leaves that line (pd::pinned_live_line).
    const bool pinned = g_pin_rd != 0;
    bool sweep = pinned;
    std::uint8_t pin_line = 0;
    for (const std::uint8_t mask : pd::kCcScanMasks) {
        if (n == 2 && pinned) {
            pin_line = pd::pinned_live_line(g_pin_rd, {samples.data(), 2});
            sweep = pin_line == 0 && pd::sweep_needed({samples.data(), 2});
        }
        if (!sweep && mask != 3) { samples[n++] = {mask, 1, false, 0}; samples[n++] = {mask, 2, false, 0}; continue; }
        cc_switches(false, mask);
        for (int line = 1; line <= 2; line++) {
            wr(kSwitches0, rd_both | (line == 1 ? kMeasCc1 : kMeasCc2));
            delay_ms(2);
            std::uint8_t status0 = 0;
            rd(kStatus0, status0);
            bool comp = (status0 & kComp) != 0;
            // Passive: a line above ~2.6 V is VCONN (or an unloaded Rp), not the Rp-over-Rd CC line.
            // Taking it as active ends in a false "no Rp" detach and a chip reset mid-attach.
            if (comp && !pinned) {
                wr(kMeasure, kMdacVconnMv);
                delay_ms(1);
                std::uint8_t hi = 0;
                rd(kStatus0, hi);
                if (hi & kComp) comp = false;
                wr(kMeasure, kMdac210mV);
            }
            samples[n++] = {mask, std::uint8_t(line), comp, std::uint8_t(status0 & 3)};
        }
    }
    pd::CcChoice c = pd::choose_cc_forced(samples, g_force_cc != 0 ? g_force_cc : pin_line);
    // Cross-check the comparator choice against the ADC net levels (Ui feeds sink_cc() every tick):
    // the live wire is the one net inside the Rd window, an open Rp (~3.3 V) must never win. A pinned
    // boot (g_pin_rd / pinned_live_line) and a forced debug line (g_force_cc) are explicit choices and
    // are not overridden. The ~2 Hz rescan after a detach (poll() -> detach_reset() -> this function)
    // lands here again with the fresh levels, so a VBUS collapse mid-negotiation cannot leave a wrong
    // line latched from a bad first sample.
    if (!pinned && g_force_cc == 0) {
        const std::uint8_t rd_line = pd::rd_window_line(g_cc1_mv, g_cc2_mv);
        if (rd_line != 0 && rd_line != c.line) {
            const pd::CcChoice alt = pd::choose_cc_forced(samples, rd_line);
            if (alt.found) {
                K2LOG("[FUSB302] CC override: ADC cc1=%d cc2=%d mV names CC%u, the comparator chose CC%u\n",
                          int(g_cc1_mv), int(g_cc2_mv), unsigned(rd_line), unsigned(c.line));
                c = alt;
            }
        }
    }

    // [PD] one line per scan result change: switch mask, per-line level (-: no Rp) for each setting.
    static std::uint32_t last_sig = 0xFFFFFFFFu;
    std::uint32_t sig = c.found ? (1u << 16 | std::uint32_t(c.mask) << 8 | c.line) : 0;
    for (const pd::CcSample& x : samples) sig = sig * 5 + (x.comp ? 1u + x.bc_lvl : 0u);
    if (sig != last_sig) {
        last_sig = sig;
        char lv[6][2] = {};
        for (int i = 0; i < 6; i++) { lv[i][0] = samples[i].comp ? char('0' + samples[i].bc_lvl) : '-'; }
        K2LOG("[PD] CC scan sw=3:%s/%s sw=1:%s/%s sw=2:%s/%s (CC1/CC2 BC_LVL) -> %s\n", lv[0], lv[1], lv[2], lv[3],
                     lv[4], lv[5], c.found ? "live" : "none");
    }

    g_scan_samples = samples;
    g_scan_sample_n = unsigned(n);
    if (c.found) {
        apply_cc_choice(c, pinned, rd_both);
        return;
    }
    g_active_cc = 0;  // nothing attached yet: keep both Rd pull-downs, both switches closed
    g_cc_mask = 3;
    cc_switches(false, g_cc_mask);
    wr(kSwitches0, rd_both);
}

// Cold boot: the attach scan runs about 1 s after reset, before the UI has fed sink_cc() any ADC levels,
// so its Rd-window cross-check saw -1/-1 and could not override a wrongly latched line (an open Rp net
// won the comparator tie). Re-run the cross-check whenever fresh levels arrive while a line is attached
// and no PD message has been decoded on it yet, rate-limited. Passive: comparator reads only, unless the
// levels name another line. A forced debug line (g_force_cc) and a pinned self-power boot are explicit
// choices and are never overridden; PD traffic already decoded on the attached line ends the re-check.
void cc_window_recheck() {
    if (g_active_cc == 0 || g_pd_traffic || g_force_cc != 0 || g_pin_rd != 0) return;
    const std::uint32_t now = systick::millis();
    if (std::int32_t(now - g_cc_recheck_at) < std::int32_t(kCcRecheckMinMs)) return;
    g_cc_recheck_at = now;
    const std::uint8_t rd_line = pd::rd_window_override(g_cc1_mv, g_cc2_mv, g_active_cc);
    if (rd_line == 0) return;
    pd::CcChoice alt = pd::choose_cc_forced({g_scan_samples.data(), g_scan_sample_n}, rd_line);
    if (!alt.found) {
        // The last scan's samples do not name the line (the comparator never saw it): a fresh mini-scan
        // over the current routing, both lines measured against the 210 mV threshold.
        std::uint8_t sw0 = 0;
        rd(kSwitches0, sw0);
        wr(kMeasure, kMdac210mV);
        std::array<pd::CcSample, 2> mini{};
        cc_switches(false, g_cc_mask);
        for (int line = 1; line <= 2; line++) {
            wr(kSwitches0, std::uint8_t(idle_rd() | (line == 1 ? kMeasCc1 : kMeasCc2)));
            delay_ms(2);
            std::uint8_t status0 = 0;
            rd(kStatus0, status0);
            mini[line - 1] = {g_cc_mask, std::uint8_t(line), (status0 & kComp) != 0, std::uint8_t(status0 & 3)};
        }
        alt = pd::choose_cc_forced(mini, rd_line);
        if (!alt.found) { wr(kSwitches0, sw0); return; }   // the line is not there: leave the chip as it was
    }
    K2LOG("[FUSB302] CC override: ADC cc1=%d cc2=%d mV names CC%u, the comparator chose CC%u\n",
              int(g_cc1_mv), int(g_cc2_mv), unsigned(rd_line), unsigned(g_active_cc));
    apply_cc_choice(alt, false, idle_rd());
}

// ---- Source_Capabilities logging ----

void log_pdo(int n, const pd::Pdo& p) {
    const int v = p.max_mv, vmin = p.min_mv, a = p.max_ma, w = int(p.max_mw / 1000);
    using T = pd::PdoType;
    switch (p.type) {
        case T::Fixed:
            K2LOG("[PD]  PDO %d: FIXED %d.%02dV @ %d.%02dA (%dW)\n", n, v / 1000, (v % 1000) / 10,
                        a / 1000, (a % 1000) / 10, w);
            break;
        case T::Battery:
            K2LOG("[PD]  PDO %d: BATT %d-%dV @ %dW\n", n, vmin / 1000, v / 1000, w);
            break;
        case T::Variable:
            K2LOG("[PD]  PDO %d: VAR %d-%dV @ %d.%02dA\n", n, vmin / 1000, v / 1000, a / 1000,
                        (a % 1000) / 10);
            break;
        case T::Pps:
            K2LOG("[PD]  PDO %d: PPS %d.%dV-%d.%dV @ %d.%02dA (%dW)\n", n, vmin / 1000,
                        (vmin % 1000) / 100, v / 1000, (v % 1000) / 100, a / 1000, (a % 1000) / 10, w);
            break;
        case T::Avs:
            K2LOG("[PD]  PDO %d: AVS %d-%dV @ %d.%02dA (%dW)\n", n, vmin / 1000, v / 1000, a / 1000,
                        (a % 1000) / 10, w);
            break;
        default: break;
    }
}

// The outcome of an SPR-mode EPR_Get_Source_Cap (no EPR entry): the proof for "EPR available, needs an EPR cable".
void log_epr_info(pd::EprInfo s) {
    switch (s) {
        case pd::EprInfo::Got:
            K2LOG("[PD] EPR caps info: list received (%d PDOs)\n", g_engine.epr_info.num_pdos);
            // The raw words, zero ones included (padding, positions up to 7): what the list really holds
            for (unsigned i = 0; i < g_engine.epr_info.num_pdos && i < g_engine.epr_info.pdos.size(); i++)
                K2LOG("[PD] EPR caps info: PDO%u raw=%08X\n", i + 1, unsigned(g_engine.epr_info.pdos[i].raw));
            break;
        case pd::EprInfo::Refused: K2LOG("[PD] EPR caps info: refused (Reject / Not_Supported)\n"); break;
        case pd::EprInfo::Timeout: K2LOG("[PD] EPR caps info: no answer\n"); break;
        default: break;
    }
}

void log_ext(unsigned type) {
    const pd::ExtInfo& x = g_engine.ext;
    switch (type) {
        case pd::kExtSourceCapsExt:
            K2LOG("[PD] Source_Cap_Ext: VID=0x%04X PID=0x%04X XID=0x%08X FW=%d HW=%d PDP=%dW EPR=%dW\n",
                        x.caps_ext.vid, x.caps_ext.pid, unsigned(x.caps_ext.xid), x.caps_ext.fw,
                        x.caps_ext.hw, x.caps_ext.pdp_w, x.caps_ext.epr_pdp_w);
            break;
        case pd::kExtStatus:
            K2LOG("[PD] Status: temp=%dC input=0x%02X events=0x%02X tstat=%d\n", x.status.internal_temp_c,
                        x.status.present_input, x.status.event_flags, x.status.temp_status);
            break;
        case pd::kExtBatteryCaps:
            K2LOG("[PD] Battery_Cap: design=%d.%dWh last_full=%d.%dWh\n", x.battery.design_dwh / 10,
                        x.battery.design_dwh % 10, x.battery.last_full_dwh / 10, x.battery.last_full_dwh % 10);
            break;
        case pd::kExtManufacturerInfo:
            K2LOG("[PD] Manufacturer_Info: VID=0x%04X PID=0x%04X \"%s\"\n", x.mfr.vid, x.mfr.pid,
                        x.mfr.name);
            break;
        case pd::kExtPpsStatus:
            K2LOG("[PD] PPS_Status: %dmV %dmA ptf=%d omf=%d\n", x.pps.out_mv, x.pps.out_ma, x.pps.ptf,
                        x.pps.omf);
            break;
        default: K2LOG("[PD] Extended message 0x%02X\n", type); break;
    }
}

void log_vdm(std::uint16_t header, std::span<const std::uint8_t> payload) {
    if (payload.size() < 4) return;
    const std::uint32_t hdr = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (std::uint32_t(payload[3]) << 24);
    const unsigned cmd_type = (hdr >> 6) & 3, cmd = hdr & 0x1F;
    const pd::VdmInfo& v = (header >> 8) & 1 ? g_engine.vdm : g_engine.sink_vdm;  // the engine filed it by sender
    K2LOG("[VDM] hdr=0x%08X SVID=0x%04X (%s) type=%u cmd=%u\n", unsigned(hdr), unsigned(hdr >> 16),
                pd::svid_name(hdr >> 16), cmd_type, cmd);
    if (cmd_type != 1 || (hdr >> 16) != pd::kSvidPd) return;
    if (cmd == 1 && v.identity.valid) {
        K2LOG("[VDM] Identity: VID=0x%04X (%s) PID=0x%04X bcd=0x%04X%s%s\n", v.identity.vid,
                    v.identity.vendor, v.identity.pid, v.identity.bcd, v.identity.model ? " model=" : "",
                    v.identity.model ? v.identity.model : "");
        // Raw VDOs: ID header, cert, product, then product-type VDOs (USB link classifier input).
        for (std::size_t i = 4; i + 3 < payload.size(); i += 4) {
            const std::uint32_t w = payload[i] | (payload[i + 1] << 8) | (payload[i + 2] << 16) |
                                    (std::uint32_t(payload[i + 3]) << 24);
            K2LOG("[VDM] Identity VDO%d=0x%08X\n", int(i / 4), unsigned(w));
        }
    } else if (cmd == 2) {
        for (int i = 0; i < v.num_svids; i++)
            K2LOG("[VDM] SVID %d: 0x%04X (%s)\n", i + 1, v.svids[i], pd::svid_name(v.svids[i]));
    }
}

// One received frame (SOP token stripped): the engine decodes it, we log what it produced.
void log_cable(const pd::Emarker& e) {
    K2LOG("[EMARK] SOP%s PD%s, VDO v%d, latency %s, %s%s, EPR=%d\n", e.sop == 1 ? "'" : "''",
                e.spec_rev == 1 ? "2.0" : "3.x", e.vdo_version, e.latency_str, e.termination_str,
                e.type_c_plug ? ", Type-C plug" : "", e.epr_capable);
    if (e.vdo2_valid)
        K2LOG("[EMARK] Active VDO2: max %dC, shutdown %dC, U3 power %d, USB4=%d, lanes=%d, gen2=%d, optical=%d\n",
                    e.max_temp_c, e.shutdown_temp_c, e.u3_power, e.usb4_supported, e.usb_lanes, e.usb_gen2,
                    e.optical_isolated);
    if (g_engine.cable_thunderbolt()) K2LOG("[EMARK] Cable advertises Thunderbolt SVID 0x8087\n");
}

// Sink engine state changes go to the black box; called after every place the engine can move.
void trace_sink() {
    static std::uint8_t last[5] = {0xFF, 0, 0, 0, 0};
    const pd::SinkStatus& s = g_sink.status();
    const std::uint8_t cur[5] = {std::uint8_t(s.state), std::uint8_t(s.result), std::uint8_t(s.held),
                                 std::uint8_t(s.epr_mode), s.pos};
    for (int i = 0; i < 5; i++) {
        if (cur[i] == last[i]) continue;
        for (int j = 0; j < 5; j++) last[j] = cur[j];
        pdtrace::sink(cur[0], cur[1], s.held, s.epr_mode, s.pos);
        return;
    }
}

void handle_frame(pd::Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, bool wire = true) {
    const bool ext = (header >> 15) & 1;
    const unsigned msg_type = header & 0x1F, num_do = (header >> 12) & 7;
    g_pd_traffic = true;
    g_pd_seen_attach = true;
    // First capabilities after a re-attach: drop the previous charger's data so two sources never mix.
    if (g_new_source && sop == pd::Sop::Sop &&
        ((!ext && msg_type == 0x01 && num_do > 0) || (ext && msg_type == pd::kExtEprSourceCaps))) {
        g_engine.reset_source();
        g_new_source = false;
    }
    if (sop == pd::Sop::Sop && !ext && msg_type == 0x01 && num_do > 0)
        pdtrace::mark(pdtrace::kMarkCaps, std::uint16_t(inline_now() ? 2 : 0), std::uint16_t(g_sink.status().state));
    if (sop == pd::Sop::Sop && !ext && msg_type == 0x01 && num_do > 0) {
        g_caps_count++;
        g_caps_acked = g_crc_on;
        g_session.caps_rx();
    }
    if (sop == pd::Sop::Sop && !ext && num_do == 0 &&
        (msg_type == pd::kCtlAccept || msg_type == pd::kCtlReject || msg_type == pd::kCtlPsRdy ||
         msg_type == pd::kCtlWait))
        K2LOG("[PD] t=%u RX %s id %d (auto-crc %s)\n", unsigned(systick::millis()), pd::control_name(msg_type),
                    (header >> 9) & 7, g_crc_on ? "on" : "OFF");
    if (sop == pd::Sop::Sop && !ext && num_do > 0 && msg_type == pd::kDataEprMode && payload.size() >= 4) {
        const unsigned action = payload[3], data = payload[2];   // EPR_Mode data object bits 31..24 / 23..16
        if (action == pd::kEprFailed)
            K2LOG("[PD] RX EPR_Mode Enter_Failed: %u (%s)\n", data, pd::epr_fail_name(std::uint8_t(data)));
        else
            K2LOG("[PD] RX EPR_Mode action %u\n", action);
    }
    const auto cable_acks = g_engine.cable_acks;
    const pd::EprInfo info_before = g_sink.status().epr_info;
    g_engine.epr_info_only = info_before == pd::EprInfo::Pending;   // the answer to our SPR-mode query is not the live list
    g_engine.on_message(sop, header, payload, g_active_cc);
    if (wire && !g_sop_prime_hook && g_engine.cable_acks != cable_acks) {
        g_physical_cable = pd::cable_limits(g_engine.cable);
        g_physical_session = g_session.id();
    }
    {
        const std::uint32_t now = systick::millis();
        // Get_Sink_Cap is answered only by a live Pd-mode sink: never in the PD-silent legacy session, never inline, never
        // while another sink's Request was seen. The identity / VDM and Soft_Reset replies follow the same inline rule
        // (set before this frame is handed to the sink: a far sink's Request precedes its first VDM).
        g_sink.set_gives_sink_cap(!g_legacy_only && sole_sink() && g_active_cc != 0);
        g_sink.set_answers_sop(sole_sink());
        g_sink.set_never_hard_reset(g_pin_rd != 0);   // see sync_hard_reset_policy()
        const unsigned swaps_before = g_sink.swaps_rx(pd::SwapKind::Dr) + g_sink.swaps_rx(pd::SwapKind::Pr) + g_sink.swaps_rx(pd::SwapKind::Fr);
        g_sink.on_message(sop, header, payload, now);
        if (swaps_before != unsigned(g_sink.swaps_rx(pd::SwapKind::Dr)) + g_sink.swaps_rx(pd::SwapKind::Pr) + g_sink.swaps_rx(pd::SwapKind::Fr)) {
            // Role swap asked by the partner (the sink answered it; see Sink::swaps_rx)
            const pd::SwapKind k = msg_type == pd::kCtlDrSwap ? pd::SwapKind::Dr : msg_type == pd::kCtlPrSwap ? pd::SwapKind::Pr : pd::SwapKind::Fr;
            const std::uint8_t reply = pd::Sink::swap_reply(k, g_sink.rev());
            K2LOG("[PD] swap rx %s -> %s\n", k == pd::SwapKind::Dr ? "DR_Swap" : k == pd::SwapKind::Pr ? "PR_Swap" : "FR_Swap",
                  reply == pd::kCtlReject ? "Reject" : reply == pd::kCtlNotSupported ? "Not_Supported" : "none");
        }
        if (g_sink.status().epr_info != info_before) log_epr_info(g_sink.status().epr_info);
        if (sop == pd::Sop::Sop && !ext && num_do == 0) {
            const pd::VconnState vs = g_vconn.state();
            g_vconn.on_control(std::uint8_t(msg_type), now);
            if (vs != g_vconn.state() || msg_type == pd::kCtlVconnSwap) {
                K2LOG("[VCONN] RX control %u: state %u -> %u (why %u)\n", unsigned(msg_type), unsigned(vs), unsigned(g_vconn.state()), unsigned(g_vconn.why()));
                vconn_mark();
            }
        }
        bool requested_now = false;   // one of the branches below already asked for PDO 1 in this frame
        if (sop == pd::Sop::Sop && !ext && num_do > 0 && msg_type == pd::kDataSourceCap && g_sink.take_resume() && !inline_now() && g_active_cc != 0 &&
            !g_legacy_only) {
            // After a source hard reset the contract is gone: ask for PDO 1 (5 V) again on our own.
            const pd::Err e = g_sink.trigger({1, 0, 0}, now);
            requested_now = true;
            K2LOG("[PD] auto re-request after hard reset: err=%u\n", unsigned(e));
        }
        // A run start is a sink while it waits: PDO 1 (5 V) once, so the source gets its GoodCRC and a Request
        // and brings VBUS up (the C135 ramps ~2 V/s and gives up when its caps go unanswered, hardware 2026-11-03).
        if (g_rd_start && sop == pd::Sop::Sop && !ext && num_do > 0 && msg_type == pd::kDataSourceCap) {
            const std::uint8_t why = pd::start_request_blockers(inline_now(), g_active_cc, g_sink.status().state == pd::SinkState::Idle, g_start_requested, g_legacy_only);
            if (why == 0) {
                g_start_requested = true;
                const pd::Err e = g_sink.trigger({1, 0, 0}, now);
                requested_now = true;
                pdtrace::mark(pdtrace::kMarkAutoReq, std::uint16_t(e));   // the black box has no K2LOG: it shows the decision
                K2LOG("[PD] run start: request PDO 1 err=%u (auto-crc %s)\n", unsigned(e), g_crc_on ? "on" : "OFF");
            } else {
                pdtrace::mark(pdtrace::kMarkAutoReq, std::uint16_t(0x100 | why));
            }
        }
        // Whatever turned Auto-CRC on (a pd-reread's Identify, a Get_Source_Cap, the armed spoof), the chip's GoodCRC on these
        // caps started the source's SenderResponseTimer (24-30 ms): only a Request stops it, silence ends in a Hard Reset
        // (OnePlus 120W, 2026-11-06: reread on an already known line, caps acked, no Request, Hard Reset 27 ms later). So the
        // caps the chip acked always get PDO 1 here, unless the answer is somebody else's (pd::acked_caps_blockers). Auto-CRC
        // off (the passive idle path) never reaches this: nothing was acked, the source heard nothing and sends no timer.
        if (sop == pd::Sop::Sop && !ext && num_do > 0 && msg_type == pd::kDataSourceCap) {
            // A self-powered K2 (pinned Rd) asks for 5 V at a modest current and never lets a failed attempt leave the source
            // without a Request: Failed counts as idle there, so the next re-announce is asked again (nothing sends a Hard Reset).
            const bool hold = !g_rd_start && pd::self_hold_wanted(g_pin_rd, inline_now(), g_legacy_only);
            const pd::SinkState st = g_sink.status().state;
            const bool sink_idle = st == pd::SinkState::Idle || (hold && st == pd::SinkState::Failed);
            if (pd::acked_caps_need_request(g_caps_acked, inline_now(), g_active_cc, g_legacy_only, sink_idle, g_prep.active(),
                                            requested_now)) {
                const pd::Err e = g_sink.trigger({1, 0, hold ? pd::kSelfHoldMa : std::uint16_t(0)}, now);
                pdtrace::mark(pdtrace::kMarkAutoReq, std::uint16_t((hold ? 0x300 : 0x200) | unsigned(e)));
                K2LOG("[PD] acked caps: request PDO 1 err=%u\n", unsigned(e));
                if (hold) K2LOG("[PD] self-power hold: 5 V contract asked at %u mA\n", unsigned(pd::kSelfHoldMa));
            }
        }
        g_ident.on_message(sop, header, payload, now);
        g_probe.on_message(sop, header, payload, now);
        g_reread.on_message(sop, header, payload, now);
    }
    if (sop != pd::Sop::Sop && g_sop_prime_hook) g_sop_prime_hook(sop == pd::Sop::SopP ? 1 : 2, header, payload);
    if (sop != pd::Sop::Sop) {  // passive SOP'/SOP'' traffic: report, never answer
        K2LOG("[PD] SOP%s %s 0x%04X, %d objects\n", sop == pd::Sop::SopP ? "'" : "''",
                    msg_type == pd::kMsgVendorDefined ? "VDM" : "msg", header, num_do);
        if (g_engine.cable.valid && msg_type == pd::kMsgVendorDefined) log_cable(g_engine.cable);
        return;
    }
    const bool info_chunk = g_engine.epr_info_only && sop == pd::Sop::Sop && ext && msg_type == pd::kExtEprSourceCaps;
    const pd::SourceCaps& c = info_chunk ? g_engine.epr_info : g_engine.caps;
    if (sop == pd::Sop::Sop && ext && msg_type == pd::kExtEprSourceCaps) {
        K2LOG("[PD] EPR_Source_Capabilities%s chunk: %d PDOs so far%s, rev=%d (CC%d)\n", info_chunk ? " (info)" : "", c.num_pdos,
                    c.epr_partial ? " (partial)" : "", c.spec_rev, c.active_cc);
        for (int i = 0; i < c.num_pdos; i++) log_pdo(i + 1, c.pdos[i]);
    } else if (sop == pd::Sop::Sop && !ext && msg_type == 0x01 && num_do > 0) {
        // A source that repeats its caps floods the HID log ring: the first block of a source, a change and one block per
        // 10 s get through (with the repeats held back); the Black Box records every message regardless.
        std::uint32_t raw[pd::kMaxPdos];
        const unsigned n = unsigned(c.num_pdos) < pd::kMaxPdos ? unsigned(c.num_pdos) : unsigned(pd::kMaxPdos);
        for (unsigned i = 0; i < n; i++) raw[i] = c.pdos[i].raw;
        const pd::CapsLogGate::Decision d = g_caps_log.on_caps(g_session.id(), pd::caps_content_hash(raw, n, unsigned(c.spec_rev), unsigned(c.active_cc)),
                                                              systick::millis());
        if (d.log) {
            if (d.repeats) K2LOG("[PD] Source_Capabilities: %u identical repeats held back\n", unsigned(d.repeats));
            K2LOG("[PD] Source_Capabilities received: %d PDOs, rev=%d (CC%d)\n", c.num_pdos, c.spec_rev,
                        c.active_cc);
            for (int i = 0; i < c.num_pdos; i++) log_pdo(i + 1, c.pdos[i]);
        }
    } else if (sop == pd::Sop::Sop && !ext && msg_type == pd::kMsgVendorDefined) {
        log_vdm(header, payload);
    } else if (sop == pd::Sop::Sop && ext) {
        log_ext(msg_type);
    } else if (sop == pd::Sop::Sop && !ext && msg_type == pd::kDataRevision) {
        const pd::Revision& r = g_engine.ext.revision;
        K2LOG("[PD] Revision: PD %d.%d, version %d.%d\n", r.rev_major, r.rev_minor, r.ver_major,
                    r.ver_minor);
    } else if (num_do == 0 && !ext) {
        K2LOG("[PD] Control message: %s (0x%02X)\n", pd::control_name(msg_type), msg_type);
    }
}

}  // namespace

// INT_N on PB12 (input with pull-up) through EXTI12, falling edge, EXTI15_10 (IRQ 40). The ISR
// only sets a flag; every chip access stays in service().
void exti_init() {
#ifndef K2_HOST_PINS
    reg::crm().APB2EN |= reg::Crm::APB2EN_AFIO | reg::Crm::APB2EN_GPIOB;
    pins::fusb_int.pull_up();
    auto& af = reg::afio();
    af.EXTICR[3] = (af.EXTICR[3] & ~(0xFu << 0)) | (1u << 0);  // line 12 = EXTICR4[3:0]: port B
    auto& ex = reg::exti();
    ex.RTSR &= ~(1u << 12);
    ex.FTSR |= 1u << 12;
    ex.PR = 1u << 12;
    ex.IMR |= 1u << 12;
    reg::nvic().ISER[1] = 1u << (40 - 32);
#endif
}

void self_power_early(bootmode::Mode mode, bool warm) {
    // Before anything slow: decide the pin, write SWITCHES0, then route the CC switches. The write comes FIRST: the chip's
    // power-on default (0x03, Rd on both lines) would otherwise reach the connector through the freshly closed switches, and a
    // HID-powered K2 must never present Rd it did not choose. A SWITCHES0 write only needs the I2C bus.
    // The pin: a key mode pins its line; a plain boot after a warm reset keeps the pin the backup-register mark says this
    // firmware held (a charger-powered K2 that resets with VBUS present must not take the Rd away from the charger). The chip's
    // own SWITCHES0 is never read for this: after a flash or `reboot` it holds the power-on 0x03 or a reread's Rd, and a K2
    // powered over HID has no pin, so its warm reset is a detach (bootmode::initial_rd).
#ifndef K2_HOST_PINS
    reg::crm().APB2EN |= reg::Crm::APB2EN_GPIOC;
#endif
    for (const auto& p : {pins::cc_vconn_gnd, pins::cc1_switch, pins::cc2_switch}) p.mode(reg::Mode::OutPp50);
    g_mark_read = mode == bootmode::Mode::Plain && warm;
    const std::uint16_t mark = g_mark_read ? pinmark::read() : std::uint16_t(0);
    g_mark_raw = mark;
    g_pin_rd = bootmode::initial_rd(mode, warm, mark) & (kPdwn1 | kPdwn2);
    g_initial_pin_rd = g_pin_rd;
    g_pin_moved = false;
    g_rd_inherited = mode == bootmode::Mode::Plain && g_pin_rd != 0;
    set_boot_pin(g_pin_rd);     // a run's detach takes the pin over and restores it afterwards; the mark follows it
    wr(kSwitches0, g_pin_rd);   // a plain boot writes 0 as well: no Rd, the passive state
    g_cc_mask = bootmode::cc_mask(mode);
    cc_switches(false, g_cc_mask);
}

bool rd_inherited() { return g_rd_inherited && g_pin_rd != 0; }

BootRecord boot_record() {
    return {g_initial_pin_rd, g_pin_rd, g_boot_pin_rd, g_rd_inherited, g_pin_moved, g_mark_read, g_mark_raw};
}

unsigned boot_note_count() { return g_boot_note_n + g_readback_n; }

BootNote boot_note_at(unsigned i) {
    if (i < g_boot_note_n) return g_boot_notes[i];
    i -= g_boot_note_n;
    return i < g_readback_n ? g_readbacks[i] : BootNote{};
}

// Reads SWITCHES0 back when the next readback is due. Called from the boot sampler (delay hook, from the first moment) and from
// poll(), so the readbacks happen on time whether the main loop has started or not; a late call reads late and says so (ms).
void boot_readback_service() {
    if (g_readback_n >= kBootReadbacks) return;
    const std::uint32_t t = systick::millis();
    if (t < kReadbackAtMs[g_readback_n]) return;
    std::uint8_t v = 0;
    if (!rd(kSwitches0, v)) v = 0xFF;   // the bus failed: shown as FF (the K2 never writes a value with bit 7 set)
    g_readbacks[g_readback_n++] = {std::uint16_t(t), 2, v};
}

// Drop an inherited Rd for good (no run resumes on it): the pin, its restore and the chip's bits.
void self_power_release() {
    if (!g_rd_inherited) return;
    g_rd_inherited = false;
    set_boot_pin(0);
    self_power_leave();
}

// Move the single pinned Rd to `line` (1 or 2), at most once per boot (bootmode::moved_pin decides when). The pin, its restore
// after a run's detach and the pin mark follow. Never Rd on both lines: the pair is the Debug Accessory signature, which latches
// a source's Rp off until a physical detach (OnePlus 120W brick, hardware 2026-11-06), so there is no widen.
bool self_power_move(std::uint8_t line) {
    if (g_pin_moved || g_pin_rd == 0 || (line != 1 && line != 2)) return false;
    const std::uint8_t bit = line == 1 ? kPdwn1 : kPdwn2;
    if (g_pin_rd == bit) return false;
    g_pin_moved = true;
    g_pin_rd = bit;
    set_boot_pin(bit);
    if (g_active_cc == 0) wr(kSwitches0, g_pin_rd);
    return true;
}

void self_power_leave() {
    if (g_pin_rd == 0) return;
    g_pin_rd = 0;
    if (g_active_cc == 0) wr(kSwitches0, 0);   // plain: no Rd
}

// Give the boot-mode Rd pin back (after a run's detach took it over): Rd is dropped with an
// explicit SWITCHES0 write even in the Rd boot modes, so the charger sees a
// real detach; afterwards the pinned Rd returns (self-powered idle).
void self_power_restore() {
    if (g_boot_pin_rd == 0 || g_pin_rd == g_boot_pin_rd) return;
    g_pin_rd = g_boot_pin_rd;
    if (g_active_cc == 0) wr(kSwitches0, g_pin_rd);
}

bool self_powered() { return g_pin_rd != 0; }
std::uint8_t pin_rd() { return g_pin_rd; }

// `debug pin`: a HID-powered K2 acts like a charger-powered one (lab only). The pin, its mark (so a warm reset inherits it, as
// after a key-mode boot) and SWITCHES0 follow; 0 gives it back. Refused with a sink inline.
bool debug_pin(std::uint8_t line) {
    if (line > 2 || inline_now()) return false;
    if (line == 0) {
        g_rd_inherited = false;
        set_boot_pin(0);
        self_power_leave();
        K2LOG("[PIN] released\n");
        return true;
    }
    const std::uint8_t bit = line == 1 ? kPdwn1 : kPdwn2;
    g_pin_moved = false;
    g_pin_rd = bit;
    g_rd_inherited = true;
    set_boot_pin(bit);
    if (g_active_cc == 0) wr(kSwitches0, g_pin_rd);
    K2LOG("[PIN] Rd pinned on CC%u (mark written)\n", unsigned(line));
    return true;
}

bool init() {
    pdtrace::init();
    g_engine.reset();
    g_physical_cable = {};
    g_physical_session = 0;
    g_sink.bind(&g_engine.caps);
    g_sink.bind_epr_info(&g_engine.epr_info);
    g_msg_id = 0;
    g_rd_op = false;

    // PC13..PC15 drive the CC analog switches and the VCONN path.
#ifndef K2_HOST_PINS
    reg::crm().APB2EN |= reg::Crm::APB2EN_GPIOC;
#endif
    for (const auto& p : {pins::cc_vconn_gnd, pins::cc1_switch, pins::cc2_switch}) p.mode(reg::Mode::OutPp50);
    cc_switches(false, g_cc_mask);   // 3 unless a self-powered boot mode routed them already
    K2LOG("[FUSB302] Hardware CC switches enabled (mask %d)\n", g_cc_mask);

    if (!rd(kDeviceId, g_device_id)) {
        K2LOG("[FUSB302] ERROR: Failed to read DEVICE_ID\n");
        return false;
    }
    K2LOG("[FUSB302] Detected ID: 0x%02X (version 0x%X, rev 0x%X)\n", g_device_id,
                (g_device_id >> 4) & 0xF, g_device_id & 0xF);

    wr(kReset, kSwReset);
    // SW_RES puts the chip's own power-on SWITCHES0 (0x03, Rd on both lines) on the connector through the closed CC switches:
    // write the pin (or 0) before anything slow, so a key mode's source never sees the Debug Accessory pair, not even for the
    // 10 ms the chip needs (hardware 2026-11-06: that pair latches a source's Rp off).
    wr(kSwitches0, g_pin_rd);
    delay_ms(10);
    g_active_cc = 0;
    configure_passive();
    flush_fifos();
    detect_cc_orientation();
    exti_init();
    g_irq = true;  // drain once whatever arrived before the line was armed
    return true;
}

void park() { wr(kReset, kSwReset); }

// Legacy-only (charger test PD-to-QC handover): detach emulation, then no PD sink policy.
void legacy_only(bool on) {
    if (on == g_legacy_only) return;
    g_legacy_only = on;
    if (!on) return;
    K2LOG("[PD] Legacy-only: dropping PD (CC open), sink policy off\n");
    g_prep.abort();
    // Not release(): that leaves a Ready sink behind, which answers the source's next Source_Capabilities with a Request
    // and puts the contract back (the C135 keeps D+/D- dead while it holds one).
    g_sink.detach();
    if (!inline_now()) {   // a real sink on the other port: never open the lines
        pins::cc1_switch.clear();
        pins::cc2_switch.clear();
        delay_ms(300);   // the source sees a detach and falls back to legacy on re-attach
        cc_switches(false, g_cc_mask);
    }
    g_detach.reset();
    g_engine.reset_source();
    g_new_source = true;
    g_pd_traffic = false;
    g_last_rx_ms = systick::millis();
}

std::uint32_t pd_quiet_ms() { return std::uint32_t(systick::millis() - g_last_rx_ms); }

void rescan() {
    if (g_legacy_only) return;   // the charger test holds the line quiet for a legacy fast-charge probe
    if (inline_now()) {
        // A real sink is attached: stay off the line. The source's next Source_Capabilities
        // (a sink Request, a charger re-announce) starts a new source in the engine.
        K2LOG("[PD] Rescan (inline): passive, waiting for the next Source_Capabilities\n");
        g_new_source = true;
        return;
    }
    K2LOG("[PD] Rescan requested: Cycling CC lines\n");
    for (const pd::Op& op : pd::rescan_plan(false)) {
        switch (op.kind) {
            case pd::OpKind::CcSwitches:
                if (op.value) { cc_switches(false, g_cc_mask); }
                else { pins::cc1_switch.clear(); pins::cc2_switch.clear(); }
                break;
            case pd::OpKind::Delay: delay_ms(op.value); break;
        }
    }
    // The source answers the CC cycle with Source_Capabilities by itself; nothing is sent from here.
    g_detach.reset();  // the 150 ms the line was open is not a detach
    g_new_source = true;
}

bool cc_open(std::uint32_t ms) {
    if (inline_now() || ms < 1 || ms > 1000) return false;   // a live sink is attached: never open the lines
    K2LOG("[PD] CC open %u ms\n", static_cast<unsigned>(ms));
    pins::cc1_switch.clear();
    pins::cc2_switch.clear();
    delay_ms(ms);
    cc_switches(false, g_cc_mask);
    g_detach.reset();   // the open time is not a detach
    g_new_source = true;
    return true;
}

void set_inline(bool on) {
    if (on == g_inline) return;
    g_inline = on;
    // Inline the charger must see the sink's Rd alone: ours comes off (measure only)
    // and goes back when inline ends.
    apply_rd();   // inline: our Rd comes off (the sink's Rd alone), back when inline ends
    g_cc_sink_mv = -1;
    g_detach.inline_begin();
    pdtrace::link(g_inline, false);
    usb::log(on ? "[PD] Inline: real sink attached, active operations need confirm\n"
                : "[PD] Inline cleared\n");
}
bool inline_mode() { return inline_now(); }

namespace {
bool set_rd_for_run(bool on, bool keep_scan) {
    if (on && g_inline) return false;   // the sink's Rd must stay alone on the line
    if (!on && !keep_scan) self_power_restore();   // the run is over: the boot-mode Rd pin comes back
    if (g_rd_start == on) return true;
    g_rd_start = on;
    if (!on && !keep_scan) g_scanned_valid = false;
    if (on) g_start_requested = false;
    else if (g_start_requested) { g_start_requested = false; g_sink.release(systick::millis()); }   // back to the 5 V we asked for
    apply_rd();
    usb::log(on ? "[PD] Run start: Rd presented\n" : keep_scan ? "[PD] Run start: Rd paused\n" : "[PD] Run start over: Rd off\n");
    return true;
}
}  // namespace

// Charger test unit lifecycle (docs/CHARGER_TEST_FLOW.md). Every unit starts detached and attaches with its own session mode.
// unit_detach(): Rd off (the source sees a detach and drops its output), the 5 V default, the sink and source state forgotten,
// the PD-silent mode off. The scanned cable is kept (the same cable stays plugged in).
void unit_detach() {
    g_reread_owns_rd = false;
    g_reread_made_contract = false;
    g_reread.abort();
    g_prep.abort();
    self_power_leave();          // pinned boot-mode Rd would keep the charger attached: drop the pin too
    set_rd_for_run(false, true);
    g_rd_op = false;             // the unit's detach is Rd off, whatever the Rd gate's linger still says
    g_rd_gate.reset();
    g_contract_linger.reset();
    apply_rd();
    g_sink.detach();
    g_legacy_only = false;
    g_detach.reset();
    g_engine.reset_source();
    g_new_source = true;
    g_pd_traffic = false;
    g_last_rx_ms = systick::millis();
    usb::log("[PD] Unit detach: Rd off, sink forgotten\n");
}
// unit_attach(legacy): Rd on as a fresh attach. Legacy = PD-silent: the source's caps are heard (and recorded) but never answered
// with GoodCRC or a Request, so a charger with a legacy mode gives PD up; otherwise the START path (automatic PDO 1 request).
void unit_attach(bool legacy) {
    g_reread_owns_rd = false;
    g_reread_made_contract = false;
    g_legacy_only = legacy;
    self_power_restore();        // back to the boot-mode pin before presenting Rd for the unit
    g_sink.detach();
    g_detach.reset();
    g_engine.reset_source();
    g_new_source = true;
    g_pd_traffic = false;
    g_last_rx_ms = systick::millis();
    if (legacy && !g_sop_prime_hook) {   // a PD-silent attach starts with Auto-CRC off, whatever demand (the spoof) held it until now
        g_crc.reset();                   // else its 300 ms linger would GoodCRC the charger's first caps: it keeps PD and its
        if (g_crc_on) { g_crc_on = false; write_sw1(); }   // D+/D- protocols stay dead (Baseus 70W, hardware 2026-11-07)
    }
    set_rd_for_run(true, false);
    usb::log(legacy ? "[PD] Unit attach: PD-silent\n" : "[PD] Unit attach: PD\n");
}
// unit_hold(legacy): the unit boundary of a self-powered run. The pinned Rd is never touched (no detach for the charger to
// answer with a VBUS cut, no second Rd line), CC stays closed. The rest is what unit_detach + unit_attach reset, without the wire.
void unit_hold(bool legacy) {
    g_reread_owns_rd = false;
    g_reread_made_contract = false;
    g_reread.abort();
    g_prep.abort();
    g_legacy_only = legacy;      // PD-silent: Auto-CRC drops with the run-start demand, caps are heard and never answered
    g_sink.detach();             // forget the contract; sends nothing
    if (!legacy) g_sink.soft_reset(systick::millis());   // the source re-announces its caps; the acked-caps path requests PDO 1
    g_detach.reset();
    g_engine.reset_source();
    g_new_source = true;
    g_pd_traffic = false;
    g_last_rx_ms = systick::millis();
    usb::log(legacy ? "[PD] Unit hold: PD-silent, Rd kept\n" : "[PD] Unit hold: Soft_Reset, Rd kept\n");
}
bool rd_for_run(bool on) {   // a run start takes the Rd and the contract over from a re-read
    g_reread_owns_rd = false;
    g_reread_made_contract = false;
    if (g_ra) {   // the Ra mode goes with the run: owned from the START, released when the run is over
        g_ra_run = on;
        if (!on) ra_set(false);
    }
    return set_rd_for_run(on, false);
}
void rd_for_run_pause() { g_reread_owns_rd = false; g_reread_made_contract = false; set_rd_for_run(false, true); }
bool rd_for_run_on() { return g_rd_start; }

std::uint32_t cable_seq() { return g_engine.cable_acks; }
std::uint32_t session() { return g_session.id(); }
const pd::CableLimits& physical_cable() {
    static constexpr pd::CableLimits unknown;
    if (g_physical_session == g_session.id()) return g_physical_cable;
    return g_rd_start && g_scanned_valid ? g_scanned_cable : unknown;
}
const pd::Revision& source_revision() { return g_engine.ext.revision; }

std::uint32_t typec_ma() {
    if (g_active_cc == 0) return 0;
    return pd::typec_only_ma(systick::millis() - g_attach_ms, g_session.fresh(), g_bc_max, inline_now(), g_pd_traffic);
}
bool pd_traffic() { return g_active_cc != 0 && g_pd_traffic; }
bool pd_seen_attach() { return g_active_cc != 0 && g_pd_seen_attach; }

namespace {

void log_found_cable() {
    const pd::Emarker& e = g_engine.cable;
    K2LOG("[EMARK] E-Marker detected: VID=0x%04X (%s), PID=0x%04X\n", e.vid, e.vendor_str, e.pid);
    K2LOG("[EMARK] Rating: %d.0A / %dV (%dW), Speed: %s\n", e.max_current_a, e.max_voltage_v,
                e.max_power_w, e.speed_str);
    K2LOG("[EMARK] Cable Type: %s, HW: v%d, FW: v%d, XID: 0x%08X\n", e.is_active ? "Active" : "Passive",
                e.hw_rev, e.fw_rev, unsigned(e.xid));
    log_cable(e);
}

// A source is attached: it powers VCONN and sends SOP' Discover Identity itself. Touch nothing on CC
// (injecting would drop Rd, the source would see a detach and take VBUS and VCONN with it); make sure
// SOP' receive is on and report what poll() has already decoded.
bool sniff_emarker() {
    if (g_engine.cable.valid) {
        K2LOG("[EMARK] Source attached: cable data captured from the source's own SOP' exchange\n");
        log_found_cable();
        return true;
    }
    K2LOG("[EMARK] Source attached: no VCONN injection (it would drop the charger), listening only\n");
    K2LOG("[EMARK] The source asks the cable once at attach: replug the charger end, or scan with the source removed\n");
    return false;
}

}  // namespace

namespace {

bool g_cable_open = false;   // a cable_session() body is running: the scan's VCONN, routing and Auto-CRC are still in place
std::uint8_t g_cable_id = 0; // MessageID of the next SOP' request of the session (the scan's Discover Identity used 0)

// One SOP' request of a session and its answer. Same transport as the scan (flush, request, poll INTERRUPTA and the RX FIFO),
// MessageID handling as transmit(): advanced when the chip saw the GoodCRC (TXSENT) or when nothing was seen at all (the
// GoodCRC may have come late), kept on RETRYFAIL.
bool cable_exchange_impl(const emk_learn::Request& req, emk_learn::Reply& rep, std::uint32_t timeout_ms) {
    rep = {};
    if (!g_cable_open) return false;
    const unsigned n = req.n > emk_learn::kMaxObjs ? emk_learn::kMaxObjs : req.n;
    constexpr unsigned kMsgVdm = 0x0F;   // Vendor_Defined data message
    const std::uint16_t header = std::uint16_t(kMsgVdm | (unsigned(req.spec_rev & 3u) << 6) | (unsigned(g_cable_id & 7u) << 9) |
                                               (n << 12));
    std::array<std::uint8_t, 4 + 1 + 2 + 4 * emk_learn::kMaxObjs + 3> buf;
    unsigned len = 0;
    for (std::uint8_t b : {0x12, 0x12, 0x1B, 0x1B}) buf[len++] = b;   // SOP'
    buf[len++] = std::uint8_t(0x80 | (2 + 4 * n));
    buf[len++] = std::uint8_t(header);
    buf[len++] = std::uint8_t(header >> 8);
    const unsigned objs_at = len;
    for (unsigned i = 0; i < n; i++)
        for (unsigned k = 0; k < 4; k++) buf[len++] = std::uint8_t(req.w[i] >> (8 * k));
    buf[len++] = 0xFF;   // JAM_CRC
    buf[len++] = 0x14;   // EOP
    buf[len++] = 0xFE;   // TXOFF
    flush_fifos();
    delay_ms(2);
    clear_irqs();
    fifo_write_tx(header, {buf.data() + objs_at, 4 * n}, {buf.data(), len});
    wr(kControl0, kTxStart);
    bool sent = false, failed = false;
    const std::uint32_t t0 = systick::millis();   // wall clock: every bit-banged read below costs ~2 ms of it
    while (systick::millis() - t0 < timeout_ms && !failed && !rep.got) {
        delay_ms(2);
        std::uint8_t ia = 0;
        if (rd(kInterruptA, ia)) {
            if (ia & kIntHardRst) g_hr_latched = true;
            if (ia & kIntTxSent) sent = true;
            if (ia & kIntRetryFail) failed = true;
        }
        std::uint8_t status1;
        while (!failed && !rep.got && rd(kStatus1, status1) && !(status1 & kRxEmpty)) {
            std::array<std::uint8_t, 3> pre;   // token, PD header
            if (!fifo_read(pre)) break;
            const std::uint16_t h = std::uint16_t(pre[1] | (pre[2] << 8));
            const unsigned num_do = (h >> 12) & 7;
            std::array<std::uint8_t, 36> body;   // data objects + 4 CRC bytes
            if (!fifo_read({body.data(), num_do * 4 + 4})) break;
            pdtrace::rx(pre[0], h, {body.data(), num_do * 4 + 4});
            if (sop_from_token(pre[0]) == pd::Sop::Sop) continue;   // the source side: not the cable
            if (num_do == 0 && (h & 0x1F) == pd::kCtlGoodCrc) { sent = true; continue; }   // the cable's GoodCRC of our request
            rep.got = true;
            rep.header = h;
            rep.n = std::uint8_t(num_do);
            for (unsigned i = 0; i < num_do && i < rep.w.size(); i++)
                rep.w[i] = std::uint32_t(body[4 * i]) | (std::uint32_t(body[4 * i + 1]) << 8) |
                           (std::uint32_t(body[4 * i + 2]) << 16) | (std::uint32_t(body[4 * i + 3]) << 24);
        }
    }
    if (!failed) g_cable_id = (g_cable_id + 1) & 7;   // only a RETRYFAIL keeps the id (see transmit())
    K2LOG("[EMK] cable tx hdr 0x%04X id %d: %s%s\n", header, int((header >> 9) & 7),
                failed ? "RETRYFAIL (no GoodCRC)" : sent || rep.got ? "GoodCRC" : "no GoodCRC seen",
                rep.got ? ", answered" : "");
    return !failed && (sent || rep.got);
}

bool read_emarker_impl(bool confirmed, pd::EmarkerMode mode, CableBody body, void* ctx) {
    if (mode == pd::EmarkerMode::Sniff) return sniff_emarker();
    if (!emkra::vconn_switch_allowed(g_ra)) {
        K2LOG("[EMARK] Refused: the Ra mode is on (the chip's VCONN switch would fight the Ra on CC1)\n");
        return false;
    }
    if (inline_now() && !confirmed) {
        K2LOG("[EMARK] Refused: inline with a live sink; VCONN and SOP' may interrupt charging\n");
        return false;
    }
    const bool tx_was_allowed = g_tx_allowed;
    g_tx_allowed = true;   // the VCONN scan is an active feature: its SOP' Discover Identity goes on the wire
    g_engine.reset_cable();
    g_scanned_valid = false;
    struct Pre { std::uint8_t power = kPwrAll, sw0 = 0, sw1 = 0; } pre;
    // Snapshot; if a read fails, fall back to what the driver itself would have set.
    std::uint8_t v;
    if (rd(kPower, v)) pre.power = v;
    if (rd(kSwitches0, v)) pre.sw0 = v;
    if (rd(kSwitches1, v)) pre.sw1 = v;
    K2LOG("[EMARK] Scanning for E-Marker (Dual-orientation SOP')...\n");
    K2LOG("[EMK] snapshot POWER=0x%02X SW0=0x%02X SW1=0x%02X inline=%d active_cc=%d cable_acks=%u\n",
                pre.power, pre.sw0, pre.sw1, int(inline_now()), int(g_active_cc), unsigned(g_engine.cable_acks));
    cc_switches(true);
    g_cable_read = true;   // every flush below writes CONTROL1 with SOP' and SOP'' receive on

    // A: CC2 powers VCONN (SWITCHES0 0x24), BMC on CC1 (SWITCHES1 0x25).
    // B: CC1 powers VCONN (SWITCHES0 0x18), BMC on CC2 (SWITCHES1 0x26).
    bool found = true;
    if (scan_orientation(0x24, 0x25)) {
        K2LOG("[EMARK] Response on Orientation A (CC1 BMC, CC2 VCONN)\n");
    } else if (scan_orientation(0x18, 0x26)) {
        K2LOG("[EMARK] Response on Orientation B (CC2 BMC, CC1 VCONN)\n");
    } else {
        found = false;
    }

    // A session body (`emarker learn`) talks to the cable now, with the scan's VCONN, routing and Auto-CRC still in place.
    if (found && body != nullptr) {
        g_cable_open = true;
        g_cable_id = 1;   // the scan's Discover Identity was id 0
        K2LOG("[EMK] cable session open\n");
        body(ctx);
        g_cable_open = false;
        K2LOG("[EMK] cable session closed\n");
    }
    g_cable_read = false;
    g_tx_allowed = tx_was_allowed;   // the scan is over: back to whatever the demand model says

    // Back to the pre-scan state, whatever happened above: VCONN off, then the registers the scan
    // touched are written back from the snapshot taken before it (Rd, measure line, BMC routing,
    // Auto-CRC, SOP' receive enables). A failed scan must leave nothing behind.
    cc_switches(false, g_cc_mask);
    wr(kPower, pre.power);
    wr(kSwitches0, pre.sw0);
    wr(kSwitches1, std::uint8_t((pre.sw1 & ~kAutoCrc) | (g_crc_on ? kAutoCrc : 0)));  // Auto-CRC as before the scan
    wr(kControl1, ctl1());
    flush_fifos();
    clear_irqs();
    g_detach.reset();
    {
        std::uint8_t p = 0, a = 0, b = 0, c1 = 0;
        rd(kPower, p); rd(kSwitches0, a); rd(kSwitches1, b); rd(kControl1, c1);
        K2LOG("[EMK] restored POWER=0x%02X SW0=0x%02X SW1=0x%02X CTRL1=0x%02X found=%d\n", p, a, b, c1,
                    int(found));
    }

    if (!found) {
        K2LOG("[EMARK] No E-Marker detected / Standard 3A cable\n");
        return false;
    }
    log_found_cable();
    if (!inline_now()) { g_scanned_cable = pd::cable_limits(g_engine.cable); g_scanned_valid = true; }
    return true;
}

}  // namespace

bool read_emarker(bool confirmed, pd::EmarkerMode mode) { return read_emarker_impl(confirmed, mode, nullptr, nullptr); }

bool cable_session(bool confirmed, CableBody body, void* ctx) {
    if (body == nullptr) return false;
    return read_emarker_impl(confirmed, pd::EmarkerMode::Vconn, body, ctx);
}

bool cable_exchange(const emk_learn::Request& req, emk_learn::Reply& rep, std::uint32_t timeout_ms) {
    return cable_exchange_impl(req, rep, timeout_ms);
}

// ---- active operations, run from poll() ----

bool line_idle(std::uint32_t now) { return g_rx_empty && now - g_last_rx_ms >= kIdleQuietMs; }

// One step of the trigger prepare: cycle the CC lines so the source re-announces to an ACKing chip,
// then fire the stored Request 5 ms after its caps.
// What was asked and what goes on the wire: the choice's 0 means "PDO maximum", the plan holds the
// effective mV / mA and the RDO.
void log_trigger(const pd::Choice& c, pd::Err e) {
    const pd::Plan& p = g_sink.plan();
    if (e == pd::Err::Ok)
        K2LOG("[PD] t=%u Trigger pos %d (asked %d mV %d mA) -> %d mV %d mA RDO 0x%08X session %u: ok\n",
                    unsigned(systick::millis()), c.pos, c.mv, c.ma, p.mv, p.ma, unsigned(p.rdo),
                    unsigned(g_session.id()));
    else
        K2LOG("[PD] Trigger pos %d (asked %d mV %d mA): %s\n", c.pos, c.mv, c.ma, pd::err_name(e));
}

void prep_tick(std::uint32_t now) {
    switch (g_prep.tick(now, g_caps_count)) {
        case pd::TriggerPrep::Act::None: break;
        case pd::TriggerPrep::Act::OpenCc:
            K2LOG("[PD] t=%u Trigger prepare: caps were not ACKed, cycling CC (Auto-CRC on)\n", unsigned(now));
            pins::cc1_switch.clear();
            pins::cc2_switch.clear();
            break;
        case pd::TriggerPrep::Act::CloseCc:
            cc_switches(false, g_cc_mask);
            g_msg_id = 0;
            g_new_source = true;
            g_detach.reset();  // the 150 ms the line was open is not a detach
            K2LOG("[PD] t=%u Trigger prepare: CC closed, waiting for Source_Capabilities\n", unsigned(now));
            break;
        case pd::TriggerPrep::Act::Send: {
            if (!g_session.fresh()) {
                K2LOG("[PD] t=%u Trigger not sent: no Source_Capabilities in this attach (session %u)\n",
                            unsigned(now), unsigned(g_session.id()));
                break;
            }
            const pd::Err e = g_sink.trigger(g_prep_choice, now);
            log_trigger(g_prep_choice, e);
            break;
        }
    }
}

pd::RereadLink reread_link() {
    return {.contract = g_sink.status().state == pd::SinkState::Ready,
            .pdos = std::uint8_t(g_engine.caps.valid ? g_engine.caps.num_pdos : 0),
            .rev = std::uint8_t(g_engine.caps.valid ? g_engine.caps.spec_rev : 0)};
}

// Once per pass: logs a finished re-read (Black Box mark and log line) and ends the attach it made itself: back to PDO 1,
// the contract forgotten (Auto-CRC and the sink demand go with it) and the Rd it presented dropped. At once when nothing
// came of the run, after the linger otherwise (the contract and the caps stay readable: k2_remote pdos). A run on a contract
// that was already there, or one a START / trigger / unit took over, is never touched; a held trigger (also one that took
// the re-read's contract over) keeps contract and Rd until `pd release`, then this linger starts over.
void reread_housekeeping(std::uint32_t now) {
    if (!g_reread.running() && !g_reread_logged) {
        g_reread_logged = true;
        const pd::RereadStatus& rs = g_reread.status();
        const unsigned held = g_engine.caps.valid ? g_engine.caps.num_pdos : 0;
        pdtrace::mark(pdtrace::kMarkReread, std::uint16_t(0x8000 | (rs.skipped_mask << 8) | rs.ok_mask),
                      std::uint16_t((rs.avs_seen ? 0x8000 : 0) | (rs.caps_rx << 8) | rs.pdos_last));
        K2LOG("[REREAD] %s ok=0x%02X skipped=0x%02X caps_rx=%u pdos %u -> %u (last list %u) avs=%d attach_failed=%d\n",
              rs.state == pd::RereadState::Aborted ? "aborted" : "done", unsigned(rs.ok_mask), unsigned(rs.skipped_mask),
              unsigned(rs.caps_rx), unsigned(rs.pdos_before), held, unsigned(rs.pdos_last), int(rs.avs_seen), int(rs.attach_failed));
        const bool nothing = rs.attach_failed || rs.state == pd::RereadState::Aborted;
        g_reread_release_at = nothing ? now : now + kRereadLingerMs;
    }
    if (g_trigger_took_over && (g_reread_owns_rd || g_reread_made_contract)) {
        // A user trigger (pd attach) took the contract over; the Rd the re-read presented stays on until the attach ends. The
        // sink's `held` cannot say this: the re-read's own START Request is held too. The trigger is the user's until
        // `pd release` (or a detach); the re-read's linger starts over after it (hardware 2026-11-06: Rd and VBUS went at +39 s).
        const pd::SinkStatus& ts = g_sink.status();
        g_reread_release_at = now + kRereadLingerMs;
        if (!ts.held && !g_prep.active()) g_trigger_took_over = false;
        return;
    }
    if ((g_reread_owns_rd || g_reread_made_contract) && !g_reread.running() && g_reread_logged &&
        std::int32_t(now - g_reread_release_at) >= 0) {
        const pd::SinkStatus& ss = g_sink.status();
        const bool quiet = ss.state == pd::SinkState::Idle || ss.state == pd::SinkState::Ready || ss.state == pd::SinkState::Failed;
        if (!quiet) return;   // an exchange in flight finishes first
        if (ss.state == pd::SinkState::Ready && ss.pos != 1) {   // back to the 5 V default first
            g_sink.release(now);
            return;
        }
        K2LOG("[REREAD] attach released: contract %d, Rd %d\n", int(g_reread_made_contract), int(g_reread_owns_rd));
        if (g_reread_owns_rd) set_rd_for_run(false, false);
        g_reread_owns_rd = false;
        g_reread_made_contract = false;
        g_sink.detach();
    }
}

// The linger of a Ready contract nobody holds ran out (pd::ContractLinger; the sink demand, Rd and Auto-CRC are already off
// by now). A contract sitting at another voltage, or in EPR mode, first goes back to the 5 V default: that Request is
// engaged again and starts a new linger. Otherwise the contract is forgotten, so the source sees the sink gone: Rd is off,
// and it drops VBUS and re-announces on its own. Never touches a held trigger, a run's own Rd or an operation in flight.
void contract_expired(std::uint32_t now) {
    const pd::SinkStatus& ss = g_sink.status();
    if (ss.state != pd::SinkState::Ready || ss.held) return;
    if (ss.pos != 1 || ss.epr_mode) {
        K2LOG("[PD] Contract unused for %u s: back to 5 V\n", unsigned(pd::kSinkIdleLingerMs / 1000));
        g_sink.release(now);
        return;
    }
    K2LOG("[PD] Contract released: Rd off, unused for %u s\n", unsigned(pd::kSinkIdleLingerMs / 1000));
    g_sink.detach();
}

// CC1 / CC2 net in mV from the ADC's newest sweep (raw * 3.3 V / 4096; the pins have no divider, so a 5 V VCONN reads at the rail).
bool cc_net_mv(std::uint16_t& cc1, std::uint16_t& cc2) {
    std::uint16_t r1 = 0, r2 = 0;
    if (!adc::latest_cc(r1, r2)) return false;
    cc1 = std::uint16_t(std::uint32_t(r1) * 3300u / 4096u);
    cc2 = std::uint16_t(std::uint32_t(r2) * 3300u / 4096u);
    return true;
}

static void vconn_mark() {
    std::uint16_t c1 = 0, c2 = 0;
    cc_net_mv(c1, c2);
    pdtrace::mark(pdtrace::kMarkVconn,
                  std::uint16_t(unsigned(g_vconn.state()) | unsigned(g_vconn.why()) << 4 | (g_vconn_applied ? 0x100u : 0u) | unsigned(g_active_cc) << 12),
                  g_active_cc == 1 ? c2 : c1);   // b = the net VCONN goes on
}

// VCONN off now, PC13 low, the chip's switch open, the engine forgotten.
void vconn_force_off(pd::VconnWhy why) {
    const bool was = g_vconn_applied;
    g_vconn.reset(why);
    g_vconn_applied = false;
    if (was) {
        pins::cc_vconn_gnd.write(g_ra);
        if (g_active_cc != 0) apply_rd();
        K2LOG("[VCONN] off (%u)\n", unsigned(why));
        vconn_mark();
    }
}

// Follows g_vconn: switches VCONN on / off. It goes only onto the line opposite the active one (the captive plug's other CC),
// only with a contract-less-safe line state: the other net must read like an Rp (above 2.0 V), not an Rd / Ra, and no sink inline.
void vconn_sync(std::uint32_t now) {
    const bool want = g_vconn.want_on();
    if (want && !g_vconn_applied) {
        std::uint16_t c1 = 0, c2 = 0;
        const bool have = cc_net_mv(c1, c2);
        const std::uint16_t other = g_active_cc == 1 ? c2 : c1;
        if (g_active_cc == 0 || inline_now() || !have || other < 2000 || !emkra::vconn_switch_allowed(g_ra)) {
            K2LOG("[VCONN] refused: cc=%u inline=%d adc=%d other net %u mV\n", unsigned(g_active_cc), int(inline_now()), int(have), unsigned(other));
            vconn_force_off(pd::VconnWhy::Refused);
            return;
        }
        pins::cc_vconn_gnd.write(true);
        g_vconn_applied = true;
        apply_rd();
        g_vconn_on_ms = now;
        g_vconn_logged = false;
        K2LOG("[VCONN] on: active cc%u, VCONN on cc%u (before: %u mV)\n", unsigned(g_active_cc), g_active_cc == 1 ? 2u : 1u, unsigned(other));
        vconn_mark();
    } else if (!want && g_vconn_applied) {
        vconn_force_off(g_vconn.why());   // the engine already gave the role up (partner's PS_RDY, timeout)
    }
    if (g_vconn_applied && !g_vconn_logged && static_cast<std::int32_t>(now - g_vconn_on_ms) >= 5) {
        g_vconn_logged = true;
        std::uint16_t c1 = 0, c2 = 0;
        cc_net_mv(c1, c2);
        K2LOG("[VCONN] settled: cc1 %u mV, cc2 %u mV\n", unsigned(c1), unsigned(c2));
    }
}

// A pinned Rd means the source feeds the K2: a Hard Reset of our own would cut VBUS and kill it, so the sink engine only fails
// (it never transmits one). The explicit sink_hard_reset() tool is not covered.
void sync_hard_reset_policy() { g_sink.set_never_hard_reset(g_pin_rd != 0); }

void active_tick() {
    const std::uint32_t now = systick::millis();
    sync_hard_reset_policy();
    prep_tick(now);
    const pd::EprInfo info_was = g_sink.status().epr_info;
    g_sink.tick(now);
    if (g_sink.status().epr_info != info_was) log_epr_info(g_sink.status().epr_info);
    g_vconn.tick(now);
    vconn_sync(now);
    {   // Fully active for the whole contract: a running or held trigger, Identify, a pending
        // Get_Source_Cap or the prepare. Passive again only after release / detach (plus the linger).
        const pd::SinkStatus& ss0 = g_sink.status();
        const bool ready = ss0.state == pd::SinkState::Ready;
        const bool in_flight = ss0.state != pd::SinkState::Idle && ss0.state != pd::SinkState::Failed && !ready;
        pd::CrcDemand d{.sink_active = false,
                        .sink_held = ss0.held,
                        .ident = g_ident.running() || g_probe.running() || g_reread.running(),
                        .getcap = g_getcap_armed || g_vconn.state() != pd::VconnState::Off,   // a VCONN swap needs our GoodCRC
                        .prep = g_prep.crc_wanted(),
                        .run_start = g_rd_start && !inline_now() && !g_legacy_only,   // legacy-only: no GoodCRC for the source's caps
                        .spoof = g_sop_prime_hook != nullptr,   // the armed spoof needs our GoodCRC on SOP', no Rd
                        .self_hold = pd::self_hold_wanted(g_pin_rd, inline_now(), g_legacy_only)};   // pinned Rd: the brick feeds us, keep its PD alive
        // A Ready contract nobody holds counts only for kSinkIdleLingerMs after the last thing that used it.
        const bool engaged = in_flight || pd::rd_demand(d);
        const bool counts = g_contract_linger.update(now, engaged);
        d.sink_active = in_flight || (ready && counts);
        update_crc(pd::crc_demand(d), pd::rd_demand(d));
        // The TX choke point's input: the same demand that drives Auto-CRC (plus the explicit hard-reset tool).
        // transmit() refuses everything while this is false, so a passive K2 can never put a frame on CC.
        g_tx_allowed = pd::crc_demand(d) || g_hr_tool_armed;
        if (ready && !counts) contract_expired(now);
    }
    pd::TxMsg m;
    bool sent = false;
    if (g_sink.take_tx(m)) {
        if (m.hard_reset) g_hr_tool_armed = false;   // the tool's one Hard Reset went out (or was refused)
        g_sink.on_tx_result(transmit(m), now);
        sent = true;
    } else if (g_vconn.take_tx(m, g_sink.rev())) {
        const bool ok = transmit(m);
        K2LOG("[VCONN] TX type %u: %s\n", unsigned(m.header & 0x1F), ok ? "GoodCRC" : "no GoodCRC");
        vconn_mark();
        sent = true;
    }
    trace_sink();
    const pd::SinkState ss = g_sink.status().state;
    const bool sink_quiet = ss == pd::SinkState::Idle || ss == pd::SinkState::Ready || ss == pd::SinkState::Failed;
    if (g_ident.running() && (!sink_quiet || !line_idle(now))) {   // [ID] probe: why Identify is not transmitting
        static std::uint32_t last_log;
        if (now - last_log >= 1000) {
            last_log = now;
            K2LOG("[ID] blocked: sink_quiet=%d rx_empty=%d quiet_ms=%u inline=%d step=%d\n", int(sink_quiet),
                        int(g_rx_empty), unsigned(now - g_last_rx_ms), int(inline_now()), int(g_ident.status().step));
        }
    }
    if (g_probe.running()) {   // ticked before the idle gate so its deadline holds on a busy line
        const bool idle = !sent && sink_quiet && line_idle(now);
        g_probe.tick(now, idle);
        const bool tx = idle && g_probe.take_tx(m);
        if (tx) g_probe.on_tx_result(transmit(m), now);
        if (!g_probe.running() && !g_probe_logged) {
            g_probe_logged = true;
            const pd::ProbeStatus& ps = g_probe.status();
            K2LOG("[PROBE] done ok=%u skipped=%u\n", unsigned(ps.ok_mask), unsigned(ps.skipped_mask));
        }
        if (tx) return;
    }
    if (g_reread.running() || !g_reread_logged) {   // same shape as the probe: ticked before the idle gate
        const bool idle = !sent && sink_quiet && line_idle(now);
        g_reread.tick(now, idle, reread_link());
        const bool tx = idle && g_reread.take_tx(m);
        if (tx) g_reread.on_tx_result(transmit(m) || g_tx_timed_out, now);   // a timeout may still have been delivered: wait for the answer
        if (tx) return;
    }
    if (sent || !sink_quiet || !line_idle(now)) return;

    // Not while the Xiaomi auth chain runs: a Get_Source_Cap of ours in the middle of it kills the chain; it stays armed.
    if (g_getcap_armed && std::int32_t(now - g_getcap_at) >= 0 && !g_sink.xiaomi_chain_active(now)) {
        g_getcap_armed = false;
        if (g_getcap_force || !g_engine.caps.valid) {  // forced, or nothing arrived by itself
            g_getcap_force = false;
            pd::TxMsg gc;
            gc.header = pd::make_header(pd::kCtlGetSourceCap, 0, g_sink.rev());
            K2LOG("[PD] No Source_Capabilities yet: sending Get_Source_Cap\n");
            transmit(gc);
            return;
        }
    }
    if (g_ident.running()) {
        // Inline, the SVIDs worth a Discover Modes are the sink's when it listed any; else the charger's.
        const pd::VdmInfo& iv = inline_now() && g_engine.sink_vdm.num_svids ? g_engine.sink_vdm : g_engine.vdm;
        std::array<std::uint16_t, pd::kMaxSvids> sv = iv.svids;
        g_ident.tick(now, true, {sv.data(), iv.num_svids});
        if (g_ident.take_tx(m)) g_ident.on_tx_result(transmit(m), now);
    }
}

void on_int_n() { g_irq = true; }

void new_source() { g_new_source = true; }

void clear_source() {
    g_engine.reset_source();
    g_new_source = true;
}

// Partner gone: RESET=0x03 (SW_RES + PD_RESET), then SWITCHES0=0, then the whole
// passive profile again (the soft reset wiped it) and a fresh start for every protocol engine.
static void detach_reset(std::uint8_t status0) {
    pdtrace::cc(pdtrace::CcEvent::Detach, g_active_cc, status0);
    if (g_ra && !emkra::survives_detach(g_ra_run)) {
        g_ra = false;
        K2LOG("[EMKRA] off (detach)\n");
    }
    pins::cc_vconn_gnd.write(g_ra);   // VCONN off before anything else changes (the Ra mode keeps PC13 high)
    g_vconn_applied = false;
    g_vconn.reset(pd::VconnWhy::Abort);
    // Passive: no SW_RES. It restores the chip's power-on SWITCHES0 (Rd on both lines) until the next
    // write, which a host and a dock negotiating through the K2 can see.
    wr(kReset, g_pin_rd != 0 ? std::uint8_t(kSwReset | kPdReset) : kPdReset);
    wr(kSwitches0, g_pin_rd);
    g_inline = false;
    g_rd_op = false;
    g_cc_sink_mv = -1;
    g_active_cc = 0;
    g_bc_max = 0;
    g_cc_mask = 3;  // the next scan tries every routing again
    g_vbus_mv = -1;
    g_detach.reset();
    g_sink.detach();
    g_prep.abort();
    g_ident.abort(pd::IdWhy::Detached);
    g_probe.abort();
    g_reread.abort();
    g_getcap_armed = false;
    g_getcap_force = false;
    g_msg_id = 0;
    g_engine.reset_cable();  // the next sniff must not report the old cable as heard
    g_engine.reset_sink();
    g_engine.reset_source();  // the offers go with the partner: nothing of charger A may serve charger B
    g_new_source = true;
    g_caps_acked = false;
    g_session.detach();
    g_engine.sink = {};
    configure_passive();
    flush_fifos();
    // No Rd hold: plain boot has no Rd to hold off, and a pinned boot mode needs its Rd back at once (a cheap
    // charger never turns VBUS on without it, hardware 2026-11-01). The hold only delayed the next scan.
    g_rd_hold.cancel();
}

// The START wait loop blocks the main loop, so poll()'s 2 Hz scan never runs inside it. Without this the charger's Rp
// is noticed only after the wait, and the caps it sends meanwhile get no Request (Lenovo C135, hardware 2026-11-03:
// attach at the next scan, caps 27 ms later, Hard Reset 57 ms after those). One scan per call, ~25 ms of I2C.
void attach_scan() {
    if (g_active_cc != 0 || g_rd_hold.held(systick::millis())) return;
    detect_cc_orientation();
}

void debug_cc_matrix(uint8_t v, uint16_t modes) {
    pins::cc_vconn_gnd.mode(reg::Mode((modes >> 0) & 0xF));
    pins::cc1_switch.mode(reg::Mode((modes >> 4) & 0xF));
    pins::cc2_switch.mode(reg::Mode((modes >> 8) & 0xF));
    pins::cc_vconn_gnd.write(emkra::pc13_high(v & 1, g_ra));   // the Ra mode keeps PC13 high
    pins::cc1_switch.write((v >> 1) & 1);
    pins::cc2_switch.write((v >> 2) & 1);
}

namespace {

// The PC13/14/15 state `debug cc-probe` snapshots and puts back: the output latch (what the pin drives; the input
// register reads 0 in analog mode) and the 4-bit CRH mode.
bool probe_pin_level(const pins::Pin &p) {
#ifdef K2_HOST_PINS
    return p.read();
#else
    return (reg::gpio(p.port).ODR & p.mask()) != 0;
#endif
}
unsigned probe_pin_mode(const pins::Pin &p) {
#ifdef K2_HOST_PINS
    (void)p;
    return unsigned(reg::Mode::OutPp50);
#else
    const reg::R32 &cr = p.n < 8 ? reg::gpio(p.port).CRL : reg::gpio(p.port).CRH;
    return (cr >> ((p.n & 7u) * 4u)) & 0xFu;
#endif
}
std::uint8_t probe_levels() {
    return std::uint8_t((probe_pin_level(pins::cc_vconn_gnd) ? 1 : 0) | (probe_pin_level(pins::cc1_switch) ? 2 : 0) |
                        (probe_pin_level(pins::cc2_switch) ? 4 : 0));
}
std::uint16_t probe_modes() {
    return std::uint16_t(probe_pin_mode(pins::cc_vconn_gnd) | probe_pin_mode(pins::cc1_switch) << 4 |
                         probe_pin_mode(pins::cc2_switch) << 8);
}
void probe_set_levels(std::uint8_t v) {
    pins::cc_vconn_gnd.write(v & 1);
    pins::cc1_switch.write((v >> 1) & 1);
    pins::cc2_switch.write((v >> 2) & 1);
}
void probe_set_modes(std::uint16_t m) {
    pins::cc_vconn_gnd.mode(reg::Mode((m >> 0) & 0xF));
    pins::cc1_switch.mode(reg::Mode((m >> 4) & 0xF));
    pins::cc2_switch.mode(reg::Mode((m >> 8) & 0xF));
}

}  // namespace

ccprobe::Refusal cc_probe(const ccprobe::Args &a, VbusFn vbus_mv, ccprobe::Result &out) {
    out = {};
    std::uint8_t sw0 = 0, sw1 = 0, st0 = 0;
    if (!rd(kSwitches0, sw0) || !rd(kSwitches1, sw1) || !rd(kStatus0, st0)) return ccprobe::Refusal::Bus;

    ccprobe::Ctx c;
    c.attached = g_active_cc != 0 || g_inline;
    const pd::SinkState ss = g_sink.status().state;
    c.contract = ss != pd::SinkState::Idle && ss != pd::SinkState::Failed;
    c.rd = g_rd_op || g_rd_start || g_ra || g_pin_rd != 0 || (sw0 & (kPdwn1 | kPdwn2)) != 0;   // the Ra mode is an active attach
    c.engine_running = g_ident.running() || g_probe.running() || g_reread.running() || g_prep.active();
    c.vconn_on = g_vconn_applied || g_vconn.state() != pd::VconnState::Off;
    c.vbus_mv = vbus_mv ? vbus_mv() : -1;
    ccprobe::Refusal why = ccprobe::refusal(c);
    if (why == ccprobe::Refusal::None && (st0 & ccprobe::kStatus0VbusOk)) why = ccprobe::Refusal::VbusHigh;   // the chip sees VBUS even when the monitor does not
    if (why != ccprobe::Refusal::None) return why;

    // Snapshot, then apply: pin modes (only when asked), levels, and last the SWITCHES0 value.
    out.sw0 = sw0; out.sw1 = sw1;
    out.pc = probe_levels();
    out.modes = probe_modes();
    const ccprobe::Plan plan = ccprobe::plan(a.ms);
    if (a.modes != ccprobe::kModesKeep) probe_set_modes(a.modes);
    probe_set_levels(a.pc);
    wr(kSwitches0, a.sw0);
    const std::uint32_t t0 = systick::micros();

    // One sample: the ADC's newest CC sweep, VBUS, and STATUS0 with the measure switch on each line in turn. The
    // chip's comparator gets no settle time here (a 2 ms wait would swallow the +1 ms sample): STATUS0 may lag the switch
    // by a few tens of microseconds. SWITCHES0 goes back to the requested value afterwards.
    bool vbus_up = false;
    const auto take = [&](ccprobe::Sample &s) {
        s.t_us = systick::micros() - t0;
        std::uint16_t r1 = 0, r2 = 0;
        if (adc::latest_cc(r1, r2)) { s.cc1_raw = r1; s.cc2_raw = r2; }
        const std::int32_t v = vbus_mv ? vbus_mv() : -1;
        if (v >= 0) s.vbus_mv = std::uint16_t(v > 0xFFFE ? 0xFFFE : v);
        std::uint8_t a1 = 0, a2 = 0;
        wr(kSwitches0, ccprobe::measure_sw0(a.sw0, 1)); rd(kStatus0, a1);
        wr(kSwitches0, ccprobe::measure_sw0(a.sw0, 2)); rd(kStatus0, a2);
        wr(kSwitches0, a.sw0);
        s.st0_cc1 = a1; s.st0_cc2 = a2;
        vbus_up = ccprobe::vbus_abort(v, std::uint8_t(a1 | a2));
    };

    std::uint8_t n = 0;
    std::uint32_t vbus_checked = 0;
    for (; n < plan.n && !vbus_up; n++) {
        const std::uint32_t due = std::uint32_t(plan.at_ms[n]) * 1000u;
        while (!vbus_up) {
            const std::uint32_t el = systick::micros() - t0;
            if (el >= due) break;
            service();
            std::uint8_t s = 0;
            if (rd(kStatus0, s) && ccprobe::vbus_abort(-1, s)) { vbus_up = true; break; }
            if (vbus_mv && el - vbus_checked >= 5000) {
                vbus_checked = el;
                if (ccprobe::vbus_abort(vbus_mv(), 0)) { vbus_up = true; break; }
            }
            if (due - el > 1500) delay_ms(1); else systick::delay_us(50);   // delay_ms keeps the USB endpoint serviced
        }
        take(out.s[n]);   // the sample that was due, or the abort sample in its slot
    }
    out.n = n;
    if (vbus_up) out.flags |= ccprobe::kFlagAborted;

    // Restore in the reverse order, then read it all back: the flag says whether it landed.
    for (int attempt = 0; attempt < 2; attempt++) {
        wr(kSwitches0, sw0);
        probe_set_levels(out.pc);
        if (a.modes != ccprobe::kModesKeep) probe_set_modes(out.modes);
        std::uint8_t b0 = 0xFF, b1 = 0xFF;
        if (rd(kSwitches0, b0) && rd(kSwitches1, b1) && b0 == sw0 && b1 == sw1 && probe_levels() == out.pc &&
            probe_modes() == out.modes) {
            out.flags |= ccprobe::kFlagRestored;
            break;
        }
    }
    return ccprobe::Refusal::None;
}

void poll() {
    static std::uint32_t poll_count;
    poll_count++;
    static bool notes_logged = false;
    boot_readback_service();
    if (!notes_logged && g_readback_n == kBootReadbacks) {   // the 3 s readback closes the record
        notes_logged = true;
        for (unsigned i = 0; i < g_boot_note_n; i++)
            K2LOG("[BOOT] t=%u %s=0x%02X\n", unsigned(g_boot_notes[i].ms), g_boot_notes[i].kind == 0 ? "sw0" : "cc", unsigned(g_boot_notes[i].val));
        for (unsigned i = 0; i < g_readback_n; i++)
            K2LOG("[BOOT] t=%u sw0 readback=0x%02X\n", unsigned(g_readbacks[i].ms), unsigned(g_readbacks[i].val));
        K2LOG("[BOOT] cc write trace: %u kept, %u more not kept; Rd pin 0x%X inherited %u\n", g_boot_note_n, g_boot_note_lost,
              unsigned(g_pin_rd), unsigned(g_rd_inherited));
    }
    // The hold is over: Rd comes back whether or not the scan finds anything (unless a sink is inline).
    if (g_active_cc == 0 && !inline_now() && g_rd_hold.take_expiry(systick::millis())) wr(kSwitches0, idle_rd());
    if (g_active_cc == 0 && poll_count % 25 == 0 && !g_rd_hold.held(systick::millis())) detect_cc_orientation();  // ~2 Hz at the 20 ms poll

    // Detach rule: VBUS below 3.0 V for 1 s, or no Rp on the active line for 500 ms. BMC
    // bursts pull the line low for microseconds, which the 500 ms hold rides out.
    if (g_active_cc != 0) {
        std::uint8_t status0 = 0;
        static std::uint8_t last_status0 = 0xFF;
        const bool got = rd(kStatus0, status0);
        if (got && (status0 & 3) > g_bc_max) g_bc_max = std::uint8_t(status0 & 3);
        if (got && status0 != last_status0) { last_status0 = status0; pdtrace::status0(status0); }
        const bool sink_ok = !inline_now() || pd::sink_present(g_cc_sink_mv);
        // Only a known reading inside the sink band proves a sink is on the line; the sink-left timer
        // starts after that, never for a line that has not shown a sink since inline began.
        if (inline_now() && g_cc_sink_mv >= 0 && sink_ok) g_detach.sink_seen();
        if (got && g_detach.update(systick::millis(), g_vbus_mv, (status0 & kComp) != 0, sink_ok)) {
            K2LOG("[FUSB302] Detach: %s (cc=%d mV, inline=%d)\n", pd::detach_reason_name(g_detach.reason()),
                         int(g_cc_sink_mv), int(inline_now()));
            detach_reset(status0);
        }
    }

    service();
}

// Receive and answer: hard reset, FIFO drain, transmit engines. Cheap when idle (two register
// reads), so the main loop runs it every millisecond to meet tSenderResponse (~30 ms).
void service() {
    pdtrace::service_called();
    // Legacy-only: a source that kept announcing during its give-up cycle refilled the caps; once it has
    // been quiet they are stale and would make the inline guard refuse every legacy trigger (PdContract).
    if (g_legacy_only && g_engine.caps.valid && pd_quiet_ms() >= 2000) g_engine.reset_source();
    // INT_N (or the 1 ms fallback) brings us here. Drain while the chip signalled again during the
    // pass or the RX FIFO was not empty; bounded so a stuck line cannot starve the main loop.
    for (int pass = 0; pass < 8; pass++) {
        g_irq = false;  // cleared first: an edge during this pass sets it again
        std::uint8_t ia = 0;  // a hard reset from the source shows up here, or was latched by an earlier clear_irqs() / transmit() read
        bool hard_reset = false;
        if (g_active_cc != 0) {
            if (rd(kInterruptA, ia) && (ia & kIntHardRst)) { hard_reset = true; pdtrace::irq(ia, 0, 0); }
            if (g_hr_latched) hard_reset = true;
        }
        g_hr_latched = false;
        if (hard_reset) {
            pdtrace::reset(pdtrace::ResetKind::HardRx);
            K2LOG("[PD] t=%u Hard reset received from the source\n", unsigned(systick::millis()));
            g_prep.abort();
            vconn_force_off(pd::VconnWhy::Abort);   // a hard reset ends the VCONN role (the source drops VBUS and every role)
            wr(kReset, kPdReset);
            flush_fifos();
            g_msg_id = 0;
            g_new_source = true;
            g_sink.on_hard_reset_rx(systick::millis());
            g_ident.abort(pd::IdWhy::HardReset);
            g_probe.abort();
            g_reread.on_link_reset();   // an attach still waiting for its contract goes on: the sink re-requests after a hard reset
        }

        g_rx_empty = true;
        for (int pkt = 0; pkt < 4; pkt++) {
            std::uint8_t status1;
            if (!rd(kStatus1, status1) || (status1 & kRxEmpty)) break;
            g_rx_empty = false;
            g_last_rx_ms = systick::millis();
            if (pkt == 0) pdtrace::poll_gap();

            std::array<std::uint8_t, 3> pre;  // token, PD header
            if (!fifo_read(pre)) break;
            const unsigned tok = pre[0] & 0xE0;
            if (tok != 0xE0 && tok != 0xC0 && tok != 0xA0) {  // not SOP, SOP' or SOP''
                pdtrace::rx_bad(pre[0], pre[1], pre[2]);
                flush_rx();
                break;
            }
            const std::uint16_t header = pre[1] | (pre[2] << 8);
            const unsigned num_do = (header >> 12) & 7;

            std::array<std::uint8_t, 36> body;  // data objects + 4 CRC bytes
            if (!fifo_read({body.data(), num_do * 4 + 4})) break;
            pdtrace::rx(pre[0], header, {body.data(), num_do * 4 + 4});
            if (num_do == 0 && !(header & 0x8000) && (header & 0x1F) == pd::kCtlSoftReset) pdtrace::reset(pdtrace::ResetKind::SoftRx);
            clear_irqs();
            // A Hard Reset caught by that read: this frame is the old link's (the Baseus sent it after a caps nobody
            // answered in time). Drop it; the next pass resets the engine and the FIFO.
            if (g_hr_latched) break;

            handle_frame(sop_from_token(pre[0]), header, {body.data(), num_do * 4});
        }
        if (!g_irq && g_rx_empty) break;
    }
    if (g_active_cc != 0) active_tick();
    else if (g_reread.running()) g_reread.tick(systick::millis(), false, reread_link());   // an attach whose source has not shown yet: its wait still ends
    reread_housekeeping(systick::millis());
    trace_sink();
}

// ---- active PD API ----

pd::TriggerRefusal trigger_refusal() {
    if (g_active_cc == 0) return inline_now() ? pd::TriggerRefusal::Inline : pd::TriggerRefusal::None;
    return pd::can_trigger({inline_now(), g_detach.sink_was_seen(), far_sink_seen()});
}

pd::Err sink_request(const pd::Choice& c, bool confirmed) {
    if (g_legacy_only) return pd::Err::Busy;
    (void)confirmed;  // the UI / remote layer owns the "above 5 V needs confirm" rule
    g_reread_made_contract = false;   // a trigger takes the contract over from a re-read (reread_start sets the flag after its own)
    if (const pd::TriggerRefusal r = trigger_refusal(); r != pd::TriggerRefusal::None) {
        K2LOG("[PD] trigger refused: inline (%s)\n", pd::trigger_refusal_name(r));
        return pd::Err::Inline;
    }
    if (g_active_cc == 0) return pd::Err::NoCaps;
    if (g_ident.running() || g_probe.running() || g_prep.active()) return pd::Err::Busy;
    if (!g_session.fresh()) {   // never a Request that answers nothing: the offers must be from this attach
        K2LOG("[PD] Trigger refused: no Source_Capabilities in this attach (session %u)\n",
                    unsigned(g_session.id()));
        return pd::Err::NoCaps;
    }
    if (!g_caps_acked && g_engine.caps.valid && c.pos != 0 && c.pos <= g_engine.caps.num_pdos) {
        // The caps went by un-ACKed (passive): re-announce first, the Request follows from active_tick.
        g_prep_choice = c;
        g_prep.start(systick::millis(), g_caps_count, false);
        K2LOG("[PD] Trigger pos %d %d mV %d mA: preparing (last caps not ACKed)\n", c.pos, c.mv, c.ma);
        g_trigger_took_over = true;
        return pd::Err::Ok;
    }
    const pd::Err e = g_sink.trigger(c, systick::millis());
    log_trigger(c, e);
    trace_sink();
    if (e == pd::Err::Ok) g_trigger_took_over = true;   // reread_start clears it again after its own internal Request
    return e;
}
void sink_release() {
    g_prep.abort();
    g_sink.release(systick::millis());
}
void sink_hard_reset() {
    if (inline_now() || g_active_cc == 0) return;
    g_hr_tool_armed = true;   // the explicit tool is an active feature: its one Hard Reset may go out even from idle
    g_sink.hard_reset(systick::millis());
}
void sink_set_cap(std::uint32_t mv) { g_sink.set_cap_mv(mv); }
void sink_cc(std::int32_t cc1_mv, std::int32_t cc2_mv) {
    g_cc1_mv = cc1_mv;   // kept for the attach scan's ADC cross-check (pd::rd_window_line) and its re-run on fresh levels
    g_cc2_mv = cc2_mv;
    cc_window_recheck();   // fresh levels while attached with no PD decoded yet: re-run the Rd-window cross-check
    // The CC line closest to 1.5 V; the other one is the unused line of the cable.
    auto dist = [](std::int32_t v) { const std::int32_t d = v - pd::kSinkBandCenterMv; return d < 0 ? -d : d; };
    g_cc_sink_mv = dist(cc1_mv) <= dist(cc2_mv) ? cc1_mv : cc2_mv;
}
void sink_vbus(std::int32_t mv) {
    g_vbus_mv = mv;
    g_sink.on_vbus(mv, systick::millis());
}
const pd::SinkStatus& sink_status() { return g_sink.status(); }
const pd::XiaomiTrace& xiaomi_trace() { return g_sink.xiaomi_trace(); }
pd::Err epr_info_request(bool force) { return g_sink.request_epr_info(systick::millis(), force); }
const pd::SourceCaps& epr_info_list() { return g_engine.epr_info; }

bool get_source_caps(bool confirmed) {
    if (g_active_cc == 0 || (inline_now() && !confirmed)) return false;
    g_getcap_armed = true;
    g_getcap_at = systick::millis();
    g_getcap_force = true;  // an explicit request is sent even when caps are already known
    return true;
}

bool identify_start(bool confirmed) {
    if (g_active_cc == 0 || (inline_now() && !confirmed)) {
        K2LOG("[ID] Identify refused (no CC / inline without confirm)\n");
        return false;
    }
    const pd::SinkState ss = g_sink.status().state;
    if (ss != pd::SinkState::Idle && ss != pd::SinkState::Ready && ss != pd::SinkState::Failed) return false;
    g_ident_session = g_session.id();
    g_ident.start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, systick::millis());
    K2LOG("[ID] Identify started\n");
    return true;
}
bool probe_start() {
    if (g_active_cc == 0 || inline_now() || g_ident.running() || g_prep.active()) return false;
    const pd::SinkState ss = g_sink.status().state;
    if (ss != pd::SinkState::Idle && ss != pd::SinkState::Ready && ss != pd::SinkState::Failed) return false;
    g_probe_logged = false;
    g_probe.start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, systick::millis());
    K2LOG("[PROBE] started\n");
    return true;
}
// Pd mode only (never inline, never the PD-silent charger test). Without a contract the run makes one first through the
// same paths the START and the trigger menu use: no CC yet (plain passive boot, no Rd) -> present Rd as the START does,
// whose first Source_Capabilities gets the automatic Request for PDO 1; a source already on the line -> sink_request
// PDO 1, which re-announces the caps with Auto-CRC on when the last ones went un-ACKed (a PD-silent boot). The run then
// waits for the contract (kRereadAttachMs), settles, and runs the steps.
bool reread_start(std::uint8_t mask) {
    if (inline_now() || g_legacy_only || g_ident.running() || g_probe.running() || g_reread.running() || g_prep.active()) return false;
    const pd::SinkState ss = g_sink.status().state;
    const bool have_contract = ss == pd::SinkState::Ready;
    if (!have_contract && ss != pd::SinkState::Idle && ss != pd::SinkState::Failed) return false;
    if (have_contract && g_active_cc == 0) return false;
    if (g_force_cc != 0 && g_active_cc != g_force_cc) {
        if (have_contract) return false;   // a contract on the other line is not ours to move
        // The passive scan took another line (the Ra net of a powered cable, CC1's Rp is above its VCONN limit): forget it,
        // so the attach below presents Rd on both lines and the scan then takes only the forced one.
        if (g_active_cc != 0) detach_reset(0);
        K2LOG("[REREAD] forced line CC%u\n", unsigned(g_force_cc));
    }
    const unsigned held = g_engine.caps.valid ? g_engine.caps.num_pdos : 0;
    g_reread_logged = false;
    g_trigger_took_over = false;   // a new run: the attach below is the re-read's own
    g_reread.start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, mask, std::uint8_t(held), systick::millis(), true);
    pdtrace::mark(pdtrace::kMarkReread, g_reread.status().mask, std::uint16_t(held));
    K2LOG("[REREAD] started mask=0x%02X with %u PDOs held, contract %d\n", unsigned(g_reread.status().mask), held, int(have_contract));
    if (!have_contract) {
        if (g_active_cc == 0) {
            if (!g_rd_start && set_rd_for_run(true, false)) g_reread_owns_rd = g_reread_made_contract = true;   // the START path's Request makes the contract
            K2LOG("[REREAD] attach: no CC yet, Rd %s\n", g_rd_start ? "presented" : "refused");
        } else {
            const pd::Err e = sink_request({1, 0, 0}, false);
            g_reread_made_contract = e == pd::Err::Ok;
            g_trigger_took_over = false;   // that Request is ours, not a user trigger
            K2LOG("[REREAD] attach: Request PDO 1 err=%u\n", unsigned(e));
        }
    }
    return true;
}
void set_force_line(std::uint8_t line) {
    g_force_cc = line <= 2 ? line : 0;
    K2LOG("[PD] CC scan forced line %u\n", unsigned(g_force_cc));
}
bool ra_set(bool on) {
    if (on == g_ra) return true;
    if (!on) {
        g_ra = false;
        g_ra_run = false;
        pins::cc_vconn_gnd.write(g_vconn_applied);   // PC13 low again (unless the VCONN experiment has it)
        if (g_active_cc == 0) wr(kSwitches0, idle_rd()); else apply_rd();   // Rd back to the run-start rule
        K2LOG("[EMKRA] off\n");
        return true;
    }
    const pd::SinkState ss = g_sink.status().state;
    emkra::Ctx c;
    c.inline_sink = inline_now();
    c.contract = ss != pd::SinkState::Idle && ss != pd::SinkState::Failed;
    c.self_hold = pd::self_hold_wanted(g_pin_rd, inline_now(), g_legacy_only);   // the contract is our own 5 V hold
    c.busy =g_ident.running() || g_probe.running() || g_reread.running() || g_prep.active();
    c.pinned_rd = g_pin_rd;
    c.vconn = g_vconn_applied || g_vconn.state() != pd::VconnState::Off;
    const emkra::Refuse why = emkra::check(c);
    if (why != emkra::Refuse::None) {
        K2LOG("[EMKRA] refused: %s\n", emkra::refuse_name(why));
        return false;
    }
    // A fresh attach: the Ra goes on before any Rd. A pinned (self-powered) K2 cannot detach: the charger would cut the VBUS
    // that feeds it, so there the Ra goes on beside the Rd that stays (the source's re-look is the caller's business).
    if (g_active_cc != 0 && g_pin_rd == 0) detach_reset(0);
    g_ra = true;
    g_force_cc = emkra::kLine;
    cc_switches(false, g_cc_mask);   // PC13 high now, ahead of Rd
    if (g_active_cc == 0) wr(kSwitches0, idle_rd());   // a run start that already presents Rd keeps it on CC2 alone
    K2LOG("[EMKRA] on: PC13 high (Ra on CC1), Rd on CC2 only, attach scan forced to CC%u\n", unsigned(g_force_cc));
    return true;
}
bool ra_active() { return g_ra; }
void ra_run_own() { if (g_ra) g_ra_run = true; }
// `debug detach`: every owner of Rd is dropped (the re-read's attach, the run's Rd, the forced line, the Ra mode), the
// partner is forgotten as on a real detach, and Rd stays off for kRdHoldMs, far longer than the source's tPDDebounce, so
// the charger sees the sink leave and re-announces once the scan finds it again. The boot-mode pin (sink mode) is its own
// owner and stays: refused there, and inline (a real sink's Rd must not be touched).
bool cc_detach() {
    if (inline_now() || g_pin_rd != 0) return false;
    g_reread_owns_rd = false;
    g_reread_made_contract = false;
    g_reread.abort();
    g_force_cc = 0;
    g_rd_op = false;             // no Rd write below may put the sink's Rd back for an instant
    g_rd_gate.reset();
    ra_set(false);
    set_rd_for_run(false, false);
    detach_reset(0);
    g_rd_hold.start(systick::millis());   // after detach_reset: it cancels the hold
    K2LOG("[PD] CC detach: Rd off for %u ms\n", unsigned(pd::kRdHoldMs));
    return true;
}
void reread_abort() {
    g_force_cc = 0;
    ra_set(false);
    g_reread.abort();
    if (g_reread_owns_rd || g_reread_made_contract) g_reread_release_at = systick::millis();   // housekeeping ends the attach on its next pass
}
const pd::RereadStatus& reread_status() { return g_reread.status(); }
void set_sink_cap_ext_mode(pd::SinkCapExtMode m) {
    g_sink.set_sink_cap_ext_mode(m);
    K2LOG("[PD] Get_Sink_Cap_Extended answer mode %u\n", unsigned(m));
}
void set_proactive_discover(bool on) { g_sink.set_proactive_discover(on); }
void probe_abort() { g_probe.abort(); }
const pd::ProbeStatus& probe_status() { return g_probe.status(); }
void identify_abort() { g_ident.abort(pd::IdWhy::Stopped); }
const pd::IdStatus& identify_status() { return g_ident.status(); }
bool identify_current() { return g_ident_session != 0 && g_ident_session == g_session.id(); }

void feed(std::uint8_t sop, std::uint16_t header, std::span<const std::uint8_t> payload) {
    handle_frame(sop == 1 ? pd::Sop::SopP : sop == 2 ? pd::Sop::SopPP : pd::Sop::Sop, header, payload, false);
}

const pd::SourceCaps& caps() { return g_engine.caps; }
const pd::Emarker& emarker() { return g_engine.cable; }
const pd::VdmInfo& vdm() { return g_engine.vdm; }
const pd::VdmInfo& sink_vdm() { return g_engine.sink_vdm; }
usblink::Link usb_link(std::int32_t dp_mv, std::int32_t dm_mv) {
    usblink::Classifier c = g_engine.usb;
    c.on_lines(dp_mv, dm_mv);
    return c.link();
}
const pd::ExtInfo& ext() { return g_engine.ext; }
bool cable_thunderbolt() { return g_engine.cable_thunderbolt(); }
std::uint32_t rx_count() { return g_engine.caps.rx_count; }
std::uint8_t device_id() { return g_device_id; }
std::uint8_t active_cc() { return g_active_cc; }
const pd::Contract& contract() { return g_engine.contract; }
const pd::SinkCapsSeen& sink_caps() { return g_engine.sink; }

void set_sop_prime_hook(SopPrimeHook hook) {
    g_sop_prime_hook = hook;
    if (hook != nullptr) g_tx_allowed = true;   // the armed spoof is an active feature: its SOP' replies may go out at once
    if (g_active_cc != 0) write_sw1();   // SOP' enable follows the hook
}

bool vconn_request() {
    if (g_active_cc == 0 || inline_now() || g_legacy_only || g_sink.status().state != pd::SinkState::Ready || !emkra::vconn_switch_allowed(g_ra)) {
        K2LOG("[VCONN] request refused: cc=%u inline=%d sink state %u\n", unsigned(g_active_cc), int(inline_now()), unsigned(g_sink.status().state));
        return false;
    }
    const bool ok = g_vconn.request(systick::millis());
    K2LOG("[VCONN] request %s\n", ok ? "queued" : "refused: a swap is running");
    if (ok) vconn_mark();
    return ok;
}

void vconn_off() { vconn_force_off(pd::VconnWhy::Abort); }

VconnInfo vconn_info() {
    VconnInfo v;
    v.state = std::uint8_t(g_vconn.state());
    v.why = std::uint8_t(g_vconn.why());
    v.applied = g_vconn_applied ? 1 : 0;
    v.swaps_rx = g_vconn.swaps_rx();
    v.active_cc = g_active_cc;
    cc_net_mv(v.cc1_mv, v.cc2_mv);
    return v;
}

bool send_sop_prime_frame(std::span<const std::uint8_t> tokens) {
    if (g_active_cc == 0) return false;
    if (!g_tx_allowed) {   // passive mode: no SOP' frame either (only the armed spoof ever calls this)
        K2LOG("[FUSB302] SOP' TX refused (passive)\n");
        return false;
    }
    // Token stream: SYNC x4, PACKSYM|len, header lo/hi, objects, JAM_CRC, EOP, TXOFF. Parse it so the
    // Black Box sees the spoof reply too.
    std::uint16_t header = 0;
    std::span<const std::uint8_t> objs;
    if (tokens.size() >= 10) {
        header = std::uint16_t(tokens[5] | (tokens[6] << 8));
        objs = tokens.subspan(7, tokens.size() - 10);
    }
    if (!fifo_write_tx(header, objs, tokens)) return false;
    return wr(kControl0, kTxStart);
}

}  // namespace k2::fusb302

// EXTI lines 10..15 share this vector; only line 12 is enabled.
extern "C" void EXTI15_10_IRQHandler() {
#ifndef K2_HOST_PINS
    k2::reg::exti().PR = 1u << 12;
#endif
    k2::fusb302::on_int_n();
}
