// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_setup_page.hpp"
#include "k2/ui/spec_text.hpp"
#include "k2/core/loop_guard.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/run_layout.hpp"
#include "k2/ui/sweep_hero.hpp"
#include "k2/ui/text.hpp"
#include "k2/core/bbpersist.hpp"
#include "k2/core/gap_meter.hpp"
#include "k2/core/log.hpp"
#include "k2/core/paint_meter.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/core/rescan_bridge.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/pd/boot_ra.hpp"
#include "k2/pd/cable_fact.hpp"
#include "k2/chargertest/charger_journal_hw.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/pd/pd_ccmode.hpp"
#include "k2/pd/link.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/pd/power_source.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/pd/transcap.hpp"
#include "k2/pd/transcap_reason.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/chargertest/report_card_deep.hpp"
#include "k2/chargertest/report_card_delta.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/chargertest/run_events.hpp"
#include "k2/core/scratch.hpp"
#include "k2/chargertest/pd_version.hpp"
#include "k2/chargertest/prog_check.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/chargertest/oem_watts.hpp"
#include "k2/core/storage.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/fastcharge/svooc_trig.hpp"
#include <algorithm>
#include <bit>
#include <cstring>

namespace k2 {
// A reason as the screen words it: charger_test::refusal_words() owns the words (the shared name, shortened where the
// 26-27 cell lines need it); the log line of a START preflight refusal says the same words.
namespace {
namespace ct = charger_test;
using A = ct::Algorithm;
// Run events for the live debug stream (docs/DEBUG_STREAMING.md, k2/chargertest/run_events.hpp): the row whose RowStart went out last, so a
// row that starts without a hook of its own (the 5 V rows) is still announced exactly once.
constexpr uint16_t kNoRow = 0xFFFF;
uint16_t g_ev_row = kNoRow;
uint8_t ev8(size_t v) { return static_cast<uint8_t>(std::min<size_t>(v, 255)); }
// The families, in the default run order (docs/CHARGER_TEST_FLOW.md): OEM vendor modes by company, then USB PD, then the
// legacy / standard D+/D- protocols. Xiaomi / Redmi has no algorithm bit of its own: its rows (the SVID 0x2717 chain row and the
// modes it unlocks) are planned inside the PD unit, so its row follows the PD selection and cannot be toggled alone.
constexpr int kFamHuawei = 0, kFamSamsung = 1, kFamOppo = 2, kFamXiaomi = 3, kFamPd = 4, kFamQc = 5;
constexpr uint32_t kFamilies[] = {
    ct::bit(A::Fcp) | ct::bit(A::Scp),
    ct::bit(A::Afc),
    ct::bit(A::Vooc) | ct::bit(A::SuperVooc) | ct::bit(A::SuperVooc65) | ct::bit(A::Vivo),
    0,
    ct::kPdFixedAlgorithms | ct::kPpsAlgorithms,
    ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Qc3Plus) | ct::bit(A::Qc2ClassB) | ct::bit(A::Qc3ClassB) | ct::bit(A::Ufcs)   // Class B (20 V) is part of the QC family
};
using runlay::kFamilyNames;
// A family name is a Custom / Steps row text (16 px, x28 up to the count tail) and the Steps header title ("<name> steps").
static_assert(all_fit(kFamilyNames, kSmallAdvPx, 112), "a family name is wider than its row");
static_assert(all_fit(kFamilyNames, kLabelAdv, runlay::kLeftW), "a family name is wider than the Running header's left column");
// Tests rows (wizard step 1): 0 Depth, 1..4 the presets, 5 "Custom...", 6 "Last results" once a run exists.
// Custom rows: 0..kFamilyCount-1 the families, then "Done" (row kFamilyCount).
constexpr int kFamilyCount = 6, kDepthRow = 0, kPresetFirst = 1, kPresets = 4, kCustomRow = 5, kViewRow = 6;
// Review: only START (index 0).
constexpr int kStartRow = 0;
// The presets are subsets of "All tests" (the default set, Class B included): PD = the PD family (its Xiaomi rows ride
// along), Legacy = every D+/D- family (OEM vendor modes + QC / UFCS), Vendor modes = the OEM D+/D- families only.
constexpr const char* kPresetNames[kPresets] = {"All tests", "PD only", "Legacy only", "Vendor modes"};
// The 12 px note under the focused row (x28 .. 202, 29 chars).
constexpr const char* kPresetNotes[kPresets] = {"Everything, QC Class B 20V", "PD fixed, PPS, EPR, Xiaomi",
                                                "D+/D-: vendor modes, QC, UFCS", "Huawei, Samsung, OPPO, vivo"};
static_assert(all_fit(kPresetNotes, kLabelAdv, kit::kNoteMaxPx), "a preset note is wider than the row note");
static_assert(all_fit(kPresetNames, kSmallAdvPx, 150), "a preset name is wider than its row");
// The planned re-attach (rdintent::plan_reattach) is announced on the footer of the START confirmation, never as a warning or a
// stage of its own (27 characters at most): the second press goes straight on.
constexpr const char* kReattachConfirm = "Press again, K2 blinks once";
static_assert(fits_px(kSmallAdvPx, kReattachConfirm, 216), "the re-attach confirmation is wider than the footer");
// Depth row's note on Tests (charger_test::Level order: Medium, Light, Deep).
// A build with the PD spec stage (K2_PDSPEC) says so on Deep; the default image has no such stage and keeps the old words.
constexpr const char *kLevelItems[ct::kLevelCount] = {"Light", "Medium", "Deep"};
constexpr int level_segment(ct::Level l) { return l == ct::Level::Light ? 0 : l == ct::Level::Medium ? 1 : 2; }
static_assert(level_segment(ct::next_level(ct::Level::Light)) == 1 && level_segment(ct::next_level(ct::Level::Medium)) == 2 &&
              level_segment(ct::next_level(ct::Level::Deep)) == 0, "OK steps the level to the right and wraps");
static_assert(all_fit(kLevelItems, kSmallAdvPx, 18 * kSmallAdvPx), "a Level word is wider than its value cell");
constexpr const char *kLevelNotes[ct::kLevelCount] = {"Tests each advertised voltage", "Checks what charger offers",
                                                       specsec::kBuilt ? "Sweeps, accuracy & PD spec" : "Sweeps & voltage accuracy"};
static_assert(all_fit(kLevelNotes, kLabelAdv, kit::kNoteMaxPx), "a level note is wider than the row note");
const char *level_note(ct::Level l) { return kLevelNotes[unsigned(l) < ct::kLevelCount ? unsigned(l) : 0]; }
// A row whose engine times its own pulses stops the screen for the length of the handshake (the engine window the LCD idle hook
// does not serve, Runner::paint_blocked; Engine::blocks_paint names the protocols). One Info footer line says why, painted before
// the engine starts and left alone until the window ends. Protocols with no such window say nothing: the QC2 / QC3 / QC3+ handshake
// is served from the idle hook and never stops the screen. Footer text: 27 cells at most.
constexpr const char* kFcpWords = "FCP: handshake";
constexpr const char* kScpWords = "SCP: handshake";
constexpr const char* kVoocWords = "VOOC: listening";
constexpr const char* kUfcsWords = "UFCS: handshake";
constexpr const char* kVivoWords = "VIVO: handshake";
constexpr const char* kHandshakeWords[] = {kFcpWords, kScpWords, kVoocWords, kUfcsWords, kVivoWords};
static_assert(all_fit(kHandshakeWords, kSmallAdvPx, 216), "a handshake footer line is wider than the footer");
const char* handshake_words(fc::Proto p) {
    switch (p) {
    case fc::Proto::Fcp: return kFcpWords;
    case fc::Proto::Scp: return kScpWords;
    case fc::Proto::Vooc: case fc::Proto::SuperVooc: case fc::Proto::SuperVooc65: return kVoocWords;
    case fc::Proto::Ufcs: return kUfcsWords;
    case fc::Proto::Vivo: return kVivoWords;
    default: return nullptr;
    }
}
// The Rd gap of the planned re-attach (measured on the lab recipe: 30 ms is enough for the charger to drop VBUS, and the cable
// look follows the fresh attach) and how long the charger's new contract is awaited before the run goes on regardless.
constexpr uint32_t kReattachGapMs = 30;
constexpr uint32_t kReattachWaitMs = 5000;
// How long the arm waits for the source's own claim (the probe's own bound is 1 s) before the offer's shape decides alone.
constexpr uint32_t kClaimWaitMs = 1000;
constexpr uint32_t kPresetMasks[kPresets] = {
    ct::kDefaultAlgorithms, ct::kDefaultAlgorithms & kFamilies[kFamPd],
    ct::kDefaultAlgorithms & ~kFamilies[kFamPd],
    kFamilies[kFamHuawei] | kFamilies[kFamSamsung] | kFamilies[kFamOppo]};
// The preset row the selection equals (1-based row), 0 = none: "Custom..." carries the pill then.
int preset_row_of(uint32_t selected) {
    for (int n = 0; n < kPresets; ++n) if (kPresetMasks[n] == selected) return n + 1;
    return 0;
}
constexpr int kBestChars = 25;   // the Results best line: 16 px (8 px cells) in the 200 px beside the page margins
using result_layout::kGridMetrics;
static_assert((kFamilies[0] | kFamilies[1] | kFamilies[2] | kFamilies[3] | kFamilies[4] | kFamilies[5]) == ct::kDefaultAlgorithms);
static_assert(kPresetMasks[3] != 0 && (kPresetMasks[3] & ct::kDefaultAlgorithms) == kPresetMasks[3]);
int family(A a) {
    for (int n = 0; n < kFamilyCount; ++n) if (kFamilies[n] & ct::bit(a)) return n;
    return -1;
}
// The family a result step belongs to: the Xiaomi rows are PD requests but read under Xiaomi / Redmi.
int family(const ct::Step& s) { return s.hunt ? kFamXiaomi : family(s.algorithm); }
// The Ra guard reads the lines while the identity can still be picked and START pressed, with the K2 passive.
bool guard_stage(ChargerTestPage::Stage s) { return s == ChargerTestPage::Stage::Cable || s == ChargerTestPage::Stage::Review; }
// A step-1 stage: the Tests list, its Custom sub-list, or one family's Group.
bool tests_stage(ChargerTestPage::Stage s) {
    return s == ChargerTestPage::Stage::Algorithms || s == ChargerTestPage::Stage::Custom || s == ChargerTestPage::Stage::Group;
}
unsigned selected(uint32_t mask) { return std::popcount(mask); }
// The footer line for a refused START. "tool busy" alone does not say which tool or what to do: Ui::busy_reason() does.
// A wrong voltage under a live PDO 1 contract names the measured level (the log line carries the full phrase).
void review_refusal(ct::Reason r, const char* busy, int vbus_mv, StrBuf<32>& out) {
    if (r == ct::Reason::Busy && busy) { out.put(busy); return; }
    char buf[32];
    const size_t n = ct::refusal_footer_text(r, vbus_mv, buf);
    out.put(std::string_view(buf, n));
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
// A caution by tracking (a passing sweep whose worst error passes the pass window, result_layout::deviates): the reason words, "off 450mV".
void put_off(StrBufBase& b, const ctsweep::View& v, const ct::Step& s) {
    b.put("off ").put_u32(result_layout::sweep_worst_mv(v, static_cast<uint8_t>(s.algorithm), s.pos)).put("mV");
}
// pass / fail / skip by verdict; a caution (a Pass that carries Reason::Folded, a voltage fold the sweep found, or a passing sweep
// whose worst error passes 100 mV, result_layout::deviates) is its own count: neither a pass nor a fail. A Capped step stays a fail
// by verdict, as it always counted. The sweep view is the Results grid's: a chip and its counts read one rule.
struct Counts {
    unsigned pass = 0, fail = 0, skip = 0, caution = 0, listed = 0, adv = 0, total = 0;   // listed, adv: a Light run's listing (also in skip)
};
// The steps of the picked grid row (`row`, result_layout::row_has) and of one protocol family (`f`); no row, no family: all of them.
Counts counts(const ct::Runner& runner, const result_layout::CompactRow* row = nullptr, int f = -1) {
    Counts out;
    const auto view = runner.sweep_view();
    for (const auto& s : runner.steps()) {
        if ((row && !result_layout::row_has(*row, s)) || (f >= 0 && family(s) != f)) continue;
        ++out.total;
        const bool caution = result_layout::caution(s, view);
        out.caution += caution;
        out.pass += s.verdict == ct::Verdict::Pass && !caution;
        out.fail += s.verdict == ct::Verdict::Fail;
        out.listed += result_layout::noted(s);
        out.adv += result_layout::advertised(s);
        out.skip += s.verdict == ct::Verdict::Skip;
    }
    return out;
}
// One slice of a grid's paint under the page budget: a whole row when the pass is fresh, else what is left. True when the
// budget wants the rest of the page held back until the next tick.
bool draw_grid_slice(kit::ResultGrid& grid, PaintBudget* pbg) {
    const int room = pbg ? (pbg->spent == 0 ? ChargerTestPage::kFlightRowPx : pbg->left()) : (1 << 30);
    const int drawn = grid.draw_some(room, -1, pbg && pbg->spent != 0);
    if (!pbg) return false;
    pbg->spent += drawn;
    if (grid.pending()) pbg->defer();
    return pbg->more;
}
// A row's footer ("PPS: 3 pass, 1 caution", the Results line's and its Steps page's) and its tone: amber for a failure or a caution,
// green for a pass or an advertised PD offer (a Light run's listing), else plain.
FooterTone put_counts(StrBuf<32>& f, const char* label, const Counts& c) {
    result_layout::FamilyTally t;
    t.pass = c.pass; t.caution = c.caution; t.fail = c.fail; t.listed = c.listed; t.skip = c.skip - c.listed;
    f.put(result_layout::family_footer(label, t).view());
    return c.fail || c.caution ? FooterTone::Caution : c.pass || c.adv ? FooterTone::Ok : FooterTone::Info;
}
}

// The K2 lives on the measured port: a pinned Rd (boot keys) or no host and VBUS at boot (a charger's A port through an A-to-C cable).
static bool self_supplied() { return fusb302::self_powered() || power_source::bus_powered(); }

pd::CableLimits ChargerTestSession::physical_cable() const {
    const auto& now = fusb302::physical_cable();
    if (now.observed) { seen_cable_mv_ = now.max_mv; seen_cable_ma_ = now.max_ma; seen_cable_ = true; return now; }
    if (!seen_cable_) return now;
    pd::CableLimits kept; kept.max_mv = seen_cable_mv_; kept.max_ma = seen_cable_ma_; kept.observed = true;
    return kept;
}

charger_test::Input ChargerTestSession::input(uint32_t now) const {
    charger_test::Input in;
    in.now = now; in.session = fusb302::session(); in.cap_mv = ctx_.cap_mv;
    const auto physical = physical_cable();
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
    in.loop_lock = loopguard::locked();   // the HID-cable loop 5 V lock: refuses the start, ends a run (Reason::Loop)
    in.pd = fusb302::sink_status();
    in.epr_info = &fusb302::epr_info_list();
    in.xm = &fusb302::xiaomi_trace();
    in.fc_caps = &fctrig::caps();
    const auto fs = fctrig::status();
    in.fc_state = fs.state; in.fc_fail = fs.fail; in.fc_tries = static_cast<uint8_t>(fs.tries);
    in.fc_ident = static_cast<uint8_t>(fs.ident);
    in.fc_adapter_mv = fs.adapter_mv; in.fc_held = fs.held;
    in.pd_quiet_ms = fusb302::pd_quiet_ms();
    in.probe_done = fusb302::probe_status().state != pd::ProbeState::Running;
#if K2_PDSPEC
    in.spec_running = ctx_.ui->pd_spec().running();   // the engine's own run (the stage ends when it has finished and released)
#endif
    // D+/D- free for the ceiling stage (only the release before it waits on this).
    if (runner_.state() == ct::State::Releasing) in.lines_free = svooc_trig::lines_free();
    // The SVOOC ceiling stage's view of the trigger (the snapshot is a copy: only while that stage is the state).
    if (runner_.state() == ct::State::Ceiling) {
        const auto sv = svooc_trig::snapshot();
        in.svooc.running = svooc_trig::running();
        in.svooc.judged = svooc_ceiling::ladder_judged(std::span<const svooc::Responder::Ask>(sv.asks.data(), sv.n_asks));
        in.svooc.abort = static_cast<uint8_t>(sv.report.abort);
        in.svooc.commands = static_cast<uint8_t>(std::min<uint32_t>(sv.report.commands, 255u));
        in.svooc_word = sv.ceiling_word; in.svooc_asks = sv.n_asks;
        in.svooc.quiet_ms = sv.quiet_ms;
        in.svooc_vmax_mv = static_cast<uint16_t>(std::clamp<int32_t>(sv.report.vbus_max_mv, 0, 0xFFFF));
        in.svooc_last_ms = sv.log.last_edge_ms;
    }
    return in;
}
// The transition capture records every request that moves the rail off the 5 V default (a PPS / AVS step, a fixed
// 9 V and up): the C135-style shut-offs happen right after an Accept and need VBUS and CC at sub-millisecond
// resolution. arm() is cheap and refuses by itself while another tool owns the overlay.
pd::Err ChargerTestSession::request_pd(pd::Choice c) {
    if (c.pos > 1 || c.mv > 5000) {
        const auto st = transcap::status().state;
        // Not over a capture still being written, nor over one an event of this row froze (its evidence: the step's verdict keeps it).
        if (st == transcap::State::Off || st == transcap::State::Armed) {
            if (transcap::arm(c.pos, c.mv) == transcap::ArmResult::Ok) capture_kept_ = false;   // a new recording: its row may keep it
        }
    }
    return fusb302::sink_request(c, true);
}
fc::Fail ChargerTestSession::request_fc(fc::Proto p, int mv, bool retarget, int ma) {
    const int ufcs_ma = p == fc::Proto::Ufcs ? ma : 0;   // a UFCS row's ma is its Request current (0 = the no-cable 2 A)
    if (!retarget) {
        fc_words_ = handshake_words(p);
        if (fc_words_ && view_) view_->announce(fc_words_);   // before the engine starts: nothing can be painted once it has
    }
    if (retarget) return fctrig::set_target(mv, true, ufcs_ma, p == fc::Proto::Scp ? ma : -1);
    // A PD-silent attach whose source is still announcing (quiet_ms below the caps' stale time) is not a contract; a load or a foreign sink still refuses.
    const bool silent_caps = ct::legacy_guard_override() && link::silent_caps_only_now();
    return fctrig::start(p, mv, true, silent_caps, ma, ufcs_ma);
}
void ChargerTestSession::fc_keep(bool on) { fctrig::keep(on); }
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
// The source's own power claim in whole watts: the Source_Caps_Ext PDP of this attach, 0 when it was never asked or never answered
// (the probe's fallback PDP, the largest PDO, is no claim).
static uint32_t source_claim_w() {
    const auto& e = fusb302::ext().caps_ext;
    return e.valid ? e.pdp_w : 0u;
}
bool ChargerTestSession::probe_identity(bool on) {
    if (on) return fusb302::probe_start();
    fusb302::probe_abort();
    const auto st = fusb302::probe_status();
    const auto r = pd::probe_result(st, fusb302::vdm(), fusb302::ext(), fusb302::caps());
    // After a 5 A identity was read the offer should have grown to the claim: when it did not, the run's evidence says so.
    if (run_cable_.kind != ctcable::Kind::Off && run_facts_.max_ma >= 5000 && source_claim_w() != 0) {
        const uint32_t best = rdintent::best_power_w(fusb302::caps());
        if (best != 0 && source_claim_w() >= best + rdintent::kClaimMarginW)
            K2LOG("[CT] offer still below the claim after the cable read: %u W of %u W\n", unsigned(best), unsigned(source_claim_w()));
    }
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
// The SVOOC ceiling stage (docs/CHARGER_TEST_FLOW.md): the no-load trigger with the Probe responder (the 0x86 ladder asked
// of the adapter, never answered with a draw). `ports_busy` is false: this session is the owner and has released the family
// session before it asks. The trigger's own guards (50 mA, 5 V, the far-sink check, no PD) refuse by themselves.
uint8_t ChargerTestSession::ceiling_start() {
    svooc::Params p;
    p.profile = svooc::Profile::Probe;
    return static_cast<uint8_t>(svooc_trig::start(true, false, svooc_ceiling::kCapS, false, p));
}
void ChargerTestSession::ceiling_stop() { svooc_trig::stop(); }
// The PD spec stage (include/k2/chargertest/spec_section.hpp): the pdspec engine runs inside this test (PdSpecSession::start_in_test:
// the test keeps the ports, the Rd and the run-owner flag) and Ui::poll ticks it after this session's poll.
uint8_t ChargerTestSession::spec_start() {
#if K2_PDSPEC
    // An EPR AVS APDO the run reached (in EPR mode, with its cable) is a window of the stage too: the live list lost it at EPR exit.
    if (const ct::Step* s = ct::reached_epr_avs(runner_.steps())) {
        const pd::Pdo d = pd::decode_pdo(s->pdo);
        return static_cast<uint8_t>(ctx_.ui->pd_spec().start_in_test(ctx_.now, runner_.run_id(), &d, s->pos));
    }
    return static_cast<uint8_t>(ctx_.ui->pd_spec().start_in_test(ctx_.now, runner_.run_id()));
#else
    return 255;
#endif
}
void ChargerTestSession::spec_stop() {
#if K2_PDSPEC
    ctx_.ui->pd_spec().cancel();
#endif
}
void ChargerTestSession::spec_collect(specsec::Result& r) {
#if K2_PDSPEC
    const pdspec::Engine* e = ctx_.ui->pd_spec().engine_for_test();
    if (!e) return;
    const pdspec::Stop st = e->stop();
    specsec::fill_from_aggs(r, e->agg(), st == pdspec::Stop::Done ? specsec::End::Done : st == pdspec::Stop::Cancelled || st == pdspec::Stop::None ? specsec::End::Cancelled : specsec::End::Fault,
                            static_cast<uint8_t>(st), e->release_unconfirmed());
#else
    (void)r;
#endif
}
bool ChargerTestSession::spec_move(const pdspec::ProgMove& mv, uint8_t flags) {
#if K2_PDSPEC
    return ctx_.ui->pd_spec().test_move(runner_.run_id(), mv, flags, ctx_.now);
#else
    (void)mv; (void)flags;
    return false;
#endif
}
bool ChargerTestSession::spec_feeding() {
#if K2_PDSPEC
    return ctx_.ui->pd_spec().feeding();
#else
    return false;
#endif
}
// The stage's result is final: noted for the journal (one small record, written by journal_pump behind the rows and the header, on the
// same write gate), so a reboot after the stage resumes with the section on the grid.
void ChargerTestSession::spec_done(const specsec::Result&) {
#if K2_SPECROWS
    spec_live_run_ = runner_.run_id();   // the engine still holds the records behind the result: the detail page lists them from there
#endif
    if (journal_active_ && !journal_closed_) charger_journal::hw::note_spec(&runner_.spec_apdo());
}
// The Deep transition check: the row's own request armed the recorder (request_pd); the hold's start freezes it kPostMs on, so it
// holds the Accept, the ramp and the first of the hold. Not over a capture an earlier event of this row already froze.
static_assert(transcap::kPostUs == ct::progcheck::kPostMs * 1000, "the runner's hold timing assumes the recorder's post window");
bool ChargerTestSession::prog_begin(const ct::Step&) {
    if (transcap::status().state != transcap::State::Armed) return false;
    transcap::trigger(transcap::Reason::Voltage);   // a hold that then fails keeps this capture as its evidence
    return transcap::triggered();
}
uint8_t ChargerTestSession::prog_take(const ct::Step& s) {
    const auto* rec = transcap::frozen();
    if (!rec) return 0;
    const bool avs = s.algorithm != ct::Algorithm::Pps;
    const auto o = ct::progcheck::measure(*rec, {avs, 0, int32_t(s.mv), ct::progcheck::kSpanMs * 1000, s.pos});
    if (!o.measured) return 0;
    K2LOG("[PCHK] PDO%u %umV flags=%u over=%d band=%d lag=%d out=%d err=%d creep=%d slew=%d\n", unsigned(s.pos), unsigned(s.mv), unsigned(o.flags),
          int(o.over_mv), int(o.band_us), o.lag_known ? int(o.lag_us) : 0, int(o.out_us), int(o.err_mv), o.creep_known ? int(o.creep_mv) : 0, int(o.slew_mv_ms));
    return o.flags;
}
// A row whose verdict stands but whose reason changed afterwards (the sweep closing marks its APDO's fixed rows, Reason::Folded): only
// the journal follows (the row's section is dirty, no flash here); the row's own events and log line went out when it ended.
void ChargerTestSession::amend(const ct::Step& s) {
    const auto idx = static_cast<size_t>(&s - runner_.steps().data());
    if (journal_active_ && !journal_closed_ && idx < runner_.steps().size()) charger_journal::hw::note(runner_.steps(), idx);
}
void ChargerTestSession::record(const ct::Step& s) {
    const auto idx = static_cast<size_t>(&s - runner_.steps().data());
    // The record is a flash write (a section file: an erase and some programs, with interrupts masked): never while a row is in
    // flight, and never inside this call either (the Runner is mid-decision, the PD link may be mid-exchange). It is only noted
    // here (the row's section is marked dirty, no flash); journal_pump() rewrites it, one file per tick, in the gap behind the row
    // once the link is quiet.
    if (journal_active_ && !journal_closed_ && idx < runner_.steps().size()) charger_journal::hw::note(runner_.steps(), idx);
    if (s.verdict == ct::Verdict::Pending) return;   // a withdrawn row a later re-announce brought back: the journal is dirty, no event
    // A failure skips every row behind it in one pass (55 on a full run): those rows are one run event and one log line per
    // range of equal reason, not one each, which overflowed the 16-event ring and the 1 KB log ring (hardware 2026-11-07).
    if (s.verdict == ct::Verdict::Skip) {
        if (idx < runner_.steps().size() && runev::skip(ctx_.now, static_cast<uint16_t>(idx), static_cast<uint16_t>(s.reason)))
            K2LOG("[TEST] rows from %u skipped: %s\n", unsigned(idx), detail_name(s));
        if (idx == g_ev_row) log_row_gap(idx, ctx_.now);   // a row that began and then ended as a skip (QC2 Class B "Class A") still ran
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
    // A deep-sweep row closes with its summary as numbers: the points, the peak and a fold-back (the summary text
    // would ride a %s, which the binlog cuts at 12 bytes - the numbers are the log, the prose is the strip footer).
    if (ct::is_sweep(s))
        if (const auto* rw = runner_.sweep_view().find(uint8_t(s.algorithm), s.pos, ct::sweep_low(s)))
            K2LOG("[SWEEP] PDO%u %u..%umV %u pts %s %s peak=%umV at=%umV fold=%umV from=%umV\n",
                  unsigned(s.pos), unsigned(rw->lo_mv), unsigned(rw->hi_mv), unsigned(rw->n),
                  ct::name(s.verdict), ct::name(s.reason),
                  unsigned(rw->sum.peak_mv), unsigned(rw->sum.peak_at_mv), unsigned(rw->sum.fold_mv), unsigned(rw->sum.fold_from_mv));
    log_row_gap(idx, ctx_.now);
}
// What held the loop while the row was in flight: the longest main-loop iteration (1 ms when healthy) and the longest
// gap between two UI ticks (20 ms when healthy), in ms since the row began. A big number names a blocking path.
// Same window: what painting cost the row (ms inside the lcd stream calls, wire wait included; pixels sent to the panel),
// what the file-system flash writes cost (ms inside page program / sector erase), and the longest continuous time the page
// wanted to paint and could not (an engine window the idle hook does not serve): the screen held still that long.
void ChargerTestSession::log_row_gap(size_t idx, uint32_t now) {
    const paint::Reading pw = paint::meter.window();
    K2LOG("[CT] gap row=%u loop=%ums ui=%ums paint=%ums px=%u fs=%ums blocked=%ums\n", unsigned(idx), unsigned(gap::loop.peak()),
          unsigned(gap::ui.peak()), unsigned(pw.paint_ms), unsigned(pw.px), unsigned(pw.fs_ms), unsigned(gap::blocked.peak(now)));
}
// pattern budget-reannounce-mid-run: the source re-announced its list mid-run and the PD rows were re-planned per row (the row in
// flight finished or was withdrawn, the still-offered rows follow the new list, the dropped ones read "withdrawn by source").
void ChargerTestSession::replan(unsigned follow, unsigned withdrawn, unsigned back) {
    K2LOG("[TEST] re-plan: %u rows follow, %u withdrawn, %u back\n", follow, withdrawn, back);
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
// Behind the row (the Runner out of Requesting/Holding): ONE file per tick, only through charger_journal::write_ok - no PD message
// or VBUS change for kWriteQuietMs and the last Accept / PS_RDY kAfterContractMs old - so the main loop is never away from a
// live exchange (2026-11-08: a 2.2 s begin with a contract up cost the link) nor from the requests a source makes after a
// contract (2026-11-15: a Get_Sink_Cap 226 ms after PS_RDY waited for an arm write, 14.8 ms). A source that never goes quiet
// does not keep the records out for ever: after kJrWaitMs of waiting they go in one per tick regardless. A run that has ended writes without waiting (nothing is being negotiated any more). Nothing ever drains: no call
// writes more than one file, so no tick is longer than the slowest section (about 250 ms with a compaction, 100 to 150 ms usual).
namespace {
constexpr uint32_t kJrWaitMs = 1000;   // past the 95th percentile of the post-contract requests (charger_journal.hpp)
constexpr uint32_t kArmWaitMs = 5000;
// The journal's write gate on the live link (charger_journal::write_ok), `waited` ms after the write began to wait
bool journal_may_write(uint32_t now, uint32_t waited, uint32_t max_wait) {
    const pd::SinkStatus& s = fusb302::sink_status();
    return charger_journal::write_ok(pdtrace::since_activity_ms(), charger_journal::contract_age_ms(now, s.accept_at, s.ps_rdy_at),
                                     waited, max_wait);
}
}
void ChargerTestSession::journal_pump(uint32_t now) {
    if (!journal_active_ || journal_closed_ || arm_) return;
    if (!charger_journal::hw::pending()) { jr_wait_ms_ = 0; return; }
    if (runner_.quiet()) return;
    const bool ended = runner_.state() == ct::State::Done || runner_.state() == ct::State::Stopped;
    if (!ended) {
        if (!jr_wait_ms_) jr_wait_ms_ = now | 1u;
        if (!journal_may_write(now, now - jr_wait_ms_, kJrWaitMs)) return;
    }
    pump_journal();
}
#if K2_PDSPEC && K2_SPECROWS
namespace {
struct SpecRowsCtx { const pdspec::Engine* e; uint32_t run; };
void spec_rows_fill(void* p, size_t off, std::span<uint8_t> out) {
    const auto* c = static_cast<const SpecRowsCtx*>(p);
    specsec::rows_fill(*c->e, c->run, off, out.data(), out.size());
}
static_assert(specsec::kRowsMaxFile <= charger_journal::kSpecRowsMax, "the journal's rows file ceiling holds the engine's cap");
}
#endif
// The journal's pump (K2_SPECROWS: with the spec rows' source): the engine while it still holds this run's records (a rows file is the
// engine's projection written once, behind the spec record; nothing is computed unless the journal writes it).
void ChargerTestSession::pump_journal() {
    namespace cj = charger_journal;
#if K2_PDSPEC && K2_SPECROWS
    const pdspec::Engine* e = spec_live_run_ && spec_live_run_ == runner_.run_id() ? ctx_.ui->pd_spec().engine_for_test() : nullptr;
    SpecRowsCtx c{e, runner_.run_id()};
    // No transitions, no file: a header alone would only cost a flash write and a stale-file drop.
    const cj::SpecRowsSource src{e && specsec::rows_total(*e) ? specsec::rows_file_size(*e) : 0, &spec_rows_fill, &c};
    cj::hw::pump(runner_.run_id(), options_, runner_.evidence(), runner_.steps(), &src);
#else
    cj::hw::pump(runner_.run_id(), options_, runner_.evidence(), runner_.steps());
#endif
}
// The run's journal: rows and header in pieces (hw::pump), one per tick on a quiet link, the Runner held in its Baseline so that
// the first above-5 V request (and the phase marker ahead of it) comes after the header. Then the meta record, one more tick.
void ChargerTestSession::arm_step(uint32_t now) {
    namespace cj = charger_journal;
    if (!arm_) return;
    if (arm_ == 4) { claim_step(now); return; }   // no flash write: the wait is the probe's own, it does not wait for a quiet link
    if (!journal_may_write(now, now - arm_t0_, kArmWaitMs)) return;
    if (arm_ == 1) {
        pump_journal();
        if (!cj::hw::working()) arm_ = 2;
        return;
    }
    if (arm_ == 2) {
        cj::hw::meta(arm_meta_);
        K2LOG("[TEST] journal armed in %u ms\n", unsigned(now - arm_t0_));
        if (reattach_ask_) { arm_ = 4; arm_t0_ = now; claim_asked_ = false; return; }   // the claim decides whether the K2 blinks
        if (reattach_plan_) { arm_ = 3; return; }   // decided at START: the marker and the gap are the next quiet tick's, one flash write per tick
        arm_ = 0;
        runner_.hold(false);
        return;
    }
    arm_ = 0;
    reattach_begin(now);
}
// The blink is decided here, from the source's own claim. The Runner is held in its Baseline, so the quick probe (Source_Caps_Ext and
// the rest) is ours to ask: it answers in under a second or the wait ends (kClaimWaitMs) and the offer's own shape decides
// (rdintent::cable_limited_no_claim). A claim already in hand from this attach (a re-read) is used as it is.
void ChargerTestSession::claim_step(uint32_t now) {
    const bool have = source_claim_w() != 0;
    const bool timed_out = now - arm_t0_ >= kClaimWaitMs;
    if (!have && !timed_out) {
        if (!claim_asked_) claim_asked_ = fusb302::probe_start();   // refused while the sink is busy: asked again next tick
        if (!claim_asked_ || fusb302::probe_status().state == pd::ProbeState::Running) return;
    }
    if (claim_asked_) fusb302::probe_abort();
    claim_asked_ = false;
    decide_reattach();
    if (reattach_plan_) { arm_ = 3; return; }   // the marker and the gap are the next quiet tick's: one flash write per tick
    arm_ = 0;
    runner_.hold(false);
}
// The blink's verdict: the static conditions held at START (reattach_ask_), the source's offer decides now.
void ChargerTestSession::decide_reattach() {
    const uint32_t claim = source_claim_w();
    const bool held = rdintent::offer_held_by_cable(fusb302::caps(), claim);
    reattach_ask_ = false;
    reattach_plan_ = held && rdintent::plan_reattach(run_cable_, run_facts_, fusb302::pin_rd(), fusb302::self_powered(), true);
    K2LOG("[CT] re-attach decision: claim %u W, best offer %u W, nothing above 3 A=%u -> %s\n", unsigned(claim),
          unsigned(rdintent::best_power_w(fusb302::caps())), unsigned(!rdintent::offers_above_ceiling(fusb302::caps())),
          reattach_plan_ ? "blink" : "no blink");
}
// The planned re-attach (the K2 lives on the charger's VBUS and the charger skipped the cable at its attach): the run is journaled,
// the identity armed. The intent marker goes to the volume first, then the Rd drops for kReattachGapMs with the Ra kept. The
// charger sees a detach and cuts VBUS, which the K2 does not outlive (about 165 ms dark); the comeback boot keeps the Rd and the
// Ra from its first instruction and the journal resumes the run. A K2 that does outlive it (a host on the other port) goes on
// as it is once the charger's new contract is up (reattach_step). The Runner stays held until then.
void ChargerTestSession::reattach_begin(uint32_t now) {
    if (!rdcomeback::begin_reattach(run_cable_)) {
        K2LOG("[TEST] planned re-attach skipped: the intent marker is not on the volume\n");
        reattach_plan_ = false;
        runner_.hold(false);
        return;
    }
    fusb302::rd_glitch_begin();
    reattach_ = 1; reattach_t0_ = now;
    K2LOG("[TEST] planned re-attach: Rd off for %u ms, cable %u/%u\n", unsigned(kReattachGapMs), unsigned(run_cable_.kind), unsigned(run_cable_.seq));
}
void ChargerTestSession::reattach_step(uint32_t now) {
    if (reattach_ == 1) {
        if (now - reattach_t0_ < kReattachGapMs) return;
        fusb302::rd_glitch_end();
        reattach_ = 2; reattach_t0_ = now;
        return;
    }
    if (reattach_ != 2) return;
    // rd_glitch_end forgot the old contract, so Ready here is the charger's new one: this K2 outlived the gap.
    const bool ready = fusb302::sink_status().state == pd::SinkState::Ready;
    if (!ready && now - reattach_t0_ < kReattachWaitMs) return;
    K2LOG("[TEST] planned re-attach: %s after %u ms, the run goes on\n", ready ? "new contract" : "no new contract", unsigned(now - reattach_t0_));
    reattach_drop();
    runner_.hold(false);
}
// The planned re-attach is over (or the run ended inside it): the Rd is given back if the gap is still open and the marker is the
// follower's again.
void ChargerTestSession::reattach_drop() {
    if (reattach_ == 1) fusb302::rd_glitch_end();
    if (reattach_) rdcomeback::end_reattach();
    reattach_ = 0;
    reattach_plan_ = false;
}
// The run ended before its plan was on flash: the header goes in last, so no run of this one is there to resume, and there is
// nothing to close either.
void ChargerTestSession::arm_abort() {
    charger_journal::hw::cancel();
    arm_ = 0; journal_active_ = false; reattach_plan_ = false; reattach_ask_ = false; claim_asked_ = false;
    runner_.hold(false);
    K2LOG("[TEST] journal arm abandoned: the run ended before its plan was written\n");
}
// The run ends for good: Finish goes into the journal FIRST, then the Runner writes the skip rows. A reset (or a flash that gives
// up) between those rows leaves a closed run with a few rows still Pending, never a stopped run that the next boot resumes.
// A suspended run never comes here (it is only interrupted, and resumes).
void ChargerTestSession::closing(ct::Reason r) {
    if (!journal_active_ || journal_closed_) return;
    if (arm_ == 1) { arm_abort(); return; }
    if (arm_ >= 2) {
        if (arm_ == 4 && claim_asked_) fusb302::probe_abort();
        arm_ = 0; reattach_plan_ = false; reattach_ask_ = false; claim_asked_ = false; runner_.hold(false);
    }   // the header is in; the meta record of a run that ends here is not worth a write
    // Nothing is drained: the section files still to be written are moot for a run that is over (Finish closes it, a closed
    // run is never resumed, and no write here may take more than its own one file).
    charger_journal::hw::cancel();
    charger_journal::hw::finish(static_cast<uint8_t>(ct::State::Stopped), static_cast<uint8_t>(r));
    journal_closed_ = true;
}
// A failed step freezes the capture and asks for it to be stored with the run; a passed step just ends the
// recording (it is overwritten by the next arm). Skip rows (the followers of a failure) change nothing.
void ChargerTestSession::note_transition(const ct::Step& s) {
    // The recorder goes back to free for whatever step follows (the PD spec section's captures included): an armed or frozen
    // capture that no row keeps would hold it for the rest of the run (hardware 2026-11-17).
    if (s.verdict == ct::Verdict::Pass) { transcap::disarm_if_armed(); transcap::release_unkept(); return; }
    if (s.verdict != ct::Verdict::Fail) return;
    const transcap::Reason r = ct::capture_reason(s.reason);
    if (r == transcap::Reason::None) { transcap::disarm_if_armed(); transcap::release_unkept(); return; }
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
    // A comeback boot armed the recorded identity before there was a page. When no run takes it over (the journal is gone, closed
    // or cancelled, and the boot did not arm the selection) it ends here, with its Ra; the Rd stays (the comeback's judge decides).
    if (!resume && !dialog_spoof_) rdcomeback::drop_identity();
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
        // A comeback boot whose marker said Reattach (the planned re-attach) resumes without counting a resume.
        const bool planned = rdcomeback::planned_reattach();
        runner_.restore(r.options, r.evidence, r.run_id, std::span<const ct::Step>(r.steps.data(), r.count),
                        r.phase, r.phase_index, r.resumed, r.attempt_index, r.attempts, planned);
        runner_.spec_apdo_restore(r.spec_apdo);
        if (planned) K2LOG("[TEST] planned re-attach boot: the resume is not counted as a power loss\n");
        resume_pending_ = runner_.resumable() && !cancel;
        auto_resume_ = resume_pending_;   // every interrupted run resumes by itself once VBUS has been steady (BACK at boot cancels)
        self_run_ = r.has_meta && r.meta.unattended;   // unattended is what self_powered() was when the run started
        run_guard_ = r.has_meta ? r.meta.guard : uint8_t(0);   // the guard's reading at START stays with the run (its card)
        resume_cable_ = r.has_meta ? ctcable::Choice{static_cast<ctcable::Kind>(r.meta.cable_kind), r.meta.cable_seq} : ctcable::Choice{};
        // The run's identity rides in the intent marker from this boot on (a second power loss before the resume re-arms it).
        if (resume_pending_) rdcomeback::set_run_cable(resume_cable_);
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
    // chosen(), not selected(): the default (EPR spoof) is START's, a boot with nothing ever picked arms nothing and stays passive.
    const ctcable::Choice c = resume_pending_ ? resume_cable_ : ctx_.cables.chosen();
    in.identity = c.kind != ctcable::Kind::Off && ctcable::valid(c);
    in.journaled = resume_pending_ && in.identity;
    in.inline_sink = fusb302::inline_mode();
    in.pinned_rd = fusb302::pin_rd();
    // A comeback boot (k2/core/rd_comeback.hpp) has had its own Ra on CC1 since its first instruction, so CC1 reads low: that is not a
    // cable in between, and the run's START already passed the guard (journaled): no reading, the journaled identity arms. A Rescan
    // comeback (the Connection page's Refresh) never had an Ra and was never a run: its CC1 reading is a real one.
    const bool comeback = rescanbridge::supports_run_resume(rdcomeback::found().kind);
    if (!(comeback && in.journaled)) {
        if (const auto a = adc::read()) { in.cc1_mv = a->cc1_mv; in.cc2_mv = a->cc2_mv; }
    }
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
    if (r != ct::Reason::None) { auto_resume_ = false; message(ct::refusal_words(r)); return; }
    resume_pending_ = false; auto_resume_ = false;
    resume_rd_ = resume_hold_ = false;   // the Rd (if we presented it) belongs to the run now; release_run() ends it with the run
    wait_ = pd::VbusWait();
    capture_identity();
    fusb302::set_run_owner(true);
    charger_journal::hw::resumed(runner_.resumed());
    g_ev_row = kNoRow;
    gap::blocked.begin_run(now);
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
    freeze_facts();
    set_stage(Stage::Results);
}
void ChargerTestSession::capture_identity() {
    card_id_ = {}; card_no_ = 0; delta_n_ = 0; mfr_[0] = 0; card_name_[0] = 0;
    facts_ = {};   // a new run: its facts freeze when it ends
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
// Out of line: the card under construction is a large local, and LTO inlining it into poll() (it did once the main loop moved
// to its own unit) put poll()'s frame over the 1 KB budget.
[[gnu::noinline]] void ChargerTestSession::save_card() {
    card_no_ = 0; delta_n_ = 0;
    if (runner_.state() != ct::State::Done) return;
    if (card_id_.fp == 0) card_id_.fp = report_card::legacy_fingerprint(runner_.steps());   // no PD caps: identify by the legacy answers
    // V5: the cable the run went through — the physically read e-marker, else the physical limits, and the
    // spoofed library identity when one was armed (its limits are what the run actually used).
    report_card::Cable cable;
    const auto& em = fusb302::emarker();
    const auto pc = physical_cable();
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
    if (runner_.reattach_count()) cable.flags |= report_card::kCableReattach;   // the run went through the planned re-attach (its reboot is not a resume)
    // A fresh card per run (the fields build_into does not set: name, card_no, runs, ...); the NSDMI values
    // (format, times) come back through the default constructor.
    card_ = report_card::Card{};
    report_card::build_into(card_, runner_.steps(), runner_.evidence(), runner_.handover(), card_id_,
                            report_card::hw::context(), runner_.state(), runner_.reason(), cable, runner_.resumed(),
                            runner_.sweep_view());
    card_.level = runner_.level();             // the card names a Light / Deep run (v8 spare byte)
    report_card::set_auto_name(card_, mfr_);   // "<Brand> <PDP>W"; hw::put keeps a name the user gave
    // The run's block (a Deep run's spec summary, SVOOC ceiling and sweep graphs; the Xiaomi section of any run whose chain row ran) is
    // built now, while the sweep is in RAM, into the share buffer (no share view is open under a finishing run); it is stored beside
    // the card and the share link carries it.
    static_assert(ShareRecord::kBuffer >= report_card::deep::kMaxBlob, "build() needs the whole worst-case block");
    const std::size_t blk = report_card::deep::build(card_, runner_.steps(), runner_.evidence(), runner_.sweep_view(), ctx_.ui->share_buffer(),
                                                       &runner_.spec_apdo());
    save_card_store(std::span<const uint8_t>(ctx_.ui->share_buffer()).first(blk));
}
// save_card's tail, on its own frame: it holds the charger's previous Card (292 B) that save_card used to keep
// beside the new one — with both on save_card's stack the frame was stack-gated (tools/stack_depth.py). The one
// previous Card is filled by the store's own read before the write (after put the file holds the new card) and
// handed back, so the store keeps none of its own; the delta is taken after the write.
[[gnu::noinline]] void ChargerTestSession::save_card_store(std::span<const uint8_t> blk) {
    report_card::Card prev;
    bool had = false;
    const auto err = report_card::hw::put(card_, prev, had, blk);
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
        // Rd already on: a finished re-read's attach is still lingering. The run takes the Rd and the contract over
        // now (rd_for_run(true) clears the re-read's ownership), so its linger cannot release them mid-run.
        if (fusb302::rd_for_run_on()) fusb302::rd_for_run(true);
        K2LOG("[TEST] START without Rd: pm=%d rd_on=%d inline=%d vbus=%d mV\n", int(ctx_.pm_online), int(fusb302::rd_for_run_on()),
              int(ctx_.inline_mode), int(ctx_.vbus_fast_mv));
        return false;
    }
    if (!present_rd(ctx_.now)) return false;
    // The wait is a stage of its own, carried by poll(): the main loop keeps running (the PD engine's service() every
    // millisecond, the UI tick every 20 ms) and BACK cancels. A blocking loop here froze the UI for up to 21 s.
    fusb302::set_run_owner(true);
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
        // The log says what the cable turned out to be, not only whether it was read: a chip that stayed silent (pull-down
        // seen, no answer) is not "no e-marker". Log only: the limits still treat an unread cable as unread (cable_observed).
        K2LOG("[TEST] START e-marker scan (VCONN, VBUS %d mV): %s, cable %s %u mV %u mA\n", int(ctx_.vbus_fast_mv),
              found ? "found" : "no answer",
              cablefact::name(cablefact::classify(cable.observed, fusb302::last_cable_read(), fusb302::pull_down())),
              unsigned(cable.max_mv), unsigned(cable.max_ma));
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
void ChargerTestSession::runner_tick(uint32_t now) { runner_.poll(input(now), fusb302::caps()); }
void ChargerTestSession::poll(uint32_t now) {
    // The run owns the line in every phase, the unit cycle's Rd-off / VBUS-dark gaps included: the idle wake (a sleeping-port
    // helper) must never read them as a sleeping port (bench 2026-11-10: every PD row "no PD after re-attach" with the setting on).
    // Derived each tick, so no path out of a run can leave it set; release_run() clears it at once.
    fusb302::set_run_owner(resume_pending_ || stage_ == Stage::Waiting || runner_.busy());
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
    // The + key is wired to the CC pins and wakes a sleeping charger when held: while it is down in Waiting (and 2 s after) the
    // lines it pulls go to the log, so the shell sees what the hold did (CC1 / CC2 / VBUS, ~10 Hz, fixed phrase + %d numbers).
    {
        using Act = waitplus::PlusWatch::Act;
        const Act act = plus_watch_.step(now, stage_ == Stage::Waiting, ctx_.plus_key);
        if (act != Act::None) {
            const int cc1 = ctx_.analog ? ctx_.analog->cc1_mv : -1, cc2 = ctx_.analog ? ctx_.analog->cc2_mv : -1;
            const int vbus = static_cast<int>(ctx_.vbus_fast_mv);
            const int ms = static_cast<int>(plus_watch_.held_ms(now));
            if (act == Act::Press) K2LOG("[WAKE] + pressed: CC1 %d mV CC2 %d mV VBUS %d mV\n", cc1, cc2, vbus);
            else if (act == Act::Release) K2LOG("[WAKE] + released after %d ms: CC1 %d mV CC2 %d mV VBUS %d mV\n", ms, cc1, cc2, vbus);
            else K2LOG("[WAKE] t=%d ms key=%d CC1 %d mV CC2 %d mV VBUS %d mV\n", ms, plus_watch_.down() ? 1 : 0, cc1, cc2, vbus);
        }
    }
    if (stage_ == Stage::Waiting) wait_step(now);
    // The confirm window ran out without the second START press: nothing was touched (no Rd, no identity), so only the window goes.
    if (confirming_ && static_cast<int32_t>(now - confirm_until_) >= 0) invalidate_confirmation();
    if (guard_stage(stage_) && static_cast<int32_t>(now - guard_t_) >= static_cast<int32_t>(raguard::kSampleMs)) guard_sample(now);
    // A remote START is confirmed like the second OK, once the Ra guard holds a full window of passive readings (~0.8 s, as long
    // as a person takes at the least) and the window still describes the proposal it opened on.
    if (remote_confirm_ && guard_.count() >= raguard::PeakWindow::kSamples) {
        remote_confirm_ = false;
        if (stage_ == Stage::Review && confirm_live()) { K2LOG("[TEST] remote START confirmed\n"); confirmed_start(); }
        else invalidate_confirmation();
    }
    if (runner_.attaching()) fusb302::attach_scan();   // Rd is back on: notice the charger's Rp (START's blocking loop does the same)
    const auto before = runner_.state();
    // The journal's own plan write (still arming) goes before the Runner: it is held in Baseline until the header is on flash.
    if (arm_) arm_step(now);
    if (reattach_) reattach_step(now);
    runner_tick(now);
    // A live run: the Black Box writes nothing but the rare-event captures (hard reset, detach, refusal) until the run is over;
    // every save stalls the main loop 100-250 ms. Renewed each tick, it also covers the arm.
    if (journal_active_ && !journal_closed_) bbpersist::hold_run();
    journal_pump(now);   // out of the row: the held-back records go in now, one file per tick on a quiet link
    // The row that just began (the Runner has no hook for it): its first pass as Running names it for the live stream.
    if (const auto idx = runner_.index(); idx < runner_.steps().size() && idx != g_ev_row && runner_.steps()[idx].verdict == ct::Verdict::Running) {
        const auto& s = runner_.steps()[idx];
        g_ev_row = static_cast<uint16_t>(idx);
        gap::loop.begin_window(); gap::ui.begin_window(); paint::meter.begin_window();   // the row's own longest gaps and paint cost (read by record())
        gap::blocked.begin_window(now);
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
        // The state's full name is fixed text: it belongs in the format string, not in a %s (the binlog cuts %s at 12 bytes).
        const char* reason_name = ct::name(runner_.reason());
        switch (runner_.state()) {
        case ct::State::Idle:       K2LOG("[TEST] Setup %s\n", reason_name); break;
        case ct::State::Baseline:   K2LOG("[TEST] Verify 5V %s\n", reason_name); break;
        case ct::State::Requesting: K2LOG("[TEST] Negotiating %s\n", reason_name); break;
        case ct::State::Holding:    K2LOG("[TEST] Hold %s\n", reason_name); break;
        case ct::State::Releasing:  K2LOG("[TEST] Return to 5V %s\n", reason_name); break;
        case ct::State::Done:       K2LOG("[TEST] Complete %s\n", reason_name); break;
        case ct::State::Stopped:    K2LOG("[TEST] Stopped %s\n", reason_name); break;
        case ct::State::Unsafe:     K2LOG("[TEST] UNSAFE: unplug source %s\n", reason_name); break;
        case ct::State::Detach:     K2LOG("[TEST] Detach charger %s\n", reason_name); break;
        case ct::State::Attach:     K2LOG("[TEST] Initializing... %s\n", reason_name); break;
        case ct::State::Ceiling:    K2LOG("[TEST] SVOOC ceiling %s\n", reason_name); break;
        case ct::State::Spec:       K2LOG("[TEST] PD spec %s\n", reason_name); break;
        }
        if (runner_.state() == ct::State::Done) save_card();
        if (runner_.state() == ct::State::Done || runner_.state() == ct::State::Stopped) {
            emit_end(now);
            release_run();   // the run is over: passive again (Ra mode and the dialog's spoof end with it)
            transcap::disarm_if_armed(); transcap::release_unkept();
            freeze_facts();   // the Results grid is the run's, whatever the source does after this
            set_stage(Stage::Results);
        }
    }
}
void ChargerTestSession::invalidate_confirmation() {
    if (confirming_ && view_) view_->on_confirmation_dropped();   // the footer and the START cell show the confirmation
    if (remote_confirm_) K2LOG("[TEST] remote START dropped: the confirmation closed\n");
    confirming_ = false; confirm_until_ = 0; confirm_proposal_ = {}; remote_confirm_ = false;
}
// The remote START: Review (the "All tests" preset first when asked), then the first press's path. A refusal there is the
// device's own (start_failed: the log line, the reason recorded on the runner, the footer words) and comes back at once.
ChargerTestSession::RemoteStart ChargerTestSession::remote_start(bool all_tests) {
    RemoteStart r;
    if (start_held()) { r.result = RemoteStart::Result::Busy; return r; }
    if (all_tests) options_.selected = kPresetMasks[0];
    if (stage_ != Stage::Review) set_stage(Stage::Review, kStartRow);
    else { invalidate_confirmation(); selection_ = kStartRow; }
    open_confirmation();
    if (!confirming_) {
        r.result = RemoteStart::Result::Refused;
        r.reason = runner_.reason(); r.refusal_mv = runner_.refusal_mv();
        return r;
    }
    remote_confirm_ = true;   // poll() confirms it once the Ra guard's window is full
    K2LOG("[TEST] remote START: confirm window open, selected=%u units=%u\n", unsigned(options_.selected), unsigned(options_.units.n));
    return r;
}
// BACK's order: a resume offer, a live run, the confirm window, the wait, the Refuse warning.
ChargerTestSession::Stopped ChargerTestSession::stop() {
    if (stage_ == Stage::Resume) { abandon_run(); return Stopped::Resume; }
    if (runner_.busy()) { cancel(); return Stopped::Run; }
    if (confirming_) { invalidate_confirmation(); return Stopped::Confirmation; }   // nothing was touched
    if (stage_ == Stage::Waiting) { wait_abort(); return Stopped::Wait; }           // Rd back, nothing started
    if (stage_ == Stage::Refuse) { set_stage(Stage::Review, kStartRow); return Stopped::Refusal; }   // nothing was touched
    return Stopped::Nothing;
}
void ChargerTestSession::cancel() {
    if (stage_ == Stage::Waiting) { wait_abort(); return; }   // release_run() and the stage are its business
    if (resume_pending_ && resume_rd_) return;   // a page change keeps the resume's Rd: the resume goes on in the background, only BACK on its page ends it
    const auto before = runner_.state();
    runner_.cancel(input(ctx_.now)); invalidate_confirmation();
    if (runner_.state() != before && (runner_.state() == ct::State::Done || runner_.state() == ct::State::Stopped)) emit_end(ctx_.now);
    release_run();
    transcap::disarm_if_armed(); transcap::release_unkept();
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
    reattach_drop();   // a run that ends inside the planned re-attach gives the Rd back and the marker to the follower first
    rdcomeback::set_run_cable({});   // no run: the intent marker follows a session without an identity
    fusb302::set_run_owner(false);   // the sleeping-port wake may act again
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
int ChargerTestPage::spec_n() const { return s_.runner_.evidence().spec.shown() ? static_cast<int>(specsec::shown_n(split_)) : 0; }
size_t ChargerTestPage::spec_cell(int k) const { return k < 0 ? specsec::kCells : specsec::shown_cell(split_, static_cast<size_t>(k)); }
// The one way SpecDetail reads its check: false = nothing to show (no stage, a cell out of range, a range the run no longer holds).
bool ChargerTestPage::spec_open(pdspec::Check &c, specsec::Cell &cell) const {
    const specsec::Result &r = s_.runner_.evidence().spec;
    if (!r.shown()) return false;
    if (s_.spec_pos_) {
        const specsec::Apdo *p = s_.runner_.spec_apdo().find(s_.spec_pos_);
        c = p ? specsec::apdo_check(p->avs, s_.spec_slot_) : pdspec::Check::Count;
        if (c == pdspec::Check::Count) return false;
        cell = specsec::slot_cell(r, *p, s_.spec_slot_);
        return true;
    }
    if (s_.detail_index_ < 0 || s_.detail_index_ >= static_cast<int>(specsec::kCells)) return false;
    c = specsec::kOrder[static_cast<size_t>(s_.detail_index_)];
    cell = r.cells[static_cast<size_t>(s_.detail_index_)];
    return true;
}
const specsec::Apdo *ChargerTestPage::spec_range() const {
    if (s_.stage_ != Stage::Steps || !s_.spec_range_ || !s_.runner_.evidence().spec.shown()) return nullptr;
    return s_.runner_.spec_apdo().find(s_.spec_range_);
}
uint8_t ChargerTestPage::line_range_pos(int line) const {
    if (line < 0 || static_cast<size_t>(line) >= line_n_) return 0;
    const auto ln = lines_[static_cast<size_t>(line)];
    if (layout_.rows[ln.row()].kind != result_layout::CompactKind::Pd) return 0;
    result_layout::RowTokens rt;
    result_layout::row_tokens(layout_, ln.row(), s_.runner_.steps(), s_.runner_.sweep_view(), rt);
    for (int k = ln.first(); k < ln.first() + ln.count(); ++k)
        if (rt.toks[static_cast<size_t>(k)].kind) return static_cast<uint8_t>(rt.toks[static_cast<size_t>(k)].mv);
    return 0;
}
int ChargerTestPage::range_spec_n() const {
    const specsec::Apdo *p = spec_range();
    return p ? static_cast<int>(specsec::slot_n(p->avs)) : 0;
}
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
        if (const char* busy = ctx_.ui->busy_reason()) {
            K2LOG("[TEST] cable arm refused: %s\n", busy);   // the footer alone was invisible to ct_start.py (2026-11-10, 72c2e28)
            message(busy);
            return false;
        }
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
// The limits of a choice that is not armed yet (the planned re-attach is announced before the START arms anything).
__attribute__((noinline)) static bool selected_facts(ctcable::Choice c, ctcable::Facts& f) {
    c = ctcable::canonical(c);
    if (!ctcable::valid(c) || c.kind == ctcable::Kind::Off) return false;
    if (c.kind == ctcable::Kind::Builtin) { f = ctcable::facts(emrec::builtin_cable(c.seq)); return true; }
    emlib::Entry e;
    emk_quirk::Quirks quirks;
    if (!storage::lib_get(c.seq, e, quirks)) return false;
    f = ctcable::facts(e.rec);
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
    // One reading inside the fall of a USB-A port's auto-off cut (1 V up to vSafe5V) is not a refusal: START's own check waits for it.
    const bool in_cut = preview.refusal == ct::Reason::Release && ct::dip_reading(in.vbus_mv);
    if (preview.refusal != ct::Reason::None && !later && !in_cut) {
        start_failed(preview.refusal, ctx_.ui->busy_reason(), in.vbus_mv);   // the log line + footer + recorded reason
        return;
    }
    // The same rule begin_run applies to the identity the START arms: the footer says so when the K2 is going to blink off.
    const auto sel = ctx_.cables.selected();
    ctcable::Facts fx;
    // The footer promises the blink only when the facts are in hand: the source's claim (Source_Caps_Ext) was read in this attach.
    // The preview touches nothing, so a claim it does not have is not asked for here; the run asks it in its arm (claim_step).
    confirm_reattach_ = selected_facts(sel, fx) && source_claim_w() != 0 &&
        rdintent::plan_reattach(sel, fx, fusb302::pin_rd(), fusb302::self_powered(),
                                rdintent::cable_limited(fusb302::caps(), source_claim_w()));
    confirming_ = true; confirm_until_ = ctx_.now + Ui::kConfirmWindowMs; confirm_proposal_ = proposal(in);
}
// The second press: an identity needs the charger directly on the plug (include/k2/pd/ra_guard.hpp), so the guard judges
// the lines first and a refusal opens the Refuse stage; then the identity is armed — whatever was armed is disarmed
// first and the pick takes over (Off arms nothing) — and the start goes on with the Rd and the wait.
void ChargerTestSession::confirmed_start() {
    const bool skip_spoof = skip_spoof_run_;   // "Start, skip spoof" on the Refuse stage: this run without the identity
    skip_spoof_run_ = false;                  // the stored pick stays; only this run carries no identity
    const auto choice = skip_spoof ? ctcable::Choice{} : ctx_.cables.selected();
    const bool bypass = guard_bypass_;   // "Start anyway" on the Cable-connected warning: the identity runs, the reading is recorded
    guard_bypass_ = false;
    run_guard_ = 0;
    if (choice.kind != ctcable::Kind::Off) {
        topology_ = guard_topology(ctx_.now);
        const bool refused = !raguard::spoof_allowed(topology_);
        if (refused && !bypass) { set_stage(Stage::Refuse); return; }
        run_guard_ = static_cast<uint8_t>((1 + static_cast<uint8_t>(topology_)) | (refused ? 0x80 : 0));
        if (refused) K2LOG("[GUARD] started anyway against a %s reading\n", raguard::name(topology_));
    }
    // A self-powered K2 is attached to the charger before it can show a cable, and a charger that reads the e-marker does so at its
    // attach: no warning for that. The run goes straight on; begin_run plans the re-attach when it pays (plan_reattach).
    if (apply_cable(choice)) press_start();
    else invalidate_confirmation();   // refused (message on the footer): the next OK is a first one again
}
// After the identity is settled: Rd (and the wait for VBUS) as the last stage, then the run.
void ChargerTestSession::press_start() {
    bbpersist::make_room();   // the Black Box ring's spare sector is erased now, not inside the run (its rare captures need it)
    if (start_begin()) { wait_step(ctx_.now); return; }   // Waiting; a charger already up ends it on this very call
    launch();
}
// Back to a passive K2 on Review with the reason on the footer: nothing started.
void ChargerTestSession::start_failed(const char* why) {
    release_run();
    if (stage_ != Stage::Review) set_stage(Stage::Review, kStartRow);   // from Waiting; set_stage clears the message, so it comes after
    message(why);
}
// A START the preflight refused: one [TEST] log line with the reason (a remote START, tools/ct_start.py, prints it
// when the run id stays 0), the reason + measured VBUS recorded on the runner (the remote summary reads them), and
// the footer words on screen.
void ChargerTestSession::start_failed(ct::Reason r, const char* busy, int vbus_mv) {
    // The log line names the holding tool when busy, and what latched the inline state on "load attached".
    const pd::InlineCause cause = ctx_.ui->link().inline_cause();
    const char* detail = r != ct::Reason::Inline ? busy : cause != pd::InlineCause::None ? pd::inline_cause_name(cause) : nullptr;
    runner_.note_preflight_refusal(r, vbus_mv, detail);
    StrBuf<32> t;
    review_refusal(r, busy, vbus_mv, t);
    start_failed(t.c_str());
}
// The preflight on the final reading (the wait changed it), then the run itself.
void ChargerTestSession::launch() {
    const auto in = input(ctx_.now);
    const auto preview = ct::Runner::preview(fusb302::caps(), options_, in);
    // A reading inside a port cut's fall is waited for by the runner's own 5 V check (Runner::start), not refused here.
    const bool in_cut = preview.refusal == ct::Reason::Release && ct::dip_reading(in.vbus_mv);
    if (preview.refusal != ct::Reason::None && !in_cut) {
        start_failed(preview.refusal, ctx_.ui->busy_reason(), in.vbus_mv);   // the wait ended in a refusal: the log says why
        return;
    }
    begin_run();
}
void ChargerTestSession::begin_run() {
#if K2_PDSPEC
    ctx_.ui->pd_spec().yield_record();
#endif
    seen_cable_ = false;   // a new run reads its own cable
    auto in = input(ctx_.now);
    // VBUS up with no Rp anywhere on the line: a USB-A style port, whose output a detach never turns off.
    in.no_cc = in.vbus_mv >= pd::kVSafe0Mv && !in.self_powered && !fusb302::rp_on_line();
    journal_active_ = false; capture_kept_ = false; reattach_plan_ = false; reattach_ask_ = false; reattach_ = 0;
    capture_identity();
    const auto r = runner_.start(fusb302::caps(), options_, in, true);
    K2LOG("[TEST] start selected=%u units=%u level=%s -> %s\n", unsigned(options_.selected), unsigned(options_.units.n),
          ct::name(options_.level), ct::name(r));
    charger_journal::RunMeta meta;
    meta.units = options_.units;
    meta.pps_sweep = options_.pps_sweep;   // a resumed run re-plans the sweep rows from the journaled options
    meta.avs_sweep = options_.avs_sweep;
    meta.level = options_.level;           // a resumed Light run stays Light (never replanned as Medium)
    meta.unattended = self_supplied();   // the K2 runs on the measured port (pinned Rd or bus-powered): the run never detaches
    self_run_ = meta.unattended;
    meta.cable_kind = static_cast<uint8_t>(run_cable_.kind); meta.cable_seq = run_cable_.seq;
    meta.guard = run_guard_;   // the Ra guard's reading when the identity was started (and whether the user went on anyway)
    options_.units = {};   // an explicit unit list serves one run; the next START uses the default order
    invalidate_confirmation();
    if (r == ct::Reason::None || runner_.state() == ct::State::Unsafe) {
        fusb302::set_run_owner(true);   // from the START itself, not from the next tick
        g_ev_row = kNoRow;
        gap::blocked.begin_run(ctx_.now);
        runev::emit(runev::Code::Start, ctx_.now, ev8(meta.units.n), ev8(runner_.steps().size()), 0);
        // The plan goes to flash in pieces, one file per tick on a quiet link (a synchronous begin held the main loop 1.5-2.2 s with
        // the charger's contract up, 2026-11-08: no GoodCRC, hard reset). The Runner waits in its Baseline for it (hold), so no
        // request, and no phase marker, comes before the header; the meta record follows the header.
        // The identity rides in the intent marker for the whole run (a charger Hard Reset or a lost VBUS brings the K2 back as the
        // same sink, k2/core/rd_comeback.hpp). The planned re-attach comes after the journal is armed, before any row.
        rdcomeback::set_run_cable(run_cable_);
        // Whether the blink pays depends on the source's own claim: in hand from this attach, it decides now; otherwise the arm asks
        // the source once the journal is on flash (claim_step).
        reattach_ask_ = r == ct::Reason::None &&
            rdintent::plan_reattach(run_cable_, run_facts_, fusb302::pin_rd(), fusb302::self_powered(), true);
        claim_asked_ = false;
        if (reattach_ask_ && source_claim_w() != 0) decide_reattach();
        arm_meta_ = meta; arm_ = 1; arm_t0_ = ctx_.now; jr_wait_ms_ = 0;
        charger_journal::hw::arm_start();
        runner_.hold(true);
        journal_active_ = true; journal_closed_ = false;
        set_stage(Stage::Running); follow_ = true;
    }
    else start_failed(r, ctx_.ui->busy_reason(), in.vbus_mv);   // the runner's own preflight refused the START
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
void ChargerTestSession::set_level(ct::Level l) {
    options_ = ct::with_level(options_, l);
    invalidate_confirmation();   // the preview's step count follows the level
}
void ChargerTestSession::enter() {
    ctx_.cables.sync();
    entering_ = true;   // the screen still shows the previous page, which the redraw erases: no body wipe here
    if (resume_pending_) set_stage(Stage::Resume);
    else set_stage(Stage::Algorithms, kPresetFirst);
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
void ChargerTestSession::freeze_facts() {
    facts_.cap_mv = ctx_.cap_mv;
    facts_.xiaomi_seen = fusb302::vdm().xiaomi;
    facts_.mi_auth = fusb302::sink_status().xiaomi_authed;
    facts_.frozen = true;
}

ChargerTestPage::ChargerTestPage(UiContext& c, ChargerTestSession& s) : PageBase(c, Page::ChargerTest, "Charger test"), s_(s) {
    s_.bind_view(this);
}
void ChargerTestPage::on_stage(bool entering) {
    ctx_.toast.clear();
    invalidate();
    grid_ = kit::ResultGrid{};
    if (!entering && !ctx_.blank) clear_y_ = kit::kBelowBar;
    if (s_.stage_ == Stage::Results) build_results();
#if K2_SPECROWS
    if (s_.stage_ == Stage::SpecDetail) spec_list_load(s_.detail_index_);
#endif
}
#if K2_SPECROWS
// The spec rows file of the run in hand for the remote: the engine's projection while it holds this run's records, else the journal's
// file (its header must name this run). 0 = none. Both sources give the same bytes (rows_fill writes what the journal stores).
// The journal's rows file when its header names `run`: its size and the parsed index. 0 = none.
static size_t journal_rows_index(uint32_t run, specsec::RowsIndex &ix) {
    const size_t size = charger_journal::hw::spec_rows_size();
    uint8_t hdr[specsec::kRowsHdr];
    if (size < specsec::kRowsHdr || size > 0xFFFFu || !charger_journal::hw::spec_rows_read(0, hdr) || !specsec::rows_parse(hdr, size, run, ix)) return 0;
    return size;
}
uint16_t ChargerTestSession::spec_rows_len() const {
    if (!runner_.evidence().spec.shown()) return 0;
#if K2_PDSPEC
    if (spec_live_run_ && spec_live_run_ == runner_.run_id())
        if (const pdspec::Engine *e = ctx_.ui->pd_spec().engine_for_test()) return specsec::rows_total(*e) ? static_cast<uint16_t>(specsec::rows_file_size(*e)) : 0;
#endif
    specsec::RowsIndex ix;
    return static_cast<uint16_t>(journal_rows_index(runner_.run_id(), ix));
}
size_t ChargerTestSession::spec_rows_read(uint16_t off, std::span<uint8_t> out) const {
    const size_t total = spec_rows_len();
    if (off >= total || out.empty()) return 0;
    const size_t n = std::min<size_t>(out.size(), total - off);
#if K2_PDSPEC
    if (spec_live_run_ && spec_live_run_ == runner_.run_id())
        if (const pdspec::Engine *e = ctx_.ui->pd_spec().engine_for_test()) {
            specsec::rows_fill(*e, runner_.run_id(), off, out.data(), n);
            return n;
        }
#endif
    return charger_journal::hw::spec_rows_read(off, out.first(n)) ? n : 0;
}
// The rows of check `k` (a cell index) for the detail list: from the engine while it holds this run's records, else from the journal's
// rows file (the run's id must match). Fewer than two rows, a verdict that is not pass or fail, or no source: no list, the summary shows.
[[gnu::noinline]] void ChargerTestPage::spec_list_load(int k) {
    spec_list_ = {};
    if (s_.spec_pos_ || k < 0 || k >= static_cast<int>(specsec::kCells)) return;   // a range's slot: its summary table only
    const pdspec::Verdict v = s_.runner_.evidence().spec.cells[static_cast<size_t>(k)].verdict();
    if (v != pdspec::Verdict::Pass && v != pdspec::Verdict::Fail) return;
    if (!specsec::has_list(static_cast<size_t>(k))) return;
#if K2_PDSPEC
    if (s_.spec_live_run_ && s_.spec_live_run_ == s_.runner_.run_id()) {
        if (const pdspec::Engine *e = ctx_.ui->pd_spec().engine_for_test()) {
            spec_list_.n = static_cast<uint16_t>(specsec::row_count(*e, static_cast<size_t>(k)));
            spec_list_.worst = static_cast<uint8_t>(specsec::worst_row(*e, static_cast<size_t>(k)));
            spec_list_.live = true;
        }
    }
#endif
    if (!spec_list_.live) {
        specsec::RowsIndex ix;
        if (!journal_rows_index(s_.runner_.run_id(), ix)) return;
        spec_list_.n = static_cast<uint16_t>(ix.count(static_cast<size_t>(k)));
        spec_list_.first = static_cast<uint16_t>(ix.first(static_cast<size_t>(k)));
        spec_list_.worst = static_cast<uint8_t>(ix.worst(static_cast<size_t>(k)));
    }
    for (int i = 0; i < spec_list_.n; ++i) {
        specsec::Row r;
        if (spec_list_row(i, r) && (r.flags & specsec::kRowFail)) ++spec_list_.fails;
    }
    // A verdict that says fail but whose rows show none (a pass pool that overflowed; the documented gap) keeps the summary: a band that
    // reads PASS over a failed check would lie.
    if ((v == pdspec::Verdict::Fail) != (spec_list_.fails != 0)) spec_list_ = {};
}
bool ChargerTestPage::spec_list_row(int i, specsec::Row &r) const {
    const int k = s_.detail_index_;
    if (i < 0 || i >= spec_list_.n || k < 0) return false;
#if K2_PDSPEC
    if (spec_list_.live) {
        const pdspec::Engine *e = ctx_.ui->pd_spec().engine_for_test();
        return e && specsec::row_at(*e, static_cast<size_t>(k), static_cast<size_t>(i), r);
    }
#endif
    uint8_t d[specsec::kRowBytes];
    if (!charger_journal::hw::spec_rows_read(static_cast<uint32_t>(specsec::kRowsHdr + (spec_list_.first + static_cast<size_t>(i)) * specsec::kRowBytes), d)) return false;
    specsec::row_decode(d, r);
    return true;
}
#endif
void ChargerTestPage::on_confirmation_dropped() { foot_.invalidate(); action_.invalidate(); }
void ChargerTestPage::on_cable_changed() { ctx_.ui->refresh(ctx_.now); }
void ChargerTestPage::build_grid(bool running_grid, bool note_cuts) {
    result_layout::OemNotes oem;
    if (running_grid) {
        oem.headers = false;   // the rows are the plan's steps, in the order the runner walks them
        oem.fold = false;      // a unit keeps its row when it fails or is skipped: the finished rows stay above the running one
        line_n_ = result_layout::build_compact(layout_, kGridMetrics, lines_, s_.runner_.steps(), oem, s_.runner_.sweep_view());
        return;
    }
    // The live inputs come from the snapshot taken when the run ended; a page entered with none (a run this boot never saw end)
    // freezes them now.
    if (!s_.facts_.frozen) s_.freeze_facts();
    const auto& facts = s_.facts_;
    // Passive labels from the identity and the advertised APDOs (the same facts as the card), shown in
    // the OEM-over-PD section: the QC4+/QC5 label, a VID-driven vendor name, the Mi Turbo auth state.
    const auto card = report_card::build(s_.runner_.steps(), s_.runner_.evidence(), s_.runner_.handover(), s_.card_id_, {}, s_.runner_.state(),
                                         s_.runner_.reason(), {}, 0, s_.runner_.sweep_view());
    oem.headers = false;   // the grid is units only: no separator rows between the sections
    oem.vendor = result_layout::vendor_oem_name(s_.card_id_.vid);
    if (!oem.vendor && facts.xiaomi_seen) oem.vendor = "Xiaomi";   // SVID 0x2717 seen on the wire even without a VID
    oem.mi_auth = facts.mi_auth;
    oem.oppo_svid = report_card::oppo_note(card) != nullptr;
    oem.rebalanced = s_.runner_.evidence().rebalanced;   // the source re-split its budget mid-run this many times
    char scp_note[24] = {};   // "SCP cap 20V 6A": the row's label is the first two words
    if (report_card::scp_note(card, scp_note, sizeof scp_note)) oem.scp_cap = scp_note + 8;
    // "<=11.1V, 10.9A 121W claimed" / "stopped at 3.3s, ...": the SuperVOOC ceiling stage's result (Deep only; nothing when it did not
    // run), and the VOOC family's ident the detection heard ("0xA8", every level)
    char svooc_note[svooc_ceiling::kCapChars] = {};
    char ident_note[6] = {};
    const auto& ev = s_.runner_.evidence();
    if (svooc_ceiling::format_ident(ev.vooc_ident, ident_note)) oem.vooc_ident = ident_note;
    if (ev.svooc_end != 0) {
        svooc_ceiling::format_cap({ev.svooc_word, static_cast<svooc_ceiling::End>(ev.svooc_end), ev.svooc_vmax_mv, ev.svooc_stop_ms, ev.svooc_why},
                                  svooc_note);
        oem.svooc_cap = svooc_note;
    }
    // The VOOC family by its levels' watts: what the ceiling stage claimed of each (every level not tested without one)
    oem.vooc_watts = true;
    oem.vooc_claims = oem_watts::claims(oem_watts::Family::Vooc, {ev.svooc_word, static_cast<svooc_ceiling::End>(ev.svooc_end),
                                                                  ev.svooc_vmax_mv, ev.svooc_stop_ms, ev.svooc_why});
    char justification[48] = {};
    if (const auto label = report_card::qc_label(card); label != report_card::QcLabel::None) {
        report_card::qc_label_justification(card, justification, sizeof justification);
        oem.qc_label = report_card::qc_label_name(label);
        oem.qc_why = justification;   // build() copies the words into tokens inside this scope
    }
    line_n_ = result_layout::build_compact(layout_, kGridMetrics, lines_, s_.runner_.steps(), oem, s_.runner_.sweep_view());
    split_ = specsec::split(ev.spec, s_.runner_.spec_apdo(), result_layout::range_positions(s_.runner_.steps()));
    // build_compact() counts a row label or word it had to shorten: the gate fails on it like on any other cut.
    if (note_cuts && layout_.cuts) K2_TRUNC_NOTE_HERE(trunc::Kind::Cut, "results grid label or word", layout_.cuts);
}
void ChargerTestPage::build_results() {
    build_grid(false, true);
    compose_best();
}
// The best passing mode per protocol family as one line. Families are dropped from the end (lowest watts first) when the
// strip is full. Empty when nothing passed.
static pd_version::Version run_version(const ct::Evidence& e) {
    pd_version::Inputs in;
    in.wire_rev = e.wire_rev;
    in.rmdo = e.reported.valid; in.rmdo_major = e.reported.rev_major; in.rmdo_minor = e.reported.rev_minor;
    in.pps = (e.flags & ct::PpsOffer) != 0;
    in.epr = (e.flags & ct::EprTable) != 0;
    in.spr_avs = (e.flags & ct::SprAvsOffer) != 0;
    return pd_version::derive(in);
}
// The PD version leads the line ("PD3.0  PPS 11V 5A  Mi 55W", two spaces: the font has no middle dot); the trailing items go first
// when it is full, never the version (pd_version::headline).
__attribute__((noinline)) void ChargerTestPage::compose_best() {
    best_.clear();
    std::array<result_layout::SummaryLine, 3> best{};
    const size_t n = result_layout::summarize(s_.runner_.steps(), best);
    best_none_ = n == 0;
    std::array<std::string_view, 3> items{};
    for (size_t i = 0; i < n; ++i) items[i] = best[i].text;
    if (best_none_) { items[0] = "No passing mode"; }
    static_assert(pd_version::kMaxLabel + 2 + 15 <= kBestChars, "the version and \"No passing mode\" fit one line");
    char line[40];
    pd_version::headline(line, sizeof line, pd_version::label(run_version(s_.runner_.evidence())).view(), items.data(),
                         best_none_ ? 1 : n, size_t(kBestChars));
    best_.put(line);
}
void ChargerTestSession::message(const char* s) {
    message_.clear(); message_.put(s); invalidate_confirmation();
    if (view_) view_->on_message();
}
// The algorithm a Group row stands for (rows are 0-based: the family's own entries, nothing else).
A ChargerTestSession::algorithm_at(int row) const {
    if (row < 0) return A::Count;
    if (family_ == kFamPd) return row == 0 ? A::Pd3Fixed : row == 1 ? A::Pps : A::Count;   // rows 2/3 are the deep sweeps, no algorithm
    for (unsigned n = 0; n < unsigned(A::Count); ++n) {
        const auto a = static_cast<A>(n);
        if (!(kFamilies[family_] & ct::bit(a))) continue;
        if (!row--) return a;
    }
    return A::Count;
}
// A PD row and the Xiaomi row open their own steps under their grid line; any other family opens its whole step list.
const result_layout::CompactRow* ChargerTestSession::picked() const {
    return result_family_ == kFamPd || result_family_ == kFamXiaomi ? &result_row_ : nullptr;
}
bool ChargerTestSession::shows(const ct::Step& s) const {
    const auto* row = picked();
    return row ? result_layout::row_has(*row, s) : result_family_ < 0 || family(s) == result_family_;
}
int ChargerTestSession::step_index(int row) const {
    if (stage_ == Stage::Running) return row;
    const auto steps = runner_.steps();
    for (size_t n = 0; n < steps.size(); ++n) {
        if (!shows(steps[n])) continue;
        if (!row--) return static_cast<int>(n);
    }
    return -1;
}
// The deep sweeps' own lines in the PD family's Entries list, after the two offer rows (0 PD fixed, 1 PPS): the
// PPS sweep first when the source offers PPS APDOs, then the AVS sweep when it offers AVS APDOs. Never pinned at
// rows 2/3 - an AVS-only source reaches its AVS sweep at row 2. Returns 0 = PPS, 1 = AVS, -1 = not a sweep row.
constexpr int kSweepRowsBase = 2;
int sweep_kind_at(int row, unsigned pps_points, unsigned avs_points) {
    if (row < kSweepRowsBase) return -1;
    if (pps_points > 0) {
        if (row == kSweepRowsBase) return 0;
        if (avs_points > 0 && row == kSweepRowsBase + 1) return 1;
        return -1;
    }
    return avs_points > 0 && row == kSweepRowsBase ? 1 : -1;
}
bool sweep_row_shown(int family, int row, unsigned pps_points, unsigned avs_points) {
    return family == kFamPd && sweep_kind_at(row, pps_points, avs_points) >= 0;
}

unsigned ChargerTestSession::sweep_points() const {
    return ctsweep::plan_total_capped(fusb302::caps(), ctx_.cap_mv, fusb302::physical_cable().max_mv);
}
unsigned ChargerTestSession::avs_sweep_points() const {
    return ctsweep::plan_total_capped(fusb302::caps(), ctx_.cap_mv, fusb302::physical_cable().max_mv, ctsweep::Kind::Avs);
}

uint32_t ChargerTestSession::wait_total_ms() {
    return fusb302::caps().active_cc != 0 ? pd::kStartVbusAttachedWaitMs : pd::kStartVbusWaitMs;
}

void ChargerTestSession::update_summary(const ct::Input& in) {
    if (stage_ <= Stage::Review || stage_ == Stage::Custom) summary_ = ct::Runner::preview(fusb302::caps(), options_, in);
}

// The session half of the remote view; the page adds rows and the footer's words below. Every field is a read of RAM that
// the screens already keep, except the preview, which is refreshed here the same way the page tick does.
ChargerTestSession::View ChargerTestSession::view() {
    View v;
    const uint32_t now = ctx_.now;
    update_summary(input(now));
    v.stage = static_cast<uint8_t>(stage_);
    v.cursor = static_cast<uint8_t>(selection_ < 0 ? 0 : selection_ > 255 ? 255 : selection_);
    v.confirming = confirming_ && static_cast<int32_t>(now - confirm_until_) < 0;
    v.confirm_left_ms = v.confirming ? confirm_until_ - now : 0;
    v.confirm_reattach = v.confirming && confirm_reattach_;
    v.self_supplied = self_supplied();
    v.resume_pending = resume_pending_;
    v.auto_resume = auto_resume_;
    v.gate_ready = gate_.ready(now);
    v.resume_held = resume_hold_;
    v.behind_cable = resume_refused_;
    v.resume_rd = resume_rd_;
    if (stage_ == Stage::Waiting) {
        v.wait_elapsed_ms = now - wait_t0_;
        v.wait_total_ms = wait_total_ms();
        v.wait_paused = wait_paused_;
        v.wait_retry = wait_retry_;
    }
    if (stage_ == Stage::Running) v.run_elapsed_s = (now - run_t0_) / 1000;
    v.card_no = card_no_;
    v.delta_n = static_cast<uint8_t>(delta_n_ > 255 ? 255 : delta_n_);
    v.delta_runs = delta_runs_;
    v.preview = summary_;
    if (!(stage_ <= Stage::Review || stage_ == Stage::Custom)) {   // past Review the summary is the one START took: no refusal now
        v.preview.refusal = ct::Reason::None;
        v.preview.refusal_mv = 0;
    }
    v.pps_points = sweep_points();
    v.avs_points = avs_sweep_points();
    v.armed_is_pick = ctx_.cables.armed() && ctx_.cables.armed_choice() == ctx_.cables.selected();
    v.cable_armed = ctx_.cables.armed();
    v.topology = static_cast<uint8_t>(topology_);
    v.run_guard = run_guard_;
    const pd::Revision& rev = fusb302::source_revision();
    v.rmdo_valid = rev.valid;
    v.rmdo = rev.valid ? static_cast<uint8_t>((rev.rev_major << 4) | (rev.rev_minor & 0x0F)) : uint8_t(0);
    if (view_) view_->describe(v);
    return v;
}

void ChargerTestPage::describe(ChargerTestSession::View& v) {
    v.on_screen = true;
    v.rows = static_cast<uint8_t>(std::min(row_count(), 255));
    StrBuf<32> f;
    footer_line(f);
    const std::string_view t = f.view().substr(0, sizeof(v.message) - 1);
    for (size_t i = 0; i < t.size(); ++i) v.message[i] = t[i];
    v.message[t.size()] = 0;
    v.message_len = static_cast<uint8_t>(t.size());
}

int ChargerTestPage::row_count() const {
    switch (s_.stage_) {
    case Stage::Algorithms: return kViewRow + (s_.runner_.run_id() ? 1 : 0);
    case Stage::Custom: return kFamilyCount + 1;   // the families, then "Done"
    case Stage::Group: {
        if (s_.family_ != kFamPd) return static_cast<int>(selected(kFamilies[s_.family_]));
        const unsigned pps_pts = s_.sweep_points(), avs_pts = s_.avs_sweep_points();
        return 2 + (pps_pts ? 1 : 0) + (avs_pts ? 1 : 0);
    }
    case Stage::Cable: return ctx_.cables.count();
    case Stage::Review: return 1;   // START
    case Stage::Running: return static_cast<int>(s_.runner_.steps().size());
    case Stage::Results: return delta_row() + (s_.delta_n_ ? 1 : 0);
    case Stage::Changes: return static_cast<int>(s_.delta_n_);
    case Stage::Steps: return static_cast<int>(counts(s_.runner_, s_.picked(), s_.result_family_).total) + range_spec_n();
#if K2_SPECROWS
    case Stage::SpecDetail: return spec_list_.n >= 2 ? spec_list_.n : 0;   // the open check's transitions; the summary table has no rows
#else
    case Stage::SpecDetail:   // the summary table has no rows
#endif
    case Stage::Detail: return 0;
    case Stage::Resume: return 3;
    case Stage::Identity: case Stage::Ask: return 0;   // retired, never entered
    case Stage::Refuse: return 2;   // Start anyway, Start, skip spoof
    case Stage::Waiting: return 1;
    }
    return 0;
}
ListState ChargerTestPage::list_state() const {
    if (s_.stage_ == Stage::Results) return {row_count(), grid_.visible(), grid_.first()};   // the grid scrolls by pixel height, not by row slots
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
                     *kNoteFamily = "Entries inside this family", *kNoteXiaomi = "SVID 0x2717 auth, runs in PD",
                     *kNotePdFixed = "Fixed offers: SPR + EPR", *kNotePps = "PPS + SPR/EPR AVS offers",
                     *kNoteClassB = "20 V; Class A shows a skip", *kNoteUfcs = "UART 5/9/12V; needs UFCS",
                     *kNoteVivo = "UART 5/9/11V, unproven", *kNoteHs = "5V handshake, not power",
                     *kNoteEntry = "One entry of this family", *kNoteSweep = "Every 100mV, 1A; opt-in";
constexpr const char *kNotes[] = {kNoteCustom, kNoteLast, kNoteFamily, kNoteXiaomi, kNotePdFixed, kNotePps, kNoteClassB,
                                  kNoteUfcs, kNoteVivo, kNoteHs, kNoteEntry, kNoteSweep};
static_assert(all_fit(kNotes, kLabelAdv, kit::kNoteMaxPx), "a note is wider than the row note");

// The Group page's mask for row `row` of family `fam` (the PD family has two algorithm rows - fixed offers, the PPS /
// AVS offers - and, when the source offers PPS / AVS APDOs, two more: the deep sweeps, which are Options flags, not
// algorithm bits, so their mask is 0 and they are toggled by their own rows).
uint32_t group_mask(int fam, int row, A a) {
    if (fam == kFamPd) return row == 0 ? ct::kPdFixedAlgorithms : row == 1 ? ct::kPpsAlgorithms : 0;
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
// A Custom family row's box: 0 off, 1 part, 2 on. Xiaomi / Redmi follows the PD mask (its rows run in PD).
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
// A step held a plateau below its target: amber (Partial), the same tone its chip wears (result_layout::tone). A caution (a voltage fold) is
// amber too: vmap(Step) below.
bool is_capped(const ct::Step& s) { return s.verdict == ct::Verdict::Fail && s.reason == ct::Reason::Capped; }
// A Light run's listing is never a skip mark: a PD offer the source advertises wears the pass look, any other listing (LightLevel,
// UFCS, SCP) is Noted: the same tones its chip wears.
kit::Verdict vmap(const ct::Step& s, const ctsweep::View& view) {
    return is_capped(s) || result_layout::caution(s, view) ? kit::Verdict::Partial : result_layout::advertised(s) ? kit::Verdict::Pass :
           result_layout::noted(s) ? kit::Verdict::Noted : vmap(s.verdict);
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
using waitplus::cc_open;   // the same ladder cut as cc_word's "open" (and the footer's choice)
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

kit::OppRailSpec ChargerTestPage::opp_rail() const {
    const Stage st = s_.stage_;
    if (st == Stage::Algorithms || st == Stage::Custom || st == Stage::Group)
        return {"TEST", 3, 0, kit::OppTelemetry::Vaw};
    if (st == Stage::Cable)
        return {"TEST", 3, 1, kit::OppTelemetry::Vaw};
    if (st == Stage::Review || st == Stage::Refuse)
        return {"TEST", 3, 2, kit::OppTelemetry::Vaw};
    if (st == Stage::Running)
        return {"TEST", 0, 0, kit::OppTelemetry::AmpCcDp};
    if (st == Stage::Waiting)
        return {"WAITING", 0, 0, kit::OppTelemetry::WattCcDp};
    if (st == Stage::Results)
        return {"RESULTS", 0, 0, kit::OppTelemetry::Vaw};
    if (st == Stage::Steps || st == Stage::Detail || st == Stage::SpecDetail)
        return {"DETAIL", 0, 0, kit::OppTelemetry::Vaw};
    return {"TEST", 0, 0, kit::OppTelemetry::Vaw};
}

void ChargerTestPage::bar() {
    bar_valid_ = true;
    bar_key_ = static_cast<uint8_t>(static_cast<unsigned>(s_.stage_) + 1u);
}

// The footer's signature (tone + text; never 0): the same value whoever painted it, so a repaint finds its words already there.
static uint32_t footer_sig(FooterTone tone, std::string_view text) {
    return mix_u32(pg::mix_str(mix_init(), text), static_cast<uint32_t>(tone)) | 1u;
}
bool ChargerTestPage::foot(FooterTone tone, std::string_view text) {
    const bool show = !text.empty() && s_.stage_ != Stage::Running && s_.stage_ != Stage::Algorithms;
    const uint32_t sig = show ? footer_sig(tone, text) : 1u;
    if (!show && sig == foot_sig_) return true;
    if (show && sig == foot_sig_) return true;
    foot_sig_ = sig;
    if (show) {
        ctx_.toast.show(tone, text, ctx_.now);
        if (ctx_.toast.erase_pending) {
            gfx_fill(ctx_.content, 0, static_cast<int16_t>(ctx_.content.h - kToastH - kToastMarginBottom),
                     ctx_.content.w, kToastH, kBg);
            ctx_.toast.erase_pending = false;
            grid_.invalidate_from(static_cast<int16_t>(ctx_.content.h - kToastH - kToastMarginBottom));
            action_.invalidate();
            ctx_.rows.invalidate();
        }
    }
    return true;
}
constexpr int kFooterBandH = kToastH;
// A handshake window is about to open (ChargerTestSession::request_fc, before fctrig::start): paint the toast pill that says
// why the screen will be still, and remember it as painted so the page does not repaint it from inside the window.
void ChargerTestPage::announce(const char* words) {
    if (s_.stage_ != Stage::Running) return;
    const std::string_view text(words);
    const uint32_t sig = footer_sig(FooterTone::Info, text);
    if (sig == foot_sig_) return;
    ctx_.toast.show(FooterTone::Info, text, ctx_.now);
    draw_footer(ctx_.content, FooterTone::Info, text);
    ctx_.toast.erase_pending = false;
    ctx_.toast.drawn = true;
    foot_sig_ = sig;
}

// The footer of the stage (no key names: the strip says what the keys do). Returns its tone; a message takes it on every stage.
// A range's spec word as a verdict cell: a caution (an inferred limit exceeded) reads amber, nothing judged a dim "-".
static kit::Verdict range_verdict(specsec::RangeTone t) {
    switch (t) {
        case specsec::RangeTone::Pass: return kit::Verdict::Pass;
        case specsec::RangeTone::Caution: return kit::Verdict::Partial;
        case specsec::RangeTone::Fail: return kit::Verdict::Fail;
        case specsec::RangeTone::None: break;
    }
    return kit::Verdict::None;
}
// The footer of a PD spec check (a Results row or its detail page): what the cell says in words, the verdict's tone.
[[gnu::noinline]] static FooterTone spec_footer(StrBuf<32>& f, int check, const specsec::Cell& c, bool detail) {
    const char* name = spectext::label(static_cast<pdspec::Check>(check));
    const pdspec::Verdict v = c.verdict();
    switch (v) {
        case pdspec::Verdict::Skip: f.put(pdspec::skip_name(c.skip())); return FooterTone::Info;
        case pdspec::Verdict::Pending: if (!detail) f.put(name).put(": "); f.put("not judged"); return FooterTone::Info;
        case pdspec::Verdict::Unverified: if (!detail) f.put(name).put(": "); f.put("limit unverified"); return FooterTone::Info;
        case pdspec::Verdict::Pass:
        case pdspec::Verdict::Fail: break;
    }
    if (!detail) f.put(name).put(": ");
    if (c.fails) {
        f.put("fail ").put_u32(c.fails);
        if (detail && c.detailed()) f.put(" of ").put_u32(c.n).put(" measured");
        return FooterTone::Caution;
    }
    f.put("pass");
    if (detail && c.detailed()) f.put(", ").put_u32(c.n).put(" measured");
    return FooterTone::Ok;
}

static void put_of(StrBufBase &f, unsigned a, unsigned b, const char *tail) {
    f.put_u32(a).put(" of ").put_u32(b).put(tail);
}

static bool put_fold_footer(StrBufBase &f, const std::optional<ctsweep::RowView> &rw) {
    if (!rw || !rw->sum.points) return false;
    char nb[ctsweep::kFoldTextMax];
    const size_t len = ctsweep::format_fold(rw->sum, nb);
    if (!len) return false;
    f.clear();
    f.put(std::string_view(nb, len));
    return true;
}

static FooterTone range_footer(StrBuf<32> &f, const ct::Runner &runner, A algo, uint8_t pos) {
    const auto rc = result_layout::reach(runner.steps(), algo, pos);   // one line per APDO: its own reach
    // The APDO's deep sweep is part of this row: a fold it found takes the whole footer ("folds to 19.4V above 19.9V"), a lost
    // source or a clean range with a sweep names the sweep's peak; a range that fell short says how far (the full summary is the [SWEEP] log line).
    bool lost = false, dev = false;   // a failed sweep part (the source was lost) colours the chip red: the footer must not read "reached"
    for (const auto &y : runner.steps())
        if (ct::is_sweep(y) && y.algorithm == algo && y.pos == pos) {
            lost |= y.verdict == ct::Verdict::Fail;
            dev |= result_layout::caution(y, runner.sweep_view());
        }
    char wk[28] = {};
    const auto* walk = result_layout::step_caution(runner.steps(), algo, pos);   // a step walk the source did not follow at its quantum
    const size_t wl = walk ? ct::caution_text(*walk, wk) : 0;
    // Layout B: the range's own words come first (a fold, a walk, a lost source, a range it did not reach); on a range that
    // passed, a spec word that is not a pass names its check, in the tone the spec rows use
    const specsec::Apdo *sp = runner.evidence().spec.shown() ? runner.spec_apdo().find(pos) : nullptr;
    const specsec::RangeSpec rs = sp ? specsec::range_spec(*sp) : specsec::RangeSpec{};
    const bool spec_bad = rc.tone == result_layout::Tone::Pass && (rs.tone == specsec::RangeTone::Fail || rs.tone == specsec::RangeTone::Caution);
    const auto rw = runner.sweep_view().apdo(static_cast<uint8_t>(algo), pos);
    const bool has_pts = rw && rw->sum.points;
    if (has_pts && put_fold_footer(f, rw)) return FooterTone::Caution;
    if (!has_pts && lost) { f.put("sweep failed"); return FooterTone::Caution; }   // no recorded point at all
    if (wl) { f.put(std::string_view(wk, wl)); return FooterTone::Caution; }
    if (spec_bad && !lost) { f.clear(); spectext::range_footer(f, *sp, rs); return FooterTone::Caution; }
    if (has_pts && (lost || rc.tone == result_layout::Tone::Pass)) {   // both 5 V parts
        char v[12] = {};
        f.put("peak ");
        f.put(std::string_view(v, ctsweep::format_mv(rw->sum.peak_mv, v))).put("V@");
        f.put(std::string_view(v, ctsweep::format_mv(rw->sum.peak_at_mv, v))).put_char('V');
        return lost || dev ? FooterTone::Caution : FooterTone::Ok;
    }
    f.put(result_layout::reach_footer(rc).c_str());
    using result_layout::Tone;
    return rc.tone == Tone::Pass ? FooterTone::Ok : rc.tone == Tone::Partial || rc.tone == Tone::Fail ? FooterTone::Caution
                                                                                                        : FooterTone::Info;
}

#if K2_SPECROWS
[[gnu::noinline]] static FooterTone spec_detail_footer(StrBuf<32> &f, const specsec::Row &r) {
    const bool fail = (r.flags & specsec::kRowFail) != 0;
    StrBuf<24> name, val, margin;
    spectext::put_row_name(r, name);
    spectext::put_row_value(r, val);
    if (fail) spectext::put_row_tail(r, margin);
    // "worst 9→12V: 540ms +40ms" in as much of it as the line holds (27 cells): the margin goes first, then the value
    StrBuf<48> g;
    for (int part = 2; part >= 0; --part) {
        g.clear();
        g.put("worst ").put(name.view());
        if (part >= 1 && !val.view().empty()) g.put(": ").put(val.view());
        if (part >= 2 && !margin.view().empty()) g.put_char(' ').put(margin.view());
        if (text_cells(g.view()) <= 27) break;
    }
    f.put(g.view());
    return fail ? FooterTone::Error : FooterTone::Ok;
}
#endif

[[gnu::noinline]] FooterTone ChargerTestPage::footer_line(StrBuf<32> &f) {
    if (!s_.message_.view().empty()) {
        f.put(s_.message_.view().substr(0, kToastCells));
        return FooterTone::Caution;
    }
    switch (s_.stage_) {
    case Stage::Algorithms: {
        const int pr = preset_row_of(s_.options_.selected);
        f.put("Picked: ").put(pr ? kPresetNames[pr - 1] : "custom set");
        return FooterTone::Info;
    }
    case Stage::Custom: {
        unsigned on = 0;
        for (int n = 0; n < kFamilyCount; ++n) on += family_state(s_.options_.selected, n) != 0;
        put_of(f, on, kFamilyCount, " families on");
        return FooterTone::Info;
    }
    case Stage::Group: {
        unsigned on = 0;
        const int n = row_count();
        for (int i = 0; i < n; ++i) {
            if (sweep_row_shown(s_.family_, i, s_.sweep_points(), s_.avs_sweep_points())) {
                on += sweep_kind_at(i, s_.sweep_points(), s_.avs_sweep_points()) == 1 ? s_.options_.avs_sweep : s_.options_.pps_sweep;
                continue;
            }
            const uint32_t m = group_mask(s_.family_, i, algorithm_at(i));
            on += (s_.options_.selected & m) == m;
        }
        put_of(f, on, static_cast<unsigned>(n), " entries on");
        return FooterTone::Info;
    }
    case Stage::Cable: {
        const auto sel = ctx_.cables.selected();
        f.put("Picked: ");
        if (sel.kind == ctcable::Kind::Off) { f.put("no identity"); return FooterTone::Info; }
        row_ = ctx_.cables.row(ctx_.cables.index_of(sel), CableIdentityList::View::CableStep);
        const size_t room = kFooterCells - 8 - (row_.rating.empty() ? 0 : row_.rating.size() + 1);
        f.put(row_.text.view().substr(0, room));
        if (!row_.rating.empty()) f.put_char(' ').put(row_.rating);
        return FooterTone::Info;
    }
    case Stage::Review: {
        const auto in = input(ctx_.now);
        if (s_.summary_.refusal != ct::Reason::None) {
            review_refusal(s_.summary_.refusal, ctx_.ui->busy_reason(),
                           s_.summary_.refusal_mv ? int(s_.summary_.refusal_mv) : in.vbus_mv, f);
            return FooterTone::Caution;
        }
        const char *msg = "Check the other port";
        FooterTone tone = FooterTone::Caution;
        if (!s_.summary_.runnable) msg = "Nothing to run: all skip";
        else if (s_.confirming_) msg = s_.confirm_reattach_ ? kReattachConfirm : "Press again to start";
        else if (in.cable_mv == 20000 && s_.summary_.max_mv == 21000) { msg = "PPS: 21V on 20V wire"; tone = FooterTone::Info; }
        else if (self_supplied()) { msg = "Self-powered: VBUS stays up"; tone = FooterTone::Info; }
        f.put(msg);
        return tone;
    }
    case Stage::Refuse: f.put("Direct plug is the sure way"); return FooterTone::Caution;
    case Stage::Waiting: {
        const int cc1 = ctx_.analog ? ctx_.analog->cc1_mv : -1, cc2 = ctx_.analog ? ctx_.analog->cc2_mv : -1;
        const fusb302::LineTrial lt = fusb302::line_trial();   // the direct-plug fallback's own fact: the line our Rd is on while it is live
        const auto line = waitplus::footer(s_.wait_.state() == pd::VbusWait::State::Ready, cc1, cc2, ctx_.vbus_fast_mv,
                                           static_cast<uint32_t>(std::max<int32_t>(0, static_cast<int32_t>(ctx_.now - s_.wait_t0_))),
                                           lt.live ? lt.line : uint8_t(0), lt.moves);
        f.put(line.text);
        return line.tone == waitplus::Tone::Ok ? FooterTone::Ok : line.tone == waitplus::Tone::Caution ? FooterTone::Caution : FooterTone::Info;
    }
    case Stage::Running: {
        const auto &steps = s_.runner_.steps();
        const char *msg = nullptr;
        FooterTone tone = FooterTone::Info;
        if (s_.runner_.state() == ct::State::Unsafe) { msg = "Unplug; verify measured 5V"; tone = FooterTone::Error; }
        else if (s_.runner_.reason() != ct::Reason::None) { msg = ct::refusal_words(s_.runner_.reason()); tone = FooterTone::Caution; }
        else if (steps.empty()) msg = "Starting";
        else if (s_.runner_.state() == ct::State::Spec) msg = "PD spec checks";
        else if (s_.fc_words_ && fctrig::paint_critical()) msg = s_.fc_words_;   // the window the screen sits out
        if (msg) { f.put(msg); return tone; }
        unsigned i, n;
        const char *unit;
        unit_progress(s_.runner_, std::min(s_.runner_.index(), steps.size() - 1), i, n, unit);
        f.put(unit).put(": step ");
        put_of(f, i, n, "");
        return FooterTone::Info;
    }
    case Stage::Results: {
        if (s_.selection_ >= delta_row()) { f.put("Compare with the last card"); return FooterTone::Info; }
        if (s_.selection_ >= static_cast<int>(line_n_)) {   // a PD spec row
            const size_t ci = spec_cell(s_.selection_ - static_cast<int>(line_n_));
            if (ci >= specsec::kCells) return FooterTone::Info;
            return spec_footer(f, static_cast<int>(specsec::kOrder[ci]), s_.runner_.evidence().spec.cells[ci], false);
        }
        const size_t r_idx = lines_[static_cast<size_t>(s_.selection_)].row();
        const auto &row = layout_.rows[r_idx];
        const char *lbl = result_layout::row_label(layout_, r_idx);
        if (row.kind == result_layout::CompactKind::Header || row.kind == result_layout::CompactKind::Fact ||
            row.kind == result_layout::CompactKind::Collapsed) {
            f.put(lbl).put(": see steps");
            return FooterTone::Info;
        }
        if (row.kind == result_layout::CompactKind::Levels) {   // "SVOOC: up to 120W claimed": claimed, never a pass
            f.put(result_layout::levels_footer(row, lbl).c_str());
            return FooterTone::Info;
        }
        A algo = static_cast<A>(row.algo);
        uint8_t pos = 0;
        bool range = false;
        if (row.kind == result_layout::CompactKind::Pd) {   // the PD block: a range line is its chip's APDO, a fixed line the block's counts
            result_layout::RowTokens rt;
            result_layout::row_tokens(layout_, r_idx, s_.runner_.steps(), s_.runner_.sweep_view(), rt);
            const auto ln = lines_[static_cast<size_t>(s_.selection_)];
            for (int k = ln.first(); k < ln.first() + ln.count() && !range; ++k) {
                const auto &tk = rt.toks[static_cast<size_t>(k)];
                if (!tk.kind) continue;
                range = true;
                f.put(result_layout::kind_word(tk.kind)).put(": ");   // the footer names the line's kind: "PPS: caps 19.4V of 21V"
                algo = static_cast<A>(result_layout::kind_algorithm(tk.kind));
                pos = static_cast<uint8_t>(tk.mv);
            }
        }
        if (range) return range_footer(f, s_.runner_, algo, pos);
        return put_counts(f, lbl, counts(s_.runner_, &row));
    }
    case Stage::SpecDetail: {
        pdspec::Check chk;
        specsec::Cell cell;
        if (!spec_open(chk, cell)) { f.put("No result"); return FooterTone::Info; }
#if K2_SPECROWS
        if (spec_list_on()) {   // the worst transition, by name, in the verdict's tone
            specsec::Row r;
            if (!spec_list_row(spec_list_.worst, r)) { f.put("worst: no row"); return FooterTone::Info; }
            return spec_detail_footer(f, r);
        }
#endif
        return spec_footer(f, static_cast<int>(chk), cell, true);
    }
    case Stage::Steps: {
        const auto c = counts(s_.runner_, s_.picked(), s_.result_family_);
        if (const specsec::Apdo *p = spec_range(); p && s_.selection_ >= static_cast<int>(c.total)) {   // a range's own spec check
            const size_t slot = specsec::slot_at(p->avs, static_cast<size_t>(s_.selection_) - c.total);
            const pdspec::Check chk = specsec::apdo_check(p->avs, slot);
            if (chk != pdspec::Check::Count) return spec_footer(f, static_cast<int>(chk), specsec::slot_cell(s_.runner_.evidence().spec, *p, slot), false);
        }
        if (s_.picked()) return put_counts(f, s_.result_label_, c);   // "PPS: 3 pass, 1 caution": the name the bar's title uses
        f.put_u32(c.total).put(" steps, ");   // a non-PD unit (QC, UFCS, ...): its whole step list
        if (!c.fail && c.caution) f.put_u32(c.caution).put(" caution");
        else f.put_u32(c.fail).put(" failed");
        return c.fail || c.caution ? FooterTone::Caution : FooterTone::Info;
    }
    case Stage::Detail: {
        const auto steps = s_.runner_.steps();
        if (s_.detail_index_ >= 0 && static_cast<size_t>(s_.detail_index_) < steps.size()) {
            const auto &st = steps[static_cast<size_t>(s_.detail_index_)];
            if (st.reason == ct::Reason::StepResolution || st.reason == ct::Reason::ProgCheck) {
                char wk[28] = {};   // the walk's own words ("steps 100mV, not 20mV") or the transition flags ("overshoot +1")
                if (const size_t len = ct::caution_text(st, wk)) { f.put(std::string_view(wk, len)); return st.verdict == ct::Verdict::Pass ? FooterTone::Caution : FooterTone::Error; }
            } else if (st.reason == ct::Reason::Folded) {
                // a caution says what folded, from the APDO's sweep: "folds to 19.4V above 19.9V"
                if (put_fold_footer(f, s_.runner_.sweep_view().apdo(uint8_t(st.algorithm), st.pos))) return FooterTone::Caution;
            }
            if (result_layout::is_no_answer(st)) { f.put(trig::fail_long(fc::Fail::NoAnswer)); return FooterTone::Info; }   // 27 cells: the card carries ", unloaded"
        }
        f.put("Kept until the next start");
        return FooterTone::Info;
    }
    case Stage::Changes: f.put_u32(s_.delta_n_).put(" changed, was run ").put_u32(s_.delta_runs_); return FooterTone::Info;
    case Stage::Resume: {
        const auto w = resume_words(ctx_.now);
        f.put(resumetext::footer(w));
        return w.held ? FooterTone::Caution : FooterTone::Info;
    }
    default: break;
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
    switch (s_.stage_) {
    case Stage::Algorithms:
        if (index == kDepthRow) {
            out.text = "Depth";
            out.tail = kLevelItems[level_segment(s_.options_.level)];
            out.tail_color = kFocus;
            if (focused) out.note = level_note(s_.options_.level);
        } else if (index < kCustomRow) {
            const int p = index - kPresetFirst;
            out.marker = kit::Marker::Radio;
            out.state = p + 1 == preset_row_of(s_.options_.selected);
            out.text = kPresetNames[p];
            row_tail_.put_u32(entries(kPresetMasks[p]));
            if (focused) out.note = kPresetNotes[p];
        } else if (index == kCustomRow) {
            const bool custom = preset_row_of(s_.options_.selected) == 0;
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
        out.state = family_state(s_.options_.selected, index);
        out.text = kFamilyNames[index];
        if (index == kFamXiaomi) {
            out.text_color = kDim;
            out.tail = "PD";
            if (focused) out.note = kNoteXiaomi;
        } else {
            row_tail_.put_u32(entries(s_.options_.selected & kFamilies[index])).put_char('/').put_u32(entries(kFamilies[index]));
            if (focused) out.note = kNoteFamily;
        }
        break;
    case Stage::Group: {
        const A a = algorithm_at(index);
        const uint32_t m = group_mask(s_.family_, index, a);
        out.marker = kit::Marker::Check;
        if (sweep_row_shown(s_.family_, index, s_.sweep_points(), s_.avs_sweep_points())) {   // a deep sweep's own line, with its point count
            const bool avs = sweep_kind_at(index, s_.sweep_points(), s_.avs_sweep_points()) == 1;
            out.state = avs ? s_.options_.avs_sweep : s_.options_.pps_sweep;
            out.text = avs ? "AVS sweep" : "PPS sweep";
            row_tail_.put_u32(avs ? s_.avs_sweep_points() : s_.sweep_points()).put(" pts");
            if (focused) out.note = kNoteSweep;
            break;
        }
        out.state = (s_.options_.selected & m) == m;
        out.text = s_.family_ == kFamPd ? (index == 0 ? "PD fixed" : "PPS") : ct::name(a);
        if (focused) out.note = group_note(s_.family_, index, a);
        break;
    }
    case Stage::Cable:
        row_ = ctx_.cables.row(index, CableIdentityList::View::CableStep);
        CableIdentityList::fill_spec(row_, out);
        if (focused) out.note = row_.note;
        break;
    case Stage::Refuse:
        out.text = index == 0 ? "Start anyway" : "Start, skip spoof";
        if (index == 0) out.text_color = kCaution;
        break;
    case Stage::Resume:
        if (index == 0) {
            const auto c = counts(s_.runner_);
            row_text_.put("Run ").put_u32(s_.runner_.run_id()).put(": ").put_u32(c.pass + c.fail + c.skip + c.caution).put_char('/').put_u32(c.total).put(" done");
        } else if (index == 1) {
            out.text = ct::refusal_words(s_.runner_.reason());
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
        const auto &d = s_.delta_[static_cast<size_t>(index)];
        using DL = report_card::DeltaLine;
        if (d.kind == DL::Group) {
            out.text = report_card::detail::group_label(d.index);
            row_tail_.put(report_card::detail::gv_label(d.was)).put(" > ").put(report_card::detail::gv_label(d.now));
            out.tail_color = d.now == report_card::GV::Pass ? kOk : kError;
        } else {
            row_text_.put(report_card::detail::alg_label(d.index));
            if (d.kind == DL::RangeMax || (d.was_dv && d.now_dv)) {
                row_text_.put(d.kind == DL::RangeMax ? " max" : " fail");
                pg::put_dv(row_tail_, d.was_dv);
                row_tail_.put(" > ");
                pg::put_dv(row_tail_, d.now_dv);
            } else {
                row_text_.put_char(' ');
                pg::put_dv(row_text_, d.was_dv ? d.was_dv : d.now_dv);
                row_tail_.put(d.was_dv ? "FAIL > PASS" : "PASS > FAIL");
            }
            out.tail_color = (d.was_dv && !d.now_dv) ? kOk : kCaution;
        }
        break;
    }
#if K2_SPECROWS
    case Stage::SpecDetail: {   // one transition of the open check: marker, the pair, the value, the margin (red when it fails)
        specsec::Row r;
        if (!spec_list_row(index, r)) { out.text = "-"; out.text_color = kDim; break; }
        const bool fail = (r.flags & specsec::kRowFail) != 0, prog = (r.flags & specsec::kRowProg) != 0;
        out.marker = kit::Marker::Verdict;
        out.state = static_cast<uint8_t>(fail ? kit::Verdict::Fail : kit::Verdict::Pass);
        spectext::put_row_name(r, row_text_);
        // A fixed transition: the value at the second column, the margin at the tail. A programmable pass has a long name, so its tail
        // is the one number that matters: the steps that failed, else the worst error.
        if (prog && fail) spectext::put_row_tail(r, row_tail_);
        else if (prog) spectext::put_row_value(r, row_tail_);
        else { spectext::put_row_value(r, row_note_); out.col2 = row_note_.view(); spectext::put_row_tail(r, row_tail_); }
        out.tail_color = fail ? kError : kDim;
        break;
    }
#endif
    case Stage::Steps: {
        const int si = step_index(index);
        if (const specsec::Apdo *p = si < 0 ? spec_range() : nullptr) {   // a range's spec checks follow its steps: marker, check, worst value
            const int k = index - static_cast<int>(counts(s_.runner_, s_.picked(), s_.result_family_).total);
            const size_t slot = specsec::slot_at(p->avs, static_cast<size_t>(std::max(k, 0)));
            const pdspec::Check chk = specsec::apdo_check(p->avs, slot);
            if (k >= 0 && chk != pdspec::Check::Count) {
                const specsec::Cell cell = specsec::slot_cell(s_.runner_.evidence().spec, *p, slot);
                std::string_view shown;
                out.marker = kit::Marker::Verdict;
                out.state = static_cast<uint8_t>(spectext::cell_view(static_cast<int>(chk), cell, spec_txt_, shown));
                row_text_.put(spectext::label(chk));
                out.data = true;
                spectext::slot_tail(static_cast<int>(chk), cell, row_tail_);
                out.tail_color = cell.fails && cell.verdict() == pdspec::Verdict::Fail ? kError : kText;
                if (focused) { spectext::slot_note(static_cast<int>(chk), cell, row_note_); out.note = row_note_.view(); }
                break;
            }
        }
        if (si < 0) { out.text = "No selected steps"; out.text_color = kDim; break; }
        const auto &steps = s_.runner_.steps();
        const auto &s = steps[static_cast<size_t>(si)];
        out.marker = kit::Marker::Verdict;
        const auto view = s_.runner_.sweep_view();
        out.state = static_cast<uint8_t>(vmap(s, view));
        // The step's label; a mode the Xiaomi chain unlocked reads by its watts ("Mi 55W"), its before -> after is the Detail page's.
        row_text_.put(step_label::label(s, si > 0 ? &steps[static_cast<size_t>(si) - 1] : nullptr).view());
        out.data = true;
        if (s.measured_mv) {
            row_tail_.put_fixed(s.measured_mv, 1).put_char('V');
            out.tail_color = kVolt;
        } else {
            row_tail_.put("--");
            out.tail_color = kFaint;
        }
        if (focused) {
            const bool listing = result_layout::noted(s);   // a Light run's listing: LISTED / NOT RUN, no skip cause
            const bool caution = result_layout::caution(s, view);   // a voltage fold (CAUTION: folds back) or a tracking deviation (off 450mV)
            row_note_.put(listing || caution ? result_layout::verdict_word(s, view) : ct::name(s.verdict));
            char wk[28] = {};
            const bool walk = ct::caution_text(s, wk) != 0;   // "CAUTION: steps 100mV, not 20mV": the whole note, so no PDO suffix behind it
            if (result_layout::deviates(s, view)) { StrBuf<16> off; put_off(off, view, s); row_note_.put(": ").put(off.view()); }
            else if (walk) row_note_.put(": ").put(wk);
            else if (caution || (!listing && (s.verdict == ct::Verdict::Fail || s.verdict == ct::Verdict::Skip))) row_note_.put(": ").put(detail_name(s));
            if (s.pos && !(walk && s.reason == ct::Reason::StepResolution)) row_note_.put(" PDO #").put_u32(s.pos);
            out.note = row_note_.view().substr(0, kit::kNoteMaxPx / kLabelAdv);
        }
        break;
    }
    default: break;
    }
    if (!row_text_.view().empty()) out.text = row_text_.view();
    if (!row_tail_.view().empty()) out.tail = row_tail_.view();
}

// RowList stages: bind the list on first paint after an invalidate, then apply count and selection (the window follows).
// The Steps stage of a PD row has the row's own grid lines on top (a range or level chips as the Results grid shows them, at most
// two lines; the PD block shows the line that was opened); any other unit, and every other list, has no header and the list takes the whole body.
void ChargerTestPage::paint_list() {
    if (!laid_) {
#if K2_SPECROWS
        int16_t y = s_.stage_ == Stage::Resume ? runlay::kResumeRowsY : s_.stage_ == Stage::Refuse ? runlay::kRefuseRowsY : s_.stage_ == Stage::SpecDetail ? 58 : kit::kBodyY;
#else
        int16_t y = s_.stage_ == Stage::Resume ? runlay::kResumeRowsY : s_.stage_ == Stage::Refuse ? runlay::kRefuseRowsY : kit::kBodyY;
#endif
        hdr_n_ = 0;
        if (s_.stage_ == Stage::Steps && s_.picked() && results_row_ >= 0 && static_cast<size_t>(results_row_) < line_n_) {
            size_t first = static_cast<size_t>(results_row_);
            const bool block = layout_.rows[lines_[first].row()].kind == result_layout::CompactKind::Pd;   // the PD block: the opened line alone
            while (!block && first > 0 && !lines_[first].head()) --first;
            hdr_first_ = static_cast<uint8_t>(first);
            while (hdr_n_ < (block ? 1 : 2) && first + hdr_n_ < line_n_ && lines_[first + hdr_n_].row() == lines_[first].row()) {   // a row's lines
                kit::ResultRowSpec spec;
                grid_row(hdr_n_, spec);
                y = static_cast<int16_t>(y + kit::result_row_h(spec));
                ++hdr_n_;
            }
            grid_.init(ctx_.content, kit::kBodyY, y, &ChargerTestPage::result_row_fn, this);
        }
        ctx_.rows.init(ctx_.content, y, &ChargerTestPage::row_fn, this);
        ctx_.rows.restart();
        ctx_.rows.invalidate();
        laid_ = true;
    }
    if (hdr_n_) {
        grid_.set_count(hdr_n_);
        if (draw_grid_slice(grid_, ctx_.budget)) return;
    }
    ctx_.rows.set_count(row_count());
    ctx_.rows.select(s_.selection_);
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

// The "Cable connected" warning: the alert band, two lines saying why, then the two choices.
void ChargerTestPage::paint_warning() {
    if (!laid_) {
        const Surface &c = ctx_.content;
        if (ctx_.budget && !ctx_.budget->admit(static_cast<int>(c.w) * 44 + 1200, true)) return;
        kit::draw_alert_band(c, kit::kBodyY, false, "Cable connected");
        gfx_text(c, kSmall, 6, runlay::kRefuseRowsY - 46, "the spoof won't reach", kText, kBg);
        gfx_text(c, kSmall, 6, runlay::kRefuseRowsY - 26, "the charger.", kText, kBg);
        ctx_.rows.init(c, runlay::kRefuseRowsY, &ChargerTestPage::row_fn, this);
        ctx_.rows.restart();
        ctx_.rows.invalidate();
        hdr_n_ = 0;
        laid_ = true;
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
constexpr const char *kRdOn[2] = {"Rd on CC1", "Rd on CC2"};   // the checklist's first row once the Rd sits on a line
static_assert(all_fit(kRdOn, kSmallAdvPx, 190), "a Waiting checklist item is wider than its row");
// The Review table: five rows of 26 px from the body top, the key at x6 and the value from x68 (17 cells).
constexpr kit::KvLayout kKv{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, 68, 17, Align::Left};
// Review always carries the other-port warning: the alert band (y2..44), two S16 lines under it, then the
// three rows (Tests, Depth, Spoof at a 24 px pitch) and the 54 px START ticket.
constexpr int16_t kWarnLine1Y = 48, kWarnLine2Y = 66;
constexpr kit::KvLayout kReviewKv{90, 24, 6, nullptr, nullptr, 68, 17, Align::Left};
constexpr int16_t kReviewCellY = 176, kReviewCellH = 56;
static_assert(90 + 3 * 24 <= kReviewCellY && kReviewCellY + kReviewCellH <= kFooterY - 2,
              "Review: the table and START stack cleanly in 240 px");
// The Detail table: under the verdict band (y 2..37), the value column at x76 (16 cells).
constexpr kit::KvLayout kDet{44, kit::kRowH, 6, nullptr, nullptr, 76, 16, Align::Left};

kit::Verdict tone_verdict(Tone t) {
    switch (t) {
    case Tone::Pass: return kit::Verdict::Pass;
    case Tone::Fail: return kit::Verdict::Fail;
    case Tone::Partial: return kit::Verdict::Partial;
    case Tone::Pending: return kit::Verdict::Pending;
    case Tone::Skip: return kit::Verdict::Skip;
    case Tone::Noted: return kit::Verdict::Noted;   // a Light run's listing: advertised / not asked
    default: return kit::Verdict::None;
    }
}
void put_clip(char (&dst)[14], std::string_view s) {
    const size_t n = std::min<size_t>(s.size(), sizeof dst - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}
// The head line of a row whose section differs from the line above opens with a 2 px rule.
bool section_break(const result_layout::CompactGrid &l, std::span<const result_layout::CompactLine> lines, size_t i) {
    if (i == 0 || !lines[i].head()) return false;
    return result_layout::row_section(l, lines[i].row()) != result_layout::row_section(l, lines[i - 1].row());
}
// The row-relative token of `rt` that the step in flight stands for: a range or sweep step, the still-pending chip of its
// (kind, PDO slot), none when that chip is not there; any other step, the still-pending chip of its level, else the first
// still-pending chip of the row. -1 = none.
int pending_chip(const result_layout::RowTokens &rt, const ct::Step &st) {
    const uint8_t kind = result_layout::kind_of(st.algorithm);
    int first = -1;
    for (int k = 0; k < rt.count; ++k) {
        const auto &t = rt.toks[static_cast<size_t>(k)];
        if (!t.chip || t.glue || t.tone != Tone::Pending) continue;
        if (kind) {
            if (t.kind == kind && t.mv == st.pos) return k;
            continue;
        }
        if (t.kind) continue;
        if (t.mv && t.mv == st.mv) return k;
        if (first < 0) first = k;
    }
    return first;
}
// The step dot's row-relative token: the step the runner is on, Pending or Running, so the dot moves in the tick the index does.
int running_token(const result_layout::RowTokens &rt, const result_layout::CompactRow &row, const ct::Runner &runner) {
    const auto &steps = runner.steps();
    if (runner.index() >= steps.size()) return -1;
    const ct::Verdict v = steps[runner.index()].verdict;
    if (v != ct::Verdict::Running && v != ct::Verdict::Pending) return -1;
    if (!result_layout::row_has(row, steps[runner.index()])) return -1;   // a Xiaomi step has a row of its own
    return pending_chip(rt, steps[runner.index()]);
}
// The grid line the step `index` belongs to, known as soon as the runner moves to it.
int follow_line(const result_layout::CompactGrid &l, std::span<const result_layout::CompactLine> lines, const ct::Runner &runner) {
    const auto &steps = runner.steps();
    if (steps.empty()) return 0;
    const ct::Step &now = steps[std::min(runner.index(), steps.size() - 1)];
    int head = 0;
    result_layout::RowTokens rt;
    bool built = false;
    int tk = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        const uint8_t r = lines[i].row();
        if (!result_layout::row_has(l.rows[r], now)) continue;
        if (!built) {
            result_layout::row_tokens(l, r, steps, runner.sweep_view(), rt);
            tk = pending_chip(rt, now);
            built = true;
        }
        if (tk >= lines[i].first() && tk < lines[i].first() + lines[i].count()) return static_cast<int>(i);
        if (lines[i].head()) head = static_cast<int>(i);
    }
    return head;
}
// The pixels one update may still paint. Delegates to the page PaintBudget when active so every item shares one pass
// budget (and defers to the next tick when refused).
struct Budget {
    PaintBudget *parent = nullptr;
    int cap = 1 << 30;
    int spent = 0;
    bool admit(int px, bool force_first = true) {
        if (parent) {
            const bool ok = parent->admit(px, force_first);
            if (ok) spent += px;
            return ok;
        }
        if (px <= cap - spent || (force_first && spent == 0)) { spent += px; return true; }
        return false;
    }
};
// One cached text cell: `chars` cells wide, padded with blanks (left or right) so a shorter text leaves nothing behind. `sig`
// is the cell's signature (0 = not drawn); `data` marks external text (clipped and counted, not a gate failure). With a
// budget a changed cell that does not fit waits (its signature stays old, so it is still dirty on the next call).
[[gnu::noinline]] void put_line(const Surface &c, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t color, unsigned chars,
                                uint32_t &sig, bool right = false, bool data = false, Budget *bg = nullptr) {
    uint32_t h = pg::mix_str(mix_init(), text);
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
// The hero reading: 2 decimals below 10 V, 1 above; an unknown reading is two dashes. A sweep row's (`fine`) is every mV, so its
// six cells keep the point in the third whether the volts have one digit or two (" 5.000", "20.050").
void hero_text(StrBuf<12> &b, int32_t mv, bool fine) {
    if (mv < 0) { b.put("--"); return; }
    b.put_fixed(mv, fine ? 3 : mv < 10000 ? 2 : 1);
}
// Running's two stackings below the progress bar (y44..46), in k2/ui/run_layout.hpp with the sweep stack's overlap proof (every
// painted box disjoint, portrait and landscape). Plain: family y52, name y68 (x6), the 4-cell VBUS hero beside them, the target
// line y88, the gauge strip y98, a rule, the grid from y114. A sweep row (user decision 2026-11-16) makes the signed error the
// hero: it restacks, name y48, the error hero left, the act / set pair right, the gauge y116, the grid from y132. A switch wipes
// the body.
using runlay::RunLayout;
using runlay::kPlainRun;
using runlay::kSweepRun;
constexpr int16_t kRunBelowBar = runlay::kBelowBar, kRunHeroRight = runlay::kPlainHeroRight;
static_assert(kSweepRun.gauge_y + kit::kPassBandH <= kSweepRun.rule_y && kSweepRun.rule_y + 2 <= kSweepRun.grid_y, "sweep: gauge, rule, grid in order");
static_assert(kRunHeroRight + 3 + 12 <= 208, "the plain hero's unit fits the 208 px content width");
static_assert(runlay::kBlockUnitX + runlay::kAdv12 <= 208, "the act / set unit fits the 208 px content width");
static_assert(ctsweep::kErrOkMv == result_layout::kSweepPassMv, "the error hero and the sweep chip share the pass window");
static_assert(runlay::kHeroDigitsRight - 3 * runlay::kAdv48 - runlay::kAdv24 >= 0, "the sign and the three digit cells fit the content width");
}  // namespace

void ChargerTestPage::paint_review() {
    const Surface &c = ctx_.content;
    PaintBudget *const pbg = ctx_.budget;
    if (!laid_) {
        if (pbg && !pbg->admit(static_cast<int>(c.w) * 44 + 1400, true)) return;
        // The stern warning, on every Review, whatever the link says: the test opens the lines and a device on the other port can
        // be damaged. START's own arm + CONFIRM is the acknowledgement (one confirm per run).
        kit::draw_alert_band(c, kit::kBodyY, false, "OTHER PORT FREE");
        gfx_text(c, kSmall, 6, kWarnLine1Y, "Device on the other port", kText, kBg);
        gfx_text(c, kSmall, 6, kWarnLine2Y, "can be damaged. Unplug it", kText, kBg);
        action_.init(c, kReviewCellY, kReviewCellH);
        laid_ = true;
    }
    auto kv = [&](int i, const char *key, std::string_view v, uint16_t color) -> bool {
        const uint32_t h = mix_u32(pg::mix_str(pg::mix_cstr(mix_init(), key), v), color) | 1u;
        if (h == cell_sig_[i]) return true;
        if (pbg && !pbg->admit(static_cast<int>(c.w) * kReviewKv.pitch, true)) return false;
        cell_sig_[i] = h;
        const int16_t y = kit::kv_row_y(kReviewKv, i);
        gfx_fill(c, 0, static_cast<int16_t>(y + 1), c.w, kReviewKv.pitch - 1, kBg);
        kit::kv_row(c, kReviewKv, y, key, v, color);
        return true;
    };
    StrBuf<48> buf;
    // the PD spec stage's checks (Deep, K2_PDSPEC) are tests too
    buf.put_u32(s_.summary_.runnable + s_.summary_.spec_checks).put(" run, ").put_u32(s_.summary_.skipped).put(" skip");
    if (!kv(0, "Tests", buf.view(), s_.summary_.runnable ? kText : kCaution)) return;
    if (!kv(1, "Depth", kLevelItems[level_segment(s_.options_.level)], kText)) return;

    buf.clear();
    const auto sel = ctx_.cables.selected();
    if (sel.kind == ctcable::Kind::Off) buf.put("Off");   // plain: a START with Off disarms whatever is armed
    else {
        row_ = ctx_.cables.row(ctx_.cables.index_of(sel), CableIdentityList::View::CableStep);
        buf.put(row_.text.view());
        if (!row_.rating.empty()) buf.put_char(' ').put(row_.rating);
    }
    if (!kv(2, "Spoof", buf.view(), kText)) return;

    if (s_.confirming_) {
        const int32_t left = std::max<int32_t>(0, static_cast<int32_t>(s_.confirm_until_ - ctx_.now));
        const int permille = std::clamp<int>(left * 1000 / static_cast<int32_t>(Ui::kConfirmWindowMs), 0, 1000);
        buf.clear();
        buf.put("closes in ").put_u32(static_cast<uint32_t>((left + 999) / 1000)).put_char('s');
        action_.draw_armed("CONFIRM", buf.view(), permille, pbg);
    } else {
        action_.draw_idle("START", "other port must be empty", pbg, true);
    }
}

void ChargerTestPage::paint_hero(bool full, int16_t x, int16_t y, uint8_t chars, int32_t vbus_mv, uint16_t color) {
    StrBuf<12> t;
    hero_text(t, vbus_mv, chars > 5);
    kit::draw_hero(ctx_.content, hero_, kit::HeroSpec{&kBig, x, y, chars, color, "V"}, t.view(), full);
}

// A sweep row's error hero (run_layout.hpp): the signed error of the live reading against the setpoint in the 48 px font, its sign
// (the font has no '+') in the 24 px font against the first digit, the unit on the digits' baseline, graded green / amber / red
// by size. Beside it the act / set pair, 24 px values stacked on the decimal point. The error digits and the act value are Fields
// (char-diff); a colour or unit change repaints the Field whole; a sign that moves erases its old cell first. With no reading or
// no setpoint yet every value is a dim placeholder in its place.
void ChargerTestPage::paint_sweep_hero(int32_t vbus_mv, uint16_t set_mv) {
    const Surface &c = ctx_.content;
    const bool have = vbus_mv >= 0 && set_mv != 0;
    const sweep_hero::Err e = sweep_hero::format(have ? vbus_mv - static_cast<int32_t>(set_mv) : 0);
    const uint16_t color = !have ? kDim : e.grade == sweep_hero::Grade::Ok ? kOk : e.grade == sweep_hero::Grade::Caution ? kCaution : kError;
    const bool first = hero_color_ == 0;
    const bool volts = have && e.volts;
    const bool full = first || color != hero_color_ || volts != hero_volts_;
    // the sign: the cell left of the first digit, none for a zero or a placeholder
    const char sign = have ? e.sign : char(0);
    const int sx = sign ? sweep_hero::sign_x(runlay::kHeroDigitsRight, runlay::kAdv48, runlay::kAdv24, e.len, runlay::kSignGap) : -1;
    if (sign_x_ >= 0 && sx != sign_x_)   // before the digits: a cell the old sign stood in may now hold a digit
        gfx_text(c, kMid, static_cast<int16_t>(sign_x_), static_cast<int16_t>(kSweepRun.hero_y + runlay::kSignDy), " ", kText, kBg);
    StrBuf<12> t;
    if (have) t.put(std::string_view(e.digits, e.len)); else t.put("--");
    kit::draw_hero(c, hero_, kit::HeroSpec{&kBig, runlay::kHeroDigitsRight, kSweepRun.hero_y, kSweepRun.hero_chars, color, volts ? "V " : "mV"}, t.view(), full);
    const uint32_t ssig = (sign ? mix_u32(mix_u32(0x51u, static_cast<uint32_t>(sign)), color) : 0u) | 1u;
    if (full || sx != sign_x_ || ssig != sign_sig_) {
        if (sign) {
            const char s1[2] = {sign, 0};
            gfx_text(c, kMid, static_cast<int16_t>(sx), static_cast<int16_t>(kSweepRun.hero_y + runlay::kSignDy), s1, color, kBg);
        }
        sign_x_ = static_cast<int16_t>(sx);
        sign_sig_ = ssig;
    }
    hero_color_ = color;
    hero_volts_ = volts;
    // the act value, then the set value: both six cells, right-aligned, so the points line up
    const uint16_t acol = vbus_mv >= 0 ? kVolt : kDim;
    if (first || acol != act_color_) {
        act_.init(c, kMid, runlay::kBlockValueX, runlay::kActY, runlay::kBlockValueChars, Align::Right, acol, kBg);
        act_color_ = acol;
    }
    StrBuf<12> a;
    if (vbus_mv >= 0) a.put_fixed(vbus_mv, 3); else a.put("--");
    act_.draw(a.view());
    StrBuf<12> s;
    if (set_mv) s.put_fixed(set_mv, 3); else s.put("--");
    put_line(c, kMid, runlay::kBlockValueX, runlay::kSetY, s.view(), set_mv ? kText : kDim, runlay::kBlockValueChars, cell_sig_[6], true);
}

void ChargerTestPage::lay_running(bool sweeping, bool wipe) {
    const Surface &c = ctx_.content;
    const RunLayout &L = sweeping ? kSweepRun : kPlainRun;
    if (wipe) gfx_fill(c, 0, kRunBelowBar, c.w, kFooterY - kRunBelowBar, kBg);
    else gfx_fill(c, 0, L.gauge_y, c.w, kit::kPassBandH, kBg);
    gfx_fill(c, 0, L.rule_y, c.w, 2, kRule);
    if (sweeping) {   // the act / set block's fixed words: labels and units, on their values' baselines
        const int16_t dy = static_cast<int16_t>(runlay::kA24 - runlay::kA12);
        gfx_text(c, kLabelFont, runlay::kBlockLabelX, static_cast<int16_t>(runlay::kActY + dy), "ac", kDim, kBg);
        gfx_text(c, kLabelFont, runlay::kBlockUnitX, static_cast<int16_t>(runlay::kActY + dy), "V", kVolt, kBg);
        gfx_text(c, kLabelFont, runlay::kBlockLabelX, static_cast<int16_t>(runlay::kSetY + dy), "st", kDim, kBg);
        gfx_text(c, kLabelFont, runlay::kBlockUnitX, static_cast<int16_t>(runlay::kSetY + dy), "V", kDim, kBg);
    }
    grid_.init(c, L.grid_y, kFooterY, &ChargerTestPage::result_row_fn, this);   // every row dirty again
    for (int i = 2; i <= 6; ++i) cell_sig_[i] = 0;   // family, name, target, gauge strip, set / err line: they paint again
    needle_px_ = mark_px_ = -1;
    hero_color_ = 0;   // the next hero paints whole, at once
    hero_volts_ = false;
    sign_x_ = -1;
    sign_sig_ = 0;
    hero_ms_ = 0;
    grid_key_ = 0;     // and the step outline is rebuilt
    sweep_laid_ = sweeping;
}

void ChargerTestPage::paint_waiting(uint32_t now) {
    const Surface &c = ctx_.content;
    PaintBudget *const pbg = ctx_.budget;
    Budget bg{pbg, (1 << 30)};
    const int32_t vbus = ctx_.vbus_fast_mv;
    const auto &caps = fusb302::caps();
    if (!laid_) {
        if (!bg.admit(1600)) return;
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
    StrBuf<12> s;
    hero_text(s, vbus, false);
    const uint32_t hsig = mix_u32(pg::mix_str(mix_init(), s.view()), color) | 1u;
    if (hsig != hero_sig_) {
        if (!bg.admit(color != hero_color_ ? kHeroPx : 2000)) return;
        paint_hero(color != hero_color_, 170, 34, 5, vbus, color);
        hero_color_ = color;
        hero_sig_ = hsig;
    }

    const int cc1 = ctx_.analog ? ctx_.analog->cc1_mv : -1, cc2 = ctx_.analog ? ctx_.analog->cc2_mv : -1;
    const bool dv = vbus >= pd::kStartVbusMv && ctx_.analog;
    const int vals[4] = {cc1, cc2, ctx_.analog ? ctx_.analog->dp_mv : -1, ctx_.analog ? ctx_.analog->dm_mv : -1};
    for (int i = 0; i < 4; ++i) {
        std::string_view sv;
        uint16_t ink = kText;
        s.clear();
        if (i < 2) {
            if (ctx_.plus_key && vals[i] >= 0) {
                s.put_u32(static_cast<uint32_t>(vals[i])).put("mV");
                sv = s.view();
            } else {
                sv = cc_word(vals[i]);
                if (vals[i] < 0 || cc_open(vals[i])) ink = kFaint;
            }
        } else {
            if (dv) s.put_fixed(std::max(vals[i], 0), 2).put_char('V');
            else { s.put("--"); ink = kFaint; }
            sv = s.view();
        }
        put_line(c, kSmall, static_cast<int16_t>((i & 1) ? 148 : 42), static_cast<int16_t>(i < 2 ? 92 : 118), sv, ink, 7, cell_sig_[i], false,   // 7 cells: "Rp 1.5A"; 6 px after the label
                 false, &bg);
    }

    // The give-up clock: a 2 px bar that drains over the wait (the whole wait of the first attempt; a retry reads empty).
    const uint32_t total = ChargerTestSession::wait_total_ms();
    const uint32_t spent = now - s_.wait_t0_;
    const uint32_t left = (s_.wait_paused_ || spent >= total) ? 0 : total - spent;
    const int px = static_cast<int>(math::mul_div_u32(left, static_cast<uint32_t>(c.w), total));
    const uint32_t bsig = mix_u32(mix_init(), static_cast<uint32_t>(px)) | 1u;
    if (bsig != cell_sig_[4] && bg.admit(c.w * 2)) {
        cell_sig_[4] = bsig;
        if (px > 0) gfx_fill(c, 0, 138, static_cast<int16_t>(px), 2, kDim);
        if (px < c.w) gfx_fill(c, static_cast<int16_t>(px), 138, static_cast<int16_t>(c.w - px), 2, kRule);
    }

    const bool done[3] = {true, vbus >= pd::kStartVbusMv, caps.valid};
    // The first row names the line the Rd sits on ("Rd on CC2") once one is picked; the signature carries it, so a move repaints
    // the row and an idle tick writes nothing.
    const uint8_t rd_line = fusb302::active_cc();
    for (int i = 0; i < 3; ++i) {
        const uint32_t s = (done[i] ? 2u : 1u) | (i == 0 ? uint32_t(rd_line & 3u) << 2 : 0u);
        if (s == cell_sig_[5 + i]) continue;
        if (!bg.admit(static_cast<int>(c.w) * 26)) return;
        cell_sig_[5 + i] = s;
        const int16_t y = static_cast<int16_t>(141 + i * 26);
        gfx_fill(c, 0, y, c.w, 25, kBg);
        kit::draw_verdict(c, 8, static_cast<int16_t>(y + 6), 14, 14, done[i] ? kit::Verdict::Pass : kit::Verdict::None, {}, kBg);
        gfx_text(c, kSmall, 28, static_cast<int16_t>(y + 5), i == 0 && (rd_line == 1 || rd_line == 2) ? kRdOn[rd_line - 1] : kChecks[i],
                 done[i] ? kText : kDim, kBg);
        gfx_fill(c, 0, static_cast<int16_t>(y + 25), c.w, 1, kRule);
    }
}

// The Running gauge's full scale: one scale for the whole run, so the pass band slides with each target (5 V near the left,
// 20 V near the right) and the needle shows the travel between steps. 10 % above the highest target in the plan, rounded up
// to a whole volt, at least 6 V. A per-target scale (target x 1.5) put every band at the same 2/3 point and never moved.
// Derived from the plan each call: no state, a few dozen compares. A sweep row in flight also counts the top of its range
// (`sweep_hi_mv`, 0 otherwise), so the dotted band always fits; every other row keeps the plan's scale.
static int32_t gauge_full_mv(std::span<const ct::Step> steps, int32_t sweep_hi_mv = 0) {
    int32_t top = sweep_hi_mv;
    for (const auto &s : steps) top = std::max<int32_t>(top, s.mv);
    const int32_t full = (top * 110 / 100 + 999) / 1000 * 1000;
    return std::max<int32_t>(full, 6000);
}

void ChargerTestPage::paint_running(uint32_t now) {
    const Surface &c = ctx_.content;
    const auto &steps = s_.runner_.steps();
    // While a step is in flight (Runner::quiet()) every item below goes through one pixel budget (kFlightPx); an item that does
    // not fit stays dirty and paints on a later tick. update() has already returned if the engine is in a window the idle hook
    // does not serve (Runner::paint_blocked), so nothing here runs inside a handshake. Between steps the cap is off.
    PaintBudget *const pbg = ctx_.budget;
    const bool flight = s_.runner_.quiet() && laid_;
    Budget bg{pbg, flight ? kFlightPx : (1 << 30)};
    const size_t at = steps.empty() ? 0 : std::min(s_.runner_.index(), steps.size() - 1);
    const ct::Step *st = steps.empty() ? nullptr : &steps[at];
    // A deep-sweep row (one per APDO) has no single target: its strip shows the swept range as a dotted band, the setpoint as a
    // marker that moves point by point, and the needle; the hero reads to the mV with the setpoint and error under it. Every other
    // row keeps the pass band. The two rows stack the view differently (RunLayout): a switch lays the body out again.
    const ctsweep::Live live = s_.runner_.sweep_live();
    const bool sweeping = live.active;
    const RunLayout &L = sweeping ? kSweepRun : kPlainRun;
    if (!laid_ || sweeping != sweep_laid_) {
        const bool wipe = sweeping != sweep_laid_;
        if (!bg.admit(wipe ? c.w * (kFooterY - kRunBelowBar) : c.w * (kit::kPassBandH + 2))) return;
        if (!laid_) prog_.init(c, 6, 24, 196);
        lay_running(sweeping, wipe);
        laid_ = true;
    }
    const int32_t vbus = ctx_.pm_online ? ctx_.pm.vbus_mv : -1;
    const int32_t target = st ? st->mv : 0;
    const int32_t full = gauge_full_mv(steps, sweeping ? live.hi_mv : 0);
    const int32_t vbus_g = std::max<int32_t>(vbus, 0);
    // 1. the gauge strip, once per target (a new step, or the sweep range): the whole strip, the needle drawn at the live reading
    const uint32_t strip_sig = sweeping ? (mix_u32(mix_u32(0x5eedu, live.lo_mv), live.hi_mv) | 1u) : static_cast<uint32_t>(target) + 1u;
    if (strip_sig != cell_sig_[5] && bg.admit(target > 0 ? 196 * kit::kPassBandH : c.w * kit::kPassBandH)) {
        cell_sig_[5] = strip_sig;
        needle_px_ = mark_px_ = -1;
        if (sweeping) {
            const kit::RangePos p = kit::draw_range_gauge(c, 6, L.gauge_y, 196, vbus_g, live.set_mv, live.lo_mv, live.hi_mv, full);
            needle_px_ = p.needle; mark_px_ = p.mark;
        } else if (target > 0) {
            needle_px_ = kit::draw_pass_band(c, 6, L.gauge_y, 196, vbus_g, target, full, -1);
        } else {
            gfx_fill(c, 0, L.gauge_y, c.w, kit::kPassBandH, kBg);
        }
    }
    // 1a. a sweep row's marker follows the setpoint: the next point's Request is out, so the marker moves on without waiting for the hero
    if (sweeping && needle_px_ >= 0 && mark_px_ >= 0 &&
        kit::range_geom(6, 196, vbus_g, live.set_mv, live.lo_mv, live.hi_mv, full).mark != mark_px_ && bg.admit(kRangePx)) {
        const kit::RangePos p = kit::draw_range_gauge(c, 6, L.gauge_y, 196, vbus_g, live.set_mv, live.lo_mv, live.hi_mv, full, {static_cast<int16_t>(mark_px_), static_cast<int16_t>(needle_px_)});
        needle_px_ = p.needle; mark_px_ = p.mark;
    }
    // 1b. the step outline, in the same tick as the gauge band: the layout follows the runner now, and the grid moves the
    // outline from the cell that was under test to the one that is (two cell rims, never gated). The rows repaint at item 6.
    const uint32_t key = mix_u32(mix_u32(mix_init(), s_.runner_.revision()), static_cast<uint32_t>(s_.runner_.index())) | 1u;
    if (key != grid_key_) {   // rebuilt only when the runner's revision or step moved, never on a plain tick
        grid_key_ = key;
        build_grid(true);
    }
    // 5. Only a real exception or the active handshake is overlaid on Running; the progress/header already names the step.
    StrBuf<32> buf;
    {
        const bool notice = s_.runner_.state() == ct::State::Unsafe ||
                            s_.runner_.reason() != ct::Reason::None ||
                            s_.runner_.steps().empty() ||
                            s_.runner_.state() == ct::State::Spec;
        if (notice) {
            const FooterTone tone = footer_line(buf);
            const uint32_t sig = footer_sig(tone, buf.view());
            if (sig != foot_sig_) {
                if (pbg) {
                    const int before = pbg->spent;
                    if (foot(tone, buf.view())) {
                        bg.spent += pbg->spent - before;
                        foot_sig_ = sig;
                    }
                } else if (bg.admit(c.w * kFooterBandH)) {
                    foot(tone, buf.view());
                    foot_sig_ = sig;
                }
            }
        } else if (s_.fc_words_ && !fctrig::paint_critical() &&
                   ctx_.toast.view() == s_.fc_words_) {
            ctx_.toast.clear();
            foot_sig_ = 0;
            grid_.invalidate();
        }
    }
    grid_.set_count(static_cast<int>(line_n_));
    if (strip_sig == cell_sig_[5]) {
        const int dot_px = grid_.sync_dots();
        if (pbg) pbg->spent += dot_px;
        else bg.spent += dot_px;
    }
    // 2. elapsed m:ss at the right edge (one text per second): evaluated before hero so the 5200 px hero never defers a 1 s tick
    {
        const uint32_t secs = (now - s_.run_t0_) / 1000;
        buf.clear();
        buf.put_u32(secs / 60).put_char(':');
        if (secs % 60 < 10) buf.put_char('0');
        buf.put_u32(secs % 60);
        put_line(c, kSmall, 154, 4, buf.view(), kDim, 6, cell_sig_[1], true, false, &bg);
    }
    // 3. the live VBUS and its needle: every update between steps, about 4 Hz while a step is in flight (yields refreshes while grid rows are pending).
    // A sweep row's hero is the signed error (reading - setpoint, to the mV) with the act / set pair beside it, all of one reading:
    // twice a second.
    if ((!flight || now - hero_ms_ >= (sweeping ? kSweepHeroMs : kHeroMs)) && !(pbg && grid_.pending() && hero_color_ != 0) &&
        bg.admit((sweeping ? kSweepHeroPx : kHeroPx) + (needle_px_ >= 0 ? kNeedlePx : 0))) {
        hero_ms_ = now;
        if (sweeping) {
            paint_sweep_hero(vbus, live.set_mv);
        } else {
            paint_hero(hero_color_ != kVolt, kRunHeroRight, L.hero_y, L.hero_chars, vbus, kVolt);
            hero_color_ = kVolt;
        }
        if (sweeping && needle_px_ >= 0 && mark_px_ >= 0) {
            const kit::RangePos p = kit::draw_range_gauge(c, 6, L.gauge_y, 196, vbus_g, live.set_mv, live.lo_mv, live.hi_mv, full, {static_cast<int16_t>(mark_px_), static_cast<int16_t>(needle_px_)});
            needle_px_ = p.needle; mark_px_ = p.mark;
        } else if (target > 0 && needle_px_ >= 0) {
            needle_px_ = kit::draw_pass_band(c, 6, L.gauge_y, 196, vbus_g, target, full, needle_px_);
        }
    }
    // 4. the step header: counter, progress bar, protocol, step name, target. A step change is visible within a tick or two.
    {
        const auto work = result_layout::run_progress(steps);   // rows the run executes: a plan-time skip is not work (the counter and the bar read it alike)
        const int done = static_cast<int>(work.done), total = static_cast<int>(work.total);
        buf.clear();
        buf.put_u32(work.done).put_char('/').put_u32(work.total).put(" steps");
        put_line(c, kSmall, 6, 4, buf.view(), kText, 12, cell_sig_[0], false, false, &bg);
        if (bg.admit(prog_.cost(done, total))) prog_.set(done, total);
        const int fam = st ? family(*st) : -1;
        // the family label (dim, 12 px) above the name; a sweep row's name moves up and the label goes to its right, baseline to baseline
        const char *const fam_name = fam >= 0 ? kFamilyNames[fam] : "";
        if (sweeping) put_line(c, kLabelFont, runlay::kSweepFamX, L.name_y + kSmall.ascent - kLabelFont.ascent, fam_name, kDim, 16, cell_sig_[2], true, false, &bg);
        else put_line(c, kLabelFont, runlay::kLeftX, 32, fam_name, kDim, runlay::kFamCells, cell_sig_[2], false, false, &bg);
        buf.clear();
        if (st) buf.put(step_label::label(*st, at > 0 ? &steps[at - 1] : nullptr).view());
        // The plain name stays in the left column, kLeftGap short of the hero: a longer one drops to the 12 px font.
        const bool small = !sweeping && runlay::name_small(text_cells(buf.view()));
        if (small != name_small_) {   // the other font leaves rows of the old glyphs behind: blank the cell, repaint the name
            name_small_ = small;            gfx_fill(c, runlay::kLeftX, L.name_y, runlay::kLeftW, runlay::kH16, kBg);
            cell_sig_[3] = 0;
        }
        if (sweeping) put_line(c, kSmall, 6, L.name_y, buf.view(), kText, 11, cell_sig_[3], false, true, &bg);
        else if (small) put_line(c, kLabelFont, runlay::kLeftX, L.name_y + kSmall.ascent - kLabelFont.ascent, buf.view(), kText, runlay::kNameCellsSmall, cell_sig_[3], false, true, &bg);
        else put_line(c, kSmall, runlay::kLeftX, L.name_y, buf.view(), kText, runlay::kNameCells, cell_sig_[3], false, true, &bg);
        if (!sweeping) {
            buf.clear();
            if (st && st->mv) { buf.put("target "); pg::put_volts_trim(buf, st->mv); buf.put_char('V'); }
            put_line(c, kLabelFont, runlay::kLeftX, 68, buf.view(), kDim, runlay::kTargetCells, cell_sig_[4], false, false, &bg);
        }
    }
    // 6. the verdict grid follows the step under test (the layout was rebuilt at 1b). The window scrolls to the running line (a
    // manual scroll holds kFollowResumeMs, or until the step changes); rows paint top-to-bottom so finished rows above the running
    // row always update before the running row below them. Scroll and repaint may lag the dot.
    if (!s_.follow_ && (now - s_.scroll_ms_ >= kFollowResumeMs || s_.runner_.index() != s_.scroll_step_)) s_.follow_ = true;
    const int want = s_.follow_ ? follow_line(layout_, std::span<const result_layout::CompactLine>(lines_.data(), line_n_), s_.runner_)
                             : std::clamp(s_.scroll_line_, 0, std::max(0, static_cast<int>(line_n_) - 1));
    if (s_.follow_) grid_.anchor_bottom(want);   // finished rows stay above, the row under test ends the window
    else grid_.set_first(want);
    // an idle tick lets the grid have a whole row; otherwise it gets what the other items left, never more
    const int room = pbg ? (pbg->spent == 0 ? kFlightRowPx : pbg->left())
                         : (flight ? (bg.spent == 0 ? kFlightRowPx : bg.cap - bg.spent) : (1 << 30));
    const int drawn = grid_.draw_some(room, -1, pbg ? pbg->spent != 0 : (flight && bg.spent != 0));
    if (pbg) {
        pbg->spent += drawn;
        if (grid_.pending()) pbg->defer();
    } else {
        bg.spent += drawn;
    }
}

void ChargerTestPage::result_row_fn(void *p, int index, kit::ResultRowSpec &out) {
    static_cast<ChargerTestPage *>(p)->grid_row(index, out);
}

// Row `index` of the verdict grid (a layout line, or the Changes pseudo-row after them). The text lives in the page's cell buffers.
static_assert(result_layout::kGridMetrics.sweep == kit::kSweepMarkW, "the S mark's room in a chip's cell width is the kit's mark width");   // result_layout::cell_width
void ChargerTestPage::grid_row(int index, kit::ResultRowSpec &out) {
    out = {};
    const bool header = s_.stage_ == Stage::Steps;   // the Steps page's header: the picked row's own lines, never focused, no section rule
    if (header) index += hdr_first_;
    const bool results = s_.stage_ == Stage::Results;
    if (index >= static_cast<int>(line_n_)) {
        if (index < delta_row()) {   // a PD spec check: one wide cell, the number it measured
            const int k = index - static_cast<int>(line_n_);
            const size_t ci = spec_cell(k);
            if (ci >= specsec::kCells) return;
            const size_t ck = static_cast<size_t>(specsec::kOrder[ci]);
            std::string_view shown;
            const kit::Verdict v = spectext::cell_view(static_cast<int>(ck), s_.runner_.evidence().spec.cells[ci], spec_txt_, shown);
            out.label = spectext::label(static_cast<pdspec::Check>(ck));
            out.cells[0] = {v, shown, {}};
            out.section = specsec::section_start(ci) || k == 0;
        } else {
            StrBuf<12> t;
            t.put_u32(static_cast<uint32_t>(s_.delta_n_)).put(" changed");
            put_clip(cell_txt_[0][0], t.view());
            out.label = "Changes";
            out.cells[0] = {kit::Verdict::None, cell_txt_[0][0], {}};
            out.section = spec_n() > 0;
        }
        out.n = 1;
        out.focus = results && s_.selection_ == index;
        return;
    }
    const auto ln = lines_[static_cast<size_t>(index)];
    const size_t r_idx = ln.row();
    const auto &row = layout_.rows[r_idx];
    if (ln.head() || header) {   // the Steps header repeats the label on a block's continuation line
        const char *lbl = result_layout::row_label(layout_, r_idx);
        std::memset(cell_lbl_, 0, sizeof cell_lbl_);
        std::memcpy(cell_lbl_, lbl, std::min<size_t>(std::strlen(lbl), sizeof cell_lbl_ - 1));
        out.label = cell_lbl_;
    }
    result_layout::RowTokens rt;
    result_layout::row_tokens(layout_, r_idx, s_.runner_.steps(), s_.runner_.sweep_view(), rt);
    const int running = results || header ? -1 : running_token(rt, row, s_.runner_);
    // Layout B (a Deep run with its spec stage): a range line reads "PPS | 3.3-21V 5A | spec", the kind as its label, the range's
    // spec word in a cell of its own. Not while running: the stage has not judged anything yet.
    const bool spec_b = (results || header) && s_.runner_.evidence().spec.shown();
    uint8_t range_kind = 0, range_pos = 0;
    int n = 0;
    for (int k = 0; k < ln.count() && n < kit::kResultMaxCells; ++k) {
        const size_t ti = static_cast<size_t>(ln.first() + k);
        const auto &tk = rt.toks[ti];
        if (tk.glue) continue;
        std::string_view volts = tk.text, cur;
        if (tk.chip) {
            const size_t sp = volts.find(' ');
            if (sp != std::string_view::npos) { cur = volts.substr(sp + 1); volts = volts.substr(0, sp); }
        }
        if (tk.kind && spec_b) {
            put_clip(cell_txt_[n][0], volts);
            range_kind = tk.kind;
            range_pos = static_cast<uint8_t>(tk.mv);
        } else if (tk.kind) {   // a PD range chip: "PPS 5-11V", "AVS 15-28V"
            StrBuf<14> w;
            w.put(result_layout::kind_word(tk.kind)).put_char(' ').put(volts);
            put_clip(cell_txt_[n][0], w.view());
        } else {
            put_clip(cell_txt_[n][0], volts);
        }
        out.cells[n].label = cell_txt_[n][0];
        if (!cur.empty()) {
            put_clip(cell_txt_[n][1], cur);
            out.cells[n].current = cell_txt_[n][1];
        }
        out.cells[n].verdict = static_cast<int>(ti) == running ? kit::Verdict::Running : tone_verdict(tk.tone);
        out.cells[n].sweep = tk.sweep;
        ++n;
    }
    bool range_rule = false;   // a 2 px rule above the block's first range line
    if (range_kind && !header && index > 0 && lines_[static_cast<size_t>(index) - 1].row() == r_idx) {
        const auto pl = lines_[static_cast<size_t>(index) - 1];
        range_rule = true;
        for (int k = pl.first(); k < pl.first() + pl.count(); ++k) range_rule &= rt.toks[static_cast<size_t>(k)].kind == 0;
    }
    if (range_kind && n < kit::kResultMaxCells) {
        out.label = result_layout::kind_word(range_kind);
        const specsec::Apdo *p = s_.runner_.spec_apdo().find(range_pos);
        const specsec::RangeSpec rs = p ? specsec::range_spec(*p) : specsec::RangeSpec{};
        out.cells[n++] = {range_verdict(rs.tone), specsec::range_word(rs), {}};
    }
    out.n = static_cast<uint8_t>(n);
    out.focus = results && s_.selection_ >= 0 && s_.selection_ < static_cast<int>(line_n_) &&
                (row.kind == result_layout::CompactKind::Pd ? s_.selection_ == index : lines_[static_cast<size_t>(s_.selection_)].row() == r_idx);
    out.section = range_rule || (!header && section_break(layout_, std::span<const result_layout::CompactLine>(lines_.data(), line_n_), static_cast<size_t>(index)));
}

void ChargerTestPage::paint_results() {
    const Surface &c = ctx_.content;
    PaintBudget *const pbg = ctx_.budget;
    Budget bg{pbg, (1 << 30)};
    const auto all = counts(s_.runner_);
    if (!laid_) {
        if (!bg.admit(1600)) return;
        // pass / fail / skip; a run that found a voltage fold adds a fourth cell, "caution" (amber tilde), between pass and fail
        const kit::Verdict kinds[4] = {kit::Verdict::Pass, kit::Verdict::Partial, kit::Verdict::Fail, kit::Verdict::Skip};
        const int cols = all.caution ? 4 : 3, pitch = c.w / cols;
        for (int i = 0; i < cols; ++i) {
            const int16_t x = static_cast<int16_t>(i * pitch);
            if (i) gfx_fill(c, static_cast<int16_t>(x - 1), 0, 1, runlay::kResBandH, kRule);
            kit::draw_verdict(c, static_cast<int16_t>(x + 2), 6, 12, 12, kinds[all.caution || i < 1 ? i : i + 1], {}, kBg);
        }
        gfx_fill(c, 0, runlay::kResBandH, c.w, 1, kRule);
        gfx_fill(c, 0, runlay::kResRule2Y, c.w, 2, kRule);
        grid_.init(c, runlay::kResGridY, kFooterY, &ChargerTestPage::result_row_fn, this);
        laid_ = true;
    }
    const unsigned n4[4] = {all.pass, all.caution, all.fail, all.skip};
    static constexpr const char *kWords4[4] = {"pass", "caution", "fail", "skip"};
    const uint16_t ink4[4] = {kOk, kCaution, kError, kDim};
    const int cols = all.caution ? 4 : 3, pitch = c.w / cols, chars = (pitch - 18) / kSmallAdvPx;   // the text ends 2 px short of the next cell's rule
    // The words (pass / fail / skip) show for all three counts or for none: one of them too wide for its cell never leaves its
    // neighbours spelled out (a caution run has four narrow cells and shows numbers only).
    bool words = !all.caution;
    for (int i = 0; words && i < cols; ++i) {
        const int k = i < 1 ? i : i + 1;
        StrBuf<12> t;
        t.put_u32(n4[k]);
        words = static_cast<int>(t.view().size() + 1 + std::strlen(kWords4[k])) <= chars;
    }
    for (int i = 0; i < cols; ++i) {
        const int k = all.caution || i < 1 ? i : i + 1;   // without a caution the cell is skipped: pass, fail, skip
        StrBuf<12> t;
        t.put_u32(n4[k]);
        if (words) t.put_char(' ').put(kWords4[k]);
        const unsigned cw = std::max<unsigned>(static_cast<unsigned>(chars), static_cast<unsigned>(t.view().size()));
        put_line(c, kSmall, static_cast<int16_t>(i * pitch + 16), 4, t.view(), ink4[k], cw, cell_sig_[i], false, false, &bg);
    }
    put_line(c, kSmall, 6, runlay::kResBestY, best_.view(), best_none_ ? kDim : kText,
             kBestChars, cell_sig_[4], false, true, &bg);   // [0..3] are the header's cells (four with a caution)
    if (pbg && pbg->more) return;
    grid_.set_count(delta_row() + (s_.delta_n_ ? 1 : 0));
    grid_.show(s_.selection_, 1);
    draw_grid_slice(grid_, pbg);
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

// The verdict band of a detail page (y22..57): the verdict's fill and glyph, the word (font 24; font 12 when `tight`) and `right` (font 12,
// right-aligned to x218).
static void draw_band(const Surface &c, kit::Verdict v, std::string_view word, bool tight, std::string_view right, bool label = false) {
    const auto vs = kit::verdict_style(v);
    const uint16_t fill = vs.filled ? vs.fill : kSurface;
    gfx_fill(c, 0, 2, c.w, 36, fill);
    kit::draw_verdict(c, 6, 10, 20, 20, v, {}, fill);
    if (tight) gfx_text(c, kSmall, 32, 12, word, vs.ink, fill);
    else gfx_text(c, kMid, 32, 8, word, vs.ink, fill);
    const uint16_t ink = vs.ink == kDim || vs.ink == kFaint ? kText : vs.ink;
    if (label) gfx_text(c, kLabelFont, static_cast<int16_t>(c.w - 6 - static_cast<int>(right.size()) * kLabelAdv), static_cast<int16_t>(12 + kSmall.ascent - kLabelFont.ascent), right, ink, fill);
    else gfx_text(c, kSmall, static_cast<int16_t>(c.w - 6 - static_cast<int>(right.size()) * kSmallAdvPx), 12, right, ink, fill);
}

// One step's detail: a verdict band, then a fixed table (no scroll). A step with a stored capture gives its last two rows to the
// VBUS trace; the Xiaomi chain row shows the chain and the modes it unlocked where the header rows would be.
void ChargerTestPage::paint_detail() {
    if (laid_) return;
    const auto &steps = s_.runner_.steps();
    if (s_.detail_index_ < 0 || static_cast<size_t>(s_.detail_index_) >= steps.size()) { laid_ = true; return; }
    PaintBudget *const pbg = ctx_.budget;
    const Surface &c = ctx_.content;
    const auto &s = steps[static_cast<size_t>(s_.detail_index_)];
    const auto &e = s_.runner_.evidence();
    transcap::Meta cap;
    const bool has_cap = transcap::find_stored(s_.runner_.run_id(), static_cast<uint16_t>(s_.detail_index_), cap);
    const bool mi_chain = ct::is_mi_chain(s);

    StrBuf<32> val;
    if (detail_step_ == 0) {
        if (pbg && !pbg->admit(static_cast<int>(c.w) * 36, true)) return;
        const auto view = s_.runner_.sweep_view();
        const kit::Verdict v = vmap(s, view);
        val.put(step_label::label(s, s_.detail_index_ > 0 ? &steps[static_cast<size_t>(s_.detail_index_) - 1] : nullptr).view());
        // The band word (LISTED / NOT RUN for a Light run's listing) and the step's name share the band: a long word (CAUTION) beside
        // a long name (PPS 7.14->11V) would overlap, so the word drops to the small font then and the name keeps its whole text.
        const std::string_view word = result_layout::verdict_word(s, view);
        // word and name in 24 / 16 px; else the word in 16 px; else the name in 12 px too.
        const int room_mid = c.w - 6 - 32 - 8 - text_width(kMid, word), room_small = c.w - 6 - 32 - 8 - text_width(kSmall, word);
        const int name_w = static_cast<int>(val.view().size() * kSmallAdvPx);
        const bool tight = name_w > room_mid, label = tight && name_w > room_small;
        char buf[kFitBuf];
        draw_band(c, v, word, tight, fit_data(label ? kLabelFont : kSmall, val.view(), label ? room_small : tight ? room_small : room_mid, buf), label);
        detail_step_ = 1;
    }
    while (detail_step_ >= 1 && detail_step_ <= 6) {
        if (detail_step_ == 3 && mi_chain) {
            if (pbg && !pbg->admit(3600, true)) return;
            const auto modes = s_.runner_.mi_modes();
            for (int n = 0; n < 4; ++n) {
                StrBuf<40> line;
                if (n == 0) line.put("Auth ").put(mi_unlock::chain_text(s.detail).view());
                else if (n == 1 && mi_unlock::chain_done(s.detail)) {
                    if (modes.empty()) line.put("No new modes");
                    else line.put_u32(static_cast<uint32_t>(modes.size())).put(modes.size() == 1 ? " mode unlocked" : " modes unlocked");
                } else if (n >= 2 && static_cast<size_t>(n - 2) < modes.size()) line.put(mi_unlock::mode_text(modes[static_cast<size_t>(n - 2)]).view());
                if (line.view().empty()) continue;
                gfx_text(c, kLabelFont, 6, static_cast<int16_t>(116 + n * 18), line.view(), n == 0 ? kText : kDim, kBg);
            }
            detail_step_ = 7;
            laid_ = true;
            return;
        }
        if (detail_step_ == 5 && has_cap) {
            if (pbg && !pbg->admit(4500, true)) return;
            draw_capture(cap);
            detail_step_ = 7;
            laid_ = true;
            return;
        }
        if (pbg && !pbg->admit(2800, true)) return;
        val.clear();
        const char *key = nullptr;
        std::string_view sv;
        uint16_t col = kText;
        switch (detail_step_) {
        case 1:
            key = "Target";
            if (s.mv) val.put_fixed(s.mv, 2).put("V ").put_u32(s.ma).put("mA");
            else { val.put("No request"); col = kDim; }
            sv = val.view();
            break;
        case 2:
            key = "Measured";
            if (s.measured_mv) { val.put_fixed(s.measured_mv, 2).put_char('V'); col = kVolt; }
            else { val.put("--"); col = kFaint; }
            sv = val.view();
            break;
        case 3:
            if (is_capped(s)) {
                key = "Reached";
                val.put(result_layout::reach_span(result_layout::reach(steps, s.algorithm, s.pos)).view());
                col = kCaution;
            } else {
                key = "Window";
                if (s.mv) val.put_fixed(s.mv * 95 / 100, 2).put_char('-').put_fixed(s.mv * 105 / 100, 2).put_char('V');
                else { val.put("--"); col = kDim; }
            }
            sv = val.view();
            break;
        case 4: {
            key = "Reason";
            const bool deviates = result_layout::deviates(s, s_.runner_.sweep_view());
            char pk[16] = {};
            if (deviates) { put_off(val, s_.runner_.sweep_view(), s); sv = val.view(); }
            else if (s.reason == ct::Reason::ProgCheck && ct::caution_text(s, pk)) { val.put(pk); sv = val.view(); }   // "overshoot +1": the first flag and how many more (<= 12 cells)
            else sv = detail_name(s);
            if (s.verdict == ct::Verdict::Fail || deviates || result_layout::caution(s)) col = kCaution;
            break;
        }
        case 5:
            if (ct::is_mi_mode(s)) {   // the offer before the chain ("not listed": a new PDO; "not kept": a resumed run lost it)
                key = "Before";
                const auto* m = s_.runner_.mi_mode(s);
                if (m && m->before) val.put(mi_unlock::mode_text({0, m->before, m->pos}, false, kDet.value_chars).view());
                else { val.put(m ? "not listed" : "not kept"); col = kDim; }
            } else if (const auto mode = step_label::ufcs_range(s); mode.len) {
                key = "Mode";
                val.put(mode.view());
            } else {
                key = "Header";
                // An answer under what the offer proves says both (pd_version::conflict); the PD version itself is the Results line.
                const auto clash = pd_version::conflict(run_version(e));
                if (clash.len) { key = "Revision"; val.put(clash.view()); col = kCaution; }
                else {
                    val.put(pd::wire_revision_name(e.wire_rev));
                    if (e.reported.valid) val.put("  RMDO ").put_u32(e.reported.rev_major).put_char('.').put_u32(e.reported.rev_minor);
                }
            }
            sv = val.view();
            break;
        default:
            if (ct::is_mi_mode(s)) {   // the offer the chain unlocked (the row's own PDO)
                key = "After";
                val.put(mi_unlock::mode_text({0, s.pdo, s.pos}, false, kDet.value_chars).view());
                sv = val.view();
                break;
            }
            key = "Scope";
            sv = "Voltage hold";
            break;
        }
        kit::kv_row(c, kDet, kit::kv_row_y(kDet, detail_step_ - 1), key, sv, col);
        if (detail_step_ == 6) {
            gfx_fill(c, 0, kit::kv_row_y(kDet, 6), c.w, 1, kRule);
            laid_ = true;
        }
        detail_step_++;
    }
}

// One PD spec check's detail: the seven rows the old Spec page showed (verdict, worst, limit, count, fails, place, penalty) in a
// KvTable from the body top. The limit, the count and the place are what the cell kept (a restored cell from an old journal reads "-").
// K2_SPECROWS builds list the check's transitions instead when it has two or more (a band, then one row each).
void ChargerTestPage::paint_spec_detail() {
#if K2_SPECROWS
    if (spec_list_on()) {   // the check's transitions: the verdict band, then one row each (PLUS / MINUS scroll)
        if (detail_step_ == 0) {
            const Surface &c = ctx_.content;
            if (ctx_.budget && !ctx_.budget->admit(static_cast<int>(c.w) * 36, true)) return;
            const bool fail = spec_list_.fails != 0;
            StrBuf<20> cnt;
            if (fail) cnt.put_u32(spec_list_.fails).put(" of ").put_u32(spec_list_.n).put(" fail");
            else cnt.put_u32(spec_list_.n).put(" within limit");
            draw_band(c, fail ? kit::Verdict::Fail : kit::Verdict::Pass, fail ? "FAIL" : "PASS", false, cnt.view());
            detail_step_ = 1;
        }
        paint_list();
        return;
    }
#endif
    if (laid_) return;
    pdspec::Check chk;
    specsec::Cell cell;
    if (!spec_open(chk, cell)) { laid_ = true; return; }
    constexpr kit::KvLayout kSpecDet{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, kit::kKvValueX, 18, Align::Left};
    PaintBudget *const pbg = ctx_.budget;
    while (detail_step_ < spectext::kDetailRows) {
        if (pbg && !pbg->admit(2800, true)) return;
        StrBuf<20> v;
        uint16_t color = kText;
        spectext::detail_value(detail_step_, static_cast<int>(chk), cell, v, color);
        kit::kv_row(ctx_.content, kSpecDet, kit::kv_row_y(kSpecDet, detail_step_), spectext::kDetailKeys[detail_step_], v.view(), color);
        ++detail_step_;
    }
    laid_ = true;
}

void ChargerTestPage::paint_resume() {
    const Surface &c = ctx_.content;
    PaintBudget *const pbg = ctx_.budget;
    Budget bg{pbg, (1 << 30)};
    if (!laid_) {
        if (!bg.admit(static_cast<int>(c.w) * 24 + 25 * 8 * 16)) return;
        gfx_fill(c, 0, runlay::kResumeY, c.w, 24, kBg);
        put_line(c, kSmall, 4, runlay::kResumeY + 2, resumetext::status(resume_words(ctx_.now)), kCaution, 25, cell_sig_[0]);
        ctx_.rows.init(c, runlay::kResumeRowsY, &ChargerTestPage::row_fn, this);
        ctx_.rows.restart();
        ctx_.rows.invalidate();
        laid_ = true;
    } else {
        put_line(c, kSmall, 4, runlay::kResumeY + 2, resumetext::status(resume_words(ctx_.now)), kCaution, 25, cell_sig_[0], false, false, &bg);
    }
    paint_list();
}

void ChargerTestPage::sync_proposal(uint32_t now) {
    const auto in = input(now);
    if (s_.confirming_ && (static_cast<int32_t>(now - s_.confirm_until_) >= 0 || proposal(in) != s_.confirm_proposal_)) invalidate_confirmation();
    s_.update_summary(in);
}

void ChargerTestPage::share_card() {
    report_card::Card card;
    if (!report_card::hw::get(s_.card_no_, card)) return;
    const std::size_t len = qrshare::records::card(card, ctx_.ui->share_buffer());
    ctx_.ui->open_share(qrshare::Type::Card, qrshare::kSchemaCard, s_.card_name_[0] ? s_.card_name_ : "Card", len, ctx_.now,
                        report_card::hw::ext(s_.card_no_));
}

void ChargerTestPage::draw_static() {
    invalidate();
    if (!ctx_.blank && s_.stage_ != Stage::Algorithms) clear_y_ = kit::kBelowBar;
    if (!ctx_.blank && s_.stage_ == Stage::Algorithms) kit::RowList::clear_body_around(ctx_.content);
    if (ctx_.budget && !ctx_.budget->admit(static_cast<int>(ctx_.content.w) * kit::kBelowBar, true)) return;
    bar();
}

// The page's tick. Each stage's painter is its own small frame; every stage shares the page PaintBudget.
void ChargerTestPage::update(uint32_t now) {
    ctx_.cables.sync();   // the library changed: the list is read again (a chosen clone that is gone falls back to Off)
    sync_proposal(now);
    s_.selection_ = std::clamp(s_.selection_, 0, last_row());
    fill_cable_names();
    if (s_.stage_ != Stage::Running) {
        StrBuf<32> f;
        const FooterTone tone = footer_line(f);
        foot(tone, f.view());
    }
    // A step in flight inside an engine window the idle hook does not serve (the handshake pulses, the VOOC receive slices,
    // the UFCS/VIVO windows): nothing paints. The tick is dropped, the next one repaints what is dirty. Every tick tells the
    // blocked-paint meter whether it was sat out: its longest window is the `[CT] gap ... blocked=` of the row.
    const bool blocked = s_.stage_ == Stage::Running && laid_ && s_.runner_.paint_blocked(fctrig::paint_critical());
    gap::blocked.note(now, blocked);
    if (blocked) return;
    PaintBudget *const pbg = ctx_.budget;
    if (bar_key_ != static_cast<uint8_t>(static_cast<uint8_t>(s_.stage_) + 1)) {
        if (pbg && !pbg->admit(static_cast<int>(ctx_.content.w) * kit::kBelowBar, true)) return;
        bar();
    }
    title_badge(false);
    if (pbg && pbg->more) return;
    while (clear_y_ < kFooterY) {
        const int16_t h = std::min<int16_t>(PaintBudget::kStripRows, static_cast<int16_t>(kFooterY - clear_y_));
        if (pbg && !pbg->admit(static_cast<int>(ctx_.content.w) * h, true)) return;
        gfx_fill(ctx_.content, 0, clear_y_, ctx_.content.w, h, kBg);
        clear_y_ = static_cast<int16_t>(clear_y_ + h);
    }
    switch (s_.stage_) {
    case Stage::Review: paint_review(); break;
    case Stage::Refuse: paint_warning(); break;
    case Stage::Waiting: paint_waiting(now); break;
    case Stage::Running: paint_running(now); break;
    case Stage::Results: paint_results(); break;
    case Stage::Detail: paint_detail(); break;
    case Stage::SpecDetail: paint_spec_detail(); break;
    case Stage::Resume: paint_resume(); break;
    default: paint_list(); break;
    }
    if (s_.stage_ == Stage::Running) return;   // its footer is one of paint_running's budgeted items
}

bool ChargerTestPage::on_event(Event e) {
    // Waiting: + and - do nothing in any press length. The + key is wired to the CC pins (holding it wakes a sleeping charger),
    // so a press of it must never be navigation; the paging lock in dispatch() lets the long ones reach this far.
    // BACK and OK (cancel) are untouched.
    if (s_.stage_ == Stage::Waiting && (e.button == Button::Plus || e.button == Button::Minus)) return true;
    const bool long_ok = e.press == Press::Long && e.button == Button::Ok;
    // Long OK on the Results / Detail is Share: the run's report card as QR (a card exists once the run is saved).
    if (long_ok && (s_.stage_ == Stage::Results || s_.stage_ == Stage::Detail || s_.stage_ == Stage::SpecDetail) && !s_.runner_.busy() && s_.card_no_ != 0) {
        share_card();
        return true;
    }
    // Long OK on a Custom family opens its entries; on an entries page it toggles the whole group.
    const bool open_group = long_ok && s_.stage_ == Stage::Custom && s_.selection_ < kFamilyCount && s_.selection_ != kFamXiaomi;
    const bool toggle_group = long_ok && s_.stage_ == Stage::Group;
    if (e.press != Press::Short && !open_group && !toggle_group && !(e.press == Press::Long && e.button == Button::Back)) return false;
    // A long BACK stops first (a run, a resume offer, an armed START, the wait); with nothing to stop it goes to the Meter (the Ui).
    if (e.press == Press::Long && e.button == Button::Back && s_.stage_ != Stage::Resume && s_.stage_ != Stage::Waiting && !s_.runner_.busy() &&
        !s_.confirming_)
        return false;
    if (open_group) {
        s_.family_ = s_.selection_;
        set_stage(Stage::Group, 0);
        update(ctx_.now);
        return true;
    }
    if (toggle_group) {
        const uint32_t group = kFamilies[s_.family_];
        s_.options_.selected = (s_.options_.selected & group) == group ? s_.options_.selected & ~group : s_.options_.selected | group;
        invalidate_confirmation();
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Back) {
        // The stop first (a resume offer, a run, the confirm window, the wait, the Refuse warning: the remote CANCEL is the same
        // call); with nothing to stop BACK walks the wizard back.
        if (s_.stop() != ChargerTestSession::Stopped::Nothing) { update(ctx_.now); return true; }
        switch (s_.stage_) {
        case Stage::Algorithms: return false;   // the Ui leaves to the ring
        case Stage::Group: set_stage(Stage::Custom, s_.family_); break;
        case Stage::Custom: set_stage(Stage::Algorithms, kCustomRow); break;
        case Stage::Cable: {
            const int pr = preset_row_of(s_.options_.selected);
            set_stage(Stage::Algorithms, pr ? kPresetFirst + pr - 1 : kCustomRow);
            break;
        }
        case Stage::Review: set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected())); break;
        case Stage::Identity: case Stage::Ask: set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected())); break;   // retired, never entered
        case Stage::Steps: set_stage(Stage::Results, results_row_); break;
        case Stage::Detail: set_stage(Stage::Steps, s_.detail_row_); break;
        case Stage::Changes: set_stage(Stage::Results, delta_row()); break;
        case Stage::SpecDetail: set_stage(s_.spec_back_steps_ ? Stage::Steps : Stage::Results, s_.detail_row_); break;
        default: set_stage(Stage::Algorithms, kPresetFirst); break;
        }
        update(ctx_.now);
        return true;
    }
    if (e.button == Button::Plus || e.button == Button::Minus) {
        if (s_.stage_ == Stage::Review) {
            invalidate_confirmation();
            s_.message_.clear();
            update(ctx_.now);
            return true;
        }
        if (s_.stage_ == Stage::Detail || (s_.stage_ == Stage::SpecDetail && !spec_list_on())) return true;
        const int dir = e.button == Button::Plus ? 1 : -1;
        if (s_.stage_ == Stage::Running) {   // scroll the verdict grid by hand; following resumes (paint_running) after kFollowResumeMs or at the next step
            s_.scroll_line_ = std::clamp(grid_.first() + dir, 0, std::max(0, static_cast<int>(line_n_) - 1));
            s_.scroll_ms_ = ctx_.now;
            s_.scroll_step_ = s_.runner_.index();
            s_.follow_ = false;
            update(ctx_.now);
            return true;
        }
        if (s_.stage_ == Stage::Results) {   // the cursor steps unit by unit: a wrapped row's later lines are skipped (the PD block's lines are not)
            int i = s_.selection_ + dir;
            const auto stops = [&](int k) {   // the PD block stops at each of its lines (its range chips have a line each)
                const auto ln = lines_[static_cast<size_t>(k)];
                return ln.head() || layout_.rows[ln.row()].kind == result_layout::CompactKind::Pd;
            };
            while (i >= 0 && i < static_cast<int>(line_n_) && !stops(i)) i += dir;
            s_.selection_ = std::clamp(i, 0, last_row());
        } else {
            s_.selection_ = std::clamp(s_.selection_ + dir, 0, last_row());
        }
        s_.follow_ = false;
        invalidate_confirmation();
        s_.message_.clear();
        update(ctx_.now);
        return true;
    }
    if (e.button != Button::Ok) return false;
    ok_event();
    return true;
}

__attribute__((noinline)) void ChargerTestPage::ok_event() {
    if (s_.stage_ == Stage::Resume) {
        if (s_.resume_hold_) { s_.resume_hold_ = false; s_.auto_resume_ = true; }   // the identity / Rd that could not go on: try again, on OK's say
        resume_run(ctx_.now);
        update(ctx_.now);
        return;
    }
    if (s_.runner_.busy()) return;   // BACK reads Stop while a run is live (UI_SYSTEM.md 7.13.1); OK is free
    switch (s_.stage_) {
    case Stage::Algorithms:   // Depth cycles; a preset picks and advances; "Custom..." opens the per-family list
        if (s_.selection_ == kDepthRow) {
            s_.set_level(ct::next_level(s_.options_.level));
        } else if (s_.selection_ < kCustomRow) {
            s_.options_.selected = kPresetMasks[s_.selection_ - kPresetFirst];
            invalidate_confirmation();
            set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected()));
        } else if (s_.selection_ == kCustomRow) {
            set_stage(Stage::Custom, 0);
        } else {
            set_stage(Stage::Results);
        }
        break;
    case Stage::Custom:
        if (s_.selection_ >= kFamilyCount) {   // Done
            set_stage(Stage::Cable, ctx_.cables.index_of(ctx_.cables.selected()));
        } else if (s_.selection_ != kFamXiaomi) {   // Xiaomi rides the PD unit: nothing to toggle
            const uint32_t mask = kFamilies[s_.selection_];
            s_.options_.selected = (s_.options_.selected & mask) == mask ? s_.options_.selected & ~mask : s_.options_.selected | mask;
            invalidate_confirmation();
        }
        break;
    case Stage::Group:
        if (sweep_row_shown(s_.family_, s_.selection_, s_.sweep_points(), s_.avs_sweep_points())) {   // a deep sweep's own line: an Options flag, not an algorithm bit
            if (sweep_kind_at(s_.selection_, s_.sweep_points(), s_.avs_sweep_points()) == 1) s_.options_.avs_sweep = !s_.options_.avs_sweep;
            else s_.options_.pps_sweep = !s_.options_.pps_sweep;
            invalidate_confirmation();
        } else if (const A a = algorithm_at(s_.selection_); a != A::Count) {
            const uint32_t mask = group_mask(s_.family_, s_.selection_, a);
            s_.options_.selected ^= (s_.options_.selected & mask) == mask ? mask : mask & ~s_.options_.selected;
            invalidate_confirmation();
        }
        break;
    case Stage::Cable:   // a pick selects and advances: nothing is armed until START
        ctx_.cables.select(ctx_.cables.choice_at(s_.selection_));
        invalidate_confirmation();
        set_stage(Stage::Review, kStartRow);
        break;
    case Stage::Review:
        if (confirm_live()) confirmed_start();   // the second press: guard, identity, then Rd and the wait last
        else open_confirmation();                // the first press: the confirm window, nothing touched
        break;
    case Stage::Waiting: break;   // BACK stops the wait (it reads Stop); OK is free
    case Stage::Identity: case Stage::Ask: break;   // retired, never entered
    case Stage::Refuse:
        if (s_.selection_ == 0) {   // Start anyway: the identity runs armed, the guard's reading goes to the journal
            s_.guard_bypass_ = true;
            confirmed_start();
        } else {                 // Start, skip spoof: this run without the identity; the stored pick (/cable.cfg) is NOT changed
            s_.skip_spoof_run_ = true;
            invalidate_confirmation();
            set_stage(Stage::Review, kStartRow);
            confirmed_start();
        }
        break;
    case Stage::Changes: set_stage(Stage::Results, delta_row()); break;
    case Stage::SpecDetail: set_stage(s_.spec_back_steps_ ? Stage::Steps : Stage::Results, s_.detail_row_); break;
    case Stage::Results:
        if (s_.selection_ >= delta_row()) {
            if (s_.delta_n_) set_stage(Stage::Changes);
        } else if (s_.selection_ >= static_cast<int>(line_n_)) {   // a PD spec check opens its detail
            s_.detail_index_ = static_cast<int>(spec_cell(s_.selection_ - static_cast<int>(line_n_)));
            s_.detail_row_ = s_.selection_;
            s_.spec_pos_ = 0;
            s_.spec_back_steps_ = false;
            set_stage(Stage::SpecDetail);
        } else {
            int fam = -2;
            const auto &row = layout_.rows[lines_[static_cast<size_t>(s_.selection_)].row()];
            if (row.kind != result_layout::CompactKind::Header) {   // section headers are not openable
                fam = row.kind == result_layout::CompactKind::Probe ? kFamXiaomi :
                      (row.kind == result_layout::CompactKind::Fact || row.kind == result_layout::CompactKind::Collapsed) ? -1 :
                      family(static_cast<A>(row.algo));
            }
            if (fam != -2) {
                results_row_ = s_.selection_;
                s_.result_family_ = fam;
                s_.spec_range_ = line_range_pos(s_.selection_);
                s_.result_row_ = row;   // a PD row opens its own steps (a range, a level family, a sweep), the others their family
                const char *lbl = result_layout::row_label(layout_, lines_[static_cast<size_t>(s_.selection_)].row());
                std::memset(s_.result_label_, 0, sizeof s_.result_label_);
                std::memcpy(s_.result_label_, lbl, std::min<size_t>(std::strlen(lbl), sizeof s_.result_label_ - 1));
                set_stage(Stage::Steps);
            }
        }
        break;
    case Stage::Steps:
        if (const int index = step_index(s_.selection_); index >= 0) {
            s_.detail_index_ = index;
            s_.detail_row_ = s_.selection_;
            set_stage(Stage::Detail);
        } else if (const specsec::Apdo *p = spec_range()) {   // a range's spec check: its summary, BACK returns here
            const int k = s_.selection_ - static_cast<int>(counts(s_.runner_, s_.picked(), s_.result_family_).total);
            if (k >= 0 && k < range_spec_n()) {
                s_.spec_pos_ = p->pos;
                s_.spec_slot_ = static_cast<uint8_t>(specsec::slot_at(p->avs, static_cast<size_t>(k)));
                s_.spec_back_steps_ = true;
                s_.detail_row_ = s_.selection_;
                set_stage(Stage::SpecDetail);
            }
        }
        break;
    case Stage::Detail: set_stage(Stage::Steps, s_.detail_row_); break;
    case Stage::Running: s_.freeze_facts(); set_stage(Stage::Results); break;
    case Stage::Resume: break;
    }
    update(ctx_.now);
}

Strip ChargerTestPage::labels() const {
    sl::Id ok = sl::None;   // while a run is busy BACK reads Stop (UI_SYSTEM.md 7.13.1) and OK is blank
    if (!s_.runner_.busy()) {
        switch (s_.stage_) {
        case Stage::Algorithms: ok = s_.selection_ == kDepthRow ? sl::Change : s_.selection_ < kCustomRow ? sl::Pick : sl::Open; break;
        case Stage::Custom:
            ok = s_.selection_ >= kFamilyCount ? sl::Next : s_.selection_ == kFamXiaomi ? sl::None : sl::ToggleOpen;
            break;
        case Stage::Group: ok = sl::SetGrp; break;
        case Stage::Cable: ok = sl::Pick; break;
        case Stage::Review: ok = s_.confirming_ ? sl::Confirm : sl::Start; break;
        case Stage::Refuse: ok = sl::Start; break;
        case Stage::Waiting: break;   // BACK reads Stop
        case Stage::Results:
            ok = s_.selection_ >= delta_row() ? (s_.card_no_ != 0 ? sl::DiffShare : sl::Diff)
               : s_.selection_ >= static_cast<int>(line_n_) ? (s_.card_no_ != 0 ? sl::OpenShare : sl::Open)
                                                            : (s_.card_no_ != 0 ? sl::StepsShare : sl::Steps);
            break;
        case Stage::Changes: ok = sl::View; break;
        case Stage::Steps: ok = sl::Open; break;
        case Stage::Detail: ok = s_.card_no_ != 0 ? sl::StepsShare : sl::Steps; break;
        case Stage::SpecDetail:
            ok = s_.spec_back_steps_ ? (s_.card_no_ != 0 ? sl::StepsShare : sl::Steps) : (s_.card_no_ != 0 ? sl::ResultsShare : sl::Results);
            break;
        case Stage::Resume: ok = sl::Resume; break;
        case Stage::Running: ok = sl::View; break;
        default: break;   // Identity / Ask are retired
        }
    }
    Strip l = setup::labels(base_labels(), tests_stage(s_.stage_) ? setup::Step::Tests : setup::Step::Cable, ok);
    if (s_.stage_ == Stage::Review || s_.stage_ == Stage::Detail || (s_.stage_ == Stage::SpecDetail && !spec_list_on()) || s_.stage_ == Stage::Waiting) {   // nothing to move the cursor over
        l[Button::Minus] = sl::None;
        l[Button::Plus] = sl::None;
    }
    if (s_.runner_.busy() || s_.stage_ == Stage::Waiting) l[Button::Back] = sl::Stop;
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
    mark_px_ = -1;
    hero_ms_ = 0;
    grid_key_ = 0;
    foot_sig_ = 0;
    hero_sig_ = 0;
    bar_key_ = 0;
    bar_valid_ = false;
    hero_color_ = 0;
    clear_y_ = kFooterY;
    detail_step_ = 0;
}

void ChargerTestPage::invalidate_for_capture() {
    const uint32_t foot_sig = foot_sig_;
    invalidate();
    foot_sig_ = foot_sig;
}

}  // namespace k2
