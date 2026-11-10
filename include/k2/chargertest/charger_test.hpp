// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <algorithm>
#include <array>
#include <span>
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/pd_sink.hpp"

namespace k2::charger_test {

enum class Group : uint8_t { Pd, Qc2, Qc3, Qc3Plus, Afc, Fcp, Scp, Vooc, SuperVooc, SuperVooc65, Ufcs, Vivo, Count };
enum class Algorithm : uint8_t {
    Pd2Fixed, Pd3Fixed, Epr, SprAvs, Pps, Qc2, Qc3, Qc3Plus, Afc, Fcp, Scp,
    Vooc, SuperVooc, SuperVooc65, Ufcs, Vivo, Pd30Revision, Pd31Revision, Pd32Revision, EprAvs, Qc2ClassB, Qc3ClassB, Count
};
static_assert(unsigned(Algorithm::Count) < 32);
inline constexpr uint32_t kAllAlgorithms = (1u << unsigned(Algorithm::Count)) - 1;
inline constexpr size_t kMaxSteps = pd::kMaxPdos * 6 + 46;   // 40 + the six default Class B rows (Qc2ClassB 1, Qc3ClassB 5); the journal's one-block section assert caps this
// The current a PD plan row requests: 1 A (clamped to the offer and the cable). The one exception is the full-current PPS row, the
// last row of each PPS APDO: top voltage at the APDO's whole current, so it is the only non-hunt row above this. The 6 A hunt
// rows request 6 A. Host tools tell a hunt from a full row by that gap (tools/step_label.py HUNT_MA = 5000, the cable ceiling).
inline constexpr unsigned kPlanRowMa = 1000;
constexpr uint32_t bit(Algorithm a) { return a < Algorithm::Count ? 1u << unsigned(a) : 0; }
inline constexpr uint32_t kPdFixedAlgorithms = bit(Algorithm::Pd2Fixed) | bit(Algorithm::Pd3Fixed) | bit(Algorithm::Epr);
inline constexpr uint32_t kPpsAlgorithms = bit(Algorithm::Pps) | bit(Algorithm::SprAvs) | bit(Algorithm::EprAvs);
// Class B (20 V) rows are part of the default selection: on a Class A charger they read as a neutral skip (Reason::ClassAOnly).
inline constexpr uint32_t kDefaultAlgorithms = kAllAlgorithms &
    ~(bit(Algorithm::Pd30Revision) | bit(Algorithm::Pd31Revision) | bit(Algorithm::Pd32Revision));
Group group(Algorithm);
const char* name(Algorithm);
enum class Verdict : uint8_t { Pending, Running, Pass, Fail, Skip };
enum class Reason : uint8_t {
    None, Disabled, NoOffer, OverCap, NotImplemented, NeedConfirm, NoMonitor, NoSource,
    Inline, Busy, Fake, Request, Timeout, Voltage, HardReset, Lost, Cancelled, Release,
    CapsChanged, PdActive, RevisionUntestable, EmptySelection, CableLimit, UnsupportedOffer,
    StayedPd, RebootRequest, RebootHandover, EprRefused, NoContract, ShutOff, NoReattach, CutRecovered, BlockedByPd,
    NoQc3Plus,  // QC3+: the engine got no movement from the 9 V request, VBUS stayed at ~5 V (a skip: the charger does not do QC3+)
    RebootLimit,  // the unit took the K2 down kRebootBudget times (reboots / power losses): its remaining rows are skipped, the run goes on
    NeedsExternalPower,  // a self-powered K2 (running on the source's own VBUS) skips a row that can cut the output (the low PPS/AVS edge)
    PdGaveUp,            // a self-powered K2 heard PD earlier in this attach, but the source's list is gone at START (it stopped answering)
    ClassAOnly,          // a QC Class B (20 V) row the charger did not follow: a Class A charger (12 V max), a neutral skip, not a failure (QC2 Class B: only unless every QC2 Class A row failed its handshake)
    NotUnlocked,         // a 6 A hunt row the source did not offer: the OEM mode is not unlocked (detail: why), a skip, not a failure
    Qc3Only20V,          // QC2 Class B 20 V did not come although QC3 Class B reached 20 V in the same run (pattern qc3-20v-without-qc2-20v): a fail
    PdOnly,              // the source switched its output off on a PD-silent attach and stays off (pattern silent-sink-output-off): a skip, not a failure
    Capped,              // a PPS/AVS level the source accepted but whose output stopped at a steady ceiling below it (pattern output-capped-below-advertised): a fail
                         // for the compliance verdict, measured_mv = the ceiling; the levels after it still run (no chain skip)
    Withdrawn,            // pattern budget-reannounce-mid-run: the source re-announced its list mid-run without this row's offer: the row is skipped as
                         // "withdrawn by source", the unit goes on against the new list (a PDO that comes back later revives the row)
    WrongVoltage,         // pattern shared-bus-split: the attach's 5 V contract (PS_RDY at PDO 1, seen at any poll of the wait) was granted
                         // but the source holds another voltage on VBUS (outside vSafe5V): a fail, measured_mv = the highest VBUS seen during the wait
    NeedsLoad,            // pattern pps-off-at-no-load (suspected): a source that turns a PPS output off when the port draws ~0 mA. A programmable
                         // row it Accepted (no PS_RDY) and then dropped with VBUS collapsed at under kNeedsLoadMaxMa: a skip, not a failure
                         // (measured_mv = the collapsed VBUS); the levels after it still run (no chain skip)
    Floored,             // a low programmable row (PdLow) whose output stopped at a steady floor ABOVE the target: the source advertises 3.3 V
                         // but regulates at 5 V. A fail, measured_mv = the floor (the mirror of Capped's ceiling logic)
    StepResolution       // a PPS step-walk row whose +20 mV step the source did not follow (rounded to a coarser quantum or ignored):
                         // a fail, detail = the measured move in mV (0 = no move)
};
// Step.detail of a PPS step-walk row while it is Pending/Running (finish() overwrites it with the result); sits above every
// engine code and the EPR skip codes so the card and the tools never mistake it for one.
inline constexpr uint8_t kDetailStepWalk = 160;
// ROADMAP "PPS coverage" item 2: one step-walk row per PPS APDO, from the mid level, kStepWalkCount Requests of +kStepMv
// each. Every step needs Accept + PS_RDY and a measured VBUS move of kStepMv +- kStepTolMv. The tolerance is the meter's,
// not the pass band's: the INA226's VBUS LSB is 2.5 uV, aligned to 0.32 mV counts (align_vbus) and reported as whole mV, so
// each reading quantizes by 1 mV and a move of two readings carries at most 1 mV; the level's own band (max(100, mv/20))
// would accept a 100 mV rounding, so it is not used here.
inline constexpr unsigned kStepMv = 20, kStepWalkCount = 5;
inline constexpr int kStepTolMv = 2;
// A run that was resumed this many times is not offered again (a step that dies before it can leave a marker would loop forever).
inline constexpr unsigned kMaxResumes = 16;
// A unit (protocol) may take the K2 down this many times inside one run: losses 1 .. kRebootBudget-1 resume the same row, the
// last one skips what is left of the unit (RebootLimit) and the next unit runs. The count is journaled per unit.
inline constexpr unsigned kRebootBudget = 3;
// Boot policy: every interrupted run resumes by itself once the gate is ready (VBUS steady at 5 V); BACK held at boot cancels it.
constexpr bool resume_wanted(bool back_held, bool gate_ready) { return !back_held && gate_ready; }
// Detach / Attach are the two ends of every unit (see Unit below); they replace the old Handover / Reattach states in the same numbering.
enum class State : uint8_t { Idle, Baseline, Requesting, Holding, Releasing, Done, Stopped, Unsafe, Detach, Attach };
// Journal phase markers, reported through Env::phase() BEFORE the action they name. Handover = a Legacy attach begins (index of
// the unit's first pending row): a power loss there is charged to the unit like one in a Request (kRebootBudget, then RebootLimit).
enum class Phase : uint8_t { None, Request, Handover };
const char* name(Group);
const char* name(Verdict);
const char* name(Reason);
const char* name(State);
// A refusal as the screen words it: name() is the vocabulary the card export, the web tools and the report cards share
// (it never changes for a display budget); the reasons whose shared name does not fit the 26-27 cell footer lines say
// it shorter. The START preflight's log line uses these same words, so the log and the footer never disagree.
const char* refusal_words(Reason);
// The preflight refusal as the footer words it (the 27-cell footer line, so at most 27 chars): a WrongVoltage under a
// live PDO 1 contract names the measured level, e.g. "VBUS 8.5 V on a 5V contract"; every other reason uses its
// screen words. The log line carries the full "(shared bus)" phrase. Returns the length written to `out`.
size_t refusal_footer_text(Reason r, int vbus_mv, std::span<char> out);
// The sink holds a contract at PDO 1 (PS_RDY seen, contract position 1): the START's own 5 V Request was granted.
inline bool pdo1_contract(const pd::SinkStatus& pd) { return pd.state == pd::SinkState::Ready && pd.pos == 1; }

// A test unit is one attach of the charger from start to detach (docs/CHARGER_TEST_FLOW.md). The ids are the remote wire
// numbering (Cmd::ChargerUnits): the first twelve equal the Group values, PdLow is the risky tail of the PD rows.
enum class Unit : uint8_t { Pd, Qc2, Qc3, Qc3Plus, Afc, Fcp, Scp, Vooc, SuperVooc, SuperVooc65, Ufcs, Vivo, PdLow, Count };
static_assert(unsigned(Unit::Vivo) + 1 == unsigned(Group::Count));
inline constexpr size_t kMaxUnits = 16;
// How a unit attaches. Pd: Rd on, sink active (caps, an immediate Request, every re-announce answered). Legacy: Rd on, sink
// PD-silent (caps heard and recorded, never acknowledged), ready when PD is quiet and VBUS steady at 5 V.
enum class Mode : uint8_t { Pd, Legacy };
constexpr Mode mode(Unit u) { return u == Unit::Pd || u == Unit::PdLow ? Mode::Pd : Mode::Legacy; }
const char* name(Unit);
const char* name(Mode);
// How a unit boundary goes on a self-powered K2 (the supply is the source's own VBUS through a pinned Rd, so CC is never
// opened): SoftReset = a Pd unit re-announces its list with a PD Soft_Reset and requests PDO 1 again; SilentHold = a Legacy
// unit keeps the pinned Rd and the PD-silent sink (Auto-CRC off); NeedsExternalPower = the unit can make the source cut its
// output (the low PPS/AVS edge, pattern latch-off-low-pps), so it is skipped. A HID-powered K2 ignores this: detach + attach.
enum class SelfPath : uint8_t { SoftReset, SilentHold, NeedsExternalPower };
constexpr SelfPath self_path(Unit u) {
    return u == Unit::Pd ? SelfPath::SoftReset : u == Unit::PdLow ? SelfPath::NeedsExternalPower : SelfPath::SilentHold;
}
// The units of a run, in order. Empty (n == 0) = the default order: the OEM vendor modes by company, then Pd, then the
// legacy / standard protocols (QC, UFCS), then PdLow.
struct UnitList {
    std::array<Unit, kMaxUnits> u{};
    uint8_t n = 0;
    bool add(Unit x) { if (n >= u.size() || x >= Unit::Count) return false; u[n++] = x; return true; }
};
// Legacy attach: PD quiet for kLegacyQuietMs (longer than the ~1 s a source stays silent around its own hard reset) plus
// kLegacySteadyMs of vSafe5V means the source gave up PD. kLegacyGiveUpMs without that, the unit detaches and tries once more
// with kLegacyRetryMs (a source that gets no Request retries Source_Capabilities and hard-resets, 5.07 s cycle on the Baseus
// CJ21, before it disables PD); still no fallback = Reason::BlockedByPd and the unit's rows are skipped.
inline constexpr uint32_t kLegacyQuietMs = 2500, kLegacySteadyMs = 500, kLegacyGiveUpMs = 20000, kLegacyRetryMs = 60000;
// A session boundary (detach + fresh attach) happens only when the session mode changes:
// consecutive Legacy units share one PD-silent attach and only clean up the D+/D- lines in between (the engine's release holds
// them at 0 V, then Hi-Z). Waits before a unit's first handshake: kLegacyFirstWaitMs after the PD-silent attach (1000 ms),
// kLegacyGapMs after the previous unit's release (the 500 ms D+/D- discharge), plus kAfcPreMs more before AFC.
inline constexpr uint32_t kLegacyFirstWaitMs = 1000, kLegacyGapMs = 500, kAfcPreMs = 500;
// Detach: Rd off, wait until VBUS has been below vSafe0V for kDetachDarkHoldMs (at most kDetachDarkMs; a source that never lets
// go, a USB-A port, gets kDetachQuickMs from the second unit on).
inline constexpr uint32_t kDetachDarkMs = 10000, kDetachDarkHoldMs = 300, kDetachQuickMs = 1500;
// Pd attach: kAttachContractMs for the 5 V contract after the caps (pd::VbusWait covers VBUS + caps). Nothing back in time: the
// unit's rows fail with Reason::NoReattach; a PDO 1 contract whose PS_RDY was seen during the wait (latched in poll_attach)
// with VBUS outside vSafe5V fails with Reason::WrongVoltage instead (pattern shared-bus-split), measured_mv = the highest
// VBUS of the wait. A source-side Rp drop
// during the wait restarts the wait once (pattern slow-rp-wake: the standby pull-up answers ~1.1-1.7 s after a steady Rd).
inline constexpr uint32_t kAttachContractMs = 3000;
// A non-PD source that browned out gets this long (from the drop) to be back at vSafe5V before the run ends.
inline constexpr uint32_t kRecoverMs = 10000;
// pattern usb-a-cuts-on-low-dpdm: a USB-A port drops VBUS to ~2-2.5 V for ~4 s when D+/D- are held near 0 V for about a second or
// more (SCP detect, VOOC listen, VIVO; brief lows such as FCP/AFC/QC handshakes do not trigger it). An A-to-C cable's Rp is fed from
// VBUS, so CC vanishes with the output and comes back (a new attach session) a few tens of ms after VBUS is back at 5 V: after a
// legacy row's recovery has ended. Hardware (CUKTECH 120W, 2026-11-07): 80 ms after Releasing finished. A new session within this
// window of the end of such a recovery is that same sag's tail, not a new lost source. 5 s: the whole sag is ~4 s and the tail
// arrives <= ~0.1 s after the recovery, so 5 s covers a slow Rp return and a second brown-out step while a charger that really
// re-attached later (a hot-plug) still ends the run.
inline constexpr uint32_t kSagAdoptMs = 5000;
// Unsafe ("unplug; verify measured 5 V") guards against a source left ABOVE vSafe5V (a stuck contract or QC level that the K2
// could not release). It ends once nothing elevated can be left: VBUS under kUnsafeLowMv (the vSafe5V low end: a dark port, or a
// bank's 2 V standby after the reset, REMAX 10K USB-A 2026-11-08) for this long (a dark blip of a source that is still plugged
// in is shorter) acknowledges the stop and the page may start a new run. VBUS above vSafe5V never ends it.
inline constexpr uint32_t kUnsafeGoneMs = 1000;
inline constexpr int kUnsafeLowMv = pd::kVSafe5MinMv;
// What the first Legacy attach of the last run did; the report card keeps it with its duration (ms from the unit's detach).
// InContract is no longer produced (stored cards of the old flow still carry it).
enum class HandoverOutcome : uint8_t { NotTried, FellBack, StayedPd, Fault, Reboot, InContract };
struct HandoverInfo { HandoverOutcome outcome = HandoverOutcome::NotTried; uint32_t ms = 0; };
struct Options {
    uint32_t selected = kDefaultAlgorithms;
    UnitList units{};   // explicit unit list (remote command / ct_start.py --units); empty = the default order
    bool contains(Algorithm a) const { return selected & bit(a); }
};
struct Step {
    Group group = Group::Pd;
    Algorithm algorithm = Algorithm::Pd3Fixed;
    uint8_t pos = 0;
    // Layout: the four bytes before the first uint16_t fill the word (no padding hole), so a Step is 20 bytes, not 24:
    // the plan holds kMaxSteps of them. Designated initializers stay valid in this order (nothing names `tries`).
    uint8_t tries = 0;   // VOOC family: wake trains sent before the row ended (1..3); Pd rows: attempts (first included) after a retry, else 0
    uint16_t mv = 0, ma = 0;
    uint16_t measured_mv = 0;
    Verdict verdict = Verdict::Pending;
    Reason reason = Reason::None;
    uint8_t detail = 0;  // pd::Err, pd::SinkResult or fc::Fail according to reason/group.
    bool continuous = false;
    bool avs = false;
    bool hunt = false;   // OEM-mode probe (Xiaomi 6 A): request ma above the advertised limit, pass on the reply voltage
    uint32_t pdo = 0;
};
static_assert(sizeof(Step) == 20, "Step must stay free of padding holes: kMaxSteps copies live in the Runner");
// True while `s` is the step-walk row of its APDO and still in flight (the planner marks it with kDetailStepWalk).
constexpr bool is_step_walk(const Step& s) {
    return s.group == Group::Pd && s.algorithm == Algorithm::Pps && s.detail == kDetailStepWalk &&
           (s.verdict == Verdict::Pending || s.verdict == Verdict::Running);
}
// The UFCS mode a step samples, kept in `Step::pdo` (no UFCS row has a PDO): the planner writes it once the charger's
// Output_Capabilities are read, and the Results chip (the mode's top current), the Detail page (the whole mode) and the journal
// read it. Four fields in 32 bits, saturating: voltages in 100 mV (9 bits, up to 51.1 V), currents in 50 mA (7 bits, up to 6.35 A).
// A word of 0 is "no mode" (a row planned before the list was read, or any other protocol).
struct UfcsMode { unsigned min_mv = 0, max_mv = 0, min_ma = 0, max_ma = 0; };
constexpr uint32_t pack_ufcs_mode(int min_mv, int max_mv, int min_ma, int max_ma) {
    const auto field = [](int v, int unit, unsigned limit) { return v <= 0 ? 0u : std::min<unsigned>(unsigned(v) / unsigned(unit), limit); };
    return field(min_mv, 100, 511) | field(max_mv, 100, 511) << 9 | field(min_ma, 50, 127) << 18 | field(max_ma, 50, 127) << 25;
}
// True with the mode in `out` when `s` is a UFCS step that carries one.
constexpr bool ufcs_mode_of(const Step& s, UfcsMode& out) {
    if (s.group != Group::Ufcs || !s.pdo) return false;
    out = {(s.pdo & 511u) * 100u, (s.pdo >> 9 & 511u) * 100u, (s.pdo >> 18 & 127u) * 50u, (s.pdo >> 25 & 127u) * 50u};
    return true;
}
struct Input {
    uint32_t now = 0, session = 0, cap_mv = 0;
    uint16_t cable_mv = 20000, cable_ma = 3000;
    // The cable VDO limits of the identity the K2 itself presents (an armed spoof; 0 = unknown). When nothing physical was read they are
    // the cable limit the source saw, and what the hunt classifier uses instead of the 3 A default of an unread cable. Diagnostics and
    // the "not unlocked" cause only: row planning keeps reading cable_mv / cable_ma.
    uint16_t spoof_mv = 0, spoof_ma = 0;
    int vbus_mv = -1, ibus_ma = 0;   // ibus_ma: magnitude; above kLoadAbortMa during a raised row means a load
    uint32_t pd_quiet_ms = UINT32_MAX;   // ms since the last PD frame was received (since the handover began)
    bool inline_load = false, fake = false, busy = false, cable_observed = false, pd_traffic = false, spoof_on = false;
    // The K2 runs on the source's own VBUS through a pinned Rd (fusb302::self_powered()). start() and resume() latch it for the
    // run: a unit boundary must not drop VBUS (the K2 would die), and a VBUS loss suspends the run instead of ending it.
    bool self_powered = false;
    // The source sent PD messages at some point since this attach (fusb302::pd_seen_attach()). With self_powered and no source
    // list at START it means the source gave PD up (a silent K2 never GoodCRC'd its caps): the PD rows say "PD gave up before START".
    bool pd_seen_attach = false;
    pd::Revision reported{};
    pd::SinkStatus pd{};
    fc::State fc_state = fc::State::Idle;
    fc::Fail fc_fail = fc::Fail::None;
    uint8_t fc_tries = 0;   // fctrig::Status::tries
    bool probe_done = false;   // the source probe the runner asked for has finished (answered, refused or timed out)
    bool epr_proof = false;    // set by the Runner, not the caller: the source accepted EPR entry earlier in this run (cable proven)
    const pd::SourceCaps* epr_info = nullptr;   // the list the source answered to an SPR-mode EPR_Get_Source_Cap (valid + epr once answered)
    const pd::XiaomiTrace* xm = nullptr;        // the sink's chain / Request trace (null = none: a refused hunt row still records what it can)
    // What the engine's SCP / UFCS sessions read from the charger (fctrig::caps()). The Runner trusts it only after a row of the
    // run's own unit ran: START plans without it (the engine may still hold an earlier charger's list), and the rows still to run
    // are planned again once the unit's first row has read it.
    const fc::Caps* fc_caps = nullptr;
};
// The runner waits for the source probe at most this long, so a charger that ignores it costs the run under 1 s.
inline constexpr uint32_t kProbeMaxMs = 1000;
inline constexpr uint32_t kCapsReturnMs = 8000;   // how long an invalid cap list (hard reset, re-announce) is tolerated
// Restart rules for a mid-run re-announce (user decision 2026-11-09): after any new list (Source_Capabilities, or a
// complete EPR_Source_Capabilities outside the K2's own EPR entry/exit) no Request is sent until the list has been
// unchanged for kListSettleMs; each list that arrives inside the window restarts it. The sink's single-Request gate
// still applies (the Runner sends every Request through it).
inline constexpr uint32_t kListSettleMs = 1500;
inline constexpr uint32_t kEprInfoBusyMs = 1000;   // how long the informational EPR_Get_Source_Cap retries a busy line
inline constexpr uint32_t kEprReadyMs = 8000;   // how long EPR entry waits for a live contract to enter from
// pattern epr-entry-hard-reset (Baseus 140W, 2026-11-05): the source Hard Resets in the middle of the EPR entry (after our
// chunk-1 request, ~75 ms later). The entry is asked again after the source's SPR list and 5 V contract have been steady for
// kEprSettleMs (a source that has just reset is still re-arming), at most kEprEntryRetries more times; each retry waits one
// kEprSettleStepMs longer. Pattern reject-then-reset: the step after a Reject waits kRejectSettleMs before an EPR entry.
inline constexpr unsigned kEprEntryRetries = 2;
inline constexpr uint32_t kEprSettleMs = 2000, kEprSettleStepMs = 2000, kRejectSettleMs = 1500;
// The Xiaomi / CUKTECH auth chain (SVID 0x2717) finishes ~0.7 s after the contract and then the APDO limits rise to 6 A (hardware
// 2026-11-06): a 6 A hunt row waits for it, bounded, so a chain that never finishes costs the run this long once per attach.
inline constexpr uint32_t kXiaomiAuthWaitMs = 2000;
// A 6 A hunt row goes out only when the list in hand advertises 6 A for it. The source re-announces its list right after the chain
// (the sink stamps it, XiaomiTrace::reannounced): a row whose chain just finished waits this long for that list before it ends.
inline constexpr uint32_t kXiaomiReannounceMs = 400;
inline constexpr int kLoadAbortMa = 100;
// Pattern pps-off-at-no-load: the port drew less than this (a reading at the power monitor's noise floor) in the whole row.
inline constexpr int kNeedsLoadMaxMa = 50;
enum EvidenceFlag : uint8_t {
    Rmdo = 1, PpsOffer = 2, EprTable = 4, SprAvsOffer = 8, EprCapable = 16,
    PhysicalCable = 32, PartialEpr = 64, SpoofOn = 128
};
// What the source's own messages showed during the run (Evidence::seen); latched by the Runner, passive notes only.
enum SeenFlag : uint8_t { SeenOppoSvid = 1 };
struct Evidence {
    uint32_t selected = 0, session = 0;
    uint16_t monitor_mv = 0, cable_mv = 20000, cable_ma = 3000, raw_header = 0;
    uint16_t scp_ma = 0;   // the SCP current limit the adapter reported (0 = not read); a card keeps it in 50 mA units
    uint8_t wire_rev = 0, flags = 0, num_pdos = 0, seen = 0;
    uint8_t rebalanced = 0;   // how often the source re-announced its list mid-run and the PD rows were re-planned per row (budget-reannounce-mid-run)
    pd::Revision reported{};
};
struct Summary {
    uint8_t total = 0, runnable = 0, skipped = 0;
    uint16_t max_mv = 0;
    uint32_t estimate_ms = 500;
    Reason refusal = Reason::None;
    uint16_t refusal_mv = 0;   // the measured VBUS (mV) behind a Release / WrongVoltage refusal (0 = none)
};
const char* explanation(const Step&);
// Step.detail of an EPR skip. The PD group's detail space is 128 + pd::Err (below 136) and pd::SinkResult (below 12); these sit
// above it so the card (five code bits behind the 128 flag) and the tools never mistake them for an engine code.
//   EprRefused: kDetailEprFail + the source's Enter_Failed data (0..5)      OverCap on an EPR row: the monitor's ceiling
//   CableLimit on an EPR row: kDetailEprOffered = the charger listed this EPR level (EPR_Get_Source_Cap in SPR mode) and the cable
//   cannot carry it; kDetailEprWithheld = the ask was answered with no EPR PDO from a source whose PDO 1 has the EPR bit (the port
//   holds the list back without an EPR cable); kDetailEprOfferedRefused + the Enter_Failed data (0..5) = the level was listed AND the
//   entry was then refused (the entry's answer is added to the listed level, never replaces it; 149..154 fit the card's 159)
inline constexpr uint8_t kDetailEprFail = 136, kDetailMonitor36 = 144, kDetailMonitor48 = 145, kDetailMonitorOther = 146, kDetailEprOffered = 147,
                         kDetailEprWithheld = 148, kDetailEprOfferedRefused = 149;   // 149..154 are the EPR entry answers
// NotUnlocked (a 6 A hunt row the source refused or ignored): why it counts as "not unlocked" and not as a refusal. NoChain = the source
// never listed SVID 0x2717, ChainUnfinished = it listed it but the auth chain did not finish inside kXiaomiAuthWaitMs, CableCap = the
// chain finished and the cable is not known to carry more than 3 A (a far-end K2 never reads it; a refusal with a read 5 A cable stays
// a FAIL). VendorRefused = the chain finished and the K2 presented a spoofed identity whose cable VDO claims 5 A, so the cable is not what
// the source held back: it did not accept the identity as the vendor's own (sniffed Kowsi clones on a CUKTECH 120 W C1, 2026-11-07).
// SourceReset = the source answered the Request with a Hard Reset (pattern hard-reset-on-6a-over-offer): the chain and the cable
// leave no better reason (the cable was not read, or it is proven 5 A and the chain finished), so the row says what happened.
// 155..159 fit the card's five detail bits (up to 159) and sit above every pd::Err and sink result.
inline constexpr uint8_t kDetailNoChain = 155, kDetailChainUnfinished = 156, kDetailCableCap = 157, kDetailVendorRefused = 158,
                         kDetailSourceReset = 159;
// ---- Hunt evidence ----------------------------------------------------------------------------------------------------------------
// A 6 A hunt row the source refused (Reject / Wait / no answer) leaves a compact snapshot of what the sink saw: the Xiaomi chain, the
// list the Request referred to, the cable limit the run used and the source's answer. classify_hunt() names the cause where the
// evidence decides it. Diagnostics only: nothing in the run reads it back and no verdict depends on it. It lives in a small side
// table (Runner::hunt_evidence), not in Step.detail: a Fail/Request row's detail already carries the sink result.
enum class HuntCause : uint8_t {
    None,                 // not a refusal (no evidence recorded)
    NoChain,              // the source never listed SVID 0x2717: there was nothing to unlock
    ChainStalled,         // listed, but the auth chain never reached 0x2717020A (last_step says where it stopped)
    InsideChain,          // our Request left while the chain was still running: that kills the chain
    RequestHeld,          // the chain guard still held our Request when the row ended: it never went out
    FixedPdo,             // the Request referred to a fixed PDO: a fixed PDO never offers 6 A
    CableCap,             // the cable limit the run used (unread / 3 A) is below 5 A
    ApdoStale,            // authed, but no Source_Capabilities after it: the APDO in force was still the pre-auth 5 A
    CableCapped5A,        // re-announced and the APDO is still 5 A with a 5 A cable: the source capped it by the cable
    RefusedDespiteOffer,  // the APDO in force offered the asked current and the source still refused
    Unexplained,          // refused, but the evidence is incomplete (no trace, or no Request of this row found)
    Count
};
const char* name(HuntCause);          // short word for logs and the wire ("apdo-stale")
const char* explanation(HuntCause);   // the detail page / summary wording (fits 26 chars)
enum HuntFlag : uint16_t {
    HfSvid = 1, HfOpener = 2, HfAuthed = 4, HfReannounced = 8, HfInChain = 16, HfHeld = 32, HfSent = 64,
    HfCableRead = 128, HfCableInferred = 256, HfTimeout = 512, HfHardReset = 1024   // HfHardReset: answered by a source Hard Reset
};
struct HuntEvidence {
    uint32_t apdo = 0;          // PDO word the Request referred to (0 = no Request of this row went out)
    uint32_t rdo = 0;           // the Request word as sent
    uint16_t flags = 0;         // HuntFlag
    uint16_t apdo_ma = 0;       // the current limit that PDO advertised at the asked voltage
    uint16_t req_mv = 0, req_ma = 0;   // what was asked (effective voltage, operating current)
    uint16_t cable_ma = 0;      // the cable limit the run used
    uint16_t authed_ms = 0;     // ms from 0x2717020A to the refusal (saturates; 0 when not authed)
    uint16_t answer_ms = 0;     // ms from the Request to the source's answer (0 = none, saturates)
    uint8_t step = 0;           // index of the row in Runner::steps()
    uint8_t cause = 0;          // HuntCause
    uint8_t answer = 0;         // pd::ReqAnswer; None + HfTimeout = the source never answered
    uint8_t last_step = 0;      // last 0x2717020x the source sent (0 = none)
    uint8_t caps = 0;           // PDOs in the list in force at the Request
    uint8_t pos = 0;            // object position asked
    uint8_t type = 0;           // pd::PdoType of the PDO asked
};
// The cause the evidence decides; pure, in the order the table in docs/CHARGER_TEST_FLOW.md lists them.
HuntCause classify_hunt(const HuntEvidence&);
inline constexpr size_t kHuntSlots = 4;   // a plan has at most three hunt rows (11 V, 20 V, the fixed 20 V); one spare

constexpr uint8_t monitor_detail(uint32_t cap_mv) { return cap_mv == 36000 ? kDetailMonitor36 : cap_mv == 48000 ? kDetailMonitor48 : kDetailMonitorOther; }
// The EPR wording of a detail value above, nullptr for any other value.
const char* epr_detail_name(uint8_t detail);
// The unit a row belongs to: its group, except the low programmable PD rows (PdLow).
Unit unit_of(const Step&);
// The order the planner takes the algorithms in inside one unit (the plan's rows are unit-major, then in this order). A resumed run's
// rows are put back in the plan's order from it (charger_journal.hpp), so the planner and the journal read the same table.
inline constexpr Algorithm kPlanOrder[] = {
    Algorithm::Pd2Fixed, Algorithm::Pd3Fixed, Algorithm::Epr, Algorithm::SprAvs, Algorithm::Pps, Algorithm::EprAvs,
    Algorithm::Pd30Revision, Algorithm::Pd31Revision, Algorithm::Pd32Revision, Algorithm::Qc2,
    Algorithm::Qc3, Algorithm::Qc3Plus, Algorithm::Afc, Algorithm::Fcp, Algorithm::Scp,
    Algorithm::Vooc, Algorithm::SuperVooc, Algorithm::SuperVooc65, Algorithm::Ufcs, Algorithm::Vivo, Algorithm::Qc2ClassB, Algorithm::Qc3ClassB
};
static_assert(std::size(kPlanOrder) == unsigned(Algorithm::Count));
// The units in the order a run with these options executes them (what the Runner resolves its unit list to).
UnitList unit_order(const Options&);
// One verdict for a unit's rows: Pending (something left), Fail (any), Pass (any, no fail), Skip (nothing ran).
Verdict unit_outcome(std::span<const Step>, Unit);
struct Env {
    virtual pd::Err request_pd(pd::Choice) = 0;
    // `ma`: the row's current (SCP: the limit to write; 0 = detect only). Other protocols ignore it.
    virtual fc::Fail request_fc(fc::Proto, int mv, bool retarget, int ma) = 0;
    virtual void release(bool pd) = 0;
    // One unit's attach (docs/CHARGER_TEST_FLOW.md). detach(): Rd off, the 5 V default, the source sees a detach and drops its
    // output. attach(mode): Rd on again as a fresh attach, the sink PD-active (Pd: caps, an immediate PDO 1 request) or PD-silent
    // (Legacy: caps are heard, never acknowledged). The runner watches VBUS itself.
    virtual void detach() {}
    virtual void attach(Mode) {}
    // The cable identity (the run's Ra mode + e-marker spoof, when the user picked one) for the NEXT attach: on before a Pd-mode
    // unit's attach, off before a Legacy one, called while detached (Rd off). The Ra mode makes a charger ask for the cable and
    // some (Baseus 70W) then ignore every D+/D- handshake, so only the PD units run under it. Never called on a self-powered run:
    // its pinned Rd cannot be opened, so the boot-time Ra stays for the whole run.
    virtual void identity(bool on) { (void)on; }
    // A self-powered K2's unit boundary (the run latched Input::self_powered): the pinned Rd stays, CC is never opened and VBUS
    // never drops. hold(Pd) = PD Soft_Reset, the sink PD-active again (fresh caps, PDO 1 request); hold(Legacy) = the sink goes
    // PD-silent. The runner never calls detach() or pd_rescan() on such a run.
    virtual void hold(Mode) {}
    // The sink policy comes back when a run leaves a Legacy attach (done, stopped, unsafe): legacy_only(false), then pd_rescan().
    virtual void legacy_only(bool on) { (void)on; }
    virtual void pd_rescan() {}
    // Called before an above-5 V request (Phase::Request, step index) and before a Legacy unit's attach (Phase::Handover,
    // index of the unit's first row). The journal writes its marker here, from the main loop.
    virtual void phase(Phase, size_t index) { (void)index; }
    // restore() charges one power loss to a unit (`unit` = its Unit number, count = losses so far in this run): the journal keeps
    // it, so the budget survives every further loss.
    virtual void attempt(size_t unit, uint8_t count) { (void)unit; (void)count; }
    // Asked once per run, in the settled 5 V contract when the first Pd unit is over: true starts a source probe (identity
    // questions to the charger) and the runner then waits for Input::probe_done or kProbeMaxMs; false = no probe,
    // nothing to wait for. probe_identity(false) ends it (the runner calls it after the wait, on cancel and on a fault).
    virtual bool probe_identity(bool on) { (void)on; return false; }
    // Ask the source for its EPR source list while the sink stays in SPR mode (no EPR entry): Ok = asked (the answer arrives as
    // Input::pd.epr_info and Input::epr_info), Busy = the line is occupied (the runner retries briefly), anything else = no answer
    // to wait for.
    virtual pd::Err epr_info_ask() { return pd::Err::NoCaps; }
    // The PD rows were planned again from a list heard later and the row layout changed (rows after the PD block moved, or the
    // count differs): a journal that already holds the plan has to be written again. Not called for an identical re-plan.
    virtual void plan_changed() {}
    // False while the journal still has to write the plan's SHAPE (a replan's section files and the header that names them): the
    // phase marker names a row by place in those files, so an above-5 V request waits in the Baseline until this is true, the
    // page writing one file per tick meanwhile. Never blocks: the Runner asks again at every poll. Result changes of finished
    // rows do not count (a stale verdict only makes a resumed run repeat that row).
    virtual bool journal_ready() { return true; }
    // The wait above ended after `ms` (logged by the page as [CJRN] wait-for-replan).
    virtual void journal_waited(uint32_t ms) { (void)ms; }
    // The run ends for good (a verdict, a cancel, a fault, a lost source on a K2 that cannot wait): called ONCE, before the rows
    // that are still pending are skipped. The journal is closed here, first, so a reset between the skip rows cannot leave a
    // stopped run behind that the next boot resumes. Not called for a suspended run (that one is only interrupted).
    virtual void closing(Reason) {}
    virtual void record(const Step&) = 0;
    // pattern budget-reannounce-mid-run: the source announced its list again mid-run (same wire revision, none of the excluded
    // cases) and the PD rows were re-planned per row instead of the unit ending as "offers changed": `follow` rows take the new
    // slot and PDO word, `withdrawn` rows have no offer any more (Reason::Withdrawn), `back` rows a PDO came back for. Called once
    // per re-plan; the page logs it and the count is noted on the results card (Evidence::rebalanced).
    virtual void replan(unsigned follow, unsigned withdrawn, unsigned back) { (void)follow; (void)withdrawn; (void)back; }
    // A hunt row the source refused: the evidence snapshot is final (called once per row, right before record()). The page logs it,
    // marks the Black Box and freezes a capture; nothing here changes the row's verdict.
    virtual void hunt_evidence(const Step&, const HuntEvidence&) {}
protected:
    ~Env() = default;
};

// pattern burst-mode-ripple-at-20v: an unloaded charger in burst mode swings VBUS around its level (TOPADRE 65 W at 20 V:
// 18.39-20.70 V at 20 Hz, mean 19.80 V, sigma 0.70 V, 22 % of the samples outside the +-5 % band). A fixed PD row (above 5 V)
// is judged on a running mean of VBUS with time constant kBandTauMs, not on every sample. 250 ms = five samples at 20 Hz: the
// mean's sigma is ~0.33 x the ripple's (0.23 V for sigma 0.70 V, a quarter of the 1 V band at 20 V), and a level that really
// stepped 1.5 V away crosses the band in ~1.1 tau (275 ms), inside the 500 ms the mean must stay in band (Requesting
// stability time). The average is weighted by the time between polls, so the poll rate (faster than 20 Hz) does not matter.
// A single sample beyond kBandGrossPct of the level (three times the band) is not ripple: it fails the band at once and
// restarts the average. The overvoltage guards (OverCap, CableLimit) never see the mean, they read the raw sample.
// (Four bytes of state, not a sample window: the K2_NET=1 build has 8 bytes of .bss headroom against the stack floor.)
inline constexpr uint32_t kBandTauMs = 250;
inline constexpr unsigned kBandGrossPct = 15;
// pattern output-capped-below-advertised: a PPS / AVS level the source accepted (the contract is live) whose output never reached
// the band is Capped, not a plain timeout, when VBUS sits above vSafe5V and steady below the target. Steady = the samples stay
// within 1 % (at least 100 mV) of one level for kPlateauMs; or the level matches within 1 % the highest level a passed PD row of
// the run measured (the converter's ceiling already seen on a fixed row).
inline constexpr uint32_t kPlateauMs = 3000;
inline constexpr int kCappedFloorMv = 5500;
class VbusMean {
public:
    void clear() { q8_ = -1; }
    // dt_ms: time since the previous sample. The first sample after clear() seeds the mean.
    void add(int mv, uint32_t dt_ms) {
        if (q8_ < 0) { q8_ = mv * 256; return; }
        const int64_t diff = int64_t(mv) * 256 - q8_;
        q8_ += int32_t(diff * std::min(dt_ms, kBandTauMs) / int64_t(kBandTauMs));
    }
    int mean() const { return q8_ < 0 ? -1 : (q8_ + 128) >> 8; }   // mV, -1 when there is no sample
private:
    int32_t q8_ = -1;   // mV in 1/256 mV
};

class Runner {
public:
    explicit Runner(Env& env) : env_(env) {}
    static Summary preview(const pd::SourceCaps&, Options, Input);
    Reason start(const pd::SourceCaps&, Options, Input, bool confirmed);
    void poll(Input, const pd::SourceCaps&);
    // A START the preflight refused (no run began; the run id stays where it was): one [TEST] log line with the
    // reason, and the reason + measured VBUS recorded here, so the remote summary (charger-test status) and the
    // card's final-reason path read them. A WrongVoltage under a live PDO 1 contract names the level in the log:
    // "VBUS 8.5 V on a 5 V contract (shared bus)". Only outside a run.
    void note_preflight_refusal(Reason r, int vbus_mv);
    uint16_t refusal_mv() const { return refusal_mv_; }   // the measured VBUS of the recorded preflight refusal
    // While held, a run that has just been start()ed stays in its Baseline: nothing is judged or requested, and the Baseline's
    // clocks restart at every poll (so its 500 ms of vSafe5V count from the release). The page holds a run while it writes the
    // journal's plan in pieces, so the first phase marker (the one write that must precede an above-5 V request) lands after the
    // header. start() clears it; it only acts in the Baseline.
    void hold(bool on) { held_ = on; }
    // The next above-5 V request is parked in the Baseline until Env::journal_ready() (no contract held, so nothing is in danger
    // meanwhile); the page pumps its journal faster then.
    bool waiting_journal() const { return jwait_ms_ != 0 && state_ == State::Baseline; }
    void cancel(Input);
    // Reboot resume. restore() rebuilds a journaled run into State::Stopped (nothing is requested). The interrupted step
    // (an open Request / Handover marker) is not a verdict on the charger: it stays Pending and is run again, and the loss is
    // charged to its unit through Env::attempt(). `attempts` = losses already charged to unit `attempt_unit`: when this loss is the
    // kRebootBudget-th, every pending row of the unit is skipped (RebootLimit) and the run goes on with the next unit.
    // resume() continues from the first pending step through the normal Baseline (vSafe5V
    // for 500 ms, then the usual guards). A Pd unit goes on in the live attach, a Legacy unit does its normal detach and
    // PD-silent attach (the same sequence as in an uninterrupted run), never above 5 V by itself, and the units after it
    // attach as usual. A run resumed kMaxResumes times is not resumable any more.
    // `planned_reattach` = this boot's /rd.rec marker said Reattach (the charger test's planned re-attach rebooted the
    // K2): the resume() this boot performs is that re-attach, not a power loss, and is not counted in resumed_.
    void restore(const Options&, const Evidence&, uint32_t run_id, std::span<const Step>, Phase, size_t phase_index, uint8_t resumed,
                 size_t attempt_unit = 0, uint8_t attempts = 0, bool planned_reattach = false);
    bool resumable() const;
    // A self-powered run that lost VBUS: Stopped, every unfinished row still Pending (nothing skipped, no verdict), resumable
    // from memory when the power comes back, and from the journal after a reboot. The page must not close the journal then.
    bool suspended() const { return suspended_; }
    Reason resume(Input, bool confirmed);
    uint8_t resumed() const { return resumed_; }
    uint8_t reattach_count() const { return reattach_; }   // how many times the run went through the planned re-attach
    bool busy() const { return (state_ >= State::Baseline && state_ <= State::Releasing) || state_ == State::Unsafe || state_ == State::Detach || state_ == State::Attach; }
    // The page scans for the source's Rp while a unit attaches (the main loop's own scan is too slow for a fast announcer).
    bool attaching() const { return state_ == State::Attach; }
    State state() const { return state_; }
    // A row is in flight: its Request has gone out (or is about to be answered) and its verdict is not recorded yet. The main
    // loop also serves the protocol engines (a QC pulse train, a UFCS Accept, a PD transition), so the page paints and journals
    // nothing heavy then (a full LCD redraw is 100-200 ms, a littlefs erase masks interrupts). Baseline, Releasing, the unit
    // attach / detach states and the end of the run are the gaps in which that work is done. Derived from the state: no storage.
    bool quiet() const { return state_ == State::Requesting || state_ == State::Holding; }
    // Nothing may paint at all: a row is in flight AND the fast-charge engine is inside a window that only the main loop serves
    // (`engine_window` = fctrig::paint_critical(): the HVDCP handshake pulses, the VOOC receive slices, the UFCS handshake, the
    // vivo UART frames; the LCD idle hook serves the PD engine, the UFCS Ping..Keep exchange and the transition capture, so those
    // tolerate a bounded draw). quiet() alone keeps deciding the journal; this decides the screen. It takes the engine's state
    // as a value so the page reads it live (the Runner's last Input is up to a tick old) and a test can drive it.
    bool paint_blocked(bool engine_window) const { return engine_window && quiet(); }
    // The row the run begins next (the first Pending one in unit order), SIZE_MAX when none is left. During a gap the page
    // highlights it, so the list is already on that row when the next quiet stretch begins.
    size_t upcoming() const { return pick(); }
    Unit unit() const { return unit_; }   // the unit being run (Unit::Count before the first one starts)
    // The run's units in execution order (the rows of steps() follow the same order).
    std::span<const Unit> unit_order() const { return {order_.data(), n_order_}; }
    Mode attach_mode() const { return mode_; }
    // The first Legacy attach of the run (report card): FellBack = quiet and steady at 5 V, StayedPd = blocked by PD.
    HandoverInfo handover() const { return handover_; }
    Reason reason() const { return reason_; }
    uint32_t run_id() const { return run_id_; }
    uint32_t revision() const { return revision_; }
    const Evidence& evidence() const { return evidence_; }
    size_t index() const { return index_; }
    std::span<const Step> steps() const { return {steps_.data(), count_}; }
    uint32_t hold_left_ms(uint32_t now) const;
    // The refusal evidence of this run's hunt rows (a few rows at most), in the order they were refused.
    std::span<const HuntEvidence> hunt_evidence() const { return {hunt_.data(), n_hunt_}; }
    const HuntEvidence* hunt_evidence_for(size_t step) const;
private:
    void note_hunt_refusal(const Input&, bool timeout, uint16_t extra_flags = 0);
    void add(Step);
    void build(const pd::SourceCaps&, Options, Input);
    void caps_evidence(const pd::SourceCaps&);
    void replan_pd(const pd::SourceCaps&, Input, bool refresh_evidence);
    void replan_fc(Input);   // the SCP / UFCS rows still to run, planned again from what the unit's first row read (fc::Caps)
    void regrow_plan(const pd::SourceCaps&, Input);
    void follow_list(const pd::SourceCaps&, Input);   // the source announced again mid-run (pattern budget-reannounce-mid-run): per PD row the
    // new list is followed where it still offers the row (new slot and PDO word), rows with no offer are skipped as Withdrawn, rows a
    // withdrawn PDO comes back for are Pending again; before the first PD row the plan is rebuilt from the new list (replan_pd).
    // A COMPLETE EPR_Source_Capabilities outside the K2's own EPR entry/exit (epr_stage_ == 2) is a re-announce like any other (2026-11-09);
    // only a partial chunk stays excluded. When the row in flight was re-placed (new slot / word) it is restarted there once the list
    // has settled - the restart is not a try (flight_restart_).
    bool list_settled(uint32_t now) const { return list_ms_ == 0 || now - list_ms_ >= kListSettleMs; }   // rule 1: no Request until the list in hand is kListSettleMs unchanged
    // pattern reject-then-reduced-list: the running hunt row's own Request was Rejected (hunt_own_reject), or the hunt row that was
    // Rejected is still being released (hunt_reject_until_): the list announced again is the source's answer to that probe, taken as
    // the new offer (adopt_list: rows it still offers follow it, rows it dropped are skipped as not offered).
    bool hunt_own_reject(const Input&) const;
    void adopt_list(const pd::SourceCaps&);
    void record_pd_skips();
    void epr_skips(Reason, uint8_t detail = 0);   // detail: the source's Enter_Failed code for EprRefused
    bool info_ready(Input) const;                 // the EPR list the charger answered is usable
    bool ask_info(Input);                         // false while the informational EPR list is still being asked, true when done
    void epr_info_rows(const pd::SourceCaps&, Input);   // lay the EPR rows out per level from the info list (when one was heard)
    void request(Input);
    void finish(Input, Verdict, Reason, uint8_t detail = 0);
    void release(Input, bool stop);
    void reissue_release();
    void next(Input);
    size_t pick() const;
    void skip_pending(Reason);
    void suspend();
    bool unfinished() const;   // a row is still Pending or Running
    void skip_unit(Reason);
    void skip_legacy_units(Reason);   // every Legacy unit's pending rows (the PD-only answer: no later PD-silent attach)
    bool legacy_row_ran() const;      // a Legacy row of the run has a protocol verdict (Pass/Fail)
    void begin_attach(Input);
    void end_run(Input);
    void unsafe(Reason);
    void begin_unit(Unit, Input);
    void continue_legacy(Unit, Input);
    void begin_detach(Input);
    void poll_detach(Input);
    void poll_attach(Input, const pd::SourceCaps&, uint32_t gap);
    void fail_unit(Input, const pd::SourceCaps&, Reason = Reason::NoReattach, int measured_mv = -1);
    // ^ measured_mv < 0: record the live in.vbus_mv (a genuine NoReattach keeps the reading at the fail, not a clamped 0)
    void end_legacy();
    void end_probe();
    void end_sag_recovery(const Input& in);
    void retry_pd_row(Input in);
    void poll_impl(Input, const pd::SourceCaps&);
    uint32_t hold_ms() const;
    Env& env_;
    Evidence evidence_;
    std::array<Step, kMaxSteps> steps_{};
    size_t count_ = 0, index_ = 0, pd_retry_ = SIZE_MAX;
    uint8_t retries_ = 0;         // retries the row pd_retry_ has had (a source Hard Reset or loss in the row)
    uint8_t epr_tries_ = 0;       // retries of the baseline EPR entry (a source Hard Reset in the entry)
    uint32_t settle_ms_ = 0;      // extra steady 5 V a retry waits for in Releasing (kEprSettleMs + backoff)
    uint32_t settle_until_ = 0;   // an EPR entry request waits for this time (after a Reject: kRejectSettleMs)
    uint32_t hunt_reject_until_ = 0;   // a hunt row was Rejected: until then (kCapsReturnMs) a list announced again is the answer to it; cleared by the next row
    bool can_retry(const Step&) const;
    // A 6 A hunt row waits for the Xiaomi auth chain (Input::pd.xiaomi_svid && !xiaomi_authed), at most kXiaomiAuthWaitMs per
    // attach: auth_key_ = session + Hard Reset count the budget was opened for, auth_at_ = when. A row the list in hand does not
    // advertise also gives the re-announced list kXiaomiReannounceMs after the chain.
    bool hunt_auth_wait(const Input&, const Step& row);
    // A 6 A hunt row that cannot go out (the list in hand does not advertise its 6 A) ends here without any Request: a source that
    // never listed SVID 0x2717 drops the row from the plan (nothing to report), any other one skips it as "not unlocked".
    void skip_unadvertised_hunt(Input in);
    void drop_row(size_t i);
    bool auth_open_ = false; uint32_t auth_key_ = 0, auth_at_ = 0;
    // pattern preauth-list-after-reset: a Xiaomi-chain source that Hard Reset announces its fixed PDOs alone until its chain has run
    // again. xiaomi_seen_ = the run saw SVID 0x2717 (the sink forgets it on a Hard Reset); xiaomi_reset_ = a Hard Reset hit this
    // run after that, cleared when the chain is done again; tail_at_ = when the cut-short list was first seen (0 = not seen).
    // x_svid_ / x_authed_ = the sink's chain state at the last poll before any pending Hard Reset (the sink clears it on the reset).
    bool preauth_tail(const Input&, bool& waiting);
    bool band_ok(const Step&, const Input&, uint32_t gap);   // the band check of a Requesting / Holding row (window mean for a fixed PD row)
    pd::Choice pd_choice(const Step&) const;   // the Request a PD row sends now (the step-walk row walks walk_mv_)
    VbusMean band_;
    bool capped(const Step&, const Input&, bool ready) const;   // a programmable row's timeout is a ceiling (see kPlateauMs)
    bool floored(const Step&, const Input&, bool ready) const;  // ... or a floor: a low row whose output sits steady above the band ceiling
    void track_plateau(const Input&, bool ready, uint32_t gap);
    uint16_t plat_mv_ = 0;     // the level a Requesting programmable row's samples have stayed within 1 % of (0 = none yet)
    uint32_t plat_ms_ = 0;     // since when
    // The step-walk row in flight (is_step_walk): walk_mv_ = the target of the Request out now (s.mv at the first one,
    // +kStepMv per granted step), walk_n_ = levels granted after the base, walk_prev_mv_ = VBUS at the previous granted level.
    uint16_t walk_mv_ = 0;
    uint8_t walk_n_ = 0;
    int walk_prev_mv_ = -1;
    void walk_step(Input, const Step& eff);   // one granted level of the walk: check its move, ask for the next or finish
    bool xiaomi_seen_ = false, xiaomi_reset_ = false, x_svid_ = false, x_authed_ = false; uint32_t tail_at_ = 0, hr_count_ = 0;
    std::array<HuntEvidence, kHuntSlots> hunt_{};   // refused hunt rows; n_hunt_ in use (a full table overwrites the last slot)
    uint8_t n_hunt_ = 0;
    uint8_t hunt_seq_ = 0;   // XiaomiTrace::req_n when the running hunt row asked: a different number later = a Request of this row went out
    size_t cut_index_ = SIZE_MAX;   // a low programmable row whose source went dark: Releasing decides replug-or-carry-on
    State state_ = State::Idle;
    Reason reason_ = Reason::None;
    uint16_t refusal_mv_ = 0;   // the measured VBUS of the last preflight refusal (note_preflight_refusal)
    uint32_t release_ms_ = 0, phase_ms_ = 0, stable_ms_ = 0, last_ms_ = 0, session_ = 0, resets_ = 0;
    uint32_t run_id_ = 0, revision_ = 0;
    uint8_t resumed_ = 0;
    uint8_t reattach_ = 0;        // planned re-attaches this run went through (not counted in resumed_)
    bool reattach_planned_ = false;   // latched from restore(): this boot came back from the planned re-attach
    bool loaded_ = false;
    bool selfpwr_ = false;     // latched from Input::self_powered at start()/resume(): fusb302::self_powered() flips with a detach
    bool suspended_ = false;   // a self-powered VBUS loss ended the run without a verdict (see suspended())
    bool adopt_ = false;       // a resumed Baseline: the page arms the spoof/cable after resume(), a new session there is not a loss
    bool held_ = false;        // hold(): a fresh run waits in its first Baseline (the page is writing the journal's plan)
    uint32_t jwait_ms_ = 0;    // request() parked since this time (now | 1) because the journal was not ready; 0 = not waiting
    // pattern usb-a-cuts-on-low-dpdm (see kSagAdoptMs): sag_cut_ = a legacy row failed as Lost and its recovery is running;
    // sag_open_ = that recovery ended at sag_ms_ and a new attach session within kSagAdoptMs is adopted.
    // gone_ = Unsafe is timing the unplug (VBUS under 1 V since stable_ms_, see kUnsafeGoneMs); fits the padding before sag_ms_.
    bool sag_cut_ = false, sag_open_ = false, gone_ = false; uint32_t sag_ms_ = 0;
    bool recover_ = false, hot_ = false, stable_ = false, stopping_ = false, pd_owned_ = false, fc_owned_ = false;
    // Sink Hard Resets sent when the release started: a later one is the sink's own last resort for a release that did not
    // bring VBUS down (pattern budget-reannounce-mid-run), a recovery and not a lost source.
    uint32_t rel_hr_ = 0;
    uint32_t busy_retry_at_ = 0;   // a PD request refused Busy (identify/probe/prep running) is retried, not failed
    HandoverInfo handover_{};
    // The units of the run in execution order (the rows are laid out in the same order) and the one running.
    std::array<Unit, kMaxUnits> order_{};
    uint8_t n_order_ = 0;
    Unit unit_ = Unit::Count;
    Mode mode_ = Mode::Pd;
    bool attached_ = false;       // the unit's attach is done: its rows may run
    bool detached_ = false;       // the last act was a detach (Rd off): the next unit attaches without detaching again
    bool ending_ = false;         // the running detach ends the unit (not a Legacy retry)
    bool silent_ = false;         // a Legacy attach was verified PD-quiet (a resumed run has no such proof)
    bool legacy_active_ = false;  // the last attach was Legacy: legacy_only(false) + pd_rescan() restore the sink at the end
    bool never_dark_ = false;     // a detach that left VBUS up (a USB-A port has no CC): later detaches do not wait long
    bool dipped_ = false;         // VBUS fell during this detach
    bool late_pd_ = false;        // no source caps at START: the PD rows are planned from the first list heard
    uint32_t plan_sig_ = 0;       // caps_sig() of the list the PD rows were last planned from
    // Restart rules for a mid-run re-announce (2026-11-09): every new list opens a settle window (list_ms_ = when the
    // list in hand first arrived, list_sig_ = its caps_sig; an invalid->valid transition is a new list too). No Request
    // is sent while the window is open (list_settled()). The window is tracked only in the run states - Attach/Detach
    // settle the list on their own - and is seeded (not opened) by the first run-state poll after start()/restore().
    uint32_t list_ms_ = 0, list_sig_ = 0;
    bool list_valid_ = false, list_tracking_ = false;
    bool flight_restart_ = false;   // the row in flight was re-placed by follow_list: re-ask at the new slot/word once the list settles
    pd::SourceCaps caps_now_{};     // the list of the last poll (the end-of-unit sweep re-checks the Withdrawn rows against it)
    bool plan_frozen_ = false;    // a restored run: the journaled rows are the plan, a later list does not replan them
    uint8_t fc_replanned_ = 0;    // bit 0: the SCP rows follow the adapter's limits already, bit 1: the UFCS rows follow its modes
    bool pd_skips_recorded_ = true, probe_asked_ = false;
    uint8_t attach_try_ = 0;
    uint32_t unit_ms_ = 0, att_ms_ = 0;   // the unit's detach began; this attach phase began
    uint32_t pre_ms_ = 0;                 // extra quiet before the unit's first handshake, on top of the 500 ms baseline settle
    enum class Att : uint8_t { Caps, Contract } att_ = Att::Caps;
    bool rp_restart_ = false;   // the attach wait was restarted once after a source-side Rp drop (pattern slow-rp-wake)
    bool contract_wrong_ = false;   // latched in poll_attach: PS_RDY at PDO 1 was seen during this attach wait with VBUS outside vSafe5V
                                    // (pattern shared-bus-split). The source may tear the port down before the timeout, so the
                                    // fail sites cannot re-ask the sink state - they read this latch.
    int attach_max_mv_ = 0;     // the highest VBUS seen during the current attach wait (WrongVoltage records this, not the timeout reading)
    // Latched per PD row (reset in request()), read where a lost programmable row ends: pattern pps-off-at-no-load = accepted (the
    // sink waited in Transition), never PS_RDY (Ready with this row's PDO), the port drew under kNeedsLoadMaxMa the whole row.
    bool row_accepted_ = false, row_ps_rdy_ = false; int row_max_ma_ = 0;
    void latch_row(const Input&);
    bool needs_load(const Input&) const;
    enum class Probe : uint8_t { Off, Pending, Closing, Running } probe_ = Probe::Off;
    uint32_t probe_ms_ = 0;
    Options opts_{};
    uint32_t caps_gone_ms_ = 0;   // when the source caps went invalid (a hard reset re-announces them); 0 = valid
    uint8_t epr_stage_ = 0;   // 0 none, 1 enter EPR when the Pd unit's 5 V contract is live, 2 waiting for EnterSucceeded + EPR caps, 3 wanted (decided at the Pd attach), 4 cable unfit: ask the EPR list only
    uint32_t epr_ms_ = 0;
    uint8_t info_ = 0;        // the informational EPR list (SPR-mode EPR_Get_Source_Cap): 0 not started, 1 asking (line busy), 2 asked / waiting, 3 done (once per Pd attach)
    uint32_t info_ms_ = 0;
    bool info_rows_ = false;  // the EPR rows are laid out from Input::epr_info (the levels the charger offers) instead of one row per algorithm
    bool epr_proof_ = false;   // the source accepted EPR entry in this run: it checked the cable (50 V / 5 A) itself
    pd::VbusWait wait_;
    uint32_t dark_ms_ = 0;   // VBUS below vSafe0V since
    bool dark_ = false;
};
}  // namespace k2::charger_test
