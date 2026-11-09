// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_setup_page.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include "k2/core/bbpersist.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/log.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/pd/boot_ra.hpp"
#include "k2/chargertest/charger_journal_hw.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/pd/transcap_reason.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/chargertest/report_card_delta.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/chargertest/run_events.hpp"
#include "k2/core/scratch.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/core/storage.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/drivers/usb.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace k2 {
// A reason as the screen words it. charger_test::name() is the vocabulary the card export, the web tools and the report cards
// share (it never changes for a display budget); the two reasons whose shared name does not fit the 26-27 cell lines say it shorter.
const char* reason_text(charger_test::Reason r) {
    switch (r) {
    case charger_test::Reason::Release: return "VBUS not safe 5V";
    case charger_test::Reason::ShutOff: return "Source shut off: replug";
    default: return charger_test::name(r);
    }
}
namespace {
namespace ct = charger_test;
using A = ct::Algorithm;
// Run events for the live debug stream (docs/DEBUG_STREAMING.md, k2/chargertest/run_events.hpp): the row whose RowStart went out last, so a
// row that starts without a hook of its own (the 5 V rows) is still announced exactly once.
constexpr uint16_t kNoRow = 0xFFFF;
uint16_t g_ev_row = kNoRow;
uint8_t ev8(size_t v) { return static_cast<uint8_t>(std::min<size_t>(v, 255)); }
// The families, in the default run order (docs/CHARGER_TEST_FLOW.md): OEM vendor modes by company, then USB PD, then the
// legacy / standard D+/D- protocols. Xiaomi / Redmi (Mi 6A) has no algorithm bit of its own: its probe rows are planned
// inside the PD unit, so its row follows the PD selection and cannot be toggled alone.
constexpr int kFamHuawei = 0, kFamSamsung = 1, kFamOppo = 2, kFamXiaomi = 3, kFamPd = 4, kFamQc = 5;
constexpr uint32_t kFamilies[] = {
    ct::bit(A::Fcp) | ct::bit(A::Scp),
    ct::bit(A::Afc),
    ct::bit(A::Vooc) | ct::bit(A::SuperVooc) | ct::bit(A::SuperVooc65) | ct::bit(A::Vivo),
    0,
    ct::kPdFixedAlgorithms | ct::kPpsAlgorithms,
    ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Qc3Plus) | ct::bit(A::Qc2ClassB) | ct::bit(A::Qc3ClassB) | ct::bit(A::Ufcs)   // Class B (20 V) is part of the QC family
};
constexpr const char* kFamilyNames[] = {"Huawei / Honor", "Samsung", "OPPO / vivo", "Xiaomi / Redmi", "USB PD", "QC / UFCS"};
// A family name is a Custom / Steps row text (16 px, x28 up to the count tail) and the Steps header title ("<name> steps").
static_assert(all_fit(kFamilyNames, kSmallAdvPx, 112), "a family name is wider than its row");
// Tests rows (wizard step 1): 0..3 the presets, 4 "Custom...", 5 "Last results" once a run exists.
// Custom rows: 0..kFamilyCount-1 the families, then "Done" (row kFamilyCount).
constexpr int kFamilyCount = 6, kPresets = 4, kCustomRow = 4, kViewRow = 5, kStartRow = 0;
// The presets are subsets of "All tests" (the default set, Class B included): PD = the PD family (its Mi 6A probes ride
// along), Legacy = every D+/D- family (OEM vendor modes + QC / UFCS), Vendor modes = the OEM D+/D- families only.
constexpr const char* kPresetNames[kPresets] = {"All tests", "PD only", "Legacy only", "Vendor modes"};
// The 12 px note under the focused row (x28 .. 218).
constexpr const char* kPresetNotes[kPresets] = {"Everything, QC Class B 20V too", "PD fixed, PPS, EPR, Mi 6A",
                                                "D+/D-: vendor modes, QC, UFCS", "Huawei, Samsung, OPPO, vivo"};
static_assert(all_fit(kPresetNotes, kLabelAdv, kit::kNoteMaxPx), "a preset note is wider than the row note");
static_assert(all_fit(kPresetNames, kSmallAdvPx, 150), "a preset name is wider than its row");
// The warning of a START with a cable identity on a K2 that runs on the measured port (Refuse stage, refuse_late_): the alert
// word (font 24 beside the triangle), two lines at x6 (16 px, 212 px wide) and the footer (27 characters).
constexpr const char* kLateWord = "Self-powered";
constexpr const char* kLateLines[2] = {"A charger reads the cable", "at plug-in, before the Ra."};
constexpr const char* kLateFooter = "Cable arrives after attach";
static_assert(all_fit(kLateLines, kSmallAdvPx, 212), "a self-powered warning line is wider than the screen");
static_assert(fits_px(kSmallAdvPx, kLateFooter, 216), "the self-powered footer is wider than the footer");
static_assert(fits_px(12, kLateWord, 180), "the self-powered alert word is wider than the band");
constexpr uint32_t kPresetMasks[kPresets] = {
    ct::kDefaultAlgorithms, ct::kDefaultAlgorithms & kFamilies[kFamPd],
    ct::kDefaultAlgorithms & ~kFamilies[kFamPd],
    kFamilies[kFamHuawei] | kFamilies[kFamSamsung] | kFamilies[kFamOppo]};
// The preset row the selection equals (1-based row), 0 = none: "Custom..." carries the pill then.
int preset_row_of(uint32_t selected) {
    for (int n = 0; n < kPresets; ++n) if (kPresetMasks[n] == selected) return n + 1;
    return 0;
}
constexpr int kBestChars = 26;   // the Results best line: 16 px (8 px cells) in the 212 px beside the page margins
using result_layout::kGridMetrics;
static_assert((kFamilies[0] | kFamilies[1] | kFamilies[2] | kFamilies[3] | kFamilies[4] | kFamilies[5]) == ct::kDefaultAlgorithms);
static_assert(kPresetMasks[3] != 0 && (kPresetMasks[3] & ct::kDefaultAlgorithms) == kPresetMasks[3]);
int family(A a) {
    for (int n = 0; n < kFamilyCount; ++n) if (kFamilies[n] & ct::bit(a)) return n;
    return -1;
}
// The family a result step belongs to: the Mi 6A probe rows are PD requests but read under Xiaomi / Redmi.
int family(const ct::Step& s) { return s.hunt ? kFamXiaomi : family(s.algorithm); }
// The Ra guard reads the lines while the identity can still be picked and START pressed, with the K2 passive.
bool guard_stage(ChargerTestPage::Stage s) { return s == ChargerTestPage::Stage::Cable || s == ChargerTestPage::Stage::Review; }
// A step-1 stage: the Tests list, its Custom sub-list, or one family's Group.
bool tests_stage(ChargerTestPage::Stage s) {
    return s == ChargerTestPage::Stage::Algorithms || s == ChargerTestPage::Stage::Custom || s == ChargerTestPage::Stage::Group;
}
unsigned selected(uint32_t mask) { return std::popcount(mask); }
// The footer line for a refused START. "tool busy" alone does not say which tool or what to do: Ui::busy_reason() does.
const char* review_refusal(ct::Reason r, const char* busy) {
    if (r == ct::Reason::Busy && busy) return busy;
    return reason_text(r);
}
unsigned entries(uint32_t mask) {
    return selected(mask & ~kFamilies[kFamPd]) + bool(mask & ct::kPdFixedAlgorithms) + bool(mask & ct::kPpsAlgorithms);
}
const char* detail_name(const ct::Step& s) {
    if (s.reason != ct::Reason::Request) return ct::explanation(s);
    if (s.group != ct::Group::Pd) return fc::name(static_cast<fc::Fail>(s.detail));
    return s.detail >= 128 ? pd::err_name(static_cast<pd::Err>(s.detail - 128)) :
                            pd::result_name(static_cast<pd::SinkResult>(s.detail));
}
// One line (n = 0..3) of a refused hunt row's evidence, each at most about 32 characters (the detail page's width).
void put_hunt_line(StrBuf<64>& s, const ct::HuntEvidence& h, int n) {
    const auto yn = [&](ct::HuntFlag f) { return h.flags & f ? 'Y' : 'n'; };
    if (n == 0) s.put("Why: ").put(ct::explanation(static_cast<ct::HuntCause>(h.cause)));
    else if (n == 1) {
        s.put("Chain svid:").put_char(yn(ct::HfSvid)).put(" open:").put_char(yn(ct::HfOpener)).put(" auth:").put_char(yn(ct::HfAuthed))
         .put(" re:").put_char(yn(ct::HfReannounced));
    } else if (n == 2) {
        s.put("Caps ").put_u32(h.caps).put(" APDO ").put_u32(h.apdo_ma).put("mA cable ").put_u32(h.cable_ma).put("mA");
    } else {
        static constexpr const char* kAnswers[] = {"no answer", "Accept", "Reject", "Wait"};
        if (!(h.flags & ct::HfSent)) { s.put(h.flags & ct::HfHeld ? "Request held, never sent" : "No Request went out"); return; }
        s.put("Ask #").put_u32(h.pos).put_char(' ').put_fixed(h.req_mv, 2).put("V ").put_u32(h.req_ma).put("mA ")
         .put(kAnswers[h.answer & 3]);
        if (h.answer) s.put_char(' ').put_u32(h.answer_ms).put("ms");
    }
}
void put_revision(StrBuf<48>& s, uint8_t wire, const pd::Revision& r) {
    s.put(pd::wire_revision_name(wire)).put("  RMDO ");
    if (r.valid) s.put_u32(r.rev_major).put_char('.').put_u32(r.rev_minor);
    else s.put("-");
}
struct Counts {
    unsigned pass = 0, fail = 0, skip = 0, total = 0;
};
Counts counts(const ct::Runner& runner, int f = -1) {
    Counts out;
    for (const auto& s : runner.steps()) {
        if (f >= 0 && family(s) != f) continue;
        ++out.total;
        out.pass += s.verdict == ct::Verdict::Pass;
        out.fail += s.verdict == ct::Verdict::Fail;
        out.skip += s.verdict == ct::Verdict::Skip;
    }
    return out;
}
}

// The K2 lives on the measured port: a pinned Rd (boot keys) or no host and VBUS at boot (a charger's A port through an A-to-C cable).
static bool self_supplied() { return fusb302::self_powered() || power_source::bus_powered(); }

charger_test::Input ChargerTestSession::input(uint32_t now) const {
    charger_test::Input in;
    in.now = now; in.session = fusb302::session(); in.cap_mv = ctx_.cap_mv;
    const auto& physical = fusb302::physical_cable();
    in.cable_mv = physical.max_mv; in.cable_ma = physical.max_ma; in.cable_observed = physical.observed;
    in.reported = fusb302::source_revision(); in.pd_traffic = fusb302::pd_traffic(); in.pd_seen_attach = fusb302::pd_seen_attach();
    in.spoof_on = emk_spoof::active();
    if (in.spoof_on && run_cable_.kind != ctcable::Kind::Off) { in.spoof_mv = run_facts_.max_mv; in.spoof_ma = run_facts_.max_ma; }   // the identity this run armed
    in.self_powered = self_supplied();
    in.vbus_mv = ctx_.pm_online ? ctx_.vbus_fast_mv : -1;
    in.ibus_ma = ctx_.pm_online ? static_cast<int>(std::min<uint32_t>(power::magnitude(ctx_.pm_avg.current_ma), INT32_MAX)) : 0;
    // No USB host: the K2 runs on this VBUS and its own draw crosses the shunt; only what is above it is a load.
    if (!usb::configured()) in.ibus_ma = std::max(0, in.ibus_ma - static_cast<int>(pd::kSelfSupplyAllowanceMa));
    in.inline_load = ctx_.inline_mode; in.fake = pd::is_fake(); in.busy = ctx_.ui->spoof_busy();
    in.pd = fusb302::sink_status();
    in.epr_info = &fusb302::epr_info_list();
    in.xm = &fusb302::xiaomi_trace();
    in.fc_caps = &fctrig::caps();
    const auto fs = fctrig::status();
    in.fc_state = fs.state; in.fc_fail = fs.fail; in.fc_tries = static_cast<uint8_t>(fs.tries);
    in.pd_quiet_ms = fusb302::pd_quiet_ms();
    in.probe_done = fusb302::probe_status().state != pd::ProbeState::Running;
    return in;
}
// The transition capture records every request that moves the rail off the 5 V default (a PPS / AVS step, a fixed
// 9 V and up): the C135-style shut-offs happen right after an Accept and need VBUS and CC at sub-millisecond
// resolution. arm() is cheap and refuses by itself while another tool owns the overlay.
pd::Err ChargerTestSession::request_pd(pd::Choice c) {
    if (c.pos > 1 || c.mv > 5000) {
        const auto st = transcap::status().state;
        if (st == transcap::State::Off || st == transcap::State::Armed) {   // not over a capture still being written
            if (transcap::arm(c.pos, c.mv) == transcap::ArmResult::Ok) capture_kept_ = false;   // a new recording: its row may keep it
        }
    }
    return fusb302::sink_request(c, true);
}
fc::Fail ChargerTestSession::request_fc(fc::Proto p, int mv, bool retarget, int ma) {
    const int ufcs_ma = p == fc::Proto::Ufcs ? ma : 0;   // a UFCS row's ma is its Request current (0 = the no-cable 2 A)
    return retarget ? fctrig::set_target(mv, true, ufcs_ma) : fctrig::start(p, mv, true, false, ma, ufcs_ma);
}
void ChargerTestSession::legacy_only(bool on) { fusb302::legacy_only(on); }
void ChargerTestSession::pd_rescan() { fusb302::rescan(); }
// One unit's ends (docs/CHARGER_TEST_FLOW.md): detach = Rd off, the 5 V default, VBUS dark; attach = Rd on as a fresh attach, the
// sink either PD-active (the next caps get the automatic PDO 1 request, the START path) or PD-silent (legacy units).
void ChargerTestSession::detach() {
    K2LOG("[TEST] unit detach: Rd off, waiting for VBUS to go dark\n");
    runev::emit(runev::Code::Detach, ctx_.now, 1);
    fusb302::unit_detach();
}
void ChargerTestSession::attach(ct::Mode m) {
    K2LOG("[TEST] unit attach: Rd on, %s\n", m == ct::Mode::Legacy ? "PD-silent" : "PD");
    runev::emit(runev::Code::Attach, ctx_.now, static_cast<uint8_t>(m));
    fusb302::unit_attach(m == ct::Mode::Legacy);
}
// A self-powered K2's boundary: the pinned Rd stays, CC stays closed, VBUS stays up (Soft_Reset for a Pd unit, PD-silent for a Legacy one).
void ChargerTestSession::hold(ct::Mode m) {
    K2LOG("[TEST] unit hold: Rd kept, %s\n", m == ct::Mode::Legacy ? "PD-silent" : "Soft_Reset");
    runev::emit(runev::Code::Hold, ctx_.now, static_cast<uint8_t>(m));
    fusb302::unit_hold(m == ct::Mode::Legacy);
}
// The runner asks the charger who it is once, at 5 V in the live contract (fusb302 probe, one try per question,
// over in under a second). The answers are copied into the card identity before anything can wipe the caps.
bool ChargerTestSession::probe_identity(bool on) {
    if (on) return fusb302::probe_start();
    fusb302::probe_abort();
    const auto st = fusb302::probe_status();
    const auto r = pd::probe_result(st, fusb302::vdm(), fusb302::ext(), fusb302::caps());
    if (card_id_.fp != 0) {
        card_id_.vid = r.vid; card_id_.pid = r.pid; card_id_.pdp_w = r.pdp_w;
        std::memcpy(mfr_, r.mfr, sizeof mfr_);
    }
    K2LOG("[PROBE] card id vid=%04X pid=%04X pdp=%uW mfr=\"%s\" ok=%u skipped=%u\n", unsigned(r.vid), unsigned(r.pid),
                 unsigned(r.pdp_w), r.mfr, unsigned(st.ok_mask), unsigned(st.skipped_mask));
    return false;
}
// EPR_Get_Source_Cap in SPR mode: the charger lists its EPR levels without an EPR entry (a cable that cannot carry EPR).
pd::Err ChargerTestSession::epr_info_ask() { return fusb302::epr_info_request(); }
void ChargerTestSession::release(bool pd) {
    if (pd) fusb302::sink_release();
    else fctrig::stop();
}
void ChargerTestSession::record(const ct::Step& s) {
    const auto idx = static_cast<size_t>(&s - runner_.steps().data());
    // The record is a flash write (a section file: an erase and some programs, with interrupts masked): never while a row is in
    // flight, and never inside this call either (the Runner is mid-decision, the PD link may be mid-exchange). It is only noted
    // here (the row's section is marked dirty, no flash); journal_pump() rewrites it, one file per tick, in the gap behind the row
    // once the link is quiet.
    if (journal_active_ && !journal_closed_ && idx < runner_.steps().size()) charger_journal::hw::note(runner_.steps(), idx);
    // A failure skips every row behind it in one pass (55 on a full run): those rows are one run event and one log line per
    // range of equal reason, not one each, which overflowed the 16-event ring and the 1 KB log ring (hardware 2026-11-07).
    if (s.verdict == ct::Verdict::Skip) {
        if (idx < runner_.steps().size() && runev::skip(ctx_.now, static_cast<uint16_t>(idx), static_cast<uint16_t>(s.reason)))
            K2LOG("[TEST] rows from %u skipped: %s\n", unsigned(idx), detail_name(s));
        return;
    }
    if (idx < runner_.steps().size()) {
        runev::emit(runev::Code::RowEnd, ctx_.now, ev8(idx), static_cast<uint16_t>(s.verdict), static_cast<uint16_t>(s.reason), s.measured_mv);
        runev::emit(runev::Code::RowEnd2, ctx_.now, ev8(idx), s.tries, s.mv, s.ma);
    }
    note_transition(s);
    K2LOG("[TEST] %s PDO%u target=%umV request=%umA measured=%umV %s %s\n",
        ct::name(s.algorithm), unsigned(s.pos), unsigned(s.mv), unsigned(s.ma), unsigned(s.measured_mv),
        ct::name(s.verdict), detail_name(s));
    // What held the loop while the row was in flight: the longest main-loop iteration (1 ms when healthy) and the longest
    // gap between two UI ticks (20 ms when healthy), in ms since the row began. A big number names a blocking path.
    K2LOG("[CT] gap row=%u loop=%ums ui=%ums\n", unsigned(idx), unsigned(gap::loop.peak()), unsigned(gap::ui.peak()));
}
// A 6 A hunt row the source refused: one tagged line with the whole evidence, a Black Box mark and a frozen capture (the ring wraps in
// seconds on a chatty source). Reads and logs only; the row's verdict is written by the Runner right after this returns.
void ChargerTestSession::hunt_evidence(const ct::Step&, const ct::HuntEvidence& e) {
    K2LOG("[HUNT] step=%u cause=%s answer=%u after=%ums last=%X flags=%X caps=%u pos=%u apdo=%X rdo=%X limit=%umA cable=%umA authed=%ums\n",
        unsigned(e.step), ct::name(static_cast<ct::HuntCause>(e.cause)), unsigned(e.answer), unsigned(e.answer_ms), unsigned(e.last_step),
        unsigned(e.flags), unsigned(e.caps), unsigned(e.pos), unsigned(e.apdo), unsigned(e.rdo), unsigned(e.apdo_ma), unsigned(e.cable_ma),
        unsigned(e.authed_ms));
    pdtrace::refusal(uint16_t(e.cause | (e.answer & 15) << 8 | (e.flags & ct::HfTimeout ? 0x1000 : 0)), e.apdo_ma);
}
// The PD rows were planned again from a list heard later and the rows behind them moved: the section files that now hold other
// rows (count or layout) are rewritten, and the header that names them. Only noted here (a hash pass, no flash); journal_pump()
// writes them, one file per tick. The Runner does not request above 5 V before they are on flash (journal_ready()).
void ChargerTestSession::plan_changed() {
    if (journal_active_ && !journal_closed_) charger_journal::hw::note_plan(runner_.steps());
}
// The phase marker names a row by its place in the section files: until a replan's files are written an above-5 V request waits in
// the Baseline (no contract is held there), the page pumping them. A journal that is not live holds nothing back.
bool ChargerTestSession::journal_ready() {
    return !journal_active_ || journal_closed_ || !charger_journal::hw::plan_pending();
}
void ChargerTestSession::journal_waited(uint32_t ms) { K2LOG("[CJRN] wait-for-replan ms=%u\n", unsigned(ms)); }
// Behind the row (the Runner out of Requesting/Holding): ONE file per tick, only while no PD message or VBUS change was seen
// for kJrQuietMs, so the main loop is never away from a live exchange (2026-11-08: a 2.2 s begin with a contract up cost the
// link). A source that never goes quiet does not keep the records out for ever: after kJrWaitMs of waiting they go in one per tick
// regardless. A run that has ended writes without waiting (nothing is being negotiated any more). Nothing ever drains: no call
// writes more than one file, so no tick is longer than the slowest section (about 250 ms with a compaction, 100 to 150 ms usual).
namespace {
constexpr uint32_t kJrQuietMs = 80;
constexpr uint32_t kJrWaitMs = 400;
constexpr uint32_t kArmWaitMs = 5000;
}
void ChargerTestSession::journal_pump(uint32_t now) {
    if (!journal_active_ || journal_closed_ || arm_) return;
    if (!charger_journal::hw::pending()) { jr_wait_ms_ = 0; return; }
    if (runner_.quiet()) return;
    const bool ended = runner_.state() == ct::State::Done || runner_.state() == ct::State::Stopped;
    if (!ended) {
        if (!jr_wait_ms_) jr_wait_ms_ = now | 1u;
        if (now - jr_wait_ms_ < kJrWaitMs && pdtrace::since_activity_ms() < kJrQuietMs) return;
    }
    charger_journal::hw::pump(runner_.run_id(), options_, runner_.evidence(), runner_.steps());
}
// The run's journal: rows and header in pieces (hw::pump), one per tick on a quiet link, the Runner held in its Baseline so that
// the first above-5 V request (and the phase marker ahead of it) comes after the header. Then the meta record, one more tick.
void ChargerTestSession::arm_step(uint32_t now) {
    namespace cj = charger_journal;
    if (!arm_) return;
    if (pdtrace::since_activity_ms() < kJrQuietMs && now - arm_t0_ < kArmWaitMs) return;
    if (arm_ == 1) {
        cj::hw::pump(runner_.run_id(), options_, runner_.evidence(), runner_.steps());
        if (!cj::hw::working()) arm_ = 2;
        return;
    }
    cj::hw::meta(arm_meta_);
    arm_ = 0;
    runner_.hold(false);
    K2LOG("[TEST] journal armed in %u ms\n", unsigned(now - arm_t0_));
}
// The run ended before its plan was on flash: the header goes in last, so no run of this one is there to resume, and there is
// nothing to close either.
void ChargerTestSession::arm_abort() {
    charger_journal::hw::cancel();
    arm_ = 0; journal_active_ = false;
    runner_.hold(false);
    K2LOG("[TEST] journal arm abandoned: the run ended before its plan was written\n");
}
// The run ends for good: Finish goes into the journal FIRST, then the Runner writes the skip rows. A reset (or a flash that gives
// up) between those rows leaves a closed run with a few rows still Pending, never a stopped run that the next boot resumes.
// A suspended run never comes here (it is only interrupted, and resumes).
void ChargerTestSession::closing(ct::Reason r) {
    if (!journal_active_ || journal_closed_) return;
    if (arm_ == 1) { arm_abort(); return; }
    if (arm_ == 2) { arm_ = 0; runner_.hold(false); }   // the header is in; the meta record of a run that ends here is not worth a write
    // Nothing is drained: the section files still to be written are moot for a run that is over (Finish closes it, a closed
    // run is never resumed, and no write here may take more than its own one file).
    charger_journal::hw::cancel();
    charger_journal::hw::finish(static_cast<uint8_t>(ct::State::Stopped), static_cast<uint8_t>(r));
    journal_closed_ = true;
}
// A failed step freezes the capture and asks for it to be stored with the run; a passed step just ends the
// recording (it is overwritten by the next arm). Skip rows (the followers of a failure) change nothing.
void ChargerTestSession::note_transition(const ct::Step& s) {
    if (s.verdict == ct::Verdict::Pass) { transcap::disarm_if_armed(); return; }
    if (s.verdict != ct::Verdict::Fail) return;
    const transcap::Reason r = ct::capture_reason(s.reason);
    if (r == transcap::Reason::None) return;
    const auto idx = static_cast<size_t>(&s - runner_.steps().data());
    if (idx >= runner_.steps().size()) return;
    // The units no longer end on the PD rows: a Legacy row that fails after a PD failure finds that failure's frozen capture still
    // waiting to be written and must not re-key it to itself.
    if (capture_kept_) return;
    transcap::trigger(r);   // refused when an earlier event (the detach of the cut) already froze the capture...
    if (transcap::triggered()) { transcap::keep(runner_.run_id(), static_cast<uint16_t>(idx)); capture_kept_ = true; }   // ...which is still this step's
}
void ChargerTestSession::phase(ct::Phase p, size_t index) {
    runev::emit(runev::Code::Phase, ctx_.now, static_cast<uint8_t>(p), ev8(index));
    if (!journal_active_) return;
    // Written BEFORE the request / handover: the marker is what a power loss inside the exchange is told by, so it is the one
    // journal write a row makes ahead of its own protocol exchange: one small record, never a drain. The marker names the row by
    // its place in the section files, which the Runner has already waited for (journal_ready()) before an above-5 V request.
    charger_journal::hw::phase(p, runner_.steps(), index);
}
// A power loss inside this step was seen `count` times (the Runner decides what that means; this only persists it).
void ChargerTestSession::attempt(size_t index, uint8_t count) {
    if (journal_active_) charger_journal::hw::attempt(index, count);
}
// `cancel` = BACK held at power-up. A warm reset on a charger-powered K2 left the Rd pin in the chip (inherited); nothing that
// resumes needs it any more once the run is dropped or there is none, so it is released then (a run keeps it until its end).
bool ChargerTestSession::boot_restore(bool cancel) {
    const bool resume = boot_load(cancel);
    boot_arm_cable();   // after the Rd decision above: a released inherited pin is no longer a self-powered boot
    return resume;
}
bool ChargerTestSession::boot_load(bool cancel) {
    namespace cj = charger_journal;
    cj::hw::mount();
    {
        // The replay (~2.6 KB) is transient: it is leased from the scratch arena only while its fields move into the Runner and
        // the page (steps, options, cable identity); a pending resume keeps those copies, never the Replay. The lease covers
        // pure copies and a log line, nothing that leases again; the journal write below runs after it is released.
        const scratch::Lease lease("cjrn.boot");
        auto& r = scratch::make<cj::Replay>();
        if (!cj::hw::load(r)) {
            if (fusb302::rd_inherited()) fusb302::self_power_release();
            return false;
        }
        options_ = r.options;
        journal_active_ = true; journal_closed_ = false;   // the interrupted-step marks are journaled too
        runner_.restore(r.options, r.evidence, r.run_id, std::span<const ct::Step>(r.steps.data(), r.count),
                        r.phase, r.phase_index, r.resumed, r.attempt_index, r.attempts);
        resume_pending_ = runner_.resumable() && !cancel;
        auto_resume_ = resume_pending_;   // every interrupted run resumes by itself once VBUS has been steady (BACK at boot cancels)
        self_run_ = r.has_meta && r.meta.unattended;   // unattended is what self_powered() was when the run started
        run_guard_ = r.has_meta ? r.meta.guard : uint8_t(0);   // the guard's reading at START stays with the run (its card)
        resume_cable_ = r.has_meta ? ctcable::Choice{static_cast<ctcable::Kind>(r.meta.cable_kind), r.meta.cable_seq} : ctcable::Choice{};
        gate_.reset();
        resume_rd_ = resume_hold_ = resume_refused_ = false; resume_judged_ = 0; guard_.clear();
        K2LOG("[TEST] journal: run %u unfinished, %u steps, phase %u, lost %u, resumed %u -> %s\n", unsigned(r.run_id), unsigned(r.count),
                     unsigned(r.phase), unsigned(r.attempts), unsigned(r.resumed),
                     cancel ? "cancelled (BACK held)" : !resume_pending_ ? "nothing to resume" : auto_resume_ ? "auto-resume" : "resume?");
    }
    if (!resume_pending_) {
        // A run that cannot go on is closed in the journal, so the next boot does not find it again.
        cj::hw::finish(static_cast<uint8_t>(runner_.state()), static_cast<uint8_t>(runner_.reason()));
        journal_active_ = false;
        if (fusb302::rd_inherited()) fusb302::self_power_release();
    }
    return resume_pending_;
}
// The journaled Ra mode / spoof identity goes back on before the resumed run asks the charger anything. False = refused.
bool ChargerTestSession::reapply_cable() {
    if (resume_cable_.kind == ctcable::Kind::Off) return true;
    if (!ctcable::valid(resume_cable_)) return false;
    if (run_cable_ == resume_cable_ && dialog_spoof_ && dialog_ra_ && emk_spoof::active()) return true;   // boot_arm_cable did it
    return arm_cable(resume_cable_);
}
// A source asks the cable (SOP' Discover Identity) when it attaches, and a key-mode boot attaches at plug-in: an identity armed
// at START comes seconds too late. So a self-powered boot (key mode or inherited pin) that wants an identity (the journaled one
// of a run that resumes, else the shared selection) arms it and puts the Ra on right here, before the 5 V contract and before
// anything else asks. Only self-powered: a HID-powered boot keeps START's detach / re-attach arming. The Ra guard cannot judge
// a pinned K2 (its own Rd is not "passive"), so the line the pin leaves free decides: an open Rp on CC1 is a receptacle charger,
// anything lower a cable in between (k2/pd/boot_ra.hpp). A refusal is logged with its reason and arms nothing.
void ChargerTestSession::boot_arm_cable() {
    bootra::In in;
    in.self_powered = fusb302::self_powered();
    const ctcable::Choice c = resume_pending_ ? resume_cable_ : ctx_.cables.selected();
    in.identity = c.kind != ctcable::Kind::Off && ctcable::valid(c);
    in.journaled = resume_pending_ && in.identity;
    in.inline_sink = fusb302::inline_mode();
    in.pinned_rd = fusb302::pin_rd();
    if (const auto a = adc::read()) { in.cc1_mv = a->cc1_mv; in.cc2_mv = a->cc2_mv; }
    bootra::Verdict v = bootra::decide(in);
    if (v == bootra::Verdict::Arm && !arm_cable(c)) v = bootra::Verdict::Failed;   // the reason is on the footer
    bootra::note(v);
    if (in.self_powered && in.identity)
        K2LOG("[BOOTRA] %s cc1=%d pin=%u journaled=%u\n", bootra::name(v), int(in.cc1_mv), unsigned(in.pinned_rd), unsigned(in.journaled));
}
void ChargerTestSession::resume_run(uint32_t now) {
    if (!gate_.ready(now)) { message("Waiting for stable 5V"); return; }
    if (!reapply_cable()) { auto_resume_ = false; return; }   // the reason is on the footer; the OK press decides from here
    const auto r = runner_.resume(input(now), true);    K2LOG("[TEST] resume -> %s\n", ct::name(r));
    if (r != ct::Reason::None) { auto_resume_ = false; message(reason_text(r)); return; }
    resume_pending_ = false; auto_resume_ = false;
    resume_rd_ = resume_hold_ = false;   // the Rd (if we presented it) belongs to the run now; release_run() ends it with the run
    wait_ = pd::VbusWait();
    capture_identity();
    charger_journal::hw::resumed(runner_.resumed());
    g_ev_row = kNoRow;
    runev::emit(runev::Code::Resume, now, ev8(runner_.index()));
    set_stage(Stage::Running); follow_ = true;
}
// The Rd of a K2 that nothing but the host powers (see the header): the identity first (only for a charger directly on the
// plug, the Ra guard reads the lines while the K2 is passive), then the e-marker scan and the Rd exactly as START does it, then
// the same VBUS / caps wait. The resume itself stays the gate's business (poll()).
void ChargerTestSession::resume_rd_step(uint32_t now) {
    using State = pd::VbusWait::State;
    if (resume_rd_) {
        if (!wait_paused_ && wait_.state() != State::Waiting) return;   // settled: the gate decides from here
        if (wait_tick(now) != State::Waiting) K2LOG("[TEST] resume: charger wait over (%s), VBUS %d mV\n",
                                                    wait_.state() == State::Ready ? "up" : "timeout", int(ctx_.vbus_fast_mv));
        return;
    }
    // Self-supplied (pinned Rd, or fed through the A port): VBUS comes without our help. Not decided: the supply is unknown.
    // A sink inline gets no Rd, as for START; VBUS already up needs none.
    if (resume_hold_ || !power_source::decided() || self_supplied() || !ctx_.pm_online || ctx_.inline_mode ||
        fusb302::rd_for_run_on() || !pd::start_needs_rd(ctx_.inline_mode, ctx_.vbus_fast_mv)) return;
    if (resume_cable_.kind != ctcable::Kind::Off) {
        if (resume_judged_ != 0 && static_cast<int32_t>(now - resume_judged_) < static_cast<int32_t>(raguard::kSampleMs)) return;
        resume_judged_ = now;
        const auto t = guard_topology(now, false);
        if (t != topology_) K2LOG("[TEST] resume: lines say %s, identity %s\n", raguard::name(t), raguard::spoof_allowed(t) ? "armed" : "waits");
        topology_ = t;
        resume_refused_ = !raguard::spoof_allowed(t);
        if (resume_refused_) return;   // behind a cable (or nothing plugged in): the identity cannot answer, wait for a direct plug
        if (!ctcable::valid(resume_cable_) || !arm_cable(resume_cable_)) { resume_hold_ = true; auto_resume_ = false; return; }   // reason on the footer
    }
    if (!present_rd(now)) { release_run(); resume_hold_ = true; auto_resume_ = false; message("Rd refused: sink inline"); return; }
    resume_rd_ = true;
    K2LOG("[TEST] resume: Rd presented, waiting for the charger\n");
}
// BACK: nothing is presented again, whatever the resume had put on the line goes back.
void ChargerTestSession::resume_rd_drop() {
    wait_ = pd::VbusWait(); wait_paused_ = wait_retry_ = false;
    resume_rd_ = resume_hold_ = resume_refused_ = false;
}
resumetext::In ChargerTestSession::resume_words(uint32_t now) const {
    resumetext::In in;
    in.self_now = self_supplied();
    in.gate_ready = gate_.ready(now);
    in.rd_on = resume_rd_;
    in.wait_over = !wait_paused_ && wait_.state() != pd::VbusWait::State::Waiting;
    in.behind_cable = resume_refused_ && !resume_rd_;
    in.held = resume_hold_;
    return in;
}
void ChargerTestSession::abandon_run() {
    K2LOG("[TEST] resume abandoned\n");
    // The run ends here, so what it armed ends with it: the resume's own Rd and identity, and the START's (or the self-powered
    // boot's) identity of a run that was suspended and never resumed. Hardware 2026-11-07: an identity a run armed stayed armed.
    release_run();
    resume_rd_drop();
    charger_journal::hw::finish(static_cast<uint8_t>(runner_.state()), static_cast<uint8_t>(runner_.reason()));
    resume_pending_ = false; journal_active_ = false; auto_resume_ = false;
    arm_ = 0;
    if (fusb302::rd_inherited()) fusb302::self_power_release();
    set_stage(Stage::Results);
}
void ChargerTestSession::capture_identity() {
    card_id_ = {}; card_no_ = 0; delta_n_ = 0; mfr_[0] = 0; card_name_[0] = 0;
    const auto& caps = fusb302::caps();
    if (!caps.valid || caps.num_pdos == 0) return;   // no PD source caps: nothing to recognise the charger by
    std::array<uint32_t, pd::kMaxPdos> raw{};
    const size_t n = std::min<size_t>(caps.num_pdos, raw.size());
    for (size_t i = 0; i < n; ++i) raw[i] = caps.pdos[i].raw;
    card_id_.fp = report_card::fingerprint({raw.data(), n}, caps.spec_rev);
    // Without the probe (resumed run, no answer yet): a Discover Identity the K2 already saw, and the power from the PDOs.
    const auto r = pd::probe_result({}, fusb302::vdm(), fusb302::ext(), caps);
    card_id_.vid = r.vid; card_id_.pid = r.pid; card_id_.pdp_w = r.pdp_w;
}
void ChargerTestSession::save_card() {
    card_no_ = 0; delta_n_ = 0;
    if (runner_.state() != ct::State::Done) return;
    if (card_id_.fp == 0) card_id_.fp = report_card::legacy_fingerprint(runner_.steps());   // no PD caps: identify by the legacy answers
    // V5: the cable the run went through — the physically read e-marker, else the physical limits, and the
    // spoofed library identity when one was armed (its limits are what the run actually used).
    report_card::Cable cable;
    const auto& em = fusb302::emarker();
    const auto& pc = fusb302::physical_cable();
    if (em.valid) {
        cable.flags |= report_card::kCableObserved;
        cable.vid = em.vid; cable.pid = em.pid;
        cable.max_mv = uint16_t(em.max_voltage_v * 1000u); cable.max_ma = uint16_t(em.max_current_a * 1000u);
        cable.kind = report_card::kCableKindReal;
    } else {
        cable.max_mv = pc.max_mv; cable.max_ma = pc.max_ma;
        if (pc.observed) cable.flags |= report_card::kCableObserved;
    }
    if (run_cable_.kind != ctcable::Kind::Off) {   // the dialog's identity answered the charger: its limits are what the run used
        cable = {};
        cable.seq = run_cable_.seq; cable.vid = run_facts_.vid; cable.pid = run_facts_.pid;
        cable.max_mv = run_facts_.max_mv; cable.max_ma = run_facts_.max_ma;
        cable.kind = run_cable_.kind == ctcable::Kind::Builtin ? uint8_t(run_cable_.seq) : uint8_t(0);   // the built-in's id
        cable.flags = report_card::kCableSpoofed;
    } else if (emk_spoof::active()) cable.flags |= report_card::kCableSpoofed;
    if (self_run_) cable.flags |= report_card::kCableSelfPowered;
    if (run_guard_ & 0x7F) {   // the Ra guard read the plug when the identity was started; bit 7 = started anyway
        cable.flags |= uint8_t(((run_guard_ & 0x7F) << report_card::kCableGuardShift) & report_card::kCableGuardMask);
        if (run_guard_ & 0x80) cable.flags |= report_card::kCableGuardBypassed;
    }
    // A fresh card per run (the fields build_into does not set: name, card_no, runs, ...); the NSDMI values
    // (format, times) come back through the default constructor.
    card_ = report_card::Card{};
    report_card::build_into(card_, runner_.steps(), runner_.evidence(), runner_.handover(), card_id_,
                            report_card::hw::context(), runner_.state(), runner_.reason(), cable, runner_.resumed());
    report_card::set_auto_name(card_, mfr_);   // "<Brand> <PDP>W"; hw::put keeps a name the user gave
    save_card_store();
}
// save_card's tail, on its own frame: it holds the charger's previous Card (292 B) that save_card used to keep
// beside the new one — with both on save_card's stack the frame was stack-gated (tools/stack_depth.py). The order
// is unchanged: prev is read before the write (after put the file holds the new card), the delta is taken after it.
[[gnu::noinline]] void ChargerTestSession::save_card_store() {
    report_card::Card prev;
    const bool had = report_card::hw::find(card_.id.fp, prev);
    const auto err = report_card::hw::put(card_);
    if (err != report_card::Err::Ok) { K2LOG("[CARD] save failed %d\n", int(err)); return; }
    card_no_ = card_.card_no;
    std::memcpy(card_name_, card_.name, std::min(sizeof card_name_ - 1, sizeof card_.name)); card_name_[sizeof card_name_ - 1] = 0;
    if (had) { delta_n_ = report_card::delta(prev, card_, delta_); delta_runs_ = prev.runs; }
}
// A charger alone on the bottom port never turns VBUS on without Rd and plain boot presents none: the START
// presents it (no sink inline) and enters Waiting, which poll() carries until VBUS and the caps are up (or the wait
// gave up), then the usual preflight runs on the fresh reading. True = Waiting entered; false = nothing to wait for.
bool ChargerTestSession::start_begin() {
    // A dialog identity (Ra mode on) takes the Rd path whatever VBUS shows: the Rd is what the run owns the Ra mode through.
    if (!ctx_.pm_online || fusb302::rd_for_run_on() || !(dialog_ra_ || pd::start_needs_rd(ctx_.inline_mode, ctx_.vbus_fast_mv))) {
        K2LOG("[TEST] START without Rd: pm=%d rd_on=%d inline=%d vbus=%d mV\n", int(ctx_.pm_online), int(fusb302::rd_for_run_on()),
              int(ctx_.inline_mode), int(ctx_.vbus_fast_mv));
        return false;
    }
    if (!present_rd(ctx_.now)) return false;
    // The wait is a stage of its own, carried by poll(): the main loop keeps running (the PD engine's service() every
    // millisecond, the UI tick every 20 ms) and BACK cancels. A blocking loop here froze the UI for up to 21 s.
    set_stage(Stage::Waiting);
    return true;
}
// The Rd presentation shared by START and a resume on a host-powered K2. False = refused (a sink is inline), nothing changed.
bool ChargerTestSession::present_rd(uint32_t now) {
    // Nothing supplies VBUS yet, so nobody has asked the cable: read its e-marker now, VCONN from us, before Rd goes on
    // (the C135 cable is 20 V 5 A, VCONN required; "assumed 3 A" cost the run its 5 A rows, hardware 2026-11-03).
    // Not while a source's Rp is on the wire: it hears our SOP' and the cable's replies (C135: Hard Reset right after).
    const bool rp = fusb302::rp_on_line();
    if (dialog_ra_) K2LOG("[TEST] START: no e-marker scan, the spoofed identity answers (Ra mode)\n");   // VCONN from us would fight the Ra
    else if (pd::start_scan_wanted(ctx_.vbus_fast_mv, rp)) {
        const bool found = fusb302::read_emarker(false, pd::EmarkerMode::Vconn);
        const auto& cable = fusb302::physical_cable();
        K2LOG("[TEST] START e-marker scan (VCONN, VBUS %d mV): %s, cable %s %u mV %u mA\n", int(ctx_.vbus_fast_mv),
              found ? "found" : "no answer", cable.observed ? "read" : "not read", unsigned(cable.max_mv), unsigned(cable.max_ma));
    } else K2LOG("[TEST] START: no e-marker scan (VBUS %d mV, Rp on the wire %d), cable not read\n", int(ctx_.vbus_fast_mv), int(rp));
    if (!fusb302::rd_for_run(true)) { K2LOG("[TEST] START: Rd refused (sink inline)\n"); return false; }
    wait_ = pd::VbusWait();
    wait_.begin(now);
    wait_t0_ = now; wait_slow_ = now - pd::kStartWaitSlowMs;   // the first scan is due at once
    wait_retry_ = wait_paused_ = false;
    return true;
}
// The start's own Request (PDO 1) goes out from fusb302's frame handler on the first caps: the wait holds until the sink
// engine has settled it, so the preview / a refusal's release never touch a sink mid-exchange.
bool ChargerTestSession::request_pending() const {
    const auto s = fusb302::sink_status().state;
    return s == pd::SinkState::Requesting || s == pd::SinkState::Transition || s == pd::SinkState::Aborting;
}
// One tick of Waiting. The VBUS reading is the tick's own (ctx_.vbus_fast_mv, the latest sample); the PD engine's
// service() keeps its own pace in the main loop (the source's SenderResponseTimer is ~30 ms: Baseus 140W sent a Hard Reset
// when the old blocking loop took 56 ms a pass), so only the attach scan is ours, every kStartWaitSlowMs.
void ChargerTestSession::wait_step(uint32_t now) {
    if (wait_tick(now) != pd::VbusWait::State::Waiting) wait_done(now);
}
// Waiting while the wait goes on (also during the retry's Rd-off); anything else = the wait is over (Ready or TimedOut).
pd::VbusWait::State ChargerTestSession::wait_tick(uint32_t now) {
    using State = pd::VbusWait::State;
    if (wait_paused_) {   // the 1 s Rd-off of the retry: the source sees a clean detach
        if (static_cast<int32_t>(now - wait_pause_until_) < 0) return State::Waiting;
        wait_paused_ = false;
        if (!fusb302::rd_for_run(true)) return wait_.state();   // refused: the wait ends with the state it had
        wait_ = pd::VbusWait();
        wait_.begin(now);
        wait_slow_ = now - pd::kStartWaitSlowMs;
    }
    if (pd::start_wait_slow_due(now, wait_slow_)) { fusb302::attach_scan(); wait_slow_ = now; }   // notice the charger's Rp
    const auto& caps = fusb302::caps();
    const auto state = wait_.update(now, ctx_.vbus_fast_mv, caps.active_cc != 0, caps.valid, request_pending());
    if (state == State::Waiting) return state;
    // A charger that hard-resets right after the Rd and stays silent (C135 after the e-marker scan, hardware 2026-11-03)
    // gets one more attach: Rd off for 1 s so it sees a clean detach, then Rd on again.
    if (state == State::TimedOut && !wait_retry_) {
        K2LOG("[TEST] START: no VBUS after Rd, re-presenting Rd once\n");
        fusb302::rd_for_run_pause();   // not rd_for_run(false): that forgets the cable scanned in start_begin()
        wait_retry_ = wait_paused_ = true;
        wait_pause_until_ = now + 1000;
        return State::Waiting;
    }
    return state;
}
void ChargerTestSession::wait_done(uint32_t now) {
    const bool up = wait_.state() == pd::VbusWait::State::Ready;
    // The averaged reading lags a VBUS step by ~2.5 s (the C135 switches on right after the Rd): the second START press
    // and the run's preflight would still read the old ~0.5 V and refuse (hardware 2026-11-03, run 5).
    power::restart_average();
    {   // the black box has no K2LOG: when and why the wait ended
        const uint32_t waited = std::min<uint32_t>(now - wait_t0_, 0x7FFF);
        pdtrace::mark(pdtrace::kMarkStartWait, uint16_t(std::clamp<int32_t>(ctx_.vbus_fast_mv, 0, 0xFFFF)),
                      uint16_t(waited | (up ? 0x8000u : 0u)));
    }
    K2LOG("[TEST] Rd for the start: VBUS %d mV (%s)\n", int(ctx_.vbus_fast_mv), up ? "up" : "timeout");
    launch();   // the wait was the last stage: the run starts by itself (or the preflight refuses, back on Review)
}
// BACK, a second OK or leaving the page during the wait: nothing started, the Rd (and the dialog's identity) goes back.
void ChargerTestSession::wait_abort() {
    K2LOG("[TEST] START cancelled in the wait (%u ms)\n", unsigned(ctx_.now - wait_t0_));
    wait_paused_ = wait_retry_ = false;
    wait_ = pd::VbusWait();
    release_run();
    set_stage(Stage::Review, kStartRow);
}
void ChargerTestSession::poll(uint32_t now) {
    if (resume_pending_) {
        gate_.update(now, input(now).vbus_mv);
        // An interrupted run goes on by itself once VBUS has been steady (no OK press in any power mode).
        // (Not before the supply is known: a bus-powered K2 resumes as a self-powered one.)
        // A host-powered K2 has no Rd on the line, so it presents one (resume_rd_step) and resumes once that wait is over too.
        if (auto_resume_) resume_rd_step(now);
        const bool wait_over = !resume_rd_ || (!wait_paused_ && wait_.state() != pd::VbusWait::State::Waiting);
        if (auto_resume_ && ct::resume_wanted(false, wait_over && gate_.ready(now) && power_source::decided())) resume_run(now);
        return;
    }
    if (stage_ == Stage::Waiting) wait_step(now);
    // The confirm window ran out without the second START press: nothing was touched (no Rd, no identity), so only the window goes.
    if (confirming_ && static_cast<int32_t>(now - confirm_until_) >= 0) invalidate_confirmation();
    if (guard_stage(stage_) && static_cast<int32_t>(now - guard_t_) >= static_cast<int32_t>(raguard::kSampleMs)) guard_sample(now);
    if (runner_.attaching()) fusb302::attach_scan();   // Rd is back on: notice the charger's Rp (START's blocking loop does the same)
    const auto before = runner_.state();
    // The journal's own plan write (still arming) goes before the Runner: it is held in Baseline until the header is on flash.
    if (arm_) arm_step(now);
    runner_.poll(input(now), fusb302::caps());
    // A live run: the Black Box writes nothing but the rare-event captures (hard reset, detach, refusal) until the run is over;
    // every save stalls the main loop 100-250 ms. Renewed each tick, it also covers the arm.
    if (journal_active_ && !journal_closed_) bbpersist::hold_run();
    journal_pump(now);   // out of the row: the held-back records go in now, one file per tick on a quiet link
    // The row that just began (the Runner has no hook for it): its first pass as Running names it for the live stream.
    if (const auto idx = runner_.index(); idx < runner_.steps().size() && idx != g_ev_row && runner_.steps()[idx].verdict == ct::Verdict::Running) {
        const auto& s = runner_.steps()[idx];
        g_ev_row = static_cast<uint16_t>(idx);
        gap::loop.begin_window(); gap::ui.begin_window();   // the row's own longest gaps (read by record())
        runev::emit(runev::Code::RowStart, now, ev8(idx), static_cast<uint16_t>(s.group), static_cast<uint16_t>(s.algorithm), s.pos);
    }
    if (runner_.suspended()) {
        runev::emit(runev::Code::Suspend, now, ev8(runner_.index()));
        // A self-powered K2 lost VBUS but still runs (a reset normally ends it before this line): the run is not over. The journal
        // stays open with its phase marker, the rows stay Pending, and the usual unattended resume waits for VBUS to be steady.
        // (Hardware 2026-11-06: finalizing here is what turned a lost brick into "stopped (source lost)" with every row skipped.)
        K2LOG("[TEST] suspended: VBUS lost on a self-powered K2, resume when steady\n");
        resume_pending_ = true; auto_resume_ = true; gate_.reset();
        resume_rd_ = resume_hold_ = resume_refused_ = false;
        set_stage(Stage::Resume);
        return;
    }
    if (journal_active_ && (runner_.state() == ct::State::Done || runner_.state() == ct::State::Stopped)) {
        if (arm_ == 1) arm_abort();
        else {
            if (!journal_closed_) {
                arm_ = 0; runner_.hold(false);
                charger_journal::hw::cancel();
                charger_journal::hw::finish(static_cast<uint8_t>(runner_.state()), static_cast<uint8_t>(runner_.reason()));
                journal_closed_ = true;
            }
            // Finish closed the run: the skip rows the Runner wrote after it are not journaled (a closed run is never resumed).
            journal_active_ = false;
        }
    }
    if (runner_.state() != before) {
        K2LOG("[TEST] %s %s\n", ct::name(runner_.state()), ct::name(runner_.reason()));
        if (runner_.state() == ct::State::Done) save_card();
        if (runner_.state() == ct::State::Done || runner_.state() == ct::State::Stopped) {
            emit_end(now);
            release_run();   // the run is over: passive again (Ra mode and the dialog's spoof end with it)
            transcap::disarm_if_armed();
            set_stage(Stage::Results);
        }
    }
}
void ChargerTestSession::invalidate_confirmation() {
    if (confirming_ && view_) view_->on_confirmation_dropped();   // the footer and the START cell show the confirmation
    confirming_ = false; confirm_until_ = 0; confirm_proposal_ = {};
}
void ChargerTestSession::cancel() {
    if (stage_ == Stage::Waiting) { wait_abort(); return; }   // release_run() and the stage are its business
    if (resume_pending_ && resume_rd_) return;   // a page change keeps the resume's Rd: the resume goes on in the background, only BACK on its page ends it
    const auto before = runner_.state();
    runner_.cancel(input(ctx_.now)); invalidate_confirmation();
    if (runner_.state() != before && (runner_.state() == ct::State::Done || runner_.state() == ct::State::Stopped)) emit_end(ctx_.now);
    release_run();
    transcap::disarm_if_armed();
}
// The run is over (Done or Stopped): how it ended and how many rows got a verdict, for the live stream.
void ChargerTestSession::emit_end(uint32_t now) {
    uint16_t finished = 0;
    for (const auto& s : runner_.steps()) if (s.verdict != ct::Verdict::Pending && s.verdict != ct::Verdict::Running) finished++;
    runev::emit(runev::Code::End, now, static_cast<uint8_t>(runner_.state()), finished, static_cast<uint16_t>(runner_.reason()));
}
// Passive again. The dialog's Ra mode and spoof end with the Rd; the identity of the run stays for the card header.
// `rd_for_run(false)` already drops the Ra mode of a run that owns it; the explicit off covers a START that never got
// as far as the Rd, and the forced scan line goes with it.
void ChargerTestSession::release_run() {
    fusb302::rd_for_run(false);
    drop_identity();
}
// The Ra mode, the forced scan line and the spoof off. The identity the run was started with (run_cable_) is kept: the card
// header names it and identity(true) arms it again.
void ChargerTestSession::drop_identity() {
    if (dialog_ra_) { fusb302::ra_set(false); fusb302::set_force_line(0); }
    if (dialog_spoof_ && !spoof_user_) ctx_.cables.disarm();   // what a run armed goes with it; the user's own Tools spoof stays
    dialog_ra_ = dialog_spoof_ = spoof_user_ = false;
}
// The identity per unit mode (charger_test::Env::identity): on for the Pd-mode units, off for the Legacy ones. Baseus 70W
// (2026-11-07): under the Ra mode every D+/D- handshake got no answer; with plain Rd 27 of 54 rows passed. Called by the runner
// while detached (Rd off), so the Ra and the spoof are in place before the Rd of the next attach. No identity selected = no-op.
// Re-arming fails only when a tool or the 5 V handover is busy (footer message): the Pd unit then runs without the identity.
void ChargerTestSession::identity(bool on) {
    if (run_cable_.kind == ctcable::Kind::Off) return;
    if (!on) { drop_identity(); runev::emit(runev::Code::Identity, ctx_.now, 0); return; }
    if (dialog_spoof_ && dialog_ra_ && emk_spoof::active()) return;   // still armed (the START's, or a Pd unit after a Pd unit)
    drop_identity();
    if (arm_cable(run_cable_, true)) { fusb302::ra_run_own(); runev::emit(runev::Code::Identity, ctx_.now, 1); }
    else K2LOG("[TEST] cable identity: could not be re-armed for the Pd unit\n");
}
// The Ra guard (include/k2/pd/ra_guard.hpp): the lines are read while the dialog is open, with the K2 passive, so the
// Ra mode is never turned on across a cable. A sample every raguard::kSampleMs; the verdict follows the peak of the last 8.
void ChargerTestSession::guard_sample(uint32_t now) {
    guard_t_ = now;
    guard_.add(ctx_.analog ? ctx_.analog->cc1_mv : -1, ctx_.analog ? ctx_.analog->cc2_mv : -1);
}
raguard::Topology ChargerTestSession::guard_topology(uint32_t now, bool log) {
    guard_sample(now);   // the freshest reading counts too: a dialog answered within one sample period still has two
    raguard::Reading r;
    r.cc1_mv = guard_.cc1(); r.cc2_mv = guard_.cc2();
    r.vbus_mv = ctx_.pm_online ? ctx_.vbus_fast_mv : -1;
    // Passive = nothing of ours on the lines: no run Rd, no Ra mode, no boot Rd, no sink inline. Anything else
    // changes what the lines read and the guard cannot judge.
    r.passive = !fusb302::rd_for_run_on() && !fusb302::ra_active() && !fusb302::self_powered() && !ctx_.inline_mode;
    const auto t = raguard::classify(r);
    if (log) K2LOG("[GUARD] cc1 %d cc2 %d vbus %d mV passive %d -> %s\n", int(r.cc1_mv), int(r.cc2_mv), int(r.vbus_mv), int(r.passive), raguard::name(t));
    return t;
}
int ChargerTestPage::last_row() const { return std::max(0, row_count() - 1); }
bool ChargerTestSession::confirm_live() const {
    return confirming_ && static_cast<int32_t>(ctx_.now - confirm_until_) < 0 && proposal(input(ctx_.now)) == confirm_proposal_;
}
// The remote / test seam: select an identity in the shared list. A clone that is not in the library is refused.
bool ChargerTestSession::set_cable_choice(ctcable::Choice c) {
    if (!ctx_.cables.select(c)) return false;
    invalidate_confirmation();
    if (stage_ == Stage::Cable) {
        selection_ = ctx_.cables.index_of(ctx_.cables.selected());
        if (view_) view_->on_cable_changed();
    }
    return true;
}
// The saved clones' names of the rows the Cable list is about to show (same window arithmetic as update()). A clone that
// cannot be read keeps its slot empty and the row says "Clone #n".
void ChargerTestSession::fill_cable_names() {
    if (stage_ != Stage::Cable) return;
    const int r = std::max(ctx_.rows.visible(), kit::kWindowRows);
    const int first = CableIdentityList::window_first(ctx_.rows.state().offset, selection_, r, ctx_.cables.count());
    ctx_.cables.fill_names(first, r);
}
ctcable::Info ChargerTestSession::cable_info() const {
    ctcable::Info i;
    i.choice = ctx_.cables.selected(); i.run = run_cable_;
    std::memcpy(i.run_name.data(), run_cable_name_, std::min(i.run_name.size(), sizeof run_cable_name_));
    return i;
}
// The dialog's OK: the identity answers the charger's SOP' Discover Identity (spoof) and the Ra mode makes the charger
// ask (PC13 high = an Ra on CC1, Rd on CC2 alone, from before the Rd until the run ends). START takes the spoof over:
// whatever is armed now (a Tools -> Spoof arming, a boot arming, the remote) is disarmed unless it is the pick, which
// arms again below; Off arms nothing, so a START with Off leaves nothing armed. False = refused with the reason on
// the footer, nothing left armed by this call.
bool ChargerTestSession::apply_cable(ctcable::Choice c) {
    const bool applied = c == run_cable_ &&
        (c.kind == ctcable::Kind::Off ? !dialog_spoof_ && !emk_spoof::active() : dialog_spoof_ && dialog_ra_ && emk_spoof::active());
    if (applied) return true;   // the confirm window was dropped and START is pressed again: same identity, nothing to redo
    release_run();
    if (emk_spoof::active() && !(ctx_.cables.armed() && ctx_.cables.armed_choice() == c)) ctx_.cables.disarm();
    return arm_cable(c);
}
// The arming half of apply_cable (also the resume path, which must not drop the Rd the run already owns).
// `in_run`: the run itself re-arms the identity for a Pd unit. Its own run is "busy" to spoof_busy() and must not refuse it, and a
// refusal leaves the run's identity (card header, facts) as it was.
__attribute__((noinline)) bool ChargerTestSession::arm_cable(ctcable::Choice c, bool in_run) {
    if (!in_run) { run_cable_ = {}; run_facts_ = {}; run_cable_name_[0] = 0; }
    if (c.kind == ctcable::Kind::Off) return true;
    c = ctcable::canonical(c);   // a journal written before id 3 was retired resumes as id 4 (the card kind records the canonical id)
    if (!in_run) {
        if (const char* busy = ctx_.ui->busy_reason()) { message(busy); return false; }
    }
    ctcable::Facts facts{};
    char name[emlib::kNameMax + 1] = {};
    // Callers drop the run's own arming first (release_run / identity), so an identity still armed for `c` is the user's
    // (Tools -> Spoof): it is presented, not owned, and the run's end must not disarm it.
    const bool already = ctx_.cables.armed_choice() == c;
    const auto r = ctx_.cables.arm(c, facts, name);   // the one arming path (Tools -> Spoof uses it too)
    if (r != CableIdentityList::ArmErr::Ok) {
        message(r == CableIdentityList::ArmErr::Gone ? "Saved clone gone"
                : r == CableIdentityList::ArmErr::Inline ? "Live sink: unplug load" : "Bad saved record");
        return false;
    }
    if (!fusb302::ra_set(true)) {
        if (!already) ctx_.cables.disarm();
        message(emkra::refuse_footer(fusb302::ra_refusal()));   // the reason: a contract, a tool, a pin, a live sink
        return false;
    }
    dialog_spoof_ = dialog_ra_ = true;
    spoof_user_ = already;
    run_cable_ = c; run_facts_ = facts;
    std::strncpy(run_cable_name_, name, sizeof run_cable_name_ - 1);
    K2LOG("[TEST] cable identity: %s (Ra mode)\n", name);
    return true;
}
// START is two OK presses on Review, in this order: the confirmation (the "no load" warning, nothing touched), then the
// guard, the identity, and last of all the Rd with the wait for the charger; the run starts by itself when VBUS is ready.
// The first press: open the confirm window. No Rd, no identity, no guard, no e-marker scan. A source that is not up
// yet is what the Rd is for, so a "no source" / "not safe 5 V" preview is not refused here; the preflight judges it again
// when the wait ends. Every other refusal shows on the footer at once, with no window.
void ChargerTestSession::open_confirmation() {
    const auto in = input(ctx_.now);
    const auto preview = ct::Runner::preview(fusb302::caps(), options_, in);
    const bool later = (preview.refusal == ct::Reason::NoSource || preview.refusal == ct::Reason::Release) &&
                       pd::start_needs_rd(ctx_.inline_mode, ctx_.vbus_fast_mv);
    if (preview.refusal != ct::Reason::None && !later) { message(review_refusal(preview.refusal, ctx_.ui->busy_reason())); return; }
    confirming_ = true; confirm_until_ = ctx_.now + Ui::kConfirmWindowMs; confirm_proposal_ = proposal(in);
}
// The second press: an identity needs the charger directly on the plug (include/k2/pd/ra_guard.hpp), so the guard judges
// the lines first and a refusal opens the Refuse stage; then the identity is armed — whatever was armed is disarmed
// first and the pick takes over (Off arms nothing) — and the start goes on with the Rd and the wait.
void ChargerTestSession::confirmed_start() {
    const auto choice = ctx_.cables.selected();
    const bool bypass = guard_bypass_;   // "Start anyway" on the Cable-on-plug warning: the identity runs, the reading is recorded
    guard_bypass_ = false;
    run_guard_ = 0;
    if (choice.kind != ctcable::Kind::Off) {
        topology_ = guard_topology(ctx_.now);
        const bool refused = !raguard::spoof_allowed(topology_);
        // A K2 that runs on the measured port is attached to the charger before it can show a cable, and a charger that reads the
        // e-marker does so at its attach: the identity would come late (runs/selfpwr-6a/). The user is told and can go on.
        const bool late = !refused && self_supplied();
        if ((refused || late) && !bypass) { refuse_late_ = late; set_stage(Stage::Refuse); return; }
        run_guard_ = static_cast<uint8_t>((1 + static_cast<uint8_t>(topology_)) | ((refused || late) ? 0x80 : 0));
        if (refused) K2LOG("[GUARD] started anyway against a %s reading\n", raguard::name(topology_));
        else if (late) K2LOG("[GUARD] started anyway, self-supplied: the Ra follows the attach\n");
    }
    if (apply_cable(choice)) press_start();
    else invalidate_confirmation();   // refused (message on the footer): the next OK is a first one again
}
// After the identity is settled: Rd (and the wait for VBUS) as the last stage, then the run.
void ChargerTestSession::press_start() {
    bbpersist::make_room();   // the Black Box gives the journal its blocks back now, not on some later save inside the run
    if (start_begin()) { wait_step(ctx_.now); return; }   // Waiting; a charger already up ends it on this very call
    launch();
}
// Back to a passive K2 on Review with the reason on the footer: nothing started.
void ChargerTestSession::start_failed(const char* why) {
    release_run();
    if (stage_ != Stage::Review) set_stage(Stage::Review, kStartRow);   // from Waiting; set_stage clears the message, so it comes after
    message(why);
}
// The preflight on the final reading (the wait changed it), then the run itself.
void ChargerTestSession::launch() {
    const auto preview = ct::Runner::preview(fusb302::caps(), options_, input(ctx_.now));
    if (preview.refusal != ct::Reason::None) { start_failed(review_refusal(preview.refusal, ctx_.ui->busy_reason())); return; }
    begin_run();
}
void ChargerTestSession::begin_run() {
    const auto in = input(ctx_.now);
    journal_active_ = false; capture_kept_ = false;
    capture_identity();
    const auto r = runner_.start(fusb302::caps(), options_, in, true);
    K2LOG("[TEST] start selected=%u units=%u -> %s\n", unsigned(options_.selected), unsigned(options_.units.n), ct::name(r));
    charger_journal::RunMeta meta;
    meta.units = options_.units;
    meta.unattended = self_supplied();   // the K2 runs on the measured port (pinned Rd or bus-powered): the run never detaches
    self_run_ = meta.unattended;
    meta.cable_kind = static_cast<uint8_t>(run_cable_.kind); meta.cable_seq = run_cable_.seq;
    meta.guard = run_guard_;   // the Ra guard's reading when the identity was started (and whether the user went on anyway)
    options_.units = {};   // an explicit unit list serves one run; the next START uses the default order
    invalidate_confirmation();
    if (r == ct::Reason::None || runner_.state() == ct::State::Unsafe) {
        g_ev_row = kNoRow;
        runev::emit(runev::Code::Start, ctx_.now, ev8(meta.units.n), ev8(runner_.steps().size()), 0);
        // The plan goes to flash in pieces, one file per tick on a quiet link (a synchronous begin held the main loop 1.5-2.2 s with
        // the charger's contract up, 2026-11-08: no GoodCRC, hard reset). The Runner waits in its Baseline for it (hold), so no
        // request, and no phase marker, comes before the header; the meta record follows the header.
        arm_meta_ = meta; arm_ = 1; arm_t0_ = ctx_.now; jr_wait_ms_ = 0;
        charger_journal::hw::arm_start();
        runner_.hold(true);
        journal_active_ = true; journal_closed_ = false;
        set_stage(Stage::Running); follow_ = true;
    }
    else start_failed(review_refusal(r, ctx_.ui->busy_reason()));
}
void ChargerTestSession::leave() { cancel(); }
bool ChargerTestSession::set_units(std::span<const uint8_t> units) {
    ct::UnitList list;
    uint32_t seen = 0;
    for (const uint8_t u : units) {
        if (u >= static_cast<uint8_t>(ct::Unit::Count) || (seen & (1u << u)) || !list.add(static_cast<ct::Unit>(u))) return false;
        seen |= 1u << u;
    }
    options_.units = list;
    invalidate_confirmation();   // the preview's runnable count follows the list
    return true;
}
void ChargerTestSession::enter() {
    ctx_.cables.sync();
    entering_ = true;   // the screen still shows the previous page, which the redraw erases: no body wipe here
    set_stage(resume_pending_ ? Stage::Resume : Stage::Algorithms);
    entering_ = false;
}
void ChargerTestSession::set_stage(Stage s, int selection) {
    invalidate_confirmation();
    // Entering the identity / review pair from elsewhere starts the Ra guard's window afresh (the Waiting stage ends in
    // Review with the old, still passive samples: its Rd-on readings are never sampled).
    if (guard_stage(s) && !guard_stage(stage_) && stage_ != Stage::Waiting) { guard_.clear(); guard_sample(ctx_.now); }
    if (s != stage_) runev::emit(runev::Code::Stage, ctx_.now, static_cast<uint8_t>(s));
    if (s == Stage::Running && stage_ != Stage::Running) run_t0_ = ctx_.now;
    stage_ = s; selection_ = selection; message_.clear();
    if (view_) view_->on_stage(entering_);   // the page repaints the new stage (no page on screen: nothing to paint)
}
// A stage owns the whole body: wipe the old one (a (re)draw of the page needs no wipe: the screen is clean or being erased),
// then every element of the new stage paints once. ctx_.rows is shared with the other pages, so it starts afresh too.
ChargerTestPage::ChargerTestPage(UiContext& c, ChargerTestSession& s) : PageBase(c, Page::ChargerTest, "Charger test"), s_(s) {
    s_.bind_view(this);
}
void ChargerTestPage::on_stage(bool entering) {
    if (!entering) gfx_fill(ctx_.content, 0, 22, ctx_.content.w, kFooterY - 22, kBg);
    invalidate();
    grid_ = kit::ResultGrid{};
    if (s_.stage_ == Stage::Results) build_results();
}
void ChargerTestPage::on_confirmation_dropped() { foot_.invalidate(); action_.invalidate(); }
void ChargerTestPage::on_cable_changed() { update(ctx_.now); }
void ChargerTestPage::build_results() {
    // Passive labels from the identity and the advertised APDOs (the same facts as the card), shown in
    // the OEM-over-PD section: the QC4+/QC5 label, a VID-driven vendor name, the Mi Turbo auth state.
    const auto card = report_card::build(runner_.steps(), runner_.evidence(), runner_.handover(), card_id_, {}, runner_.state(), runner_.reason());
    result_layout::OemNotes oem;
    oem.headers = false;   // the grid is units only: no separator rows between the sections
    oem.vendor = result_layout::vendor_oem_name(card_id_.vid);
    if (!oem.vendor && fusb302::vdm().xiaomi) oem.vendor = "Xiaomi";   // SVID 0x2717 seen on the wire even without a VID
    oem.mi_auth = fusb302::sink_status().xiaomi_authed;
    oem.oppo_svid = report_card::oppo_note(card) != nullptr;
    char scp_note[24] = {};   // "SCP cap 20V 6A": the row's label is the first two words
    if (report_card::scp_note(card, scp_note, sizeof scp_note)) oem.scp_cap = scp_note + 8;
    char justification[48] = {};
    if (const auto label = report_card::qc_label(card); label != report_card::QcLabel::None) {
        report_card::qc_label_justification(card, justification, sizeof justification);
        oem.qc_label = report_card::qc_label_name(label);
        oem.qc_why = justification;   // build() copies the words into tokens inside this scope
    }
    result_layout::build(layout_, runner_.steps(), oem);
    // build() counts a row label or word it had to shorten: the gate fails on it like on any other cut.
    if (layout_.cuts) K2_TRUNC_NOTE_HERE(trunc::Kind::Cut, "results grid label or word", layout_.cuts);
    line_n_ = result_layout::wrap(layout_, kGridMetrics, lines_);
    compose_best();
}
// The best passing mode per protocol family as one line. Families are dropped from the end (lowest watts first) when the
// strip is full. Empty when nothing passed.
__attribute__((noinline)) void ChargerTestPage::compose_best() {
    best_.clear();
    std::array<result_layout::SummaryLine, 3> best{};
    const size_t n = result_layout::summarize(runner_.steps(), best);
    for (size_t i = 0; i < n; ++i) {
        const size_t len = std::strlen(best[i].text);
        if (best_.view().size() + (i ? 2 : 0) + len > size_t(kBestChars)) break;
        if (i) best_.put("  ");
        best_.put(best[i].text);
    }
}
void ChargerTestSession::message(const char* s) {
    message_.clear(); message_.put(s); invalidate_confirmation();
    if (view_) view_->on_message();
}
// The algorithm a Group row stands for (rows are 0-based: the family's own entries, nothing else).
A ChargerTestSession::algorithm_at(int row) const {
    if (row < 0) return A::Count;
    if (family_ == kFamPd) return row == 0 ? A::Pd3Fixed : row == 1 ? A::Pps : A::Count;
    for (unsigned n = 0; n < unsigned(A::Count); ++n) {
        const auto a = static_cast<A>(n);
        if (!(kFamilies[family_] & ct::bit(a))) continue;
        if (!row--) return a;
    }
    return A::Count;
}
int ChargerTestSession::step_index(int row) const {
    if (stage_ == Stage::Running) return row;
    const auto steps = runner_.steps();
    for (size_t n = 0; n < steps.size(); ++n) {
        if (result_family_ >= 0 && family(steps[n]) != result_family_) continue;
        if (!row--) return static_cast<int>(n);
    }
    return -1;
}
int ChargerTestPage::row_count() const {
    switch (stage_) {
    case Stage::Algorithms: return kViewRow + (runner_.run_id() ? 1 : 0);
    case Stage::Custom: return kFamilyCount + 1;   // the families, then "Done"
    case Stage::Group: return family_ == kFamPd ? 2 : static_cast<int>(selected(kFamilies[family_]));
    case Stage::Cable: return ctx_.cables.count();
    case Stage::Review: return 1;
    case Stage::Running: return static_cast<int>(runner_.steps().size());
    case Stage::Results: return static_cast<int>(line_n_) + (delta_n_ ? 1 : 0);
    case Stage::Changes: return static_cast<int>(delta_n_);
    case Stage::Steps: return static_cast<int>(counts(runner_, result_family_).total);
    case Stage::Detail: return 0;
    case Stage::Resume: return 3;
    case Stage::Identity: case Stage::Ask: return 0;   // retired, never entered
    case Stage::Refuse: return 3;   // Start anyway, Start with the cable OFF, Cancel
    case Stage::Waiting: return 1;
    }
    return 0;
}
ListState ChargerTestPage::list_state() const {
    if (stage_ == Stage::Results) return {row_count(), grid_.visible(), grid_.first()};   // the grid scrolls by pixel height, not by row slots
    return {row_count(), ctx_.rows.visible(), ctx_.rows.state().offset};
}

ChargerTestSession::Proposal ChargerTestSession::proposal(ct::Input in) const {
    const auto& caps = fusb302::caps();
    Proposal out{options_.selected, in.session, in.cap_mv, uint32_t(in.cable_mv), uint32_t(in.cable_ma),
                 uint32_t(in.cable_observed), uint32_t(in.inline_load), uint32_t(in.fake),
                 uint32_t(in.busy), uint32_t(pd::is_vsafe5(in.vbus_mv)),
                 uint32_t(in.pd_traffic), emk_spoof::generation(), uint32_t(emk_spoof::active()),
                 uint32_t(caps.valid), uint32_t(caps.num_pdos), uint32_t(caps.active_cc),
                 uint32_t(caps.spec_rev), uint32_t(caps.epr), uint32_t(caps.epr_partial),
                 uint32_t(in.reported.valid), uint32_t(in.reported.rev_major), uint32_t(in.reported.rev_minor),
                 uint32_t(in.reported.ver_major), uint32_t(in.reported.ver_minor)};
    out[23] = 0;
    for (size_t n = 0; n < caps.pdos.size(); ++n) out[24 + n] = caps.pdos[n].raw;
    return out;
}

// ====================================================================================================================
// The view (docs/UI_DESIGN.md "Charger test"): the shared kit, one painter per stage.
// ====================================================================================================================
namespace {
// ---- copy that is not a preset (every 12 px note fits the kit's 190 px note box; the assert below pins it)
constexpr const char *kNoteCustom = "Choose families one by one", *kNoteLast = "The run kept from last time",
                     *kNoteFamily = "Entries inside this family", *kNoteXiaomi = "Mi 6A probes run in PD",
                     *kNotePdFixed = "Fixed offers: SPR + EPR", *kNotePps = "PPS + SPR/EPR AVS offers",
                     *kNoteClassB = "20 V; Class A shows a skip", *kNoteUfcs = "UART 5/9/12V; needs UFCS source",
                     *kNoteVivo = "UART 5/9/11V, unproven", *kNoteHs = "5V handshake, not power",
                     *kNoteEntry = "One entry of this family";
constexpr const char *kNotes[] = {kNoteCustom, kNoteLast, kNoteFamily, kNoteXiaomi, kNotePdFixed, kNotePps, kNoteClassB,
                                  kNoteUfcs, kNoteVivo, kNoteHs, kNoteEntry};
static_assert(all_fit(kNotes, kLabelAdv, kit::kNoteMaxPx), "a note is wider than the row note");

// The Group page's mask for row `row` of family `fam` (the PD family has two rows: fixed offers and the PPS / AVS offers).
uint32_t group_mask(int fam, int row, A a) {
    if (fam == kFamPd) return row == 0 ? ct::kPdFixedAlgorithms : ct::kPpsAlgorithms;
    return ct::bit(a);
}
const char *group_note(int fam, int row, A a) {
    if (fam == kFamPd) return row == 0 ? kNotePdFixed : kNotePps;
    switch (a) {
    case A::Qc2ClassB: case A::Qc3ClassB: return kNoteClassB;
    case A::Ufcs: return kNoteUfcs;
    case A::Vivo: return kNoteVivo;
    case A::Vooc: case A::SuperVooc: case A::SuperVooc65: return kNoteHs;
    default: return kNoteEntry;
    }
}
// A Custom family row's box: 0 off, 1 part, 2 on. Xiaomi / Redmi follows the PD mask (its probes run in PD).
uint8_t family_state(uint32_t sel, int f) {
    const uint32_t m = kFamilies[f == kFamXiaomi ? kFamPd : f];
    const uint32_t on = sel & m;
    return on == 0 ? 0 : on == m ? 2 : 1;
}
kit::Verdict vmap(ct::Verdict v) {
    switch (v) {
    case ct::Verdict::Running: return kit::Verdict::Running;
    case ct::Verdict::Pass: return kit::Verdict::Pass;
    case ct::Verdict::Fail: return kit::Verdict::Fail;
    case ct::Verdict::Skip: return kit::Verdict::Skip;
    default: return kit::Verdict::Pending;
    }
}
// A step held a plateau below its target: amber (Partial), the same tone its chip wears (result_layout::tone).
bool is_capped(const ct::Step& s) { return s.verdict == ct::Verdict::Fail && s.reason == ct::Reason::Capped; }
kit::Verdict vmap(const ct::Step& s) { return is_capped(s) ? kit::Verdict::Partial : vmap(s.verdict); }
// A delta range value (100 mV units) as "13V" / "13.5V".
template <typename Buf> void put_dv(Buf &b, unsigned dv) {
    b.put_u32(dv / 10);
    if (dv % 10) b.put_char('.').put_u32(dv % 10);
    b.put_char('V');
}
// A voltage without trailing zeros: "5", "9", "5.5", "3.3", "20".
template <typename Buf> void put_volts_trim(Buf &b, int32_t mv) {
    if (mv < 0) mv = 0;
    b.put_u32(static_cast<uint32_t>(mv / 1000));
    const int32_t r = mv % 1000;
    if (!r) return;
    b.put_char('.').put_u32(static_cast<uint32_t>(r / 100));
    if (r % 100) b.put_u32(static_cast<uint32_t>((r / 10) % 10));
    if (r % 10) b.put_u32(static_cast<uint32_t>(r % 10));
}
// A CC line's reading as the Waiting page words it (the Rp ladder of a source's pull-up; "open" = no pull-up or above it).
const char *cc_word(int mv) {
    if (mv < 0) return "--";
    if (mv < 200) return "open";
    if (mv < 660) return "Rp USB";
    if (mv < 1230) return "Rp 1.5A";
    if (mv < 2200) return "Rp 3A";
    return "open";
}
bool cc_open(int mv) { return mv >= 0 && (mv < 200 || mv >= 2200); }
// "12 pass" style parts of a counter line, appended only while the footer's 27 cells still hold them.
template <typename Buf> void put_part(Buf &b, unsigned n, const char *word, bool &first) {
    if (!n) return;
    StrBuf<16> part;
    part.put_u32(n).put_char(' ').put(word);
    if (b.view().size() + (first ? 0 : 2) + part.view().size() > kFooterCells) return;
    if (!first) b.put(", ");
    b.put(part.view());
    first = false;
}
// One line of the Running grid's step counters: the unit of step `at`, and which of its steps this is.
void unit_progress(const ct::Runner &runner, size_t at, unsigned &i, unsigned &n, const char *&unit) {
    const auto &steps = runner.steps();
    const auto u = ct::unit_of(steps[at]);
    unit = ct::name(u);
    i = n = 0;
    for (size_t k = 0; k < steps.size(); ++k) {
        if (ct::unit_of(steps[k]) != u) continue;
        ++n;
        if (k <= at) ++i;
    }
}
}  // namespace

void ChargerTestPage::bar() {
    StrBuf<24> t;
    int step = 0;
    switch (stage_) {
    case Stage::Algorithms: t.put("Tests"); step = 1; break;
    case Stage::Custom: t.put("Custom"); step = 1; break;
    case Stage::Group: t.put("Entries"); step = 1; break;
    case Stage::Cable: case Stage::Identity: case Stage::Ask: t.put("Cable"); step = 2; break;
    case Stage::Review: t.put("Review"); step = 3; break;
    case Stage::Results: t.put("Results"); break;
    case Stage::Changes: t.put("Changes"); break;
    case Stage::Detail: t.put("Step detail"); break;
    case Stage::Steps:
        if (results_row_ >= 0 && static_cast<size_t>(results_row_) < line_n_) t.put(layout_.rows[lines_[results_row_].row].label).put_char(' ');
        t.put("steps");
        break;
    default: t.put("Charger test"); break;
    }
    kit::draw_top_bar(ctx_.content, t.view(), step, fusb302::self_powered() ? "SNK" : "PAS");
    bar_valid_ = false;   // the band repaint erased the V x A readout: title_badge paints it again
    bar_key_ = static_cast<uint8_t>(static_cast<uint8_t>(stage_) + 1);
}

void ChargerTestPage::foot(FooterTone tone, std::string_view text) {
    draw_footer_cached(ctx_.content, tone, text, foot_);
}

// The footer of the stage (no key names: the strip says what the keys do). Returns its tone; a message takes it on every stage.
[[gnu::noinline]] FooterTone ChargerTestPage::footer_line(StrBuf<32> &f) {
    if (!message_.view().empty()) {
        f.put(message_.view().substr(0, kFooterCells));
        return FooterTone::Caution;
    }
    switch (stage_) {
    case Stage::Algorithms: {
        const int pr = preset_row_of(options_.selected);
        f.put("Picked: ").put(pr ? kPresetNames[pr - 1] : "custom set");
        return FooterTone::Info;
    }
    case Stage::Custom: {
        unsigned on = 0;
        for (int n = 0; n < kFamilyCount; ++n) on += family_state(options_.selected, n) != 0;
        f.put_u32(on).put(" of ").put_u32(kFamilyCount).put(" families on");
        return FooterTone::Info;
    }
    case Stage::Group: {
        unsigned on = 0;
        const int n = row_count();
        for (int i = 0; i < n; ++i) {
            const uint32_t m = group_mask(family_, i, algorithm_at(i));
            on += (options_.selected & m) == m;
        }
        f.put_u32(on).put(" of ").put_u32(static_cast<unsigned>(n)).put(" entries on");
        return FooterTone::Info;
    }
    case Stage::Cable: case Stage::Identity: case Stage::Ask: {
        const auto sel = ctx_.cables.selected();
        if (sel.kind == ctcable::Kind::Off) { f.put("Picked: no identity"); return FooterTone::Info; }
        row_ = ctx_.cables.row(ctx_.cables.index_of(sel), CableIdentityList::View::CableStep);
        f.put("Picked: ");
        const size_t room = kFooterCells - 8 - (row_.rating.empty() ? 0 : row_.rating.size() + 1);
        f.put(row_.text.view().substr(0, room));
        if (!row_.rating.empty()) f.put_char(' ').put(row_.rating);
        return FooterTone::Info;
    }
    case Stage::Review: {
        const auto in = input(ctx_.now);
        if (summary_.refusal != ct::Reason::None) {
            f.put(review_refusal(summary_.refusal, ctx_.ui->busy_reason()));
            return FooterTone::Caution;
        }
        if (!summary_.runnable) { f.put("Nothing to run: all skip"); return FooterTone::Caution; }
        if (confirming_) { f.put("Press again to start"); return FooterTone::Caution; }
        if (in.cable_mv == 20000 && summary_.max_mv == 21000) { f.put("PPS: 21V on 20V wire"); return FooterTone::Info; }
        if (self_supplied()) { f.put("Self-powered: VBUS stays up"); return FooterTone::Info; }
        f.put("Runs without a load");
        return FooterTone::Info;
    }
    case Stage::Refuse: f.put(refuse_late_ ? kLateFooter : "Direct plug is the sure way"); return FooterTone::Caution;
    case Stage::Waiting: {
        const int cc1 = ctx_.analog ? ctx_.analog->cc1_mv : -1, cc2 = ctx_.analog ? ctx_.analog->cc2_mv : -1;
        const int32_t vbus = ctx_.vbus_fast_mv;
        if (wait_.state() == pd::VbusWait::State::Ready) { f.put("Charger up, starting"); return FooterTone::Ok; }
        if (cc_open(cc1) && cc_open(cc2) && vbus < 500) { f.put("No charger on either CC"); return FooterTone::Caution; }
        if (vbus >= 500 && vbus < 4500) { f.put("Standby. Wake the charger"); return FooterTone::Info; }
        if (vbus >= 4500 && static_cast<int32_t>(ctx_.now - wait_t0_) >= 3000) { f.put("VBUS up, no PD caps yet"); return FooterTone::Info; }
        f.put("Charger to the top port");
        return FooterTone::Info;
    }
    case Stage::Running: {
        const auto &steps = runner_.steps();
        if (runner_.state() == ct::State::Unsafe) { f.put("Unplug; verify measured 5V"); return FooterTone::Error; }
        if (runner_.reason() != ct::Reason::None) { f.put(reason_text(runner_.reason())); return FooterTone::Caution; }
        if (steps.empty()) { f.put("Starting"); return FooterTone::Info; }
        unsigned i, n;
        const char *unit;
        unit_progress(runner_, std::min(runner_.index(), steps.size() - 1), i, n, unit);
        f.put(unit).put(": step ").put_u32(i).put(" of ").put_u32(n);
        return FooterTone::Info;
    }
    case Stage::Results: {
        if (selection_ >= static_cast<int>(line_n_)) { f.put("Compare with the last card"); return FooterTone::Info; }
        const auto &row = layout_.rows[lines_[selection_].row];
        f.put(row.label).put(": ");
        if (row.algorithm == A::Count) { f.put("see steps"); return FooterTone::Info; }
        if (ct::kPpsAlgorithms & ct::bit(row.algorithm)) {   // a range says how far the source got, not a count
            const auto rc = result_layout::reach(runner_.steps(), row.algorithm);
            f.put(result_layout::reach_footer(rc).c_str());
            using result_layout::Tone;
            return rc.tone == Tone::Pass ? FooterTone::Ok : rc.tone == Tone::Partial || rc.tone == Tone::Fail ? FooterTone::Caution
                                                                                                                : FooterTone::Info;
        }
        unsigned pass = 0, fail = 0, other = 0;
        for (const auto &s : runner_.steps()) {
            if (s.algorithm != row.algorithm) continue;
            pass += s.verdict == ct::Verdict::Pass;
            fail += s.verdict == ct::Verdict::Fail;
            other += s.verdict != ct::Verdict::Pass && s.verdict != ct::Verdict::Fail;
        }
        bool first = true;
        put_part(f, pass, "pass", first);
        put_part(f, fail, "fail", first);
        put_part(f, other, "other", first);
        if (first) f.put("no result");
        return fail ? FooterTone::Caution : pass ? FooterTone::Ok : FooterTone::Info;
    }
    case Stage::Steps: {
        const auto c = counts(runner_, result_family_);
        f.put_u32(c.total).put(" steps, ").put_u32(c.fail).put(" failed");
        return c.fail ? FooterTone::Caution : FooterTone::Info;
    }
    case Stage::Detail: f.put("Kept until the next start"); return FooterTone::Info;
    case Stage::Changes: f.put_u32(delta_n_).put(" changed, was run ").put_u32(delta_runs_); return FooterTone::Info;
    case Stage::Resume: {
        const auto w = resume_words(ctx_.now);
        f.put(resumetext::footer(w));
        return w.held ? FooterTone::Caution : FooterTone::Info;
    }
    }
    return FooterTone::Info;
}

void ChargerTestPage::row_fn(void *p, int index, bool focused, kit::RowSpec &out) {
    static_cast<ChargerTestPage *>(p)->list_row(index, focused, out);
}

// Row `index` of the stage's list. The text lives in the page's shared buffers (consumed at once by the kit's paint).
void ChargerTestPage::list_row(int index, bool focused, kit::RowSpec &out) {
    out = {};
    row_text_.clear();
    row_tail_.clear();
    row_note_.clear();
    switch (stage_) {
    case Stage::Algorithms:
        if (index < kPresets) {
            out.marker = kit::Marker::Radio;
            out.state = index + 1 == preset_row_of(options_.selected);
            out.text = kPresetNames[index];
            row_tail_.put_u32(entries(kPresetMasks[index]));
            out.tail = row_tail_.view();
            if (focused) out.note = kPresetNotes[index];
        } else if (index == kCustomRow) {
            const bool custom = preset_row_of(options_.selected) == 0;
            out.marker = kit::Marker::Radio;
            out.state = custom;
            out.text = "Custom...";
            out.text_color = custom ? kFocus : kText;
            out.chevron = true;
            if (focused) out.note = kNoteCustom;
        } else {
            out.text = "Last results";
            out.chevron = true;
            if (focused) out.note = kNoteLast;
        }
        break;
    case Stage::Custom:
        if (index >= kFamilyCount) {
            out.text = "Done";
            out.text_color = kFocus;
            break;
        }
        out.marker = kit::Marker::Box3;
        out.state = family_state(options_.selected, index);
        out.text = kFamilyNames[index];
        if (index == kFamXiaomi) {
            out.text_color = kDim;
            out.tail = "PD";
            if (focused) out.note = kNoteXiaomi;
        } else {
            row_tail_.put_u32(entries(options_.selected & kFamilies[index])).put_char('/').put_u32(entries(kFamilies[index]));
            out.tail = row_tail_.view();
            if (focused) out.note = kNoteFamily;
        }
        break;
    case Stage::Group: {
        const A a = algorithm_at(index);
        const uint32_t m = group_mask(family_, index, a);
        out.marker = kit::Marker::Check;
        out.state = (options_.selected & m) == m;
        out.text = family_ == kFamPd ? (index == 0 ? "PD fixed" : "PPS") : ct::name(a);
        if (focused) out.note = group_note(family_, index, a);
        break;
    }
    case Stage::Cable: case Stage::Identity: case Stage::Ask:
        row_ = ctx_.cables.row(index, CableIdentityList::View::CableStep);
        CableIdentityList::fill_spec(row_, out);
        if (focused) out.note = row_.note;
        break;
    case Stage::Refuse:
        out.text = index == 0 ? "Start anyway" : index == 1 ? "Start, cable OFF" : "Cancel";
        if (index == 0) out.text_color = kCaution;
        break;
    case Stage::Resume:
        if (index == 0) {
            const auto c = counts(runner_);
            row_text_.put("Run ").put_u32(runner_.run_id()).put(": ").put_u32(c.pass + c.fail + c.skip).put_char('/').put_u32(c.total).put(" done");
            out.text = row_text_.view();
        } else if (index == 1) {
            out.text = reason_text(runner_.reason());
            out.text_color = kCaution;
            out.data = true;
        } else {
            const auto w = resume_words(ctx_.now);
            out.text = resumetext::row(w);
            out.text_color = w.gate_ready ? kOk : kCaution;
            if (focused) out.note = resumetext::note(w);
        }
        break;
    case Stage::Changes: {
        const auto &d = delta_[static_cast<size_t>(index)];
        using DL = report_card::DeltaLine;
        if (d.kind == DL::Group) {
            out.text = report_card::detail::group_label(d.index);
            row_tail_.put(report_card::detail::gv_label(d.was)).put(" > ").put(report_card::detail::gv_label(d.now));
            out.tail_color = d.now == report_card::GV::Pass ? kOk : kError;
        } else {
            row_text_.put(report_card::detail::alg_label(d.index));
            if (d.kind == DL::RangeMax || (d.was_dv && d.now_dv)) {
                row_text_.put(d.kind == DL::RangeMax ? " max" : " fail");
                put_dv(row_tail_, d.was_dv);
                row_tail_.put(" > ");
                put_dv(row_tail_, d.now_dv);
            } else {
                row_text_.put_char(' ');
                put_dv(row_text_, d.was_dv ? d.was_dv : d.now_dv);
                row_tail_.put(d.was_dv ? "FAIL > PASS" : "PASS > FAIL");
            }
            out.tail_color = (d.was_dv && !d.now_dv) ? kOk : kCaution;
        }
        out.text = row_text_.view().empty() ? out.text : row_text_.view();
        out.tail = row_tail_.view();
        break;
    }
    case Stage::Steps: {
        const int si = step_index(index);
        if (si < 0) { out.text = "No selected steps"; out.text_color = kDim; break; }
        const auto &steps = runner_.steps();
        const auto &s = steps[static_cast<size_t>(si)];
        out.marker = kit::Marker::Verdict;
        out.state = static_cast<uint8_t>(vmap(s));
        row_text_.put(step_label::label(s, si > 0 ? &steps[static_cast<size_t>(si) - 1] : nullptr).view());
        out.text = row_text_.view();
        out.data = true;
        if (s.measured_mv) {
            row_tail_.put_fixed(s.measured_mv, 1).put_char('V');
            out.tail_color = kVolt;
        } else {
            row_tail_.put("--");
            out.tail_color = kFaint;
        }
        out.tail = row_tail_.view();
        if (focused) {
            row_note_.put(ct::name(s.verdict));
            if (s.verdict == ct::Verdict::Fail || s.verdict == ct::Verdict::Skip) row_note_.put(": ").put(detail_name(s));
            if (s.pos) row_note_.put(" PDO #").put_u32(s.pos);
            out.note = row_note_.view().substr(0, kit::kNoteMaxPx / kLabelAdv);
        }
        break;
    }
    default: break;
    }
}

// RowList stages: bind the list on first paint after an invalidate, then apply count and selection (the window follows).
void ChargerTestPage::paint_list() {
    if (!laid_) {
        const int16_t y = stage_ == Stage::Resume ? 56 : stage_ == Stage::Refuse ? 110 : kit::kBodyY;
        ctx_.rows.init(ctx_.content, y, &ChargerTestPage::row_fn, this);
        ctx_.rows.restart();
        ctx_.rows.invalidate();
        laid_ = true;
    }
    ctx_.rows.set_count(row_count());
    ctx_.rows.select(selection_);
    ctx_.rows.draw(ctx_.now);
}

// The "Cable on plug" warning: the alert band, two lines of what it means, then the three choices.
void ChargerTestPage::paint_warning() {
    if (!laid_) {
        const Surface &c = ctx_.content;
        kit::draw_alert_band(c, 22, false, refuse_late_ ? kLateWord : "Cable on plug");
        gfx_text(c, kSmall, 6, 64, refuse_late_ ? kLateLines[0] : "The spoof may not reach", kText, kBg);
        gfx_text(c, kSmall, 6, 84, refuse_late_ ? kLateLines[1] : "the charger.", kText, kBg);
    }
    paint_list();
}

// ====================================================================================================================
// The dense stages: Review, Waiting, Running, Results, Detail. Each lays its static part out once (laid_) and then repaints
// only what changed, every dynamic piece behind its own signature (cell_sig_) so an idle update writes nothing.
// ====================================================================================================================
namespace {
using result_layout::Tone;
constexpr const char *kChecks[3] = {"Rd presented", "VBUS up", "Source caps"};
static_assert(all_fit(kChecks, kSmallAdvPx, 190), "a Waiting checklist item is wider than its row");
// The Review table: five rows of 26 px from the body top, the key at x6 and the value from x72 (18 cells).
constexpr kit::KvLayout kKv{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, 72, 18, Align::Left};
// The Detail table: under the verdict band, the value column at x92 (16 cells).
constexpr kit::KvLayout kDet{60, kit::kRowH, 6, nullptr, nullptr, 92, 16, Align::Left};

kit::Verdict tone_verdict(Tone t) {
    switch (t) {
    case Tone::Pass: return kit::Verdict::Pass;
    case Tone::Fail: return kit::Verdict::Fail;
    case Tone::Partial: return kit::Verdict::Partial;
    case Tone::Pending: return kit::Verdict::Pending;
    case Tone::Skip: return kit::Verdict::Skip;
    default: return kit::Verdict::None;
    }
}
void put_clip(char (&dst)[12], std::string_view s) {
    const size_t n = std::min<size_t>(s.size(), sizeof dst - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}
// The section of the layout row `row`: the section of the last protocol row at or before it (a collapsed row follows its neighbour).
int row_section(const result_layout::Layout &l, size_t row) {
    for (size_t r = row + 1; r-- > 0;)
        if (l.rows[r].algorithm != A::Count) return static_cast<int>(result_layout::section_of(l.rows[r].algorithm));
    return -1;
}
// The head line of a row whose section differs from the line above opens with a 2 px rule.
bool section_break(const result_layout::Layout &l, std::span<const result_layout::Line> lines, size_t i) {
    if (i == 0 || !lines[i].head) return false;
    return row_section(l, lines[i].row) != row_section(l, lines[i - 1].row);
}
// The token of layout row `row` that the step in flight (level `mv`) stands for: the still-pending chip of that level, else the
// first still-pending chip of the row (a range or a probe chip has no level). A legacy chip merges the up and down steps of one
// level, so it stays pending while a later step of its level is left: "first pending" alone would keep the dot on the 5 V chip
// while the runner is on 9 V. -1 = none.
int pending_chip(const result_layout::Layout &l, size_t row, uint32_t mv) {
    const auto &r = l.rows[row];
    int first = -1;
    for (int k = 0; k < r.count; ++k) {
        const auto &t = l.tokens[static_cast<size_t>(r.first + k)];
        if (!t.chip || t.glue || t.tone != Tone::Pending) continue;
        if (t.mv && t.mv == mv) return r.first + k;
        if (first < 0) first = r.first + k;
    }
    return first;
}
// The step dot's token: the step the runner is on, Pending or Running, so the dot moves in the tick the index does (the gauge's
// target comes from the same index). A finished step at the index (the run's end) has no dot.
int running_token(const result_layout::Layout &l, const ct::Runner &runner, size_t row) {
    const auto &steps = runner.steps();
    if (runner.index() >= steps.size()) return -1;
    const ct::Verdict v = steps[runner.index()].verdict;
    if (v != ct::Verdict::Running && v != ct::Verdict::Pending) return -1;
    if (l.rows[row].algorithm != steps[runner.index()].algorithm) return -1;
    return pending_chip(l, row, steps[runner.index()].mv);
}
// The grid line the step `index` belongs to, known as soon as the runner moves to it (its verdict need not be Running yet):
// the line holding the first pending chip of its protocol's row, else the head line of that row, else the top (0).
int follow_line(const result_layout::Layout &l, std::span<const result_layout::Line> lines, std::span<const ct::Step> steps, size_t index) {
    if (steps.empty()) return 0;
    const auto alg = steps[std::min(index, steps.size() - 1)].algorithm;
    int head = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (l.rows[lines[i].row].algorithm != alg) continue;
        const int tk = pending_chip(l, lines[i].row, steps[std::min(index, steps.size() - 1)].mv);
        if (tk >= lines[i].first && tk < lines[i].first + lines[i].count) return static_cast<int>(i);
        if (lines[i].head) head = static_cast<int>(i);
    }
    return head;
}
// The pixels one update may still paint (Running, a step in flight). An item that does not fit stays dirty and paints on a
// later tick. (The grid, whose rows can exceed the cap, is admitted separately: alone, with kFlightRowPx.)
constexpr int kFooterBandH = 240 - kFooterY;   // the footer band: y222..239
struct Budget {
    int cap;
    int spent = 0;
    bool admit(int px) {
        if (px <= cap - spent) { spent += px; return true; }
        return false;
    }
};
// One cached text cell: `chars` cells wide, padded with blanks (left or right) so a shorter text leaves nothing behind. `sig`
// is the cell's signature (0 = not drawn); `data` marks external text (clipped and counted, not a gate failure). With a
// budget a changed cell that does not fit waits (its signature stays old, so it is still dirty on the next call).
void put_line(const Surface &c, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t color, unsigned chars, uint32_t &sig,
              bool right = false, bool data = false, Budget *bg = nullptr) {
    uint32_t h = mix(mix_init(), text);
    h = mix_u32(mix_u32(h, color), chars * 2u + (right ? 1u : 0u)) | 1u;
    if (h == sig) return;
    if (bg && !bg->admit(static_cast<int>(chars) * f.adv * f.height)) return;
    sig = h;
    char buf[kFitBuf];
    const int px = static_cast<int>(chars * static_cast<unsigned>(f.adv));
    const std::string_view t = data ? fit_data(f, text, px, buf) : fit_text(f, text, px, buf);
    StrBuf<40> line;
    const size_t pad = chars > t.size() ? chars - t.size() : 0;
    if (right) for (size_t i = 0; i < pad; ++i) line.put_char(' ');
    line.put(t);
    if (!right) for (size_t i = 0; i < pad; ++i) line.put_char(' ');
    gfx_text(c, f, x, y, line.view(), color, kBg);
}
// The hero reading: 2 decimals below 10 V, 1 above; an unknown reading is two dashes.
void hero_text(StrBuf<12> &b, int32_t mv) {
    if (mv < 0) { b.put("--"); return; }
    b.put_fixed(mv, mv < 10000 ? 2 : 1);
}
}  // namespace

void ChargerTestPage::paint_review() {
    const Surface &c = ctx_.content;
    if (!laid_) {
        gfx_fill(c, 0, 154, c.w, 1, kRule);
        action_.init(c, 156, 64);
        laid_ = true;
    }
    const auto in = input(ctx_.now);
    const auto &caps = fusb302::caps();
    auto kv = [&](int i, const char *key, std::string_view v, uint16_t color) {
        const uint32_t h = mix_u32(mix(mix(mix_init(), key), v), color) | 1u;
        if (h == cell_sig_[i]) return;
        cell_sig_[i] = h;
        const int16_t y = kit::kv_row_y(kKv, i);
        gfx_fill(c, 0, static_cast<int16_t>(y + 1), c.w, kit::kRowH - 1, kBg);
        kit::kv_row(c, kKv, y, key, v, color);
    };
    StrBuf<32> t;
    t.put_u32(summary_.runnable).put(" run, ").put_u32(summary_.skipped).put(" skip");
    kv(0, "Tests", t.view(), summary_.runnable ? kText : kCaution);

    StrBuf<32> pk;
    const pd::Pdo *best = nullptr;
    if (caps.valid)
        for (size_t n = 0; n < caps.num_pdos && n < caps.pdos.size(); ++n)
            if (!best || caps.pdos[n].max_mw > best->max_mw) best = &caps.pdos[n];
    if (best) {
        put_volts_trim(pk, best->max_mv);
        pk.put("V ");
        put_volts_trim(pk, best->max_ma);
        pk.put_char('A');
        if (best->max_mw) pk.put_char(' ').put_u32(best->max_mw / 1000).put_char('W');
    } else if (summary_.max_mv) {
        put_volts_trim(pk, static_cast<int32_t>(summary_.max_mv));
        pk.put_char('V');
    } else {
        pk.put("--");
    }
    kv(1, "Peak", pk.view(), pk.view() == "--" ? kDim : kText);

    StrBuf<48> cb;
    const auto sel = ctx_.cables.selected();
    if (sel.kind == ctcable::Kind::Off) cb.put("OFF");   // plain: a START with Off disarms whatever is armed
    else {
        row_ = ctx_.cables.row(ctx_.cables.index_of(sel), CableIdentityList::View::CableStep);
        cb.put(row_.text.view());
        if (!row_.rating.empty()) cb.put_char(' ').put(row_.rating);
    }
    kv(2, "Cable", cb.view(), kText);

    StrBuf<48> hd;
    if (caps.valid) put_revision(hd, caps.spec_rev, in.reported);
    else hd.put("--");
    kv(3, "Header", hd.view(), caps.valid ? kText : kDim);
    kv(4, "Power", self_supplied() ? "From the charger" : "K2 on USB", kText);

    if (confirming_) {
        const int32_t left = std::max<int32_t>(0, static_cast<int32_t>(confirm_until_ - ctx_.now));
        const int permille = std::clamp<int>(left * 1000 / static_cast<int32_t>(Ui::kConfirmWindowMs), 0, 1000);
        StrBuf<24> sub;
        sub.put("closes in ").put_u32(static_cast<uint32_t>((left + 999) / 1000)).put_char('s');
        action_.draw_armed("CONFIRM", sub.view(), permille);
    } else {
        action_.draw_idle("START", "no load on the port");
    }
}

void ChargerTestPage::paint_hero(bool full, int16_t x, int16_t y, uint8_t chars, int32_t vbus_mv, uint16_t color) {
    StrBuf<12> t;
    hero_text(t, vbus_mv);
    kit::draw_hero(ctx_.content, hero_, kit::HeroSpec{&kBig, x, y, chars, color, "V"}, t.view(), full);
}

void ChargerTestPage::paint_waiting(uint32_t now) {
    const Surface &c = ctx_.content;
    const int32_t vbus = ctx_.vbus_fast_mv;
    const auto &caps = fusb302::caps();
    if (!laid_) {
        gfx_text(c, kLabelFont, 6, 26, "VBUS", kDim, kBg);
        gfx_fill(c, 0, 84, c.w, 2, kRule);
        static constexpr const char *kTags[2][2] = {{"CC1", "CC2"}, {"D+", "D-"}};
        for (int i = 0; i < 2; ++i) {
            const int16_t y = static_cast<int16_t>(86 + i * 26);
            gfx_text(c, kSmall, 6, static_cast<int16_t>(y + 6), kTags[i][0], kDim, kBg);
            gfx_text(c, kSmall, 118, static_cast<int16_t>(y + 6), kTags[i][1], kDim, kBg);
            gfx_fill(c, 112, y, 1, 25, kRule);
            gfx_fill(c, 0, static_cast<int16_t>(y + 25), c.w, 1, kRule);
        }
        laid_ = true;
    }
    const uint16_t color = vbus >= pd::kStartVbusMv ? kVolt : vbus > 500 ? kCaution : kFaint;
    paint_hero(color != hero_color_, 170, 34, 5, vbus, color);
    hero_color_ = color;

    const int cc1 = ctx_.analog ? ctx_.analog->cc1_mv : -1, cc2 = ctx_.analog ? ctx_.analog->cc2_mv : -1;
    const int cc[2] = {cc1, cc2};
    for (int i = 0; i < 2; ++i) {
        const int mv = cc[i];
        put_line(c, kSmall, static_cast<int16_t>(i ? 154 : 42), 92, cc_word(mv), mv < 0 || cc_open(mv) ? kFaint : kText, 8, cell_sig_[i]);
    }
    const bool dv = vbus >= pd::kStartVbusMv && ctx_.analog;
    const int d[2] = {ctx_.analog ? ctx_.analog->dp_mv : -1, ctx_.analog ? ctx_.analog->dm_mv : -1};
    for (int i = 0; i < 2; ++i) {
        StrBuf<12> v;
        if (dv) v.put_fixed(std::max(d[i], 0), 2).put_char('V');
        else v.put("--");
        put_line(c, kSmall, static_cast<int16_t>(i ? 154 : 42), 118, v.view(), dv ? kText : kFaint, 8, cell_sig_[2 + i]);
    }

    // The give-up clock: a 2 px bar that drains over the wait (the whole wait of the first attempt; a retry reads empty).
    const uint32_t total = caps.active_cc != 0 ? pd::kStartVbusAttachedWaitMs : pd::kStartVbusWaitMs;
    const uint32_t spent = now - wait_t0_;
    const uint32_t left = (wait_paused_ || spent >= total) ? 0 : total - spent;
    const int px = static_cast<int>(static_cast<uint64_t>(left) * static_cast<uint32_t>(c.w) / total);
    const uint32_t bsig = mix_u32(mix_init(), static_cast<uint32_t>(px)) | 1u;
    if (bsig != cell_sig_[4]) {
        cell_sig_[4] = bsig;
        if (px > 0) gfx_fill(c, 0, 138, static_cast<int16_t>(px), 2, kDim);
        if (px < c.w) gfx_fill(c, static_cast<int16_t>(px), 138, static_cast<int16_t>(c.w - px), 2, kRule);
    }

    const bool done[3] = {true, vbus >= pd::kStartVbusMv, caps.valid};
    for (int i = 0; i < 3; ++i) {
        const uint32_t s = done[i] ? 2u : 1u;
        if (s == cell_sig_[5 + i]) continue;
        cell_sig_[5 + i] = s;
        const int16_t y = static_cast<int16_t>(141 + i * 26);
        gfx_fill(c, 0, y, c.w, 25, kBg);
        kit::draw_verdict(c, 8, static_cast<int16_t>(y + 6), 14, 14, done[i] ? kit::Verdict::Pass : kit::Verdict::None, {}, kBg);
        gfx_text(c, kSmall, 28, static_cast<int16_t>(y + 5), kChecks[i], done[i] ? kText : kDim, kBg);
        gfx_fill(c, 0, static_cast<int16_t>(y + 25), c.w, 1, kRule);
    }
}

// The Running verdict grid: the light layout of the plan (no card), wrapped the way Results wraps it.
void ChargerTestPage::build_running_grid() {
    result_layout::OemNotes oem;
    oem.headers = false;
    oem.fold = false;   // a unit keeps its row when it fails or is skipped: the finished rows stay above the running one
    result_layout::build(layout_, runner_.steps(), oem);
    line_n_ = result_layout::wrap(layout_, kGridMetrics, lines_);
}

// The Running gauge's full scale: one scale for the whole run, so the pass band slides with each target (5 V near the left,
// 20 V near the right) and the needle shows the travel between steps. 10 % above the highest target in the plan, rounded up
// to a whole volt, at least 6 V. A per-target scale (target x 1.5) put every band at the same 2/3 point and never moved.
// Derived from the plan each call: no state, a few dozen compares.
static int32_t gauge_full_mv(std::span<const ct::Step> steps) {
    int32_t top = 0;
    for (const auto &s : steps) top = std::max<int32_t>(top, s.mv);
    const int32_t full = (top * 110 / 100 + 999) / 1000 * 1000;
    return std::max<int32_t>(full, 6000);
}

void ChargerTestPage::paint_running(uint32_t now) {
    const Surface &c = ctx_.content;
    const auto &steps = runner_.steps();
    // While a step is in flight (Runner::quiet()) every item below goes through one pixel budget (kFlightPx); an item that does
    // not fit stays dirty and paints on a later tick. update() has already returned if the engine is in a window the idle hook
    // does not serve (Runner::paint_blocked), so nothing here runs inside a handshake. Between steps the cap is off.
    const bool flight = runner_.quiet() && laid_;
    Budget bg{flight ? kFlightPx : (1 << 30)};
    const size_t at = steps.empty() ? 0 : std::min(runner_.index(), steps.size() - 1);
    const ct::Step *st = steps.empty() ? nullptr : &steps[at];
    if (!laid_) {
        prog_.init(c, 6, 44, 212);
        gfx_fill(c, 0, 98, c.w, kit::kPassBandH, kBg);
        gfx_fill(c, 0, 112, c.w, 2, kRule);
        grid_.init(c, 114, kFooterY, &ChargerTestPage::result_row_fn, this);
        laid_ = true;
    }
    const int32_t vbus = ctx_.pm_online ? ctx_.pm.vbus_mv : -1;
    const int32_t target = st ? st->mv : 0;
    // 1. the gauge strip, once per target (a new step): the whole strip, the needle drawn at the live reading
    if (static_cast<uint32_t>(target) + 1u != cell_sig_[5] && bg.admit(target > 0 ? 210 * kit::kPassBandH : c.w * kit::kPassBandH)) {
        cell_sig_[5] = static_cast<uint32_t>(target) + 1u;
        needle_px_ = -1;
        if (target > 0) needle_px_ = kit::draw_pass_band(c, 6, 98, 210, std::max<int32_t>(vbus, 0), target, gauge_full_mv(steps), -1);
        else gfx_fill(c, 0, 98, c.w, kit::kPassBandH, kBg);
    }
    // 1b. the step dot, in the same tick as the gauge band: the layout follows the runner now, and the grid moves the dot from
    // the cell that was under test to the one that is (kRunDotPx, never gated). The rows themselves scroll and repaint at item 6.
    const uint32_t key = mix_u32(mix_u32(mix_init(), runner_.revision()), static_cast<uint32_t>(runner_.index())) | 1u;
    if (key != grid_key_) {
        grid_key_ = key;
        build_running_grid();
    }
    grid_.set_count(static_cast<int>(line_n_));
    bg.spent += grid_.sync_dots();
    // 2. the live VBUS and its needle: every update between steps, about 4 Hz while a step is in flight
    if ((!flight || now - hero_ms_ >= kHeroMs) && bg.admit(kHeroPx + (needle_px_ >= 0 ? kNeedlePx : 0))) {
        hero_ms_ = now;
        paint_hero(hero_color_ != kVolt, 196, 54, 4, vbus, kVolt);
        hero_color_ = kVolt;
        if (target > 0 && needle_px_ >= 0) needle_px_ = kit::draw_pass_band(c, 6, 98, 210, std::max<int32_t>(vbus, 0), target, gauge_full_mv(steps), needle_px_);
    }
    // 3. elapsed m:ss at the right edge (one text per second)
    {
        const uint32_t secs = (now - run_t0_) / 1000;
        StrBuf<12> e;
        e.put_u32(secs / 60).put_char(':');
        if (secs % 60 < 10) e.put_char('0');
        e.put_u32(secs % 60);
        put_line(c, kSmall, 170, 25, e.view(), kDim, 6, cell_sig_[1], true, false, &bg);
    }
    // 4. the step header: counter, progress bar, protocol, step name, target. A step change is visible within a tick or two.
    {
        const auto all = counts(runner_);
        StrBuf<16> n;
        n.put_u32(st ? static_cast<uint32_t>(at + 1) : 0).put_char('/').put_u32(static_cast<uint32_t>(steps.size())).put(" steps");
        put_line(c, kSmall, 6, 25, n.view(), kText, 12, cell_sig_[0], false, false, &bg);
        const int done = static_cast<int>(all.pass + all.fail + all.skip), total = static_cast<int>(steps.size());
        if (bg.admit(prog_.cost(done, total))) prog_.set(done, total);
        const int fam = st ? family(*st) : -1;
        put_line(c, kLabelFont, 6, 52, fam >= 0 ? kFamilyNames[fam] : "", kDim, 16, cell_sig_[2], false, false, &bg);
        StrBuf<32> nm;
        if (st) nm.put(step_label::label(*st, at > 0 ? &steps[at - 1] : nullptr).view());
        put_line(c, kSmall, 6, 68, nm.view(), kText, 11, cell_sig_[3], false, true, &bg);
        StrBuf<16> tg;
        if (st && st->mv) { tg.put("target "); put_volts_trim(tg, st->mv); tg.put_char('V'); }
        put_line(c, kLabelFont, 6, 88, tg.view(), kDim, 16, cell_sig_[4], false, false, &bg);
    }
    // 5. the footer (the unit and its step), one cached band
    {
        StrBuf<32> f;
        const FooterTone tone = footer_line(f);
        const uint32_t sig = mix_u32(mix(mix_init(), f.view()), static_cast<uint32_t>(tone)) | 1u;
        if (sig != foot_sig_ && bg.admit(c.w * kFooterBandH)) {
            foot_sig_ = sig;
            foot(tone, f.view());
        }
    }
    // 6. the verdict grid follows the step under test (the layout was rebuilt at 1b). The window scrolls to the running line (a
    // manual scroll holds kFollowResumeMs, or until the step changes); rows paint one at a time, the running line first, and
    // only the cells whose verdict changed when the rows themselves did not move. Scroll and repaint may lag the dot.
    if (!follow_ && (now - scroll_ms_ >= kFollowResumeMs || runner_.index() != scroll_step_)) follow_ = true;
    const int want = follow_ ? follow_line(layout_, std::span<const result_layout::Line>(lines_.data(), line_n_), steps, runner_.index())
                             : std::clamp(scroll_line_, 0, std::max(0, static_cast<int>(line_n_) - 1));
    if (follow_) grid_.anchor_bottom(want);   // finished rows stay above, the row under test ends the window
    else grid_.set_first(want);
    // an idle tick lets the grid have a whole row; otherwise it gets what the other items left, never more
    const int room = bg.spent == 0 ? kFlightRowPx : bg.cap - bg.spent;
    const int drawn = grid_.draw_some(flight ? room : (1 << 30), follow_ ? want : -1, flight && bg.spent != 0);
    bg.spent += drawn;
}

void ChargerTestPage::result_row_fn(void *p, int index, kit::ResultRowSpec &out) {
    static_cast<ChargerTestPage *>(p)->grid_row(index, out);
}

// Row `index` of the verdict grid (a layout line, or the Changes pseudo-row after them). The text lives in the page's cell buffers.
void ChargerTestPage::grid_row(int index, kit::ResultRowSpec &out) {
    out = {};
    const bool results = stage_ == Stage::Results;
    if (index >= static_cast<int>(line_n_)) {
        StrBuf<12> t;
        t.put_u32(static_cast<uint32_t>(delta_n_)).put(" changed");
        put_clip(cell_txt_[0][0], t.view());
        out.label = "Changes";
        out.cells[0] = {kit::Verdict::None, cell_txt_[0][0], {}};
        out.n = 1;
        out.focus = results && selection_ == index;
        return;
    }
    const auto &ln = lines_[static_cast<size_t>(index)];
    const auto &row = layout_.rows[ln.row];
    if (ln.head) {
        std::memset(cell_lbl_, 0, sizeof cell_lbl_);
        std::memcpy(cell_lbl_, row.label, std::min<size_t>(std::strlen(row.label), sizeof cell_lbl_ - 1));
        out.label = cell_lbl_;
    }
    const int running = results ? -1 : running_token(layout_, runner_, ln.row);
    int n = 0;
    for (int k = 0; k < ln.count && n < kit::kResultMaxCells; ++k) {
        const size_t ti = static_cast<size_t>(ln.first + k);
        const auto &tk = layout_.tokens[ti];
        if (tk.glue) continue;
        std::string_view volts = tk.text, cur;
        if (tk.chip) {
            const size_t sp = volts.find(' ');
            if (sp != std::string_view::npos) { cur = volts.substr(sp + 1); volts = volts.substr(0, sp); }
        }
        put_clip(cell_txt_[n][0], volts);
        out.cells[n].label = cell_txt_[n][0];
        if (!cur.empty()) {
            put_clip(cell_txt_[n][1], cur);
            out.cells[n].current = cell_txt_[n][1];
        }
        out.cells[n].verdict = static_cast<int>(ti) == running ? kit::Verdict::Running : tone_verdict(tk.tone);
        ++n;
    }
    out.n = static_cast<uint8_t>(n);
    out.focus = results && selection_ >= 0 && selection_ < static_cast<int>(line_n_) && lines_[static_cast<size_t>(selection_)].row == ln.row;
    out.section = section_break(layout_, std::span<const result_layout::Line>(lines_.data(), line_n_), static_cast<size_t>(index));
}

void ChargerTestPage::paint_results() {
    const Surface &c = ctx_.content;
    const auto all = counts(runner_);
    if (!laid_) {
        const kit::Verdict kinds[3] = {kit::Verdict::Pass, kit::Verdict::Fail, kit::Verdict::Skip};
        for (int i = 0; i < 3; ++i) {
            const int16_t x = static_cast<int16_t>(i * 75);
            if (i) gfx_fill(c, static_cast<int16_t>(x - 1), 22, 1, 24, kRule);
            kit::draw_verdict(c, static_cast<int16_t>(x + 2), 28, 12, 12, kinds[i], {}, kBg);
        }
        gfx_fill(c, 0, 46, c.w, 1, kRule);
        gfx_fill(c, 0, 70, c.w, 2, kRule);
        grid_.init(c, 72, kFooterY, &ChargerTestPage::result_row_fn, this);
        laid_ = true;
    }
    const unsigned n[3] = {all.pass, all.fail, all.skip};
    static constexpr const char *kWords[3] = {"pass", "fail", "skip"};
    const uint16_t ink[3] = {kOk, kError, kDim};
    for (int i = 0; i < 3; ++i) {
        StrBuf<12> t;
        t.put_u32(n[i]);
        if (t.view().size() + 1 + std::strlen(kWords[i]) <= 7) t.put_char(' ').put(kWords[i]);
        put_line(c, kSmall, static_cast<int16_t>(i * 75 + 16), 26, t.view(), ink[i], 7, cell_sig_[i]);
    }
    put_line(c, kSmall, 6, 51, best_.view().empty() ? std::string_view("No passing mode") : best_.view(), best_.view().empty() ? kDim : kText,
             kBestChars, cell_sig_[3], false, true);
    grid_.set_count(static_cast<int>(line_n_) + (delta_n_ ? 1 : 0));
    grid_.show(selection_, 1);
    grid_.draw();
}

// VBUS around the failed step, from the stored transition capture: min..max per column over the ~2 s window, the first PD
// Accept in amber and the trigger (the failure) in red.
void ChargerTestPage::draw_capture(const transcap::Meta &m) {
    constexpr int16_t kTop = 167, kHeight = 26, kX = 6;
    constexpr int kScale = 2;
    draw_range_trace(ctx_.content, kX, kTop, static_cast<int>(transcap::kThumbCols), kScale, kHeight, m.thumb.lo.data(), m.thumb.hi.data(),
                     m.thumb.accept_col, kCaution, m.thumb.trig_col, kError, kVolt, kBg);
    StrBuf<40> cap;
    cap.put("Peak ").put_fixed(m.thumb.vmax_mv, 2).put("V ").put(transcap::reason_name(m.reason));
    char fitted[kFitBuf];
    gfx_text(ctx_.content, kLabelFont, 6, kTop + kHeight + 1, fit_text(kLabelFont, cap.view(), 150, fitted), kDim, kBg);
    if (m.thumb.accept_col != 0xFF) gfx_text(ctx_.content, kLabelFont, 160, kTop + kHeight + 1, "|Accept", kCaution, kBg);
}

// One step's detail: a verdict band, then a fixed table (no scroll). A step with a stored capture gives its last two rows to the
// VBUS trace; a refused 6 A hunt row shows its evidence where the header rows would be.
void ChargerTestPage::paint_detail() {
    if (laid_) return;
    laid_ = true;
    const auto &steps = runner_.steps();
    if (detail_index_ < 0 || static_cast<size_t>(detail_index_) >= steps.size()) return;
    const Surface &c = ctx_.content;
    const auto &s = steps[static_cast<size_t>(detail_index_)];
    const auto &e = runner_.evidence();
    transcap::Meta cap;
    const bool has_cap = transcap::find_stored(runner_.run_id(), static_cast<uint16_t>(detail_index_), cap);
    const ct::HuntEvidence *hunt = runner_.hunt_evidence_for(static_cast<size_t>(detail_index_));

    const kit::Verdict v = vmap(s);
    const auto vs = kit::verdict_style(v);
    const uint16_t fill = vs.filled ? vs.fill : kSurface;
    gfx_fill(c, 0, 22, c.w, 36, fill);
    kit::draw_verdict(c, 6, 30, 20, 20, v, {}, fill);
    const char *word = "PENDING";
    switch (s.verdict) {
    case ct::Verdict::Pass: word = "PASS"; break;
    case ct::Verdict::Fail: word = is_capped(s) ? "CAPPED" : "FAIL"; break;
    case ct::Verdict::Skip: word = "SKIP"; break;
    case ct::Verdict::Running: word = "RUNNING"; break;
    default: break;
    }
    gfx_text(c, kMid, 32, 28, word, vs.ink, fill);
    StrBuf<32> nm;
    nm.put(step_label::label(s, detail_index_ > 0 ? &steps[static_cast<size_t>(detail_index_) - 1] : nullptr).view());
    char buf[kFitBuf];
    const std::string_view name = fit_data(kSmall, nm.view(), 96, buf);
    const uint16_t name_ink = vs.ink == kDim || vs.ink == kFaint ? kText : vs.ink;
    gfx_text(c, kSmall, static_cast<int16_t>(218 - static_cast<int>(name.size()) * kSmallAdvPx), 32, name, name_ink, fill);

    int row = 0;
    auto kv = [&](const char *key, std::string_view value, uint16_t color) {
        kit::kv_row(c, kDet, kit::kv_row_y(kDet, row++), key, value, color);
    };
    StrBuf<24> t;
    if (s.mv) t.put_fixed(s.mv, 2).put("V ").put_u32(s.ma).put("mA");
    else t.put("No request");
    kv("Target", t.view(), s.mv ? kText : kDim);
    StrBuf<16> m;
    if (s.measured_mv) m.put_fixed(s.measured_mv, 2).put_char('V');
    else m.put("--");
    kv("Measured", m.view(), s.measured_mv ? kVolt : kFaint);
    if (hunt) {
        for (int n = 0; n < 4; ++n) {
            StrBuf<64> line;
            put_hunt_line(line, *hunt, n);
            char fitted[kFitBuf];
            gfx_text(c, kLabelFont, 6, static_cast<int16_t>(116 + n * 18), fit_text(kLabelFont, line.view(), c.w - 12, fitted),
                     n == 0 ? kCaution : kDim, kBg);
        }
        return;
    }
    StrBuf<24> w;
    if (is_capped(s)) {   // the window of a capped step adds nothing: say how far its range got (in the rows above any capture)
        w.put(result_layout::reach_span(result_layout::reach(steps, s.algorithm, s.pos)).view());
        kv("Reached", w.view(), kCaution);
    } else {
        if (s.mv) w.put_fixed(s.mv * 95 / 100, 2).put_char('-').put_fixed(s.mv * 105 / 100, 2).put_char('V');
        else w.put("--");
        kv("Window", w.view(), s.mv ? kText : kDim);
    }
    kv("Reason", detail_name(s), s.verdict == ct::Verdict::Fail ? kCaution : kText);
    if (has_cap) {
        draw_capture(cap);
        return;
    }
    if (const auto mode = step_label::ufcs_range(s); mode.len) {   // a UFCS step: the mode it samples ("5-20V 0.5-3A"), not a PD header
        kv("Mode", mode.view(), kText);
    } else {
        StrBuf<24> h;
        h.put(pd::wire_revision_name(e.wire_rev));
        if (e.reported.valid) h.put("  RMDO ").put_u32(e.reported.rev_major).put_char('.').put_u32(e.reported.rev_minor);
        kv("Header", h.view(), kText);
    }
    kv("Scope", "Voltage hold", kText);
    gfx_fill(c, 0, kit::kv_row_y(kDet, row), c.w, 1, kRule);
}

void ChargerTestPage::paint_resume() {
    const Surface &c = ctx_.content;
    if (!laid_) gfx_fill(c, 0, 26, c.w, 24, kBg);
    put_line(c, kSmall, 6, 28, resumetext::status(resume_words(ctx_.now)), kCaution, 26, cell_sig_[0]);
    paint_list();
}

void ChargerTestPage::sync_proposal(uint32_t now) {
    const auto in = input(now);
    if (confirming_ && (static_cast<int32_t>(now - confirm_until_) >= 0 || proposal(in) != confirm_proposal_)) invalidate_confirmation();
    if (stage_ <= Stage::Review || stage_ == Stage::Custom) summary_ = ct::Runner::preview(fusb302::caps(), options_, in);
}

void ChargerTestPage::share_card() {
    report_card::Card card;
    if (!report_card::hw::get(card_no_, card)) return;
    const std::size_t len = qrshare::records::card(card, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::Card, qrshare::kSchemaCard, card_name_[0] ? card_name_ : "Card", len, ctx_.now);
}

void ChargerTestPage::draw_static() {
    invalidate();
    bar();
}

// The page's tick. Each stage's painter is its own small frame; a step in flight on Running paints within a pixel budget
// (paint_running) and not at all inside an engine window (Runner::paint_blocked).
void ChargerTestPage::update(uint32_t now) {
    ctx_.cables.sync();   // the library changed: the list is read again (a chosen clone that is gone falls back to Off)
    sync_proposal(now);
    selection_ = std::clamp(selection_, 0, last_row());
    fill_cable_names();
    // A step in flight inside an engine window the idle hook does not serve (the handshake pulses, the VOOC receive slices,
    // the UFCS/VIVO windows): nothing paints. The tick is dropped, the next one repaints what is dirty.
    if (stage_ == Stage::Running && laid_ && runner_.paint_blocked(fctrig::paint_critical())) return;
    if (bar_key_ != static_cast<uint8_t>(static_cast<uint8_t>(stage_) + 1)) bar();
    title_badge(false);
    switch (stage_) {
    case Stage::Review: paint_review(); break;
    case Stage::Refuse: paint_warning(); break;
    case Stage::Waiting: paint_waiting(now); break;
    case Stage::Running: paint_running(now); break;
    case Stage::Results: paint_results(); break;
    case Stage::Detail: paint_detail(); break;
    case Stage::Resume: paint_resume(); break;
    default: paint_list(); break;
    }
    if (stage_ == Stage::Running) return;   // its footer is one of paint_running's budgeted items
    StrBuf<32> f;
    const FooterTone tone = footer_line(f);
    foot(tone, f.view());
}

bool ChargerTestPage::on_event(Event e) {
    const bool long_ok = e.press == Press::Long && e.button == Button::Ok;
    // Long OK on the Results / Detail is Share: the run's report card as QR (a card exists once the run is saved).
    if (long_ok && (stage_ == Stage::Results || stage_ == Stage::Detail) && !runner_.busy() && card_no_ != 0) {
        share_card();
        return true;
    }
    // Long OK on a Custom family opens its entries; on an entries page it toggles the whole group.
    const bool open_group = long_ok && stage_ == Stage::Custom && selection_ < kFamilyCount && selection_ != kFamXiaomi;
    const bool toggle_group = long_ok && stage_ == Stage::Group;
    if (e.press != Press::Short && !open_group && !toggle_group && !(e.press == Press::Long && e.button == Button::Back)) return false;
    if (open_group) {
        family_ = selection_;
        set_stage(Stage::Group, 0);
        update(ctx_.now);
        return true;
    }
    if (toggle_group) {
        const uint32_t group = kFamilies[family_];
        options_.selected = (options_.selected & group) == group ? options_.selected & ~group : options_.selected | group;
        invalidate_confirmation();
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Back) {
        if (stage_ == Stage::Resume) { abandon_run(); update(ctx_.now); return true; }
        if (runner_.busy()) { cancel(); update(ctx_.now); return true; }
        if (confirming_) { invalidate_confirmation(); update(ctx_.now); return true; }   // Back out of the confirm window: nothing was touched
        switch (stage_) {
        case Stage::Refuse: set_stage(Stage::Review, kStartRow); break;   // Cancel: nothing was touched
        case Stage::Waiting: wait_abort(); break;                          // Cancel: Rd back, nothing started
        case Stage::Algorithms: return back_event(e);
        case Stage::Group: set_stage(Stage::Custom, family_); break;
        case Stage::Custom: set_stage(Stage::Algorithms, kCustomRow); break;
        case Stage::Cable: {
            const int pr = preset_row_of(options_.selected);
            set_stage(Stage::Algorithms, pr ? pr - 1 : kCustomRow);
            break;
        }
        case Stage::Review: set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected())); break;
        case Stage::Identity: case Stage::Ask: set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected())); break;   // retired, never entered
        case Stage::Steps: set_stage(Stage::Results, results_row_); break;
        case Stage::Detail: set_stage(Stage::Steps, detail_row_); break;
        case Stage::Changes: set_stage(Stage::Results, static_cast<int>(line_n_)); break;
        default: set_stage(Stage::Algorithms); break;
        }
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Plus || e.button == Button::Minus) {
        if (stage_ == Stage::Detail) return true;
        const int dir = e.button == Button::Plus ? 1 : -1;
        if (stage_ == Stage::Running) {   // scroll the verdict grid by hand; following resumes (paint_running) after kFollowResumeMs or at the next step
            scroll_line_ = std::clamp(grid_.first() + dir, 0, std::max(0, static_cast<int>(line_n_) - 1));
            scroll_ms_ = ctx_.now;
            scroll_step_ = runner_.index();
            follow_ = false;
            update(ctx_.now);
            return true;
        }
        if (stage_ == Stage::Results) {   // the cursor steps unit by unit: a wrapped row's later lines are skipped
            int i = selection_ + dir;
            while (i >= 0 && i < static_cast<int>(line_n_) && !lines_[static_cast<size_t>(i)].head) i += dir;
            selection_ = std::clamp(i, 0, last_row());
        } else {
            selection_ = std::clamp(selection_ + dir, 0, last_row());
        }
        follow_ = false;
        invalidate_confirmation();
        message_.clear();
        update(ctx_.now);
        return true;
    }
    if (e.button != Button::Ok) return false;
    ok_event();
    return true;
}

__attribute__((noinline)) void ChargerTestPage::ok_event() {
    if (stage_ == Stage::Resume) {
        if (resume_hold_) { resume_hold_ = false; auto_resume_ = true; }   // the identity / Rd that could not go on: try again, on OK's say
        resume_run(ctx_.now);
        update(ctx_.now);
        return;
    }
    if (runner_.busy()) { cancel(); update(ctx_.now); return; }
    switch (stage_) {
    case Stage::Algorithms:   // a preset picks and advances; "Custom..." opens the per-family list
        if (selection_ < kPresets) {
            options_.selected = kPresetMasks[selection_];
            invalidate_confirmation();
            set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected()));
        } else if (selection_ == kCustomRow) {
            set_stage(Stage::Custom, 0);
        } else {
            set_stage(Stage::Results);
        }
        break;
    case Stage::Custom:
        if (selection_ >= kFamilyCount) {   // Done
            set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected()));
        } else if (selection_ != kFamXiaomi) {   // Mi 6A rides the PD unit: nothing to toggle
            const uint32_t mask = kFamilies[selection_];
            options_.selected = (options_.selected & mask) == mask ? options_.selected & ~mask : options_.selected | mask;
            invalidate_confirmation();
        }
        break;
    case Stage::Group:
        if (const A a = algorithm_at(selection_); a != A::Count) {
            const uint32_t mask = group_mask(family_, selection_, a);
            options_.selected ^= (options_.selected & mask) == mask ? mask : mask & ~options_.selected;
            invalidate_confirmation();
        }
        break;
    case Stage::Cable:   // a pick selects and advances: nothing is armed until START
        ctx_.cables.select(ctx_.cables.choice_at(selection_));
        invalidate_confirmation();
        set_stage(Stage::Review, kStartRow);
        break;
    case Stage::Review:
        if (confirm_live()) confirmed_start();   // the second press: guard, identity, then Rd and the wait last
        else open_confirmation();                // the first press: the confirm window, nothing touched
        break;
    case Stage::Waiting: wait_abort(); break;   // OK is a cancel here too: the wait is never the answer to a stray press
    case Stage::Identity: case Stage::Ask: break;   // retired, never entered
    case Stage::Refuse:
        if (selection_ == 0) {   // Start anyway: the identity runs armed, the guard's reading goes to the journal
            guard_bypass_ = true;
            confirmed_start();
        } else if (selection_ == 1) {   // Start with the cable OFF (already confirmed)
            ctx_.cables.select({});
            invalidate_confirmation();
            set_stage(Stage::Review, kStartRow);
            confirmed_start();
        } else {
            set_stage(Stage::Review, kStartRow);   // Cancel
        }
        break;
    case Stage::Changes: set_stage(Stage::Results, static_cast<int>(line_n_)); break;
    case Stage::Results:
        if (selection_ >= static_cast<int>(line_n_)) {
            if (delta_n_) set_stage(Stage::Changes);
        } else if (!layout_.rows[lines_[static_cast<size_t>(selection_)].row].header) {   // section headers are not openable
            const auto &row = layout_.rows[lines_[static_cast<size_t>(selection_)].row];
            results_row_ = selection_;
            result_family_ = row.algorithm != A::Count ? family(row.algorithm) : std::strcmp(row.label, "Turbo 6A") == 0 ? kFamXiaomi : -1;   // the Mi 6A probes
            set_stage(Stage::Steps);
        }
        break;
    case Stage::Steps:
        if (const int index = step_index(selection_); index >= 0) {
            detail_index_ = index;
            detail_row_ = selection_;
            set_stage(Stage::Detail);
        }
        break;
    case Stage::Detail: set_stage(Stage::Steps, detail_row_); break;
    case Stage::Running: set_stage(Stage::Results); break;
    case Stage::Resume: break;
    }
    update(ctx_.now);
}

PanelLabels ChargerTestPage::labels() const {
    const char *ok = "";
    const char *ok_long = "";
    if (runner_.busy()) {
        ok = "Stop";
    } else {
        switch (stage_) {
        case Stage::Algorithms: ok = selection_ < kPresets ? "Pick" : "Open"; break;
        case Stage::Custom:
            ok = selection_ >= kFamilyCount ? "Next" : selection_ == kFamXiaomi ? "" : "Toggle";
            if (selection_ < kFamilyCount && selection_ != kFamXiaomi) ok_long = "Open";
            break;
        case Stage::Group: ok = "Set"; ok_long = "Grp"; break;
        case Stage::Cable: ok = "Pick"; break;
        case Stage::Review: ok = confirming_ ? "Confirm" : "Start"; break;
        case Stage::Refuse: ok = selection_ >= 2 ? "Cancel" : "Start"; break;
        case Stage::Waiting: ok = "Cancel"; break;
        case Stage::Results:
            ok = selection_ >= static_cast<int>(line_n_) ? "Diff" : "Steps";
            if (card_no_ != 0) ok_long = "Share";
            break;
        case Stage::Changes: ok = "View"; break;
        case Stage::Steps: ok = "Open"; break;
        case Stage::Detail: ok = "Steps"; if (card_no_ != 0) ok_long = "Share"; break;
        case Stage::Resume: ok = "Resume"; break;
        case Stage::Running: ok = "View"; break;
        default: break;   // Identity / Ask are retired
        }
    }
    PanelLabels l = setup::labels(base_labels(), tests_stage(stage_) ? setup::Step::Tests : setup::Step::Cable, ok, ok_long);
    if (stage_ == Stage::Detail || stage_ == Stage::Review || stage_ == Stage::Waiting) {   // nothing to move the cursor over
        l.b[static_cast<int>(Button::Minus)].shrt = "";
        l.b[static_cast<int>(Button::Plus)].shrt = "";
    }
    return l;
}

void ChargerTestPage::invalidate() {
    ctx_.rows.invalidate();
    grid_.invalidate();   // a forced repaint keeps the scroll position; set_stage() starts a stage at the top
    action_.invalidate();
    prog_.invalidate();
    foot_.invalidate();
    body_.invalidate();
    std::fill(std::begin(cell_sig_), std::end(cell_sig_), 0u);
    laid_ = false;
    needle_px_ = -1;
    hero_ms_ = 0;
    grid_key_ = 0;
    foot_sig_ = 0;
    bar_key_ = 0;
    bar_valid_ = false;
    hero_color_ = 0;
}

}  // namespace k2
