// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cold setup, self-powered boot/pin, VCONN/SOP' e-marker scan, CC/Ra probes, and lab control paths for FUSB302.
// Compiled under COLD_SIZE_FLAGS (-Oz) by keeping K2_HOT out of this translation unit; hot/tick/ISR stay in fusb302.cpp.
#include "fusb302_internal.hpp"

#include <array>
#include <span>

#include "k2/core/log.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/pin_mark.hpp"
#include "k2/drivers/pins.hpp"
#include "k2/drivers/regs.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/pd/emk_variant.hpp"
#include "k2/pd/pd_rescan.hpp"
#include "k2/pd/pd_survey.hpp"
#include "k2/pd/pdtrace.hpp"

namespace k2::fusb302 {

namespace {

using systick::delay_ms;
namespace phy = pd::phy;
using namespace pd::phy;
using namespace detail;

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

constexpr std::array<std::array<std::uint8_t, 14>, emk_variant::kAttempts> make_di_frames() {
    std::array<std::array<std::uint8_t, 14>, emk_variant::kAttempts> f{};
    for (std::size_t i = 0; i < f.size(); i++) f[i] = emk_variant::discover_identity_frame(emk_variant::kTable[i]);
    return f;
}
constexpr auto kDiFrames = make_di_frames();
constexpr std::size_t kNoAnswer = emk_variant::kAttempts;

std::size_t scan_orientation(std::uint8_t sw0, std::uint8_t sw1, bool pc13, bool attached) {
    cc_switches(pc13);
    wr(kPower, kPwrAll);
    wr(kSwitches0, sw0);
    wr(kSwitches1, sw1);
    delay_ms(40);  // VCONN must power up the cable's e-marker before it can answer

    for (std::size_t attempt = 0; attempt < emk_variant::kAttempts; attempt++) {
        const emk_variant::Variant var = emk_variant::kTable[attempt];
        flush_fifos();
        delay_ms(2);
        clear_irqs();
        if (attached && g_hr_latched) return kNoAnswer;
        const auto& di = kDiFrames[attempt];
        fifo_write_tx(std::uint16_t(di[5] | (di[6] << 8)), {di.data() + 7, 4}, di);
        wr(kControl0, kTxStart);
        bool tx_seen = false, acked = false;

        for (int t = 0; t < 40; t++) {  // up to 80 ms for the answer
            delay_ms(2);
            std::uint8_t ia = 0;
            if ((!tx_seen || attached) && rd(kInterruptA, ia)) {
                if (ia & kIntHardRst) {
                    g_hr_latched = true;
                    if (attached) return kNoAnswer;
                }
                if (!tx_seen && (ia & (kIntTxSent | kIntRetryFail))) {
                    tx_seen = true;
                    acked = (ia & kIntTxSent) != 0;
                    if (acked && g_read_info.acks < 255) g_read_info.acks++;
                    K2LOG("[EMK] sw0=0x%02X attempt %d (PD %d.0, SVDM %d.0): SOP' %s after %d ms\n", sw0, int(attempt),
                                var.spec_rev == 1 ? 2 : 3, int(var.svdm), acked ? "GoodCRC" : "RETRYFAIL (no GoodCRC)", (t + 1) * 2);
                }
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
            if (g_engine.cable.valid) return attempt;
        }
        if (acked)
            K2LOG("[EMK] sw0=0x%02X attempt %d (PD %d.0, SVDM %d.0): acked, no answer\n", sw0, int(attempt),
                  var.spec_rev == 1 ? 2 : 3, int(var.svdm));
    }
    return kNoAnswer;
}

void log_found_cable() {
    const pd::Emarker& e = g_engine.cable;
    K2LOG("[EMARK] E-Marker detected: VID=0x%04X (%s), PID=0x%04X\n", e.vid, e.vendor_str, e.pid);
    K2LOG("[EMARK] Rating: %d.0A / %dV (%dW), Speed: %s\n", e.max_current_a, e.max_voltage_v,
                e.max_power_w, e.speed_str);
    K2LOG("[EMARK] Cable Type: %s, HW: v%d, FW: v%d, XID: 0x%08X\n", e.is_active ? "Active" : "Passive",
                e.hw_rev, e.fw_rev, unsigned(e.xid));
    log_cable(e);
}

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

bool g_cable_open = false;
std::uint8_t g_cable_id = 0;

bool cable_exchange_impl(const emk_learn::Request& req, emk_learn::Reply& rep, std::uint32_t timeout_ms) {
    rep = {};
    if (!g_cable_open) return false;
    const unsigned n = req.n > emk_learn::kMaxObjs ? emk_learn::kMaxObjs : req.n;
    constexpr unsigned kMsgVdm = 0x0F;
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
    const std::uint32_t t0 = systick::millis();
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
            std::array<std::uint8_t, 3> pre;
            if (!fifo_read(pre)) break;
            const std::uint16_t h = std::uint16_t(pre[1] | (pre[2] << 8));
            const unsigned num_do = (h >> 12) & 7;
            std::array<std::uint8_t, 36> body;
            if (!fifo_read({body.data(), num_do * 4 + 4})) break;
            pdtrace::rx(pre[0], h, {body.data(), num_do * 4 + 4});
            if (sop_from_token(pre[0]) == pd::Sop::Sop) continue;
            if (num_do == 0 && (h & 0x1F) == pd::kCtlGoodCrc) { sent = true; continue; }
            rep.got = true;
            rep.header = h;
            rep.n = std::uint8_t(num_do);
            for (unsigned i = 0; i < num_do && i < rep.w.size(); i++)
                rep.w[i] = std::uint32_t(body[4 * i]) | (std::uint32_t(body[4 * i + 1]) << 8) |
                           (std::uint32_t(body[4 * i + 2]) << 16) | (std::uint32_t(body[4 * i + 3]) << 24);
        }
    }
    if (!failed) g_cable_id = (g_cable_id + 1) & 7;
    K2LOG("[EMK] cable tx hdr 0x%04X id %d: %s%s\n", header, int((header >> 9) & 7),
                failed ? "RETRYFAIL (no GoodCRC)" : sent || rep.got ? "GoodCRC" : "no GoodCRC seen",
                rep.got ? ", answered" : "");
    return !failed && (sent || rep.got);
}

bool read_emarker_impl(bool confirmed, pd::EmarkerMode mode, CableBody body, void* ctx) {
    if (mode == pd::EmarkerMode::Sniff) return sniff_emarker();
    g_read_info = {};
    if (!emkra::vconn_switch_allowed(g_ra)) {
        K2LOG("[EMARK] Refused: the Ra mode is on (the chip's VCONN switch would fight the Ra on CC1)\n");
        return false;
    }
    if (inline_now()) {
        K2LOG("[EMARK] Refused: inline with a live sink; the VCONN/SOP' scan never runs inline\n");
        return false;
    }
    (void)confirmed;
    const bool tx_was_allowed = g_tx_allowed;
    g_tx_allowed = true;
    g_engine.reset_cable();
    g_answered = {};
    g_pull_down = raclass::PullDown::NotProbed;
    g_scanned_valid = false;
    struct Pre { std::uint8_t power = kPwrAll, sw0 = 0, sw1 = 0; } pre;
    std::uint8_t v;
    if (rd(kPower, v)) pre.power = v;
    if (rd(kSwitches0, v)) pre.sw0 = v;
    if (rd(kSwitches1, v)) pre.sw1 = v;
    K2LOG("[EMARK] Scanning for E-Marker (Dual-orientation SOP')...\n");
    K2LOG("[EMK] snapshot POWER=0x%02X SW0=0x%02X SW1=0x%02X inline=%d active_cc=%d cable_acks=%u\n",
                pre.power, pre.sw0, pre.sw1, int(inline_now()), int(g_active_cc), unsigned(g_engine.cable_acks));
    std::uint8_t st0 = 0;
    const bool rd_up = (g_active_cc == 1 || g_active_cc == 2) &&
                       (pre.sw0 & (g_active_cc == 1 ? pd::phy::kPdwn1 : pd::phy::kPdwn2)) != 0;
    if (rd_up && !rd(kStatus0, st0)) st0 = 0;
    emk_attached::Facts facts;
    facts.active_cc = g_active_cc;
    facts.sw0 = pre.sw0;
    facts.vbus_mv = g_vbus_mv;
    facts.caps_valid = g_engine.caps.valid;
    facts.sink_busy = sink().status().state != pd::SinkState::Idle;
    const emk_attached::Plan plan = emk_attached::plan(emk_attached::attached(facts), g_active_cc);
    g_read_info = {plan.attached ? emk_attached::Mode::Attached : emk_attached::Mode::Alone, 0};
    if (rd_up)
        K2LOG("[EMK] scan mode %s: active_cc=%d vbus_mv=%d caps=%d sink=%d (diag vbus_ok=%d contract=%d)\n",
              emk_attached::mode_text(g_read_info.mode).data(), int(g_active_cc), int(g_vbus_mv), int(g_engine.caps.valid),
              int(sink().status().state), int((st0 & ccprobe::kStatus0VbusOk) != 0), int(g_engine.contract.valid));
    cc_switches(plan.pc13);
    g_cable_read = true;

    bool found = true;
    std::size_t row = kNoAnswer;
    std::uint8_t orient = 0;
    for (std::uint8_t i = 0; i < plan.count && row == kNoAnswer; i++) {
        row = scan_orientation(plan.steps[i].sw0, plan.steps[i].sw1, plan.pc13, plan.attached);
        orient = plan.steps[i].orientation;
    }
    g_answered = emk_variant::answered_from(orient, row);
    if (g_answered.valid()) {
        K2LOG("[EMARK] Response on Orientation %s, %s\n", emk_variant::orientation_text(g_answered).c_str(),
              emk_variant::combo_text(g_answered).c_str());
    } else {
        found = false;
    }

    if (found && body != nullptr) {
        g_cable_open = true;
        g_cable_id = 1;
        K2LOG("[EMK] cable session open\n");
        body(ctx);
        g_cable_open = false;
        K2LOG("[EMK] cable session closed\n");
    }
    g_cable_read = false;
    g_tx_allowed = tx_was_allowed;

    cc_switches(false, cc_mask());
    wr(kPower, pre.power);
    wr(kSwitches0, pre.sw0);
    wr(kSwitches1, std::uint8_t((pre.sw1 & ~kAutoCrc) | (g_crc_on ? kAutoCrc : 0)));
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
        if (!plan.attached) {
            raprobe::Result res;
            const raprobe::Refusal why = ra_probe(raprobe::Args{}, g_vbus_source, res);
            g_pull_down = raclass::classify(res);
            if (why != raprobe::Refusal::None)
                K2LOG("[EMK] pull-down probe: refused reason=%u verdict=%s\n", unsigned(why), raclass::name(g_pull_down));
            else
                K2LOG("[EMK] pull-down probe: verdict=%s n=%u flags=%u\n", raclass::name(g_pull_down), unsigned(res.n), unsigned(res.flags));
        }
        return false;
    }
    log_found_cable();
    if (!inline_now()) { g_scanned_cable = pd::cable_limits(g_engine.cable); g_scanned_valid = true; }
    return true;
}

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

void self_power_early(bootmode::Mode mode, bool warm) {
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
    g_lab_pin = false;
    g_rd_inherited = mode == bootmode::Mode::Plain && g_pin_rd != 0;
    set_boot_pin(g_pin_rd);
    wr(kSwitches0, g_pin_rd);
    set_cc_mask(bootmode::cc_mask(mode));
    cc_switches(false, cc_mask());
}

void self_power_early_ra() {
    if (g_pin_rd != kPdwn2) return;
    g_ra_early = true;
    pins::cc_vconn_gnd.write(true);
    boot_note(1, std::uint8_t((cc_mask() & 3) | 4));
}

void self_power_drop() {
    g_rd_inherited = false;
    set_boot_pin(0);
    self_power_leave();
    g_ra_early = false;
    if (!g_ra) pins::cc_vconn_gnd.write(g_vconn_applied);
    apply_rd();
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

void boot_readback_service() {
    if (g_readback_n >= kBootReadbacks) return;
    const std::uint32_t t = systick::millis();
    if (t < kReadbackAtMs[g_readback_n]) return;
    std::uint8_t v = 0;
    if (!rd(kSwitches0, v)) v = 0xFF;
    g_readbacks[g_readback_n++] = {std::uint16_t(t), 2, v};
}

void self_power_release() {
    if (!g_rd_inherited) return;
    g_rd_inherited = false;
    set_boot_pin(0);
    self_power_leave();
}

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
    if (g_active_cc == 0) wr(kSwitches0, 0);
}

void self_power_restore() {
    if (g_boot_pin_rd == 0 || g_pin_rd == g_boot_pin_rd) return;
    g_pin_rd = g_boot_pin_rd;
    if (g_active_cc == 0) wr(kSwitches0, g_pin_rd);
}

bool self_powered() { return g_pin_rd != 0; }
std::uint8_t pin_rd() { return g_pin_rd; }
LineTrial line_trial() {
    LineTrial t;
    t.moves = g_linefb.moves();
    t.live = g_active_cc != 0 && g_linefb.live(systick::millis());
    t.line = t.live ? g_active_cc : 0;
    return t;
}

bool debug_pin(std::uint8_t line) {
    if (line > 2 || inline_now()) return false;
    if (line == 0) {
        g_lab_pin = false;
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
    g_lab_pin = true;
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
    g_read_info = {};
    g_pull_down = raclass::PullDown::NotProbed;
    sink().bind(&g_engine.caps);
    sink().bind_epr_info(&g_engine.epr_info);
    g_msg_id = 0;
    g_rd_op = false;
    rdseek().clear();
    rdseek().rd_changed(systick::millis());
    g_idle_wake.reset();
    g_run_owner = false;
    g_wake_attach = false;

#ifndef K2_HOST_PINS
    reg::crm().APB2EN |= reg::Crm::APB2EN_GPIOC;
#endif
    for (const auto& p : {pins::cc_vconn_gnd, pins::cc1_switch, pins::cc2_switch}) p.mode(reg::Mode::OutPp50);
    cc_switches(false, cc_mask());
    K2LOG("[FUSB302] Hardware CC switches enabled (mask %d)\n", cc_mask());

    if (!rd(kDeviceId, g_device_id)) {
        K2LOG("[FUSB302] ERROR: Failed to read DEVICE_ID\n");
        return false;
    }
    K2LOG("[FUSB302] Detected ID: 0x%02X (version 0x%X, rev 0x%X)\n", g_device_id,
                (g_device_id >> 4) & 0xF, g_device_id & 0xF);

    wr(kReset, g_pin_rd != 0 ? kPdReset : kSwReset);
    wr(kSwitches0, g_pin_rd);
    delay_ms(10);
    g_active_cc = 0;
    configure_passive();
    flush_fifos();
    detect_cc_orientation();
    exti_init();
    g_irq = true;
    return true;
}

void park() { wr(kReset, kSwReset); }

void legacy_only(bool on) {
    if (on == g_legacy_only) return;
    g_legacy_only = on;
    if (!on) return;
    K2LOG("[PD] Legacy-only: dropping PD (CC open), sink policy off\n");
    g_prep.abort();
    sink().detach();
    if (!inline_now()) {
        pins::cc1_switch.clear();
        pins::cc2_switch.clear();
        delay_ms(300);
        cc_switches(false, cc_mask());
    }
    g_detach.reset();
    g_engine.reset_source();
    g_new_source = true;
    g_pd_traffic = false;
    g_last_rx_ms = systick::millis();
}

std::uint32_t pd_quiet_ms() { return std::uint32_t(systick::millis() - g_last_rx_ms); }

void rescan() {
    if (g_legacy_only) return;
    if (downstream_now()) {
        K2LOG("[PD] Rescan (inline): passive, waiting for the next Source_Capabilities\n");
        g_new_source = true;
        return;
    }
    K2LOG("[PD] Rescan requested: Cycling CC lines\n");
    for (const pd::Op& op : pd::rescan_plan(false)) {
        switch (op.kind) {
            case pd::OpKind::CcSwitches:
                if (op.value) { cc_switches(false, cc_mask()); }
                else { pins::cc1_switch.clear(); pins::cc2_switch.clear(); }
                break;
            case pd::OpKind::Delay: delay_ms(op.value); break;
        }
    }
    g_detach.reset();
    g_new_source = true;
}

bool cc_open(std::uint32_t ms) {
    if (downstream_now() || ms < 1 || ms > 1000) return false;
    K2LOG("[PD] CC open %u ms\n", static_cast<unsigned>(ms));
    pins::cc1_switch.clear();
    pins::cc2_switch.clear();
    delay_ms(ms);
    cc_switches(false, cc_mask());
    g_detach.reset();
    g_new_source = true;
    return true;
}

bool read_emarker(bool confirmed, pd::EmarkerMode mode) { return read_emarker_impl(confirmed, mode, nullptr, nullptr); }

bool cable_session(bool confirmed, CableBody body, void* ctx) {
    if (body == nullptr) return false;
    return read_emarker_impl(confirmed, pd::EmarkerMode::Vconn, body, ctx);
}

bool cable_exchange(const emk_learn::Request& req, emk_learn::Reply& rep, std::uint32_t timeout_ms) {
    return cable_exchange_impl(req, rep, timeout_ms);
}

void debug_cc_matrix(uint8_t v, uint16_t modes) {
    pins::cc_vconn_gnd.mode(reg::Mode((modes >> 0) & 0xF));
    pins::cc1_switch.mode(reg::Mode((modes >> 4) & 0xF));
    pins::cc2_switch.mode(reg::Mode((modes >> 8) & 0xF));
    pins::cc_vconn_gnd.write(emkra::pc13_high(v & 1, g_ra));
    pins::cc1_switch.write((v >> 1) & 1);
    pins::cc2_switch.write((v >> 2) & 1);
}

ccprobe::Refusal cc_probe(const ccprobe::Args &a, VbusFn vbus_mv, ccprobe::Result &out) {
    out = {};
    std::uint8_t sw0 = 0, sw1 = 0, st0 = 0;
    if (!rd(kSwitches0, sw0) || !rd(kSwitches1, sw1) || !rd(kStatus0, st0)) return ccprobe::Refusal::Bus;

    ccprobe::Ctx c;
    c.attached = g_active_cc != 0 || g_inline;
    const pd::SinkState ss = sink().status().state;
    c.contract = ss != pd::SinkState::Idle && ss != pd::SinkState::Failed;
    c.rd = g_rd_op || g_rd_start || g_ra || g_pin_rd != 0 || (sw0 & (kPdwn1 | kPdwn2)) != 0;
    c.engine_running = ident_running() || probe_running() || reread_running() || g_prep.active();
    c.vconn_on = g_vconn_applied || g_vconn.state() != pd::VconnState::Off;
    c.vbus_mv = vbus_mv ? vbus_mv() : -1;
    ccprobe::Refusal why = ccprobe::refusal(c);
    if (why == ccprobe::Refusal::None && (st0 & ccprobe::kStatus0VbusOk)) why = ccprobe::Refusal::VbusHigh;
    if (why != ccprobe::Refusal::None) return why;

    out.sw0 = sw0; out.sw1 = sw1;
    out.pc = probe_levels();
    out.modes = probe_modes();
    const ccprobe::Plan plan = ccprobe::plan(a.ms);
    if (a.modes != ccprobe::kModesKeep) probe_set_modes(a.modes);
    probe_set_levels(a.pc);
    wr(kSwitches0, a.sw0);
    const std::uint32_t t0 = systick::micros();

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
            if (due - el > 1500) delay_ms(1); else systick::delay_us(50);
        }
        take(out.s[n]);
    }
    out.n = n;
    if (vbus_up) out.flags |= ccprobe::kFlagAborted;

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

loopprobe::CcRefusal loop_cc_hold(std::span<const std::uint8_t> plan, std::uint16_t hold_us, VbusFn vbus_mv,
                                  loopprobe::CcResult &out) {
    using loopprobe::CcRefusal;
    out = {};
    const auto refuse = [&](CcRefusal why) {
        out.refusal = std::uint8_t(why);
        return why;
    };
    if (!loopprobe::cc_plan_ok(plan)) return refuse(CcRefusal::BadPlan);
    std::uint8_t sw0 = 0;
    if (!rd(kSwitches0, sw0)) return refuse(CcRefusal::Bus);
    out.sw0 = sw0;
    out.pc = probe_levels();
    const pd::SinkState ss = sink().status().state;
    if (ss != pd::SinkState::Idle && ss != pd::SinkState::Failed) return refuse(CcRefusal::Contract);
    if (ident_running() || probe_running() || reread_running() || g_prep.active() || g_rd_op || g_rd_start || g_ra)
        return refuse(CcRefusal::Engine);
    if (g_vconn_applied || g_vconn.state() != pd::VconnState::Off || (sw0 & (kVconnCc1 | kVconnCc2)) != 0)
        return refuse(CcRefusal::Vconn);
    const auto vbus = [&] { return vbus_mv ? vbus_mv() : std::int32_t(-1); };
    if (vbus() > loopprobe::kCcVbusLimitMv) return refuse(CcRefusal::VbusHigh);

    std::uint8_t n = 0;
    for (const std::uint8_t cfg : plan) {
        const std::uint8_t v = loopprobe::cc_sw0(sw0, cfg);
        wr(kSwitches0, v);
        systick::delay_us(hold_us);
        loopprobe::CcRow &row = out.rows[n++];
        row.cfg = cfg;
        std::uint8_t back = 0xFF;
        if (!rd(kSwitches0, back) || back != v) out.flags |= loopprobe::kCcDisturbed;
        std::uint16_t r1 = 0xFFFF, r2 = 0xFFFF;
        adc::latest_cc(r1, r2);
        row.cc1_raw = r1;
        row.cc2_raw = r2;
        std::uint8_t a1 = 0, a2 = 0;
        const bool ok1 = wr(kSwitches0, ccprobe::measure_sw0(v, 1));
        systick::delay_us(loopprobe::kCcMeasureUs);
        const bool ok2 = ok1 && rd(kStatus0, a1) && wr(kSwitches0, ccprobe::measure_sw0(v, 2));
        systick::delay_us(loopprobe::kCcMeasureUs);
        const bool ok = ok2 && rd(kStatus0, a2);
        row.bc = std::uint8_t((a1 & 3u) | (a2 & 3u) << 2 | (ok ? 0x10u : 0u));
        const std::int32_t mv = vbus();
        row.vbus_mv = std::uint16_t(mv < 0 ? 0xFFFF : (mv > 0xFFFE ? 0xFFFE : mv));
        wr(kSwitches0, sw0);
        if (mv > loopprobe::kCcVbusLimitMv) {
            out.flags |= loopprobe::kCcAborted;
            break;
        }
        service();
    }
    out.n = n;

    for (int attempt = 0; attempt < 2; attempt++) {
        wr(kSwitches0, sw0);
        std::uint8_t b0 = 0xFF;
        if (rd(kSwitches0, b0) && b0 == sw0 && probe_levels() == out.pc) {
            out.flags |= loopprobe::kCcRestored;
            break;
        }
    }
    return CcRefusal::None;
}

raprobe::Refusal ra_probe(const raprobe::Args &a, VbusFn vbus_mv, raprobe::Result &out) {
    out = {};
    const auto refuse = [&](raprobe::Refusal why) {
        out.refusal = std::uint8_t(why);
        return why;
    };
    std::uint8_t sw0 = 0, sw1 = 0, st0 = 0;
    if (!rd(kSwitches0, sw0) || !rd(kSwitches1, sw1) || !rd(kStatus0, st0)) return refuse(raprobe::Refusal::Bus);

    raprobe::Ctx c;
    c.inline_sink = g_inline;
    const pd::SinkState ss = sink().status().state;
    c.contract = ss != pd::SinkState::Idle && ss != pd::SinkState::Failed;
    c.rd = g_rd_op || g_rd_start || g_pin_rd != 0 || (sw0 & (kPdwn1 | kPdwn2)) != 0;
    c.ra_mode = g_ra;
    c.engine_running = ident_running() || probe_running() || reread_running() || g_prep.active();
    c.vconn_on = g_vconn_applied || g_vconn.state() != pd::VconnState::Off;
    c.vbus_flag = (st0 & raprobe::kStatus0VbusOk) != 0;
    c.vbus_mv = vbus_mv ? vbus_mv() : -1;
    if (const raprobe::Refusal why = raprobe::refusal(c); why != raprobe::Refusal::None) return refuse(why);

    out.sw0 = sw0;
    out.sw1 = sw1;
    out.pc = probe_levels();
    out.vconn_pc = std::uint8_t(a.vconn_pc & 6u);
    const auto note_vbus = [&](std::int32_t v) {
        if (v < 0) return;
        const std::uint16_t u = std::uint16_t(v > 0xFFFE ? 0xFFFE : v);
        if (out.vbus_max_mv == raprobe::kNoRaw || u > out.vbus_max_mv) out.vbus_max_mv = u;
    };
    note_vbus(c.vbus_mv);

    const auto release = [&] {
        wr(kSwitches0, sw0);
        probe_set_levels(out.pc);
    };
    const auto wait = [&](std::uint32_t t0, std::uint32_t ms) {
        const std::uint32_t due = ms * 1000u;
        std::uint32_t checked = 0;
        for (;;) {
            const std::uint32_t el = systick::micros() - t0;
            if (el >= due) return true;
            service();
            std::uint8_t s = 0;
            bool up = rd(kStatus0, s) && raprobe::vbus_abort(-1, s);
            if (!up && vbus_mv && el - checked >= 5000) {
                checked = el;
                const std::int32_t v = vbus_mv();
                note_vbus(v);
                up = raprobe::vbus_abort(v, 0);
            }
            if (up) {
                release();
                out.flags |= raprobe::kFlagAborted;
                return false;
            }
            if (due - el > 1500) delay_ms(1); else systick::delay_us(50);
        }
    };
    const auto source_on_line = [](std::uint16_t r1, std::uint16_t r2) {
        return r1 == raprobe::kNoRaw || r2 == raprobe::kNoRaw || raprobe::source_level(raprobe::raw_to_mv(r1), raprobe::raw_to_mv(r2));
    };
    const auto run_phase = [&](std::uint8_t phase) {
        if (phase != raprobe::kPhaseIdle) {
            probe_set_levels(raprobe::phase_pc(phase, out.pc, out.vconn_pc));
            wr(kSwitches0, raprobe::phase_sw0(phase, sw0));
        }
        const std::uint32_t t0 = systick::micros();
        for (std::uint8_t i = 0; i < raprobe::kSamplesPerPhase; i++) {
            if (!wait(t0, raprobe::kSampleMs[i])) return false;
            raprobe::Sample &s = out.s[raprobe::sample_index(phase, i)];
            std::uint16_t r1 = 0, r2 = 0;
            if (adc::latest_cc(r1, r2)) { s.cc1_raw = r1; s.cc2_raw = r2; }
            out.n++;
        }
        return true;
    };

    if (!run_phase(raprobe::kPhaseIdle)) return refuse(raprobe::Refusal::VbusHigh);
    const raprobe::Sample &idle = out.s[raprobe::sample_index(raprobe::kPhaseIdle, raprobe::kSamplesPerPhase - 1)];
    if (source_on_line(idle.cc1_raw, idle.cc2_raw)) return refuse(raprobe::Refusal::SourceLevel);

    bool ok = run_phase(raprobe::kPhaseVconn1);
    if (ok) {
        release();
        ok = wait(systick::micros(), raprobe::kSettleMs);
        std::uint16_t r1 = 0, r2 = 0;
        if (ok && (!adc::latest_cc(r1, r2) || source_on_line(r1, r2))) {
            out.flags |= raprobe::kFlagAborted;
            ok = false;
        }
    }
    if (ok) ok = run_phase(raprobe::kPhaseVconn2);
    if (ok) release();

    for (int attempt = 0; attempt < 2; attempt++) {
        release();
        std::uint8_t b0 = 0xFF, b1 = 0xFF;
        if (rd(kSwitches0, b0) && rd(kSwitches1, b1) && b0 == sw0 && b1 == sw1 && probe_levels() == out.pc) {
            out.flags |= raprobe::kFlagRestored;
            break;
        }
    }
    return raprobe::Refusal::None;
}

pd::Err refresh_source_caps() {
    if (g_active_cc == 0 || inline_now() || g_legacy_only) return pd::Err::Busy;
    if (ident_running() || probe_running() || reread_running() || g_prep.active()) return pd::Err::Busy;
    return sink().request_source_caps(systick::millis());
}

pd::Err request_pps_status() {
    if (g_active_cc == 0 || inline_now() || g_legacy_only) return pd::Err::Busy;
    if (ident_running() || probe_running() || reread_running() || g_prep.active()) return pd::Err::Busy;
    return sink().request_pps_status(systick::millis());
}

bool sink_soft_reset() {
    if (g_active_cc == 0 || inline_now() || g_legacy_only) return false;
    if (ident_running() || probe_running() || reread_running() || g_prep.active()) return false;
    if (sink().status().state != pd::SinkState::Ready) return false;
    sink().soft_reset(systick::millis());
    trace_sink();
    K2LOG("[PD] Soft_Reset sent on a live contract\n");
    return true;
}

bool get_source_caps(bool confirmed) {
    if (g_active_cc == 0 || inline_now()) {
        K2LOG("[PD] Get_Source_Cap refused (no CC / inline: an inline K2 never transmits)\n");
        return false;
    }
    (void)confirmed;
    g_getcap_armed = true;
    g_getcap_at = systick::millis();
    g_getcap_force = true;
    return true;
}

bool identify_start(bool confirmed) {
    if (inline_now()) {
        K2LOG("[ID] Identify refused (inline: an inline K2 never transmits)\n");
        return false;
    }
    (void)confirmed;
    const bool alone = pd::identify_alone(false, g_vbus_mv >= 0, g_vbus_mv, g_session.fresh(), g_wait_kind);
    if (g_active_cc == 0) {
        if (!alone) {
            K2LOG("[ID] Identify refused (no CC: nothing to ask)\n");
            return false;
        }
        if (!set_rd_for_run(true, false)) {
            K2LOG("[ID] Identify refused (Rd refused: a sink is inline)\n");
            return false;
        }
        g_ident_owns_rd = true;
        K2LOG("[ID] alone: Rd presented, waiting for a Ready contract\n");
    } else if (alone) {
        const pd::Err e = request_inner({1, 0, 0}, false);
        g_reread_made_contract = false;
        K2LOG("[ID] alone: CC%u held, Request PDO 1 err=%u\n", unsigned(g_active_cc), unsigned(e));
    }
    if (alone) {
        g_ident_wait = true;
        g_ident_alone = true;
        g_ident_wait_at = systick::millis();
        g_ident_ready_at = 0;
        g_ident_result_valid = false;
        g_ident_alone_hold = false;
        return true;
    }
    const pd::SinkState ss = sink().status().state;
    if (ss != pd::SinkState::Idle && ss != pd::SinkState::Ready && ss != pd::SinkState::Failed) return false;
    g_ident_session = g_session.id();
    g_ident_result_valid = true;
    g_ident_alone_hold = false;
    ident().start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, systick::millis());
    g_ident_was_running = true;
    K2LOG("[ID] Identify started\n");
    return true;
}

bool probe_start() {
    if (g_active_cc == 0 || inline_now() || ident_running() || g_prep.active()) return false;
    const pd::SinkState ss = sink().status().state;
    if (ss != pd::SinkState::Idle && ss != pd::SinkState::Ready && ss != pd::SinkState::Failed) return false;
    g_probe_log_pending = true;
    probe().start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, systick::millis());
    K2LOG("[PROBE] started\n");
    return true;
}

bool reread_start(std::uint8_t mask) {
    if (inline_now() || g_legacy_only || ident_running() || probe_running() || reread_running() || g_prep.active()) return false;
    const pd::SinkState ss = sink().status().state;
    const bool have_contract = ss == pd::SinkState::Ready;
    if (!have_contract && ss != pd::SinkState::Idle && ss != pd::SinkState::Failed) return false;
    if (have_contract && g_active_cc == 0) return false;
    if (g_force_cc != 0 && g_active_cc != g_force_cc) {
        if (have_contract) return false;
        if (g_active_cc != 0) detach_reset(0);
        K2LOG("[REREAD] forced line CC%u\n", unsigned(g_force_cc));
    }
    const unsigned held = g_engine.caps.valid ? g_engine.caps.num_pdos : 0;
    g_reread_log_pending = true;
    reread().start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, mask, std::uint8_t(held), systick::millis(), true);
    pdtrace::mark(pdtrace::kMarkReread, reread().status().mask, std::uint16_t(held));
    K2LOG("[REREAD] started mask=0x%02X with %u PDOs held, contract %d\n", unsigned(reread().status().mask), held, int(have_contract));
    if (!have_contract) {
        if (g_active_cc == 0) {
            if (!g_rd_start && set_rd_for_run(true, false)) g_reread_owns_rd = g_reread_made_contract = true;
            K2LOG("[REREAD] attach: no CC yet, Rd %s\n", g_rd_start ? "presented" : "refused");
        } else {
            const pd::Err e = request_inner({1, 0, 0}, false);
            g_reread_made_contract = e == pd::Err::Ok;
            K2LOG("[REREAD] attach: Request PDO 1 err=%u\n", unsigned(e));
        }
    }
    return true;
}

bool survey_wire_start(std::uint8_t mask) {
    if (inline_now() || g_legacy_only || g_active_cc == 0 || ident_running() || probe_running() || reread_running() ||
        g_prep.active() || sink().status().state != pd::SinkState::Ready || g_reread_log_pending)
        return false;
    const unsigned held = g_engine.caps.valid ? g_engine.caps.num_pdos : 0;
    g_reread_log_pending = true;
    pd::RereadOpts opts;
    opts.survey_order = true;
    opts.pace_ms = pd::kSurveyPaceMs;
    opts.total_ms = pd::kSurveyWireMs;
    reread().start(g_engine.caps.valid ? g_engine.caps.spec_rev : 2, mask, std::uint8_t(held), systick::millis(), false, opts);
    pdtrace::mark(pdtrace::kMarkReread, reread().status().mask, std::uint16_t(held));
    K2LOG("[SURVEY] wire started mask=0x%02X, %u PDOs held\n", unsigned(reread().status().mask), held);
    return true;
}

void set_force_line(std::uint8_t line) {
    g_force_cc = line <= 2 ? line : 0;
    K2LOG("[PD] CC scan forced line %u\n", unsigned(g_force_cc));
}

void set_idle_wake(bool on) {
    const bool holding = g_idle_wake.line() != 0;
    if (on == g_idle_wake.enabled()) return;
    g_idle_wake.set_enabled(on);
    K2LOG("[FUSB302] Wake sleeping port %s\n", on ? "ON" : "OFF");
    if (on) return;
    if (holding && g_active_cc == 0) wr(kSwitches0, idle_rd());
    else if (g_wake_attach && g_active_cc != 0 && idle_owner_free()) {
        K2LOG("[FUSB302] Detach: the wake's attach on CC%u ends with the setting\n", unsigned(g_active_cc));
        detach_reset(0);
    }
}

bool ra_set(bool on) {
    if (!on && g_ra_early && !g_ra) {
        g_ra_early = false;
        pins::cc_vconn_gnd.write(g_vconn_applied);
        K2LOG("[EMKRA] early off\n");
        return true;
    }
    if (on == g_ra) return true;
    if (!on) {
        g_ra = false;
        g_ra_run = false;
        pins::cc_vconn_gnd.write(g_vconn_applied);
        if (g_active_cc == 0) wr(kSwitches0, idle_rd()); else apply_rd();
        K2LOG("[EMKRA] off\n");
        return true;
    }
    const pd::SinkState ss = sink().status().state;
    emkra::Ctx c;
    c.inline_sink = inline_now();
    c.contract = ss != pd::SinkState::Idle && ss != pd::SinkState::Failed;
    c.self_hold = pd::self_hold_wanted(g_pin_rd, downstream_now(), g_legacy_only) || reread_own_hold() ||
                  pd::auto_own_hold(sink().status());
    c.busy = ident_running() || probe_running() || reread_running() || g_prep.active();
    c.pinned_rd = g_pin_rd;
    c.vconn = g_vconn_applied || g_vconn.state() != pd::VconnState::Off;
    const emkra::Refuse why = emkra::check(c);
    g_ra_refused = why;
    if (why != emkra::Refuse::None) {
        K2LOG("[EMKRA] refused: %s\n", emkra::refuse_name(why));
        return false;
    }
    if (g_active_cc != 0 && g_pin_rd == 0) detach_reset(0);
    g_ra = true;
    g_ra_early = false;
    g_force_cc = emkra::kLine;
    cc_switches(false, cc_mask());
    if (g_active_cc == 0) wr(kSwitches0, idle_rd());
    K2LOG("[EMKRA] on: PC13 high (Ra on CC1), Rd on CC2 only, attach scan forced to CC%u\n", unsigned(g_force_cc));
    return true;
}

bool cc_detach() {
    if (inline_now() || g_pin_rd != 0) return false;
    g_reread_owns_rd = false; g_ident_owns_rd = false;
    g_reread_made_contract = false;
    reread().abort();
    g_force_cc = 0;
    g_rd_op = false;
    g_rd_gate.reset();
    ra_set(false);
    set_rd_for_run(false, false);
    detach_reset(0);
    g_rd_hold.start(systick::millis());
    K2LOG("[PD] CC detach: Rd off for %u ms\n", unsigned(pd::kRdHoldMs));
    return true;
}

bool reread_abort() {
    if (inline_now() || g_run_owner) return false;
    g_force_cc = 0;
    ra_set(false);
    reread().abort();
    const std::uint32_t now = systick::millis();
    if (g_reread_owns_rd || g_reread_made_contract) {
        g_reread_release_at = now;
    } else if (g_rd_start && g_pin_rd == 0 && !ident_running() && !probe_running()) {
        K2LOG("[REREAD] abort: lab-held attach adopted, Rd and contract released\n");
        g_reread_owns_rd = true;
        g_reread_made_contract = sink().status().state != pd::SinkState::Idle;
        g_reread_release_at = now;
    }
    return true;
}

pd::IqRefusal interrogate_start(const pd::IqFacts& outside) {
    pd::IqFacts f = outside;
    f.cc = g_active_cc != 0;
    f.inline_link = inline_now();
    f.busy = f.busy || ident_running() || g_ident_wait || probe_running() || reread_running() || g_prep.active() ||
             g_vconn.state() != pd::VconnState::Off || g_getcap_armed || g_sop_prime_hook != nullptr;
    const pd::SinkStatus& ss = sink().status();
    f.own_contract = f.own_contract || ss.held || (ss.state != pd::SinkState::Idle && ss.state != pd::SinkState::Failed);
    f.self_powered = f.self_powered || self_powered();
    const std::uint32_t now = systick::millis();
    const pd::IqRefusal r = iq().start(f, now);
    if (r == pd::IqRefusal::None) {
        g_iq_phase = pd::IqPhase::Idle;
        g_iq_closed = 0;
        K2LOG("[IQ] t=%u started (contract sniffed: %u, source id %u)\n", unsigned(now), unsigned(iq().watch().contract().ready),
              unsigned(iq().watch().next_id()));
    } else {
        K2LOG("[IQ] refused: %s\n", pd::iq_refusal_name(r));
    }
    return r;
}

void interrogate_abort() {
    if (iq_running()) iq().abort(pd::IqWhy::Stopped, systick::millis());
}

bool vconn_request() {
    if (g_active_cc == 0 || inline_now() || g_legacy_only || sink().status().state != pd::SinkState::Ready || !emkra::vconn_switch_allowed(g_ra)) {
        K2LOG("[VCONN] request refused: cc=%u inline=%d sink state %u\n", unsigned(g_active_cc), int(inline_now()), unsigned(sink().status().state));
        return false;
    }
    const bool ok = g_vconn.request(systick::millis());
    K2LOG("[VCONN] request %s\n", ok ? "queued" : "refused: a swap is running");
    if (ok) vconn_mark();
    return ok;
}

bool vconn_roundtrip() {
    if (g_active_cc == 0 || inline_now() || g_legacy_only || sink().status().state != pd::SinkState::Ready) {
        K2LOG("[VCONN] round trip refused: cc=%u inline=%d sink state %u\n", unsigned(g_active_cc), int(inline_now()), unsigned(sink().status().state));
        return false;
    }
    const bool ok = g_vconn.request(systick::millis(), true);
    g_vconn_give = ok;
    K2LOG("[VCONN] round trip %s (ra=%d)\n", ok ? "queued" : "refused: a swap is running", int(g_ra));
    if (ok) vconn_mark();
    return ok;
}

bool data_reset() {
    if (g_active_cc == 0 || inline_now() || sink().status().state != pd::SinkState::Ready) return false;
    const bool ok = g_vconn.send_raw(14);
    K2LOG("[VCONN] Data_Reset %s\n", ok ? "queued" : "refused: TX slot busy");
    return ok;
}

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
    if (!g_tx_allowed) {
        K2LOG("[FUSB302] SOP' TX refused (passive)\n");
        return false;
    }
    std::uint16_t header = 0;
    std::span<const std::uint8_t> objs;
    if (tokens.size() >= 10) {
        header = std::uint16_t(tokens[5] | (tokens[6] << 8));
        objs = tokens.subspan(7, tokens.size() - 10);
    }
    if (!fifo_write_tx(header, objs, tokens)) return false;
    if (!wr(kControl0, kTxStart)) return false;
    if (inline_now()) iq().on_sopp_reply(systick::millis());
    return true;
}

}  // namespace k2::fusb302
