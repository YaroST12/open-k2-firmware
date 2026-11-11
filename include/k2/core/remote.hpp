// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Firmware side of the remote-control channel: queues HID OUT reports, runs the commands from
// the main loop and injects virtual button presses. Protocol and dispatcher: remote_proto.hpp.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "k2/drivers/adc.hpp"
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

    // Queue one report from the USB stack (called through usb::on_remote_report()).
    void enqueue(const uint8_t *data, uint16_t len);

    // Backend interface for remote::handle_request (public so the dispatcher template can call it).
    remote::PingInfo ping();
    remote::ChargerTestSnapshot charger_test_results() const;
    remote::ChargerTestView charger_test_view();   // the wizard / Waiting / Running screens as data (0x0B req [3])
    remote::Status charger_units(std::span<const uint8_t> units);   // unit order of the next charger run (empty = default)
    remote::Status charger_sweep(bool on);   // the PPS deep sweep of the next charger run (opt-in)
    remote::Status charger_avs_sweep(bool on);   // the AVS deep sweep of the next charger run (opt-in)
    remote::ChargerCableReply charger_cable(const ctcable::Choice* set);   // cable identity of the START dialog (null = read)
    remote::ChargerHuntReply charger_hunt(uint8_t slot) const;   // evidence of a refused 6 A hunt row (read-only)
    remote::Status press(uint8_t button, uint16_t ms);
    remote::StateInfo state();
    [[gnu::noinline]] remote::SessionInfo session();   // GET_STATE part 1: Ui::session() on the wire (a read)
    [[gnu::noinline]] remote::DiagInfo diag();   // GET_STATE part 2: the Diag page counters (a read)
    [[gnu::noinline]] remote::KeysInfo keys();   // GET_STATE part 3: the strip labels of the page on screen (a read)
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
    remote::Status screenshot();                // Busy while Settings > Screenshot is off
    bool screenshot_enabled();
    remote::ScreenInfo screen_info();
    remote::Status debug_fake_pdos(uint8_t n);
    remote::Status debug_link(bool attached);
    remote::Status debug_pd_msg(uint8_t sop, uint16_t header, std::span<const uint8_t> payload);
    remote::Status debug_cc_matrix(uint8_t v, uint16_t modes);
    remote::Status debug_cc_probe(const ccprobe::Args &a, ccprobe::Result &out);   // passive only, blocks a.ms; Busy for any other state
    remote::VconnSnapshot debug_vconn(uint8_t op);   // 0 status, 1 send VCONN_Swap, 2 VCONN off
    remote::EprInfoSnapshot debug_epr_info(uint8_t op);   // 0 status, 1 ask (gated on the EPR bit), 2 ask regardless
    remote::LcdSnapshot debug_lcd();
    remote::AdcRawSnapshot adc_raw();
    remote::Status ripple(uint8_t sample_code);   // captures now; poll() streams the samples
    remote::RippleInfo ripple_info() const { return ripple_info_; }
    remote::IdentifyResult identify_device(uint8_t op);   // 0 status, 1 start, 2 start + confirm
    remote::RereadResult reread(uint8_t op, uint8_t mask);   // source-cap re-read: 0 status, 1 start, 2 abort, 3 sink-cap-ext mode, 4 force the scan line, 5 Ra mode
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
    remote::Status fc_cmd(uint8_t op, uint8_t proto, uint16_t mv, uint8_t flags);
    remote::FcInfo fc_info();
    remote::StoreInfo store_cmd(uint8_t op, uint8_t slot);   // explicit flash-store actions (k2::storage)
    void cable_r_reset();
    void cable_r_mode(bool kelvin);
    remote::CableRInfo cable_r_info();
    remote::LabResult lab_load(uint8_t duty, uint16_t ms);
    remote::SniffInfo lab_sniff(uint8_t sub);
    // --- vooc-monitor (k2/vooc-detect) ---
    remote::VoocSnapshot vooc_monitor(uint8_t set);           // inline VOOC monitor: 0 off, 1 on, 2 keep; never drives
    // --- end vooc-monitor ---
    remote::UfcsInfo ufcs_info();                         // UFCS tuner / 5 V probe (k2::fctrig)
    uint32_t ufcs_get(uint8_t field);
    remote::Status ufcs_set(uint8_t field, uint32_t value);
    remote::Status ufcs_op(uint8_t op);                    // kUfcsReset / kUfcsProbe / kUfcsStop
    size_t ufcs_read(uint16_t off, std::span<uint8_t> out);
    // Cmd::QrTest: the QR scan-test rig (k2/ui/ui_qrtest.hpp). Op 1 opens the full-screen test QR page
    // (Busy when the arena or the ports refuse it now); op 2 pops it when it is showing.
    remote::Status qrtest_show(uint8_t version, uint8_t px, uint8_t quiet);
    bool qrtest_stop();
    size_t blackbox(uint8_t op, uint32_t arg, std::span<uint8_t> out);   // PD black box ring (k2::pdtrace)
    bool ports_busy() { return ui_.ports_busy(); }   // a charger test or an FC release owns the link
    size_t emlib(std::span<const uint8_t> req, std::span<uint8_t> out);   // saved-cable library and spoof (k2::storage, k2::emk_spoof)
    size_t cards(std::span<const uint8_t> req, std::span<uint8_t> out);   // charger report cards (k2::report_card::hw)
    size_t transcap(std::span<const uint8_t> req, std::span<uint8_t> out);   // transition capture (k2::transcap)
    uint8_t screenshot_seq() const { return shot_seq_; }
    bool log_enabled();
    void set_log(bool on);

private:
    uint16_t spoof_seq_ = 0;   // library number armed through the remote, for the status reply
    uint8_t ufcs_start_ = 0;   // ufcs::ProbeStart of the last probe op, for the UFCS status block
    static constexpr unsigned kQueueDepth = 4;
    struct Slot { uint8_t data[remote::kFrameSize]; uint16_t len; };

    Ui &ui_;
    Slot queue_[kQueueDepth] = {};
    unsigned head_ = 0, tail_ = 0;
    uint32_t dropped_ = 0, dropped_reported_ = 0;
    VirtualButton virt_[4] = {};
    bool reboot_pending_ = false;
    bool dfu_pending_ = false;
    uint8_t dfu_mode_ = 0;
    bool fault_pending_ = false;
    uint8_t fault_kind_ = 0;
    bool shot_pending_ = false;
    bool ripple_pending_ = false;
    std::array<uint16_t, adc::kRippleSamples> ripple_{};
    remote::RippleInfo ripple_info_{};
    void stream_ripple(uint8_t seq);
    uint8_t shot_seq_ = 0;
    lease::Lease lease_;
    void lease_tick();
};

}  // namespace k2
