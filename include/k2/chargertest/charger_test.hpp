// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <algorithm>
#include <array>
#include <span>
#include "k2/chargertest/mi_unlock.hpp"
#include "k2/chargertest/spec_section.hpp"
#include "k2/chargertest/svooc_ceiling.hpp"
#include "k2/core/bss_zero.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/chargertest/sweep.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/pdspec/prog_judge.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/pd_sink.hpp"

namespace k2::charger_test {

enum class Group : uint8_t { Pd, Qc2, Qc3, Qc3Plus, Afc, Fcp, Scp, Vooc, SuperVooc, SuperVooc65, Ufcs, Vivo, Count };
enum class Algorithm : uint8_t {
    Pd2Fixed, Pd3Fixed, Epr, SprAvs, Pps, Qc2, Qc3, Qc3Plus, Afc, Fcp, Scp,
    Vooc, SuperVooc, SuperVooc65, Ufcs, Vivo, Pd30Revision, Pd31Revision, Pd32Revision, EprAvs, Qc2ClassB, Qc3ClassB, Count
};
// The engine's "protocol not spoken" failure of a unit's detection step: AFC / FCP / SCP Unsupported, UFCS / VIVO NoResponse (no frame in the
// entry window), QC2 / QC3 / QC3+ / UFCS NoHandshake (the first handshake got nothing back). A unit that never answered was not offered:
// it is a skip ("no reply"), not a fail. The runner applies it to a unit's first step only; a unit that answered and then fails stays a fail.
constexpr bool never_answered(Group g, fc::Fail f) {
    return ((g == Group::Afc || g == Group::Fcp || g == Group::Scp) && f == fc::Fail::Unsupported) ||
           ((g == Group::Ufcs || g == Group::Vivo) && f == fc::Fail::NoResponse) ||
           ((g == Group::Qc2 || g == Group::Qc3 || g == Group::Qc3Plus || g == Group::Ufcs) && f == fc::Fail::NoHandshake);
}
// A VOOC family that clocked no opener after either wake (NoAnswer): a skip too, an unloaded adapter may stay silent.
constexpr bool silent_vooc(Group g, fc::Fail f) { return g >= Group::Vooc && g <= Group::SuperVooc65 && f == fc::Fail::NoAnswer; }
constexpr bool no_reply(Group g, fc::Fail f) { return never_answered(g, f) || silent_vooc(g, f); }
static_assert(unsigned(Algorithm::Count) < 32);
inline constexpr uint32_t kAllAlgorithms = (1u << unsigned(Algorithm::Count)) - 1;
inline constexpr size_t kMaxSteps = pd::kMaxPdos * 6 + 46;   // 40 + the six default Class B rows (Qc2ClassB 1, Qc3ClassB 5); the journal's one-block section assert caps this
// The current a PD plan row requests: 1 A (clamped to the offer and the cable). The one exception is the full-current PPS row, the
// last row of each PPS APDO: top voltage at the APDO's whole current, so it is the only plain row above this. The Xiaomi unit's mode
// rows (Step::hunt) ask an unlocked PDO at its new level, never above what it advertises.
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
    NotUnlocked,         // retired 2026-11-18 (older cards and journals): a 6 A hunt row the source did not offer (detail 155..159: why)
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
    StepResolution,      // a step-walk row (PPS 20 mV, AVS 100 mV quanta) whose step the source did not follow (rounded to a coarser quantum
                         // or ignored): a CAUTION on a Pass since 2026-11-18, detail = the measured move in mV (0 = no move). Older cards and
                         // journals hold it on a Fail: a consumer keys the tone on the verdict
    Loop,                // the HID-cable loop 5 V lock (k2/core/loop_guard.hpp, Input::loop_lock): no run starts; a lock mid-run fails the
                         // row in flight and ends the run back at 5 V
    Advertised,          // a Light run (Level::Light): a level the source's own list offers (a PD PDO / APDO, an EPR level, a UFCS mode, an
                         // SCP limit read from the adapter), recorded as advertised and never requested. A skip that is no failure and
                         // no "not tested": the UI shows it as noted, never as pass
    LightLevel,          // a Light run: a legacy level other than the 5 V handshake (nothing advertises it), or a protocol only told
                         // apart above 5 V (AFC, QC3+, QC Class B): not asked at this level, a Medium run asks it
    PortCuts,            // pattern usb-a-no-load-auto-off: the row met a cut of the port's output (VBUS under kCutMv for a moment, back within
                         // kCutWindowMs) and its one retry met another: a skip, never a failure (the port cuts without load)
    Folded,              // user decision 2026-11-16: the deep sweep of the row's PDO found a voltage fold (pd::TrackScan: the settled level fell
                         // by more than 50 mV while the request rose). A CAUTION on a Pass verdict - the sweep row, and the fixed rows of the
                         // same APDO at or above the first folded setpoint that read below the peak - never a fail (Summary::fold_mv)
    MiChain,             // the Xiaomi unit's chain row (is_mi_chain): detail = mi_unlock::chain_code. Pass = the 0x2717 chain finished
                         // (with or without new modes), Skip = no SVID 0x2717, no answer or a stall; nothing is requested on this row
    ProgCheck            // a Deep PPS / AVS row whose transition capture broke a spec limit (progcheck::Flag bits in detail: overshoot, slow rise,
                         // transient, level off): a CAUTION on a Pass, never a fail - the level was delivered
};
// Step.detail of a PPS step-walk row while it is Pending/Running (finish() overwrites it with the result); sits above every
// engine code and the EPR skip codes so the card and the tools never mistake it for one.
// Charger-test thoroughness (Options::level, with_level). The values are the wire values: 0 = Medium. Offers is the "Read offers"
// preset, not a depth: it never enters the Depth cycle or Settings (kLevelCount counts the depths), only the wires (kRunLevelCount).
enum class Level : uint8_t { Medium = 0, Light = 1, Deep = 2, Offers = 3 };
inline constexpr unsigned kLevelCount = 3, kRunLevelCount = 4;
inline constexpr uint8_t kDetailStepWalk = 160;
// Step.detail of a deep-sweep row (ROADMAP "PPS deep sweep"; the AVS sweep reuses the marker on its AVS algorithms).
// Unlike the walk's marker it SURVIVES finish() (finish keeps it), so a finished run's results strip and the card can
// still tell the row apart; the verdict's result code rides in Reason alone, as for StepResolution.
inline constexpr uint8_t kDetailSweep = 161;
// ROADMAP "PPS coverage" item 2: one step-walk row per PPS / AVS APDO in a Deep run, from the mid level, kStepWalkCount Requests of
// one quantum (walk_step_mv) each. Every step needs Accept + PS_RDY; the walk is judged by (1) a jump: a single measured move of
// 2 quanta or more ends it at once (a source that rounds to a coarser quantum shows it), (2) the total: the last level minus the
// base must be kStepWalkCount quanta +- one quantum, and (3) the trend: the least-squares slope over all kStepWalkCount + 1
// recorded levels must lie in half a quantum .. one and a half per step. A miss is a CAUTION on a Pass (Reason::StepResolution,
// detail = the measured move, 0 = steps ignored), never a fail: the levels the walk asked for were granted. A single step is NOT
// judged for direction or size below the jump: the output creeps up 20-36 mV for ~2 s after each PS_RDY (hardware
// 2026-11-10, 13.00 -> 13.10 V: one level held 13039..13075), so one +20 mV step reads anywhere from <= 0 to ~+36 mV
// while the total is right (~95 mV). The slope band (PPS 10..30 mV/step) passes that creep and a slow-but-correct source
// (+15 +25 +18 +22 +20 reads 20.4), and flags a source that ignores the steps (slope 0) or follows only about half of them.
inline constexpr unsigned kStepMv = 20, kAvsStepMv = 100, kStepWalkCount = 5;
// The walk's quantum: PPS Requests move in 20 mV, AVS in 100 mV. The judging scales with it: the total is kStepWalkCount x step
// +- one step (also the rebase drop), the least-squares slope per step lies in half a step .. one and a half steps.
constexpr unsigned walk_step_mv(Algorithm a) { return a == Algorithm::Pps ? kStepMv : kAvsStepMv; }
// walk-settle (ROADMAP "The PPS step walk fails a good source: its base level is read before the output settles"): a granted
// walk level is recorded only once VBUS is steady - at least kWalkSteadySamples samples inside the last kWalkSteadyWindowMs
// all within kWalkSteadyMv of each other (3 samples within 3 mV at the 100 ms host-test cadence; at the 20 ms UI tick the
// window holds ~15) - or kWalkSteadyTimeoutMs passed since the level was first seen granted, whichever comes first, so the
// wait is bounded. The walk's sum rule is unchanged.
inline constexpr unsigned kWalkSteadySamples = 3;
inline constexpr int kWalkSteadyMv = 3;
inline constexpr uint32_t kWalkSteadyWindowMs = 300;
inline constexpr uint32_t kWalkSteadyTimeoutMs = 2000;
// A run that was resumed this many times is not offered again (a step that dies before it can leave a marker would loop forever).
inline constexpr unsigned kMaxResumes = 16;
// A unit (protocol) may take the K2 down this many times inside one run: losses 1 .. kRebootBudget-1 resume the same row, the
// last one skips what is left of the unit (RebootLimit) and the next unit runs. The count is journaled per unit.
inline constexpr unsigned kRebootBudget = 3;
// Boot policy: every interrupted run resumes by itself once the gate is ready (VBUS steady at 5 V); BACK held at boot cancels it.
constexpr bool resume_wanted(bool back_held, bool gate_ready) { return !back_held && gate_ready; }
// Detach / Attach are the two ends of every unit (see Unit below); they replace the old Handover / Reattach states in the same numbering.
// Ceiling = the SVOOC ceiling stage (svooc_ceiling.hpp): the guarded trigger runs in a Legacy unit after the VOOC family's
// session was released, at 5 V, nothing drawn. Appended last so the earlier numbers (the wire, the journal) stay.
// Spec = the PD spec stage of a Deep run (spec_section.hpp): the pdspec engine runs the no-load conformance checks on the Pd unit's
// own link, at the end of the Pd unit, once its contract is released to 5 V.
enum class State : uint8_t { Idle, Baseline, Requesting, Holding, Releasing, Done, Stopped, Unsafe, Detach, Attach, Ceiling, Spec };
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
// Legacy attach: the quiet_ms knob defaults to 0 (start inside the source's announcement); set to kLegacyQuietMs (longer than the
// ~1 s a source stays silent around its own hard reset) it waits for that much PD quiet, plus
// kLegacySteadyMs of vSafe5V means the source gave up PD. kLegacyGiveUpMs without that, the unit detaches and tries once more
// with kLegacyRetryMs (a source that gets no Request retries Source_Capabilities and hard-resets, 5.07 s cycle on the Baseus
// CJ21, before it disables PD); still no fallback = Reason::BlockedByPd and the unit's rows are skipped.
inline constexpr uint32_t kLegacyQuietMs = 2500, kLegacySteadyMs = 500, kLegacyGiveUpMs = 20000, kLegacyRetryMs = 60000;
// pattern rp-without-vbus: the source's Rp is on the line but VBUS stays dark this long into an attach: the Rd is cycled at once
// (the same one retry as the give-up, taken 2 s in instead of 20 s).
inline constexpr uint32_t kLegacyNoVbusMs = 2000;
// pattern output-reset-after-unanswered-scp: a source that cuts its own output between two rows of a PD-silent Legacy unit and
// announces Source_Capabilities again when VBUS is back (UGREEN X865, runs/ugreen-qa/r1a-stream: first frame ~300 ms after the
// return; its PD-silent attach announced for ~6.1 s). The next row waits after the return: kRestartCapsMs with no frame means the
// source is not announcing; a frame means kLegacyQuietMs of quiet after the last one (the driver drops stale caps after 2 s, so the
// fc engine's link guard passes). kRestartSettleMs from the return bounds the wait (~0.3 + 6.1 + 2.5 s = 8.9 s on the X865); a
// source still talking then is PD live and the unit is skipped PdActive as before. A load or a PD contract still refuses at once.
inline constexpr uint32_t kRestartCapsMs = 1000, kRestartSettleMs = 10000;
// A session boundary (detach + fresh attach) happens only when the session mode changes:
// consecutive Legacy units share one PD-silent attach and only clean up the D+/D- lines in between (the engine's release holds
// them at 0 V, then Hi-Z). Waits before a unit's first handshake: kLegacyFirstWaitMs after the PD-silent attach (1000 ms),
// kLegacyGapMs after the previous unit's release (the 500 ms D+/D- discharge), plus kAfcPreMs more before AFC.
inline constexpr uint32_t kLegacyFirstWaitMs = 1000, kLegacyGapMs = 500, kAfcPreMs = 500;
// The four waits above plus the row Hold are switchable at run time (Cmd::Lab kLabTune, `k2_remote charger-legacy-tune`): RAM only, the
// constants again at boot. quiet_ms 0 starts the first handshake as soon as VBUS is steady at 5 V, caps still arriving or not.
// Chain is a level, not a time: 0 = every legacy row opens its own session as before, 1 = the SCP rows of a unit retarget one session
// (no D+/D- release between them).
enum class LegacyKnob : uint8_t { QuietMs, SteadyMs, FirstWaitMs, GapMs, HoldMs, Chain, Count };
uint32_t legacy_tune(LegacyKnob k);
bool legacy_tune_set(uint8_t knob, uint32_t value);   // false: unknown knob or out of range
void legacy_tune_reset();
// The PD driver drops Source_Capabilities nobody answered this long after the last frame; until then the link guard reads a PD-silent
// source's own announcements as a PD contract and refuses the fast-charge start. quiet_ms under it (link::kCapsStaleMs) means the run
// starts inside that window, so the guard's "PD contract" (and only that one, never a load) must not stop the Runner's own Legacy request.
bool legacy_guard_override();
// pattern reannounce-after-own-release (Anker IQ4, runs/qc2b-2026-11-21): a source that reads the engine's D+/D- release as a re-plug
// announces its list to the PD-silent sink again (first frame 0.1 to 0.5 s after the release, VBUS at 5 V all along). The first release of
// a silent Legacy attach is watched for kOwnReleaseCapsMs (the 500 ms line discharge + kRestartCapsMs); a frame in it is the source's
// answer to our own release, not PD traffic: the unit waits in place for kLegacyQuietMs of quiet (at most kRestartSettleMs) instead of
// detaching. Frames before the release are real PD traffic and arm nothing. A source quiet in that window is not watched again.
// The wait is bounded by kOwnReleaseSettleMs: the IQ4's attach announced for 7.4 s, so one as long as that ends ~10.4 s after the release
// (7.4 + 2.5 s quiet + the first frame), past kRestartSettleMs; the wait that ends in a detach anyway is lost time.
inline constexpr uint32_t kOwnReleaseCapsMs = kLegacyGapMs + kRestartCapsMs, kOwnReleaseSettleMs = 12000;
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
// pattern usb-a-no-load-auto-off (CUKTECH 120W USB-A port, A-to-C cable, no load, 2026-11-15): at 5 V the port cuts its output every 7.0 s,
// VBUS ~2 V for ~1.0 s (1.96 V at the lowest), then it is back at 5.11 V; it never cut above 5 V. A row that fails while VBUS fell under
// kCutMv for at least kCutMinMs and was back at vSafe5V within kCutWindowMs of the fall is the cut's casualty, not a verdict: it is asked
// once more once VBUS is steady (cut_retry). Another cut in the retry reads Reason::PortCuts (a skip). kCutWindowMs is twice the cut seen
// and far under the period, so a cut that long is told from an output that is gone; kCutMv sits under the sags other ports show (JT-P18
// 3.6-4.9 V, a 4.5 V row) and over the cut's dip; kCutMinMs leaves out the instant dips of the fast VBUS reading (0.65 V, JT-P18);
// kCutNoAboveMv: only a row that never went above 5 V can be a casualty (the port does not cut a raised output).
inline constexpr int kCutMv = 3000, kCutNoAboveMv = 5500;
inline constexpr uint32_t kCutMinMs = 200, kCutWindowMs = 2000;
// A row lost to a cut is asked again up to kCutRetries times, each from the first steady 5 V after a clean comeback (user decision
// 2026-11-15, the port cuts every ~7 s and a row can meet several); only when the last retry is cut too does it read PortCuts (a skip).
// A row that fails with no cut at all, or a protocol that stays wrong or silent after a clean comeback, still fails at once.
inline constexpr uint8_t kCutRetries = 3;
// A 5 V-held legacy step on a port with a learned cut period must end inside one clean window. The CUKTECH USB-A port cuts every
// 7.04 s with a dark of ~1.0 s; a retry starts ~0.15 s after the comeback, so the next fall comes ~5.86 s after the retry's start
// (measured, runs/cuktech-usba/medium3-stream, 2026-11-15): 5500 ms is what a step may take, with ~0.3 s to spare.
inline constexpr uint32_t kCutStepBudgetMs = 5500;
inline constexpr unsigned kCutOnsets = 8;   // the onsets kept to read the cut period from (the median gap between them)
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
// A reading from kDipMinMv up to (not including) vSafe5V is a dip: the fall of a port's cut (pattern usb-a-no-load-auto-off: the first
// falling sample of the CUKTECH USB-A port read 4.65 V, then 3.1, 2.7 ... down to ~2.0 V) or a sag. Only the samples after it show
// which. Every 5 V check of the run's Baseline, the START and the release windows therefore waits for the dip to come back (VBUS at
// vSafe5V) inside kCutWindowMs and for at most kCutRetries dips between two rows, and only then calls the source unsafe / lost.
// VBUS above vSafe5V and under kDipMinMv (a source that is gone) keep their own verdicts. kDipTailMs: the cable's Rp returns after
// VBUS (a new attach session, the same cut's tail), so a session change this soon after the comeback is adopted.
inline constexpr int kDipMinMv = 1000;
inline constexpr uint32_t kDipTailMs = 1000;
constexpr bool dip_reading(int mv) { return mv >= kDipMinMv && mv < pd::kVSafe5MinMv; }
// What the first Legacy attach of the last run did; the report card keeps it with its duration (ms from the unit's detach).
// InContract is no longer produced (stored cards of the old flow still carry it).
enum class HandoverOutcome : uint8_t { NotTried, FellBack, StayedPd, Fault, Reboot, InContract };
struct HandoverInfo { HandoverOutcome outcome = HandoverOutcome::NotTried; uint32_t ms = 0; };
struct Options {
    uint32_t selected = kDefaultAlgorithms;
    UnitList units{};   // explicit unit list (remote command / ct_start.py --units); empty = the default order
    // The PPS deep sweep (ROADMAP "PPS deep sweep"): not an algorithm bit (the presets carry `selected` only), so this flag
    // rides next to it (journal meta byte, remote draft flags). The charger-test session turns it on for its default run
    // (2026-11-14); the struct default stays off, so a journal without the bit (an older run) resumes without sweep rows.
    bool pps_sweep = false;
    // The AVS deep sweep (ROADMAP "AVS deep sweep"): its own flag next to the PPS one, opt-in (off in the default run).
    bool avs_sweep = false;
    // The SVOOC ceiling stage (svooc_ceiling.hpp): opt-in like the sweeps, never in a preset. On = after a detected VOOC family
    // the guarded trigger asks the adapter 1.5 / 4 / 6.3 / 10.9 A (a protocol word, no load) and the highest ask it went on
    // under is the result. UsbA (the retired USB-A waiver, kept for the wire) runs as On: the inline latch only keeps certain evidence now.
    svooc_ceiling::Mode svooc_cap = svooc_ceiling::Mode::Off;
    // The PD spec stage (spec_section.hpp): Deep only, like the SVOOC ceiling. Journaled in the meta byte (bit 5).
    bool spec = false;
    // How thorough the run is (with_level). Medium is 0 on every wire (journal, remote status word, card), so data written before
    // the level existed reads as Medium.
    Level level = Level::Medium;
    bool contains(Algorithm a) const { return selected & bit(a); }
};
// The one place a level maps to the run's switches (user decision 2026-11-15):
//   Light  - VBUS stays at 5 V: the PD 5 V row, the 5 V handshake of each legacy protocol, the source's own list recorded as
//            Advertised rows (Reason::Advertised); no Request above or below 5 V, no EPR entry (its levels are read in SPR mode),
//            no sweeps, no SVOOC ceiling, no PdLow unit.
//   Medium - the fixed levels, the PPS / AVS points, the legacy level steps; no deep sweeps, no SVOOC ceiling.
//   Deep   - Medium plus the PPS and AVS deep sweeps over every APDO, the SVOOC ceiling stage and the PD spec stage (no-load
//            conformance of the transitions and the PPS / AVS moves, one more section of the results).
//   Offers - PD only (kOffersAlgorithms, no PdLow, no sweeps): the source's lists are read (SPR, then EPR after the usual entry) and
//            every offer is an Advertised row, except ONE AVS Request at the APDO's minimum, 1 A at most (the EPR AVS APDO when
//            the run is in EPR, else the SPR one). No AVS offered = the AVS row's NoOffer skip.
// The sweep flags stay their own switches afterwards (the Custom flow's sweep rows and the remote commands toggle them).
// The SVOOC ceiling stage (the guarded trigger, VBUS up to the adapter's ceiling) is Deep only: Light and Medium stop at the
// VOOC family's detection and its ident. Deep turns it on, keeping the mode the operator set (UsbA is the retired waiver and
// runs as On).
inline constexpr uint32_t kOffersAlgorithms = kPdFixedAlgorithms | kPpsAlgorithms;
// A level whose rows are the source's own list (Light, Offers): a PD row above 5 V is never asked except Offers' one AVS Request.
constexpr bool lists_only(Level l) { return l == Level::Light || l == Level::Offers; }
constexpr Options with_level(Options o, Level l) {
    o.level = l;
    o.pps_sweep = o.avs_sweep = o.spec = l == Level::Deep;
    if (l != Level::Deep) o.svooc_cap = svooc_ceiling::Mode::Off;
    else if (o.svooc_cap == svooc_ceiling::Mode::Off) o.svooc_cap = svooc_ceiling::Mode::On;
    if (l == Level::Offers) { o.selected &= kOffersAlgorithms; if (!o.selected) o.selected = kOffersAlgorithms; }
    return o;
}
// The Depth cycle: Offers is not part of it (a stray Offers value re-enters at Light).
constexpr Level next_level(Level l) { return l == Level::Light ? Level::Medium : l == Level::Medium ? Level::Deep : Level::Light; }
const char* name(Level);
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
    bool hunt = false;   // a Xiaomi unit row: pos 0 = the chain row (is_mi_chain), else a mode the chain unlocked (is_mi_mode)
    uint32_t pdo = 0;
};
static_assert(sizeof(Step) == 20, "Step must stay free of padding holes: kMaxSteps copies live in the Runner");
// True while `s` is the step-walk row of its APDO and still in flight (the planner marks it with kDetailStepWalk).
constexpr bool is_step_walk(const Step& s) {
    return s.group == Group::Pd && (kPpsAlgorithms & bit(s.algorithm)) && s.detail == kDetailStepWalk &&
           (s.verdict == Verdict::Pending || s.verdict == Verdict::Running);
}
// True when `s` is a deep-sweep row (one per APDO part, ctsweep::part_range; the points live in the Runner's sweep record, not as rows).
constexpr bool is_sweep(const Step& s) {
    return s.group == Group::Pd && (kPpsAlgorithms & bit(s.algorithm)) && s.detail == kDetailSweep;
}
// The Xiaomi unit (SVID 0x2717): one chain row, planned last of the Pd unit, that waits for the sink's auth chain and diffs the list
// re-announced after it against the list before it (mi_unlock::diff); each raised or new PDO then gets a mode row right behind it,
// asking that PDO at its new level (mi_unlock::test_point) like any PD row. Both ride Step::hunt (journal bit 4, Step stays 20 B).
constexpr bool is_mi_chain(const Step& s) { return s.group == Group::Pd && s.hunt && s.pos == 0; }
// The EPR AVS APDO a run reached (a row of it ran: EPR entered, the cable fit): the PD spec stage sweeps it too, though the live list
// loses it at EPR exit. nullptr = none.
constexpr const Step* reached_epr_avs(std::span<const Step> steps) {
    for (const Step& s : steps)
        if (s.algorithm == Algorithm::EprAvs && s.pos >= 8 && s.pdo && s.verdict != Verdict::Skip) return &s;
    return nullptr;
}
constexpr bool is_mi_mode(const Step& s) { return s.group == Group::Pd && s.hunt && s.pos != 0; }
// Which part of its APDO a sweep row covers: below ctsweep::kSplitMv (the PdLow row) or from it up. The row's mv is its first
// setpoint until it closes and the setpoint of its peak after; both stay on the row's side of the split.
constexpr bool sweep_low(const Step& s) { return s.mv && s.mv < ctsweep::kSplitMv; }
// The setpoints a sweep row walks: its part of the APDO's point range, capped at cap_mv (monitor ceiling, cable limit).
ctsweep::PointRange sweep_range(const Step& s, unsigned cap_mv);
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
    // the cable limit the source saw, and what the hunt classifier and the planned row currents (the full-current PPS row) use instead
    // of the 3 A default of an unread cable, capped at 5 A. Row voltages, the EPR gate and the 1 A sweep points keep reading cable_mv /
    // cable_ma.
    uint16_t spoof_mv = 0, spoof_ma = 0;
    int vbus_mv = -1, ibus_ma = 0;   // ibus_ma: magnitude; above kLoadAbortMa during a raised row means a load
    uint32_t pd_quiet_ms = UINT32_MAX;   // ms since the last PD frame was received (since the handover began)
    bool inline_load = false, fake = false, busy = false, cable_observed = false, pd_traffic = false, spoof_on = false;
    // At START: VBUS is up and no Rp is on the line, a port with no CC (USB-A). It never goes dark on a detach, so START does not wait for it.
    bool no_cc = false;
    bool loop_lock = false;   // k2::loopguard::locked(): the HID-cable loop 5 V lock (Reason::Loop)
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
    uint8_t fc_ident = 0;   // fctrig::Status::ident: the VOOC family's ident command of a Detected session (0 = none)
    bool fc_held = false;   // fctrig::Status::held: an SCP session whose row ended is kept alive for the next one
    int fc_adapter_mv = 0;  // fctrig::Status::adapter_mv: the SCP adapter's own voltage readback of this session (0 = not read / not SCP)
    bool probe_done = false;   // the source probe the runner asked for has finished (answered, refused or timed out)
    bool epr_proof = false;    // set by the Runner, not the caller: the source accepted EPR entry earlier in this run (cable proven)
    const pd::SourceCaps* epr_info = nullptr;   // the list the source answered to an SPR-mode EPR_Get_Source_Cap (valid + epr once answered)
    const pd::XiaomiTrace* xm = nullptr;        // the sink's chain / Request trace (null = none: a refused hunt row still records what it can)
    // What the engine's SCP / UFCS sessions read from the charger (fctrig::caps()). The Runner trusts it only after a row of the
    // run's own unit ran: START plans without it (the engine may still hold an earlier charger's list), and the rows still to run
    // are planned again once the unit's first row has read it.
    const fc::Caps* fc_caps = nullptr;
    // The guarded SVOOC trigger of the ceiling stage (svooc_trig::snapshot()), meaningful while the stage runs.
    svooc_ceiling::Live svooc{};
    uint16_t svooc_word = 0;   // the highest ask the adapter went on under so far (svooc_trig Snapshot::ceiling_word)
    uint8_t svooc_asks = 0;    // the asks answered so far
    uint16_t svooc_vmax_mv = 0;   // the highest VBUS the trigger saw (svooc::Report::vbus_max_mv)
    uint16_t svooc_last_ms = 0;   // the adapter's last edge, ms after the wake (svooc::Log::last_edge_ms)
    // The PD spec stage: the pdspec engine is still running (Env::spec_start was accepted and it has not finished).
    bool spec_running = false;
    // D+/D- are free for the ceiling stage's trigger: no owner holds them and they sit at the passive idle (the trigger's own
    // start check, svooc_trig::lines_free()). False for 500 ms after a legacy session's release (the engine's 0 V discharge).
    bool lines_free = true;
};
// The runner waits for the source probe at most this long, so a charger that ignores it costs the run under 1 s.
inline constexpr uint32_t kProbeMaxMs = 1000;
inline constexpr uint32_t kCapsReturnMs = 8000;   // how long an invalid cap list (hard reset, re-announce) is tolerated
// Restart rules for a mid-run re-announce (user decision 2026-11-09): after any new list (Source_Capabilities, or a
// complete EPR_Source_Capabilities outside the K2's own EPR entry/exit) no Request is sent until the list has been
// unchanged for kListSettleMs; each list that arrives inside the window restarts it. The sink's single-Request gate
// still applies (the Runner sends every Request through it).
inline constexpr uint32_t kListSettleMs = 1500;
// Pattern budget-reannounce-mid-run, the end-of-unit wait (2026-11-17): when a re-split left rows of the running PD unit Withdrawn and
// the list in force still lacks them, the unit stays open this long for the offer to come back before it closes. The Anker 2-way
// splitter re-announces its list about every 6.6 s (hardware trace), so the bound covers two periods (13.3 s) plus the settle
// window (1.5 s) the returned list needs before a row may ask: 14.8 s, rounded up to 15 s. A source that does not return the
// offer in that time costs one wait, once per re-split (a revival re-arms it); a charger that withdrew rows for good is
// otherwise a unit closed on time.
inline constexpr uint32_t kWithdrawnWaitMs = 15000;
inline constexpr uint32_t kEprInfoBusyMs = 1000;   // how long the informational EPR_Get_Source_Cap retries a busy line
inline constexpr uint32_t kEprReadyMs = 8000;   // how long EPR entry waits for a live contract to enter from
// pattern epr-entry-hard-reset (Baseus 140W, 2026-11-05): the source Hard Resets in the middle of the EPR entry (after our
// chunk-1 request, ~75 ms later). The entry is asked again after the source's SPR list and 5 V contract have been steady for
// kEprSettleMs (a source that has just reset is still re-arming), at most kEprEntryRetries more times; each retry waits one
// kEprSettleStepMs longer. Pattern reject-then-reset: the step after a Reject waits kRejectSettleMs before an EPR entry.
inline constexpr unsigned kEprEntryRetries = 2;
inline constexpr uint32_t kEprSettleMs = 2000, kEprSettleStepMs = 2000, kRejectSettleMs = 1500;
// The Xiaomi / CUKTECH auth chain (SVID 0x2717) finishes ~0.7 s after the contract and then the source re-announces raised limits
// (hardware 2026-11-06): the Xiaomi chain row waits for it, bounded, so a chain that never finishes costs the run this long once.
inline constexpr uint32_t kXiaomiAuthWaitMs = 2000;
// The source re-announces its list right after the chain (the sink stamps it, XiaomiTrace::reannounced): the chain row waits this
// long for that list after the chain finished before it diffs the list in hand.
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
// PS_RDY timing quirks the engine tolerates and no verdict reads (Evidence::timing, OR over the run's PD rows; one [CT] ps_rdy line
// names the row). PsRdyOutOfBand = PS_RDY arrived while VBUS sat outside the row's band (FH22: PS_RDY at a 21 V contract with 19.42 V
// delivered); PsRdyInferred = no PS_RDY came and the sink took the contract as ready from a settled VBUS (no PS_RDY on a down-step);
// PsRdyLate = PS_RDY came later than the spec maximum after the Accept. The waits the engine tolerates stay kPsRdyMs / kPsRdyDownMs.
enum TimingFlag : uint8_t { PsRdyOutOfBand = 1, PsRdyInferred = 2, PsRdyLate = 4 };
inline constexpr uint32_t kPsRdyMaxSprMs = 550, kPsRdyMaxEprMs = 1020;
constexpr bool ps_rdy_late(uint32_t accept_to_ps_rdy_ms, bool epr) { return accept_to_ps_rdy_ms > (epr ? kPsRdyMaxEprMs : kPsRdyMaxSprMs); }
struct Evidence {
    uint32_t selected = 0, session = 0;
    uint16_t monitor_mv = 0, cable_mv = 20000, cable_ma = 3000, raw_header = 0;
    uint16_t scp_ma = 0;   // the SCP current limit the adapter reported (0 = not read); a card keeps it in 50 mA units
    uint8_t wire_rev = 0, flags = 0, num_pdos = 0, seen = 0;
    uint8_t timing = 0;       // TimingFlag bits seen on any PD row of this run (not journaled, not on the card)
    uint8_t ps_rdy_max = 0;   // the worst Accept -> PS_RDY time of the run in 20 ms units, saturating at 255; 0 = none measured
    uint8_t rebalanced = 0;   // how often the source re-announced its list mid-run and the PD rows were re-planned per row (budget-reannounce-mid-run)
    // The SVOOC ceiling stage (svooc_ceiling.hpp; not journaled, not on the card): the highest 0x86 word the adapter went on under
    // (100 mA per count; 0 = none accepted), how the stage ended (svooc_ceiling::End; 0 = not run), why (the trigger's
    // svooc::Refusal for Refused, svooc::Abort for Aborted), how many asks were made. A protocol readout: the K2 never drew.
    uint16_t svooc_word = 0;
    uint8_t svooc_end = 0, svooc_why = 0, svooc_asks = 0;
    // ... the highest VBUS the trigger saw in the session (mV, its start and exit watch included) and when the adapter's last
    // D+/D- edge came (ms after the wake, saturating): "stopped at" for End::Stopped.
    uint16_t svooc_vmax_mv = 0, svooc_stop_ms = 0;
    // The PD spec stage (spec_section.hpp): how it ended and one verdict per check. Journaled (/jr/sp.rec), not on the card.
    specsec::Result spec{};
    // The VOOC family's ident command (0xA8 / 0xAC / 0xA6) when its detection row passed; 0 = none heard. One ident proves the
    // family, not the variant. Not journaled, not on the card.
    uint8_t vooc_ident = 0;
    // pattern usb-a-no-load-auto-off: rows asked again after a cut of the port's output (cut_retry), and the period of the cuts seen
    // (the median gap between the last kCutOnsets onsets, in 100 ms units; 0 = fewer than two cuts seen or over 25.5 s). Only cut_retries
    // is exported (timing byte bits 3..7, saturating at 31: the 42-byte page header is full); the period is a [CT] cut period log line.
    uint8_t cut_retries = 0, cut_period_ds = 0;
    pd::Revision reported{};
    // The Xiaomi unit (Runner::mi_latch): the chain outcome (mi_unlock::chain_code, 0 = not run) and the unlocked modes, before and
    // after word and slot. Not journaled; the card carries it in its extension block (report_card_deep.hpp).
    uint8_t mi_chain = 0, mi_n = 0;
    std::array<mi_unlock::Mode, mi_unlock::kMaxModes> mi{};
};
struct Summary {
    uint8_t total = 0, runnable = 0, skipped = 0;
    uint16_t max_mv = 0;
    uint32_t estimate_ms = 500;
    uint8_t spec_checks = 0;   // the PD spec stage's checks (specsec::kCells; 0 = no stage: not Deep, no PD unit, or not built)
    Reason refusal = Reason::None;
    uint16_t refusal_mv = 0;   // the measured VBUS (mV) behind a Release / WrongVoltage refusal (0 = none)
};
const char* explanation(const Step&);
// The step-walk caution's note, "steps 100mV, not 20mV" (the mean move, rounded to 10 mV from 50 up, then the nominal step) or
// "steps ignored"; or a transition check's finding, "overshoot +1" (the first flag, then how many more); 0 characters for any other reason.
// NUL-terminated within `out`, 24 characters at most.
size_t caution_text(const Step&, std::span<char> out);
// Step.detail of an EPR skip. The PD group's detail space is 128 + pd::Err (below 136) and pd::SinkResult (below 12); these sit
// above it so the card (five code bits behind the 128 flag) and the tools never mistake them for an engine code.
//   EprRefused: kDetailEprFail + the source's Enter_Failed data (0..5)      OverCap on an EPR row: the monitor's ceiling
//   CableLimit on an EPR row: kDetailEprOffered = the charger listed this EPR level (EPR_Get_Source_Cap in SPR mode) and the cable
//   cannot carry it; kDetailEprWithheld = the ask was answered with no EPR PDO from a source whose PDO 1 has the EPR bit (the port
//   holds the list back without an EPR cable); kDetailEprOfferedRefused + the Enter_Failed data (0..5) = the level was listed AND the
//   entry was then refused (the entry's answer is added to the listed level, never replaces it; 149..154 fit the card's 159)
inline constexpr uint8_t kDetailEprFail = 136, kDetailMonitor36 = 144, kDetailMonitor48 = 145, kDetailMonitorOther = 146, kDetailEprOffered = 147,
                         kDetailEprWithheld = 148, kDetailEprOfferedRefused = 149;   // 149..154 are the EPR entry answers
// Retired with the 6 A hunt rows (2026-11-18, the Xiaomi unit replaced them): the NotUnlocked details of older cards, journals and
// share links, kept so they still decode. NoChain = no SVID 0x2717, ChainUnfinished = the chain did not finish, CableCap = a 3 A
// cable, VendorRefused = a spoofed 5 A identity the source did not take, SourceReset = answered with a Hard Reset. Nothing new
// writes them.
inline constexpr uint8_t kDetailNoChain = 155, kDetailChainUnfinished = 156, kDetailCableCap = 157, kDetailVendorRefused = 158,
                         kDetailSourceReset = 159;
// ---- Hunt evidence (retired wire format) ------------------------------------------------------------------------------------------
// The refusal snapshot of a 6 A hunt row. The rows are gone (the Xiaomi unit asks only what the list advertises), so nothing records
// one any more; the layout stays because CMD 0x0E (remote_proto.hpp, answers count 0 now) and the Black Box refusal mark of older
// captures (pdtrace kMarkRefusal, tools/pd_trace.py) carry it.
enum class HuntCause : uint8_t {
    None,                 // not a refusal (no evidence recorded)
    NoChain,              // the source never listed SVID 0x2717: there was nothing to unlock
    ChainStalled,         // listed, but the auth chain never reached 0x27170208 (last_step says where it stopped)
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
const char* name(HuntCause);          // short word for logs and the wire ("apdo-stale"): decodes older captures
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
    uint16_t authed_ms = 0;     // ms from the auth finish to the refusal (saturates; 0 when not authed)
    uint16_t answer_ms = 0;     // ms from the Request to the source's answer (0 = none, saturates)
    uint8_t step = 0;           // index of the row in Runner::steps()
    uint8_t cause = 0;          // HuntCause
    uint8_t answer = 0;         // pd::ReqAnswer; None + HfTimeout = the source never answered
    uint8_t last_step = 0;      // last 0x2717020x the source sent (0 = none)
    uint8_t caps = 0;           // PDOs in the list in force at the Request
    uint8_t pos = 0;            // object position asked
    uint8_t type = 0;           // pd::PdoType of the PDO asked
};
inline constexpr size_t kHuntSlots = 4;   // the slots CMD 0x0E accepts (the old plan had at most three hunt rows)

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
    // After a request_fc that started or retargeted an SCP row: whether the session outlives a detect / "no change" end (a row follows).
    virtual void fc_keep(bool) {}
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
    // A finished row's reason changed after its record (the caution of a fold the APDO's sweep found, Reason::Folded): the journal
    // mirrors it, no event and no log line (the row's own record() ran when it finished).
    virtual void amend(const Step&) {}
    // pattern budget-reannounce-mid-run: the source announced its list again mid-run (same wire revision, none of the excluded
    // cases) and the PD rows were re-planned per row instead of the unit ending as "offers changed": `follow` rows take the new
    // slot and PDO word, `withdrawn` rows have no offer any more (Reason::Withdrawn), `back` rows a PDO came back for. Called once
    // per re-plan; the page logs it and the count is noted on the results card (Evidence::rebalanced).
    virtual void replan(unsigned follow, unsigned withdrawn, unsigned back) { (void)follow; (void)withdrawn; (void)back; }
    // The SVOOC ceiling stage (svooc_ceiling.hpp). ceiling_start starts the guarded trigger with the Probe profile and the
    // stage's cap: 0 = started, else the trigger's svooc::Refusal (never 0 for a refusal), 255 = no trigger on this build.
    // ceiling_stop ends it (idempotent, the exit watch follows). The stage reads the rest from Input::svooc.
    virtual uint8_t ceiling_start() { return 255; }
    virtual void ceiling_stop() {}
    // The PD spec stage (spec_section.hpp). spec_start starts the pdspec engine on the live link (the transitions and PPS / AVS
    // families, specsec::kFamilyMask): 0 = started, else the refusal (pdspec::StartResult, never 0), 255 = no engine on this build.
    // spec_stop asks it to end in order (the keep-alives back, the source released to 5 V; idempotent). spec_collect reads the
    // finished run into `r`: the cells, how it stopped and whether the return to 5 V was confirmed; a run still going (the Runner's
    // abandon after the grace) is ended hard first. Input::spec_running says it is over.
    virtual uint8_t spec_start() { return 255; }
    virtual void spec_stop() {}
    virtual void spec_collect(specsec::Result& r) { (void)r; }
    // The stage's result is final (called once, before the source is released to 5 V): the journal keeps it.
    virtual void spec_done(const specsec::Result& r) { (void)r; }
    // The Deep PPS / AVS transition check (prog_check.hpp). prog_begin: the hold of a fixed programmable row begins; the page freezes the
    // capture of the row's own Request (false = none: not recording, or another tool owns it). prog_take: the hold ended, the capture
    // is read and its findings come back as progcheck::Flag bits (0 = clean, or nothing could be measured). Only a begin that said
    // true is followed by a take; both before the row is recorded, so the page can still release the capture with the row.
    virtual bool prog_begin(const Step&) { return false; }
    virtual uint8_t prog_take(const Step&) { return 0; }
    // A Deep run's own PPS / AVS moves feed the PD spec stage still ahead, so it sweeps no APDO twice. spec_move offers the move the
    // recorder holds (pdspec::MoveFlag bits; false = not taken); spec_feeding stays true until the stage's engine has judged it, and
    // the Runner asks for nothing new meanwhile.
    virtual bool spec_move(const pdspec::ProgMove& mv, uint8_t flags) { (void)mv; (void)flags; return false; }
    virtual bool spec_feeding() { return false; }
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
// A row that stands on such a plateau (above kCappedFloorMv, below the band, contract live) for this long is Capped at once; before
// it waited out the 12 s row timeout for the same verdict. Longer than kPlateauMs so a source that pauses on a ledge and then goes on
// rising (a slow ramp) still gets its time; the timeout path keeps the kPlateauMs rule.
inline constexpr uint32_t kCappedPlateauMs = 5000;
// The sink's PPS keep-alive (kPpsKeepAliveMs) re-requests the same level and `ready` drops for the 20-140 ms between its Request and
// PS_RDY (FH22, 2026-11-11): that window does not restart the plateau. A Request unanswered for longer than this does.
inline constexpr uint32_t kPlateauRenewMs = 1000;
inline constexpr int kCappedFloorMv = 5500;
// A QC Class B 20 V row (15 s QC2 / 12 s QC3 timeout) whose VBUS has stayed inside the 12 V band for kClassAStuckMs since the row
// started is a Class A top: nothing is on its way to 20 V. The row ends as the timeout would end it, without the rest of the wait.
inline constexpr uint32_t kClassAStuckMs = 3000;
inline constexpr int kClassAStuckLoMv = 11400, kClassAStuckHiMv = 12600;
class VbusMean {
public:
    void clear() { q8_ = -1; }
    // dt_ms: time since the previous sample. The first sample after clear() seeds the mean.
    void add(int mv, uint32_t dt_ms) {
        if (q8_ < 0) { q8_ = mv * 256; return; }
        const int32_t diff = static_cast<int32_t>(mv) * 256 - q8_;
        const uint32_t dt = std::min(dt_ms, kBandTauMs);
        q8_ += math::mul_div_i32(diff, static_cast<int32_t>(dt), static_cast<int32_t>(kBandTauMs));
    }
    int mean() const { return q8_ < 0 ? -1 : (q8_ + 128) >> 8; }   // mV, -1 when there is no sample
private:
    int32_t q8_ = -1;   // mV in 1/256 mV
};

// walk-settle: the VBUS samples of one granted walk level, kept for the last kWalkSteadyWindowMs. steady() = at least
// kWalkSteadySamples of them all within kWalkSteadyMv (max - min) - the output has stopped moving. Pure logic over
// (time, mV) pairs, host-tested directly.
class WalkSteady {
public:
    void clear() { n_ = 0; }
    void add(uint32_t now_ms, int mv) {
        if (n_ < kCap) {
            t_[n_] = now_ms; v_[n_] = mv; ++n_;
        } else {   // full: shift left, append (polls are >= 15 ms apart, so the window holds at most kCap samples)
            for (unsigned i = 1; i < kCap; ++i) { t_[i - 1] = t_[i]; v_[i - 1] = v_[i]; }
            t_[kCap - 1] = now_ms; v_[kCap - 1] = mv;
        }
        while (n_ > 0 && now_ms - t_[0] > kWalkSteadyWindowMs) {   // drop samples older than the window
            for (unsigned i = 1; i < n_; ++i) { t_[i - 1] = t_[i]; v_[i - 1] = v_[i]; }
            --n_;
        }
    }
    bool steady() const {
        if (n_ < kWalkSteadySamples) return false;
        int lo = v_[0], hi = v_[0];
        for (unsigned i = 1; i < n_; ++i) { if (v_[i] < lo) lo = v_[i]; if (v_[i] > hi) hi = v_[i]; }
        return hi - lo <= kWalkSteadyMv;
    }
    unsigned count() const { return n_; }
    // The level the window stands for: the mean of its samples (a ripple around a held level averages out, one sample reads the
    // ripple). `fallback` (the latest sample) when the window is empty.
    int mean(int fallback) const {
        if (!n_) return fallback;
        int32_t sum = 0;
        for (unsigned i = 0; i < n_; ++i) sum += v_[i];
        return static_cast<int>((sum + static_cast<int32_t>(n_) / 2) / static_cast<int32_t>(n_));
    }
private:
    static constexpr unsigned kCap = 20;   // 300 ms of samples at the 15 ms worst-case poll cadence
    std::array<uint32_t, kCap> t_{};
    std::array<int, kCap> v_{};
    unsigned n_ = 0;
};

class Runner {
public:
    explicit Runner(Env& env) : env_(env) {}
    static Summary preview(const pd::SourceCaps&, Options, Input);
    [[gnu::noinline]] Reason start(const pd::SourceCaps&, Options, Input, bool confirmed);
    void poll(Input, const pd::SourceCaps&);
    // A START the preflight refused (no run began; the run id stays where it was): one [TEST] log line with the
    // reason, and the reason + measured VBUS recorded here, so the remote summary (charger-test status) and the
    // card's final-reason path read them. A WrongVoltage under a live PDO 1 contract names the level in the log:
    // "VBUS 8.5 V on a 5 V contract (shared bus)". A Busy refusal names the holding tool via `detail` (the UI's
    // busy_reason footer text, e.g. "PD hold: release it"), so the log line says which tool holds the line; an Inline
    // refusal names what latched the inline state (pd::inline_cause_name: load or sink request). Only outside a run.
    void note_preflight_refusal(Reason r, int vbus_mv, const char* detail = nullptr);
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
    [[gnu::noinline]] void restore(const Options&, const Evidence&, uint32_t run_id, std::span<const Step>, Phase, size_t phase_index, uint8_t resumed,
                 size_t attempt_unit = 0, uint8_t attempts = 0, bool planned_reattach = false);
    bool resumable() const;
    // A self-powered run that lost VBUS: Stopped, every unfinished row still Pending (nothing skipped, no verdict), resumable
    // from memory when the power comes back, and from the journal after a reboot. The page must not close the journal then.
    bool suspended() const { return suspended_; }
    Reason resume(Input, bool confirmed);
    uint8_t resumed() const { return resumed_; }
    Level level() const { return opts_.level; }   // the level of the run in hand (started or restored)
    uint8_t reattach_count() const { return reattach_; }   // how many times the run went through the planned re-attach
    bool busy() const { return (state_ >= State::Baseline && state_ <= State::Releasing) || state_ == State::Unsafe || state_ == State::Detach || state_ == State::Attach || state_ == State::Ceiling || state_ == State::Spec; }
    // The page scans for the source's Rp while a unit attaches (the main loop's own scan is too slow for a fast announcer).
    bool attaching() const { return state_ == State::Attach; }
    State state() const { return state_; }
    // A row is in flight: its Request has gone out (or is about to be answered) and its verdict is not recorded yet. The main
    // loop also serves the protocol engines (a QC pulse train, a UFCS Accept, a PD transition), so the page paints and journals
    // nothing heavy then (a full LCD redraw is 100-200 ms, a littlefs erase masks interrupts). Baseline, Releasing, the unit
    // attach / detach states and the end of the run are the gaps in which that work is done. Derived from the state: no storage.
    // The SVOOC ceiling stage counts: its wire is timed in the main loop too, and a littlefs erase masks interrupts.
    bool quiet() const { return state_ == State::Requesting || state_ == State::Holding || state_ == State::Ceiling || state_ == State::Spec; }
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
    uint32_t unit_time_ms(Unit u) const { return u < Unit::Count ? unit_time_[size_t(u)] : 0; }   // busy ms spent in the unit this run
    // The run's units in execution order (the rows of steps() follow the same order).
    std::span<const Unit> unit_order() const { return {order_.data(), n_order_}; }
    Mode attach_mode() const { return mode_; }
    // The first Legacy attach of the run (report card): FellBack = quiet and steady at 5 V, StayedPd = blocked by PD.
    HandoverInfo handover() const { return handover_; }
    Reason reason() const { return reason_; }
    uint32_t run_id() const { return run_id_; }
    uint32_t revision() const { return revision_; }
    const Evidence& evidence() const { return evidence_; }
    // The PD spec stage per PPS / AVS APDO (the range rows' spec cells), folded live from the engine's programmable measurements
    // (Env::on_apdo); clear = the engine started again from nothing. Beside Evidence, not in it: a snapshot copies Evidence on the stack.
    const specsec::Apdos& spec_apdo() const { return spec_apdo_; }
    void spec_apdo(pdspec::Check c, const pdspec::Meas& m) { spec_apdo_.add(c, m); }
    void spec_apdo_clear() { spec_apdo_ = {}; }
    void spec_apdo_restore(const specsec::Apdos& a) { spec_apdo_ = a; }   // after restore(): the journal's ranges
    size_t index() const { return index_; }
    std::span<const Step> steps() const { return {steps_.data(), count_}; }
    uint32_t hold_left_ms(uint32_t now) const;
    // The Xiaomi unit: the chain outcome (mi_unlock::chain_code; 0 = the chain row has not run) and the modes it unlocked (before
    // and after PDO word, slot), latched by the chain row. mi_mode: the mode a mode row tests (by its slot), null for any other row.
    uint8_t mi_chain() const { return evidence_.mi_chain; }
    std::span<const mi_unlock::Mode> mi_modes() const { return {evidence_.mi.data(), evidence_.mi_n}; }
    const mi_unlock::Mode* mi_mode(const Step&) const;
    // The deep sweep (ROADMAP "PPS deep sweep"): the finished rows (one per APDO part: a PPS sweep is split at 5 V) and the whole point record
    // (one int16 error per point). The results strip, the card and the remote read it through this view.
    ctsweep::View sweep_view() const { return {{sweep_rows_.data(), n_sweep_rows_}, {sweep_err_.data(), sweep_used_}}; }
    // PD Stress borrows the point buffer for its own record (no second 1.3 KB of .bss). Refused while a test runs; the last test's sweep
    // plot is gone afterwards (the view reads empty), which the caller says out loud. Empty span = refused.
    std::span<int16_t> claim_sweep_record() {
        if (busy()) return {};
        n_sweep_rows_ = 0;
        sweep_used_ = 0;
        sweep_first_ = sweep_n_ = 0;
        return {sweep_err_.data(), sweep_err_.size()};
    }
    // The sweep row under test, live: its point range, the setpoint in flight and the last recorded point (the Running view).
    ctsweep::Live sweep_live() const {
        ctsweep::Live l;
        if (index_ >= count_ || steps_[index_].verdict != Verdict::Running || !is_sweep(steps_[index_]) || sweep_hi_ < sweep_lo_) return l;
        l.active = true;
        l.lo_mv = sweep_lo_; l.hi_mv = sweep_hi_; l.set_mv = sweep_mv_; l.n = sweep_n_;
        if (sweep_n_ && sweep_first_ + sweep_n_ <= ctsweep::kMaxPoints) l.last_err = sweep_err_[sweep_first_ + sweep_n_ - 1u];
        return l;
    }
private:
    void add(Step);
    void build(const pd::SourceCaps&, Options, Input);
    void caps_evidence(const pd::SourceCaps&);
    void replan_pd(const pd::SourceCaps&, Input, bool refresh_evidence);
    void replan_fc(const Input&);   // the SCP / UFCS rows still to run, planned again from what the unit's first row read (fc::Caps)
    [[gnu::noinline]] size_t replan_fc_rows(const Input&, size_t& first);   // its planning half: rows replaced (0 = none)
    void regrow_plan(const pd::SourceCaps&, const Input&);
    void follow_list(const pd::SourceCaps&, const Input&);   // the source announced again mid-run (pattern budget-reannounce-mid-run): per PD row the
    // new list is followed where it still offers the row (new slot and PDO word), rows with no offer are skipped as Withdrawn, rows a
    // withdrawn PDO comes back for are Pending again; before the first PD row the plan is rebuilt from the new list (replan_pd).
    // A COMPLETE EPR_Source_Capabilities outside the K2's own EPR entry/exit (epr_stage_ == 2) is a re-announce like any other (2026-11-09);
    // only a partial chunk stays excluded. When the row in flight was re-placed (new slot / word) it is restarted there once the list
    // has settled - the restart is not a try (flight_restart_).
    bool list_settled(uint32_t now) const { return list_ms_ == 0 || now - list_ms_ >= kListSettleMs; }   // rule 1: no Request until the list in hand is kListSettleMs unchanged
    void record_pd_skips(bool final = false);
    void epr_skips(Reason, uint8_t detail = 0);   // detail: the source's Enter_Failed code for EprRefused
    bool info_ready(const Input&) const;                 // the EPR list the charger answered is usable
    bool ask_info(const Input&);                         // false while the informational EPR list is still being asked, true when done
    void epr_info_rows(const pd::SourceCaps&, const Input&);   // lay the EPR rows out per level from the info list (when one was heard)
    void request(const Input&);
    void finish(const Input&, Verdict, Reason, uint8_t detail = 0);
    void finish_row(const Input&, Verdict, Reason, uint8_t detail);   // finish() once the retry checks (cut_hold, race_hold) let the verdict stand
    void release(const Input&, bool stop);
    void reissue_release();
    void next(const Input&);
    size_t pick() const;
    size_t scp_next(const Input&, bool passed) const;
    void skip_group_rows(Group g, bool raises_only, uint8_t detail);   // every Pending row of g (only those above 5 V) ends Skip / Request
    void skip_pending(Reason);
    void suspend();
    bool unfinished() const;   // a row is still Pending or Running
    void skip_unit(Reason);
    void skip_legacy_units(Reason);   // every Legacy unit's pending rows (the PD-only answer: no later PD-silent attach)
    bool legacy_row_ran() const;      // a Legacy row of the run has a protocol verdict (Pass/Fail)
    bool unit_row_ran() const;        // a row of the running unit has a protocol verdict (Pass/Fail)
    bool legacy_row_cut() const;      // a Legacy row of the run failed as Lost: a row of ours already cost the source its output
    void begin_attach(const Input&);
    void end_run(const Input&);
    void unsafe(Reason);
    void begin_unit(Unit, const Input&);
    void continue_legacy(Unit, const Input&);
    void begin_detach(const Input&);
    void poll_detach(const Input&);
    void poll_attach(const Input&, const pd::SourceCaps&, uint32_t gap);
    void fail_unit(const Input&, const pd::SourceCaps&, Reason = Reason::NoReattach, int measured_mv = -1);
    // ^ measured_mv < 0: record the live in.vbus_mv (a genuine NoReattach keeps the reading at the fail, not a clamped 0)
    void end_legacy();
    void end_probe();
    void end_sag_recovery(const Input& in);
    void retry_pd_row(const Input& in);
    // noinline: LTO folded poll_impl into the single Runner::poll call site (ChargerTestSession::runner_tick), next to the
    // caller's Input copies: 1048 B of frame against FRAME_MAX 1024.
    [[gnu::noinline]] void poll_impl(Input&, const pd::SourceCaps&);
    void count_unit_time(uint32_t now);
    uint32_t hold_ms() const;
    // The SVOOC ceiling stage: queued by finish() when the VOOC family was detected and the options asked for it, begun by next()
    // once the family session is released (D+/D- free, 5 V), ended by poll_ceiling(). One stage per run.
    bool ceiling_wanted() const;
    bool begin_ceiling(const Input&);   // false = the stage did not start (refused): the run goes on
    void poll_ceiling(const Input&);
    void end_ceiling(const Input&, svooc_ceiling::End);
    void note_ceiling(const Input&, svooc_ceiling::End);
    svooc_ceiling::End ceil_end_ = svooc_ceiling::End::None;
    bool ceiling_pending_ K2_BSS_INIT(false), ceiling_done_ K2_BSS_INIT(false), ceil_stopping_ K2_BSS_INIT(false);
    // ceil_recover_: the release after the stage watches for the adapter dropping VBUS (an Rd cycle brings it back);
    // ceil_low_ms_: when VBUS went under vSafe5V in that release (0 = it is not).
    bool ceil_recover_ K2_BSS_INIT(false);
    bool ceil_cycled_ K2_BSS_INIT(false);   // that Rd cycle ran: an attach after it that never sees VBUS is a lost source (poll_attach)
    uint8_t ceil_why_ K2_BSS_INIT(0);   // the Runner's own guard that stopped the trigger, as a svooc::Abort (0 = the trigger's own end)
    uint32_t ceil_ms_ K2_BSS_INIT(0), ceil_stop_ms_ K2_BSS_INIT(0), ceil_low_ms_ K2_BSS_INIT(0);
    // The PD spec stage: begun by next() at the end of the Pd unit (after its contract is released, at a settled 5 V), ended by
    // poll_spec(). One stage per run; a resumed run never repeats it (its result is journaled).
    bool spec_wanted() const;
    bool begin_spec(const Input&);   // false = the stage did not start (refused): the run goes on
    void poll_spec(const Input&);
    void end_spec(const Input&, specsec::End);
    bool spec_pending_ K2_BSS_INIT(false), spec_done_ K2_BSS_INIT(false), spec_stopping_ K2_BSS_INIT(false), spec_fault_ K2_BSS_INIT(false);
    uint32_t spec_ms_ K2_BSS_INIT(0), spec_stop_ms_ K2_BSS_INIT(0);
    Env& env_;
    Evidence evidence_;
    specsec::Apdos spec_apdo_{};
    std::array<Step, kMaxSteps> steps_ K2_BSS_ZERO;
    size_t count_ K2_BSS_INIT(0), index_ K2_BSS_INIT(0), pd_retry_ = SIZE_MAX;
    uint8_t retries_ K2_BSS_INIT(0);         // retries the row pd_retry_ has had (a source Hard Reset or loss in the row)
    uint8_t epr_tries_ K2_BSS_INIT(0);       // retries of the baseline EPR entry (a source Hard Reset in the entry)
    uint32_t settle_ms_ K2_BSS_INIT(0);      // extra steady 5 V a retry waits for in Releasing (kEprSettleMs + backoff)
    uint32_t settle_until_ K2_BSS_INIT(0);   // an EPR entry request waits for this time (after a Reject: kRejectSettleMs)
    bool can_retry(const Step&) const;
    // The Xiaomi chain row waits for the sink's auth chain (Input::pd.xiaomi_svid && !xiaomi_authed), at most kXiaomiAuthWaitMs per
    // attach: auth_key_ = session + Hard Reset count the budget was opened for, auth_at_ = when. A finished chain also gives the
    // re-announced list kXiaomiReannounceMs.
    bool mi_chain_wait(const Input&);
    // The chain row's turn: the chain outcome, the diff of the list in hand against XiaomiTrace::pre, and one mode row per unlocked
    // PDO inserted right behind the chain row (skipped "cable limit" / over the monitor / Light where they cannot go out). No Request.
    void mi_latch(const Input&);
    bool insert_row(size_t at, const Step&);   // the rows from `at` move down; false when the plan is full
    void drop_row(size_t i);
    bool auth_open_ K2_BSS_INIT(false); uint32_t auth_key_ K2_BSS_INIT(0), auth_at_ K2_BSS_INIT(0);
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
    void track_plateau(const Input&, bool ready, bool renewing, uint32_t gap);
    int from_mv_ K2_BSS_INIT(0);          // VBUS when the Requesting row's level was asked (0 = unknown): a programmable row going up gets the tight band
    uint16_t plat_mv_ K2_BSS_INIT(0);     // the level a Requesting programmable row's samples have stayed within 1 % of (0 = none yet)
    uint16_t plat_nr_ms_ K2_BSS_INIT(0);  // how long the sink's keep-alive Request has been unanswered inside that plateau (kPlateauRenewMs)
    uint32_t plat_ms_ K2_BSS_INIT(0);     // since when
    // The lowest / highest VBUS a Requesting QC Class B 20 V row has seen (kClassAStuck*); reset by request().
    uint16_t peak_lo_mv_ = 0xFFFF, peak_hi_mv_ K2_BSS_INIT(0);
    // The step-walk row in flight (is_step_walk): walk_mv_ = the target of the Request out now (s.mv at the first one,
    // +kStepMv per granted step), walk_n_ = levels granted after the base, walk_prev_mv_ = VBUS at the previous granted level.
    uint16_t walk_mv_ K2_BSS_INIT(0);
    uint8_t walk_n_ K2_BSS_INIT(0);
    int walk_prev_mv_ = -1;
    int walk_base_mv_ = -1;   // VBUS at the walk's base level (the sum check)
    bool walk_rebased_ K2_BSS_INIT(false);   // the base was an overshoot plateau and was replaced once by the first step's level (walk_step)
    std::array<int, kStepWalkCount + 1> walk_lv_;   // VBUS recorded at each granted level, base first (the trend check)
    // walk-settle: before a granted level is recorded, VBUS must be steady (walk_steady_) or the wait timed out
    // (walk_grant_ms_ + kWalkSteadyTimeoutMs); both reset when the walk starts and when it advances to the next level.
    WalkSteady walk_steady_;
    uint32_t walk_grant_ms_ K2_BSS_INIT(0);   // first walk_step call at the current granted level (0 = none yet)
    void walk_step(const Input&, const Step& eff);   // one granted level of the walk: record it, ask for the next or judge the walk
    // The deep-sweep row in flight (is_sweep). The record: one int16 error (measured - setpoint) per point, no heap;
    // sweep_rows_ holds one RowView per APDO part (ctsweep::part_range), filled when the row closes (sweep_close in finish()).
    alignas(4) std::array<int16_t, ctsweep::kMaxPoints> sweep_err_;
    std::array<ctsweep::RowView, ctsweep::kMaxRows> sweep_rows_ K2_BSS_ZERO;
    uint8_t n_sweep_rows_ K2_BSS_INIT(0);
    uint16_t sweep_used_ K2_BSS_INIT(0);                 // points recorded so far, all sweep rows together
    uint16_t sweep_first_ K2_BSS_INIT(0), sweep_n_ K2_BSS_INIT(0);   // the row in flight: first slot in sweep_err_, points so far
    uint16_t sweep_mv_ K2_BSS_INIT(0), sweep_lo_ K2_BSS_INIT(0), sweep_hi_ K2_BSS_INIT(0);   // the point whose Request is out / the row's point range
    void sweep_begin(const Step&, const Input&);   // request(): the point range from the row's PDO word, the row's record reset
    void sweep_point(const Input&, const Step&);   // one granted point: record it, ask the next or finish the row
    void sweep_close(Step&);                // finish(): summarize the row's points into sweep_rows_ (and mark a fold, fold_caution)
    void fold_caution(Step& sweep, const ctsweep::Summary&);   // a fold: the sweep row and the APDO's fixed rows above it read Reason::Folded
    bool xiaomi_seen_ K2_BSS_INIT(false), xiaomi_reset_ K2_BSS_INIT(false), x_svid_ K2_BSS_INIT(false), x_authed_ K2_BSS_INIT(false); uint32_t tail_at_ K2_BSS_INIT(0), hr_count_ K2_BSS_INIT(0);
    size_t cut_index_ = SIZE_MAX;   // a low programmable row whose source went dark: Releasing decides replug-or-carry-on
    State state_ = State::Idle;
    Reason reason_ = Reason::None;
    uint16_t refusal_mv_ K2_BSS_INIT(0);   // the measured VBUS of the last preflight refusal (note_preflight_refusal)
    uint32_t release_ms_ K2_BSS_INIT(0), phase_ms_ K2_BSS_INIT(0), stable_ms_ K2_BSS_INIT(0), last_ms_ K2_BSS_INIT(0), session_ K2_BSS_INIT(0), resets_ K2_BSS_INIT(0);
    uint32_t run_id_ K2_BSS_INIT(0), revision_ K2_BSS_INIT(0);
    uint8_t resumed_ K2_BSS_INIT(0);
    uint8_t reattach_ K2_BSS_INIT(0);        // planned re-attaches this run went through (not counted in resumed_)
    bool reattach_planned_ K2_BSS_INIT(false);   // latched from restore(): this boot came back from the planned re-attach
    bool loaded_ K2_BSS_INIT(false);
    bool selfpwr_ K2_BSS_INIT(false);     // latched from Input::self_powered at start()/resume(): fusb302::self_powered() flips with a detach
    bool suspended_ K2_BSS_INIT(false);   // a self-powered VBUS loss ended the run without a verdict (see suspended())
    bool adopt_ K2_BSS_INIT(false);       // a resumed Baseline: the page arms the spoof/cable after resume(), a new session there is not a loss
    bool held_ K2_BSS_INIT(false);        // hold(): a fresh run waits in its first Baseline (the page is writing the journal's plan)
    uint32_t jwait_ms_ K2_BSS_INIT(0);    // request() parked since this time (now | 1) because the journal was not ready; 0 = not waiting
    // pattern usb-a-cuts-on-low-dpdm (see kSagAdoptMs): sag_cut_ = a legacy row failed as Lost and its recovery is running;
    // sag_open_ = that recovery ended at sag_ms_ and a new attach session within kSagAdoptMs is adopted.
    // gone_ = Unsafe is timing the unplug (VBUS under 1 V since stable_ms_, see kUnsafeGoneMs); fits the padding before sag_ms_.
    // own_cut_ = the window above was opened by the running unit's Baseline finding VBUS under vSafe5V before any row of the unit ran
    // (the source cut its own output); cleared at every unit start. VBUS then dark past the window ends that unit, not the run.
    bool sag_cut_ K2_BSS_INIT(false), sag_open_ K2_BSS_INIT(false), gone_ K2_BSS_INIT(false), own_cut_ K2_BSS_INIT(false); uint32_t sag_ms_ K2_BSS_INIT(0);
    bool recover_ K2_BSS_INIT(false), hot_ K2_BSS_INIT(false), stable_ K2_BSS_INIT(false), stopping_ K2_BSS_INIT(false), pd_owned_ K2_BSS_INIT(false), fc_owned_ K2_BSS_INIT(false);
    bool restart_dark_ K2_BSS_INIT(false), restart_wait_ K2_BSS_INIT(false);   // a silent Legacy release: the source's output went dark / came back
    uint32_t restart_ms_ K2_BSS_INIT(0);                             // when it came back (restart_wait_)
    // own_rel_ = the last fc release of a silent Legacy attach is watched (kOwnReleaseCapsMs), own_rel_ms_ = its time; reann_ = what this
    // attach's source did after a release: 0 not seen yet, 1 quiet (not watched again), 2 announces (every release is waited out).
    bool own_rel_ K2_BSS_INIT(false); uint8_t reann_ K2_BSS_INIT(0); uint32_t own_rel_ms_ K2_BSS_INIT(0);
    // pattern usb-a-no-load-auto-off (kCutMv): the cut clocks of the legacy attach. cut_start_ms_ = the first sample under kCutMv of the
    // last completed fall, cut_back_ms_ = the first vSafe5V sample after it, both 0 = none since the row's request, row_hi_ = the highest VBUS since the row's request.
    // cut_onsets_ = the onsets of the cuts that came back inside kCutWindowMs (the period is read from them), cut_retry_idx_ = the row
    // asked again after a cut (up to kCutRetries per row).
    // cut_dark_ / cut_dark_ms_ / cut_last_mv_ = a fall under kCutMv in progress, its first sample, the sample before it.
    uint32_t cut_start_ms_ K2_BSS_INIT(0), cut_back_ms_ K2_BSS_INIT(0), cut_dark_ms_ K2_BSS_INIT(0); int row_hi_ K2_BSS_INIT(0), cut_last_mv_ = -1; bool cut_dark_ K2_BSS_INIT(false);
    std::array<uint32_t, kCutOnsets> cut_onsets_ K2_BSS_ZERO; uint8_t n_cut_onsets_ K2_BSS_INIT(0);
    size_t cut_retry_idx_ = SIZE_MAX; uint8_t cut_tries_ K2_BSS_INIT(0);   // the row asked again, and how many times (kCutRetries at most)
    // kDipMinMv: dip_open_ = VBUS has been in the dip range since dip_ms_; dip_back_ = it came back to vSafe5V at dip_back_ms_ (its tail
    // session is adopted for kDipTailMs); dips_ = the dips that came back since the last row ran (kCutRetries are waited for).
    uint32_t dip_ms_ K2_BSS_INIT(0), dip_back_ms_ K2_BSS_INIT(0); bool dip_open_ K2_BSS_INIT(false), dip_back_ K2_BSS_INIT(false); uint8_t dips_ K2_BSS_INIT(0);
    // A casualty whose cut is still in progress waits in Releasing (cut_wait_) for VBUS to come back: the row is Pending, the verdict it
    // would have had is kept here (cut_v_ / cut_why_ / cut_detail_ / cut_mv_ = the VBUS of that moment), cut_final_ = the retries of this row are used up.
    bool cut_wait_ K2_BSS_INIT(false), cut_final_ K2_BSS_INIT(false); Verdict cut_v_ = Verdict::Fail; Reason cut_why_ = Reason::None; uint8_t cut_detail_ K2_BSS_INIT(0); int cut_mv_ K2_BSS_INIT(0);
    // pattern legacy-start-races-announcement: a unit that started inside the source's announcement (PD quiet under kLegacyQuietMs at its
    // first fast-charge request) asks its first step once more after real quiet when it fails with a race-type reason (race_hold).
    // race_seen_ = the unit's first request was made, race_idx_ its row, race_tried_ = the one retry is used, race_wait_ = the retry's
    // Releasing wait (for kLegacyQuietMs of quiet, at most kRestartSettleMs from race_ms_).
    bool race_seen_ K2_BSS_INIT(false), race_fast_ K2_BSS_INIT(false), race_tried_ K2_BSS_INIT(false), race_wait_ K2_BSS_INIT(false);
    size_t race_idx_ = SIZE_MAX; uint32_t race_ms_ K2_BSS_INIT(0);
    bool race_hold(const Input&, Verdict, Reason, uint8_t detail);   // finish(): a first step the announcement spoiled is asked again (true)
    void race_reset() { race_seen_ = race_fast_ = race_tried_ = race_wait_ = false; race_idx_ = SIZE_MAX; }
    void track_cut(const Input&);
    void track_dip(const Input&);                            // the dip clocks above (every poll)
    bool dip_wait(const Input&) const;                       // VBUS is in a dip that may still come back: the 5 V check waits
    bool dip_adopts(const Input&) const;                     // a session change is the dip's (during it, or its tail after the comeback)
    bool cut_hold(const Input&, Verdict, Reason, uint8_t detail);   // finish(): a casualty of a port cut is asked again (true) instead of failed
    void cut_resolve(Input);                                 // Releasing: the held casualty's cut came back (retry or skip) or the window ran out (it fails)
    // Sink Hard Resets sent when the release started: a later one is the sink's own last resort for a release that did not
    // bring VBUS down (pattern budget-reannounce-mid-run), a recovery and not a lost source.
    uint32_t rel_hr_ K2_BSS_INIT(0);
    uint32_t busy_retry_at_ K2_BSS_INIT(0);   // a PD request refused Busy (identify/probe/prep running) is retried, not failed
    HandoverInfo handover_ K2_BSS_ZERO;
    // The units of the run in execution order (the rows are laid out in the same order) and the one running.
    std::array<Unit, kMaxUnits> order_ K2_BSS_ZERO;
    uint8_t n_order_ K2_BSS_INIT(0);
    Unit unit_ = Unit::Count;
    Mode mode_ = Mode::Pd;
    bool attached_ K2_BSS_INIT(false);       // the unit's attach is done: its rows may run
    bool detached_ K2_BSS_INIT(false);       // the last act was a detach (Rd off): the next unit attaches without detaching again
    bool ending_ K2_BSS_INIT(false);         // the running detach ends the unit (not a Legacy retry)
    bool silent_ K2_BSS_INIT(false);         // a Legacy attach was verified PD-quiet (a resumed run has no such proof)
    bool legacy_active_ K2_BSS_INIT(false);  // the last attach was Legacy: legacy_only(false) + pd_rescan() restore the sink at the end
    bool never_dark_ K2_BSS_INIT(false);     // a detach that left VBUS up (a USB-A port has no CC): later detaches do not wait long
    bool rel_rd_ K2_BSS_INIT(false);         // the Rd off that answers a D+/D- release which left VBUS up: its end checks VBUS before going on
    bool dipped_ K2_BSS_INIT(false);         // VBUS fell during this detach
    bool late_pd_ K2_BSS_INIT(false);        // no source caps at START: the PD rows are planned from the first list heard
    uint32_t plan_sig_ K2_BSS_INIT(0);       // caps_sig() of the list the PD rows were last planned from
    // Restart rules for a mid-run re-announce (2026-11-09): every new list opens a settle window (list_ms_ = when the
    // list in hand first arrived, list_sig_ = its caps_sig; an invalid->valid transition is a new list too). No Request
    // is sent while the window is open (list_settled()). The window is tracked only in the run states - Attach/Detach
    // settle the list on their own - and is seeded (not opened) by the first run-state poll after start()/restore().
    uint32_t list_ms_ K2_BSS_INIT(0), list_sig_ K2_BSS_INIT(0);
    bool list_valid_ K2_BSS_INIT(false), list_tracking_ K2_BSS_INIT(false);
    bool flight_restart_ K2_BSS_INIT(false);   // the row in flight was re-placed by follow_list: re-ask at the new slot/word once the list settles
    pd::SourceCaps caps_now_ K2_BSS_ZERO;     // the list of the last poll (the end-of-unit sweep re-checks the Withdrawn rows against it)
    bool wd_wait_ K2_BSS_INIT(false);          // the unit is held open for a withdrawn offer to return (next(), kWithdrawnWaitMs from wd_wait_ms_)
    uint32_t wd_wait_ms_ K2_BSS_INIT(0);
    bool plan_frozen_ K2_BSS_INIT(false);    // a restored run: the journaled rows are the plan, a later list does not replan them
    uint8_t fc_replanned_ K2_BSS_INIT(0);    // bit 0: the SCP rows follow the adapter's limits already, bit 1: the UFCS rows follow its modes
    bool pd_skips_recorded_ = true, probe_asked_ K2_BSS_INIT(false);
    bool pd_skips_held_ K2_BSS_INIT(false);   // record_pd_skips kept provisional EPR "not offered" rows back (the entry has not decided them yet)
    uint8_t attach_try_ K2_BSS_INIT(0);
    uint32_t unit_ms_ K2_BSS_INIT(0), att_ms_ K2_BSS_INIT(0);   // the unit's detach began; this attach phase began
    std::array<uint32_t, size_t(Unit::Count)> unit_time_ K2_BSS_ZERO;   // busy ms per unit this run (the end-of-run [CT] split)
    uint32_t time_ms_ K2_BSS_INIT(0);                                    // the poll the split last counted to (0 = idle)
    bool time_pending_ K2_BSS_INIT(false);                               // a run started and its split is not logged yet
    uint32_t pre_ms_ K2_BSS_INIT(0);                 // extra quiet before the unit's first handshake, on top of the 500 ms baseline settle
    enum class Att : uint8_t { Caps, Contract } att_ = Att::Caps;
    bool rp_restart_ K2_BSS_INIT(false);   // the attach wait was restarted once after a source-side Rp drop (pattern slow-rp-wake)
    bool contract_wrong_ K2_BSS_INIT(false);   // latched in poll_attach: PS_RDY at PDO 1 was seen during this attach wait with VBUS outside vSafe5V
                                    // (pattern shared-bus-split). The source may tear the port down before the timeout, so the
                                    // fail sites cannot re-ask the sink state - they read this latch.
    int attach_max_mv_ K2_BSS_INIT(0);     // the highest VBUS seen during the current attach wait (WrongVoltage records this, not the timeout reading)
    // Latched per PD row (reset in request()), read where a lost programmable row ends: pattern pps-off-at-no-load = accepted (the
    // sink waited in Transition), never PS_RDY (Ready with this row's PDO), the port drew under kNeedsLoadMaxMa the whole row.
    bool row_accepted_ K2_BSS_INIT(false), row_ps_rdy_ K2_BSS_INIT(false); int row_max_ma_ K2_BSS_INIT(0);
    // The timing quirks of this row (TimingFlag bits, over its walk levels / sweep points) and its worst Accept -> PS_RDY time. They are
    // reset in request(); the accepted / PS_RDY latches above restart at every walk level and sweep point (level_mv_).
    uint8_t row_timing_ K2_BSS_INIT(0); uint16_t row_ps_rdy_ms_ K2_BSS_INIT(0);
    int level_mv(const Step&) const;   // the voltage the Request out now asks (the row's, a walk level's, a sweep point's)
    void latch_row(const Input&);
    void note_ps_rdy(const Input&, const Step&);
    bool needs_load(const Input&) const;
    bool prog_wanted(const Step&) const;       // a fixed PPS / AVS row of a run with the Deep checks on (Options::pps_sweep / avs_sweep)
    bool prog_on_ K2_BSS_INIT(false);          // the row in flight asked for a transition capture at its hold's start (Env::prog_begin said yes)
    bool spec_feed(const Input&, const Step&, int from_mv, int to_mv, uint8_t cls, uint8_t flags);   // true: wait, the stage takes the move
    uint32_t feed_ms_ K2_BSS_INIT(0);          // a move was handed to the spec stage, which is still judging it (0 = none)
    uint16_t prog_from_mv_ K2_BSS_INIT(0);     // the row's Request came from this set point of its own APDO (0 = from another level)
    enum class Probe : uint8_t { Off, Pending, Closing, Running } probe_ = Probe::Off;
    uint32_t probe_ms_ K2_BSS_INIT(0);
    Options opts_{};
    uint32_t caps_gone_ms_ K2_BSS_INIT(0);   // when the source caps went invalid (a hard reset re-announces them); 0 = valid
    uint8_t epr_stage_ K2_BSS_INIT(0);   // 0 none, 1 enter EPR when the Pd unit's 5 V contract is live, 2 waiting for EnterSucceeded + EPR caps, 3 wanted (decided at the Pd attach), 4 cable unfit: ask the EPR list only
    uint32_t epr_ms_ K2_BSS_INIT(0);
    uint8_t info_ K2_BSS_INIT(0);        // the informational EPR list (SPR-mode EPR_Get_Source_Cap): 0 not started, 1 asking (line busy), 2 asked / waiting, 3 done (once per Pd attach)
    uint32_t info_ms_ K2_BSS_INIT(0);
    bool info_rows_ K2_BSS_INIT(false);  // the EPR rows are laid out from Input::epr_info (the levels the charger offers) instead of one row per algorithm
    bool epr_proof_ K2_BSS_INIT(false);   // the source accepted EPR entry in this run: it checked the cable (50 V / 5 A) itself
    pd::VbusWait wait_;
    uint32_t dark_ms_ K2_BSS_INIT(0);   // VBUS below vSafe0V since
    bool dark_ K2_BSS_INIT(false);
};
}  // namespace k2::charger_test
