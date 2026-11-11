// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fake remote::Backend shared by the dispatcher and the HTTP API host tests.
#pragma once
#include "k2/core/remote_proto.hpp"
#include <array>
#include <string>
#include <string_view>
#include <vector>

using namespace k2::remote;

struct Fake {
    int calls = 0;
    uint8_t last_btn = 0xFF; uint16_t last_ms = 0;
    Status press_status = Status::Ok;
    Status page_status = Status::Ok;
    uint8_t last_action = 0, last_page = 0xFF;
    int reboots = 0;
    bool log_on = true;
    PdoSnapshot snap{};
    EmarkerSnapshot em{};

    PingInfo ping() { calls++; return {0x01020304, "abc1234", "1.0"}; }
    ChargerTestSnapshot test_results{};
    ChargerTestSnapshot charger_test_results() { calls++; return test_results; }
    ChargerTestView view{};
    ChargerTestView charger_test_view() { calls++; return view; }
    std::vector<uint8_t> units_set; Status units_status = Status::Ok;   // what charger_units() was given, and its answer
    Status charger_units(std::span<const uint8_t> u) { calls++; units_set.assign(u.begin(), u.end()); return units_status; }
    bool sweep_set = false; int sweep_sets = 0; Status sweep_status = Status::Ok;
    Status charger_sweep(bool on) { calls++; sweep_set = on; ++sweep_sets; return sweep_status; }
    bool avs_sweep_set = false; int avs_sweep_sets = 0; Status avs_sweep_status = Status::Ok;
    Status charger_avs_sweep(bool on) { calls++; avs_sweep_set = on; ++avs_sweep_sets; return avs_sweep_status; }
    k2::ctcable::Choice cable_set{}; int cable_sets = 0; Status cable_status = Status::Ok;   // what charger_cable() was given, and its answer
    k2::ctcable::Info cable_info{};
    ChargerCableReply charger_cable(const k2::ctcable::Choice* set) {
        calls++;
        if (set) { cable_set = *set; ++cable_sets; }
        return {cable_status, cable_info};
    }
    ChargerHuntReply hunt_reply{}; uint8_t hunt_asked = 0xFF;   // what charger_hunt() answers, and the slot it was last asked for
    ChargerHuntReply charger_hunt(uint8_t slot) { calls++; hunt_asked = slot; return hunt_reply; }
    Status press(uint8_t b, uint16_t ms) { calls++; last_btn = b; last_ms = ms; return press_status; }
    StateInfo state() {
        calls++;
        StateInfo s;
        s.page = 1; s.active_cc = 2; s.spec_rev = 2; s.num_pdos = 4; s.pd_valid = 1;
        s.chip = 3; s.pm_online = 1; s.log_on = log_on;
        s.vbus_mv = 20000; s.current_ma = -1500; s.power_mw = 0x00012345; s.energy_mwh = 7;
        s.dp_mv = 0x0123; s.dm_mv = 0x0456; s.cc1_mv = 0x0789; s.cc2_mv = 0x0ABC; s.aux_mv = 0x2345;
        s.temp_deci_c = -25; s.fc = 6; s.adc_flags = 11; s.mcu_temp_deci_c = 371;
        s.link_flags = kLinkStale | kLinkInline; s.age_s = 83; s.cable_age_s = 0x0102;
        s.reset_flags = (1u << 27) | (1u << 26);
        return s;
    }
    SessionInfo sess{};
    SessionInfo session() { calls++; return sess; }
    DiagInfo dg{};
    DiagInfo diag() { calls++; return dg; }
    KeysInfo ky{};
    KeysInfo keys() { calls++; return ky; }
    PdoSnapshot pdos() { calls++; return snap; }
    EmarkerSnapshot emarker() { calls++; return em; }
    IdentitySnapshot id{};
    IdentitySnapshot identity() { calls++; return id; }
    bool last_confirm = false;
    Status action(uint8_t id, bool confirm) { calls++; last_action = id; last_confirm = confirm; return Status::Ok; }
    Status set_page(uint8_t p) { calls++; last_page = p; return page_status; }
    void reboot() { calls++; reboots++; }
    int dfus = 0;
    uint8_t dfu_mode = 255;
    int faults = 0; uint8_t fault_kind = 255;
    void debug_fault(uint8_t kind) { calls++; faults++; fault_kind = kind; }
    void dfu_enter(uint8_t reset_mode) { calls++; dfus++; dfu_mode = reset_mode; }
    int wipes = 0;
    uint8_t wipe_mode = 255;
    FactoryWipeInfo wipe_snap{};
    FactoryWipeInfo factory_wipe(uint8_t reset_mode) { calls++; wipes++; wipe_mode = reset_mode; return wipe_snap; }
    int dump_calls = 0; uint32_t dump_addr = 0;
    void flash_read(uint32_t addr, std::span<uint8_t> out) {
        calls++; dump_calls++; dump_addr = addr;
        for (size_t i = 0; i < out.size(); i++) out[i] = static_cast<uint8_t>((addr + i) ^ 0x5A);
    }
    // BOOT_INFO: a held text (rendered on every offset-0 call, counted) and a list of samples
    std::string boot_line = "K2B1 t=1 vb=0"; int boot_renders = 0;
    std::vector<k2::bootinfo::PowerSample> boot_samples;
    uint16_t boot_text(bool refresh) { calls++; if (refresh) boot_renders++; return uint16_t(boot_line.size()); }
    void boot_text_read(uint16_t off, std::span<uint8_t> out) {
        for (size_t i = 0; i < out.size(); i++) out[i] = uint8_t(boot_line[off + i]);
    }
    uint16_t boot_sample_count() { calls++; return uint16_t(boot_samples.size()); }
    k2::bootinfo::PowerSample boot_sample(uint16_t i) { return boot_samples[i]; }
    Status shot_status = Status::Ok; int shots = 0;
    Status screenshot() { calls++; shots++; return shot_status; }
    bool shot_enabled = true;
    bool screenshot_enabled() { return shot_enabled; }
    ScreenInfo screen_info() { return {}; }
    int fake_n = -1; Status fake_status = Status::Ok;
    int link_arg = -1;
    Status debug_link(bool on) { calls++; link_arg = on; return Status::Ok; }
    int cc_matrix_arg = -1; int cc_modes_arg = -1;
    Status debug_cc_matrix(uint8_t v, uint16_t m) { calls++; cc_matrix_arg = v; cc_modes_arg = m; return Status::Ok; }
    int vconn_op = -1;
    VconnSnapshot debug_vconn(uint8_t op) { calls++; vconn_op = op; VconnSnapshot s; s.ok = 1; s.state = op; return s; }
    int epr_info_op = -1; EprInfoSnapshot epr_info_snap{};
    EprInfoSnapshot debug_epr_info(uint8_t op) { calls++; epr_info_op = op; return epr_info_snap; }
    k2::ccprobe::Result probe_res{}; Status probe_status = Status::Ok;
    int probe_sw0 = -1, probe_pc = -1, probe_ms = -1, probe_modes = -1;
    Status debug_cc_probe(const k2::ccprobe::Args &a, k2::ccprobe::Result &out) {
        calls++; probe_sw0 = a.sw0; probe_pc = a.pc; probe_ms = a.ms; probe_modes = a.modes;
        if (probe_status != Status::Ok) return probe_status;
        out = probe_res;
        return Status::Ok;
    }
    k2::raprobe::Result ra_res{}; Status ra_status = Status::Ok; int ra_pc = -1;
    Status ra_probe(const k2::raprobe::Args &a, k2::raprobe::Result &out) {
        calls++; ra_pc = a.vconn_pc;
        out = ra_res;
        return ra_status;
    }
    LcdSnapshot lcd_snap{};
    LcdSnapshot debug_lcd() { calls++; return lcd_snap; }
    LoopSnapshot loop_snap{}; unsigned loop_page = 255; bool loop_reset = false;
    LoopSnapshot debug_loop(uint8_t page, bool reset) { calls++; loop_page = page; loop_reset = reset; return loop_snap; }
    Status debug_fake_pdos(uint8_t n) { calls++; fake_n = n; return fake_status; }
    uint8_t msg_sop = 255; uint16_t msg_header = 0; std::array<uint8_t, 28> msg_body{}; size_t msg_len = 0;
    Status msg_status = Status::Ok;
    Status debug_pd_msg(uint8_t sop, uint16_t h, std::span<const uint8_t> pl) {
        calls++; msg_sop = sop; msg_header = h; msg_len = pl.size();
        std::copy(pl.begin(), pl.end(), msg_body.begin());
        return msg_status;
    }
    bool log_enabled() { return log_on; }
    void set_log(bool on) { calls++; log_on = on; }
    AdcRawSnapshot adc_raw() {
        calls++;
        AdcRawSnapshot a; a.ok = 1;
        for (size_t i = 0; i < a.all.size(); i++) a.all[i] = static_cast<uint16_t>(0x100 + i);
        for (size_t i = 0; i < a.scan.size(); i++) a.scan[i] = static_cast<uint16_t>(0x200 + i);
        return a;
    }
    uint8_t last_code = 0xFF; Status ripple_status = Status::Ok;
    Status ripple(uint8_t code) { calls++; last_code = code; return ripple_status; }
    RippleInfo ripple_info() { return {0x00030201, 512, 0x0010, 0x0FF0, 0x0800}; }
    uint8_t rr_op = 0xFF, rr_mask = 0xFF; int rr_calls = 0; RereadResult rr_result{};
    RereadResult reread(uint8_t op, uint8_t mask) { calls++; rr_calls++; rr_op = op; rr_mask = mask; return rr_result; }
    uint8_t ar_op = 0xFF; int ar_calls = 0; AppleReadResult ar_result{};
    AppleReadResult apple_read(uint8_t op) { calls++; ar_calls++; ar_op = op; return ar_result; }
    uint8_t id_op = 0xFF; IdentifyResult id_result{};
    IdentifyResult identify_device(uint8_t op) { calls++; id_op = op; return id_result; }
    uint8_t rq_pos = 0xFF; uint16_t rq_mv = 0, rq_ma = 0; bool rq_confirm = false, rq_over_load = false; PdOpResult rq_result{};
    PdOpResult pd_request(uint8_t pos, uint16_t mv, uint16_t ma, bool confirm, bool over_load) {
        calls++; rq_pos = pos; rq_mv = mv; rq_ma = ma; rq_confirm = confirm; rq_over_load = over_load; return rq_result;
    }
    int stops = 0; StopInfo stop_result{};
    StopInfo stop() { calls++; stops++; return stop_result; }
    // The dead-man lease (k2/core/lease.hpp): touches and arms are counted apart from `calls` (they are not backend work).
    int lease_touches = 0, lease_arms = 0, lease_calls = 0; uint8_t lease_op = 0xFF; uint16_t lease_secs = 0xFFFF; LeaseInfo lease_result{};
    void lease_touch() { lease_touches++; }
    void lease_arm() { lease_arms++; }
    LeaseInfo lease(uint8_t op, uint16_t secs) { calls++; lease_calls++; lease_op = op; lease_secs = secs; return lease_result; }
    int releases = 0, resets = 0;
    uint8_t reset_flags = 0xFF;
    PdOpResult pd_release() { calls++; releases++; return {}; }
    PdOpResult pd_hard_reset(uint8_t flags = 0) { calls++; resets++; reset_flags = flags; return {}; }
    int hb_calls = 0;
    uint8_t hb_flags = 0xFF;
    HandbackResult hb_result{};
    HandbackResult pd_handback(uint8_t flags = 0) { calls++; hb_calls++; hb_flags = flags; return hb_result; }
    uint8_t trig_pin = 0;   // what pd_trigger() reports as the pinned Rd line
    uint8_t trig_line = 0, trig_moves = 0, trig_auto = 0;   // the direct-plug line trial pd_trigger() reports (line under trial, Rd moves)
    PdTriggerSnapshot pd_trigger() {
        calls++;
        PdTriggerSnapshot t{5, 1, 1, 0, 4, 0x4E20, 0x0BB8, 0x01020304, 0x05060708};
        t.pin = trig_pin;
        t.trial_line = trig_line;
        t.trial_moves = trig_moves;
        t.auto_hold = trig_auto;
        return t;
    }
    uint16_t attach_mv = 0, attach_ma = 0; bool attach_confirm = false; int attaches = 0; PdOpResult attach_verdict{};   // a non-Ok verdict is a refusal: status + err byte, no snapshot
    bool attach_raised = false;   // the accepted attach started / holds a contract above 5 V (false: only Rd, no PDO, a failed request)
    PdAttachResult pd_attach(uint16_t target_mv, uint16_t target_ma, bool confirm) {
        calls++; attach_mv = target_mv; attach_ma = target_ma; attach_confirm = confirm; attaches++;
        return {attach_verdict.status, attach_verdict.err, pd_trigger(), attach_raised};
    }
    int fc_calls = 0; uint8_t fc_op = 0xFF, fc_proto = 0xFF; uint16_t fc_mv = 0xFFFF; uint8_t fc_flags = 0;
    Status fc_status = Status::Ok;
    Status fc_cmd(uint8_t op, uint8_t proto, uint16_t mv, uint8_t flags) {
        calls++; fc_calls++; fc_op = op; fc_proto = proto; fc_mv = mv; fc_flags = flags; return fc_status;
    }
    int store_calls = 0; uint8_t store_op = 0xFF, store_arg = 0xFF;
    StoreInfo store_cmd(uint8_t op, uint8_t arg) {
        calls++; store_calls++; store_op = op; store_arg = arg;
        StoreInfo i; i.err = 0; i.count = 3; i.flags = 5; i.slot = 2; i.log_len = 0x0102; i.bank = 2; i.native = 4;
        return i;
    }
    FcInfo fc_info() {
        FcInfo i; i.state = 2; i.proto = 1; i.fail = 0; i.scan_flags = 1; i.scan_step = 3;
        i.target_mv = 9000; i.vbus_mv = 8950; i.num_results = 2;
        i.results[0] = {4, 1, 9010}; i.results[1] = {5, 2, 0};
        return i;
    }
    int cable_resets = 0;
    void cable_r_reset() { calls++; cable_resets++; }
    int cable_kelvin = -1;   // last mode selected: 0 passive, 1 kelvin
    void cable_r_mode(bool kelvin) { calls++; cable_kelvin = kelvin ? 1 : 0; }
    UsbLinkSnapshot usb_link() {
        UsbLinkSnapshot s; s.family = 5; s.tbt = 2; s.conf = 3; s.source = 4; s.flags = 1; s.mbps = 40000;
        const char t[] = "USB4 40G TBT4 (neg)"; for (unsigned i = 0; i + 1 < sizeof t; i++) s.text[i] = t[i];
        s.text_len = sizeof t - 1; return s;
    }
    MatchSnapshot match_info() {
        MatchSnapshot m; m.caps_seen = 1; m.request_seen = 1; m.flags = 2; m.verdict = 3; m.rows = 2; m.want_pos = 2;
        m.want_mv = 0x2EE0; m.want_ma = 0x0BB8; m.need_ma = 0x0BB8; m.have_ma = 0x0384;
        m.src_max_mv = 0x3A98; m.src_max_w = 0x0064; m.row[0] = 3; m.row[1] = 1; return m;
    }
    uint8_t lab_duty = 0; uint16_t lab_ms = 0;
    LabResult lab_load(uint8_t duty, uint16_t ms) {
        calls++; lab_duty = duty; lab_ms = ms; LabResult r; r.reason = 6; r.duty = duty; r.ms = ms; r.peak_ma = 120; return r;
    }
    uint8_t sniff_sub = 0xFF;
    SniffInfo lab_sniff(uint8_t sub) { calls++; sniff_sub = sub; SniffInfo r; r.state = 2; r.reason = 3; r.flags = 2; r.edges = 0x0123; return r; }
    // --- vooc-monitor (k2/vooc-detect) ---
    int vooc_set = -1;                  // last switch argument (2 = keep)
    VoocSnapshot vooc_snap{};
    VoocSnapshot vooc_monitor(uint8_t set) {
        calls++; vooc_set = set;
        if (set <= 1) vooc_snap.enabled = set == 1;
        return vooc_snap;
    }
    // --- end vooc-monitor ---
    // UFCS tuner / probe: a RAM field array standing in for the tuner, a canned blob for the read op
    std::uint32_t ufcs_vals[k2::ufcs::kFieldCount] = {};
    Status ufcs_busy = Status::Ok;      // what set / ops answer when forced
    UfcsInfo ufcs_state{};
    std::vector<uint8_t> ufcs_blob;
    std::vector<uint8_t> ufcs_ops;      // every ufcs_op code seen
    UfcsInfo ufcs_info() { calls++; UfcsInfo i = ufcs_state; i.blob_len = uint16_t(ufcs_blob.size()); return i; }
    std::uint32_t ufcs_get(uint8_t f) { calls++; return ufcs_vals[f]; }
    Status ufcs_set(uint8_t f, std::uint32_t v) { calls++; if (ufcs_busy != Status::Ok) return ufcs_busy; ufcs_vals[f] = v; return Status::Ok; }
    Status ufcs_op(uint8_t op) { calls++; ufcs_ops.push_back(op); return ufcs_busy; }
    size_t ufcs_read(uint16_t off, std::span<uint8_t> out) {
        calls++;
        size_t n = 0;
        for (; n < out.size() && off + n < ufcs_blob.size(); n++) out[n] = ufcs_blob[off + n];
        return n;
    }
    // PD Stress (Cmd::PdStress): a canned status block and result blob; the start options and stops are recorded
    PdStressInfo ps_info{};
    std::vector<uint8_t> ps_blob;
    std::vector<PdStressStart> ps_starts;
    Status ps_start_status = Status::Ok;
    int ps_stops = 0;
    PdStressInfo pdstress_info() { calls++; PdStressInfo i = ps_info; i.blob_len = uint16_t(ps_blob.size()); return i; }
    Status pdstress_start(const PdStressStart &s) { calls++; ps_starts.push_back(s); return ps_start_status; }
    void pdstress_stop() { calls++; ps_stops++; }
    size_t pdstress_read(uint16_t off, std::span<uint8_t> out) {
        calls++;
        size_t n = 0;
        for (; n < out.size() && off + n < ps_blob.size(); n++) out[n] = ps_blob[off + n];
        return n;
    }
    // Loop probe (Cmd::LoopProbe): the stored result file, empty = none.
    std::vector<uint8_t> lp_file;
    uint16_t loopprobe_len() { calls++; return uint16_t(lp_file.size()); }
    size_t loopprobe_read(uint16_t off, std::span<uint8_t> out) {
        calls++;
        size_t n = 0;
        for (; n < out.size() && off + n < lp_file.size(); n++) out[n] = lp_file[off + n];
        return n;
    }
    // QrTest (Cmd::QrTest): the dispatcher validates and builds the list itself; the backend only opens/pops the page.
    Status qrtest_status = Status::Ok;
    std::array<uint8_t, 3> qrtest_shown{}; bool qrtest_show_called = false; bool qrtest_stopped = false;
    Status qrtest_show(uint8_t version, uint8_t px, uint8_t quiet) {
        calls++; qrtest_show_called = true; qrtest_shown = {version, px, quiet};
        return qrtest_status;
    }
    bool qrtest_stop() { calls++; qrtest_stopped = true; return qrtest_stopped_value; }
    bool qrtest_stopped_value = false;
    CableRInfo cable_r_info() { return {3, 1, 0x0064, 5100, 40, 4900, 2050, 0x0032, 0x0046}; }
    uint8_t bb_op = 0xFF; uint32_t bb_arg = 0xFFFFFFFF;
    size_t blackbox(uint8_t op, uint32_t arg, std::span<uint8_t> out) {
        calls++; bb_op = op; bb_arg = arg;
        out[0] = 0xA5; out[1] = 0x78; out[2] = 0x56; out[3] = 0x34; out[4] = 0x12;
        return 5;
    }
    bool ports_busy_value = false;   // a charger test or an FC release owns the link
    bool ports_busy() { return ports_busy_value; }
    std::vector<uint8_t> em_req;
    size_t emlib(std::span<const uint8_t> req, std::span<uint8_t> out) {
        calls++; em_req.assign(req.begin(), req.end());
        out[0] = 0xE0; out[1] = 0x11; out[2] = 0x22;
        return 3;
    }
    std::vector<uint8_t> tc_req;
    size_t transcap(std::span<const uint8_t> req, std::span<uint8_t> out) {
        calls++; tc_req.assign(req.begin(), req.end());
        out[0] = 0x7C; out[1] = req[0];
        return 2;
    }
    std::vector<uint8_t> cards_req;
    size_t cards(std::span<const uint8_t> req, std::span<uint8_t> out) {
        calls++; cards_req.assign(req.begin(), req.end());
        out[0] = 0xC4; out[1] = 0x33;
        return 2;
    }
    std::vector<uint8_t> twin_req;
    uint8_t twin_first = 0;   // what the fake answers in out[0] (0 = accepted)
    size_t twin(std::span<const uint8_t> req, std::span<uint8_t> out) {
        calls++; twin_req.assign(req.begin(), req.end());
        out[0] = twin_first; out[1] = req[0];
        return 2;
    }
};
