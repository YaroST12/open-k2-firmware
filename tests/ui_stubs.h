// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include "k2/drivers/usbcfg.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/chargertest/cablepref.hpp"
#include <stdint.h>
#include <optional>
#include <vector>
#include <string>
#include "k2/fastcharge/fc_scan.hpp"
#include "k2/fastcharge/ufcs_probe.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/pd/usb_link.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/core/storage.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/chargertest/charger_journal.hpp"
#include "k2/core/rd_intent.hpp"
#include "k2/core/rescan.hpp"
#include "k2/pd/pd_survey.hpp"
#include "k2/twin/twin_hw.hpp"
#include "k2/pd/diag.hpp"
#include <initializer_list>
#include <utility>
struct StubState {
    uint32_t ms = 1000;
    // The charger-test journal as the boot finds it (load() answers journal_load) and what the page wrote back.
    k2::charger_journal::Replay journal{};
    bool journal_load = false, rd_inherited = false;
    int journal_finishes = 0, journal_resumes = 0, journal_attempts = 0, journal_metas = 0, rd_releases = 0;
    int bb_holds = 0;   // bbpersist::hold_run() calls: a live charger-test run renews it every tick
    int bb_make_rooms = 0;   // bbpersist::make_room() calls: START frees the journal's blocks before Rd goes on
    // The journal's polled jobs (arm = rows and header of a run start, replan = the changed rows). A job needs `arm_pumps` /
    // `replan_pumps` pump() calls to finish (0 = one per row plus the header, as the real one writes them).
    int journal_arms = 0, journal_pumps = 0, journal_cancels = 0, journal_replan_starts = 0;
    int arm_pumps = 0, replan_pumps = 3;
    bool journal_job = false, journal_job_arm = false, journal_arm_open = false;   // a job runs / it is the arm / arm or meta still due
    int journal_job_left = 0;
    // The noted-but-unwritten state of the real journal: rows whose result is to go into a section file, and a plan whose section
    // files differ from the flash (a replan job: `replan_pumps` pump() calls, one file each, then the header).
    std::vector<size_t> journal_noted;
    bool journal_plan_dirty = false;
    int journal_replan_left = 0;
    int journal_writes_in_arm = 0;   // step / phase / replan writes between arm_start() and meta(): there must be none
    std::string journal_log;         // one letter per hw call: A arm_start, P pump, H arm done, M meta, F phase, S step, R replan done, N finish
    int journal_skips = 0, journal_skips_before_finish = 0;   // Skip rows journaled, and how many came before any Finish
    std::vector<size_t> journal_skip_rows_before_finish;      // which rows those were
    // Every journal write (step / replan / phase) and how many of them landed while the probe said a row was in flight.
    // A test points `journal_quiet` at the page's Runner::quiet(); null = not probing.
    int journal_writes = 0, journal_writes_quiet = 0, journal_phase_writes = 0, journal_replans = 0;
    bool (*journal_quiet)() = nullptr;
    std::vector<uint8_t> journal_verdicts;   // the last verdict journaled for each step index (0xFF = never written)
    k2::charger_journal::RunMeta journal_meta{};   // the last meta the page journaled
    // What the scratch arena looked like while the boot replayed the journal: the holder's tag (null = none) and whether the
    // Replay the caller handed over lies in the arena. The Replay is too big to keep in .bss: it is transient.
    const char* journal_load_owner = nullptr;
    bool journal_load_in_arena = false;
    k2::pd::SourceCaps caps{};
    k2::pd::Emarker em{};
    k2::pd::CableLimits cable_limits{};
    k2::pd::Revision source_revision{};
    k2::pd::VdmInfo vdm{};
    k2::pd::VdmInfo sink_vdm{};
    k2::pd::AppleInfo apple{};             // what fusb302::apple_info() returns (the lab-only readout's last run)
    bool cable_tb = false;
    k2::emk_variant::Answered cable_ans{};   // what fusb302::cable_answered() returns (the Cable tab's Answered row)
    k2::raclass::PullDown pull_down = k2::raclass::PullDown::NotProbed;   // what fusb302::pull_down() returns (a silent cable's chip)
    k2::emk_attached::ReadInfo cable_read{};                              // what fusb302::last_cable_read() returns
    k2::usblink::Link ul{};               // what fusb302::usb_link() returns (the USB / Data rows of the Connection page)
    uint8_t cc = 1;
    k2::pd::Contract contract{};
    std::uint32_t req_period_ms = 0;      // what fusb302::request_period_ms() returns
    k2::pd::SinkCapsSeen sink_seen{};
    k2::pd::XiaomiTrace xm{};             // the sink's Request / Xiaomi chain record (fusb302::xiaomi_trace)
    k2::pd::SourceCaps epr_info_caps{};   // the list an informational EPR_Get_Source_Cap returned
    int epr_info_asks = 0; bool epr_info_forced = false; k2::pd::Err epr_info_err = k2::pd::Err::Ok;
    bool far_cc = false;                  // fusb302::sink_cc_far
    bool link_inline = false, inl = false, scan_confirmed = false, self_powered = false;
    k2::raguard::WaitKind wait_kind = k2::raguard::WaitKind::Unknown;   // fusb302::wait_kind / set_wait_kind
    bool caps_this_attach = false;        // fusb302::caps_this_attach()
    uint8_t pin = 0;                         // fusb302::pin_rd() override (0 = derive from self_powered: CC2)
    uint8_t trial_line = 0, trial_moves = 0; bool trial_live = false;   // fusb302::line_trial(): the direct-plug fallback's line / moves / live
    bool bus_powered = false, supply_decided = true;   // power_source: no host and VBUS at boot / the host window is over
    bool usb_configured = true;              // usb::configured(): a host enumerated the HID link (the reboot world has none)
    bool rd_run = false, vbus_on_rd = false, pm_after_rd = false;
    bool reread_hold = false;                // fusb302::reread_own_hold(): a finished re-read's own leftover PDO 1 hold
    int reread_take_overs = 0;               // fusb302::reread_take_over(): a tool (PD Stress, twin run) took the re-read's attach over
    std::uint8_t idle_wake_hold = 0;         // fusb302::idle_wake_line(): the Wake sleeping port's Rd hold
    std::int32_t open_cc_mv[2] = {-1, -1};   // fusb302::open_cc_mv(): the settled open readings of CC1 / CC2
    int rd_run_changes = 0;
    int rd_pauses = 0;       // rd_for_run_pause(): Rd off that keeps the scanned cable
    bool ra_on = false, ra_refuse = false;   // fusb302::ra_set(): the Ra mode, and a forced refusal
    int ra_ons = 0, ra_offs = 0;             // ra_set(true) / (false) that changed the mode
    int ra_owned = 0;                        // ra_run_own() calls that found the Ra mode on (a run took it over)
    // The Rd comeback (k2/core/rd_comeback.hpp) and the planned re-attach's Rd gap (fusb302::rd_glitch_begin/end).
    k2::rdintent::Intent rdc_found{};        // rdcomeback::found(): what the boot's marker said (None = an ordinary boot)
    k2::ctcable::Choice rdc_run_cable{};     // the last rdcomeback::set_run_cable()
    int rdc_run_sets = 0, rdc_begins = 0, rdc_ends = 0, rdc_drops = 0;
    bool rdc_begin_ok = true;                // begin_reattach()'s answer: the record is on the volume
    k2::ctcable::Choice rdc_begin_cable{};   // the identity begin_reattach() was asked to record
    int ra_at_first_rd = -1;                 // 1 when the Ra mode was already on at the run's first Rd edge, 0 when not, -1 no Rd yet
    int glitch_begins = 0, glitch_ends = 0;  // rd_glitch_begin / rd_glitch_end
    int ra_ons_at_glitch = -1;               // ra_ons when the first gap opened: the Ra went on BEFORE the Rd edge
    bool ra_on_at_glitch = false;
    int force_line = 0;                      // fusb302::set_force_line()
    bool run_owner = false;                  // fusb302::set_run_owner(): a charger-test run owns the line (the idle wake stays off)
    bool idle_wake = false;                 // fusb302::set_idle_wake(): Settings > Wake sleeping port, applied now
    int unit_detaches = 0, unit_attaches = 0, unit_holds = 0; bool legacy_attach = false;   // charger test units
    bool pd_chatter = false;        // a Legacy attach whose charger never goes PD-quiet
    bool model_units = true; int32_t unit_vbus_mv = 5000;   // detach drops VBUS, attach restores it (and a 5 V contract)
    uint32_t delays_ms = 0;
    void (*delay_hook)() = nullptr;   // runs inside delay_ms, like usb::service() runs remote presses on the K2
    int rescans = 0, scans = 0, new_sources = 0, clear_sources = 0, cc_detaches = 0;
    // Refresh (core/rescan.hpp): what the stub request() answers and how often the page asked; the driver half of it
    int rescan_requests = 0; k2::rescan::Result rescan_result = k2::rescan::Result::Passive;
    int refresh_asks = 0; k2::pd::Err refresh_err = k2::pd::Err::Ok; bool legacy_active = false;
    int rdc_rescan_begins = 0; bool rdc_rescan_ok = true, rdc_rescan_landing = false;
    // The survey (core/survey.hpp): the page reads this table; tests fill it through start()/set(), on_refresh() is counted.
    k2::pd::Survey survey; int survey_refreshes = 0; k2::rescan::Result survey_last = k2::rescan::Result::Busy;
    k2::pd::EmarkerMode last_scan_mode = k2::pd::EmarkerMode::Vconn;
    bool scan_result = true;           // what read_emarker() returns
    void (*on_scan)() = nullptr;   // called inside read_emarker: lets a test capture the scanning screen
    std::uint32_t session = 1;         // fusb302::session()
    std::uint32_t cable_seq = 0;       // fusb302::cable_seq()
    k2::power::Metrics pm{};
    bool pm_online = true;
    int32_t vbus_ceiling_mv = 0;  // > 0: a request moves VBUS to its target, clamped here (a converter that stops short); a release goes back to 5 V
    bool pm_fast_set = false;     // pm_fast_mv overrides power::vbus_fast_mv(); unset it follows pm.vbus_mv
    int32_t pm_fast_mv = 0;       // pm is the 2.5 s average, this the un-averaged latest sample
    bool pm_display_set = false;       // pm_display is what power::read_display() returns; unset it follows pm
    k2::power::Metrics pm_display{};   // the One Euro filtered view; pm above is the 2.5 s average
    k2::power::Chip power_chip = k2::power::Chip::Ina226;
    std::optional<k2::adc::Reading> adc;
    k2::fastcharge::Protocol fc = k2::fastcharge::Protocol::Pending;
    k2::vooc::Live vooc{};             // fastcharge::vooc_live(): the inline VOOC monitor's live view
    int polls = 0;
    k2::pd::SinkStatus sink{};
    k2::pd::IdStatus ident{};
    k2::pd::ProbeStatus probe{};
    k2::pd::ExtInfo ext{};
    int probes = 0; bool probe_ok = true;   // probe_start() calls and its answer
    k2::pd::Err request_err = k2::pd::Err::Ok;
    k2::pd::Choice last_choice{};
    bool last_confirmed = false, last_over_load = false;
    int requests = 0, releases = 0, hard_resets = 0, getcaps = 0, identifies = 0;
    k2::pd::TriggerRefusal trig_refusal = k2::pd::TriggerRefusal::None;   // fusb302::trigger_refusal(): a sink sits behind the K2
    uint32_t sink_cap = 0;
    int32_t sink_vbus = 0;
    bool identify_ok = true;
    bool pd_traffic = false;      // fusb302::pd_traffic(): a PD message was seen this attach
    bool pd_seen_attach = false;  // fusb302::pd_seen_attach(): the same, cleared only by a new attach
    k2::pd::OverLoadFacts ol_facts{};   // fusb302::over_load_facts(): .far_pd_message = the sink answered this attach
    std::uint32_t typec_ma_val = 0;     // fusb302::typec_ma(): the source's advertised Rp current
    bool ident_current = false;   // identify_current(): the stored Identify answers belong to this attach
    int fc_stops = 0, fc_scans = 0;
    int fc_starts = 0, fc_inline_ok = 0;   // fctrig::start calls, and those carrying the override
    int fc_start_mv = 0;
    bool fc_confirmed = false;
    int fc_start_fail = 0;       // k2::fc::Fail returned by fctrig::start while non-zero
    int fc_retargets = 0, fc_retarget_mv = 0, fc_retarget_fail = 0;   // fctrig::set_target calls
    int max_vbus_mv = 36000;     // power::max_vbus_mv() of the stub (INA226)
    int fc_state = 0;            // k2::fc::State returned by the fctrig stub
    k2::fc::Caps fc_caps;        // fctrig::caps() of the stub: what the SCP / UFCS sessions read
    bool fc_critical = false;    // fctrig::paint_critical(): an engine window the idle hook does not service
    bool fc_open_on_start = false;   // fctrig::start opens an engine window (fc_critical) at once for a protocol Engine::blocks_paint names (FCP, SCP, VOOC, UFCS, VIVO; not the QC handshake)
    void (*fc_start_hook)(int proto) = nullptr;   // called at the top of fctrig::start: what the screen holds as the engine is about to start
    bool fc_answers = false;    // fctrig::start reaches Active (the charger answered); false stays Negotiating forever
    int fc_proto = 0, fc_target_mv = 0;   // the fctrig stub's status protocol and target
    bool fc_scanning = false;    // fctrig::scan_running
    int fc_scan_step = 0;
    k2::ufcs::Tuner ufcs_tuner{};            // fctrig::ufcs_tuner(): the stub's RAM tuner
    bool ufcs_probe_run = false;             // fctrig::ufcs_probe_running(): a probe "runs" until the test clears it
    int ufcs_probe_starts = 0, ufcs_probe_stops = 0;
    int ufcs_probe_refuse = 0;               // k2::ufcs::ProbeStart returned by ufcs_probe_start while non-zero
    std::vector<uint8_t> ufcs_blob;          // what ufcs_probe_blob() returns
    k2::ufcs::ProbeResult ufcs_result{};     // what ufcs_probe_result() returns
    k2::fc::Scanner *reader_scanner = nullptr;  // real UI -> scanner -> fake charger seam
    std::vector<k2::fc::ScanResult> fc_scan_res;   // fctrig::scan_results
    std::vector<k2::emlib::Entry> lib;    // storage::lib_*: the stub library, numbers 1.. in order
    k2::emlib::Err lib_err = k2::emlib::Err::Ok;   // forced error for lib_save_current / lib_remove
    int lib_saves = 0, lib_removes = 0;
    std::uint32_t spoof_requests = 0;
    bool spoof_on = false;
    int spoof_arms = 0, spoof_disarms = 0;
    uint32_t spoof_generation = 0;
    k2::emrec::Record spoof_record{};
    k2::emk_quirk::Quirks spoof_quirks{};                    // what arm() was given
    std::vector<std::pair<std::uint16_t, k2::emk_quirk::Quirks>> lib_quirks;   // per saved clone (seq -> table)
    k2::emk_spoof::Err spoof_err = k2::emk_spoof::Err::Ok;
    k2::usbcfg::Setting usb_stored{};     // usbcfg::stored() / set()
    int usb_sets = 0;
    k2::uiprefs::Prefs prefs{};           // uiprefs::stored() / set()
    int prefs_sets = 0;
    k2::ctcable::Choice cable_pref{};     // cablepref::stored() / set(): the remembered cable selection, kept across a stub "reboot"
    int cable_pref_sets = 0;
    bool cable_pref_ok = true;            // false = flash refuses the write
    k2::smoothing::Preset smoothing_applied = k2::smoothing::kDefault;   // power::set_smoothing()
    bool usb_set_ok = true;
    // transition capture stub (k2::transcap): the calls the charger test makes, and a stored capture to find
    k2::transcap::State tc_state = k2::transcap::State::Off;
    k2::transcap::ArmResult tc_arm_result = k2::transcap::ArmResult::Ok;
    bool tc_freeze_on_arm = false;   // an earlier event (the detach of the cut) freezes the capture before the step's verdict
    int tc_arms = 0, tc_disarms = 0, tc_triggers = 0, tc_keeps = 0;
    int tc_arm_pos = 0, tc_arm_mv = 0;
    k2::transcap::Reason tc_reason = k2::transcap::Reason::None;
    uint32_t tc_keep_run = 0;
    int tc_keep_step = -1;
    bool tc_has_meta = false;             // find_stored() answers with tc_meta (run / step must match)
    k2::transcap::Meta tc_meta{};
    std::vector<std::vector<uint8_t>> reports;
    void (*on_report)() = nullptr;
    void (*on_log)(const char* line) = nullptr;   // every formatted K2LOG line, uncapped (the world-replay test keeps them timed)
    std::vector<uint8_t> bin_log;          // usb::log_bin(): the K2LOG entries the stub ring holds (whole entries, oldest first)
    uint32_t bin_base = 0;
    bool bin_older = false;
    std::string log_text;                  // usb::log_fmt() text (K2LOG on the host builds), capped; tests search it for log lines
    std::string log_fmts;                  // the raw format strings K2LOG was called with, capped; tests assert fixed text lives in the format, not in a %s
    // The device twin library (storage::twin_*, numbers 1..) and the live proof (twin_hw::live_*), simulated.
    std::vector<std::pair<uint8_t, k2::twin::Twin>> twins;
    unsigned twin_gen = 0;                 // bumped by a save / remove / rename, like storage::generation()
    int twin_learns = 0;                   // learn_from_identify() calls
    k2::twin_hw::Learned twin_learned{};   // what learn_from_identify answers; an Ok one also adds its twin to `twins`
    k2::twin::live::Refusal twin_refusal = k2::twin::live::Refusal::None;   // forced answer of live_start (None = the real runner decides)
    bool twin_reject = false;              // the simulated charger answers the next Request with Reject
    int twin_starts = 0, twin_cancels = 0; // live_start() calls the runner accepted, live_cancel() calls
    std::vector<uint8_t> twin_start_seqs;  // the numbers of the last live_start()
    bool twin_start_confirmed = false;     // ... and its confirm flag
    unsigned twin_stamp = 0;               // moves the prediction stamp (twin_hw::predict_stamp) like a new charger list would
    // PD Stress: Soft_Reset asks and the journal file (journal_hw::store()) the stand-in keeps in RAM.
    int ps_soft_resets = 0;
    bool ps_soft_reset_ok = true;
    std::vector<uint8_t> ps_file;
    bool ps_file_present = false, ps_write_fails = false;
    int ps_writes = 0, ps_removes = 0;
    // The HID-cable loop guard (k2/core/loop_guard.hpp): loopguard::locked() answers this.
    bool loop_locked = false;
    // The loop probe: what its Env reads (loopprobe::hw::env()) and its result file (loopprobe::hw::store()), kept in RAM.
    bool lp_pm_ok = true;
    int32_t lp_vbus_mv = 5050, lp_aux_mv = 5010;
    std::vector<uint8_t> lp_file;
    bool lp_file_present = false;
    int lp_writes = 0, lp_cc_holds = 0, lp_spins = 0;
    // The Black Box diagnosis (pd::diag::live): the report the screens read and what it is about (0 none, 1 open, 2 last).
    k2::pd::diag::Report diag_report{};
    uint8_t diag_state = 0;
};
extern StubState g_stub;
void stub_cards_reset();   // empties the RAM-backed report card store
// Adds a twin to the stub library (the sink PDO words are the device's own); returns its number.
uint8_t stub_twin_add(const char *name, std::initializer_list<uint32_t> sink_pdos, uint16_t vid = 0, uint16_t pid = 0);
void stub_twin_reset();    // empties the library and the simulated proof (a fresh g_stub does the library; this the runner)
