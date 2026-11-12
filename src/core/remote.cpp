// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remote-control adapter: runs protocol commands (remote_proto.hpp) against the real UI,
// FUSB302 and power monitor. Everything here runs in the main loop; the USB callback only queues.
#include "k2/core/remote.hpp"
#include "k2/core/remote_keys.hpp"
#include "k2/ui/uiprefs.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include "k2/core/log.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/loop_stats.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/pd/attach_pick.hpp"
#include "k2/pd/cable_r.hpp"
#include "k2/fastcharge/dpsniff.hpp"
#include "k2/fastcharge/edges.hpp"
#include "k2/drivers/load.hpp"
#include "k2/pd/emk_learn_hw.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/core/factory_wipe.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/drivers/flash.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/pin_mark.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/core/bootinfo_hw.hpp"
#include "k2/core/boottrace_store.hpp"
#include "k2/core/handback.hpp"
#include "k2/fastcharge/fctrig.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/pd/pd_passive.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/chargertest/report_card_remote.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/core/storage.hpp"
#include "k2/drivers/sysreset.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/twin/twin_hw.hpp"
#include "k2/twin/wire.hpp"
#include "k2/ui/ui_qrtest.hpp"
#include "k2/loopprobe/loop_probe_hw.hpp"
#include "k2/core/version.hpp"
#include "k2/pd/diag_live.hpp"
#include "k2/pd/diag_wire.hpp"

namespace k2 {

namespace {

Remote *g_instance = nullptr;

constexpr const char *kButtonNames[4] = {"BACK", "-", "OK", "+"};   // Button order

static_assert(pd::kMaxPdos == remote::kMaxPdos, "PDO table size must match the protocol");
static_assert(std::tuple_size_v<decltype(pd::Emarker::raw_vdos)> == remote::kMaxVdos,
              "VDO table size must match the protocol");

constexpr uint32_t kStreamTimeoutMs = 500;
constexpr unsigned kStreamMinFree = 3;

std::string_view sv(const char *s) { return s ? std::string_view(s) : std::string_view(); }

}  // namespace

Remote::Remote(Ui &ui) : ui_(ui) { g_instance = this; }

void Remote::enqueue(const uint8_t *data, uint16_t len) {
    const unsigned next = (head_ + 1) % kQueueDepth;
    if (next == tail_) {
        dropped_++;
        return;
    }
    Slot &s = queue_[head_];
    s.len = len > remote::kFrameSize ? remote::kFrameSize : len;
    for (uint16_t i = 0; i < s.len; i++) s.data[i] = data[i];
    head_ = next;
}

RemoteEvent Remote::poll() {
    lease_tick();
    if (dropped_ != dropped_reported_) {
        dropped_reported_ = dropped_;
        K2LOG("[RMT] request queue overflow, dropped=%d\n", static_cast<int>(dropped_));
    }
    while (tail_ != head_) {
        const Slot &s = queue_[tail_];
        std::array<uint8_t, remote::kFrameSize> out{};
        const bool answer = remote::handle_request(std::span<const uint8_t>(s.data, s.len), *this, out);
        if (answer) {
            if (out[4] != 0) {
                K2LOG("[RMT] cmd=%d seq=%d status=%d\n", static_cast<int>(out[2]),
                            static_cast<int>(out[3]), static_cast<int>(out[4]));
            }
            usb::send_report(out.data());
        }
        const uint8_t seq = s.data[3];
        if (ripple_pending_) {
            ripple_pending_ = false;
            stream_ripple(seq);
        }
        tail_ = (tail_ + 1) % kQueueDepth;
        if (shot_pending_) {
            shot_pending_ = false;
            shot_seq_ = seq;
            return RemoteEvent::Screenshot;      // the caller streams the strips after the response
        }
        if (fault_pending_) {
            fault_pending_ = false;
            return RemoteEvent::Fault;
        }
        if (dfu_pending_) {
            dfu_pending_ = false;
            return RemoteEvent::Dfu;             // the caller runs dfu::enter() after the response
        }
        if (reboot_pending_) {
            reboot_pending_ = false;
            return RemoteEvent::Reboot;
        }
    }
    return RemoteEvent::None;
}

bool Remote::button_down(Button b, uint32_t now_ms) {
    return virt_[static_cast<unsigned>(b)].down(now_ms);
}

remote::PingInfo Remote::ping() {
    return {systick::millis(), sv(kGitHash), sv(kFwVersion)};
}

remote::Status Remote::press(uint8_t button, uint16_t ms) {
    if (!virt_[button].press(systick::millis(), ms)) return remote::Status::Busy;
    K2LOG("[RMT] button=%s down for %dms\n", kButtonNames[button], static_cast<int>(ms));
    return remote::Status::Ok;
}

remote::ChargerTestSnapshot Remote::charger_test_results() const {
    return remote::charger_test_snapshot(ui_.charger_test_results(), ui_.charger_test_selection(), ui_.charger_test_stage(),
                                                ui_.charger_test_cursor());
}

static uint16_t sat16(uint32_t v) { return v > 0xFFFFu ? uint16_t(0xFFFF) : static_cast<uint16_t>(v); }

remote::ChargerTestView Remote::charger_test_view() {
    const auto v = ui_.charger_test_view();
    remote::ChargerTestView o;
    o.stage = v.stage; o.cursor = v.cursor; o.rows = v.rows;
    const auto flag = [&o](bool on, uint16_t bit) { if (on) o.flags = static_cast<uint16_t>(o.flags | bit); };
    flag(v.on_screen, remote::kCtViewOnScreen);
    flag(v.confirming, remote::kCtViewConfirming);
    flag(v.confirm_reattach, remote::kCtViewConfirmReattach);
    flag(v.self_supplied, remote::kCtViewSelfSupplied);
    flag(v.resume_pending, remote::kCtViewResumePending);
    flag(v.auto_resume, remote::kCtViewAutoResume);
    flag(v.gate_ready, remote::kCtViewGateReady);
    flag(v.resume_held, remote::kCtViewResumeHeld);
    flag(v.behind_cable, remote::kCtViewBehindCable);
    flag(v.resume_rd, remote::kCtViewResumeRd);
    flag(v.wait_paused, remote::kCtViewWaitPaused);
    flag(v.wait_retry, remote::kCtViewWaitRetry);
    flag(v.armed_is_pick, remote::kCtViewArmedIsPick);
    flag(v.rmdo_valid, remote::kCtViewRmdoValid);
    flag(v.cable_armed, remote::kCtViewCableArmed);
    o.confirm_left_ms = sat16(v.confirm_left_ms);
    o.wait_elapsed_ms = sat16(v.wait_elapsed_ms);
    o.wait_total_ms = sat16(v.wait_total_ms);
    o.run_elapsed_s = sat16(v.run_elapsed_s);
    o.card_no = v.card_no; o.delta_n = v.delta_n; o.delta_runs = v.delta_runs;
    o.runnable = v.preview.runnable; o.skipped = v.preview.skipped;
    o.refusal = static_cast<uint8_t>(v.preview.refusal); o.refusal_mv = v.preview.refusal_mv;
    o.pps_points = sat16(v.pps_points); o.avs_points = sat16(v.avs_points);
    o.topology = v.topology; o.run_guard = v.run_guard; o.rmdo = v.rmdo;
    const size_t n = v.message_len < o.message.size() ? v.message_len : o.message.size();
    for (size_t i = 0; i < n; ++i) o.message[i] = v.message[i];
    o.message_len = static_cast<uint8_t>(n);
    return o;
}

remote::Status Remote::charger_units(std::span<const uint8_t> units) {
    if (ui_.charger_test_busy()) return remote::Status::Busy;
    K2LOG("[TEST] units set n=%u\n", unsigned(units.size()));
    return ui_.charger_test_set_units(units) ? remote::Status::Ok : remote::Status::BadArg;
}

remote::Status Remote::charger_sweep(bool on) {
    if (ui_.charger_test_busy()) return remote::Status::Busy;
    K2LOG("[TEST] deep sweep %s\n", on ? "on" : "off");
    ui_.charger_test_set_sweep(on);
    return remote::Status::Ok;
}

remote::Status Remote::charger_avs_sweep(bool on) {
    if (ui_.charger_test_busy()) return remote::Status::Busy;
    K2LOG("[TEST] avs deep sweep %s\n", on ? "on" : "off");
    ui_.charger_test_set_avs_sweep(on);
    return remote::Status::Ok;
}

remote::ChargerCableReply Remote::charger_cable(const ctcable::Choice* set) {
    remote::ChargerCableReply r;
    if (set) {
        if (ui_.charger_test_busy()) { r.status = remote::Status::Busy; return r; }
        if (!ui_.charger_test_set_cable(*set)) { r.status = remote::Status::BadArg; return r; }
        K2LOG("[TEST] cable choice set kind=%u seq=%u\n", unsigned(set->kind), unsigned(set->seq));
    }
    r.info = ui_.charger_test_cable_info();
    return r;
}

remote::ChargerHuntReply Remote::charger_hunt(uint8_t slot) const {
    const auto& runner = ui_.charger_test_results();
    const auto held = runner.hunt_evidence();
    remote::ChargerHuntReply r;
    r.run_id = runner.run_id();
    r.count = static_cast<uint8_t>(held.size());
    if (slot < held.size()) r.slot = held[slot];
    return r;
}

remote::StateInfo Remote::state() {
    remote::StateInfo s;
    const auto pm = power::read();
    const bool online = pm.has_value();
    const pd::SourceCaps *caps = &pd::active_caps();
    s.page = ui_.page_index();
    s.active_cc = fusb302::active_cc();
    s.spec_rev = caps->spec_rev;
    s.num_pdos = caps->num_pdos;
    s.pd_valid = caps->valid;
    s.chip = static_cast<uint8_t>(power::chip());
    s.pm_online = online;
    s.log_on = usb::log_enabled();
    if (online) {
        s.vbus_mv = pm->vbus_mv;
        s.current_ma = pm->current_ma;
        s.power_mw = pm->power_mw;
    }
    s.energy_mwh = ui_.energy_mwh();
    if (const auto a = adc::read()) {
        s.dp_mv = static_cast<uint16_t>(a->dp_mv);
        s.dm_mv = static_cast<uint16_t>(a->dm_mv);
        s.cc1_mv = static_cast<uint16_t>(a->cc1_mv);
        s.cc2_mv = static_cast<uint16_t>(a->cc2_mv);
        s.aux_mv = static_cast<uint16_t>(a->aux_mv);
        s.temp_deci_c = static_cast<int16_t>(a->temp.deci_c);
        s.mcu_temp_deci_c = static_cast<int16_t>(a->mcu.deci_c);
        s.adc_flags = 1u | (a->temp.valid ? 2u : 0u) | (a->temp.calibrated ? 4u : 0u) | (a->mcu.valid ? 8u : 0u);
    }
    s.fc = static_cast<uint8_t>(fastcharge::label());   // SVOOC while a proven session is live, like Home
    const pd::Link &link = ui_.link();
    const uint32_t now = systick::millis();
    s.link_flags = (link.charger_stale() ? remote::kLinkStale : 0) | (link.waiting() || link.charger_waiting() ? remote::kLinkWaiting : 0) |
                   (link.inline_mode() ? remote::kLinkInline : 0) | (link.cable_stale() ? remote::kLinkCableStale : 0) |
                   (link.cable_scanned() ? remote::kLinkCableScanned : 0) |
                   (link.cc_cross() == raguard::CrossKind::Crossed ? remote::kLinkCrossed : 0) |
                   (link.cc_cross() == raguard::CrossKind::Hint ? remote::kLinkFlipHint : 0) |
                   (link.pd_verdict() == pd::PdVerdict::None ? remote::kLinkNoPd : 0);   // the settled no-PD verdict (Row C)
    s.age_s = link.attached() ? 0 : link.detached_for_s(now);
    s.cable_age_s = link.cable_scanned() ? link.cable_age_s(now) : 0;
    s.reset_flags = sysreset::reset_flags() & 0xFC000000u;   // CTRLSTS reset-cause bits 26..31 only
    return s;
}

// Ui::session() field for field; the page-dependent derivations live in the Ui, next to the screens that show them.
remote::SessionInfo Remote::session() {
    const Ui::Session u = ui_.session();
    remote::SessionInfo s;
    s.mah = u.mah;
    s.elapsed_s = u.elapsed_s;
    s.proto_kind = u.proto_kind;
    s.limit_ma = u.limit_ma; s.max_mv = u.max_mv; s.typec_ma = u.typec_ma;
    s.busy = static_cast<uint8_t>(u.busy);
    s.fc_release = u.fc_release;
    s.topology = u.topology; s.wait_kind = u.wait_kind; s.pd_none_why = u.pd_none_why;
    s.open_cc_mv = u.open_cc_mv;
    s.depth = u.depth;
    for (size_t i = 0; i < s.stack.size(); i++) s.stack[i] = u.stack[i];
    s.sub = u.sub; s.sel = u.sel; s.count = u.count; s.home_window = u.home_window; s.flags = u.flags;
    return s;
}

// The Diag page rows as numbers: the same driver reads DiagPage::sample() and update() make (usb::stats_snapshot, fusb302).
remote::DiagInfo Remote::diag() {
    const usb::StatsSnapshot u = usb::stats_snapshot();
    remote::DiagInfo d;
    d.fusb_id = fusb302::device_id();
    d.pd_rx = fusb302::rx_count();
    d.usb_resets = u.resets; d.usb_isrs = u.isrs; d.log_sent = u.log_sent; d.log_dropped = u.log_dropped;
    d.poll_gap_max_ms = u.poll_gap_max;
    d.usb_configured = usb::configured() ? 1 : 0;
    static_assert(unsigned(loopguard::Reason::Certain) == remote::kLoopCertain && unsigned(loopguard::Reason::Suspected) == remote::kLoopSuspected);
    d.loop = static_cast<uint8_t>(static_cast<unsigned>(loopguard::reason()) | (usb::host_active() ? remote::kLoopHost : 0));
    d.loop_events = loopguard::events();
    return d;
}

// The strip as the page on screen sets it (Ui::labels(), what draw_slot reads), cut to the wire's 7 character words.
remote::KeysInfo Remote::keys() {
    return remote::keys_from_labels(ui_.labels(), ui_.strip_hidden());
}

remote::PdoSnapshot Remote::pdos() {
    remote::PdoSnapshot snap;
    const pd::SourceCaps *caps = &pd::active_caps();
    snap.valid = caps->valid;
    snap.spec_rev = caps->spec_rev;
    snap.num_pdos = caps->num_pdos > pd::kMaxPdos ? pd::kMaxPdos : caps->num_pdos;
    for (unsigned i = 0; i < snap.num_pdos; i++) {
        const pd::Pdo &p = caps->pdos[i];
        snap.pdo[i] = {static_cast<uint8_t>(p.type), p.raw, p.min_mv, p.max_mv, p.max_ma, p.max_mw};
    }
    return snap;
}

remote::EmarkerSnapshot Remote::emarker() {
    remote::EmarkerSnapshot e;
    const pd::Emarker *m = &fusb302::emarker();
    e.valid = m->valid;
    e.is_active = m->is_active;
    e.vid = m->vid;
    e.pid = m->pid;
    e.xid = m->xid;
    e.hw_rev = m->hw_rev;
    e.fw_rev = m->fw_rev;
    e.max_a = m->max_current_a;
    e.max_v = m->max_voltage_v;
    e.max_w = m->max_power_w;
    e.num_vdos = m->num_vdos > remote::kMaxVdos ? remote::kMaxVdos : m->num_vdos;
    e.vendor = sv(m->vendor_str);
    e.speed = sv(m->speed_str);
    for (unsigned i = 0; i < e.num_vdos; i++) e.vdo[i] = m->raw_vdos[i];
    e.sop = m->sop;
    e.spec_rev = m->spec_rev;
    e.vdo_version = m->vdo_version;
    e.latency_code = m->latency_code;
    e.termination = m->termination;
    e.max_temp_c = m->max_temp_c;
    e.shutdown_temp_c = m->shutdown_temp_c;
    e.u3_power = m->u3_power;
    e.usb_lanes = m->usb_lanes;
    e.flags = (m->type_c_plug ? remote::kEmFlagTypeC : 0) | (m->vconn_required ? remote::kEmFlagVconn : 0) |
              (m->epr_capable ? remote::kEmFlagEpr : 0) | (m->vdo2_valid ? remote::kEmFlagVdo2 : 0) |
              (m->usb4_supported ? remote::kEmFlagUsb4 : 0) | (m->usb_gen2 ? remote::kEmFlagGen2 : 0) |
              (m->optical_isolated ? remote::kEmFlagOptical : 0) |
              (fusb302::cable_thunderbolt() ? remote::kEmFlagThunderbolt : 0);
    const emk_variant::Answered a = fusb302::cable_answered();
    e.answered_orientation = a.orientation;
    e.answered_hdr_rev = a.spec_rev;
    e.answered_svdm = a.svdm;
    e.answered_attempt = a.attempt;
    const emk_attached::ReadInfo ri = fusb302::last_cable_read();
    e.read_mode = static_cast<uint8_t>(ri.mode);
    e.read_acks = ri.acks;
    e.pull_down = static_cast<uint8_t>(fusb302::pull_down());
    return e;
}

remote::IdentitySnapshot Remote::identity() {
    remote::IdentitySnapshot s;
    const pd::VdmInfo &v = fusb302::vdm();
    const pd::SourceCaps &c = pd::active_caps();
    // After an alone-Identify's release the detach cleared the live VDM record: fall back to the identity
    // the Identify collected, while it is still current (the phone still on the port).
    const pd::Identity &id = v.identity.valid ? v.identity : (fusb302::identify_current() ? fusb302::identify_identity() : v.identity);
    s.flags = (c.epr ? remote::kIdFlagEpr : 0) | (c.epr_partial ? remote::kIdFlagEprPartial : 0) |
              (v.cuktech ? remote::kIdFlagCuktech : 0) | (v.xiaomi ? remote::kIdFlagXiaomi : 0);
    s.valid = id.valid;
    s.vid = id.vid;
    s.pid = id.pid;
    s.bcd = id.bcd;
    s.num_svids = v.num_svids > remote::kMaxIdSvids ? remote::kMaxIdSvids : v.num_svids;
    for (unsigned i = 0; i < s.num_svids; i++) s.svids[i] = v.svids[i];
    s.vendor = sv(id.vendor);
    s.model = sv(id.model);
    return s;
}

remote::Status Remote::action(uint8_t id, bool confirm) {
    if (id < static_cast<uint8_t>(Action::PdRescan) || id > static_cast<uint8_t>(Action::Identify))
        return remote::Status::BadArg;
    K2LOG("[RMT] action=%d confirm=%d\n", static_cast<int>(id), confirm ? 1 : 0);
    // The dispatcher already refuses this; the backend asks again so no other caller can start a VBUS-raising action unconfirmed.
    if (remote::action_needs_confirm(id) && !confirm) return remote::Status::NeedConfirm;
    // A declined run (a charger test or an FC release owns the ports, a negotiation is in flight) is Busy with no
    // payload; NeedConfirm is only the confirm check above.
    return remote::action_start_status(ui_.run_action(static_cast<Action>(id), systick::millis(), confirm));
}

remote::IdentifyResult Remote::identify_device(uint8_t op) {
    remote::IdentifyResult r;
    if (op != 0) {
        K2LOG("[RMT] identify-device op=%d\n", static_cast<int>(op));
        const Ui::IdResult ir = ui_.identify(systick::millis());
        if (ir != Ui::IdResult::Started) {
            const IdRefusalWire w = identify_refusal_wire(ir);   // inline -> pd::Err::Inline, a running test stays Busy
            r.status = w.status;
            r.err = w.err;
            return r;
        }
    }
    const pd::IdStatus &s = fusb302::identify_status();
    r.progress.state = static_cast<uint8_t>(s.state);
    r.progress.step = static_cast<uint8_t>(s.step);
    r.progress.ok_mask = s.ok_mask;
    r.progress.skipped_mask = s.skipped_mask;
    r.progress.retries = s.retries;
    r.progress.num_sink_pdos = s.num_sink_pdos;
    for (unsigned i = 0; i < r.progress.sink_pdos.size(); i++) r.progress.sink_pdos[i] = s.sink_pdos[i];
    r.progress.revision = s.revision;
    return r;
}

remote::RereadResult Remote::reread(uint8_t op, uint8_t mask) {
    remote::RereadResult r;
    if (op == 1) {
        K2LOG("[RMT] reread start mask=0x%02X\n", static_cast<unsigned>(mask));
        if (!fusb302::reread_start(mask)) { r.status = remote::Status::Busy; return r; }
    } else if (op == 2) {   // stop a re-read and release the lab attach, whoever made it; Busy = a run or a far sink owns the line, nothing changed
        K2LOG("[RMT] reread abort\n");
        if (ui_.ports_busy() || !fusb302::reread_abort()) { r.status = remote::Status::Busy; return r; }
    } else if (op == 3) {
        fusb302::set_sink_cap_ext_mode(static_cast<pd::SinkCapExtMode>(mask));
    } else if (op == 4) {   // the next run's attach scan may take only this line (0 = normal)
        if (mask > 2) { r.status = remote::Status::BadArg; return r; }
        fusb302::set_force_line(mask);
    } else if (op == 5) {   // Ra mode (PC13 high, Rd on CC2 alone): the top-plug e-marker spoof
        K2LOG("[RMT] reread ra %u\n", static_cast<unsigned>(mask));
        if (!fusb302::ra_set(mask != 0)) { r.status = remote::Status::Busy; return r; }
    } else if (op == 6) {   // lab: pin the Rd (1 / 2, 0 = release) so a HID-powered K2 stands in for a charger-powered one
        K2LOG("[RMT] reread pin %u\n", static_cast<unsigned>(mask));
        if (!fusb302::debug_pin(mask)) { r.status = remote::Status::Busy; return r; }
    } else if (op == 8) {   // lab: a controlled re-attach, the Rd off for mask x 10 ms (Ra kept), VBUS watched every ms
        if (fusb302::inline_mode()) { r.status = remote::Status::Busy; return r; }   // never drive CC with a host's link inline
        const uint32_t gap = static_cast<uint32_t>(mask) * 10u;
        int32_t vmin = INT32_MAX, vend = -1;
        uint32_t below4 = 0;
        power::poll();
        const int32_t vstart = power::vbus_fast_mv().value_or(-1);
        auto watch = [&](bool serve) {   // one pass per millisecond
            power::poll();
            if (serve) fusb302::service();
            const int32_t v = power::vbus_fast_mv().value_or(-1);
            if (v >= 0 && v < vmin) vmin = v;
            if (v >= 0 && v < 4000) below4++;
            vend = v;
        };
        fusb302::rd_glitch_begin();
        const uint32_t t0 = systick::millis();
        uint32_t last = t0;
        while (systick::millis() - t0 < gap) {
            const uint32_t t = systick::millis();
            if (t == last) continue;
            last = t;
            watch(false);
        }
        fusb302::rd_glitch_end();
        const uint32_t t1 = systick::millis();
        last = t1;
        while (systick::millis() - t1 < 1500) {   // the re-attach too
            const uint32_t t = systick::millis();
            if (t == last) continue;
            last = t;
            watch(true);
        }
        K2LOG("[RMT] rd glitch %u ms: vbus start %d min %d end %d mV, %u ms below 4 V\n", static_cast<unsigned>(gap),
              static_cast<int>(vstart), static_cast<int>(vmin == INT32_MAX ? -1 : vmin), static_cast<int>(vend), static_cast<unsigned>(below4));
    } else if (op == 7) {   // lab: the unit boundary's Soft_Reset (the source re-announces, the Rd is kept)
        K2LOG("[RMT] reread relook\n");
        fusb302::unit_hold(false);
    }
    const pd::RereadStatus &s = fusb302::reread_status();
    r.info.state = static_cast<uint8_t>(s.state);
    r.info.step = static_cast<uint8_t>(s.step);
    r.info.mask = s.mask;
    r.info.ok_mask = s.ok_mask;
    r.info.skipped_mask = s.skipped_mask;
    r.info.pdos_before = s.pdos_before;
    r.info.pdos_last = s.pdos_last;
    r.info.caps_rx = s.caps_rx;
    r.info.flags = uint8_t((s.avs_seen ? 1 : 0) | (s.attaching ? 2 : 0) | (s.attach_failed ? 4 : 0) | (fusb302::ra_active() ? 8 : 0));
    return r;
}

remote::AppleReadResult Remote::apple_read(uint8_t op) {
    // Lab-only, off by default (k2/pd/apple_readout.hpp): op 1 arms the readout - our SVIDs ACK adds
    // 0x05AC and the sink re-requests the current contract, so the source re-runs its 0x05AC
    // discovery. The gate is the one in pd::apple_read_refusal; the sink double-checks the hard ones.
    remote::AppleReadResult r;
    if (op == 1) {
        const pd::AppleFacts facts{ui_.charger_test_busy(), ui_.link().inline_mode(),
                                   fusb302::trigger_refusal() != pd::TriggerRefusal::None,
                                   fusb302::sink_status().held, fusb302::answers_sop()};
        pd::AppleRefusal ref = pd::apple_read_refusal(facts);
        if (ref == pd::AppleRefusal::None) {
            K2LOG("[RMT] apple-read start\n");
            const pd::Err e = fusb302::apple_read_start();
            if (e != pd::Err::Ok)
                ref = e == pd::Err::Inline ? pd::AppleRefusal::Inline
                    : e == pd::Err::NoCaps ? pd::AppleRefusal::NoContract : pd::AppleRefusal::Busy;
        }
        if (ref != pd::AppleRefusal::None) {
            K2LOG("[RMT] apple-read refused why=%s\n", pd::apple_refusal_name(ref));
            r.status = ref == pd::AppleRefusal::Busy ? remote::Status::Busy : remote::Status::BadArg;
            r.refusal = static_cast<uint8_t>(ref);
            return r;
        }
    }
    const pd::AppleInfo &a = fusb302::apple_info();
    r.info.armed = fusb302::apple_armed() ? 1 : 0;
    r.info.why = static_cast<uint8_t>(a.why);
    r.info.reads = a.reads;
    r.info.ok = a.ok;
    // The field arrays are always NUL-terminated (arm zeroes them, the engine writes < 24 chars).
    r.info.maker = a.maker.data();
    r.info.product = a.product.data();
    r.info.fw = a.fw.data();
    r.info.hw = a.hw.data();
    r.info.field90 = a.field90.data();
    r.info.serial = a.serial.data();
    return r;
}

static remote::PdOpResult pd_result(pd::Err e) {
    remote::PdOpResult r;
    r.err = static_cast<uint8_t>(e);
    r.status = e == pd::Err::Ok ? remote::Status::Ok : e == pd::Err::Busy ? remote::Status::Busy : remote::Status::BadArg;
    return r;
}

remote::PdOpResult Remote::pd_request(uint8_t pos, uint16_t mv, uint16_t ma, bool confirm, bool over_load) {
    K2LOG("[RMT] pd request pos=%d mv=%d ma=%d confirm=%d over_load=%d\n", static_cast<int>(pos), static_cast<int>(mv),
                 static_cast<int>(ma), confirm ? 1 : 0, over_load ? 1 : 0);
    const Ui::TriggerOutcome o = ui_.trigger({pos, mv, ma}, systick::millis(), confirm, over_load);
    if (o.need_confirm) return {remote::Status::NeedConfirm, 0};
    return pd_result(o.err);
}

remote::PdOpResult Remote::pd_release() {
    K2LOG("[RMT] pd release\n");
    lease_.clear();   // the host let go of the output itself
    ui_.trigger_release(systick::millis());
    return {};
}

// The host dead-man lease (k2/core/lease.hpp). The dispatcher arms it for the host commands that can hold the output above
// 5 V and refreshes it on every request; poll() ticks it once per main-loop pass and runs the same stop path on expiry or
// an unplug. A charger test is never stopped by it: the lease ends when a test takes the ports.
void Remote::lease_touch() { lease_.touch(systick::millis()); }

void Remote::lease_arm() {
    const uint32_t now = systick::millis();
    lease_.arm(now);
    K2LOG("[RMT] lease armed %d\n", static_cast<int>(lease_.info(now).length_s));
}

remote::LeaseInfo Remote::lease(uint8_t op, uint16_t secs) {
    const uint32_t now = systick::millis();
    if (op == 1) lease_.set_length(now, secs);
    return lease_.info(now);
}

void Remote::lease_tick() {
    const uint32_t now = systick::millis();
    const auto vbus = power::vbus_fast_mv();
    const pd::SinkStatus &sk = fusb302::sink_status();
    // Nothing holds or negotiates above 5 V and VBUS reads 5 V: the lease may settle on its own (lease::safe_5v).
    lease::SafeFacts f;
    f.fc_idle = fctrig::status().state == fc::State::Idle && !fctrig::scan_running();
    f.vbus_known = vbus.has_value();
    f.vbus_mv = vbus ? *vbus : 0;
    f.sink_idle = sk.state == pd::SinkState::Idle;
    f.sink_ready = sk.state == pd::SinkState::Ready;
    f.held = sk.held;
    f.verifying = sk.verifying;
    f.contract_mv = sk.mv;
    const bool safe_5v = lease::safe_5v(f);
    switch (lease_.tick(now, usb::configured(), safe_5v, ui_.run_busy())) {   // a charger test or a PD Stress run
    case lease::Event::None: break;
    case lease::Event::Expired: K2LOG("[RMT] lease expired: stop\n"); stop(); break;
    case lease::Event::HostGone: K2LOG("[RMT] host gone: stop\n"); stop(); break;
    }
}

// The one-call stop (k2/core/stop.hpp): the HID `stop` command, the lease expiry and the CDC shell `stop` all land here.
remote::StopInfo Remote::stop() {
    struct RealEnv {
        Remote &r;
        bool test_active() { return r.ui_.run_busy() || r.ui_.twin_live_busy(); }   // a twin live proof and a PD Stress run are tests here
        bool fc_active() { return fctrig::status().state != fc::State::Idle || fctrig::scan_running(); }
        bool pd_held() { return pd::user_hold(fusb302::sink_status()); }   // the driver's own 5 V answer is nobody's trigger
        bool spoof_active() { return emk_spoof::active(); }
        void cancel_test() { r.ui_.cancel_runs(); }
        void stop_fc() { r.ui_.stop_fc(systick::millis()); }
        void release_pd() { r.ui_.trigger_release(systick::millis()); }
        void disarm_spoof() { emk_spoof::disarm(); r.spoof_seq_ = 0; }
        int32_t vbus_mv() { return power::vbus_fast_mv().value_or(-1); }
        bool fc_release_busy() { return r.ui_.fc_release().busy(); }
    } env{*this};
    lease_.clear();   // a stop by the host (or the shell) ends the lease; an expiry has already ended it with its own reason
    const remote::StopInfo info = k2::stop::stop_all(env);
    K2LOG("[RMT] stop active=%d vbus=%d fc_rel=%d\n", static_cast<int>(info.active), static_cast<int>(info.vbus_mv),
          info.fc_release_busy ? 1 : 0);
    return info;
}

remote::PdOpResult Remote::pd_hard_reset(uint8_t flags) {
    // Lab-only inline Hard Reset (`pd reset --inline --confirm`, ROADMAP 2026-11-10 connection-matrix P4): by
    // default a reset is refused while a live sink is attached (it would cut that sink's supply); the lab flags
    // lift exactly that refusal, gated by pd::lab_reset_verdict - refused while a charger test runs, and both
    // flags are required. The shell / UI paths call this with flags 0 and keep today's behaviour.
    const pd::LabResetFacts facts{ui_.charger_test_busy(), ui_.link().inline_mode(),
                                  fusb302::trigger_refusal() != pd::TriggerRefusal::None};
    const bool flag_inline = (flags & remote::kPdFlagResetInline) != 0;
    const bool flag_confirm = (flags & remote::kPdFlagConfirm) != 0;
    switch (pd::lab_reset_verdict(facts, flag_inline, flag_confirm)) {
        case pd::LabResetVerdict::RefusedBusy:
            K2LOG("[RMT] pd reset refused: charger test running\n");
            return {remote::Status::Busy, 0};
        case pd::LabResetVerdict::NeedConfirm:
            return {remote::Status::NeedConfirm, 0};
        case pd::LabResetVerdict::RefusedInline:
            return pd_result(pd::Err::Inline);   // never reset a live sink's supply
        case pd::LabResetVerdict::Allowed: break;
    }
    if (flag_inline) K2LOG("[RMT] pd hard reset lab inline flags=0x%02X\n", static_cast<unsigned>(flags));   // the lab op, logged
    else K2LOG("[RMT] pd hard reset\n");
    // The driver has its own inline guard: only the verdict's lab override lifts it (both flags, checked above).
    if (!ui_.trigger_hard_reset(systick::millis(), flag_inline && flag_confirm)) return pd_result(pd::Err::Inline);
    lease_.clear();   // a Hard Reset returns the source to 5 V
    return {};
}

remote::HandbackResult Remote::pd_handback(uint8_t flags) {
    // Phase-1 lab hand-back (ROADMAP 2026-11-10 "An inline latch leaves the far sink on Type-C current", phase 1
    // lab only): the whole Fix sequence - wait for /rd.rec to be gone, drop the Rd, one Hard Reset - is armed in
    // k2::handback only when the decision allows it; otherwise the refusal reason goes on the wire (the verdict
    // byte) and in the log. `pd reset --inline --confirm` stays the raw single reset.
    const bool confirm = (flags & remote::kPdFlagConfirm) != 0;
    const pd::SinkStatus& sk = fusb302::sink_status();
    const pd::HandbackFacts facts{ui_.charger_test_busy(), ui_.link().inline_mode(),
                                  sk.state == pd::SinkState::Ready || sk.held,   // an explicit contract the K2 owns
                                  fusb302::far_sink_seen(), handback::done()};
    const pd::HandbackVerdict v = pd::handback_verdict(facts, confirm);
    K2LOG("[LINK] handback verdict=%s (busy=%d inline=%d contract=%d far_req=%d done=%d confirm=%d)\n",
          pd::handback_verdict_name(v), int(facts.charger_test_busy), int(facts.inline_mode), int(facts.contract_owned),
          int(facts.far_request_seen), int(facts.handback_done), int(confirm));
    if (v != pd::HandbackVerdict::Allowed) return {remote::Status::Ok, uint8_t(v)};   // the verdict is the refusal reason
    handback::arm(systick::millis());   // the sequence runs from the main loop (the follower needs it to scrub /rd.rec)
    return {remote::Status::Ok, uint8_t(v)};
}

remote::PdAttachResult Remote::pd_attach(uint16_t target_mv, uint16_t target_ma, bool confirm) {
    // The verdict runs before anything touches the link: a refused attach puts no Rd on CC and sends nothing.
    // No target = 5 V; above 5 V needs the confirm flag exactly like `pd request`; inline (a sink behind the K2,
    // a sink Rd or a foreign Request on the wire) and busy (a charger test or an FC release owns the ports) are
    // outright refusals, never a re-attach under a live link.
    target_mv = pd::attach_target_mv(target_mv);
    const pd::AttachFacts facts{ui_.ports_busy(), ui_.link().inline_mode() || fusb302::inline_mode(),
                                fusb302::trigger_refusal() != pd::TriggerRefusal::None};
    const pd::AttachVerdict v = pd::attach_verdict(facts, target_mv, confirm);
    if (v != pd::AttachVerdict::Allowed) {
        K2LOG("[RMT] pd attach refused: %s (target_mv=%d busy=%d inline=%d trigger=%d confirm=%d)\n",
              v == pd::AttachVerdict::RefusedBusy ? "ports busy" : v == pd::AttachVerdict::RefusedInline ? "inline"
              : "confirm needed", static_cast<int>(target_mv), int(facts.ports_busy), int(facts.inline_mode),
              int(facts.trigger_refusal), int(confirm));
        if (v == pd::AttachVerdict::NeedConfirm) return {remote::Status::NeedConfirm, 0, {}};
        const remote::PdOpResult r = pd_result(v == pd::AttachVerdict::RefusedBusy ? pd::Err::Busy : pd::Err::Inline);
        return {r.status, r.err, {}};
    }

    // Get the monitor ceiling
    const std::int32_t cap_mv = power::max_vbus_mv();
    // Clamp target to the monitor ceiling, never request above 36000 mV
    if (target_mv > 36000) target_mv = 36000;  // hard clamp to 36 V for safety
    if (cap_mv > 0 && target_mv > static_cast<uint16_t>(cap_mv)) target_mv = static_cast<uint16_t>(cap_mv);

    K2LOG("[RMT] pd attach target_mv=%d target_ma=%d\n", static_cast<int>(target_mv), static_cast<int>(target_ma));

    const pd::SourceCaps& caps = pd::active_caps();

    // No valid capability list (whatever the active-CC line says: a charger that already waits shows its Rp there with
    // no list yet): present the single Rd (PD mode) and wait for the source caps. The verdict above guarantees
    // nothing is inline, so this is the plain sink attach.
    if (pd::attach_step(caps.valid, caps.num_pdos) == pd::AttachStep::PresentRd) {
        fusb302::unit_attach(false);
        K2LOG("[RMT] pd attach: presented Rd, waiting for caps\n");
        return {remote::Status::Ok, 0, pd_trigger()};
    }

    // Select the best PDO for the target voltage
    pd::Choice c = pd::pick_attach_pdo(caps, target_mv, target_ma, cap_mv);

    if (c.pos == 0) {
        // No suitable PDO found, return current status
        K2LOG("[RMT] pd attach: no suitable PDO for %d mV\n", static_cast<int>(target_mv));
        return {remote::Status::Ok, 0, pd_trigger()};
    }

    // The PDO choice may sit above the target (nearest fixed offer): the confirm rule applies to what is requested.
    const uint16_t chosen_mv = c.mv != 0 ? c.mv : caps.pdos[c.pos - 1].max_mv;
    if (chosen_mv > pd::kAttachDefaultMv && !confirm) {
        K2LOG("[RMT] pd attach refused: confirm needed (chosen pos=%d %d mV)\n", static_cast<int>(c.pos), static_cast<int>(chosen_mv));
        return {remote::Status::NeedConfirm, 0, {}};
    }

    // Request the selected PDO
    const pd::Err e = fusb302::sink_request(c, confirm);
    if (e != pd::Err::Ok) {
        K2LOG("[RMT] pd attach: sink_request err=%s\n", pd::err_name(e));
    }

    // Return the current trigger status. Only a request the sink engine accepted for a level above 5 V holds the output
    // under host control, so only that arms the dead-man lease (a failed request, Rd-only or no-PDO attach raised nothing).
    return {remote::Status::Ok, 0, pd_trigger(), e == pd::Err::Ok && chosen_mv > pd::kAttachDefaultMv};
}

remote::PdTriggerSnapshot Remote::pd_trigger() {
    const pd::SinkStatus &s = fusb302::sink_status();
    remote::PdTriggerSnapshot t;
    t.state = static_cast<uint8_t>(s.state);
    t.result = static_cast<uint8_t>(s.result);
    t.held = s.held;
    t.auto_hold = s.auto_hold;
    t.epr = s.epr_mode;
    t.pos = s.pos;
    t.mv = s.mv;
    t.ma = s.ma;
    t.hard_resets = s.hard_resets;
    t.source_hard_resets = s.source_hard_resets;
    t.pin = fusb302::pin_rd();   // a leftover Rd pin (pd-reread --pin) shows here until it clears
    const fusb302::LineTrial lt = fusb302::line_trial();
    t.trial_line = lt.line;      // the line under trial while the direct-plug schedule is live, else 0
    t.trial_moves = lt.moves;
    return t;
}

remote::MatchSnapshot Remote::match_info() {
    const MatchResult r = match_live(ui_.link());
    remote::MatchSnapshot m;
    m.caps_seen = r.caps_seen; m.request_seen = r.request_seen; m.sink_ok = r.sink_ok;
    m.flags = static_cast<uint8_t>(r.flags); m.verdict = static_cast<uint8_t>(r.verdict);
    m.want_pos = static_cast<uint8_t>(r.want_pos);
    m.want_mv = static_cast<uint16_t>(r.want_mv); m.want_ma = static_cast<uint16_t>(r.want_ma);
    m.need_ma = static_cast<uint16_t>(r.need_ma); m.have_ma = static_cast<uint16_t>(r.have_ma);
    m.src_max_mv = static_cast<uint16_t>(r.src_max_mv);
    m.src_max_w = static_cast<uint16_t>(std::min<uint32_t>(r.src_max_mw / 1000, 0xFFFF));
    m.rows = static_cast<uint8_t>(std::min<int>(r.rows, remote::kMatchMaxRows));
    for (size_t i = 0; i < m.rows; i++) m.row[i] = static_cast<uint8_t>(r.row[i]);
    return m;
}

remote::UsbLinkSnapshot Remote::usb_link() {
    const auto a = adc::read();
    const usblink::Link l = fusb302::usb_link(a ? a->dp_mv : -1, a ? a->dm_mv : -1);
    remote::UsbLinkSnapshot s;
    s.family = static_cast<uint8_t>(l.family); s.tbt = static_cast<uint8_t>(l.tbt);
    s.conf = static_cast<uint8_t>(l.conf); s.source = static_cast<uint8_t>(l.source);
    s.flags = static_cast<uint8_t>((l.cable_limited ? 1 : 0) | (l.dp_4lane ? 2 : 0) | (l.billboard ? 4 : 0) | (l.dp ? 8 : 0));
    s.mbps = l.mbps;
    s.text_len = static_cast<uint8_t>(usblink::format(l, s.text));
    return s;
}

remote::Status Remote::set_page(uint8_t page) {
    if (!ui_.goto_page(page, systick::millis())) return remote::Status::BadArg;
    K2LOG("[RMT] page=%d\n", static_cast<int>(page));
    return remote::Status::Ok;
}

void Remote::reboot() {
    K2LOG("[RMT] reboot requested\n");
    reboot_pending_ = true;
}

void Remote::debug_fault(uint8_t kind) {
    K2LOG("[RMT] DEBUG_FAULT kind=%d\n", kind);
    fault_kind_ = kind;
    fault_pending_ = true;
}

void Remote::dfu_enter(uint8_t reset_mode) {
    K2LOG("[RMT] DFU_ENTER confirmed, reset=%s\n", reset_mode == 1 ? "iwdg" : "sys");
    dfu_mode_ = reset_mode;
    dfu_pending_ = true;
}

namespace {

// The real wipe environment: the one littlefs volume, memory-mapped reads (ownership proof of the legacy
// pages), the guarded store erase, memory-mapped blank check, this firmware's backup registers.
struct WipeEnv {
    void unmount() { fs::instance().unmount(); }
    uint16_t read16(uint32_t addr) const { return *reinterpret_cast<const volatile uint16_t *>(addr); }
    flash::Status erase(uint32_t addr) { return flash::store_erase(addr); }
    bool blank(uint32_t addr) {
        const uint8_t *p = flash::store_view(addr);
        if (!p) return false;
        for (uint32_t i = 0; i < flash::kSectorSize; i++)
            if (p[i] != 0xFF) return false;
        return true;
    }
    void clear_backup() { session::wipe(); pinmark::write(0); }   // the pin mark too: the next boot must not inherit an Rd
};

}  // namespace

remote::FactoryWipeInfo Remote::factory_wipe(uint8_t reset_mode) {
    if (ui_.charger_test_busy()) K2LOG("[WIPE] note: a charger test was running\n");
    const bool grown = fs::grown();          // the run is part of the volume this boot: it goes too
    K2LOG("[WIPE] factory wipe: %u volume sectors + our own legacy pages, reset=%s\n",
          static_cast<unsigned>(grown ? flash::kFsGrownBlocks : factory_wipe::kFsSectors), reset_mode == 1 ? "iwdg" : "sys");
    WipeEnv env;
    const factory_wipe::Result r = factory_wipe::run(env, grown);
    remote::FactoryWipeInfo i;
    i.err = static_cast<uint8_t>(r.err);
    i.sectors = r.sectors; i.erased = r.erased; i.verified = r.verified; i.failed = r.failed;
    i.first_fail = r.first_fail; i.status = static_cast<uint8_t>(r.status);
    if (r.err == factory_wipe::Err::Ok) {
        K2LOG("[WIPE] done: %u erased, %u verified blank; DFU follows\n", static_cast<unsigned>(r.erased),
              static_cast<unsigned>(r.verified));
        dfu_mode_ = reset_mode;
        dfu_pending_ = true;                  // the response goes out first, then main enters DFU
    } else {
        K2LOG("[WIPE] FAILED err=%u erased=%u verified=%u failed=%u first=%08X status=%u; staying in the app\n",
              static_cast<unsigned>(r.err), static_cast<unsigned>(r.erased), static_cast<unsigned>(r.verified),
              static_cast<unsigned>(r.failed), static_cast<unsigned>(r.first_fail), static_cast<unsigned>(r.status));
    }
    return i;
}

void Remote::flash_read(uint32_t addr, std::span<uint8_t> out) {
    const volatile uint8_t *src = reinterpret_cast<const volatile uint8_t *>(addr);
    for (size_t i = 0; i < out.size(); i++) out[i] = src[i];
}

namespace {
char g_boot_text[bootinfo::kQrMax + 1];   // the K2B1 line a BOOT_INFO read-through holds (rendered at offset 0)
uint16_t g_boot_text_len = 0;
}

uint16_t Remote::boot_text(bool refresh) {
    if (refresh) {
        bootinfo::Snapshot snap;                // ~0.5 KB, one frame
        bootinfo::collect(snap);
        g_boot_text_len = uint16_t(bootinfo::format(snap, g_boot_text));
    }
    return g_boot_text_len;
}

void Remote::boot_text_read(uint16_t off, std::span<uint8_t> out) {
    for (size_t i = 0; i < out.size() && off + i < g_boot_text_len; i++) out[i] = uint8_t(g_boot_text[off + i]);
}

uint16_t Remote::boot_sample_count() { return uint16_t(boottrace::sample_count()); }

bootinfo::PowerSample Remote::boot_sample(uint16_t i) {
    const boottrace::Sample s = boottrace::sample_at(i);
    return {uint16_t(i * boottrace::kPeriodMs), s.vbus_mv, s.cc1_mv, s.cc2_mv, s.aux_mv, s.plus};
}

remote::Status Remote::debug_fake_pdos(uint8_t n) {
    if (ui_.charger_test_busy()) { K2LOG("[TEST] debug refused: test running\n"); return remote::Status::Busy; }
    ui_.debug_fake_pdos(n, systick::millis());
    K2LOG("[RMT] debug fake pdos=%d\n", static_cast<int>(n));
    return remote::Status::Ok;
}

remote::Status Remote::debug_link(bool attached) {
    if (ui_.charger_test_busy()) { K2LOG("[TEST] debug refused: test running\n"); return remote::Status::Busy; }
    ui_.debug_link(attached, systick::millis());
    return remote::Status::Ok;
}

remote::Status Remote::debug_pd_msg(uint8_t sop, uint16_t header, std::span<const uint8_t> payload) {
    if (ui_.charger_test_busy()) { K2LOG("[TEST] debug refused: test running\n"); return remote::Status::Busy; }
    fusb302::feed(sop, header, payload);
    K2LOG("[RMT] debug pd-msg sop=%d header=0x%04X objects=%d\n", sop, header, int((header >> 12) & 7));
    return remote::Status::Ok;
}

remote::Status Remote::debug_cc_matrix(uint8_t v, uint16_t modes) {
    if (ui_.charger_test_busy()) { K2LOG("[TEST] debug refused: test running\n"); return remote::Status::Busy; }
    if (v > 7) return remote::Status::BadArg;
    fusb302::debug_cc_matrix(v, modes);
    K2LOG("[RMT] debug cc-matrix levels=%u modes=%03x\n", v, modes);
    return remote::Status::Ok;
}

// VBUS for the CC probe: a direct power-monitor read (a fresh value about every 4.5 ms), because the main loop that
// feeds vbus_fast_mv() is blocked while the probe holds. -1 while the monitor is offline.
static std::int32_t cc_probe_vbus_mv() {
    const auto m = power::sample_now();
    return m ? m->vbus_mv : -1;
}

remote::Status Remote::debug_cc_probe(const ccprobe::Args &a, ccprobe::Result &out) {
    if (ui_.charger_test_busy()) { K2LOG("[TEST] debug refused: test running\n"); return remote::Status::Busy; }
    K2LOG("[CCPROBE] start sw0=%02X pc=%u ms=%u modes=%X\n", a.sw0, a.pc, a.ms, a.modes);
    const ccprobe::Refusal why = fusb302::cc_probe(a, cc_probe_vbus_mv, out);
    if (why != ccprobe::Refusal::None) {
        // 1 attached, 2 contract, 3 Rd presented, 4 an engine or the VCONN experiment running, 5 VBUS unknown, 6 VBUS up, 7 chip bus
        K2LOG("[CCPROBE] refused reason=%u (not passive)\n", unsigned(why));
        return remote::Status::Busy;
    }
    K2LOG("[CCPROBE] done n=%u flags=%u (1 aborted by VBUS, 2 restored) snapshot sw0=%02X sw1=%02X pc=%u modes=%03X\n",
          out.n, out.flags, out.sw0, out.sw1, out.pc, out.modes);
    for (unsigned i = 0; i < out.n; i++) {
        const ccprobe::Sample &s = out.s[i];
        K2LOG("[CCPROBE] #%u t=%uus cc1=%u cc2=%u vbus=%u st0=%02X/%02X\n", i, unsigned(s.t_us), s.cc1_raw, s.cc2_raw,
              s.vbus_mv, s.st0_cc1, s.st0_cc2);
    }
    return remote::Status::Ok;
}

// The silent-cable-chip probe (docs/K2_PORTS_SPEC.md, k2/pd/ra_probe.hpp). Raw samples only; the verdict is the classifier's.
remote::Status Remote::ra_probe(const raprobe::Args &a, raprobe::Result &out) {
    if (ui_.charger_test_busy()) {
        K2LOG("[TEST] debug refused: test running\n");
        out.refusal = static_cast<uint8_t>(raprobe::Refusal::Running);
        return remote::Status::Busy;
    }
    K2LOG("[RAPROBE] start vconn_pc=%u\n", a.vconn_pc);
    const raprobe::Refusal why = fusb302::ra_probe(a, cc_probe_vbus_mv, out);
    // 1 inline, 2 contract, 3 Rd, 4 Ra mode, 5 an engine or the VCONN experiment running, 6 VBUS unknown, 7 VBUS up, 8 chip bus, 9 a source level on CC
    if (why != raprobe::Refusal::None) K2LOG("[RAPROBE] refused reason=%u n=%u\n", unsigned(why), out.n);
    else K2LOG("[RAPROBE] done n=%u flags=%u (1 aborted by VBUS, 2 restored) snapshot sw0=%02X sw1=%02X pc=%u vconn_pc=%u vbus_max=%u\n",
               out.n, out.flags, out.sw0, out.sw1, out.pc, out.vconn_pc, out.vbus_max_mv);
    for (unsigned i = 0; i < out.n && i < out.s.size(); i++) K2LOG("[RAPROBE] #%u cc1=%u cc2=%u\n", i, out.s[i].cc1_raw, out.s[i].cc2_raw);
    return why == raprobe::Refusal::None ? remote::Status::Ok : remote::Status::Busy;
}

remote::VconnSnapshot Remote::debug_vconn(uint8_t op) {
    remote::VconnSnapshot s;
    s.ok = 1;
    if (ui_.charger_test_busy()) { K2LOG("[TEST] debug refused: test running\n"); s.ok = 0; }
    else if (op == 1) s.ok = fusb302::vconn_request() ? 1 : 0;
    else if (op == 2) fusb302::vconn_off();
    else if (op == 3) s.ok = fusb302::vconn_roundtrip() ? 1 : 0;
    else if (op == 4) s.ok = fusb302::data_reset() ? 1 : 0;
    const fusb302::VconnInfo v = fusb302::vconn_info();
    s.state = v.state; s.why = v.why; s.applied = v.applied; s.swaps_rx = v.swaps_rx; s.active_cc = v.active_cc;
    s.cc1_mv = v.cc1_mv; s.cc2_mv = v.cc2_mv;
    K2LOG("[RMT] debug vconn op=%u ok=%u state=%u\n", op, s.ok, s.state);
    return s;
}

remote::EprInfoSnapshot Remote::debug_epr_info(uint8_t op) {
    remote::EprInfoSnapshot s;
    s.ok = 1;
    if (ui_.charger_test_busy()) { K2LOG("[TEST] debug refused: test running\n"); s.ok = 0; }
    else if (op != 0) s.err = static_cast<uint8_t>(fusb302::epr_info_request(op == 2));
    s.state = static_cast<uint8_t>(fusb302::sink_status().epr_info);
    const pd::SourceCaps &list = fusb302::epr_info_list();
    if (s.state == static_cast<uint8_t>(pd::EprInfo::Got) && list.valid) {
        s.num_pdos = list.num_pdos;
        for (size_t k = 0; k < s.pdos.size() && k < list.num_pdos; k++) s.pdos[k] = list.pdos[k].raw;
    }
    K2LOG("[RMT] debug epr-info op=%u ok=%u err=%u state=%u pdos=%u\n", op, s.ok, s.err, s.state, s.num_pdos);
    return s;
}

remote::LcdSnapshot Remote::debug_lcd() {
    const lcd::DmaInfo d = lcd::dma_info();
    remote::LcdSnapshot s;
    s.flags = static_cast<uint8_t>((d.enabled ? 1 : 0) | (d.ok ? 2 : 0));
    s.transfers = d.transfers;
    s.stalls = d.stalls;
    const paint::Reading t = paint::meter.total();
    s.paint_px = t.px;
    s.paint_ms = t.paint_ms;
    s.fs_ms = t.fs_ms;
    s.spi_div = static_cast<uint8_t>(lcd::spi_div());
    s.blocked_ms = static_cast<uint16_t>(std::min<uint32_t>(gap::blocked.run_peak(systick::millis()), 0xFFFFu));
    K2LOG("[RMT] debug lcd flags=%u transfers=%u stalls=%u\n", s.flags, unsigned(s.transfers), unsigned(s.stalls));
    return s;
}

// The read happens inside a remote poll, between two passes: the pass that runs it is still open, so its own length lands in
// the counters at its end (after a reset too, the reset keeps the gap references).
remote::LoopSnapshot Remote::debug_loop(uint8_t page, bool reset) {
    static_assert(remote::kLoopBuckets == loopstat::kBuckets);
    loopstat::Stats& st = loopstat::stats;
    const uint32_t now = systick::millis();
    remote::LoopSnapshot s;
    s.elapsed_ms = now - st.since_ms;
    auto sat = [](uint32_t v) { return static_cast<uint16_t>(std::min<uint32_t>(v, 0xFFFFu)); };
    s.loop_max_ms = sat(st.loop.max_ms);
    s.svc_max_ms = sat(st.service.max_ms);
    for (size_t i = 0; i < loopstat::kBuckets; i++) {
        s.loop_hist[i] = sat(st.loop.hist[i]);
        s.svc_hist[i] = sat(st.service.hist[i]);
    }
    const loopstat::Pass p = st.pages.get(page);
    s.page = page;
    s.page_us = p.us;
    s.page_px = p.px;
    s.worst_page = st.pages.worst_page;
    s.worst_us = st.pages.worst.us;
    s.worst_px = st.pages.worst.px;
    if (reset) st.reset(now);
    return s;
}

remote::AdcRawSnapshot Remote::adc_raw() {
    remote::AdcRawSnapshot snap;
    adc::RawAll all{};
    snap.ok = adc::sweep_all(all) ? 1 : 0;
    for (unsigned i = 0; i < snap.all.size(); i++) snap.all[i] = all[i];
    if (const auto a = adc::read())
        for (unsigned i = 0; i < snap.scan.size() && i < a->raw.size(); i++) snap.scan[i] = a->raw[i];
    return snap;
}

remote::Status Remote::ripple(uint8_t sample_code) {
    const uint32_t period = adc::capture_ripple(ripple_, sample_code);
    if (period == 0) {
        K2LOG("[RMT] ripple capture timed out\n");
        return remote::Status::Busy;
    }
    const adc::Stats st = adc::summarize(ripple_);
    ripple_info_ = {period, static_cast<uint16_t>(ripple_.size()), st.min, st.max, st.mean};
    ripple_pending_ = true;
    K2LOG("[RMT] ripple code=%d period=%dns min=%d max=%d\n", static_cast<int>(sample_code),
                 static_cast<int>(period), static_cast<int>(st.min), static_cast<int>(st.max));
    return remote::Status::Ok;
}

// Active fast-charge trigger. The confirm rule is enforced again here (and in fctrig::start): a
// refusal comes back as BadArg, a running trigger as Busy.
remote::Status Remote::fc_cmd(uint8_t op, uint8_t proto, uint16_t mv, uint8_t flags) {
    if (op == remote::kFcReset) ui_.cancel_charger_test();
    else if (op != remote::kFcStatus && ui_.ports_busy()) {
        K2LOG("[TEST] FC request refused: test running\n");
        return remote::Status::Busy;
    }
    const bool confirm = flags & remote::kFcConfirm, allow_inline = flags & remote::kFcAllowInline;
    fctrig::Fail f = fctrig::Fail::None;
    switch (op) {
    case remote::kFcStatus: return remote::Status::Ok;
    case remote::kFcReset: lease_.clear(); ui_.stop_fc(systick::millis()); return remote::Status::Ok;
    case remote::kFcScan: f = fctrig::scan_start(confirm, allow_inline); break;
    case remote::kFcTrigger:
        if (proto >= remote::kFcProtoCount) return remote::Status::BadArg;
        f = fctrig::start(static_cast<fc::Proto>(proto), mv, confirm, allow_inline);
        break;
    default: return remote::Status::BadArg;
    }
    if (f == fctrig::Fail::None) return remote::Status::Ok;
    return f == fctrig::Fail::Busy ? remote::Status::Busy : remote::Status::BadArg;
}

remote::StoreInfo Remote::store_cmd(uint8_t op, uint8_t /*slot*/) {
    storage::Result r;
    switch (op) {
    case remote::kStoreSaveEmarker: r = storage::save_emarker(); break;
    case remote::kStoreSaveLog: r = storage::save_log(); break;
    default: break;   // list
    }
    const store::Brief in = storage::brief();
    remote::StoreInfo i;
    i.err = static_cast<uint8_t>(r.err);
    uint16_t seqs[emlib::kCapacity];
    i.count = static_cast<uint8_t>(storage::lib_list(std::span<uint16_t>(seqs, emlib::kCapacity)));
    i.flags = static_cast<uint8_t>((in.has_single ? remote::kStoreFlagSingle : 0) |
                                   (in.library_foreign ? remote::kStoreFlagForeign : 0) |
                                   (in.log_len ? remote::kStoreFlagLog : 0));
    i.slot = static_cast<uint8_t>(r.slot);
    i.log_len = static_cast<uint16_t>(in.log_len);
    i.bank = storage::bank();
    i.native = in.native_count;
    return i;
}

void Remote::cable_r_reset() {
    ui_.cable_reset();
    K2LOG("[RMT] cable resistance reset\n");
}

void Remote::cable_r_mode(bool kelvin) {
    ui_.set_cable_mode(kelvin ? cable_r::Mode::Kelvin : cable_r::Mode::Passive);
    K2LOG("[RMT] cable resistance mode %s\n", kelvin ? "kelvin" : "passive");
}

// PA8 load probe (k2/drivers/load.hpp). Refused while a trigger, scan or test owns the ports.
remote::LabResult Remote::lab_load(uint8_t duty, uint16_t ms) {
    remote::LabResult out;
    if (ui_.ports_busy() || fctrig::status().state != fc::State::Idle || dpsniff::armed()) { out.status = static_cast<uint8_t>(remote::Status::Busy); return out; }
    if (duty == 0 || duty > load::kMaxDutyPct || ms == 0 || ms > load::kMaxMs) { out.status = static_cast<uint8_t>(remote::Status::BadArg); return out; }
    const load::Result r = load::probe(duty, ms);
    K2LOG("[LOAD] pin=%u%% duty=%u ms=%u reason=%u idle=%d peak=%d vmin=%d t=%d>%d\n", r.pin_high_pct, r.duty, r.ms,
                 static_cast<unsigned>(r.reason), static_cast<int>(r.idle_ma), static_cast<int>(r.peak_ma),
                 static_cast<int>(r.min_vbus_mv), r.temp_before_deci_c, r.temp_after_deci_c);
    out.reason = static_cast<uint8_t>(r.reason); out.duty = r.duty; out.ms = r.ms;
    out.idle_ma = static_cast<int16_t>(r.idle_ma); out.peak_ma = static_cast<int16_t>(r.peak_ma);
    out.min_vbus_mv = static_cast<uint16_t>(r.min_vbus_mv); out.vbus_before_mv = static_cast<uint16_t>(r.vbus_before_mv);
    out.temp_before_dc = r.temp_before_deci_c; out.temp_after_dc = r.temp_after_deci_c; out.pin_high_pct = r.pin_high_pct;
    return out;
}

// D+/D- edge capture (k2/fastcharge/dpsniff.hpp). Arming is refused while anything owns the ports; it never drives.
remote::SniffInfo Remote::lab_sniff(uint8_t sub) {
    remote::SniffInfo out;
    if (sub == 1) {
        if (ui_.ports_busy() || fctrig::status().state != fc::State::Idle || fctrig::scan_running()) { out.status = static_cast<uint8_t>(remote::Status::Busy); return out; }
        if (dpsniff::arm() != dpsniff::ArmResult::Ok) { out.status = static_cast<uint8_t>(remote::Status::Busy); return out; }
    } else if (sub == 0) {
        dpsniff::cancel();
    } else if (sub == 3) {
        if (!dpsniff::selftest()) { out.status = static_cast<uint8_t>(remote::Status::Busy); return out; }
    }
    const dpsniff::Status s = dpsniff::status();
    out.state = static_cast<uint8_t>(s.state); out.reason = static_cast<uint8_t>(s.reason);
    out.flags = static_cast<uint8_t>((s.overflow ? 1 : 0) | (s.stored ? 2 : 0)); out.edges = s.edges;
    return out;
}

// --- vooc-monitor (k2/vooc-detect) ---
// Inline VOOC monitor (k2/fastcharge/dpsniff.hpp): the switch is RAM only and only gates the passive listener; the
// fast-charge poll starts / stops it, so a switch-off releases the lines on the next poll.
remote::VoocSnapshot Remote::vooc_monitor(uint8_t set) {
    if (set <= 1) dpsniff::monitor_enable(set == 1);
    const dpsniff::MonitorInfo m = dpsniff::monitor_info();
    remote::VoocSnapshot v;
    v.enabled = m.enabled; v.running = m.running; v.s = m.s; v.guard = m.guard;
    return v;
}
// --- end vooc-monitor ---

// UFCS tuner and 5 V probe (k2/fastcharge/fctrig.hpp). The tuner is RAM only; the probe never raises VBUS and never
// sends a Request. A refused probe start answers Ok with the reason in the status block's `start` byte.
remote::UfcsInfo Remote::ufcs_info() {
    remote::UfcsInfo i;
    const ufcs::ProbeResult &r = fctrig::ufcs_probe_result();
    i.state = fctrig::ufcs_probe_running() ? 1 : fctrig::ufcs_probe_done() ? 2 : 0;
    i.verdict = static_cast<uint8_t>(r.verdict);
    i.hs_class = static_cast<uint8_t>(r.hs_class);
    i.start = ufcs_start_;
    i.blob_len = static_cast<uint16_t>(fctrig::ufcs_probe_blob().size());
    return i;
}

uint32_t Remote::ufcs_get(uint8_t field) {
    return ufcs::get(fctrig::ufcs_tuner(), static_cast<ufcs::Field>(field));
}

remote::Status Remote::ufcs_set(uint8_t field, uint32_t value) {
    if (ui_.ports_busy() || fctrig::status().state != fc::State::Idle) return remote::Status::Busy;
    switch (fctrig::ufcs_set(static_cast<ufcs::Field>(field), value)) {
    case fctrig::TuneSet::Ok: return remote::Status::Ok;
    case fctrig::TuneSet::Range: return remote::Status::BadArg;
    default: return remote::Status::Busy;
    }
}

remote::Status Remote::ufcs_op(uint8_t op) {
    if (op == remote::kUfcsStop) {
        fctrig::ufcs_probe_stop();
        return remote::Status::Ok;
    }
    if (ui_.ports_busy() || fctrig::status().state != fc::State::Idle) return remote::Status::Busy;
    if (op == remote::kUfcsReset) return fctrig::ufcs_tuner_reset() ? remote::Status::Ok : remote::Status::Busy;
    if (op != remote::kUfcsProbe) return remote::Status::BadArg;
    ufcs_start_ = static_cast<uint8_t>(fctrig::ufcs_probe_start());
    return remote::Status::Ok;
}

size_t Remote::ufcs_read(uint16_t off, std::span<uint8_t> out) {
    const std::span<const uint8_t> b = fctrig::ufcs_probe_blob();
    if (off >= b.size()) return 0;
    const size_t n = std::min(out.size(), b.size() - off);
    std::copy_n(b.begin() + off, n, out.begin());
    return n;
}

// ---- PD Stress ----
remote::PdStressInfo Remote::pdstress_info() {
    const PdStressSession::Status s = ui_.pd_stress().status(systick::millis());
    remote::PdStressInfo i;
    i.state = static_cast<uint8_t>(s.state);   // Idle 0, Restoring 1, Running 2, Finished 3: the wire values
    i.stop = s.stop; i.start = s.start;
    i.seq_idx = s.seq_idx; i.seq_total = s.seq_total; i.seq_cur = s.seq_cur;
    i.reboots = s.reboots; i.transitions = s.transitions; i.elapsed_ms = s.elapsed_ms; i.score_cp = s.score_cp; i.blob_len = s.blob_len;
    return i;
}

remote::Status Remote::pdstress_start(const remote::PdStressStart &s) {
    const pdstress::Options o = pdstress::make_options(s.mask, s.cycles_pct, s.pace, s.max_s, s.max_trans);
    const pdstress::StartResult r = ui_.pd_stress().start(o, systick::millis());
    return r == pdstress::StartResult::Busy ? remote::Status::Busy : remote::Status::Ok;
}

void Remote::pdstress_stop() { ui_.pd_stress().cancel(); }

size_t Remote::pdstress_read(uint16_t off, std::span<uint8_t> out) { return ui_.pd_stress().result_read(off, out); }
// ---- end PD Stress ----

uint16_t Remote::loopprobe_len() { return loopprobe::hw::file_len(); }

size_t Remote::loopprobe_read(uint16_t off, std::span<uint8_t> out) { return loopprobe::hw::file_read(off, out); }

remote::Status Remote::qrtest_show(uint8_t version, uint8_t px, uint8_t quiet) {
    if (!qrtest::show(ui_, version, px, quiet, systick::millis())) {
        K2LOG("[QRTEST] show refused: v%u px%u q%u\n", unsigned(version), unsigned(px), unsigned(quiet));
        return remote::Status::Busy;
    }
    K2LOG("[QRTEST] show v%u px%u q%u\n", unsigned(version), unsigned(px), unsigned(quiet));
    return remote::Status::Ok;
}

bool Remote::qrtest_stop() {
    const bool was = qrtest::stop(ui_, systick::millis());
    if (was) K2LOG("[QRTEST] stop\n");
    return was;
}

remote::CableRInfo Remote::cable_r_info() {
    remote::CableRInfo i;
    auto u16 = [](int32_t v) { return static_cast<uint16_t>(v < 0 ? 0 : v > 65535 ? 65535 : v); };
    if (ui_.cable_mode() == cable_r::Mode::Kelvin) {
        const auto &k = ui_.kelvin();
        i.flags = static_cast<uint8_t>((k ? 2 : 0) | 4);
        i.grade = static_cast<uint8_t>(cable_r::grade(k));
        if (k) { i.r_mohm = u16(k->total_mohm); i.pos_mohm = u16(k->pos_mohm); i.gnd_mohm = u16(k->gnd_mohm); }
        return i;
    }
    const cable_r::Meter &m = ui_.cable();
    const auto est = m.estimate();
    i.flags = static_cast<uint8_t>((m.has_idle() ? 1 : 0) | (est ? 2 : 0));
    i.grade = static_cast<uint8_t>(cable_r::grade(est));
    if (m.has_idle()) { i.idle_mv = u16(m.idle_mv()); i.idle_ma = u16(m.idle_ma()); }
    if (est) { i.r_mohm = est->mohm; i.load_mv = u16(est->load_mv); i.load_ma = u16(est->load_ma); }
    return i;
}

static_assert(remote::kBbRead == pdtrace::kOpRead && remote::kBbClear == pdtrace::kOpClear &&
              remote::kBbFreeze == pdtrace::kOpFreeze && remote::kBbStatus == pdtrace::kOpStatus &&
              remote::kBbCapList == pdtrace::kOpCapList && remote::kBbCapRead == pdtrace::kOpCapRead &&
              remote::kBbCapClear == pdtrace::kOpCapClear);

// Slot kCapSlots (one past the real ones) is the sniffer's whole edge buffer, not a Black Box slot: the
// offset counts EDGES, a read returns one Edges record, `len` in the reply is the edge count.
static_assert(6 + edges::kChunkBytes <= remote::kMaxRspPayload - pdtrace::kReadHeader);
size_t Remote::blackbox(uint8_t op, uint32_t arg, std::span<uint8_t> out) {
    if (op == pdtrace::kOpCapRead && (arg >> 16) == pdtrace::kCapSlots && out.size() >= pdtrace::kReadHeader) {
        std::size_t used = 0, total = 0;
        const std::uint32_t off = arg & 0xFFFF;
        const std::size_t n = dpsniff::read_record(off, out.subspan(pdtrace::kReadHeader), used, total);
        out[0] = 0;
        const std::uint32_t next = off + used, len = static_cast<std::uint32_t>(total);
        for (int i = 0; i < 4; i++) out[1 + i] = static_cast<uint8_t>(next >> (8 * i));
        for (int i = 0; i < 4; i++) out[5 + i] = static_cast<uint8_t>(len >> (8 * i));
        return pdtrace::kReadHeader + n;
    }
    return pdtrace::remote(op, arg, out);
}

namespace {
// The Cards command's store, bound to the flash-backed report_card::hw.
struct CardsApi {
    std::size_t numbers(std::span<uint16_t> o) { return report_card::hw::numbers(o); }
    bool get(uint16_t n, report_card::Card& c) { return report_card::hw::get(n, c); }
    report_card::Err rename(uint16_t n, std::string_view v) { return report_card::hw::rename(n, v); }
    report_card::Err remove(uint16_t n) { return report_card::hw::remove(n); }
};
static_assert(remote::kCardsList == report_card::remote::kList && remote::kCardsGet == report_card::remote::kGet &&
              remote::kCardsRename == report_card::remote::kRename && remote::kCardsDelete == report_card::remote::kDelete &&
              remote::kCardsChunks == report_card::remote::kChunks &&
              remote::kCardsErrBusy == report_card::remote::kErrBusy);
}  // namespace

// Writes (rename, delete) only on these explicit commands. The dispatcher refuses them with kCardsErrBusy
// while a charger test or an FC release owns the link (the run saves its own card at the end and the flash
// is shared), so they never get here then.
size_t Remote::cards(std::span<const uint8_t> p, std::span<uint8_t> out) {
    CardsApi api;
    return report_card::remote::handle(api, p, out);
}

namespace {
// The Twin command's Api: the library in flash (k2::storage), the live proof and the learning (k2::twin_hw).
struct TwinApi {
    Ui& ui;
    unsigned list(std::span<uint8_t> o) { return storage::twin_list(o); }
    bool get(uint8_t s, twin::Twin& t) { return storage::twin_get(s, t); }
    bool predict(const twin::Twin& t, twin::Outcome& o, twin::Facts& f) {
        f = twin_hw::facts_now();
        return twin_hw::predict(t, o);
    }
    void log_predict(uint8_t seq, const twin::Twin&, const twin::Outcome& o) { twin_hw::log_predict(seq, o); }
    twin_hw::Learned learn(uint8_t how, std::string_view name, bool save) {
        return how == 0 ? twin_hw::learn_from_identify(name, save) : twin_hw::learn_from_sniff(name, save);
    }
    twin::Err rename(uint8_t s, std::string_view n) { return storage::twin_rename(s, n); }
    twin::Err remove(uint8_t s) { return storage::twin_remove(s); }
    twin::live::Refusal run(std::span<const uint8_t> seqs, bool confirmed) {
        return twin_hw::live_start(seqs, confirmed, ui.twin_ctx(systick::millis()));
    }
    const twin::live::Runner& runner() const { return twin_hw::live(); }
    bool running() const { return twin_hw::live_running(); }
    void cancel() { ui.cancel_twin(); }
};
static_assert(remote::kTwList == twin::wire::kOpList && remote::kTwGet == twin::wire::kOpGet &&
              remote::kTwPredict == twin::wire::kOpPredict && remote::kTwLearn == twin::wire::kOpLearn &&
              remote::kTwRename == twin::wire::kOpRename && remote::kTwDelete == twin::wire::kOpDelete &&
              remote::kTwRun == twin::wire::kOpRun && remote::kTwStatus == twin::wire::kOpStatus &&
              remote::kTwResult == twin::wire::kOpResult && remote::kTwUsb == twin::wire::kOpUsb &&
              remote::kTwCancel == twin::wire::kOpCancel);
static_assert(remote::kTwErrBusy == twin::wire::kErrBusy && remote::kTwNameMax == twin::kNameMax &&
              remote::kTwMax == twin::kCapacity && remote::kTwParts == twin::wire::kMaxParts &&
              remote::kTwRunConfirm == twin::wire::kRunConfirm);
static_assert(twin::wire::kMaxRsp == remote::kMaxRspPayload);
}  // namespace

// Reads (list, get, predict, usb, status, result, cancel) never write. Writes (learn without the dry bit, rename, delete, run)
// reach here only when no test or FC release owns the link (the dispatcher refuses them with Busy otherwise).
size_t Remote::twin(std::span<const uint8_t> p, std::span<uint8_t> out) {
    TwinApi api{ui_};
    return twin::wire::handle(api, p, out);
}

// The Black Box diagnosis: read-only, the same report the screen shows.
size_t Remote::diagnose(std::span<const uint8_t> p, std::span<uint8_t> out) {
    namespace live = k2::pd::diag::live;
    const live::State st = live::state();
    return k2::pd::diag::wire::encode(live::report(), static_cast<uint8_t>(st), live::sessions(), p, out);
}

// The transition capture. Arming changes the power monitor's mode and stores nothing; a charger test that is
// running owns the capture (it re-arms before each step), so a manual arm is refused while one runs.
size_t Remote::transcap(std::span<const uint8_t> p, std::span<uint8_t> out) {
    namespace tc = k2::transcap;
    auto put = [&out](size_t at, uint32_t v, unsigned n) { for (unsigned i = 0; i < n; i++) out[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
    auto get32 = [](const uint8_t *q) { return uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24; };
    switch (p[0]) {
    case remote::kTcStatus: {
        const auto s = tc::status();
        out[0] = static_cast<uint8_t>(s.state); out[1] = static_cast<uint8_t>(s.reason);
        out[2] = static_cast<uint8_t>((s.fast ? 1 : 0) | (s.keep ? 2 : 0));
        put(3, s.samples, 2); out[5] = s.markers; put(6, s.lost, 4); put(10, s.span_us, 4);
        put(14, s.max_gap_us, 4); put(18, s.trigger_age_ms, 4); put(22, s.saved_seq, 4);
        return remote::kTcStatusBytes;
    }
    case remote::kTcArm:
        out[0] = static_cast<uint8_t>(ui_.charger_test_busy() ? tc::ArmResult::Busy : tc::arm());
        return 1;
    case remote::kTcTrigger: out[0] = tc::trigger(tc::Reason::Manual) ? 1 : 0; return 1;
    case remote::kTcDisarm: tc::disarm(); out[0] = 0; return 1;
    case remote::kTcSave: put(0, tc::save_now(0, 0xFFFF), 4); return 4;
    case remote::kTcLive: return tc::read_live(get32(&p[1]), out.first(std::min(out.size(), remote::kMaxRspPayload)));
    case remote::kTcStored: return tc::read_stored(get32(&p[1]), get32(&p[5]), out.first(std::min(out.size(), remote::kMaxRspPayload)));
    case remote::kTcList: {
        std::array<tc::Meta, tc::kMaxStored> m{};
        const size_t n = std::min(tc::list_stored(m), remote::kMaxRspPayload / remote::kTcListEntry);
        out[0] = static_cast<uint8_t>(n);
        for (size_t i = 0; i < n; i++) {
            const size_t at = 1 + i * remote::kTcListEntry;
            put(at, m[i].seq, 4); put(at + 4, m[i].run_id, 4); put(at + 8, m[i].step, 2);
            out[at + 10] = static_cast<uint8_t>(m[i].reason);
        }
        return 1 + n * remote::kTcListEntry;
    }
    default: tc::clear_stored(); out[0] = 0; return 1;   // kTcClear
    }
}

size_t Remote::emlib(std::span<const uint8_t> p, std::span<uint8_t> out) {
    using remote::kEmErrInline;
    auto le16 = [](const uint8_t *q) { return static_cast<uint16_t>(q[0] | (q[1] << 8)); };
    auto put16 = [&out](size_t at, uint32_t v) { out[at] = static_cast<uint8_t>(v); out[at + 1] = static_cast<uint8_t>(v >> 8); };
    auto err = [&out](emlib::Err e) { out[0] = static_cast<uint8_t>(e); return size_t{1}; };
    switch (p[0]) {
    case remote::kEmList: {
        std::array<uint16_t, emlib::kCapacity> all{};
        const unsigned total = storage::lib_list(all);
        const unsigned start = p.size() > 1 ? p[1] : 0;
        const unsigned n = start >= total ? 0 : std::min<unsigned>(total - start, remote::kEmListMax);
        out[0] = 0; out[1] = static_cast<uint8_t>(std::min(total, 255u)); out[2] = static_cast<uint8_t>(n);
        for (unsigned i = 0; i < n; i++) put16(3 + 2 * i, all[start + i]);
        return 3 + 2 * n;
    }
    case remote::kEmGet: {
        emlib::Entry e;
        if (!storage::lib_get(le16(&p[1]), e)) return err(emlib::Err::NotFound);
        out[0] = 0; put16(1, e.seq);
        for (size_t i = 0; i < remote::kEmNameMax; i++) out[3 + i] = static_cast<uint8_t>(e.name[i]);
        put16(27, emrec::vdo(e.rec, 1) & 0xFFFF); put16(29, emrec::vdo(e.rec, 3) >> 16);
        return 31;
    }
    case remote::kEmSave: {
        const emlib::Result r = storage::lib_save_current();
        out[0] = static_cast<uint8_t>(r.err); put16(1, r.seq); out[3] = r.existed ? 1 : 0;
        return 4;
    }
    case remote::kEmDelete: return err(storage::lib_remove(le16(&p[1])));
    case remote::kEmRename: {
        const uint16_t seq = le16(&p[1]);
        const emlib::Err e = storage::lib_rename(seq, std::string_view(reinterpret_cast<const char *>(&p[3]), p.size() - 3));
        out[0] = static_cast<uint8_t>(e); put16(1, seq);
        return 3;
    }
    case remote::kEmLearn: {
        switch (p[1]) {
        case remote::kEmLearnRun: {
            emk_learn::Options opt;
            bool confirm = false;
            if (!emk_learn::parse_options(p.subspan(2), opt, confirm)) { out[0] = static_cast<uint8_t>(emlib::Err::Invalid); return 1; }
            if (ui_.spoof_busy() || emk_spoof::active()) {
                K2LOG("[EMK] learn refused: tool busy or a spoof is armed\n");
                out[0] = remote::kEmErrBusy;
                return 1;
            }
            emk_learn_hw::run(opt, confirm);
            return emk_learn::encode_summary(emk_learn_hw::last(), out);
        }
        case remote::kEmLearnEntry: {
            const auto *r = emk_learn_hw::last();
            if (r == nullptr) { out[0] = static_cast<uint8_t>(emlib::Err::NotFound); return 1; }
            return emk_learn::encode_entry(*r, p[2], out);
        }
        case remote::kEmLearnSave: {
            const emlib::Result r = emk_learn_hw::save();
            out[0] = static_cast<uint8_t>(r.err); put16(1, r.seq); out[3] = r.existed ? 1 : 0;
            return 4;
        }
        default:   // kEmLearnStatus
            return emk_learn::encode_summary(emk_learn_hw::last(), out);
        }
    }
    case remote::kEmUnmatched:
        return emk_quirk::encode_seen(emk_spoof::seen(), p[1], out);
    default: {   // kEmSpoof
        uint8_t e = 0;
        if (p[1] == remote::kEmSpoofArm) {
            emlib::Entry ent;
            emk_quirk::Quirks quirks;
            if (ui_.spoof_busy()) {
                K2LOG("[TEST] spoof refused: tool busy\n");
                e = remote::kEmErrBusy;
            } else if (!storage::lib_get(le16(&p[2]), ent, quirks)) e = static_cast<uint8_t>(emlib::Err::NotFound);
            else {
                const emk_spoof::Err a = emk_spoof::arm(ent.rec, quirks);
                e = a == emk_spoof::Err::Ok ? 0 : a == emk_spoof::Err::Inline ? kEmErrInline : static_cast<uint8_t>(emlib::Err::Invalid);
                if (e == 0) spoof_seq_ = ent.seq;
            }
        } else if (p[1] == remote::kEmSpoofBuiltin) {
            if (ui_.spoof_busy()) e = remote::kEmErrBusy;
            else {
                const emk_spoof::Err a = emk_spoof::arm(emrec::builtin_cable(p[2]), emk_quirk::builtin_quirks(p[2]));
                e = a == emk_spoof::Err::Ok ? 0 : a == emk_spoof::Err::Inline ? kEmErrInline : static_cast<uint8_t>(emlib::Err::Invalid);
                spoof_seq_ = 0;
            }
        } else if (p[1] == remote::kEmSpoofDisarm) {
            emk_spoof::disarm();
            spoof_seq_ = 0;
        }
        const bool on = emk_spoof::active();
        if (on && spoof_seq_) {
            emlib::Entry ent;
            emk_quirk::Quirks quirks;
            if (!storage::lib_get(spoof_seq_, ent, quirks) || !emk_spoof::matches(ent.rec, quirks)) spoof_seq_ = 0;
        }
        out[0] = e; out[1] = on ? 1 : 0;
        put16(2, std::min<uint32_t>(emk_spoof::requests(), 0xFFFF)); put16(4, std::min<uint32_t>(emk_spoof::replies(), 0xFFFF));
        put16(6, on ? spoof_seq_ : 0);
        return 8;
    }
    }
}

remote::FcInfo Remote::fc_info() {
    const auto st = fctrig::status();
    remote::FcInfo i;
    i.state = static_cast<uint8_t>(st.state);
    i.proto = static_cast<uint8_t>(st.proto);
    i.fail = static_cast<uint8_t>(st.fail);
    i.target_mv = static_cast<uint16_t>(st.target_mv);
    i.vbus_mv = static_cast<uint16_t>(st.vbus_mv < 0 ? 0 : st.vbus_mv);
    i.scan_flags = static_cast<uint8_t>((fctrig::scan_running() ? 1 : 0) | (fctrig::scan_done() ? 2 : 0));
    i.scan_step = static_cast<uint8_t>(fctrig::scan_step());
    const auto res = fctrig::scan_results();
    const size_t n = std::min<size_t>(res.size(), i.results.size());
    for (size_t k = 0; k < n; ++k)
        i.results[k] = {static_cast<uint8_t>(res[k].proto), static_cast<uint8_t>(res[k].outcome),
                        static_cast<uint16_t>(res[k].mv < 0 ? 0 : res[k].mv)};
    i.num_results = static_cast<uint8_t>(n);
    return i;
}

// Streams the captured samples as 0x8B frames behind the response; waits for IN queue room like
// the screenshot streamer so no frame overwrites an unsent one.
void Remote::stream_ripple(uint8_t seq) {
    for (size_t off = 0; off < ripple_.size(); off += remote::kRippleChunk) {
        const uint32_t t0 = systick::millis();
        while (usb::tx_free() < kStreamMinFree) {
            usb::poll();
            if (systick::millis() - t0 > kStreamTimeoutMs) {
                K2LOG("[RMT] ripple stream stalled\n");
                return;
            }
        }
        std::array<uint8_t, remote::kFrameSize> frame;
        const size_t n = std::min(remote::kRippleChunk, ripple_.size() - off);
        remote::build_ripple_frame(frame, seq, static_cast<uint16_t>(off),
                                   std::span<const uint16_t>(ripple_.data() + off, n));
        usb::send_report(frame.data());
    }
}

bool Remote::screenshot_enabled() { return uiprefs::screenshot_enabled; }

remote::Status Remote::screenshot() {
    if (!screenshot_enabled()) return remote::Status::Busy;
    shot_pending_ = true;
    return remote::Status::Ok;
}

remote::ScreenInfo Remote::screen_info() { return {}; }

bool Remote::log_enabled() { return usb::log_enabled(); }

void Remote::set_log(bool on) {
    if (!on) K2LOG("[RMT] log off\n");
    usb::log_enable(on);
    if (on) K2LOG("[RMT] log on\n");
}

}  // namespace k2

void k2::usb::on_remote_report(const uint8_t *buffer, uint16_t length) {
    if (k2::g_instance) k2::g_instance->enqueue(buffer, length);
}

// The one handle_request<Remote>: http_server.cpp (built without LTO for the .mode_net overlay)
// declares it extern instead of carrying its own 4 KB copy.
template bool k2::remote::handle_request<k2::Remote>(std::span<const uint8_t>, k2::Remote &,
                                                     std::span<uint8_t, k2::remote::kFrameSize>);
