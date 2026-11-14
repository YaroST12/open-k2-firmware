// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// FUSB302 USB-PD PHY driver: CC orientation detection, Source_Capabilities capture, the
// e-marker scan and the active PD operations (trigger, Identify, Get_Source_Cap, hard reset). Pure decoding lives in k2/pd/pd.hpp. The chip sits on the FUSB bus at 0x22.
#pragma once
#include <cstdint>
#include <span>

#include "k2/core/bootmode.hpp"
#include "k2/core/build_config.h"
#include "k2/loopprobe/loop_cc.hpp"
#include "k2/pd/cc_probe.hpp"
#include "k2/pd/ra_probe.hpp"
#include "k2/pd/ra_class.hpp"
#include "k2/pd/emk_attached.hpp"
#include "k2/pd/emk_learn.hpp"
#include "k2/pd/emk_ra.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_interrogate.hpp"
#include "k2/pd/ra_guard.hpp"
#include "k2/pd/pd_probe.hpp"
#include "k2/pd/pd_reread.hpp"
#include "k2/pd/pd_passive.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_vdm.hpp"

namespace k2::fusb302 {

// Close the CC analog switches (GPIOC), reset the chip, apply the passive profile (pd_phy.hpp:
// SOP/SOP'/SOP'' receive, Auto-CRC off), then detect the active CC line. False if DEVICE_ID cannot be read.
bool init();

// Self-powered boot mode (k2/core/bootmode.hpp): call right after i2c::init(), before anything slow.
// Routes the CC switches (bootmode::cc_mask: both closed, PC13 low) and writes SWITCHES0 = the mode's Rd
// (0x02 Rd on CC2, 0x01 Rd on CC1) so a charger sees Rd at once and powers the K2. `warm` = the reset was not
// a power-on: a plain boot then keeps the Rd pin the backup-register pin mark says this firmware held (bootmode::initial_rd),
// so a charger-powered K2 that resets with VBUS present never takes its Rd away. The driver then keeps exactly those
// Rd bits in every unattached state (init, detach, scans) until self_power_leave(); 0 = normal passive behaviour.
// The pin is ONE line, never both (Rd/Rd is the Debug Accessory signature: a source turns its Rp off and stays off).
void self_power_early(bootmode::Mode mode, bool warm);
// A planned re-attach's comeback (k2/core/rd_intent.hpp): PC13 (Ra on CC1) high from the Rd's first moments, so the charger's first
// look at the fresh attach finds the Ra. Only when the pin is CC2; ra_set(true) takes the Ra over, ra_set(false) drops it.
void self_power_early_ra();
// Give a comeback's Rd back for good, whatever made it (key mode or marker): pin, pin mark, early Ra and the chip's bits. Drop the
// Ra mode (ra_set(false)) and the armed spoof first.
void self_power_drop();
// The pin is an inherited Rd (warm reset, no key mode), not a key mode's.
bool rd_inherited();
// Drop an inherited Rd for good: nothing resumes on it (no run, or the resume was cancelled).
void self_power_release();
// Key modes: MOVE the single pinned Rd to `line` (1 or 2), because the charger sits on the other orientation of the plug
// (bootmode::moved_pin decides). True when it moved; false when there is no pin, the pin is already on `line`, or it moved
// once already this boot (no ping-pong). Never Rd on both lines. The restore after a run's detach and the pin mark follow.
bool self_power_move(std::uint8_t line);
// End the mode: both Rd again (the normal passive profile). Not called by any UI path yet: the
// mode lasts until reboot.
void self_power_leave();
// Give the boot-mode Rd pin back after a run's detach took it over (unit_detach/unit_attach and the
// run-over path in set_rd_for_run call these, so a sink-mode boot still sees a real detach).
void self_power_restore();
bool self_powered();
std::uint8_t pin_rd();   // boot-mode Rd bits (bit0 CC1, bit1 CC2), 0 = plain boot: no Rd ever unless an active feature asks

// The direct-plug line fallback (pd::LineFallback): our single Rd is being tried on the lines of a charger plugged straight in
// (both CC lines carry its Rp, no VBUS yet). `line`: the line the Rd sits on (1 / 2, 0 = no attach); `moves`: moves made in this
// attach (0..3); `live`: a try is running (a move is left or the last dwell has not run out), the UI's "Waking charger" fact.
struct LineTrial {
    std::uint8_t line = 0;
    std::uint8_t moves = 0;
    bool live = false;
};
LineTrial line_trial();

// What this boot did to the pins, for the boot-info screen and `bootinfo` (k2/core/bootinfo.hpp). RAM only, read-only.
struct BootRecord {
    std::uint8_t pin_initial;    // Rd bits the boot wrote first
    std::uint8_t pin_now;        // Rd bits held now
    std::uint8_t pin_boot;       // Rd bits restored after a run's detach
    bool inherited;              // the pin is an inherited Rd (warm reset), not a key mode's
    bool moved;                  // self_power_move moved it this boot
    bool mark_read;              // the pin mark was consulted
    std::uint16_t mark;          // its raw word
};
BootRecord boot_record();
// The first CC-switch / SWITCHES0 writes after reset, then the SWITCHES0 readbacks: kind 0 = SWITCHES0 written, 1 = CC routing
// (bit0 PC14, bit1 PC15, bit2 PC13), 2 = SWITCHES0 read back from the chip (ms = when it was read, FF = the bus failed).
struct BootNote {
    std::uint16_t ms = 0;
    std::uint8_t kind = 0;
    std::uint8_t val = 0;
};
inline constexpr unsigned kBootReadbacks = 3;   // SWITCHES0 read back at ~50 ms, ~600 ms and ~3 s after reset
unsigned boot_note_count();
BootNote boot_note_at(unsigned i);
// Reads SWITCHES0 back when the next readback is due; cheap when not (the boot sampler and poll() call it).
void boot_readback_service();

// Call every ~20 ms: re-detect the CC line (2 Hz, sooner after a detach, a VBUS rise or a new live net: pd::ScanPace) while none is found, then drain up to 4
// received packets and decode Source_Capabilities.
void poll();

// The receive half of poll(): drain the RX FIFO and run the transmit engines. Call every ~1 ms so a
// Request answers Source_Capabilities within tSenderResponse even while the UI draws.
void service();

// The unattached CC scan alone, for the LCD idle hook: during a long redraw (a Home repaint when VBUS appears is ~150 ms)
// poll() does not run, and a source's offers ~130 ms after its VBUS went by unrouted (hardware 2026-11-11, 1 of 8 attaches,
// runs/2026-11-11-late-attach). vbus_mv (>= 0, a direct reading) is fed to pd::ScanPace first; -1 = no reading. Does nothing
// while a line is attached; otherwise the same gates as poll()'s scan (ScanPace due, no Rd hold, no idle-wake reading window).
void scan_service(std::int32_t vbus_mv);

// Standalone: open the CC lines for ~150 ms so the source re-sends its capabilities, then
// re-detect. Inline (set_inline(true)): purely passive. No Rd change, no CC switch, no
// transmit; it only arms "the next Source_Capabilities is a new source".
void rescan();
// Open both CC lines for `ms` (1..1000, blocking), then close them and arm a new-source rescan.
// False (nothing done) while inline or when ms is out of range.
bool cc_open(std::uint32_t ms);
// Charger test handover: on = emulate a detach (CC open ~300 ms, blocking) and refuse every PD request
// until off; pd_quiet_ms() = ms since the last PD frame was received.
void legacy_only(bool on);
std::uint32_t pd_quiet_ms();

// The caller (Ui, through pd::Link) tells the driver a real sink is attached. While inline every
// active tool (trigger, Identify, Get_Source_Cap, the VCONN e-marker scan, the VDM probe) refuses -
// a confirm does not override that: an inline K2 has no PD role of its own and any message it sends
// is a second sink talking out of sequence on a live wire (2026-11-10).
// Inline does not change the chip: it is passive (Auto-CRC off) except during an active operation.
void set_inline(bool on);
bool inline_mode();
// The caller (Ui, through pd::Link) feeds the link's waiting verdict in, next to set_inline(): the Rp-trace
// classification of the passive CC lines. DualRolePort = a dual-role device (a phone) alone on the port,
// toggling Rp/Rd every ~100 ms. The driver needs it for the alone-Identify decision (pd::identify_alone).
void set_wait_kind(raguard::WaitKind kind);
raguard::WaitKind wait_kind();
// True when Source_Capabilities arrived in the current attach session (a source announced itself this attach).
bool caps_this_attach();
// A sink sits behind the K2: the inline latch, or another sink's Request on the wire in this attach (the fast proof, before the
// Link has a tick to latch). While true the K2 presents no Rd, sends no Request and no reply for the device behind it, and
// opens no CC line. The far-sink half ends with the attach; the latch is the Link's to clear.
bool sink_downstream();

// A run start (charger test START, PD trigger / request) presents Rd itself while no sink is inline: on the
// line that sees Rp, both lines while none is detected, so a charger alone on the bottom port turns VBUS on
// (plain boot has no Rd). on=false drops back to passive. Refused (false) while inline.
bool rd_for_run(bool on);
// True when an awake source's Rp is on the wire right now: the active line is known, or a quick comparator look at
// CC1 and CC2 (210 mV, measure switch and threshold restored) sees a driven line. Passive, never touches VCONN or TX.
bool rp_on_line();
// Run the CC scan now when nothing is attached (poll() paces it with pd::ScanPace). The START wait loop never returns to the
// main loop, so it calls this every pass: the attach, and with it the Request for PDO 1, comes right after the charger's Rp.
void attach_scan();
// Debug only: set the CC switch matrix directly. v bit0 = PC13 (VCONN power), bit1 = PC14, bit2 = PC15
// (high = closed). Used to find the matrix slot that presents Ra on the second CC so a charger
// sends its SOP' Discover Identity and the e-marker spoof gets queried.
void debug_cc_matrix(uint8_t v, uint16_t modes);
// Debug only (`k2_remote debug cc-probe`): hold a raw SWITCHES0 value (and PC13/14/15 levels, optionally their pin modes)
// for a.ms while a charger waits for a sink, sample both CC lines at +1/+10/+100 ms and at the end, then put everything
// back. Passive only: any other state returns its Refusal and touches nothing. Blocks the caller for a.ms, calling
// service() every millisecond. vbus_mv() gives the freshest VBUS in mV (-1 = unknown); a reading above 1 V or the chip's
// own VBUS flag ends the hold at once. Args must already pass ccprobe::check_args.
using VbusFn = std::int32_t (*)();
ccprobe::Refusal cc_probe(const ccprobe::Args &a, VbusFn vbus_mv, ccprobe::Result &out);
// The loop probe's CC step (k2/loopprobe/loop_cc.hpp): with VBUS up at 5 V, hold each plan entry (the K2's own Rd on CC1 only,
// CC2 only, none, or as found) for hold_us, then sample the ADC's newest CC sweep, BC_LVL with the measure switch on each line
// and VBUS. Only the pull-down bits change; the measure switch and the CC switch pins stay as found. Between entries SWITCHES0
// goes back to the found value and service() runs, so the PD side never sees the probe's state. Refused with no write while a
// sink contract, an engine (identify, probe, re-read, prep, a run's Rd, the Ra mode) or VCONN is active, with VBUS above
// 5.5 V, or for a plan that fails cc_plan_ok. VBUS above 5.5 V at a sample ends the plan (kCcAborted). Restores SWITCHES0
// and reads it back (kCcRestored). Blocks for about plan.size() x (hold_us + 1 ms).
loopprobe::CcRefusal loop_cc_hold(std::span<const std::uint8_t> plan, std::uint16_t hold_us, VbusFn vbus_mv,
                                  loopprobe::CcResult &out);

// The loop probe's Host report (k2/loopprobe/host_report.hpp; the binding is built with K2_HOST_REPORT=1 only). The one
// exception to "an inline K2 never transmits": the far sink is the K2's own HID port (fixed Rd, no PD controller), so the K2
// is the only PD speaker on the line. While a listener is set the driver treats the link as sole-sink (the HID port's Rd
// does not count as a sink behind the K2; a Request from another sink still does), presents no Rd of its own (5.1k beside
// the HID port's 5.1k would read as one 2.55k pull-down), keeps Auto-CRC on, asks PDO 1 at 5 V on every caps message the
// chip acked, and never sends a Hard Reset (sink never_hard_reset). The data role follows loop_host_set_dfp(): the
// GoodCRC (SWITCHES1) and the SOP header bit of every message the K2 sends.
class LoopHost {
public:
    virtual void on_message(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) = 0;   // SOP only
    virtual void on_hard_reset(std::uint32_t now) = 0;   // received from the host; the role is UFP again
    virtual void on_detach(std::uint32_t now) = 0;       // the line went away (the session is over)
protected:
    ~LoopHost() = default;
};
#if K2_HOST_REPORT
void loop_host_begin(LoopHost* l);   // nullptr ends it: UFP again, the usual inline rules from the next tick
bool loop_host_send(const pd::TxMsg& m);   // one SOP message (MessageID stamped); true = GoodCRC came back
void loop_host_set_dfp(bool dfp);
bool loop_host_soft_reset();         // the sink's Soft_Reset on a Ready contract; false = refused (no session, not Ready)
bool loop_host_idle();               // at the last tick: nothing sent, the sink quiet, the line idle
std::uint8_t loop_host_rp();         // the active line's BC_LVL now (0..3), 0 with no line
#endif
// `k2_remote emarker probe`: sense the pull-down a silent cable chip hangs on its supply pin. A cable alone in the port (nothing
// inline, no contract, no Rd of ours on any line, VBUS dark): sample both CC lines at the snapshot (idle), then with the K2's
// VCONN switch on CC1 only and on CC2 only (the other line just measured), 2 / 10 / 100 ms into each phase, and put everything
// back. Raw ADC counts only (k2/pd/ra_probe.hpp); the verdict is k2/pd/ra_class.hpp. Any other state returns its Refusal
// (also in out.refusal) and drives nothing; a Rp-like level on either line at idle refuses before VCONN is ever applied. A
// VBUS rise (monitor above 1 V or the chip's flag) while VCONN is on switches it off at once and ends the run (kFlagAborted).
// Never transmits. Blocks the caller for raprobe::total_ms(), calling service() every millisecond. a.vconn_pc passes
// raprobe::check_args (PC13 is masked off regardless).
raprobe::Refusal ra_probe(const raprobe::Args &a, VbusFn vbus_mv, raprobe::Result &out);
bool rd_for_run_on();
// The Wake sleeping port's idle Rd hold: the line (1 CC1, 2 CC2) the K2 presents an Rd on while it waits for a source that sleeps,
// 0 = none. The CC levels on that line are then loaded by our Rd, not the far port's own (the crossed-CC detector stands down).
std::uint8_t idle_wake_line();
// The open (no-Rd-of-ours) ADC reading a line last had when it was settled (RdSeek::open_mv, kept while no Rd is presented),
// -1 = none. The idle "Source asleep" hint reads it: high open = the standby signature of a sleeping source.
std::int32_t open_cc_mv(std::uint8_t line);
// Rd off for a moment while the run start goes on (a silent charger gets a clean detach, then rd_for_run(true)
// again). Unlike rd_for_run(false) it keeps the cable scanned before the start: the same cable stays plugged in.
void rd_for_run_pause();
// Charger test units: every unit starts with unit_detach() (Rd off, 5 V default, PD-silent mode off, sink forgotten) and goes on
// with unit_attach(legacy): Rd on as a fresh attach, PD-active (legacy=false) or PD-silent (legacy=true).
void unit_detach();
void unit_attach(bool legacy);
// The unit boundary of a SELF-POWERED run (a K2 powered by the charger under test through its pinned Rd): the charger must never
// see a detach, or it cuts VBUS and takes the K2 with it. No Rd change, no CC open, no 5 V reset. legacy=false: the sink forgets
// its contract, a Soft_Reset goes out (answered with fresh caps, which the acked-caps path requests PDO 1 from at 5 V); legacy=true:
// the sink forgets everything and the session turns PD-silent (Auto-CRC off), the source's D+/D- stay free for the probe.
void unit_hold(bool legacy);

// INT_N (PB12, EXTI12 falling edge): the ISR only calls this; service() does the work.
void on_int_n();

// The next Source_Capabilities on SOP clears the previous source's caps, identity and contract.
void new_source();

// The charger is gone: forget its caps, identity and contract now (the lists on screen and in the remote empty).
void clear_source();

// Read the cable's e-marker; the result is in emarker(). `mode` comes from pd::emarker_mode():
//  Vconn (nothing supplies VBUS): inject VCONN and send SOP' Discover Identity, trying both
//        orientations. Blocks ~0.1 to 0.5 s. Inline it refuses (false, logged), with or without
//        `confirmed` - an inline K2 never transmits.
//  Sniff (a source is attached): touch nothing on CC, only report what the source's own SOP'
//        Discover Identity exchange already gave us (poll() decodes it whenever it is heard).
// True if the cable data is valid.
bool read_emarker(bool confirmed = false, pd::EmarkerMode mode = pd::EmarkerMode::Vconn);

// A powered-cable session for `emarker learn`: the same VCONN scan as read_emarker (Vconn mode only, same refusals),
// and when a cable answers its Discover Identity, `body(ctx)` runs with VCONN and the BMC routing still set so it
// can call cable_exchange(); the pre-scan state is restored afterwards, whatever body did. False without calling body
// when the scan found no cable (or was refused).
using CableBody = void (*)(void* ctx);
bool cable_session(bool confirmed, CableBody body, void* ctx);

// One SOP' request inside a cable_session and the cable's answer (the chip's auto GoodCRC answers the cable's reply).
// The MessageID continues after the scan's Discover Identity (1, 2, ...) and advances once the request was
// acknowledged. False: the chip reported no GoodCRC (RETRYFAIL) or no session is open; true: acknowledged, and
// `rep.got` tells whether a message other than GoodCRC followed within `timeout_ms`.
bool cable_exchange(const emk_learn::Request& req, emk_learn::Reply& rep, std::uint32_t timeout_ms);

// Counts cables decoded since boot (rises each time a SOP' Discover Identity ACK is accepted, from a
// scan or from the source's own exchange): the UI marks the cable data fresh when it changes.
std::uint32_t cable_seq();
// Attach session: changes on every attach and every detach. Offers and requests belong to one session.
std::uint32_t session();

// Hand one message to the receive path as if it came off the wire (remote `debug pd-msg`).
// `sop`: 0 SOP, 1 SOP', 2 SOP''; `payload` = data objects, no token and no CRC.
void feed(std::uint8_t sop, std::uint16_t header, std::span<const std::uint8_t> payload);

// ---- Active PD. Boot is passive; nothing below runs unless the UI or the remote calls it. ----
// The pure engines (pd_sink.hpp, pd_identify.hpp) decide what to send; the driver sends it from poll().

// Request a contract. Refused while inline (Err::Inline) even when confirmed: a live sink is attached
// and the K2 must not renegotiate its supply. The caller enforces "above 5 V needs a confirm".
// `over_load` is the lab-only bypass (remote `pd request --over-load`, never the UI or the charger
// test): it lifts the inline refusal only while the far sink is a passive load (over_load_facts()).
pd::Err sink_request(const pd::Choice& c, bool confirmed = false, bool over_load = false);
// The one trigger gate (wizard, remote and sink_request all use it): None = a Request may go out.
pd::TriggerRefusal trigger_refusal();
// Lab-only over-load facts, derived from the existing foreign-evidence tracking (no second tracker):
// far_sink = the inline latch, a sink Rd read while inline, a sink Rd on CC with none of ours, or a
// foreign Request; far_pd_message = a Request we did not send this attach, any sink-role control
// message (GoodCRC, Get_Source_Cap), or the sink-role records the engine already filed
// (Sink_Capabilities, VDMs) - all cleared with the attach.
pd::OverLoadFacts over_load_facts();
// Any PD message (any SOP: caps, Request, PS_RDY, VDM, GoodCRC) seen since this attach.
bool pd_traffic();
// A sink other than the K2 sent a Request in this attach (power-role bit clear; the K2 never hears its own TX).
// The hand-back decision (pd::HandbackFacts::far_request_seen) and the over-load facts use this one tracker.
bool far_sink_seen();
// Latched like pd_traffic() but cleared only by a new attach: a unit's hold / start-up resets pd_traffic(), this one keeps
// "this source spoke PD since we attached". The charger test uses it to tell "PD gave up before START" from "never PD".
bool pd_seen_attach();
// Back to 5 V (Request PDO 1; hard reset if the source refuses). The 5 V contract stays up for kSinkIdleLingerMs,
// then the sink drops Rd (a held trigger never times out: only this call or a detach ends it).
void sink_release();
// User-requested hard reset; false = refused (no CC, or inline). `lab_inline` = the remote's lab-only
// `pd reset --inline --confirm` (gated by pd::lab_reset_verdict): its one Hard Reset goes out even inline.
bool sink_hard_reset(bool lab_inline = false);
// Device twin live proof: answer the source's Get_Sink_Cap with these caller-owned words (1..7, not copied, they must outlive the
// override) instead of the K2's own; (nullptr, 0) restores the K2's. Only the Sink_Capabilities answer changes (pd::Sink::
// set_sink_cap_override). The run that sets it clears it, on every exit path.
void sink_cap_override(const std::uint32_t* pdos, std::uint8_t n);
void sink_set_cap(std::uint32_t mv);  // highest voltage a request may ask for (from the power monitor)
// The HID-cable loop 5 V lock (k2/core/loop_guard.hpp): while on, the sink refuses every request above 5 V (pd::Err::OverCap) and
// re-announced offers are answered at 5 V; the edge to on releases a contract (or an EPR entry) above 5 V to PDO 1.
void sink_set_hv_lock(bool on);
// Somebody else (the SuperVOOC trigger) owns VBUS: the sink originates nothing because VBUS left its contract (pd::Sink::set_vbus_external).
// The owner clears it on every exit path.
void sink_set_vbus_external(bool on);
void sink_cc(std::int32_t cc1_mv, std::int32_t cc2_mv);  // measured CC voltages; inline, no sink Rd near 1.5 V = sink left. Also stored as the latest net levels (g_cc1_mv/g_cc2_mv) for the attach scan's Rd-window cross-check and its re-run on fresh levels while a line is attached with no PD decoded yet; -1 = unknown
// A sink's Rd level (the sink band, about 1.5 V) on a CC line while the K2 pulls none down itself, from the latest sink_cc() levels:
// a phone or laptop on the other end of an inline K2, visible even when no PD message is ever exchanged. False while the K2's own
// Rd is on (the level is then its own divider) and while the levels are unknown.
bool sink_cc_far();
void sink_vbus(std::int32_t mv);      // measured VBUS; the engine aborts to 5 V above target + 10 % / the cap
const pd::SinkStatus& sink_status();
// The sink's record of the last Request it sent and the Xiaomi chain around it (diagnostics for a refused 6 A row).
const pd::XiaomiTrace& xiaomi_trace();
// Ask the source for its EPR source list while we stay in SPR mode (EPR_Get_Source_Cap, informational: never answered with an
// EPR_Request). Gated on the EPR bit in PDO 1 unless `force` (the remote debug ask). The outcome is sink_status().epr_info;
// the list, when received, is epr_info_list().
pd::Err epr_info_request(bool force = false);
const pd::SourceCaps& epr_info_list();

// Lab-only Apple charger readout (SVID 0x05AC, k2/pd/apple_readout.hpp), OFF BY DEFAULT: while unarmed the
// Discover SVIDs ACK stays exactly [0x2717]. apple_read_start() arms it - our SVIDs ACK then lists
// [0x2717, 0x05AC] and the sink re-requests the CURRENT contract (its one gate, never a different
// voltage) so the source re-runs its 0x05AC discovery; the readout resets with the attach. Refused
// inline, without a contract, to a source that is not our partner, or while busy (pd::apple_read_refusal).
pd::Err apple_read_start();
const pd::AppleInfo& apple_info();
bool apple_armed();
bool answers_sop();   // the sink answers SOP messages: the source is the K2's own partner

// Ask the source for its capabilities now. Inline it refuses, confirmed or not (an inline K2 never
// transmits). Sent by the driver outside the sink's gate: the lab shell only. The Connection page's
// Refresh uses refresh_source_caps().
bool get_source_caps(bool confirmed = false);

// The Connection page's Refresh on a live contract (core/rescan.cpp): the sink's own Get_Source_Cap through its one message gate
// (pd::Sink::request_source_caps). Ok = queued; Busy = refused (inline, PD-silent session, no CC attached, a trigger prepare or an
// identify/probe/re-read running, or the gate says so). The answer shows as caps_count() moving.
pd::Err refresh_source_caps();
// Soft_Reset on a live, settled contract (PD Stress only): the contract drops, the source re-announces, the sink Requests PDO 1 at 5 V.
// false = refused (no CC, inline, PD-silent session, an owner talking on the line, or the sink not in Ready).
bool sink_soft_reset();
// Source_Capabilities messages received since boot.
std::uint32_t caps_count();
// A charger-test legacy unit holds the line PD-silent (legacy_only(true)): a rescan changes nothing then.
bool legacy_only_active();

// Identify the port partner (SOP Discover Identity / SVIDs / Modes, Get_Sink_Cap, Get_Status,
// Get_Revision), only on an idle CC line. False when refused: no CC, or inline (even when
// `confirmed`: an inline K2 never transmits; the sniffed identity shows on the Sink tab instead).
bool identify_start(bool confirmed = false);
void identify_abort();
bool identify_busy();      // an Identify is in flight: running, or an alone-Identify still waiting for its Ready contract
const pd::IdStatus& identify_status();
bool identify_current();   // the latest Identify's answers are this partner's (after an alone-Identify's release too, until the port is really gone)
const pd::Identity& identify_identity();   // the identity the latest Identify collected (kept for the release window)

// Inline interrogation (k2/pd/pd_interrogate.hpp): the one exception to "an inline K2 never transmits", for learning a device
// twin. A Hard Reset when no fresh contract was sniffed, the device asked as its source/DFP (Discover Identity, Get_Sink_Cap,
// Get_Sink_Cap_Extended, Get_Revision, Discover SVIDs / Modes), a final Hard Reset, the renegotiated contract compared with the
// first. `outside`: what the caller knows (another owner busy: charger test, SVOOC trigger, PD Stress, FC release; the K2 fed
// by this VBUS; the user's confirm); the driver adds the CC, the inline latch, its own engines and a K2-held contract.
// Returns the refusal (None = started). Auto-CRC stays off throughout; each question goes out once, no chip retries.
pd::IqRefusal interrogate_start(const pd::IqFacts& outside);
void interrogate_abort();
bool interrogate_running();
const pd::IqStatus& interrogate_status();   // the latest run's, kept until the next start

// Quick source probe for the charger report card (k2/pd/pd_probe.hpp): Discover Identity, Get_Manufacturer_Info and
// Get_Source_Cap_Extended on SOP, one try each, over within ~1 s whatever the charger does. False when refused
// (no CC, inline, or the sink / Identify is busy). The answers land in vdm() and ext(); probe_status() says which.
bool probe_start();
void probe_abort();
const pd::ProbeStatus& probe_status();

// Source-cap re-read (k2/pd/pd_reread.hpp): the after-contract conversation, Get_Source_Cap_Extended, Discover
// Identity, Discover SVIDs, Get_Source_Cap, picked by a kReread* mask, to see whether the charger lists more PDOs (an
// SPR AVS APDO) afterwards. PD mode only: false when refused (inline, charger test, or the Identify / probe / prepare is
// busy, or the sink is mid-request). With no contract it makes one first (the attach phase): presents Rd, then the START
// path's automatic Request for PDO 1, settles, and only then runs the steps. The attach it made ends clean: 30 s after the
// run (at once on failure or abort) the sink goes back to PDO 1, forgets the contract (Auto-CRC off) and drops the Rd it
// presented ([REREAD] attach released); a START, trigger or unit that took over meanwhile keeps what it has. A TX that
// times out without TXSENT still waits for the step's answer. The result lands in reread_status(), a [REREAD] log line
// and a Black Box mark.
bool reread_start(std::uint8_t mask = pd::kRereadDefault);
// `pd-reread --abort`: the lab's one way to end whatever the lab left on the link. Stops a running re-read, clears the forced
// line and the Ra mode, and releases the Rd and contract the K2 holds for nobody: the re-read's own attach at once, and a
// lab-held one the re-read no longer owns (after `pd attach` / `pd request`, a finished re-read another tool took over, `pd
// release`) the same way: back to 5 V first, then the contract forgotten and Rd off ([REREAD] attach released), so the source
// sees the sink leave. false = refused, nothing changed: a charger test / PD Stress / twin run owns the line (run_owner) or a
// far sink is inline. A boot-pinned Rd (self-powered or bus-powered K2) and a running Identify or probe are not lab-held and stay.
bool reread_abort();
// True while the only hold on the wire is the re-read's own leftover attach (pd::reread_own_hold: a finished re-read's
// PDO 1 5 V contract inside its linger). A charger-test START and the spoof arming take it over like the self-hold
// instead of refusing "tool busy" / "Ra refused: contract".
bool reread_own_hold();
// Another owner takes the contract (and the Rd) a pd-reread attach made: from here the attach is that owner's, and the
// re-read's own release timer ([REREAD] attach released) never fires on it. Called by every tool that starts talking on the
// link after a re-read: sink_request itself (`pd attach`, `pd request`, the identity and unit paths), a PD Stress run start, a
// Device twin live run. A re-read nobody took over still releases after its linger. The attach then ends with a release
// of its new owner, a detach, or `pd-reread --abort` (reread_abort adopts any lab-held Rd). Cheap and idempotent.
void reread_take_over();
// The survey's wire questions (k2/core/survey.hpp, pd::Survey): the same sequencer on a contract that is already there,
// in the survey's order, 220 ms between messages, bounded by kSurveyWireMs. Never attaches (no Rd, no Request): false when
// there is no Ready contract, a sink inline, the PD-silent mode, or anything else owns the wire. The answer is in
// reread_status() (ok_mask / notsup_mask / skipped_mask) once it is no longer running.
bool survey_wire_start(std::uint8_t mask);
// A Xiaomi 0x2717 auth chain is in progress (the sink holds its own Request back; so does the survey).
bool xiaomi_chain_active();
// Debug option for the next re-read (pd-reread --line N): the attach scan may take only CC line N (1/2); 0 = normal.
// With another line already taken (and no contract) the run forgets it first. It stays for every later attach scan (a
// charger test START as well) until reread_abort() or set_force_line(0) or a reboot: RAM only.
void set_force_line(std::uint8_t line);
// Settings > Wake sleeping port (pattern slow-rp-wake, idle path): when on, the plain passive idle holds the K2's own Rd,
// one line at a time, on a line that read the standby signature open, to ask a sleeping source (the open readings come
// from sink_cc). Default off: with it off the idle K2 presents no Rd (connection invariants I1 / H2) and the Rd seek
// forgets its open readings on every attach, exactly as before the feature. Switching off drops a running hold at once.
// Remembered by uiprefs and applied from main() at boot; set_idle_wake survives init().
void set_idle_wake(bool on);
bool idle_wake();
// A charger-test run owns the line for its whole length (START to Done / Stopped / cancel, the resume included), in every phase:
// the unit detach and attach gaps, Waiting, Releasing. The idle wake (the hold and the dark-attach release) never acts while it
// is set, so it cannot put its own Rd on the line the run just took off, nor release the run's attach. Set by the charger-test
// session; the driver does not know the Runner. Cleared by init().
void set_run_owner(bool on);
bool run_owner();
// Top-plug e-marker spoof (k2/pd/emk_ra.hpp): PC13 high = an Ra on CC1, Rd on CC2 alone, attach scan forced to CC2, the
// chip's VCONN switches never closed. PC13 goes high at once, before any Rd. Refused (false) with a sink inline, a
// contract or tool running, a boot-pinned Rd on CC1 or the VCONN experiment on; a partner already seen without a contract
// is detached first. A pinned CC2 Rd (PLUS boot) and the contract of its own 5 V hold do not refuse: the Ra goes on beside
// them, but the source has already attached, so it may never look at the cable. Ends with ra_set(false),
// reread_abort(), a detach (except inside a charger-test run, which owns it from its START and releases it when the run is
// over) or a reboot. Logs [EMKRA].
bool ra_set(bool on);
bool ra_active();
emkra::Refuse ra_refusal();   // why the last ra_set(true) was refused (Refuse::None when it was not)
// `pd-reread` op 6 (lab): pin the Rd on CC1/CC2 (0 = release) as a key-mode boot does, mark included, so a HID-powered K2 can
// stand in for a charger-powered one (a following warm reset inherits the pin).
bool debug_pin(std::uint8_t line);
// A charger-test run takes over the Ra mode that is on (the mode then survives the run's unit detaches, as after a START's
// rd_for_run(true)). ra_set(false) hands it back, so a run that drops the Ra for its Legacy units calls this after each ra_set(true).
void ra_run_own();
// A real CC-level detach for `debug detach`: every owner of Rd is dropped (sink, re-read, run Rd, forced line, Ra), Rd goes
// off, and it stays off for kRdHoldMs (longer than tPDDebounce) so the source sees the sink leave. false (nothing done)
// inline or with a boot-pinned Rd. The scan re-attaches afterwards if a source is still there.
bool cc_detach();
const pd::RereadStatus& reread_status();
// How the Pd-mode sink answers Get_Sink_Cap_Extended (pd::SinkCapExtMode): the K2's SKEDB (default), Not_Supported or
// silent. A lab experiment switch, kept in RAM only (reboot = SKEDB).
void set_sink_cap_ext_mode(pd::SinkCapExtMode m);

// Whether the sink sends a proactive Discover-SVIDs REQ once after the contract settles
// (default on since the sink-initiated Xiaomi auth opener; the reread sends its own SVIDs
// step, so its tests switch this off for determinism).
void set_proactive_discover(bool on);

// Software reset of the chip (RESET register, SW_RES), for parity with a power cycle just before
// the system resets. The driver is unusable until init() runs again.
void park();

const pd::SourceCaps& caps();
const pd::Emarker& emarker();
// Which scan orientation and which attempt-table row the cable answered (emk_variant.hpp). Empty (valid() false)
// when no VCONN scan found the cable, e.g. the e-marker was only sniffed or none was read.
emk_variant::Answered cable_answered();
// How the last VCONN cable read of this attach ran: with the cable alone (both orientations, no Rd), or with a source
// attached (the K2's Rd kept on the live line, one orientation), and how many query variants the chip acknowledged
// (GoodCRC). mode None = no read since the attach. A silent cable alone and a silent cable with the source attached are
// different facts: readers of "cable not read" use this to tell them apart; acked_no_answer(false) = a powered chip
// that acknowledged but never answered.
emk_attached::ReadInfo last_cable_read();
// What the chip's pull-down says about a cable that gave no identity (k2/pd/ra_class.hpp): an alone scan that ends with no
// answer runs the Ra probe once and files its verdict here. NotProbed = the last read found the cable, was run with a source
// attached, the probe was refused (VBUS, an Rd, a source level on CC, no VBUS source registered) or nothing was read since the
// attach. "Chip present but silent" (cc1 / cc2) differs from "no chip" (none): readers of "cable not read" use this.
raclass::PullDown pull_down();
// The monitor reading the Ra probe's VBUS guard uses (mV, -1 = unknown; the main code passes the power monitor). Unset: the
// probe refuses with "VBUS unknown", so a cable read ends "not probed".
void set_vbus_source(VbusFn vbus_mv);
// Current-attach physical wire evidence only; injected frames and spoofing cannot raise this.
const pd::CableLimits& physical_cable();
const pd::Revision& source_revision();  // passively received source RMDO, never Identify's sink answer
// Identity, SVIDs and modes the SOURCE (the charger) sent on SOP, and what the SINK (the device behind the K2) sent:
// filed by the sender's power role, never mixed.
const pd::VdmInfo& vdm();
const pd::VdmInfo& sink_vdm();
// USB / Thunderbolt link classified from the PD traffic seen on SOP, SOP' and SOP''; the idle D+/D- levels (mV, -1 unknown)
// only decide when no PD data was seen. Passive: never transmits. Limits: k2/pd/usb_link.hpp.
usblink::Link usb_link(std::int32_t dp_mv, std::int32_t dm_mv);
usblink::Raw usb_raw();   // the PD facts behind it (Enter_USB, Thunderbolt / DisplayPort Enter_Mode): what a twin keeps as "observed"
const pd::ExtInfo& ext();   // latest extended messages from the source (Manufacturer_Info, Source_Cap_Extended, ...)
const pd::SinkCapsSeen& sink_caps();  // the sink's own Sink_Capabilities, only when the source asked for them
const pd::Contract& contract();  // passively decoded Request / PS_RDY      // identity / SVIDs / modes the source sent on SOP
std::uint32_t request_period_ms();  // period of the last two sink Requests this attach (0 = fewer than two)
// A source that sent no PD caps within 1 s of attach: its Rp current in mA (0 while undecided, or PD).
std::uint32_t typec_ma();
bool cable_thunderbolt();      // the cable advertised SVID 0x8087 (Thunderbolt)
std::uint32_t rx_count();  // PD messages received since boot (Diag page)
std::uint8_t device_id();
std::uint8_t active_cc();  // 0 none, 1 CC1, 2 CC2

// ---- SOP' responder seam (k2::emk_spoof) ----
// While a hook is set, every received SOP' and SOP'' message is passed to it (`sop` 1 or 2, header, data objects)
// after the engine has seen it; SOP never. SOP'' arrives only while CONTROL1 listens on it (the passive profile and a cable
// read; the armed spoof's Auto-CRC profile is SOP' only), so a hook that answers must check `sop`.
// send_sop_prime_frame() writes a ready FIFO token stream (sync, PACKSYM,
// message, JAM_CRC, EOP, TXOFF) and sets TX_START; false when no CC line is active.
using SopPrimeHook = void (*)(std::uint8_t sop, std::uint16_t header, std::span<const std::uint8_t> payload);
void set_sop_prime_hook(SopPrimeHook hook);
bool send_sop_prime_frame(std::span<const std::uint8_t> tokens);

// ---- VCONN_Swap experiment (remote debug command only) ----
// vconn_request(): send VCONN_Swap to the source; on its Accept the K2 switches VCONN on onto the CC line opposite
// the active one (PC13 high + the chip's VCONN switch) and sends PS_RDY. A partner VCONN_Swap while we hold it is
// answered with Accept and VCONN goes off after its PS_RDY. vconn_off() drops it at once. Refused (false) without a
// ready contract, with a sink inline, or while a swap is running.
struct VconnInfo {
    std::uint8_t state = 0;       // pd::VconnState
    std::uint8_t why = 0;         // pd::VconnWhy
    std::uint8_t applied = 0;     // VCONN is on the pin right now
    std::uint8_t swaps_rx = 0;    // VCONN_Swap messages the partner sent us
    std::uint8_t active_cc = 0;
    std::uint16_t cc1_mv = 0, cc2_mv = 0;   // ADC, no divider: a 5 V net reads the 3.3 V rail
};
bool vconn_request();
bool vconn_roundtrip();
bool data_reset();        // lab: send Data_Reset (the VCONN source cycles VCONN; a DFP may re-read the cable)   // lab: take the VCONN role without driving it, then hand it back (the source turns VCONN on)
// A controlled re-attach: Rd off (the Ra mode's PC13 stays), the caller times the gap, then Rd back with the contract forgotten, so the
// source sees a detach and a fresh attach. A K2 on this source's VBUS may die in the gap (the comeback boot finishes the job).
void rd_glitch_begin();
void rd_glitch_end();
void vconn_off();
VconnInfo vconn_info();


}  // namespace k2::fusb302
