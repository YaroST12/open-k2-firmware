// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Firmware side of the remote-control channel: queues HID OUT reports, runs the commands from
// the main loop and injects virtual button presses. Protocol and dispatcher: remote_proto.hpp.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "k2/core/bss_zero.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/core/remote_proto.hpp"
#include "k2/ui/ui.hpp"
#include "k2/drivers/virtual_button.hpp"

namespace k2 {

enum class RemoteEvent : uint8_t { None, Reboot, Screenshot, Dfu, Fault };

// Ui::IdResult on the wire for Cmd::IdentifyDevice: the frame header carries the Status, the
// payload one byte with the pd::Err of a refusal (like the PD ops), so the host tool can name it.
// The inline refusal answers pd::Err::Inline; a running charger test stays Status::Busy.
struct IdRefusalWire { remote::Status status = remote::Status::Ok; uint8_t err = 0; };
constexpr IdRefusalWire identify_refusal_wire(Ui::IdResult r) {
    switch (r) {
    case Ui::IdResult::Refused:  return {remote::Status::Busy, 0};    // a charger test is running
    case Ui::IdResult::Inline:   return {remote::Status::BadArg, static_cast<uint8_t>(pd::Err::Inline)};
    case Ui::IdResult::NoDevice: return {remote::Status::BadArg, 0};
    default:                     return {remote::Status::Ok, 0};      // Started: not a refusal
    }
}

class Remote {
public:
    explicit Remote(Ui &ui);   // also registers this object as the target of usb::on_remote_report()

    // Main loop, once per pass: answers every queued request. Returns Reboot after the response
    // to a REBOOT command has been queued; the caller then performs the same reset sequence as
    // a long BACK press.
    RemoteEvent poll();

    // Virtual pin level of a button: true while an injected press is active. The caller ORs this
    // with the physical pin before the debouncer, so injected presses take the same path.
    bool button_down(Button b, uint32_t now_ms);

    // Queue one report from the USB stack (called through usb::on_remote_report()). The route (HID or the WebUSB vendor
    // pipe) is remembered with the request: poll() makes it the reply route before the command runs, so the response and
    // whatever stream the command starts go back the way the request came.
    void enqueue(const uint8_t *data, uint16_t len, usb::Route route = usb::Route::Hid);
    uint8_t free_slots() const;

    // Backend interface for remote::handle_request (public so the dispatcher template can call it).
    remote::PingInfo ping();
    remote::ChargerTestSnapshot charger_test_results() const;
    remote::ChargerTestView charger_test_view();   // the wizard / Waiting / Running screens as data (0x0B req [3])
    remote::ChargerTestStartReply charger_test_start(uint8_t flags);   // 0x0B req [4, flags]: the device's own START (confirmed)
    uint8_t charger_test_cancel();                                     // 0x0B req [5]: the device's own stop; remote::kCtStop*
    remote::Status charger_units(std::span<const uint8_t> units);   // unit order of the next charger run (empty = default)
    remote::Status charger_sweep(bool on);   // the PPS deep sweep of the next charger run (on by default; off = opt-out)
    remote::Status charger_avs_sweep(bool on);   // the AVS deep sweep of the next charger run (opt-in)
    remote::Status charger_level(uint8_t level); // the level of the next charger run (charger_test::Level, below kLevelCount)
    remote::ChargerCableReply charger_cable(const ctcable::Choice* set);   // cable identity of the START dialog (null = read)
    remote::ChargerSvoocCapReply charger_svooc_cap(const uint8_t* set);    // SVOOC ceiling stage: mode of the next run (null = read) and the latest result
    remote::ChargerHuntReply charger_hunt(uint8_t slot) const;   // retired: always count 0 (the 6 A hunt rows are gone)
    remote::Status press(uint8_t button, uint16_t ms);
    remote::StateInfo state();
    [[gnu::noinline]] remote::SessionInfo session();   // GET_STATE part 1: Ui::session() on the wire (a read)
    [[gnu::noinline]] remote::DiagInfo diag();   // GET_STATE part 2: the Diag page counters (a read)
    [[gnu::noinline]] remote::KeysInfo keys();   // GET_STATE part 3: the strip labels of the page on screen (a read)
    remote::CalibrationInfo calibration_info();  // GET_STATE part 4: settings EEPROM calibration trims
    remote::PdoSnapshot pdos();
    remote::EmarkerSnapshot emarker();
    remote::IdentitySnapshot identity();
    remote::Status action(uint8_t id, bool confirm);
    remote::Status set_page(uint8_t page);
    void reboot();
    void dfu_enter(uint8_t reset_mode);   // DFU_ENTER confirmed: poll() returns Dfu after the response
    // FACTORY_WIPE: runs the wipe now (seconds, blocks the loop); only when it fully succeeded does
    // poll() return Dfu after the response, otherwise the K2 stays in the app.
    remote::FactoryWipeInfo factory_wipe(uint8_t reset_mode);
    // FLASH_DUMP: copies memory-mapped flash into `out`; read only. The dispatcher has already limited the
    // window to 0x080E0000..0x08100000.
    void flash_read(uint32_t addr, std::span<uint8_t> out);
    // BOOT_INFO: the K2B1 text is rendered when refresh is set and held in a RAM buffer; the length comes back. See k2/core/bootinfo.hpp.
    uint16_t boot_text(bool refresh);
    void boot_text_read(uint16_t off, std::span<uint8_t> out);
    uint16_t boot_sample_count();
    bootinfo::PowerSample boot_sample(uint16_t i);
    void debug_fault(uint8_t kind);   // DEBUG_FAULT: poll() returns Fault after the response
    uint8_t fault_kind() const { return fault_kind_; }
    uint8_t dfu_reset_mode() const { return dfu_mode_; }   // 0 sys, 1 iwdg
    remote::Status screenshot();                // Busy while Settings > Screenshot is off or a capture is active
    void screenshot_opts(uint8_t flags) {
        shot_want_cdc_ = (flags & remote::kShotOptCdc) != 0;
        shot_want_packed_ = (flags & remote::kShotOptCdcPacked) != 0;
    }
    uint8_t screenshot_transport() const { return shot_transport_; }
    bool screenshot_enabled();
    remote::ScreenInfo screen_info();
    remote::Status debug_fake_pdos(uint8_t n);
    remote::Status debug_link(bool attached);
    remote::Status debug_pd_msg(uint8_t sop, uint16_t header, std::span<const uint8_t> payload);
    remote::Status debug_cc_matrix(uint8_t v, uint16_t modes);
    remote::Status debug_cc_probe(const ccprobe::Args &a, ccprobe::Result &out);   // passive only, blocks a.ms; Busy for any other state
    remote::Status ra_probe(const raprobe::Args &a, raprobe::Result &out);   // the silent-cable-chip probe (~330 ms); passive only, Busy + out.refusal otherwise
#if K2_EEPROM_READ
    eeprom::Err eeprom_probe(uint8_t bus, eeprom::Probe &out);   // lab-only, read-only 24Cxx probe (k2/drivers/eeprom.hpp); Busy while a charger test runs
    eeprom::Err eeprom_read(uint8_t bus, const eeprom::Addr &a, std::span<uint8_t> out);
#endif
#if K2_EEPROM_WRITE
    eeprom::PageResult eeprom_page_cas(uint16_t off, const eeprom::Page &expected, const eeprom::Page &next);   // the restore's page write; PageErr::Busy while a charger test runs
#endif
#if K2_EEPROM_CAL_CLEAR
    eeprom::CalResult eeprom_cal_clear(std::span<const uint8_t, eeprom::kCalLen> expected);   // the one write (compare-and-swap); CalErr::Busy while a charger test runs
#endif
    remote::VconnSnapshot debug_vconn(uint8_t op);   // 0 status, 1 send VCONN_Swap, 2 VCONN off
    remote::EprInfoSnapshot debug_epr_info(uint8_t op);   // 0 status, 1 ask (gated on the EPR bit), 2 ask regardless
    remote::LcdSnapshot debug_lcd();
    remote::LoopSnapshot debug_loop(uint8_t page, bool reset);   // the main-loop stall counters (k2/core/loop_stats.hpp)
    remote::AdcRawSnapshot adc_raw();
    remote::Status ripple(uint8_t sample_code);   // captures now; poll() streams the samples
    remote::RippleInfo ripple_info() const { return ripple_info_; }
    remote::IdentifyResult identify_device(uint8_t op);   // 0 status, 1 start, 2 start + confirm
    remote::RereadResult reread(uint8_t op, uint8_t mask);   // source-cap re-read: 0 status, 1 start, 2 abort, 3 sink-cap-ext mode, 4 force the scan line, 5 Ra mode
#if K2_OPPO_PROBE
    remote::OppoView oppo_probe(uint8_t op, const pd::oppo::Config &cfg);   // lab-only OPPO probe: 0 status, 1 start, 2 stop (k2/pd/oppo_probe.hpp)
    remote::OppoPowerResult oppo_power(uint16_t mv, uint16_t ma, bool confirm);   // lab-only: an advertised point of the list in force
#endif
    remote::LimitView pd_limit(uint8_t op, uint32_t limit_mw);   // PD limiter (k2/pd/pd_limiter.hpp): 0 status, 1 arm, 2 off
    remote::AppleReadResult apple_read(uint8_t op);   // lab-only Apple charger readout (SVID 0x05AC): 0 status, 1 arm + re-request the current contract
    // `over_load` is the lab-only bypass (k2_remote `pd request --over-load`); the UI never sets it.
    remote::PdOpResult pd_request(uint8_t pos, uint16_t mv, uint16_t ma, bool confirm, bool over_load = false);
    remote::PdOpResult pd_release();
    remote::StopInfo stop();   // cancel a charger test, stop FC (measured 5 V release), release PD, disarm a spoof (k2/core/stop.hpp)
    // The host dead-man lease (k2/core/lease.hpp): the dispatcher arms / refreshes it, poll() ticks it and runs stop() on expiry or an unplug.
    void lease_touch();
    void lease_arm();
    remote::LeaseInfo lease(uint8_t op, uint16_t secs);   // op 0 status, op 1 set the length (seconds, 0 = default)
    remote::PdOpResult pd_hard_reset(uint8_t flags = 0);   // flags: kPdFlagConfirm | kPdFlagResetInline (lab-only inline reset, remote `pd reset` only)
    remote::HandbackResult pd_handback(uint8_t flags = 0); // flags: kPdFlagConfirm only; phase-1 lab hand-back (remote `pd handback` only), verdict in the result
    remote::PdAttachResult pd_attach(uint16_t target_mv, uint16_t target_ma, bool confirm);   // pd::attach_verdict: 5 V default, above 5 V needs confirm, inline / busy refused
    remote::PdTriggerSnapshot pd_trigger();
    remote::MatchSnapshot match_info();
    remote::UsbLinkSnapshot usb_link();
    remote::MemorySnapshot memory();   // the cached memstat figures (k2/core/memstat.hpp)
    remote::PowerSnapshot power();     // idle power state and settings (k2/core/idle_hw.hpp)
    void power_set(const remote::PowerSet& s);
    void request_seen(bool state_changing);   // every request, before it runs: the idle machine counts it as activity
    void shell_control_seen();                // a control request (remote::shell_control): the screen says the shell is driving
    remote::Status fc_cmd(uint8_t op, uint8_t proto, uint16_t mv, uint8_t flags);
    remote::FcInfo fc_info();
    remote::StoreInfo store_cmd(uint8_t op, uint8_t slot);   // explicit flash-store actions (k2::storage)
    void cable_r_reset();
    void cable_r_mode(bool kelvin);
    remote::CableRInfo cable_r_info();
    remote::LabResult lab_load(uint8_t duty, uint16_t ms);
    remote::SniffInfo lab_sniff(uint8_t sub);
    remote::ListenInfo lab_listen(uint8_t sub, uint8_t secs, uint8_t port, uint8_t flags);   // listen attach (k2::fusb302::listen_*)
    // --- vooc-monitor (k2/vooc-detect) ---
    remote::VoocSnapshot vooc_monitor(uint8_t set);           // inline VOOC monitor: 0 off, 1 on, 2 keep; never drives
    svooc::Refusal svooc_start(uint8_t cap_s, bool usb_a, const svooc::Params &params);    // guarded no-load SuperVOOC trigger (FC op 5), remote only; usb_a = far-sink waiver; params = responder knobs
    void svooc_stop();
    remote::SvoocSnapshot svooc_info();
    size_t svooc_pairs(uint16_t from, std::span<svooc::PairRow> out);   // the run's command>reply pairs (FC op 5 / 3)
    bool lines_locked();                                      // the SuperVOOC trigger holds D+/D- (fctrig::svooc_running)
    // --- end vooc-monitor ---
    remote::UfcsInfo ufcs_info();                         // UFCS tuner / 5 V probe (k2::fctrig)
    uint32_t ufcs_get(uint8_t field);
    remote::Status ufcs_set(uint8_t field, uint32_t value);
    remote::Status ufcs_op(uint8_t op);                    // kUfcsReset / kUfcsProbe / kUfcsStop
    size_t ufcs_read(uint16_t off, std::span<uint8_t> out);
    // ---- PD Stress (Cmd::PdStress): thin glue over Ui::pd_stress() (k2/ui/pd_stress_session.hpp) ----
    remote::PdStressInfo pdstress_info();
    remote::Status pdstress_start(const remote::PdStressStart &s);   // Busy when another tool owns the ports; other refusals show in info().start
    void pdstress_stop();                                            // never refused: ends a run or drops a pending resume
    size_t pdstress_read(uint16_t off, std::span<uint8_t> out);
    // ---- end PD Stress ----
    // ---- PD Spec Check (Cmd::PdSpec): thin glue over Ui::pd_spec() (k2/ui/pd_spec_session.hpp) ----
    remote::PdSpecInfo pdspec_info();
    remote::Status pdspec_start(const remote::PdSpecStart &s);   // Busy when another tool owns the ports; other refusals show in info().start
    void pdspec_stop();                                          // never refused
    size_t pdspec_read(uint16_t off, std::span<uint8_t> out);
    const specsec::Apdos& charger_test_spec_apdo();   // Cmd::ChargerTest req [9, off]
    uint8_t charger_test_spec_split();                // ... and the grid's split on its count byte
#if K2_SPECROWS
    // Cmd::ChargerTest req [8, off]: the spec rows file of the run in hand (specsec::kRowsHdr + 10 byte rows), 0 = none.
    uint16_t spec_rows_len();
    size_t spec_rows_read(uint16_t off, std::span<uint8_t> out);
#endif
    // ---- end PD Spec Check ----
    // Cmd::LoopProbe: the last loop probe result file (k2/loopprobe/loop_probe_hw.hpp); read only, the probe starts on the K2.
    uint16_t loopprobe_len();
    size_t loopprobe_read(uint16_t off, std::span<uint8_t> out);
    uint16_t hostreport_len();
    size_t hostreport_read(uint16_t off, std::span<uint8_t> out);
    // Cmd::QrTest: the QR scan-test rig (k2/ui/ui_qrtest.hpp). Op 1 opens the full-screen test QR page
    // (Busy when the arena or the ports refuse it now); op 2 pops it when it is showing.
    remote::Status qrtest_show(uint8_t version, uint8_t px, uint8_t quiet);
    bool qrtest_stop();
    size_t blackbox(uint8_t op, uint32_t arg, std::span<uint8_t> out);   // PD black box ring (k2::pdtrace)
    bool ports_busy();   // a charger test or an FC release owns the link
    size_t emlib(std::span<const uint8_t> req, std::span<uint8_t> out);   // saved-cable library and spoof (k2::storage, k2::emk_spoof)
    size_t mi_cable_auth(std::span<const uint8_t> req, std::span<uint8_t> out);   // Xiaomi cable-side auth config (k2::emk_spoof::MiAuthCfg); req [0] reads, [1,on,key,trailing(4),half,reply,payload] sets; out [on,key,trailing(4),half,reply,payload]
    size_t cards(std::span<const uint8_t> req, std::span<uint8_t> out);   // charger report cards (k2::report_card::hw)
    size_t twin(std::span<const uint8_t> req, std::span<uint8_t> out);    // device twins (k2::twin::wire over k2::storage and k2::twin_hw)
    size_t transcap(std::span<const uint8_t> req, std::span<uint8_t> out);   // transition capture (k2::transcap)
    size_t diagnose(std::span<const uint8_t> req, std::span<uint8_t> out);   // Black Box diagnosis (k2::pd::diag::live)
    size_t trace_hid(std::span<const uint8_t> req, std::span<uint8_t> out);   // Black Box push over HID (k2::trace_hid_hw)
    size_t battery(std::span<const uint8_t> req, std::span<uint8_t> out);    // battery recorder (k2::battery::wire over battery::hw)
    uint8_t screenshot_seq() const { return shot_seq_; }
    bool log_enabled();
    void set_log(bool on);

private:
    uint16_t spoof_seq_ K2_BSS_INIT(0);   // library number armed through the remote, for the status reply
    uint8_t ufcs_start_ K2_BSS_INIT(0);   // ufcs::ProbeStart of the last probe op, for the UFCS status block
    static constexpr unsigned kQueueDepth = 4;
    struct Slot { uint8_t data[remote::kFrameSize]; uint16_t len; usb::Route route; };

    Ui &ui_;
    Slot queue_[kQueueDepth];
    unsigned head_ K2_BSS_INIT(0), tail_ K2_BSS_INIT(0);
    uint32_t dropped_ K2_BSS_INIT(0), dropped_reported_ K2_BSS_INIT(0);
    VirtualButton virt_[4] K2_BSS_ZERO;
    bool reboot_pending_ K2_BSS_INIT(false);
    bool dfu_pending_ K2_BSS_INIT(false);
    uint8_t dfu_mode_ K2_BSS_INIT(0);
    bool fault_pending_ K2_BSS_INIT(false);
    uint8_t fault_kind_ K2_BSS_INIT(0);
    bool shot_pending_ K2_BSS_INIT(false);
    bool shot_want_cdc_ K2_BSS_INIT(false), shot_want_packed_ K2_BSS_INIT(false);
    uint8_t shot_transport_ = remote::kShotViaHid;
    uint8_t shot_strip_rows_ = 4;
    // A capture holds the scratch arena (k2/core/scratch.hpp) from ripple() to the end of its stream in poll(): the 1 KB of
    // samples are live only inside the one poll() call that runs the command.
    bool ripple_pending_ K2_BSS_INIT(false);
    remote::RippleInfo ripple_info_ K2_BSS_ZERO;
    void stream_ripple(uint8_t seq);
    uint8_t shot_seq_ K2_BSS_INIT(0);
    lease::Lease lease_;
    void lease_tick();
};

}  // namespace k2
