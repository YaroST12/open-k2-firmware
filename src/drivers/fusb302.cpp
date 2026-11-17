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
#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/drivers/fusb302.hpp"
#include "fusb302_internal.hpp"

#include <array>
#include <new>
#include <span>

#include "k2/drivers/i2c.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/caps_log_gate.hpp"
#include "k2/pd/emk_ra.hpp"
#include "k2/pd/emk_variant.hpp"
#include "k2/pd/pd_ccscan.hpp"
#include "k2/pd/pd_scanpace.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_interrogate.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/pd/pd_probe.hpp"
#include "k2/pd/pd_reread.hpp"
#include "k2/pd/pd_passive.hpp"
#include "k2/pd/pd_phy.hpp"
#include "k2/pd/pd_rdseek.hpp"
#include "k2/pd/pd_rescan.hpp"
#include "k2/pd/pd_rxstamp.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_survey.hpp"
#include "k2/pd/pd_vconn.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/drivers/pin_mark.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::fusb302 {

using i2c::Bus;
using systick::delay_ms;
constexpr std::uint8_t kAddr = 0x22;
namespace phy = pd::phy;
using namespace pd::phy;

using namespace detail;

namespace detail {
std::uint8_t g_device_id, g_active_cc;
bool g_hr_latched = false;
pd::DetachTracker g_detach;
bool g_pd_traffic = false;
pd::LineFallback g_linefb;
pd::IdleWake g_idle_wake;
std::uint32_t g_ident_session = 0;
volatile bool g_irq = false;
std::int32_t g_vbus_mv = -1;
bool g_new_source = false;
std::uint8_t g_pin_rd = 0;
std::uint8_t g_boot_pin_rd = 0;
bool g_rd_inherited = false;
bool g_lab_pin = false;
bool g_pin_moved = false;
pd::RdHold g_rd_hold;
bool g_rd_op = false;
bool g_start_requested = false;
bool g_rd_start = false;
bool g_run_owner = false;
bool g_wake_attach = false;
bool g_inline = false;
raguard::WaitKind g_wait_kind = raguard::WaitKind::Unknown;
bool g_ident_owns_rd = false;
bool g_ident_wait = false;
bool g_ident_alone = false;
bool g_ident_was_running = false;
bool g_ident_result_valid = false;
bool g_ident_alone_hold = false;
std::uint32_t g_ident_wait_at = 0, g_ident_ready_at = 0;
pd::AutoCrcGate g_crc;
pd::AutoCrcGate g_rd_gate;
bool g_legacy_only = false;
bool g_crc_on = false;
bool g_tx_allowed = false;
bool g_cable_read = false;
pd::TriggerPrep g_prep;
pd::CapsSession g_session;
pd::Engine g_engine;
pd::CableLimits g_physical_cable;
std::uint32_t g_physical_session = 0;
pd::CableLimits g_scanned_cable;
bool g_scanned_valid = false;
emk_variant::Answered g_answered;
emk_attached::ReadInfo g_read_info;
raclass::PullDown g_pull_down{};
VbusFn g_vbus_source = nullptr;
bool g_probe_log_pending;
bool g_reread_log_pending;
bool g_reread_owns_rd = false;
bool g_reread_made_contract = false;
std::uint32_t g_reread_release_at = 0;
std::uint8_t g_force_cc = 0;
SopPrimeHook g_sop_prime_hook = nullptr;
pd::Vconn g_vconn;
bool g_vconn_applied = false;
bool g_vconn_give = false;
bool g_ra = false;
emkra::Refuse g_ra_refused = emkra::Refuse::None;
bool g_ra_run = false;
std::uint8_t g_msg_id;
std::uint32_t g_last_rx_ms;
bool g_getcap_force = false;
bool g_getcap_armed = false;
std::uint32_t g_getcap_at;
std::array<BootNote, kBootNotes> g_boot_notes{};
unsigned g_boot_note_n = 0, g_boot_note_lost = 0;
std::array<BootNote, kBootReadbacks> g_readbacks{};
unsigned g_readback_n = 0;
std::uint8_t g_initial_pin_rd = 0;
bool g_ra_early = false;
bool g_mark_read = false;
std::uint16_t g_mark_raw = 0;
pd::IqPhase g_iq_phase{};
}  // namespace detail

namespace {

constexpr std::uint8_t kIntHardSent = 1u << 3;
constexpr std::uint8_t kComp = 1u << 5;                              // STATUS0: CC above MDAC threshold

// INTERRUPTA clears on read, and clear_irqs() / transmit() read it for other reasons. A Hard Reset from the source that
// they happen to catch is kept here for service() (Baseus 140W, Black Box 2026-11-04: it fell in the read after the caps,
// was lost, and the Request that followed went out on a link the source had already torn down).
std::uint8_t g_cc_mask_x;   // stored XOR 3 so .bss zero means PC14|PC15 closed outside VCONN scans
std::uint32_t g_requests_base = 0;
std::uint32_t g_req_seen = 0;       // contract.requests at the last Request handled
std::uint32_t g_req_last_ms = 0;    // when that Request was decoded (0 = none this attach)
std::uint32_t g_req_period_ms = 0;  // period of the last two Requests (0 = fewer than two)
pd::ScanPace g_scan_pace;           // when the unattached CC scan runs (pd_scanpace.hpp)
bool g_pd_seen_attach = false;       // the same, but only a new attach clears it (a unit hold / start-up does not)
// The Rd seek (k2/pd/pd_rdseek.hpp): the line was picked by a scan that had no Rd on any line, so our later Rd on it may be on the wrong
// one. `g_seek_passive` marks such an attach; a move to the other line keeps it, a fresh attach (scan, window re-check) decides again.
alignas(pd::RdSeek) std::uint8_t g_rdseek_raw[sizeof(pd::RdSeek)];
bool g_rdseek_inited;
bool g_seek_passive = false;
std::uint32_t g_attach_ms = 0;   // millis() at the attach of the current partner
std::uint8_t g_bc_max = 0;       // highest BC_LVL (Rp level) read since attach
pd::RxStamp g_rxstamp;        // the same ISR's microsecond timestamp: the arrival time of the frame service() reads next
std::uint32_t g_tx_start_us = 0;   // micros() right after the last transmit() wrote TX_START (the Request latency's start)
std::int32_t g_cc_sink_mv = -1;  // the CC voltage closest to the sink-Rd band (-1 unknown)
std::int32_t g_cc1_mv = -1, g_cc2_mv = -1;  // latest ADC net levels from sink_cc(); -1 = unknown
std::uint8_t g_sw0 = 0;          // the last SWITCHES0 written: whether the K2 itself pulls a CC line down (PDWN1/PDWN2)
std::array<pd::CcSample, 6> g_scan_samples{};   // the last attach scan's comparator samples, for the Rd-window re-check
unsigned g_scan_sample_n = 0;
std::uint32_t g_cc_recheck_at = 0;   // rate limit of the Rd-window re-check (kCcRecheckMinMs)
constexpr std::uint32_t kCcRecheckMinMs = 500;
std::uint32_t g_ident_gone_since = 0;
bool g_ident_gone_timing = false;
pd::Identity g_ident_identity;      // the alone-Identify's collected identity, kept for the release window
inline constexpr std::uint32_t kIdentWaitTimeoutMs = 4000;  // alone-Identify: no Ready contract within this -> refuse
inline constexpr std::uint32_t kIdentSettleMs = 200;        // alone-Identify: Ready settled this long before the first VDM
inline constexpr std::uint32_t kIdentGoneMs = 2000;         // the result survives the release until the toggle stops this long
pd::ContractLinger g_contract_linger;   // a contract nobody holds times out after kSinkIdleLingerMs
#if K2_HOST_REPORT
LoopHost* g_lh = nullptr;   // the Host report owns the line (loop_host_begin): sole sink despite the HID port's Rd
bool g_lh_dfp = false;      // ... and the K2 is DFP after an accepted DR_Swap
bool g_lh_idle = false;     // active_tick(): nothing sent, the sink quiet, the line idle
#else
constexpr LoopHost* g_lh = nullptr;
constexpr bool g_lh_dfp = false;
#endif
bool g_hr_tool_armed = false;  // the explicit sink_hard_reset() tool: one Hard Reset may go out even from idle
pd::Choice g_prep_choice;   // the trigger waiting for the prepare to finish
pd::CapsLogGate g_caps_log;        // keeps a repeating source's caps from flooding the K2LOG ring
std::uint32_t g_caps_count = 0;  // Source_Capabilities messages received since boot
bool g_caps_acked = false;       // the last of them arrived with Auto-CRC on (the source got our GoodCRC)
alignas(pd::Sink) std::uint8_t g_sink_raw[sizeof(pd::Sink)];    // transmit-side state machines are pure too; this file only sends their messages
bool g_sink_inited;
alignas(pd::Identify) std::uint8_t g_ident_raw[sizeof(pd::Identify)];
bool g_ident_inited;
alignas(pd::Probe) std::uint8_t g_probe_raw[sizeof(pd::Probe)];   // report-card source probe: never retries, ends within kProbeTotalMs
bool g_probe_inited;
alignas(pd::Reread) std::uint8_t g_reread_raw[sizeof(pd::Reread)];   // source-cap re-read (after-contract conversation); never retries, ends within kRereadTotalMs
bool g_reread_inited;
alignas(pd::Interrogate) std::uint8_t g_iq_raw[sizeof(pd::Interrogate)];  // inline interrogation: the only inline transmitter, ends within pd::kIqTotalMs plus its resets
bool g_iq_inited;
pd::IqStep g_iq_step{};

constexpr std::uint32_t kRereadLingerMs = 30000;
std::uint32_t g_vconn_on_ms = 0;  // when it went on; the settled reading is logged ~5 ms later
bool g_vconn_log_pending;
bool g_rx_nonempty;
constexpr std::uint32_t kIdleQuietMs = 50;

}  // namespace

namespace detail {

std::uint8_t cc_mask() { return g_cc_mask_x ^ 3u; }
void set_cc_mask(std::uint8_t m) { g_cc_mask_x = m ^ 3u; }
void set_boot_pin(std::uint8_t pin) {
    g_boot_pin_rd = pin;
    pinmark::write(bootmode::pin_mark(pin));
}
K2_HOT pd::RdSeek& rdseek() {
    if (!g_rdseek_inited) {
        new (g_rdseek_raw) pd::RdSeek();
        g_rdseek_inited = true;
    }
    return *std::launder(reinterpret_cast<pd::RdSeek*>(g_rdseek_raw));
}
K2_HOT pd::Sink& sink() {
    if (!g_sink_inited) {
        new (g_sink_raw) pd::Sink();
        g_sink_inited = true;
    }
    return *std::launder(reinterpret_cast<pd::Sink*>(g_sink_raw));
}
pd::Identify& ident() {
    if (!g_ident_inited) {
        new (g_ident_raw) pd::Identify();
        g_ident_inited = true;
    }
    return *std::launder(reinterpret_cast<pd::Identify*>(g_ident_raw));
}
pd::Probe& probe() {
    if (!g_probe_inited) {
        new (g_probe_raw) pd::Probe();
        g_probe_inited = true;
    }
    return *std::launder(reinterpret_cast<pd::Probe*>(g_probe_raw));
}
pd::Reread& reread() {
    if (!g_reread_inited) {
        new (g_reread_raw) pd::Reread();
        g_reread_inited = true;
    }
    return *std::launder(reinterpret_cast<pd::Reread*>(g_reread_raw));
}
pd::Interrogate& iq() {
    if (!g_iq_inited) {
        new (g_iq_raw) pd::Interrogate();
        g_iq_step = pd::IqStep::SinkCap;
        g_iq_inited = true;
    }
    return *std::launder(reinterpret_cast<pd::Interrogate*>(g_iq_raw));
}
bool ident_running() { return g_ident_inited && ident().running(); }
bool probe_running() { return g_probe_inited && probe().running(); }
bool reread_running() { return g_reread_inited && reread().running(); }
K2_HOT bool iq_running() { return g_iq_inited && iq().running(); }

K2_HOT void boot_note(std::uint8_t kind, std::uint8_t val) {
    const std::uint32_t t = systick::millis();
    if (t >= kBootNoteSpanMs) return;
    if (g_boot_note_n > 0 && g_boot_notes[g_boot_note_n - 1].kind == kind && g_boot_notes[g_boot_note_n - 1].val == val) return;
    if (g_boot_note_n < kBootNotes) g_boot_notes[g_boot_note_n++] = {std::uint16_t(t), kind, val};
    else g_boot_note_lost++;
}

K2_HOT bool wr(Reg r, std::uint8_t v) {
    const bool ok = i2c::write_reg(Bus::Fusb, kAddr, r, v);
    pdtrace::reg_write(r, v, ok);
    if (r == kSwitches0) {
        if (ok && ((v ^ g_sw0) & (kPdwn1 | kPdwn2)) != 0) rdseek().rd_changed(systick::millis());
        if (ok) g_sw0 = v;
        boot_note(0, v);
    }
    return ok;
}
K2_HOT bool rd(Reg r, std::uint8_t& v) { return i2c::read_reg(Bus::Fusb, kAddr, r, v); }
K2_HOT bool fifo_read(std::span<std::uint8_t> out) { return i2c::read(Bus::Fusb, kAddr, kFifos, out); }
K2_HOT std::uint8_t tx_sop_token(std::span<const std::uint8_t> stream) {
    if (stream.size() >= 4 && stream[2] == 0x1B && stream[3] == 0x1B) return 0xC0;
    return 0xE0;
}
K2_HOT bool fifo_write_tx(std::uint16_t header, std::span<const std::uint8_t> objs, std::span<const std::uint8_t> stream) {
    pdtrace::tx(header, objs, tx_sop_token(stream));
    return i2c::write(Bus::Fusb, kAddr, kFifos, stream);
}
K2_HOT void flush_fifos() { wr(kControl0, kTxFlush); flush_rx(); }
K2_HOT void clear_irqs() {
    std::uint8_t i = 0, a = 0, b = 0;
    rd(kInterrupt, i); rd(kInterruptA, a); rd(kInterruptB, b);
    if (a & kIntHardRst) g_hr_latched = true;
    pdtrace::irq(a, b, i);
}
K2_HOT void cc_switches(bool vconn, std::uint8_t mask) {
    pins::cc1_switch.write(mask & 1);
    pins::cc2_switch.write(mask & 2);
    const bool pc13 = emkra::pc13_high(vconn, g_ra) || g_ra_early;
    pins::cc_vconn_gnd.write(pc13);
    boot_note(1, std::uint8_t((mask & 3) | (pc13 ? 4 : 0)));
}
K2_HOT pd::Sop sop_from_token(std::uint8_t token) {
    switch (token & 0xE0) {
        case 0xC0: return pd::Sop::SopP;
        case 0xA0: return pd::Sop::SopPP;
        default: return pd::Sop::Sop;
    }
}

}  // namespace detail

namespace {

// ---- transmit (active PD) ----

// Send `m` as an SOP message: token stream SOP1 SOP1 SOP1 SOP2 (0x12 0x12 0x12 0x13; SOP1 SOP1 SOP3 SOP3 is SOP', the cable), PACKSYM|len, header, objects,
// JAM_CRC, EOP, TXOFF, then TX_START. True when INTERRUPTA reports TXSENT (the chip checked the
// partner's GoodCRC itself, with the CONTROL3 auto retries); false on RETRYFAIL or no answer.
// Set by every transmit(): the last TX ended without TXSENT or RETRYFAIL and no GoodCRC of ours turned up in the RX FIFO
// either. That is ambiguous, the message may have been delivered, so a caller that waits for an answer anyway (the
// re-read) treats it as sent.
bool g_tx_timed_out = false;

// A frame transmit() read out of the RX FIFO while looking for its GoodCRC that was not that GoodCRC. service() delivers
// it before it reads the FIFO again, so the order holds; flush_rx() drops it with the FIFO.
struct HeldRx {
    bool valid = false;
    std::array<std::uint8_t, 3> pre{};    // token, PD header
    std::array<std::uint8_t, 36> body{};  // data objects + 4 CRC bytes
};
HeldRx g_rx_held;

// transmit()'s second acknowledgement. Some messages come back GoodCRC'd, the GoodCRC sits in the RX FIFO with the sent
// MessageID, and the FUSB302 raises neither TXSENT nor RETRYFAIL: Get_Source_Cap_Extended on every charger traced up to
// 2026-11-12 (Anker, NOVOO, UGREEN, CUKTECH, OnePlus, ASUS, TOPADRE), which the Diag row then called "K2 send failed".
// Reads the FIFO's first frame: true when it is the SOP GoodCRC of `sent_id`; anything else is held for service() and
// `peek` goes false, since nothing behind a held frame may be read before it.
K2_HOT bool rx_ack_peek(std::uint8_t sent_id, bool& peek) {
    std::uint8_t status1;
    if (!rd(kStatus1, status1) || (status1 & kRxEmpty)) return false;
    peek = false;
    HeldRx f;
    if (!fifo_read(f.pre)) { flush_rx(); return false; }   // a half-read frame leaves the FIFO misaligned
    const unsigned tok = f.pre[0] & 0xE0;
    if (tok != 0xE0 && tok != 0xC0 && tok != 0xA0) {
        pdtrace::rx_bad(f.pre[0], f.pre[1], f.pre[2]);
        flush_rx();
        return false;
    }
    const std::uint16_t header = std::uint16_t(f.pre[1] | (f.pre[2] << 8));
    const unsigned num_do = (header >> 12) & 7;
    if (!fifo_read({f.body.data(), num_do * 4 + 4})) { flush_rx(); return false; }
    if (tok == 0xE0 && num_do == 0 && !(header & 0x8000) && (header & 0x1F) == pd::kCtlGoodCrc &&
        ((header >> 9) & 7) == sent_id) {
        pdtrace::rx(f.pre[0], header, {f.body.data(), 4});
        return true;
    }
    f.valid = true;
    g_rx_held = f;
    return false;
}

// A SOP frame as FIFO tokens: SOP1 SOP1 SOP1 SOP2, PACKSYM | length, the header, the data, JAM_CRC, EOP, TXOFF.
using TxFrame = std::array<std::uint8_t, 4 + 1 + 2 + 28 + 3>;
K2_HOT unsigned tx_frame(std::uint16_t header, const pd::TxMsg& m, TxFrame& buf) {
    unsigned n = 0;
    for (std::uint8_t b : {0x12, 0x12, 0x12, 0x13}) buf[n++] = b;
    buf[n++] = std::uint8_t(0x80 | (2 + m.len));
    buf[n++] = std::uint8_t(header);
    buf[n++] = std::uint8_t(header >> 8);
    for (unsigned i = 0; i < m.len && i < m.data.size(); i++) buf[n++] = m.data[i];
    buf[n++] = 0xFF;  // JAM_CRC
    buf[n++] = 0x14;  // EOP
    buf[n++] = 0xFE;  // TXOFF
    return n;
}

// An inline question (pd::Interrogate, TxMsg::inline_tx): the header already carries the MessageID and the roles the device
// expects from its source, so g_msg_id is neither used nor moved. Auto-CRC stays off (the charger GoodCRCs the device's
// answers), and the device's and the charger's PHYs both GoodCRC this frame at once, so the chip sends it exactly once (no
// auto retry: a retry would land on the device's answer). Nothing is peeked: the GoodCRCs and the answer reach handle_frame()
// through the normal receive path. The result only says that the frame left (TXSENT); the run never relies on it.
K2_HOT bool transmit_inline(const pd::TxMsg& m) {
    TxFrame buf;
    const unsigned n = tx_frame(m.header, m, buf);
    clear_irqs();
    wr(kControl3, 0);   // N_RETRIES 0, AUTO_RETRY off
    fifo_write_tx(m.header, {m.data.data(), m.len < m.data.size() ? m.len : m.data.size()}, {buf.data(), n});
    wr(kControl0, kTxStart);
    g_tx_start_us = systick::micros();
    bool ok = false;
    pdtrace::TxResult res = pdtrace::TxResult::Timeout;
    std::uint8_t last_ia = 0;
    // A 30-byte frame takes ~1.2 ms on the wire; the answer comes tens of ms later. The wait is timed on the microsecond
    // clock: delay_ms(1) lasts ~2.7 ms (delay_us is a deliberately long nop loop), so a loop of four of them blocked the
    // main loop for ~11.8 ms while the log said 4 ms (laptop recording 2026-11-13). Polls are 250 nominal us apart.
    constexpr std::uint32_t kWaitUs = 4000;
    while (std::uint32_t(systick::micros() - g_tx_start_us) < kWaitUs) {
        systick::delay_us(250);
        std::uint8_t ia = 0;
        if (!rd(kInterruptA, ia)) continue;
        last_ia |= ia;
        if (ia & kIntHardRst) { g_hr_latched = true; break; }
        if (ia & kIntTxSent) { ok = true; res = pdtrace::TxResult::TxSent; break; }
        if (ia & kIntRetryFail) { res = pdtrace::TxResult::RetryFail; break; }
    }
    wr(kControl3, kRetries3Auto);
    g_tx_timed_out = res == pdtrace::TxResult::Timeout;
    pdtrace::tx_result(res, (std::uint32_t(systick::micros() - g_tx_start_us) + 500u) / 1000u, last_ia);
    K2LOG("[IQ] t=%u TX type %u id %u rev %u: %s (%u us)\n", unsigned(systick::millis()), unsigned(m.header & 0x1F),
          unsigned((m.header >> 9) & 7), unsigned((m.header >> 6) & 3), ok ? "sent" : "no TXSENT",
          unsigned(systick::micros() - g_tx_start_us));
    return ok;
}

K2_HOT bool transmit(const pd::TxMsg& m) {
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
        if (!g_inline) g_detach.hard_reset(systick::millis());   // our own link resets: its VBUS gap is no detach (pd_passive.hpp)
        K2LOG("[PD] t=%u Hard reset sent\n", unsigned(systick::millis()));
        return true;
    }
    if (m.inline_tx) return transmit_inline(m);
    if (m.restart_id) g_msg_id = 0;
    std::uint16_t header = pd::with_msg_id(m.header, g_msg_id);
    if (g_lh_dfp && m.sop == pd::Sop::Sop) header = std::uint16_t(header | 0x20);   // Host report: the K2 is DFP
    TxFrame buf;
    const unsigned n = tx_frame(header, m, buf);
    clear_irqs();
    if ((header & 0x1F) == pd::kCtlSoftReset && m.len == 0) pdtrace::reset(pdtrace::ResetKind::SoftTx);
    fifo_write_tx(header, {m.data.data(), m.len < m.data.size() ? m.len : m.data.size()}, {buf.data(), n});
    wr(kControl0, kTxStart);
    g_tx_start_us = systick::micros();
    bool ok = false;
    pdtrace::TxResult res = pdtrace::TxResult::Timeout;
    std::uint8_t last_ia = 0;
    int waited = 0;
    const std::uint8_t sent_id = std::uint8_t((header >> 9) & 7);
    bool peek = !g_rx_held.valid;   // a frame already held stays first: nothing is read past it
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
        // No TXSENT on the first read: look for the GoodCRC in the RX FIFO. Passes 2..8 (~25 ms) cover the chip's retries;
        // past them a GoodCRC no longer comes, and each look costs a STATUS1 read.
        if (peek && t >= 1 && t < 8 && rx_ack_peek(sent_id, peek)) { ok = true; res = pdtrace::TxResult::RxGoodCrc; break; }
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

}  // namespace

namespace detail {

std::uint8_t ctl1() { return phy::control1(g_crc_on, g_sop_prime_hook != nullptr, g_cable_read); }

// SWITCHES1 for the active line, Auto-CRC on or off (`g_crc_on`).
void write_sw1() {
    wr(kSwitches1, std::uint8_t(phy::sw1(g_active_cc, g_crc_on) | (g_lh_dfp ? phy::kSw1DataRole : 0)));
    wr(kControl1, ctl1());   // SOP' receive follows Auto-CRC (except while the spoof is armed)
}
K2_HOT void flush_rx() { wr(kControl1, ctl1()); g_rx_held.valid = false; }

void configure_passive() {
    g_crc_on = false;
    g_tx_allowed = false;
    g_hr_tool_armed = false;
    g_rd_op = false;
    g_crc.reset();
    g_rd_gate.reset();
    g_contract_linger.reset();
    for (const RegWrite& w : phy::passive_config(g_active_cc)) if (w.reg != kSwitches0) wr(w.reg, w.value);
    if (g_active_cc == 0) wr(kSwitches0, idle_rd());
    else apply_rd();
    clear_irqs();
}

}  // namespace detail

namespace {

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
    if (want) K2LOG("[PD] Auto-CRC on (active operation)\n");
    else K2LOG("[PD] Auto-CRC off (passive)\n");
}

}  // namespace

namespace detail {

std::uint8_t idle_rd() {
    if (g_lh || downstream_now()) return 0;
    const std::uint8_t run = g_rd_start ? pd::key_rd_lines(emkra::run_rd_lines(g_ra, g_inline, 0), g_pin_rd, 0) : std::uint8_t(0);
    const std::uint8_t own = std::uint8_t(g_pin_rd | run) & (kPdwn1 | kPdwn2);
    if (own != 0) return own;
    const std::uint8_t hold = g_idle_wake.line();
    return hold == 1 ? kPdwn1 : hold == 2 ? kPdwn2 : std::uint8_t(0);
}

bool inline_now() { return g_inline; }

}  // namespace detail

namespace {
bool g_down_prev = false;       // downstream_now() at the last downstream_edge()
void rebase_far_sink() {        // a new attach or a detach: no other sink's Request is on record any more
    g_requests_base = g_engine.contract.requests;
    g_req_seen = g_engine.contract.requests;
    g_req_last_ms = 0;
    g_req_period_ms = 0;
    g_down_prev = g_inline;     // the far-sink part is now false; a still-latched inline is no new edge
}
}  // namespace

namespace detail {
bool downstream_now() { return (g_inline && !g_lh) || far_sink_seen(); }
}  // namespace detail

namespace {
bool sole_sink() { return !downstream_now(); }
}  // namespace

namespace detail {
void apply_rd() {
    if (g_active_cc == 0) { wr(kSwitches0, idle_rd()); return; }
    g_wake_attach = false;
    const bool down = downstream_now();
    std::uint8_t lines = pd::rd_lines(g_pin_rd, g_rd_op, down, g_active_cc);
    if (g_rd_start) lines |= pd::key_rd_lines(emkra::run_rd_lines(g_ra, down, g_active_cc), g_pin_rd, g_active_cc);
    if (g_lh) lines = 0;
    std::uint8_t vconn = 0;
    if (g_vconn_applied) {
        const std::uint8_t other_bit = g_active_cc == 1 ? 2 : 1;
        if (!(lines & other_bit)) vconn = g_active_cc == 1 ? kVconnCc2 : kVconnCc1;
    }
    wr(kSwitches0, std::uint8_t(((lines & 1) ? kPdwn1 : 0) | ((lines & 2) ? kPdwn2 : 0)) | phy::sw0_measure(g_active_cc) | vconn);
}
}  // namespace detail

namespace {

// ---- CC orientation ----

// The attach half of a CC choice, shared by the scan (detect_cc_orientation) and the Rd-window re-check
// (cc_window_recheck): route the chosen line to the chip, measure it, and reset the PD session exactly as
// on a fresh attach (switches, SWITCHES0 measure bit, SWITCHES1 via write_sw1(), chip PD reset, FIFOs).
// `seek_move`: the Rd seek's move to the other line (pd_rdseek.hpp): the Rd the policy gives follows the line like a pinned one
// (one line only, apply_rd), and the seek's budget and "picked passively" mark carry over instead of starting again.
void apply_cc_choice(const pd::CcChoice& c, bool pinned, std::uint8_t rd_both, bool seek_move = false) {
    set_cc_mask(pd::self_power_mask(c.mask, pinned));   // pinned: both closed, the Rd must reach the connector
    cc_switches(false, cc_mask());
    const int i = c.line - 1;
    g_active_cc = std::uint8_t(c.line);
    const bool by_wake = g_idle_wake.line() != 0;   // the scan read this line under the wake's own Rd
    g_idle_wake.on_attach(c.line);   // an attach during an idle Rd hold: the no-Rp detach stays off inside the dwell window
    if (!seek_move) {
        rdseek().reset(g_idle_wake.enabled());   // setting off: the open readings go with the attach, as before the idle wake
        g_linefb.reset();                        // a fresh attach: the direct-plug schedule starts again (a fallback move keeps it)
        g_seek_passive = !pinned && (rd_both & (kPdwn1 | kPdwn2)) == 0;   // no Rd on any line while the scan read: the pick is unverified
    }
    if (pinned || seek_move) apply_rd();   // the pinned Rd follows the active line (pd::rd_lines)
    else wr(kSwitches0, std::uint8_t(rd_both | (i == 0 ? kMeasCc1 : kMeasCc2)));
    g_wake_attach = by_wake && !pinned && !seek_move;   // set after apply_rd() above, which would clear it
    delay_ms(2);
    std::uint8_t status0 = 0;
    rd(kStatus0, status0);
    g_session.attach();
    g_detach.reset();
    g_attach_ms = systick::millis();
    rebase_far_sink();
    g_pd_traffic = false;
    g_pd_seen_attach = false;
    g_engine.reset_sink();   // the sniffed Sink_Capabilities and control messages belong to the previous partner
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

namespace detail {
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
    // An idle wake hold owns one line (our single Rd sits on it for the dwell): only that line may attach. The other line is
    // measured with no Rd on it, where the board's CC2 bias net idles at ~1.7 V - the level of a 3 A Rp over a sink's Rd - and
    // reads comparator-true with nothing behind it; taking it clears the hold and leaves the sleeping source unasked
    // (bench 2026-11-10, runs/2026-11-10-merge2-hw/wake2). The Rp of the awake held line shows over our Rd and attaches here.
    const std::uint8_t held = g_idle_wake.line();
    pd::CcChoice c = pd::choose_cc_forced(samples, g_force_cc != 0 ? g_force_cc : pin_line != 0 ? pin_line : held);
    // Cross-check the comparator choice against the ADC net levels (Ui feeds sink_cc() every tick):
    // the live wire is the one net inside the Rd window, an open Rp (~3.3 V) must never win. A pinned
    // boot (g_pin_rd / pinned_live_line) and a forced debug line (g_force_cc) are explicit choices and
    // are not overridden. The rescan right after a detach (pd::ScanPace) (poll() -> detach_reset() -> this function)
    // lands here again with the fresh levels, so a VBUS collapse mid-negotiation cannot leave a wrong
    // line latched from a bad first sample.
    if (!pinned && g_force_cc == 0 && held == 0) {   // a hold's line is explicit too: the ADC window must not move it
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
    set_cc_mask(3);
    cc_switches(false, cc_mask());
    wr(kSwitches0, rd_both);
}

}  // namespace detail

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

// Cold boot: the attach scan runs about 1 s after reset, before the UI has fed sink_cc() any ADC levels,
// so its Rd-window cross-check saw -1/-1 and could not override a wrongly latched line (an open Rp net
// won the comparator tie). Re-run the cross-check whenever fresh levels arrive while a line is attached
// and no PD message has been decoded on it yet, rate-limited. Passive: comparator reads only, unless the
// levels name another line. A forced debug line (g_force_cc) and a pinned self-power boot are explicit
// choices and are never overridden; PD traffic already decoded on the attached line ends the re-check.
void cc_window_recheck() {
    if (g_active_cc == 0 || g_pd_traffic || g_force_cc != 0 || g_pin_rd != 0) return;
    if (rdseek().moves() != 0) return;   // the Rd seek found the line by the Rp itself: the ADC levels do not overrule that
    if (g_linefb.moves() != 0) return;   // a direct plug: both lines read high by design, the fallback chose the line
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
        cc_switches(false, cc_mask());
        for (int line = 1; line <= 2; line++) {
            wr(kSwitches0, std::uint8_t(idle_rd() | (line == 1 ? kMeasCc1 : kMeasCc2)));
            delay_ms(2);
            std::uint8_t status0 = 0;
            rd(kStatus0, status0);
            mini[line - 1] = {cc_mask(), std::uint8_t(line), (status0 & kComp) != 0, std::uint8_t(status0 & 3)};
        }
        alt = pd::choose_cc_forced(mini, rd_line);
        if (!alt.found) { wr(kSwitches0, sw0); return; }   // the line is not there: leave the chip as it was
    }
    K2LOG("[FUSB302] CC override: ADC cc1=%d cc2=%d mV names CC%u, the comparator chose CC%u\n",
              int(g_cc1_mv), int(g_cc2_mv), unsigned(rd_line), unsigned(g_active_cc));
    apply_cc_choice(alt, false, idle_rd());
}

// The Rd seek (k2/pd/pd_rdseek.hpp): our Rd sits on a line a Rd-less scan picked and no Rp answers it. The source may show its Rp only
// on the other line (an Anker splitter behind a 140 W charger, hardware 2026-11-09). The single Rd moves there, the way a real sink's
// CC logic toggles; never both lines (the Debug Accessory signature), never inline, and never where Rd is not ours to move.
bool rd_seek_wanted() {
    return g_seek_passive && (g_rd_op || g_rd_start) && !inline_now() && !g_ra && g_pin_rd == 0 && g_force_cc == 0 &&
           !g_pd_traffic && g_vbus_mv < 3000;   // PD traffic or a live VBUS: the line is the source's, whatever the comparator says
}
void rd_seek_move() {
    const std::uint8_t to = g_active_cc == 1 ? 2 : 1;
    // the dwell that just ran: the slow one on a standby-signature line (pattern slow-rp-wake), the ordinary one otherwise
    const std::uint32_t dwell = rdseek().standby(g_active_cc) ? pd::kSlowWakeDwellMs : pd::kRdSeekDwellMs;
    K2LOG("[FUSB302] Rd seek: no Rp on CC%u over our Rd for %u ms, Rd moves to CC%u (move %u of %u)\n", unsigned(g_active_cc),
          unsigned(dwell), unsigned(to), unsigned(rdseek().moves()), unsigned(pd::kRdSeekMoves));
    apply_cc_choice({true, cc_mask(), to}, false, 0, true);
}

// The direct-plug line fallback (pd::LineFallback, pattern direct-plug-one-line-wake): our Rd sits on a line that shows the Rp,
// the other line (no Rd of ours on it) reads the open Rp level too, and VBUS stays dark: the charger may wake only for the other
// line. The same Rd owners as the seek (a charger-test run, pd-reread, pd attach: g_rd_op / g_rd_start), and the same lines that
// are nobody's to move: a key mode's or lab pin, the Ra mode, a forced line (pd-reread --line), a sink downstream, PD already seen.
// The move is the seek's: one SWITCHES0 write, so the Rd leaves one line and lands on the other in the same register write.
bool line_fallback_eligible() {
    return (g_rd_op || g_rd_start) && !downstream_now() && !g_ra && g_pin_rd == 0 && g_force_cc == 0 && !g_pd_traffic;
}
bool line_fallback_other_rp() {
    if (g_vconn_applied) return false;   // the chip's own supply sits on that line: its level says nothing about a source
    const std::int32_t mv = g_active_cc == 1 ? g_cc2_mv : g_cc1_mv;
    return mv >= pd::kStandbyOpenMv;
}
void line_fallback_move() {
    const std::uint8_t from = g_active_cc, to = from == 1 ? 2 : 1;
    K2LOG("[PD] Rd CC%u -> CC%u: no VBUS after %u ms on a direct plug (try %u)\n", unsigned(from), unsigned(to),
          unsigned(g_linefb.ran_ms()), unsigned(g_linefb.moves()));
    apply_cc_choice({true, cc_mask(), to}, false, 0, true);
}

// ---- Source_Capabilities logging ----

K2_HOT void log_pdo(int n, const pd::Pdo& p) {
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
K2_HOT void log_epr_info(pd::EprInfo s) {
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

K2_HOT void log_ext(unsigned type) {
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

}  // namespace

namespace detail {

// One received frame (SOP token stripped): the engine decodes it, we log what it produced.
K2_HOT void log_cable(const pd::Emarker& e) {
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
K2_HOT void trace_sink() {
    static std::uint8_t last[5] = {0xFF, 0, 0, 0, 0};
    const pd::SinkStatus& s = sink().status();
    const std::uint8_t cur[5] = {std::uint8_t(s.state), std::uint8_t(s.result), std::uint8_t(s.held),
                                 std::uint8_t(s.epr_mode), s.pos};
    for (int i = 0; i < 5; i++) {
        if (cur[i] == last[i]) continue;
        for (int j = 0; j < 5; j++) last[j] = cur[j];
        pdtrace::sink(cur[0], cur[1], s.held, s.epr_mode, s.pos);
        return;
    }
}

}  // namespace detail

namespace {

K2_HOT void handle_frame(pd::Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, bool wire = true) {
    const bool ext = (header >> 15) & 1;
    const unsigned msg_type = header & 0x1F, num_do = (header >> 12) & 7;
    g_pd_traffic = true;
    g_pd_seen_attach = true;
    // Every wire frame, also with no run going: the watch keeps the sniffed contract and the source's MessageID, so a run on a
    // fresh contract can skip its first reset.
    if (wire) iq().on_message(sop, header, payload, systick::millis());
    // First capabilities after a re-attach: drop the previous charger's data so two sources never mix.
    if (g_new_source && sop == pd::Sop::Sop &&
        ((!ext && msg_type == 0x01 && num_do > 0) || (ext && msg_type == pd::kExtEprSourceCaps))) {
        g_engine.reset_source();
        g_new_source = false;
    }
    if (sop == pd::Sop::Sop && !ext && msg_type == 0x01 && num_do > 0)
        pdtrace::mark(pdtrace::kMarkCaps, std::uint16_t(inline_now() ? 2 : 0), std::uint16_t(sink().status().state));
    if (sop == pd::Sop::Sop && !ext && msg_type == 0x01 && num_do > 0) {
        g_caps_count++;
        g_caps_acked = g_crc_on;
        g_session.caps_rx();
        // Source_Capabilities from another attach: the Identify result was that partner's, not this one's.
        if (g_ident_session != 0 && g_session.id() != g_ident_session) {
            g_ident_result_valid = false;
            g_ident_alone_hold = false;
        }
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
    const pd::EprInfo info_before = sink().status().epr_info;
    g_engine.epr_info_only = info_before == pd::EprInfo::Pending;   // the answer to our SPR-mode query is not the live list
    g_engine.on_message(sop, header, payload, g_active_cc);
    {   // the Request period feeds the PPS keep-alive label on the home footer (no caps on record)
        const std::uint32_t req = g_engine.contract.requests;
        if (req != g_req_seen) {
            const std::uint32_t now = systick::millis();
            if (g_req_last_ms) g_req_period_ms = now - g_req_last_ms;
            g_req_last_ms = now;
            g_req_seen = req;
        }
    }
    if (wire && !g_sop_prime_hook && g_engine.cable_acks != cable_acks) {
        g_physical_cable = pd::cable_limits(g_engine.cable);
        g_physical_session = g_session.id();
    }
    {
        const std::uint32_t now = systick::millis();
        // Get_Sink_Cap is answered only by a live Pd-mode sink: never in the PD-silent legacy session, never inline, never
        // while another sink's Request was seen. The identity / VDM and Soft_Reset replies follow the same inline rule
        // (set before this frame is handed to the sink: a far sink's Request precedes its first VDM).
        sink().set_gives_sink_cap(!g_legacy_only && sole_sink() && g_active_cc != 0);
        sink().set_answers_sop(sole_sink());
        sink().set_never_hard_reset(g_pin_rd != 0 || g_lh);   // see sync_hard_reset_policy()
        const unsigned swaps_before = sink().swaps_rx(pd::SwapKind::Dr) + sink().swaps_rx(pd::SwapKind::Pr) + sink().swaps_rx(pd::SwapKind::Fr);
        sink().on_message(sop, header, payload, now);
        if (swaps_before != unsigned(sink().swaps_rx(pd::SwapKind::Dr)) + sink().swaps_rx(pd::SwapKind::Pr) + sink().swaps_rx(pd::SwapKind::Fr)) {
            // Role swap asked by the partner (the sink answered it; see Sink::swaps_rx)
            const pd::SwapKind k = msg_type == pd::kCtlDrSwap ? pd::SwapKind::Dr : msg_type == pd::kCtlPrSwap ? pd::SwapKind::Pr : pd::SwapKind::Fr;
            const std::uint8_t reply = pd::Sink::swap_reply(k, sink().rev());
            K2LOG("[PD] swap rx %s -> %s\n", k == pd::SwapKind::Dr ? "DR_Swap" : k == pd::SwapKind::Pr ? "PR_Swap" : "FR_Swap",
                  reply == pd::kCtlReject ? "Reject" : reply == pd::kCtlNotSupported ? "Not_Supported" : "none");
        }
        if (sink().status().epr_info != info_before) log_epr_info(sink().status().epr_info);
        // VCONN_Swap and its answers are the sole sink's business: inline the message is the device's talk with the charger and
        // the VCONN handler would answer it as a second sink (hardware 2026-11-13).
        if (sop == pd::Sop::Sop && !ext && num_do == 0 && sole_sink()) {
            const pd::VconnState vs = g_vconn.state();
            g_vconn.on_control(std::uint8_t(msg_type), now);
            if (vs != g_vconn.state() || msg_type == pd::kCtlVconnSwap) {
                K2LOG("[VCONN] RX control %u: state %u -> %u (why %u)\n", unsigned(msg_type), unsigned(vs), unsigned(g_vconn.state()), unsigned(g_vconn.why()));
                vconn_mark();
            }
        }
        bool requested_now = false;   // one of the branches below already asked for PDO 1 in this frame
        if (sop == pd::Sop::Sop && !ext && num_do > 0 && msg_type == pd::kDataSourceCap && sink().take_resume() && !downstream_now() && g_active_cc != 0 &&
            !g_legacy_only) {
            // After a source hard reset the contract is gone: ask for PDO 1 (5 V) again on our own.
            const pd::Err e = sink().answer_5v({1, 0, 0}, now);
            requested_now = true;
            K2LOG("[PD] auto re-request after hard reset: err=%u\n", unsigned(e));
        }
        // A run start is a sink while it waits: PDO 1 (5 V) once, so the source gets its GoodCRC and a Request
        // and brings VBUS up (the C135 ramps ~2 V/s and gives up when its caps go unanswered, hardware 2026-11-03).
        if (g_rd_start && sop == pd::Sop::Sop && !ext && num_do > 0 && msg_type == pd::kDataSourceCap) {
            const std::uint8_t why = pd::start_request_blockers(inline_now(), g_active_cc, sink().status().state == pd::SinkState::Idle, g_start_requested, g_legacy_only);
            if (why == 0) {
                g_start_requested = true;
                const pd::Err e = sink().answer_5v({1, 0, 0}, now);
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
            const bool hold = (!g_rd_start && pd::self_hold_wanted(g_pin_rd, downstream_now(), g_legacy_only)) || g_lh;
            const pd::SinkState st = sink().status().state;
            const bool sink_idle = st == pd::SinkState::Idle || (hold && st == pd::SinkState::Failed);
            // After an alone-Identify handed the line back, a late caps message must not re-hold a contract (and the Rd).
            if (!g_ident_alone_hold &&
                pd::acked_caps_need_request(g_caps_acked, downstream_now(), g_active_cc, g_legacy_only, sink_idle, g_prep.active(),
                                            requested_now)) {
                const pd::Err e = sink().answer_5v({1, 0, hold ? pd::kSelfHoldMa : std::uint16_t(0)}, now);
                pdtrace::mark(pdtrace::kMarkAutoReq, std::uint16_t((hold ? 0x300 : 0x200) | unsigned(e)));
                K2LOG("[PD] acked caps: request PDO 1 err=%u\n", unsigned(e));
                if (hold) K2LOG("[PD] self-power hold: 5 V contract asked at %u mA\n", unsigned(pd::kSelfHoldMa));
            }
        }
        ident().on_message(sop, header, payload, now);
        probe().on_message(sop, header, payload, now);
        reread().on_message(sop, header, payload, now);
#if K2_HOST_REPORT
        if (g_lh && wire && sop == pd::Sop::Sop) g_lh->on_message(header, payload, now);
#endif
    }
    // Inline, the spoof answers only inside the run's window and only a frame the chip acknowledged (Auto-CRC on): the first
    // query arrives unacknowledged (the window opens on it), and an answer to a message the charger counts as lost would confuse it.
    if (sop != pd::Sop::Sop && g_sop_prime_hook && (!inline_now() || (g_crc_on && iq().sopp_window(systick::millis()))))
        g_sop_prime_hook(sop == pd::Sop::SopP ? 1 : 2, header, payload);
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

// Hand the line back after an alone-Identify (success, failure, timeout): the Rd drop follows in the same
// pass (g_ident_owns_rd), so the phone sees the detach and stops sourcing VBUS into the K2. The result stays
// current (identify_current) after the release until the port is really gone - see set_wait_kind.
static void alone_handback() {
    g_ident_alone = false;
    g_ident_alone_hold = true;
    // The 5 V contract the acked caps made (a held trigger contract) would keep the Rd gate on for good, and a plain
    // release would still linger kSinkIdleLingerMs: forget it here, no TX - the Rd drop below is the detach the phone sees
    // (Pixel alone, hardware 2026-11-10: "line released" but VBUS stayed 5.05 V with Rd on).
    sink().detach();
    K2LOG("[ID] alone: line released\n");
}

// The armed alone-Identify wait's premise broke (the inline latch closed, the line detached, the verdict is no
// longer DualRolePort): refuse exactly like the timeout and hand the line back. Reuses the existing refusal
// line - no new log format.
static void ident_wait_refuse() {
    if (!g_ident_wait) return;
    g_ident_wait = false;
    K2LOG("[ID] alone: no Ready contract within %u ms, Identify not sent (sink state %d)\n",
          unsigned(systick::millis() - g_ident_wait_at), int(sink().status().state));
    if (g_ident_alone) alone_handback();
}

void set_inline(bool on) {
    if (on == g_inline) return;
    g_inline = on;
    // An armed alone-Identify wait is over: with a real sink behind the K2 the alone premise is gone - refuse
    // and hand the line back now, not at the timeout.
    if (on) ident_wait_refuse();
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
bool sink_downstream() { return downstream_now(); }

// The Identify result stays visible after its release until the port is really gone: the toggle stops
// (NoSource) or a steady-Rp device takes the line (ChargerWaiting) for kIdentGoneMs. Anything else
// (DualRolePort still toggling, Unknown while we hold a line) keeps it. Called from set_wait_kind and from
// service(): time passes even when the feed is not re-sent.
static void ident_result_gate(std::uint32_t now) {
    if (g_wait_kind == raguard::WaitKind::NoSource || g_wait_kind == raguard::WaitKind::ChargerWaiting) {
        if (!g_ident_gone_timing) { g_ident_gone_timing = true; g_ident_gone_since = now; }
        if (now - g_ident_gone_since >= kIdentGoneMs) {
            g_ident_result_valid = false;
            g_ident_alone_hold = false;
            g_ident_gone_timing = false;
        }
    } else g_ident_gone_timing = false;
}

// The link's waiting verdict, fed by the UI (pd::Link::wait_kind over the passive CC levels), stored for the
// alone-Identify decision. Default Unknown: without the feed the driver never claims a device is alone.
void set_wait_kind(raguard::WaitKind kind) {
    g_wait_kind = kind;
    ident_result_gate(systick::millis());
}
raguard::WaitKind wait_kind() { return g_wait_kind; }
bool caps_this_attach() { return g_session.fresh(); }

namespace detail {
bool set_rd_for_run(bool on, bool keep_scan) {
    if (on && g_inline) return false;   // the sink's Rd must stay alone on the line
    if (!on && !keep_scan) self_power_restore();   // the run is over: the boot-mode Rd pin comes back
    if (g_rd_start == on) return true;
    g_rd_start = on;
    if (!on && !keep_scan) g_scanned_valid = false;
    if (on) g_start_requested = false;
    else if (g_start_requested) { g_start_requested = false; sink().release(systick::millis()); }   // back to the 5 V we asked for
    apply_rd();
    usb::log(on ? "[PD] Run start: Rd presented\n" : keep_scan ? "[PD] Run start: Rd paused\n" : "[PD] Run start over: Rd off\n");
    return true;
}
}  // namespace detail

// Charger test unit lifecycle (docs/CHARGER_TEST_FLOW.md). Every unit starts detached and attaches with its own session mode.
// unit_detach(): Rd off (the source sees a detach and drops its output), the 5 V default, the sink and source state forgotten,
// the PD-silent mode off. The scanned cable is kept (the same cable stays plugged in).
void unit_detach() {
    g_reread_owns_rd = false; g_ident_owns_rd = false;
    g_reread_made_contract = false;
    reread().abort();
    g_prep.abort();
    self_power_leave();          // pinned boot-mode Rd would keep the charger attached: drop the pin too
    set_rd_for_run(false, true);
    g_rd_op = false;             // the unit's detach is Rd off, whatever the Rd gate's linger still says
    g_rd_gate.reset();
    g_contract_linger.reset();
    apply_rd();
    sink().detach();
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
    g_reread_owns_rd = false; g_ident_owns_rd = false;
    g_reread_made_contract = false;
    g_legacy_only = legacy;
    self_power_restore();        // back to the boot-mode pin before presenting Rd for the unit
    sink().detach();
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
    g_reread_owns_rd = false; g_ident_owns_rd = false;
    g_reread_made_contract = false;
    reread().abort();
    g_prep.abort();
    g_legacy_only = legacy;      // PD-silent: Auto-CRC drops with the run-start demand, caps are heard and never answered
    sink().detach();             // forget the contract; sends nothing
    if (!legacy) sink().soft_reset(systick::millis());   // the source re-announces its caps; the acked-caps path requests PDO 1
    g_detach.reset();
    g_engine.reset_source();
    g_new_source = true;
    g_pd_traffic = false;
    g_last_rx_ms = systick::millis();
    usb::log(legacy ? "[PD] Unit hold: PD-silent, Rd kept\n" : "[PD] Unit hold: Soft_Reset, Rd kept\n");
}
bool rd_for_run(bool on) {   // a run start takes the Rd and the contract over from a re-read
    g_reread_owns_rd = false; g_ident_owns_rd = false;
    g_reread_made_contract = false;
    if (g_ra) {   // the Ra mode goes with the run: owned from the START, released when the run is over
        g_ra_run = on;
        if (!on) ra_set(false);
    }
    return set_rd_for_run(on, false);
}
void rd_for_run_pause() { g_reread_owns_rd = false; g_ident_owns_rd = false; g_reread_made_contract = false; set_rd_for_run(false, true); }
bool rd_for_run_on() { return g_rd_start; }
std::uint8_t idle_wake_line() { return g_idle_wake.line(); }
std::int32_t open_cc_mv(std::uint8_t line) { return rdseek().open_mv(line); }   // the settled open (no-Rd) reading of a line, -1 = none

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

// ---- active operations, run from poll() ----

bool line_idle(std::uint32_t now) { return !g_rx_nonempty && now - g_last_rx_ms >= kIdleQuietMs; }

// One step of the trigger prepare: cycle the CC lines so the source re-announces to an ACKing chip,
// then fire the stored Request 5 ms after its caps.
// What was asked and what goes on the wire: the choice's 0 means "PDO maximum", the plan holds the
// effective mV / mA and the RDO.
void log_trigger(const pd::Choice& c, pd::Err e) {
    const pd::Plan& p = sink().plan();
    if (e == pd::Err::Ok)
        K2LOG("[PD] t=%u Trigger pos %d (asked %d mV %d mA) -> %d mV %d mA RDO 0x%08X session %u: ok\n",
                    unsigned(systick::millis()), c.pos, c.mv, c.ma, p.mv, p.ma, unsigned(p.rdo),
                    unsigned(g_session.id()));
    else
        K2LOG("[PD] Trigger pos %d (asked %d mV %d mA): %s\n", c.pos, c.mv, c.ma, pd::err_name(e));
}

K2_HOT void prep_tick(std::uint32_t now) {
    switch (g_prep.tick(now, g_caps_count)) {
        case pd::TriggerPrep::Act::None: break;
        case pd::TriggerPrep::Act::OpenCc:
            K2LOG("[PD] t=%u Trigger prepare: caps were not ACKed, cycling CC (Auto-CRC on)\n", unsigned(now));
            pins::cc1_switch.clear();
            pins::cc2_switch.clear();
            break;
        case pd::TriggerPrep::Act::CloseCc:
            cc_switches(false, cc_mask());
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
            const pd::Err e = sink().trigger(g_prep_choice, now);
            log_trigger(g_prep_choice, e);
            break;
        }
    }
}

pd::RereadLink reread_link() {
    return {.contract = sink().status().state == pd::SinkState::Ready,
            .pdos = std::uint8_t(g_engine.caps.valid ? g_engine.caps.num_pdos : 0),
            .rev = std::uint8_t(g_engine.caps.valid ? g_engine.caps.spec_rev : 0),
            .svids = std::span<const std::uint16_t>(g_engine.vdm.svids.data(), g_engine.vdm.num_svids)};
}

// Once per pass: logs a finished re-read (Black Box mark and log line) and ends the attach it made itself: back to PDO 1,
// the contract forgotten (Auto-CRC and the sink demand go with it) and the Rd it presented dropped. At once when nothing
// came of the run, after the linger otherwise (the contract and the caps stay readable: k2_remote pdos). A run on a contract
// that was already there, or one a START / trigger / PD Stress / twin / unit took over (reread_take_over clears the ownership),
// is never touched, and nothing is released while a run owns the line (the linger starts over when it ends).
void reread_housekeeping(std::uint32_t now) {
    if (!reread_running() && g_reread_log_pending) {
        g_reread_log_pending = false;
        const pd::RereadStatus& rs = reread().status();
        const unsigned held = g_engine.caps.valid ? g_engine.caps.num_pdos : 0;
        pdtrace::mark(pdtrace::kMarkReread, std::uint16_t(0x8000 | (rs.skipped_mask << 8) | rs.ok_mask),
                      std::uint16_t((rs.avs_seen ? 0x8000 : 0) | (rs.caps_rx << 8) | rs.pdos_last));
        K2LOG("[REREAD] %s ok=0x%02X skipped=0x%02X notsup=0x%02X caps_rx=%u pdos %u -> %u (last list %u) avs=%d attach_failed=%d\n",
              rs.state == pd::RereadState::Aborted ? "aborted" : "done", unsigned(rs.ok_mask), unsigned(rs.skipped_mask),
              unsigned(rs.notsup_mask), unsigned(rs.caps_rx), unsigned(rs.pdos_before), held, unsigned(rs.pdos_last), int(rs.avs_seen), int(rs.attach_failed));
        const bool nothing = rs.attach_failed || rs.state == pd::RereadState::Aborted;
        g_reread_release_at = nothing ? now : now + kRereadLingerMs;
    }
    if (g_run_owner && (g_reread_owns_rd || g_reread_made_contract)) {
        // A charger test, PD Stress or twin run owns the line: its Soft_Reset / unit gaps are not an idle attach (hardware
        // 2026-11-11: the release fired inside a PD Stress run). Whatever the run does with the Rd is its own; the linger of an
        // attach it never took over starts over when the run ends.
        g_reread_release_at = now + kRereadLingerMs;
        return;
    }
    if ((g_reread_owns_rd || g_reread_made_contract) && !reread_running() && !g_reread_log_pending &&
        std::int32_t(now - g_reread_release_at) >= 0) {
        const pd::SinkStatus& ss = sink().status();
        const bool quiet = ss.state == pd::SinkState::Idle || ss.state == pd::SinkState::Ready || ss.state == pd::SinkState::Failed;
        if (!quiet) return;   // an exchange in flight finishes first
        if (ss.state == pd::SinkState::Ready && ss.pos != 1) {   // back to the 5 V default first
            sink().release(now);
            return;
        }
        K2LOG("[REREAD] attach released: contract %d, Rd %d\n", int(g_reread_made_contract), int(g_reread_owns_rd));
        if (g_reread_owns_rd) set_rd_for_run(false, false);
        g_reread_owns_rd = false; g_ident_owns_rd = false;
        g_reread_made_contract = false;
        g_legacy_only = false;
        sink().detach();
    }
}

// The linger of a Ready contract nobody holds ran out (pd::ContractLinger; the sink demand, Rd and Auto-CRC are already off
// by now). A contract sitting at another voltage, or in EPR mode, first goes back to the 5 V default: that Request is
// engaged again and starts a new linger. Otherwise the contract is forgotten, so the source sees the sink gone: Rd is off,
// and it drops VBUS and re-announces on its own. Never touches a held trigger, a run's own Rd or an operation in flight.
K2_HOT void contract_expired(std::uint32_t now) {
    const pd::SinkStatus& ss = sink().status();
    if (ss.state != pd::SinkState::Ready || ss.held) return;
    if (ss.pos != 1 || ss.epr_mode) {
        K2LOG("[PD] Contract unused for %u s: back to 5 V\n", unsigned(pd::kSinkIdleLingerMs / 1000));
        sink().release(now);
        return;
    }
    K2LOG("[PD] Contract released: Rd off, unused for %u s\n", unsigned(pd::kSinkIdleLingerMs / 1000));
    sink().detach();
}

namespace detail {

// CC1 / CC2 net in mV from the ADC's newest sweep (raw * 3.3 V / 4096; the pins have no divider, so a 5 V VCONN reads at the rail).
bool cc_net_mv(std::uint16_t& cc1, std::uint16_t& cc2) {
    std::uint16_t r1 = 0, r2 = 0;
    if (!adc::latest_cc(r1, r2)) return false;
    cc1 = std::uint16_t(std::uint32_t(r1) * 3300u / 4096u);
    cc2 = std::uint16_t(std::uint32_t(r2) * 3300u / 4096u);
    return true;
}

void vconn_mark() {
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

}  // namespace detail

// Follows g_vconn: switches VCONN on / off. It goes only onto the line opposite the active one (the captive plug's other CC),
// only with a contract-less-safe line state: the other net must read like an Rp (above 2.0 V), not an Rd / Ra, and no sink inline.
K2_HOT void vconn_sync(std::uint32_t now) {
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
        g_vconn_log_pending = true;
        K2LOG("[VCONN] on: active cc%u, VCONN on cc%u (before: %u mV)\n", unsigned(g_active_cc), g_active_cc == 1 ? 2u : 1u, unsigned(other));
        vconn_mark();
    } else if (!want && g_vconn_applied) {
        vconn_force_off(g_vconn.why());   // the engine already gave the role up (partner's PS_RDY, timeout)
    }
    if (g_vconn_applied && g_vconn_log_pending && static_cast<std::int32_t>(now - g_vconn_on_ms) >= 5) {
        g_vconn_log_pending = false;
        std::uint16_t c1 = 0, c2 = 0;
        cc_net_mv(c1, c2);
        K2LOG("[VCONN] settled: cc1 %u mV, cc2 %u mV\n", unsigned(c1), unsigned(c2));
    }
}

// A pinned Rd means the source feeds the K2: a Hard Reset of our own would cut VBUS and kill it, so the sink engine only fails
// (it never transmits one). The explicit sink_hard_reset() tool is not covered.
void sync_hard_reset_policy() { sink().set_never_hard_reset(g_pin_rd != 0 || g_lh); }

// A sink behind the K2 became known (the inline latch, or another sink's Request on the line): whatever the K2 had started as the
// device stops here. The sink engine drops its contract (no Request, no Soft Reset, no re-request) and the pin's Rd comes off.
static void downstream_edge() {
    const bool down = downstream_now();
    if (down && !g_down_prev) {
        if (sink().status().state != pd::SinkState::Idle) sink().detach();
        apply_rd();
        K2LOG("[PD] Sink downstream (inline=%d far request=%d): Rd off, no Request, no VDM answers\n", g_inline ? 1 : 0,
              far_sink_seen() ? 1 : 0);
    }
    g_down_prev = down;
}

// The inline interrogation's progress: a Black Box mark and a log line per phase or step change, a summary at the end.
namespace detail { std::uint8_t g_iq_closed = 0; }   // questions already logged
void iq_report() {
    if (!g_iq_inited) return;
    const pd::IqStatus& s = iq().status();
    if (s.closed != g_iq_closed) {   // what the wire showed after the question: who answered, who only GoodCRC'd
        g_iq_closed = s.closed;
        K2LOG("[IQ] t=%u Q %s id %u try %u: %s (device msgs %u, GoodCRC %u, charger msgs %u)\n", unsigned(systick::millis()),
              pd::iq_step_name(s.q.step), unsigned(s.q.id), unsigned(s.q.attempt), pd::iq_result_name(s.q.result),
              unsigned(s.q.dev_msgs), unsigned(s.q.dev_goodcrc), unsigned(s.q.src_msgs));
    }
    if (s.phase == g_iq_phase && s.step == g_iq_step) return;
    g_iq_phase = s.phase;
    g_iq_step = s.step;
    pdtrace::mark(pdtrace::kMarkInterrogate,
                  std::uint16_t(unsigned(s.phase) | unsigned(s.step) << 4 | unsigned(s.why) << 8 | unsigned(s.match) << 12),
                  std::uint16_t(s.asked_mask | s.ok_mask << 8));
    K2LOG("[IQ] t=%u %s, step %s\n", unsigned(systick::millis()), pd::iq_phase_name(s.phase), pd::iq_step_name(s.step));
    if (!pd::iq_finished(s.phase)) return;
    if (s.why == pd::IqWhy::SourceResets) {
        K2LOG("[IQ] the charger Hard Resets when it is asked: asking this charger is not possible, stopped after the first reset\n");
    }
    if (s.why == pd::IqWhy::SourceRestarts) {
        K2LOG("[IQ] the charger renegotiates when it is asked: asking this charger is not possible, stopped at the first restart\n");
    }
    // Three lines: a binary log entry holds 63 bytes, and the longest why and match words are 27 characters each.
    K2LOG("[IQ] end %s (%s)\n", pd::iq_phase_name(s.phase), pd::iq_why_name(s.why));
    K2LOG("[IQ] contract %s: asked 0x%02X answered 0x%02X refused 0x%02X silent 0x%02X\n", pd::iq_match_name(s.match),
          unsigned(s.asked_mask), unsigned(s.ok_mask), unsigned(s.refused_mask), unsigned(s.silent_mask));
    K2LOG("[IQ] counts: %u sent, %u device GoodCRC, resets %u sent %u seen\n", unsigned(s.sent), unsigned(s.dev_acks),
          unsigned(s.resets_sent), unsigned(s.resets_seen));
    K2LOG("[IQ] contracts: first pos %u RDO 0x%08X PDO 0x%08X, second pos %u RDO 0x%08X PDO 0x%08X\n", unsigned(s.first.pos),
          unsigned(s.first.rdo), unsigned(s.first.src_pdo), unsigned(s.second.pos), unsigned(s.second.rdo), unsigned(s.second.src_pdo));
    K2LOG("[IQ] answers: id %04X:%04X, %u sink PDOs, ext %u bytes, revision 0x%08X, %u SVIDs, %u modes\n", unsigned(s.id.vid),
          unsigned(s.id.pid), unsigned(s.num_sink_pdos), unsigned(s.sink_ext_len), unsigned(s.revision), unsigned(s.num_svids),
          unsigned(s.num_modes));
    K2LOG("[IQ] late 0x%02X, %u EPR sink PDOs%s\n", unsigned(s.late_mask), unsigned(s.num_epr_pdos), s.epr_partial ? " (first chunk only)" : "");
    if (s.spoof)
        K2LOG("[IQ] spoof: SOP' cable queried %u times (charger or device), %u replies, real cable answered %u\n", unsigned(s.sopp_queries),
              unsigned(s.sopp_replies), unsigned(s.sopp_real));
}

K2_HOT void active_tick() {
    const std::uint32_t now = systick::millis();
    downstream_edge();
    sync_hard_reset_policy();
    prep_tick(now);
    const pd::EprInfo info_was = sink().status().epr_info;
    sink().tick(now);
    if (sink().status().epr_info != info_was) log_epr_info(sink().status().epr_info);
    g_vconn.tick(now);
    if (g_vconn_give && g_vconn.state() == pd::VconnState::On && g_vconn.tx_idle()) {   // lab round trip: the role is ours (not driven), hand it back
        g_vconn_give = false;
        g_vconn.give(now);
        K2LOG("[VCONN] round trip: role taken, handing it back\n");
        vconn_mark();
    } else if (g_vconn_give && (g_vconn.state() == pd::VconnState::Failed || g_vconn.state() == pd::VconnState::Off)) {
        g_vconn_give = false;
    }
    vconn_sync(now);
    {   // Fully active for the whole contract: a running or held trigger, Identify, a pending
        // Get_Source_Cap or the prepare. Passive again only after release / detach (plus the linger).
        const pd::SinkStatus& ss0 = sink().status();
        const bool ready = ss0.state == pd::SinkState::Ready;
        // Our Soft_Reset leaves the sink Idle and not held at once; the wait for the caps it asks for is in flight too (PD Stress:
        // without it the queued Soft_Reset was refused in the same tick and Rd went 300 ms later).
        const bool in_flight = (ss0.state != pd::SinkState::Idle && ss0.state != pd::SinkState::Failed && !ready) ||
                               sink().soft_reset_pending();
        pd::CrcDemand d{.sink_active = false,
                        .sink_held = ss0.held,
                        .ident = ident_running() || g_ident_wait || probe_running() || reread_running(),
                        .getcap = g_getcap_armed || g_vconn.state() != pd::VconnState::Off,   // a VCONN swap needs our GoodCRC
                        .prep = g_prep.crc_wanted(),
                        .run_start = g_rd_start && !inline_now() && !g_legacy_only,   // legacy-only: no GoodCRC for the source's caps
                        // the armed spoof needs our GoodCRC on SOP', no Rd; inline only while the run's window is open
                        .spoof = g_sop_prime_hook != nullptr && (!inline_now() || iq().sopp_window(now)),
                        .self_hold = pd::self_hold_wanted(g_pin_rd, downstream_now(), g_legacy_only) || g_lh};   // Host report: the host feeds us   // pinned Rd: the brick feeds us, keep its PD alive
        // A Ready contract nobody holds counts only for kSinkIdleLingerMs after the last thing that used it.
        const bool engaged = in_flight || pd::rd_demand(d);
        const bool counts = g_contract_linger.update(now, engaged);
        d.sink_active = in_flight || (ready && counts);
        // Inline the run's spoof window must close at once when it is over: the 300 ms linger would GoodCRC the charger's
        // and the device's frames and the questions would collide with it.
        if (inline_now() && g_sop_prime_hook != nullptr && !pd::crc_demand(d)) g_crc.reset();
        update_crc(pd::crc_demand(d), pd::rd_demand(d));
        // The TX choke point's input: the same demand that drives Auto-CRC (plus the explicit hard-reset tool).
        // transmit() refuses everything while this is false, so a passive K2 can never put a frame on CC.
        // A running inline interrogation may transmit too, without Auto-CRC (it is no part of the CRC demand).
        g_tx_allowed = pd::crc_demand(d) || g_hr_tool_armed || iq_running();
        if (ready && !counts) contract_expired(now);
    }
    pd::TxMsg m;
    bool sent = false;
    if (sink().take_tx(m)) {
        if (m.hard_reset) g_hr_tool_armed = false;   // the tool's one Hard Reset went out (or was refused)
        const bool tx_ok = transmit(m);
        sink().set_stamp_us(g_tx_start_us);
        sink().on_tx_result(tx_ok, now);
        sent = true;
    } else if (g_vconn.take_tx(m, sink().rev())) {
        // A frame queued before the K2 knew it sat inline is dropped, never sent: inline only the run transmits.
        const bool ok = sole_sink() && transmit(m);
        K2LOG("[VCONN] TX type %u: %s\n", unsigned(m.header & 0x1F), !sole_sink() ? "dropped inline" : ok ? "GoodCRC" : "no GoodCRC");
        vconn_mark();
        sent = sole_sink();
    }
    if (iq_running()) {   // the run keeps its own quiet gap and deadlines: ticked before the idle gate
        if (!inline_now()) iq().abort(pd::IqWhy::NotInline, now);
        iq().tick(now);
        if (!sent && iq().take_tx(m)) {
            const bool hr = m.hard_reset;
            const bool ok = transmit(m);
            if (!hr) iq().on_tx_result(ok, now);
            sent = true;
        }
    }
    trace_sink();
    const pd::SinkState ss = sink().status().state;
    const bool sink_quiet = ss == pd::SinkState::Idle || ss == pd::SinkState::Ready || ss == pd::SinkState::Failed;
#if K2_HOST_REPORT
    g_lh_idle = g_lh && !sent && sink_quiet && line_idle(now);
#endif
    if (ident_running() && (!sink_quiet || !line_idle(now))) {   // [ID] probe: why Identify is not transmitting
        static std::uint32_t last_log;
        if (now - last_log >= 1000) {
            last_log = now;
            K2LOG("[ID] blocked: sink_quiet=%d rx_empty=%d quiet_ms=%u inline=%d step=%d\n", int(sink_quiet),
                        int(!g_rx_nonempty), unsigned(now - g_last_rx_ms), int(inline_now()), int(ident().status().step));
        }
    }
    if (probe_running()) {   // ticked before the idle gate so its deadline holds on a busy line
        const bool idle = !sent && sink_quiet && line_idle(now);
        probe().tick(now, idle);
        const bool tx = idle && probe().take_tx(m);
        if (tx) probe().on_tx_result(transmit(m), now);
        if (!probe_running() && g_probe_log_pending) {
            g_probe_log_pending = false;
            const pd::ProbeStatus& ps = probe().status();
            K2LOG("[PROBE] done ok=%u skipped=%u\n", unsigned(ps.ok_mask), unsigned(ps.skipped_mask));
        }
        if (tx) return;
    }
    if (reread_running() || g_reread_log_pending) {   // same shape as the probe: ticked before the idle gate
        const bool idle = !sent && sink_quiet && line_idle(now);
        reread().tick(now, idle, reread_link());
        const bool tx = idle && reread().take_tx(m);
        if (tx) reread().on_tx_result(transmit(m) || g_tx_timed_out, now);   // a timeout may still have been delivered: wait for the answer
        if (tx) return;
    }
    if (sent || !sink_quiet || !line_idle(now)) return;

    // Not while the Xiaomi auth chain runs: a Get_Source_Cap of ours in the middle of it kills the chain; it stays armed.
    if (g_getcap_armed && std::int32_t(now - g_getcap_at) >= 0 && !sink().xiaomi_chain_active(now)) {
        g_getcap_armed = false;
        if (g_getcap_force || !g_engine.caps.valid) {  // forced, or nothing arrived by itself
            g_getcap_force = false;
            pd::TxMsg gc;
            gc.header = pd::make_header(pd::kCtlGetSourceCap, 0, sink().rev());
            K2LOG("[PD] No Source_Capabilities yet: sending Get_Source_Cap\n");
            transmit(gc);
            return;
        }
    }
    if (ident_running()) {
        // Inline, the SVIDs worth a Discover Modes are the sink's when it listed any; else the charger's.
        const pd::VdmInfo& iv = inline_now() && g_engine.sink_vdm.num_svids ? g_engine.sink_vdm : g_engine.vdm;
        std::array<std::uint16_t, pd::kMaxSvids> sv = iv.svids;
        ident().tick(now, true, {sv.data(), iv.num_svids});
        if (ident().take_tx(m)) ident().on_tx_result(transmit(m), now);
    }
}

K2_HOT void on_int_n() {
    g_rxstamp.edge(systick::micros());   // ISR-safe, one timestamp: all the ISR adds (the budget is checked in the gate's stack report)
    g_irq = true;
}

void new_source() { g_new_source = true; }

void clear_source() {
    g_engine.reset_source();
    g_new_source = true;
}

namespace detail {

// Partner gone: RESET=PD_RESET, then SWITCHES0 (0, or the pinned boot's Rd), then the whole passive profile
// again (configure_passive() rewrites it in full) and a fresh start for every protocol engine.
void detach_reset(std::uint8_t status0) {
    pdtrace::cc(pdtrace::CcEvent::Detach, g_active_cc, status0);
    // No scan ran while attached: the period restarts here, so the next scan is kScanPeriodMs away (the old 25-poll cadence
    // gave 0..500 ms by chance). Only the no-Rp detach in poll() kicks it sooner, after this.
    g_scan_pace.scanned(systick::millis());
    if (g_ra && !emkra::survives_detach(g_ra_run)) {
        g_ra = false;
        K2LOG("[EMKRA] off (detach)\n");
    }
    pins::cc_vconn_gnd.write(g_ra);   // VCONN off before anything else changes (the Ra mode keeps PC13 high)
    g_vconn_applied = false;
    g_vconn.reset(pd::VconnWhy::Abort);
    if (iq_running()) iq().abort(pd::IqWhy::Detached, systick::millis());   // nobody left to ask or to reset
    // No SW_RES, on a plain or a pinned boot alike: it restores the chip's power-on SWITCHES0 (0x03, Rd on both
    // lines) until the next write - through the closed CC switches a pinned boot's source would see the Debug
    // Accessory pair for ~0.13 ms, and on a plain boot a host and a dock negotiating through the K2 can see it.
    // PD_RESET leaves SWITCHES0 alone; the write below and configure_passive() put everything back.
    wr(kReset, kPdReset);
    // A far sink belongs to one attach (g_requests_base moves on). The inline latch is the Link's: it holds it through a VBUS gap and
    // clears it with set_inline(false), so a detach never clears it nor writes the pin's Rd back under it (hardware 2026-11-09: that
    // write and the clear made a hub-powered K2 flap, Rd -> Request -> load -> inline -> VBUS collapse -> detach -> Rd again).
    rebase_far_sink();
    // A lab pin (pd-reread --pin) ends with the far end's detach: the next attach must be passive, so the
    // pin and its mark go now (a warm reset would otherwise re-inherit the lab pin). A boot / comeback pin
    // is not a lab pin: it keeps its Rd and its mark (it is what powers the K2; the /rd.rec comeback is untouched).
    if (g_detach.reason() == pd::DetachReason::Sink && g_lab_pin) {
        g_lab_pin = false;
        g_rd_inherited = false;
        set_boot_pin(0);
        g_pin_rd = 0;   // SWITCHES0 is written just below (downstream_now() or the now-empty pin)
        K2LOG("[PIN] lab pin cleared (the far end detached)\n");
    }
    wr(kSwitches0, downstream_now() ? std::uint8_t(0) : g_pin_rd);
    g_rd_op = false;
    g_wake_attach = false;
    g_cc_sink_mv = -1;
    g_active_cc = 0;
    g_bc_max = 0;
    set_cc_mask(3);  // the next scan tries every routing again
    g_vbus_mv = -1;
    rdseek().reset(g_idle_wake.enabled());   // the next attach decides again whether its line was picked without Rd (the open readings stay only while the idle wake needs them)
    g_idle_wake.reset();   // back to idle: the wake re-decides from the fresh open readings
    g_linefb.reset();      // a detach ends the direct-plug schedule with the attach it belonged to
    g_seek_passive = false;
    g_detach.reset();
    sink().detach();
    g_prep.abort();
    ident_wait_refuse();   // an armed alone-Identify wait dies with the attach: refuse, hand the line back
    ident().abort(pd::IdWhy::Detached);
    probe().abort();
    reread().abort();
    g_getcap_armed = false;
    g_getcap_force = false;
    g_msg_id = 0;
    g_engine.reset_cable();  // the next sniff must not report the old cable as heard
    g_answered = {};
    g_read_info = {};
    g_pull_down = raclass::PullDown::NotProbed;
    g_engine.reset_sink();
    g_engine.reset_source();  // the offers go with the partner: nothing of charger A may serve charger B
    g_new_source = true;
    g_caps_acked = false;
    g_session.detach();
    g_engine.sink = {};
#if K2_HOST_REPORT
    if (LoopHost* const l = g_lh) {   // the session dies with the attach
        g_lh = nullptr;
        g_lh_dfp = false;
        l->on_detach(systick::millis());
    }
#endif
    configure_passive();
    flush_fifos();
    // No Rd hold: plain boot has no Rd to hold off, and a pinned boot mode needs its Rd back at once (a cheap
    // charger never turns VBUS on without it, hardware 2026-11-01). The hold only delayed the next scan.
    g_rd_hold.cancel();
}

}  // namespace detail

// The START wait loop blocks the main loop, so poll()'s 2 Hz scan never runs inside it. Without this the charger's Rp
// is noticed only after the wait, and the caps it sends meanwhile get no Request (Lenovo C135, hardware 2026-11-03:
// attach at the next scan, caps 27 ms later, Hard Reset 57 ms after those). One scan per call, ~25 ms of I2C.
void attach_scan() {
    if (g_active_cc != 0 || g_rd_hold.held(systick::millis())) return;
    detect_cc_orientation();
}

namespace detail {

// Nobody else owns the Rd of an idle / dark port: no boot pin, run start, active operation, debug Ra or forced line, and no
// sink downstream. The two idle-wake gates in poll() (the hold itself, the dark-attach release) share it. A charger-test run
// owns the line for its whole length (g_run_owner): its unit cycle drops its own Rd and the source's VBUS between units, and
// that gap looked like a sleeping port to the wake (bench 2026-11-10: every PD row "no PD after re-attach"). A tool that holds
// the link (identify, probe, re-read, trigger prep) owns it too: a dark moment inside its window is not a dark port.
bool idle_owner_free() {
    return g_pin_rd == 0 && !g_rd_start && !g_rd_op && !g_ra && g_force_cc == 0 && !downstream_now() && !g_run_owner &&
           !ident_running() && !probe_running() && !reread_running() && !g_prep.active();
}

}  // namespace detail

namespace {
// The unattached scan, when pd::ScanPace says so; from poll() and, during long redraws, from scan_service().
K2_HOT void scan_if_due() {
    const std::uint32_t now = systick::millis();
    if (g_active_cc == 0 && g_scan_pace.due(now) && !g_rd_hold.held(now) && !g_idle_wake.reading(now)) {
        detect_cc_orientation();
        g_scan_pace.scanned(systick::millis());
    }
}
}  // namespace

K2_HOT void poll() {
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
    // pattern slow-rp-wake, idle path (2026-11-10): the idle/no-source path holds our Rd on the one line that read the
    // standby signature open (pd::IdleWake) for kSlowWakeDwellMs at a time, so a sleeping source (the splitter's port,
    // VBUS 78 mV, "No source") is asked without any pin and its Rp - 1072 ms after the Rd, measured - lands on the next
    // scan. Only when nothing else owns the idle Rd: plain passive idle, no sink downstream, VBUS dark, no debug hold.
    if (g_active_cc == 0 && idle_owner_free() && !g_rd_hold.held(systick::millis()) && g_vbus_mv < pd::kDarkVbusMv) {
        const std::uint8_t was = g_idle_wake.line();
        const std::uint32_t was_since = g_idle_wake.since();
        const std::uint8_t held = g_idle_wake.update(systick::millis(), rdseek().open_mv(1), rdseek().open_mv(2));
        // Two dwells on one line with no Rp: that pick is doubtful (a spike noted as the signature, a flipped cable). Rd comes
        // off for kIdleReadMs, the stale open readings are dropped, and the next stable pair decides again - the same line is
        // not re-armed for ever.
        if (g_idle_wake.take_reread()) {
            rdseek().invalidate_open();
            wr(kSwitches0, idle_rd());
            K2LOG("[FUSB302] Idle wake: no Rp on CC%u after %u dwells, Rd released for %u ms to re-read the open levels (now cc1=%d cc2=%d mV)\n",
                  unsigned(was), unsigned(pd::kIdleUnansweredDwells), unsigned(pd::kIdleReadMs), int(g_cc1_mv), int(g_cc2_mv));
        }
        // Each dwell is logged (the line alone changes rarely): the Rd reaches the chip now, not with the next scan, and the
        // SWITCHES0 readback and the live ADC levels show on the bench whether it did and what the held net did under it.
        else if (held != was || (held != 0 && g_idle_wake.since() != was_since)) {
            wr(kSwitches0, idle_rd());
            std::uint8_t back = 0;
            rd(kSwitches0, back);
            K2LOG("[FUSB302] Idle wake: Rd on CC%u for %u ms (open cc1=%d cc2=%d mV, now cc1=%d cc2=%d mV, sw0=0x%02X)\n",
                  unsigned(held), unsigned(pd::kSlowWakeDwellMs), int(rdseek().open_mv(1)), int(rdseek().open_mv(2)), int(g_cc1_mv),
                  int(g_cc2_mv), unsigned(back));
        }
    }
    // No scan inside the idle wake's Rd-release window either: Rd is off there, and the unrestricted scan would take the
    // board's 1.7 V CC2 net as an attach (the window is only kIdleReadMs; the next scan after it is held to the picked line).
    // Paced by pd::ScanPace: every kScanPeriodMs, or soon after a detach, a VBUS rise or a new Rd-window line (a source's
    // offers follow its Rp within a few hundred ms; the plain 2 Hz scan missed them, pd_scanpace.hpp).
    scan_if_due();

    // Detach rule: VBUS below 3.0 V for 1 s, or no Rp on the active line for 500 ms. BMC
    // bursts pull the line low for microseconds, which the 500 ms hold rides out.
    if (g_active_cc != 0) {
        g_scan_pace.attached();   // only edges seen while unattached may hurry the scan (a no-Rp detach below kicks anew)
        std::uint8_t status0 = 0;
        static std::uint8_t last_status0 = 0xFF;
        const bool got = rd(kStatus0, status0);
        if (got && (status0 & 3) > g_bc_max) g_bc_max = std::uint8_t(status0 & 3);
        if (got) sink().on_rp(std::uint8_t(status0 & 3), systick::millis());   // SinkTxOK / SinkTxNG for the sink's own messages
        if (got && status0 != last_status0) { last_status0 = status0; pdtrace::status0(status0); }
        const bool sink_ok = !inline_now() || pd::sink_present(g_cc_sink_mv);
        // Only a known reading inside the sink band proves a sink is on the line; the sink-left timer
        // starts after that, never for a line that has not shown a sink since inline began.
        if (inline_now() && g_cc_sink_mv >= 0 && sink_ok) g_detach.sink_seen();
        // Our Rd on a line a Rd-less scan picked, and no Rp answers it: try the other line before the no-Rp rule gives up.
        // pattern slow-rp-wake: while the seek dwells on a standby-signature line (a sleeping source, its Rp measured
        // 1072 ms after the Rd), the 500 ms no-Rp detach must not fire inside that window - only the CC rule is held off.
        // The idle wake's window is the same: an attach that landed during an idle Rd hold on a standby-signature line,
        // and only until its Rp is seen (on_rp_seen ends it, like the seek's rp_seen).
        if (got && (status0 & kComp) != 0) g_idle_wake.on_rp_seen();
        // A dark attach (no VBUS, no PD for kDarkAttachMs) with the setting on is the sleeping port itself: the passive scan
        // latched its Rp but only an Rd of ours wakes it, and an attach keeps the idle path (the hold) off for good. Release it.
        // "No PD for N ms" counts from the attach or the last frame / unit reset, whichever is later: an attach long past whose
        // PD bookkeeping a unit cycle cleared is not a dark port the instant VBUS drops.
        const std::uint32_t now_ms = systick::millis();
        const bool rx_after_attach = std::int32_t(g_last_rx_ms - g_attach_ms) > 0 && std::int32_t(now_ms - g_last_rx_ms) >= 0;
        const std::uint32_t quiet_since = rx_after_attach ? g_last_rx_ms : g_attach_ms;
        if (idle_owner_free() && g_idle_wake.dark_attach(systick::millis(), quiet_since, g_vbus_mv, g_pd_traffic)) {
            K2LOG("[FUSB302] Detach: dark attach (VBUS %d mV, no PD for %u ms): the idle wake takes the line (cc=%d mV)\n",
                  int(g_vbus_mv), unsigned(systick::millis() - quiet_since), int(g_cc_sink_mv));
            detach_reset(status0);
        } else if (got && rdseek().update(systick::millis(), g_active_cc, rd_seek_wanted(), (status0 & kComp) != 0)) rd_seek_move();
        else if (got && g_linefb.update(now_ms, line_fallback_eligible(), (status0 & kComp) != 0, g_vbus_mv < pd::kDarkVbusMv,
                                        line_fallback_other_rp()))
            line_fallback_move();
        else if (got && g_detach.update(systick::millis(), (g_iq_inited && iq().resetting()) ? -1 : g_vbus_mv,   // an interrogation's reset drops VBUS on purpose
                                        (status0 & kComp) != 0 || rdseek().holding_slow(systick::millis(), g_active_cc) ||
                                            g_idle_wake.holding_slow(systick::millis(), g_active_cc, rdseek().open_mv(g_active_cc)),
                                        sink_ok)) {
            K2LOG("[FUSB302] Detach: %s (cc=%d mV, inline=%d)\n", pd::detach_reason_name(g_detach.reason()),
                         int(g_cc_sink_mv), int(inline_now()));
            // No Rp on the line: it may have been the wrong one, or the partner is already back. Scan on the next poll, not up
            // to 500 ms later (pd_scanpace.hpp). A VBUS-low or dark detach leaves the Rp where it was: rescanning at once would
            // only latch it again sooner, so those keep the plain cadence.
            const bool no_rp = g_detach.reason() == pd::DetachReason::Cc;   // read first: detach_reset() clears the reason
            detach_reset(status0);
            if (no_rp) g_scan_pace.kick();
        }
    }

    service();
}

// Receive and answer: hard reset, FIFO drain, transmit engines. Cheap when idle (two register
// reads), so the main loop runs it every millisecond to meet tSenderResponse (~30 ms).
K2_HOT void service() {
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
            if (!inline_now()) g_detach.hard_reset(systick::millis());   // the source resets our link: its VBUS gap is no detach
            sink().on_hard_reset_rx(systick::millis());
#if K2_HOST_REPORT
            if (g_lh) {   // the host's Hard Reset: both ends are back to their default roles (UFP)
                g_lh_dfp = false;
                write_sw1();
                g_lh->on_hard_reset(systick::millis());
            }
#endif
            ident().abort(pd::IdWhy::HardReset);
            probe().abort();
            reread().on_link_reset();   // an attach still waiting for its contract goes on: the sink re-requests after a hard reset
            iq().on_hard_reset(systick::millis());   // the charger's or the device's: a run sniffs the renegotiation and goes on
        }

        g_rx_nonempty = false;
        for (int pkt = 0; pkt < 4; pkt++) {
            // A frame transmit() took out of the FIFO comes first (rx_ack_peek), then the FIFO.
            const bool held = g_rx_held.valid;
            std::uint8_t status1;
            if (!held && (!rd(kStatus1, status1) || (status1 & kRxEmpty))) break;
            g_rx_nonempty = true;
            g_last_rx_ms = systick::millis();
            std::uint32_t edge_us = 0;
            if (pkt == 0) {
                if (g_rxstamp.take(systick::micros(), edge_us)) pdtrace::stamp_rx(edge_us);   // the INT_N edge, not this read
                pdtrace::poll_gap();
            }

            std::array<std::uint8_t, 3> pre;  // token, PD header
            std::array<std::uint8_t, 36> body;  // data objects + 4 CRC bytes
            if (held) {
                pre = g_rx_held.pre;
                body = g_rx_held.body;
                g_rx_held.valid = false;
            } else if (!fifo_read(pre)) break;
            const unsigned tok = pre[0] & 0xE0;
            if (tok != 0xE0 && tok != 0xC0 && tok != 0xA0) {  // not SOP, SOP' or SOP''
                pdtrace::rx_bad(pre[0], pre[1], pre[2]);
                flush_rx();
                break;
            }
            const std::uint16_t header = pre[1] | (pre[2] << 8);
            const unsigned num_do = (header >> 12) & 7;

            if (!held && !fifo_read({body.data(), num_do * 4 + 4})) break;
            pdtrace::rx(pre[0], header, {body.data(), num_do * 4 + 4});
            if (num_do == 0 && !(header & 0x8000) && (header & 0x1F) == pd::kCtlSoftReset) pdtrace::reset(pdtrace::ResetKind::SoftRx);
            clear_irqs();
            // A Hard Reset caught by that read: this frame is the old link's (the Baseus sent it after a caps nobody
            // answered in time). Drop it; the next pass resets the engine and the FIFO.
            if (g_hr_latched) break;

            sink().set_stamp_us(edge_us);   // consumed by the sink's on_message if this frame reaches it
            handle_frame(sop_from_token(pre[0]), header, {body.data(), num_do * 4});
            sink().set_stamp_us(0);
        }
        if (!g_irq && !g_rx_nonempty) break;
    }
    if (g_active_cc != 0) active_tick();
    else if (reread_running()) reread().tick(systick::millis(), false, reread_link());   // an attach whose source has not shown yet: its wait still ends
    iq_report();   // also after a detach ended a run
    reread_housekeeping(systick::millis());
    ident_result_gate(systick::millis());
    // An alone-Identify runs its first VDM only on a Ready contract of this attach, settled kIdentSettleMs:
    // the phone sources only after our Rd, and a VDM sent earlier only burns its retries (Pixel: Discover
    // Identity 1 ms after Rd went RETRYFAIL). Bounded timeout, then a clear refusal and the line back.
    if (g_ident_wait) {
        const std::uint32_t now2 = systick::millis();
        // The premise is re-checked while waiting: not inline, and a Ready contract of this attach on a live line.
        // Not the wait verdict: identify_start checked DualRolePort, and the verdict leaves DualRolePort as soon as
        // the phone sources VBUS, which is exactly when the contract goes Ready (hw: OP15 timed out on a Ready line).
        const bool ready = !inline_now()
                           && sink().status().state == pd::SinkState::Ready && g_session.fresh() && g_active_cc != 0;
        if (ready && g_ident_ready_at == 0) g_ident_ready_at = now2;   // a plain stamp: `now | 1` wraps the settle when now is even
        if (ready && now2 - g_ident_ready_at >= kIdentSettleMs) {
            g_ident_wait = false;
            g_ident_session = g_session.id();
            g_ident_result_valid = true;
            g_ident_alone_hold = false;
            ident().start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, now2);
            g_ident_was_running = true;
            K2LOG("[ID] alone: Ready contract after %u ms, Identify started\n", unsigned(g_ident_ready_at - g_ident_wait_at));
        } else if (now2 - g_ident_wait_at >= kIdentWaitTimeoutMs) {
            ident_wait_refuse();
        }
    }
    if (g_ident_was_running && !ident_running()) {
        g_ident_was_running = false;
        const pd::IdStatus& st = ident().status();
        g_ident_identity = g_engine.vdm.identity.valid ? g_engine.vdm.identity : g_engine.sink_vdm.identity;
        K2LOG("[ID] result: state %d ok 0x%02X skipped 0x%02X retries %u why %d\n", int(st.state), unsigned(st.ok_mask),
              unsigned(st.skipped_mask), unsigned(st.retries), int(st.why));
        if (g_ident_alone) alone_handback();
    }
    // The alone-Identify's own Rd (it presented Rd because the dark VBUS had detached the line): give it back
    // once the sequencer is over (or the wait gave up), so the phone sees a detach and stops sourcing VBUS into the K2.
    if (g_ident_owns_rd && !ident_running() && !g_ident_wait) {
        g_ident_owns_rd = false;
        if (g_rd_start) {
            set_rd_for_run(false, false);
            K2LOG("[ID] alone attach released: Rd off\n");
        }
    }
    trace_sink();
}

// ---- active PD API ----

// A sink other than the K2 asked the source for power in this attach (a Request with the power-role bit clear: the
// K2 never hears its own TX). Cleared with the attach and with the detach (g_requests_base).
bool far_sink_seen() { return g_engine.contract.requests != g_requests_base; }

pd::TriggerRefusal trigger_refusal() {
    if (g_active_cc == 0) return inline_now() ? pd::TriggerRefusal::Inline : pd::TriggerRefusal::None;
    return pd::can_trigger({inline_now(), g_detach.sink_was_seen(), far_sink_seen()});
}

// Lab-only over-load facts, one derivation from the existing foreign-evidence state: the far-sink
// half is what downstream_now() already tracks (the inline latch, a sink Rd read while inline, a foreign Request; a sink
// Rd level on CC alone is not evidence, user decision 2026-11-15); the far-PD-message
// half is what the engine already attributes to a sink (a Request we did not send, any of its
// control messages - GoodCRC, Get_Source_Cap - its Sink_Capabilities, its VDMs). All of it belongs
// to one attach (rebase_far_sink / apply_cc_choice / reset_sink / reset_source).
pd::OverLoadFacts over_load_facts() {
    const bool far_req = far_sink_seen();
    return {inline_now() || g_detach.sink_was_seen() || far_req,
            far_req || g_engine.sink_ctl_seen || g_engine.sink.valid || g_engine.sink_vdm.num_svids != 0};
}

namespace detail {
// The Request itself, with no ownership change: the re-read's own attach asks through it (reread_start); everyone else goes
// through sink_request below, which also takes the re-read's attach over.
pd::Err request_inner(const pd::Choice& c, bool over_load) {
    if (g_legacy_only) return pd::Err::Busy;
    if (const pd::TriggerRefusal r = trigger_refusal(); r != pd::TriggerRefusal::None) {
        // Lab-only bypass (`k2_remote pd request ... --over-load`): allowed only while the far sink is a
        // passive load (Rd only, no PD message this attach). Refused otherwise, and the UI / charger
        // test never pass over_load.
        if (!over_load || !pd::over_load_allowed(over_load_facts())) {
            K2LOG("[PD] trigger refused: inline (%s)\n", pd::trigger_refusal_name(r));
            return pd::Err::Inline;
        }
        K2LOG("[PD] trigger over-load: far sink attached, no far PD message this attach (lab only)\n");
    }
    if (g_active_cc == 0) return pd::Err::NoCaps;
    if (sink().hv_lock() && c.pos > 1) {
        // The HID-cable loop 5 V lock (k2/core/loop_guard.hpp): only PDO 1 (vSafe5V) may be asked for. Checked before the
        // un-ACKed-caps preparation below, which would otherwise answer Ok and refuse later; pd::Sink::trigger holds the same rule.
        K2LOG("[LOOP] request pos %d %d mV refused: 5V lock\n", c.pos, c.mv);
        return pd::Err::OverCap;
    }
    if (ident_running() || probe_running() || g_prep.active()) return pd::Err::Busy;
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
        return pd::Err::Ok;
    }
    const pd::Err e = sink().trigger(c, systick::millis());
    log_trigger(c, e);
    trace_sink();
    return e;
}
}  // namespace detail

// A Request that was accepted for sending is a tool talking on the link: the re-read's attach (Rd, PDO 1 contract) is the
// tool's from here on, whatever happens to `held` later (a Soft_Reset step drops it). A refused Request changes nothing.
pd::Err sink_request(const pd::Choice& c, bool confirmed, bool over_load) {
    (void)confirmed;  // the UI / remote layer owns the "above 5 V needs confirm" rule
    const pd::Err e = request_inner(c, over_load);
    if (e == pd::Err::Ok) reread_take_over();
    return e;
}
void sink_release() {
    g_prep.abort();
    sink().release(systick::millis());
}
void sink_keepalive_silence(pd::KaSilence k) { sink().silence_keepalive(k, systick::millis()); }
bool sink_hard_reset(bool lab_inline) {
    if ((inline_now() && !lab_inline) || g_active_cc == 0) {
        K2LOG("[PD] Hard Reset refused (no CC / inline)\n");
        return false;
    }
    g_hr_tool_armed = true;   // the explicit tool is an active feature: its one Hard Reset may go out even from idle
    sink().hard_reset(systick::millis());
    return true;
}
void sink_set_cap(std::uint32_t mv) { sink().set_cap_mv(mv); }
void sink_set_hv_lock(bool on) {
    if (on == sink().hv_lock()) return;
    if (on) g_prep.abort();   // a re-announce prepared for a raise must not send it
    sink().set_hv_lock(on, systick::millis());
    trace_sink();
}
void sink_set_vbus_external(bool on) { sink().set_vbus_external(on, systick::millis()); }
void sink_cap_override(const std::uint32_t* pdos, std::uint8_t n) { sink().set_sink_cap_override(pdos, n); }
void sink_cc(std::int32_t cc1_mv, std::int32_t cc2_mv) {
    g_cc1_mv = cc1_mv;   // kept for the attach scan's ADC cross-check (pd::rd_window_line) and its re-run on fresh levels
    g_cc2_mv = cc2_mv;
    // pattern slow-rp-wake: keep the OPEN level per line from before our Rd went on - the Rd seek reads it back to tell
    // a sleeping source's standby pull-up (~3.0-3.3 V open, collapsing under our Rd) from a coupled net. Under our Rd the
    // level is our own divider and says nothing, so nothing is noted while any Rd is presented. These levels also arrive
    // one tick late: the feed ignores the first kOpenSettleMs after any Rd change (the old Rd's tail) and takes a level
    // only when two samples agree (CC2's measurement-pulse spike is no standby signature).
    rdseek().feed_open(systick::millis(), (g_sw0 & (kPdwn1 | kPdwn2)) != 0, cc1_mv, cc2_mv);
    cc_window_recheck();   // fresh levels while attached with no PD decoded yet: re-run the Rd-window cross-check
    g_scan_pace.feed_window_line(pd::rd_window_line(cc1_mv, cc2_mv));   // a new live net kicks the unattached scan
    // The CC line closest to 1.5 V; the other one is the unused line of the cable.
    auto dist = [](std::int32_t v) { const std::int32_t d = v - pd::kSinkBandCenterMv; return d < 0 ? -d : d; };
    g_cc_sink_mv = dist(cc1_mv) <= dist(cc2_mv) ? cc1_mv : cc2_mv;
    // No verdict is drawn from a sink-band level while the K2 presents no Rd: a floating CC2 reads ~1.75 V on a USB-A port with
    // nothing plugged in (hardware 2026-11-15), so a level is never evidence of a sink behind the K2.
}
void scan_service(std::int32_t vbus_mv) {
    if (g_active_cc != 0) return;
    if (vbus_mv >= 0) g_scan_pace.feed_vbus(vbus_mv);
    scan_if_due();
}
void sink_vbus(std::int32_t mv) {
    g_vbus_mv = mv;
    g_scan_pace.feed_vbus(mv);   // VBUS rising through 3.5 V kicks the unattached scan
    sink().on_vbus(mv, systick::millis());
}
const pd::SinkStatus& sink_status() { return sink().status(); }
const pd::XiaomiTrace& xiaomi_trace() { return sink().xiaomi_trace(); }
pd::Err epr_info_request(bool force) { return sink().request_epr_info(systick::millis(), force); }
const pd::SourceCaps& epr_info_list() { return g_engine.epr_info; }
pd::Err apple_read_start() { return sink().arm_apple_read(systick::millis()); }
const pd::AppleInfo& apple_info() { return sink().apple_info(); }
bool apple_armed() { return sink().apple_armed(); }
bool answers_sop() { return sink().answers_sop(); }
const pd::PpsAnswer& pps_answer() { return sink().pps_answer(); }
std::uint32_t caps_count() { return g_caps_count; }
bool legacy_only_active() { return g_legacy_only; }

pd::BusyWhy busy_why() {
    if (g_legacy_only) return pd::BusyWhy::Legacy;
    if (ident_running()) return pd::BusyWhy::Identify;
    if (probe_running()) return pd::BusyWhy::Probe;
    if (g_prep.active()) return pd::BusyWhy::Prepare;
    switch (sink().status().state) {   // the gate of pd::Sink::trigger
        case pd::SinkState::EprEntering: case pd::SinkState::EprWaitCaps: case pd::SinkState::Requesting:
        case pd::SinkState::Transition: case pd::SinkState::Aborting:
            return pd::BusyWhy::Negotiating;
        default: return pd::BusyWhy::None;
    }
}

bool xiaomi_chain_active() { return sink().xiaomi_chain_active(systick::millis()); }
bool idle_wake() { return g_idle_wake.enabled(); }
void set_run_owner(bool on) { g_run_owner = on; }
bool run_owner() { return g_run_owner; }
bool ra_active() { return g_ra; }
emkra::Refuse ra_refusal() { return g_ra_refused; }
void ra_run_own() { if (g_ra) g_ra_run = true; }
const pd::RereadStatus& reread_status() { return reread().status(); }
bool reread_own_hold() { return pd::reread_own_hold(sink().status(), g_reread_made_contract); }
void reread_take_over() {
    if (!g_reread_owns_rd && !g_reread_made_contract) return;   // nothing of a re-read to take over (the common case)
    K2LOG("[REREAD] attach taken over: Rd %d, contract %d stay with the new owner\n", int(g_reread_owns_rd), int(g_reread_made_contract));
    g_reread_owns_rd = false;
    g_reread_made_contract = false;
}
void set_sink_cap_ext_mode(pd::SinkCapExtMode m) {
    sink().set_sink_cap_ext_mode(m);
    K2LOG("[PD] Get_Sink_Cap_Extended answer mode %u\n", unsigned(m));
}
void set_proactive_discover(bool on) { sink().set_proactive_discover(on); }
void probe_abort() { probe().abort(); }
const pd::ProbeStatus& probe_status() { return probe().status(); }
void identify_abort() { ident().abort(pd::IdWhy::Stopped); }
const pd::IdStatus& identify_status() { return ident().status(); }
bool identify_busy() { return g_ident_wait || ident_running(); }
bool identify_current() {
    if (g_ident_session == 0 || !g_ident_result_valid) return false;
    // After an alone-Identify's release the session has moved on (the phone's VBUS went dark): the result is
    // still this partner's until the port is really gone (set_wait_kind) or another device's caps arrive.
    return g_ident_session == g_session.id() || g_ident_alone_hold;
}
const pd::Identity& identify_identity() { return g_ident_identity; }
bool interrogate_running() { return iq_running(); }
const pd::IqStatus& interrogate_status() { return iq().status(); }

void feed(std::uint8_t sop, std::uint16_t header, std::span<const std::uint8_t> payload) {
    handle_frame(sop == 1 ? pd::Sop::SopP : sop == 2 ? pd::Sop::SopPP : pd::Sop::Sop, header, payload, false);
}

const pd::SourceCaps& caps() { return g_engine.caps; }
const pd::Emarker& emarker() { return g_engine.cable; }
emk_variant::Answered cable_answered() { return g_engine.cable.valid ? g_answered : emk_variant::Answered{}; }
emk_attached::ReadInfo last_cable_read() { return g_read_info; }
raclass::PullDown pull_down() { return g_pull_down; }
void set_vbus_source(VbusFn vbus_mv) { g_vbus_source = vbus_mv; }
const pd::VdmInfo& vdm() { return g_engine.vdm; }
const pd::VdmInfo& sink_vdm() { return g_engine.sink_vdm; }
usblink::Link usb_link(std::int32_t dp_mv, std::int32_t dm_mv) {
    usblink::Classifier c = g_engine.usb;
    c.on_lines(dp_mv, dm_mv);
    return c.link();
}
usblink::Raw usb_raw() { return g_engine.usb.raw(); }
const pd::ExtInfo& ext() { return g_engine.ext; }
bool cable_thunderbolt() { return g_engine.cable_thunderbolt(); }
std::uint32_t rx_count() { return g_engine.caps.rx_count; }
std::uint8_t device_id() { return g_device_id; }
std::uint8_t active_cc() { return g_active_cc; }
const pd::Contract& contract() { return g_engine.contract; }
// Period of the last two sink Requests decoded this attach, in ms (0 = fewer than two, or a fresh
// attach). A Request repeated every <= 10 s with no Source_Capabilities on record is the PPS
// keep-alive pattern (home footer label).
std::uint32_t request_period_ms() { return g_req_period_ms; }
const pd::SinkCapsSeen& sink_caps() { return g_engine.sink; }

void set_sop_prime_hook(SopPrimeHook hook) {
    g_sop_prime_hook = hook;
    if (hook != nullptr) g_tx_allowed = true;   // the armed spoof is an active feature: its SOP' replies may go out at once
    if (g_active_cc != 0) write_sw1();   // SOP' enable follows the hook
}

// A controlled re-attach: drop the Rd (SWITCHES0 pull-downs off; the Ra mode's PC13 stays) for a gap the caller times, then put it
// back: the source sees a detach and a fresh attach, now with the Ra present, and may read the cable it skipped. The caller
// times the gap and watches VBUS (the lab op, the charger test's planned reboot). At the end the engine and the sink forget the
// old contract, as on a detach. A K2 that lives on this source's VBUS may die in the gap: the comeback boot (k2/core/rd_intent.hpp)
// finishes the job.
void rd_glitch_begin() {
    wr(kSwitches0, 0);
    K2LOG("[PD] rd glitch: Rd off (ra=%d)\n", int(g_ra));
}

void rd_glitch_end() {
    sink().detach();
    g_engine.reset_source();
    g_new_source = true;
    apply_rd();
    K2LOG("[PD] rd glitch: Rd back\n");
}

void vconn_off() { vconn_force_off(pd::VconnWhy::Abort); }

#if K2_HOST_REPORT
// ---- the Host report's line (fusb302.hpp, LoopHost) ----
void loop_host_begin(LoopHost* l) {
    if (l == g_lh) return;
    g_lh = l;
    g_lh_dfp = false;
    g_lh_idle = false;
    if (g_active_cc != 0) write_sw1();
    apply_rd();
    K2LOG("[LPHR] line %s (inline=%d cc=%u)\n", l ? "owned: sole sink, no Rd, no Hard Reset" : "released", g_inline ? 1 : 0,
          unsigned(g_active_cc));
}
bool loop_host_send(const pd::TxMsg& m) {
    if (!g_lh || m.hard_reset || m.sop != pd::Sop::Sop || g_active_cc == 0) return false;
    return transmit(m);
}
void loop_host_set_dfp(bool dfp) {
    if (!g_lh || dfp == g_lh_dfp) return;
    g_lh_dfp = dfp;
    write_sw1();
}
bool loop_host_soft_reset() {
    if (!g_lh || g_active_cc == 0 || sink().status().state != pd::SinkState::Ready) return false;
    sink().soft_reset(systick::millis());
    trace_sink();
    return true;
}
bool loop_host_idle() { return g_lh_idle; }
std::uint8_t loop_host_rp() { return g_active_cc != 0 ? g_bc_max : std::uint8_t(0); }
#endif

}  // namespace k2::fusb302

// EXTI lines 10..15 share this vector; only line 12 is enabled.
extern "C" K2_HOT void EXTI15_10_IRQHandler() {
#ifndef K2_HOST_PINS
    k2::reg::exti().PR = 1u << 12;
#endif
    k2::fusb302::on_int_n();
}
