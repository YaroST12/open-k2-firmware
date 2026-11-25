// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/charger_test.hpp"
#include "k2/chargertest/prog_check.hpp"
#include "k2/core/fmt.hpp"
#include "k2/core/log.hpp"
#include "k2/core/strpack.hpp"
#include "k2/fastcharge/accp.hpp"
#include "k2/pd/link_guard.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/fastcharge/qc.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace k2::charger_test {
Group group(Algorithm a) {
    constexpr Group groups[] = {
        Group::Pd, Group::Pd, Group::Pd, Group::Pd, Group::Pd, Group::Qc2, Group::Qc3,
        Group::Qc3Plus, Group::Afc, Group::Fcp, Group::Scp, Group::Vooc, Group::SuperVooc,
        Group::SuperVooc65, Group::Ufcs, Group::Vivo, Group::Pd, Group::Pd, Group::Pd, Group::Pd, Group::Qc2, Group::Qc3
    };
    static_assert(std::size(groups) == unsigned(Algorithm::Count));
    return a < Algorithm::Count ? groups[unsigned(a)] : Group::Count;
}
const char* name(Algorithm a) {
    constexpr const char* names[] = {
        K2_P("PD2.0 fixed"), K2_P("PD3.x fixed"), K2_P("PD3.1+ EPR"), K2_P("PD3.2+ SPR AVS"), K2_P("PPS (PD3.x)"),
        "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", K2_P("VOOC 5V handshake"), K2_P("SVOOC 5V handshake"),
        K2_P("SVOOC65 5V handshake"), "UFCS", "VIVO", K2_P("PD3.0 revision"), K2_P("PD3.1 revision"), K2_P("PD3.2 revision"), K2_P("EPR AVS"), K2_P("QC2 Class B"), K2_P("QC3 Class B")
    };
    static_assert(std::size(names) == unsigned(Algorithm::Count));
    return a < Algorithm::Count ? names[unsigned(a)] : "?";
}
const char* name(Group g) {
    static constexpr const char* names[] = {"PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", K2_P("SVOOC"), K2_P("SVOOC65"), "UFCS", "VIVO"};
    return g < Group::Count ? names[static_cast<unsigned>(g)] : "?";
}
const char* name(Verdict v) {
    static constexpr const char* names[] = {"WAIT", "RUN", "PASS", "FAIL", "SKIP"};
    return names[static_cast<unsigned>(v)];
}
const char* name(Level l) {
    static constexpr const char* names[] = {K2_P("Medium"), K2_P("Light"), "Deep", "Old"};   // Old: the retired byte 3 on an old card
    static_assert(std::size(names) == unsigned(Level::Retired) + 1);
    return unsigned(l) <= unsigned(Level::Retired) ? names[unsigned(l)] : "?";
}
const char* name(Reason r) {
    static constexpr const char* names[] = {
        "", K2_P("disabled"), K2_P("not advertised"), K2_P("over voltage cap"), K2_P("not implemented"), K2_P("confirm no load"),
        K2_P("no power monitor"), K2_P("no source"), K2_P("load attached"), K2_P("tool busy"), K2_P("fake offers"),
        K2_P("request failed"), K2_P("timeout"), K2_P("voltage outside band"), K2_P("hard reset"), K2_P("source lost"),
        K2_P("cancelled"), K2_P("5V release failed"), K2_P("offers changed"), K2_P("PD active: use QC port"),
        K2_P("revision cannot be forced"), K2_P("select at least one test"), K2_P("physical cable limit"), K2_P("unsupported PDO layout"),
        K2_P("charger stayed in PD"), K2_P("rebooted during request"), K2_P("rebooted during handover"), K2_P("EPR entry refused"),
        K2_P("no contract"), K2_P("source shut off (needs replug)"), K2_P("no PD after re-attach"), K2_P("VBUS cut, source recovered"),
        K2_P("blocked by PD"), K2_P("no QC3+ response"), K2_P("rebooted 3x"), K2_P("needs external power"), K2_P("PD gave up before START"),
        K2_P("Class A only (12 V max)"), K2_P("not unlocked"), K2_P("20 V via QC3 only"), K2_P("PD only (output off)"), K2_P("capped below target"),
        K2_P("withdrawn by source"), K2_P("wrong voltage on 5V contract"), K2_P("needs load"), K2_P("floored above target"), K2_P("step resolution"),
        K2_P("HID-cable loop: 5V lock"), K2_P("advertised"), K2_P("light run: not asked"), K2_P("port cuts without load"),
        K2_P("output folds back"), K2_P("Xiaomi chain"), K2_P("transition check"), K2_P("D+/D- open: flip or data cable")   // flipping fixes a one-sided plug, a data cable fixes a charge-only one; 30 = the toast pill's cells
    };
    static_assert(std::size(names) == unsigned(Reason::LinesOpen) + 1);
    return names[static_cast<unsigned>(r)];
}
const char* refusal_words(Reason r) {
    switch (r) {
    case Reason::Release: return K2_P("VBUS not safe 5V");
    case Reason::ShutOff: return K2_P("Source shut off: replug");
    case Reason::WrongVoltage: return K2_P("wrong V on 5V contract");
    default: return name(r);
    }
}
size_t refusal_footer_text(Reason r, int vbus_mv, std::span<char> out) {
    if (r == Reason::WrongVoltage && vbus_mv >= 0 && out.size() > 1) {
        // The 27-cell footer line: "VBUS 8.5 V on a 5V contract" (8470 mV reads 8.5 V). The log line carries the full
        // "(shared bus)" phrase, so screen and log name the same case.
        const int tenths = (vbus_mv + 50) / 100;
        return fmt::format(out, K2_P("VBUS %d.%d V on a 5V contract"), tenths / 10, tenths % 10);
    }
    return fmt::format(out, "%s", refusal_words(r));
}
void Runner::note_preflight_refusal(Reason r, int vbus_mv, const char* detail) {
    if (busy()) return;   // only a refusal outside a run is recorded (the run id stays where it was)
    reason_ = r;
    refusal_mv_ = vbus_mv >= 0 ? uint16_t(std::min(vbus_mv, 65535)) : 0;
    if (r == Reason::WrongVoltage && vbus_mv >= 0) {
        // A live PDO 1 contract held outside vSafe5V (pattern shared-bus-split): name the measured level, so the
        // log and ct_start.py say why the run id stayed 0 - "VBUS 8.5 V on a 5 V contract (shared bus)".
        const int tenths = (vbus_mv + 50) / 100;   // 8470 mV reads 8.5 V
        K2LOG("[TEST] preflight refused: VBUS %d.%d V on a 5 V contract (shared bus)\n", tenths / 10, tenths % 10);
    } else if (r == Reason::Busy && detail && detail[0]) {
        // "tool busy" alone does not say which tool holds the line: the UI's busy_reason text does ("PD hold:
        // release it", "Test running: stop it", ...), so the refusal line names it for ct_start.py.
        K2LOG("[TEST] preflight refused: tool busy: %s (%d mV)\n", detail, vbus_mv);
    } else if (r == Reason::Inline && detail && detail[0]) {
        // "load attached" at 0 mA says nothing: the latch's cause does (pd::inline_cause_name: load or sink request).
        K2LOG("[TEST] preflight refused: load attached, latched by %s (%d mV)\n", detail, vbus_mv);
    } else {
        K2LOG("[TEST] preflight refused: %s (%d mV)\n", refusal_words(r), vbus_mv);
    }
}
const char* name(State s) {
    static constexpr const char* names[] = {K2_P("Setup"), K2_P("Verify 5V"), K2_P("Negotiating"), "Hold", K2_P("Return to 5V"), K2_P("Complete"), K2_P("Stopped"), K2_P("UNSAFE: unplug source"), K2_P("Detach charger"), K2_P("Initializing..."), K2_P("SVOOC ceiling"), K2_P("PD spec")};
    return names[static_cast<unsigned>(s)];
}
const char* name(Unit u) {
    static constexpr const char* names[] = {"PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", K2_P("SVOOC"), K2_P("SVOOC65"), "UFCS", "VIVO", K2_P("PD low")};
    static_assert(std::size(names) == unsigned(Unit::Count));
    return u < Unit::Count ? names[static_cast<unsigned>(u)] : "?";
}
const char* name(Mode m) { return m == Mode::Pd ? "PD" : K2_P("legacy"); }
namespace {
// The low edge of a programmable range (below 5 V): some sources stop regulating there and latch their output off until
// the mains is replugged (pattern latch-off-low-pps, Lenovo C135). Those rows are the PdLow unit and run last.
bool low_programmable(const Step& s) {
    return s.group == Group::Pd && s.mv && s.mv < 5000 &&
        (s.algorithm == Algorithm::Pps || s.algorithm == Algorithm::SprAvs || s.algorithm == Algorithm::EprAvs);
}
}
Unit unit_of(const Step& s) {
    return s.group >= Group::Count ? Unit::Count : s.group != Group::Pd ? static_cast<Unit>(s.group) :
        low_programmable(s) ? Unit::PdLow : Unit::Pd;
}
ctsweep::PointRange sweep_range(const Step& s, unsigned cap_mv) {
    const auto p = pd::decode_pdo(s.pdo);
    return ctsweep::part_range(ctsweep::point_range(p.min_mv, p.max_mv, cap_mv), sweep_low(s));
}
ctsweep::Plan sweep_plan(const Step& s, unsigned cap_mv, ctsweep::Depth d, const pd::SourceCaps& caps) {
    const bool pps = s.algorithm == Algorithm::Pps;
    return ctsweep::make_plan(d, pps ? ctsweep::Kind::Pps : ctsweep::Kind::Avs, sweep_range(s, cap_mv),
                              std::min<unsigned>(pd::decode_pdo(s.pdo).max_mv, cap_mv), pps ? ctsweep::phone_wins(caps, cap_mv, s.pos) : 0u);
}
Verdict unit_outcome(std::span<const Step> steps, Unit u) {
    bool pass = false, fail = false;
    for (const auto& s : steps) {
        if (unit_of(s) != u) continue;
        if (s.verdict == Verdict::Pending || s.verdict == Verdict::Running) return Verdict::Pending;
        pass |= s.verdict == Verdict::Pass; fail |= s.verdict == Verdict::Fail;
    }
    return fail ? Verdict::Fail : pass ? Verdict::Pass : Verdict::Skip;
}
const char* epr_detail_name(uint8_t d) {
    switch (d) {
    case kDetailEprFail + 1: return K2_P("EPR: source rejects cable");
    case kDetailEprFail + 2: return K2_P("EPR: source has no VCONN");
    case kDetailEprFail + 3: return K2_P("EPR: refused (RDO)");
    case kDetailEprFail + 4: return K2_P("EPR: source unable now");
    case kDetailEprFail + 5: return K2_P("EPR: 5V PDO lacks EPR bit");
    case kDetailEprFail: return K2_P("EPR: entry refused");
    case kDetailMonitor36: return K2_P("EPR: above K2's 36V monitor");
    case kDetailMonitor48: return K2_P("EPR: above K2's 48V monitor");
    case kDetailMonitorOther: return K2_P("EPR: above the K2's monitor");
    case kDetailEprOffered: return K2_P("EPR offered: needs EPR cable");
    case kDetailEprWithheld: return K2_P("EPR list withheld: cable-gated");
    case kDetailEprOfferedRefused: return K2_P("EPR offered; entry: refused");
    case kDetailEprOfferedRefused + 1: return K2_P("EPR offered; entry: cable rejected");
    case kDetailEprOfferedRefused + 2: return K2_P("EPR offered; entry: no VCONN");
    case kDetailEprOfferedRefused + 3: return K2_P("EPR offered; entry: refused (RDO)");
    case kDetailEprOfferedRefused + 4: return K2_P("EPR offered; entry: unable now");
    case kDetailEprOfferedRefused + 5: return K2_P("EPR offered; entry: 5V PDO lacks EPR bit");
    case kDetailNoChain: return K2_P("not unlocked (no OEM chain)");
    case kDetailChainUnfinished: return K2_P("not unlocked (chain unfinished)");
    case kDetailCableCap: return K2_P("not unlocked (3 A cable)");
    case kDetailVendorRefused: return K2_P("not unlocked (vendor not accepted)");
    case kDetailSourceReset: return K2_P("not unlocked (source reset)");
    default: return nullptr;
    }
}
const char* explanation(const Step& s) {
    if (s.reason == Reason::RevisionUntestable) {
        if (s.algorithm == Algorithm::Pd30Revision) return K2_P("No independent PD3.0 test");
        if (s.algorithm == Algorithm::Pd31Revision) return K2_P("No independent PD3.1 test");
        if (s.algorithm == Algorithm::Pd32Revision) return K2_P("No independent PD3.2 test");
    }
    const bool epr_row = s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs;
    // Every EPR skip names its cause (results page, card): the row's `detail` says which monitor ceiling (OverCap) or which
    // Enter_Failed data the source sent (EprRefused), see kDetailEprFail.
    if (epr_row && s.reason == Reason::OverCap) return epr_detail_name(s.detail) ? epr_detail_name(s.detail) : K2_P("EPR: above the K2's monitor");
    if (s.reason == Reason::NoOffer && s.pos) return K2_P("No allowed current at target");
    if (epr_row && s.reason == Reason::CableLimit)
        return epr_detail_name(s.detail) && (s.detail == kDetailEprOffered || s.detail == kDetailEprWithheld || s.detail >= kDetailEprOfferedRefused)
                   ? epr_detail_name(s.detail) : K2_P("EPR: cable not 50V 5A");
    if (epr_row && s.reason == Reason::NoOffer) return K2_P("EPR: not offered");
    if (s.reason == Reason::CableLimit) return K2_P("cable limit");   // a mode over the cable's current (a Xiaomi 6 A mode); 16-cell column
    if (s.reason == Reason::EprRefused) return epr_detail_name(s.detail) ? epr_detail_name(s.detail) : K2_P("EPR: entry refused");
    if (s.reason == Reason::LinesOpen) return kLinesOpenCell;
    if (s.reason == Reason::Capped) return K2_P("output capped");   // the Detail value column is 16 cells; name() is the long form
    if (s.reason == Reason::Floored) return K2_P("output floored");   // the reached floor is the row's measured_mv
    if (s.reason == Reason::Folded) return K2_P("folds back");   // the fold note itself ("folds to 19.4V above 19.9V") is the row's note line
    if (s.reason == Reason::StepResolution) return s.detail ? K2_P("step resolution") : K2_P("steps ignored");
    if (s.reason == Reason::ProgCheck && s.detail) return progcheck::flag_text(s.detail);   // the first finding; caution_text counts the rest
    if (s.reason == Reason::NoContract) return K2_P("EPR: no PD contract");
    if (s.reason == Reason::NotUnlocked) return epr_detail_name(s.detail) ? epr_detail_name(s.detail) : name(s.reason);
    if (s.reason == Reason::MiChain) {   // the 16-cell value column; the Xiaomi detail page spells the step out (mi_unlock::chain_text)
        switch (mi_unlock::chain_of(s.detail)) {
        case mi_unlock::Chain::NoSvid: return K2_P("no 0x2717 SVID");
        case mi_unlock::Chain::NoAnswer: return K2_P("chain: no answer");
        case mi_unlock::Chain::Stalled: return K2_P("chain stalled");
        case mi_unlock::Chain::Verified: return K2_P("chain verified");
        case mi_unlock::Chain::Finished: return K2_P("chain done 020A");
        case mi_unlock::Chain::None: break;
        }
        return name(s.reason);
    }
    if (s.reason == Reason::NoOffer && s.algorithm == Algorithm::SprAvs) return K2_P("SPR AVS not advertised");
    if (s.reason == Reason::NoOffer && s.algorithm == Algorithm::Pps) return K2_P("PPS not advertised");
    if (s.reason == Reason::UnsupportedOffer && s.avs)
        return s.pos < 8 ? K2_P("EPR AVS in an SPR slot") : K2_P("SPR APDO in an EPR slot");
    return name(s.reason);
}
size_t caution_text(const Step& s, std::span<char> out) {
    if (out.empty()) return 0;
    size_t n = 0;
    const auto put = [&](char c) { if (n + 1 < out.size()) out[n++] = c; };
    const auto word = [&](const char* t) { strpack::for_chars(t, put); };
    if (s.reason == Reason::StepResolution) {
        if (!s.detail) word(K2_P("steps ignored"));
        else {
            const unsigned mv = s.detail >= 50 ? (s.detail + 5u) / 10u * 10u : s.detail;
            char d[4]; unsigned k = 0, v = mv;
            word(K2_P("steps "));
            do { d[k++] = char('0' + v % 10); v /= 10; } while (v);
            while (k) put(d[--k]);
            word(K2_P("mV, not "));
            v = walk_step_mv(s.algorithm);
            do { d[k++] = char('0' + v % 10); v /= 10; } while (v);
            while (k) put(d[--k]);
            word("mV");
        }
    } else if (s.reason == Reason::ProgCheck && s.detail) {
        word(progcheck::flag_text(s.detail));
        const unsigned more = unsigned(std::popcount(unsigned(s.detail))) - 1;
        if (more) { word(" +"); put(char('0' + more)); }
    }
    out[n] = 0;
    return n;
}
const char* name(HuntCause c) {
    switch (c) {
    case HuntCause::None: return "none";
    case HuntCause::NoChain: return K2_P("no-chain");
    case HuntCause::ChainStalled: return K2_P("chain-stalled");
    case HuntCause::InsideChain: return K2_P("inside-chain");
    case HuntCause::RequestHeld: return K2_P("request-held");
    case HuntCause::FixedPdo: return K2_P("fixed-pdo");
    case HuntCause::CableCap: return K2_P("cable-cap");
    case HuntCause::ApdoStale: return K2_P("apdo-stale");
    case HuntCause::CableCapped5A: return K2_P("capped-5a");
    case HuntCause::RefusedDespiteOffer: return K2_P("refused-offer");
    case HuntCause::Unexplained: return K2_P("unexplained");
    case HuntCause::Count: break;
    }
    return "?";
}
namespace {
constexpr uint32_t kHoldMs = 1000;
constexpr uint32_t kHoldFloorMs = progcheck::kStableMs + progcheck::kPostMs + 100;   // below it the transition capture would outlive the hold (PD rows only)
constexpr uint32_t kHoldKnobMinMs = 300;   // the lab knob's own floor: a Legacy row judges after one stable band
constexpr uint32_t kTuneMaxMs = 60000;
constexpr uint32_t kChainMaxLevel = 1;
constexpr std::array<uint32_t, size_t(LegacyKnob::Count)> kTuneDefault = {0, kLegacySteadyMs, kLegacyFirstWaitMs, kLegacyGapMs, kHoldMs, 1};
std::array<uint32_t, size_t(LegacyKnob::Count)> g_tune = kTuneDefault;
uint32_t quiet_ms() { return g_tune[size_t(LegacyKnob::QuietMs)]; }
// The wait `want` ms after the Baseline's own 500 ms settle (stable_ms_ / pre_ms_ count the Baseline); a shorter want pulls the start back.
void baseline_wait(uint32_t want, uint32_t extra, uint32_t now, uint32_t& pre_ms, uint32_t& stable_ms) {
    pre_ms = (want > 500 ? want - 500 : 0) + extra;
    stable_ms = want < 500 ? now - (500 - want) : now;
}
constexpr uint32_t kSpecFeedMaxMs = 300;   // the spec stage's take of one move: a freeze and one Ui tick
// The walk's move in the detail byte, nudged 1 mV off the two row markers (kDetailStepWalk, kDetailSweep) so a finished row never reads as a sweep.
constexpr uint8_t step_walk_detail(int mv) {
    const int v = std::clamp(mv, 0, 255);
    return uint8_t(v == kDetailStepWalk ? v - 1 : v == kDetailSweep ? v + 1 : v);
}
// The transition capture (Env::prog_begin) is triggered where the hold starts, kStableMs after the band's stable start, and freezes its
// post window later; the hold ends kHoldMs after that stable start, with one 50 ms tick of slack at each end (hold_ms() is the one tick).
static_assert(progcheck::kStableMs + progcheck::kPostMs + 100 <= kHoldMs, "the capture freezes before the hold ends");
}  // namespace
uint32_t legacy_tune(LegacyKnob k) { return k < LegacyKnob::Count ? g_tune[size_t(k)] : 0; }
bool legacy_tune_set(uint8_t knob, uint32_t value) {
    if (knob >= size_t(LegacyKnob::Count) || value > (knob == uint8_t(LegacyKnob::Chain) ? kChainMaxLevel : kTuneMaxMs)) return false;
    if (knob == uint8_t(LegacyKnob::HoldMs) && value < kHoldKnobMinMs) return false;
    g_tune[knob] = value;
    return true;
}
void legacy_tune_reset() { g_tune = kTuneDefault; }
bool legacy_guard_override() { return quiet_ms() < link::kCapsStaleMs; }
namespace {
// "A Hard Reset happened on the link", whoever sent it: the sink counts the ones the K2 sent (hard_resets) and the ones the source
// sent (source_hard_resets) apart, and the runner only ever compares this sum with a value it took from the same sum.
uint32_t link_resets(const pd::SinkStatus& s) { return s.hard_resets + s.source_hard_resets; }

fc::Proto proto(Group g) {
    switch (g) {
    case Group::Qc2: return fc::Proto::Qc2;
    case Group::Qc3: return fc::Proto::Qc3;
    case Group::Qc3Plus: return fc::Proto::Qc3Plus;
    case Group::Afc: return fc::Proto::Afc;
    case Group::Fcp: return fc::Proto::Fcp;
    case Group::Scp: return fc::Proto::Scp;
    case Group::Vooc: return fc::Proto::Vooc;
    case Group::SuperVooc: return fc::Proto::SuperVooc;
    case Group::SuperVooc65: return fc::Proto::SuperVooc65;
    case Group::Ufcs: return fc::Proto::Ufcs;
    case Group::Vivo: return fc::Proto::Vivo;
    default: return fc::Proto::Count;
    }
}
bool safe5(int mv) { return pd::is_vsafe5(mv); }
// A cable nobody read is taken as 20 V / 3 A, except while the source has us in EPR mode: it only accepts EPR_Mode Enter after it
// has checked the cable itself (Enter_Failed data 1 otherwise), so that contract proves a 50 V / 5 A cable.
bool epr_proven(const Input& in) { return !in.cable_observed && (in.pd.epr_mode || in.epr_proof); }
uint16_t cable_mv(const Input& in) { return epr_proven(in) ? 50000 : std::min<unsigned>(in.cable_mv, in.cable_observed ? 50000 : 20000); }
constexpr unsigned kScpLowMa = 2250;   // Huawei SCP 10 V 2.25 A (22.5 W)
constexpr unsigned kUfcs60wMa = 3000;   // UFCS 20 V row: 20 V 3 A = 60 W
constexpr int kUfcsLevelMa = 1000;      // the levels planned from Output_Capabilities ask 1 A (or the mode's smallest current)
uint16_t cable_ma(const Input& in) { return epr_proven(in) ? 5000 : std::min<unsigned>(in.cable_ma, in.cable_observed ? 5000 : 3000); }
// PDO1 of the SPR list carries "EPR Mode Capable" (bit 23): the source offers EPR PDOs only after EPR_Mode entry.
bool epr_capable_caps(const pd::SourceCaps& c) {
    return c.valid && c.num_pdos && c.pdos[0].type == pd::PdoType::Fixed && (c.pdos[0].raw & (1u << 23));
}
// EPR rows need a 50 V / 5 A cable: one that was read as such, or one the source accepted EPR entry over (epr_proven).
bool epr_cable(const Input& in) { return (in.cable_observed || epr_proven(in)) && cable_mv(in) == 50000 && cable_ma(in) == 5000; }
// The cable limit a full-current or Xiaomi mode row is judged against (rule of e3cffbe3). Nothing physical read (and no EPR proof): a
// spoofed identity is the cable the source saw, so its VDO limits count; only a cable nobody presented falls back to 20 V / 3 A.
bool spoof_cable(const Input& in) { return !in.cable_observed && !epr_proven(in) && in.spoof_on && in.spoof_ma != 0; }
uint16_t used_cable_mv(const Input& in) { return spoof_cable(in) ? std::min<uint16_t>(in.spoof_mv ? in.spoof_mv : 20000, 50000) : cable_mv(in); }
uint16_t used_cable_ma(const Input& in) { return spoof_cable(in) ? std::min<uint16_t>(in.spoof_ma, 5000) : cable_ma(in); }
// What the cable is vouched for above 5 A (0 = nothing): a cable VDO tops out at 5 A, so only a read cable that says more, or the
// spoofed identity the run presents, lets a row ask beyond it (a PPS APDO of 6.25 A, e.g. Motorola TurboPower).
uint16_t over5_cable_ma(const Input& in) { return spoof_cable(in) ? in.spoof_ma : in.cable_observed ? in.cable_ma : uint16_t(0); }
// A cable that was read and is not 50 V / 5 A: no EPR entry is attempted. An unread cable does not block the attempt (the START
// skips the SOP' scan while a source's Rp is on the line), the source's own cable check answers it.
bool epr_cable_unfit(const Input& in) { return in.cable_observed && !epr_cable(in); }
// The PPS full-current row (plan_unit: the APDO's top voltage at its whole advertised current, clamped to the cable): when a
// re-announce changes the APDO's word after the plan (the Xiaomi 0x2717 chain lifts PPS 5-11 V 3 A to 5 A), its current is taken
// again from the word in hand, so the row asks the offer the source makes now. The 1 A rows, the Xiaomi mode rows, the step walk,
// the sweeps and an above-5 A row keep their own current.
bool full_current_row(const Step& s) {
    return s.group == Group::Pd && !s.hunt && !s.avs && s.detail == 0 && s.ma > kPlanRowMa && s.ma <= 5000 &&
           pd::decode_pdo(s.pdo).type == pd::PdoType::Pps;
}
// Place row `s` at slot `at` with offer word `raw`; a full-current row's current follows the word (and the cable, used_cable_ma).
void take_offer(Step& s, int at, uint32_t raw, const Input& in) {
    if (full_current_row(s) && s.pdo != raw)
        s.ma = static_cast<uint16_t>(std::min<unsigned>(pd::current_limit_ma(pd::decode_pdo(raw), s.mv), used_cable_ma(in)) / 50 * 50);
    s.pos = static_cast<uint8_t>(at + 1); s.pdo = raw;
}
// FNV-1a over the list (valid, count, raw PDO words): tells "the same offer list" from a list that grew or changed.
uint32_t caps_sig(const pd::SourceCaps& c) {
    uint32_t h = 2166136261u;
    const auto mix = [&h](uint32_t v) { for (unsigned n = 0; n < 4; ++n) { h ^= (v >> (8 * n)) & 0xFF; h *= 16777619u; } };
    mix(c.valid); mix(c.num_pdos);
    for (size_t n = 0; c.valid && n < std::min<size_t>(c.num_pdos, c.pdos.size()); ++n) mix(c.pdos[n].raw);
    return h;
}
// Where the list in hand offers what a planned row asked for: the row's own slot first, then any slot on the same side of the
// SPR / EPR boundary, preferring the row's own offer (same word, or same type and voltage range) over a different one that
// merely carries it (a source that announces again after the cable discovery can insert an APDO ahead of the one we planned,
// so a row whose offer only moved slot is re-queued, not withdrawn). -1 when nothing offers it any more.
// What "offers it" means per row kind (review 2026-11-09):
//  - a FIXED row is offered while its voltage is still offered, at ANY current: a source that drops 15 V 3 A -> 15 V 1.5 A
//    keeps the row, which then runs at the new, lower current word (the card notes the reduced current); only a voltage
//    that is no longer offered withdraws it;
//  - a programmable (PPS / AVS) row is offered when the type matches, the row's target voltage is inside the new range
//    and its current fits the new limit (now.max_ma >= s.ma).
int find_offer(const pd::SourceCaps& caps, const Step& s) {
    if (!caps.valid || !s.pos || !s.pdo) return -1;
    const size_t n = std::min<size_t>(caps.num_pdos, caps.pdos.size());
    const pd::Pdo planned = pd::decode_pdo(s.pdo);
    // Tier 1: the same offer - the raw word equal, or the same type and voltage range (a fixed PDO's min == max == its
    // voltage, so for fixed this already is "the same voltage at any current"; for programmable the word's own current
    // limit must not have dropped, like the old pdo_covers).
    const auto same = [&](size_t i) {
        const pd::Pdo& now = caps.pdos[i];
        if (now.raw == s.pdo) return true;
        if (now.type != planned.type || planned.type == pd::PdoType::Unknown) return false;
        if (now.min_mv != planned.min_mv || now.max_mv != planned.max_mv) return false;
        return planned.type == pd::PdoType::Fixed || now.max_ma >= planned.max_ma;
    };
    // Tier 2: a different offer that still carries the row - a fixed row: the same voltage at any current; a programmable
    // row: the type matches, the target is inside the new range and the current fits the new limit.
    const auto covers = [&](size_t i) {
        const pd::Pdo& now = caps.pdos[i];
        if (planned.type == pd::PdoType::Fixed) return now.type == pd::PdoType::Fixed && now.max_mv == planned.max_mv;
        if (planned.type == pd::PdoType::Pps || planned.type == pd::PdoType::Avs)
            return now.type == planned.type && s.mv >= now.min_mv && s.mv <= now.max_mv && now.max_ma >= s.ma;
        return false;
    };
    const bool epr = s.pos >= 8;
    const size_t lo = epr ? 7 : 0, hi = epr ? n : std::min<size_t>(n, 7);
    if (s.pos <= n && same(s.pos - 1u)) return s.pos - 1;   // the row's own slot, its own offer
    for (size_t i = lo; i < hi; ++i) if (same(i)) return static_cast<int>(i);   // the same offer moved to another slot
    if (s.pos <= n && covers(s.pos - 1u)) return s.pos - 1;
    for (size_t i = lo; i < hi; ++i) if (covers(i)) return static_cast<int>(i);
    return -1;
}
uint16_t cable_limit_mv(const Input& in, Algorithm algorithm) {
    const auto limit = cable_mv(in);
    // SPR PPS permits a 21 V request without promoting the physical cable to EPR.
    return algorithm == Algorithm::Pps && limit == 20000 ? 21000 : limit;
}
uint16_t measured_cable_limit_mv(const Input& in, Algorithm algorithm) {
    const auto limit = cable_limit_mv(in, algorithm);
    // The 20 V SPR cable class permits the 5% VBUS tolerance on a 20 V target, and on the 21 V PPS ceiling (22.05 V). The 50 V EPR
    // class permits it on the 48 V level (50.4 V): without it the cable guard (50.0 V) would trip before the monitor's overvoltage
    // abort (48 V ceiling + 5 % = 50.4 V) could be reached.
    return limit == 20000 ? 21000 : limit == 21000 ? 22050 : limit == 50000 ? 50400 : limit;
}
[[gnu::noinline]] Reason limit_reason(unsigned mv, const Input& in, Algorithm algorithm) {
    return !pd::fits_monitor(mv, in.cap_mv) ? Reason::OverCap : mv > cable_limit_mv(in, algorithm) ? Reason::CableLimit : Reason::None;
}
Reason refusal(Options o, Input in, bool busy, bool confirmed) {
    return busy || in.busy ? Reason::Busy : !(o.selected & kAllAlgorithms) ? Reason::EmptySelection :
        in.loop_lock ? Reason::Loop : !confirmed ? Reason::NeedConfirm : in.fake ? Reason::Fake : in.inline_load ? Reason::Inline :
        in.vbus_mv < 0 || in.cap_mv < 5000 ? Reason::NoMonitor : cable_mv(in) < 5000 ? Reason::CableLimit :
        in.vbus_mv < 1000 ? Reason::NoSource : Reason::None;
}
// A legacy row that asks a voltage of the charger (AFC, FCP, SCP, UFCS, vivo): pattern legacy-row-wide-band. A 5 V row is the vSafe5V
// window (a 5 V charger idles at 5.1-5.2 V). Any other level is +-5 % of it, 150 mV at least. A level below the one the rail
// starts from (`prev_mv`) gets the unloaded ceiling above instead (target + max(10 %, 600 mV), the programmable rule): an
// unloaded output cannot sink down, it stops somewhere above the request.
bool legacy_band(int level, int prev_mv, int mv) {
    if (level == 5000) return safe5(mv);
    const int tol = std::max(150, level / 20);
    return mv >= level - tol && mv <= (level < prev_mv ? pd::programmable_ceiling_mv(level) : level + tol);
}
// from_mv: VBUS when the level was asked (0 = unknown). A programmable row asked to go DOWN (from above its level) keeps the wide unloaded
// ceiling, an unloaded source cannot sink; a row asked to go UP (or to stay) is judged on the same +-5 % / 100 mV as a fixed level.
// prev_mv: the level a legacy row's rail starts from (legacy_band above); the two hints are independent.
bool in_band(const Step& s, int mv, int from_mv = 0, int prev_mv = 5000) {
    if (s.group == Group::Qc2 && s.mv == 5000) return safe5(mv);   // a 5 V charger reads 5.1 V: not a 2 % miss
    // An unloaded charger reads 2-3 % high (a bank: 9.20 V for 9 V, 5.16 V for 5 V); -2 % / +3 % around the level.
    if (s.group == Group::Qc2) return (mv - int(s.mv)) * 100 >= -int(s.mv) * 2 && (mv - int(s.mv)) * 100 <= int(s.mv) * 3;
    // QC3 / QC3+: the loop aims below the level and an overshoot is the risk, so the band is -300 / +150 mV (a 5 V row is the vSafe5V window).
    if (s.group == Group::Qc3 || s.group == Group::Qc3Plus) return s.mv == 5000 ? safe5(mv) : qc::in_pass_band(mv, int(s.mv));
    // VOOC rows ask for nothing: the adapter raises VBUS itself after the accepted cable check (the engine is Active
    // only then), anywhere from 5 V up to the 12 V it is released at.
    if (s.group == Group::Vooc || s.group == Group::SuperVooc || s.group == Group::SuperVooc65)
        return mv >= 4250 && mv <= 12100;
    if (s.mv == 5000 && (s.algorithm == Algorithm::Pd2Fixed || s.algorithm == Algorithm::Pd3Fixed))
        return safe5(mv);
    if (s.algorithm == Algorithm::Pps || s.algorithm == Algorithm::SprAvs || s.algorithm == Algorithm::EprAvs) {
        const int margin = std::max(100, int(s.mv) / 20);
        const bool down = from_mv > int(s.mv);   // an unloaded source cannot sink down: only a down-step gets the wide ceiling
        return mv >= int(s.mv) - margin && mv <= (down || from_mv <= 0 ? pd::programmable_ceiling_mv(int(s.mv)) : int(s.mv) + margin);
    }
    if (s.group == Group::Pd) return std::abs(mv - int(s.mv)) <= std::max(100, int(s.mv) / 20);
    return legacy_band(int(s.mv), prev_mv, mv);
}
// A fixed PD row above 5 V (SPR or EPR): its band is judged on the VbusMean average (pattern burst-mode-ripple-at-20v).
// 5 V rows keep the per-sample vSafe5V window, PPS / AVS keep their own one-sided band, QC / legacy rows are untouched.
bool averages_band(const Step& s) {
    return s.group == Group::Pd && (bit(s.algorithm) & kPdFixedAlgorithms) && s.mv != 5000;
}
// The 20 V level of a QC Class B row. A Class A charger (12 V max) never gets there, so a request or timeout failure on this level
// is a neutral "Class A only" skip. The 5 V / 12 V levels of the QC3 Class B ramp stay real rows (a failure there is a failure).
bool is_class_b_peak(const Step& s) {
    return (s.algorithm == Algorithm::Qc2ClassB || s.algorithm == Algorithm::Qc3ClassB) && s.mv == 20000;
}
// pattern qc3-20v-without-qc2-20v (Baseus CJ11, 2026-11-07): a charger can reach 20 V by QC3 Class B pulses yet ignore the discrete
// QC2 Class B 20 V level. Once a QC3 Class B 20 V row passed, the charger is Class B: the QC2 Class B 20 V miss is a real fail.
bool qc3_class_b_peak_passed(std::span<const Step> steps) {
    for (const auto& x : steps) if (x.algorithm == Algorithm::Qc3ClassB && x.mv == 20000 && x.verdict == Verdict::Pass) return true;
    return false;
}
// JT-P18 USB-C (2026-11-07): the port speaks no QC and QC2 5/9/12 V all failed "no handshake", yet the QC2 Class B 20 V row read "Class A
// only". That reason says the charger does QC2 and stops at 12 V, which needs a QC2 handshake that worked: a Class A row of the run
// that failed in the engine (Reason::Request) with none passed is the evidence it never did, and the Class B row carries that failure.
// A run with no QC2 Class A row to learn from (Class B selected alone, rows skipped) has no such evidence: the neutral skip stays.
// The silent first handshake is a skip "no reply" (never_answered), the same evidence as the failure it replaced.
bool qc2_never_handshook(std::span<const Step> steps) {
    bool failed = false;
    for (const auto& x : steps) {
        if (x.algorithm != Algorithm::Qc2) continue;
        if (x.verdict == Verdict::Pass) return false;
        failed |= x.reason == Reason::Request && (x.verdict == Verdict::Fail || (x.verdict == Verdict::Skip && no_reply(x.group, fc::Fail(x.detail))));
    }
    return failed;
}
// The Class B 20 V row of a charger that did not follow it ends as a neutral skip only where "Class A only" is a claim the run can back.
bool class_a_only(const Step& s, std::span<const Step> steps) {
    return is_class_b_peak(s) && !(s.algorithm == Algorithm::Qc2ClassB && qc2_never_handshook(steps));
}
// The default order is the company-family order of the wizard (docs/CHARGER_TEST_FLOW.md): the OEM vendor fast modes first
// (Huawei / Honor: FCP, SCP; Samsung: AFC; OPPO / OnePlus / vivo: VOOC, SVOOC, SVOOC65, VIVO), then USB PD (its rows end with the
// Xiaomi Mi 6A hunts), then the legacy / standard D+/D- protocols (QC2, QC3, QC3+, UFCS), and the risky low PPS/AVS rows last of
// all. The order within each family is kept.
constexpr Unit kDefaultUnits[] = {Unit::Fcp, Unit::Scp, Unit::Afc, Unit::Vooc, Unit::SuperVooc, Unit::SuperVooc65, Unit::Vivo,
                                  Unit::Pd, Unit::Qc2, Unit::Qc3, Unit::Qc3Plus, Unit::Ufcs, Unit::PdLow};
bool has_unit(const UnitList& l, Unit u) { for (size_t n = 0; n < l.n; ++n) if (l.u[n] == u) return true; return false; }
// The execution order: the list without duplicates (empty = the default order), PdLow never before Pd (its rows are the risky tail).
// A unit takes part when one of its algorithms is selected (Pd and PdLow both follow the PD algorithms).
bool unit_selected(Unit u, const Options& o) {
    const bool pd = u == Unit::Pd || u == Unit::PdLow;
    for (unsigned a = 0; a < unsigned(Algorithm::Count); ++a) {
        const auto algorithm = static_cast<Algorithm>(a);
        if (!o.contains(algorithm)) continue;
        const Group g = group(algorithm);
        if (pd ? g == Group::Pd : g == static_cast<Group>(u)) return true;
    }
    return false;
}
UnitList resolve(const Options& o) {
    const UnitList& in = o.units;
    UnitList out;
    // A Light run has no PdLow unit: its rows are the low edge of the APDOs (below 5 V), which Light lists, never asks.
    auto takes = [&](Unit u) { return unit_selected(u, o) && !(o.level == Level::Light && u == Unit::PdLow); };
    if (!in.n) { for (const Unit u : kDefaultUnits) if (takes(u)) out.add(u); return out; }
    for (size_t n = 0; n < in.n; ++n) if (!has_unit(out, in.u[n]) && takes(in.u[n])) out.add(in.u[n]);
    size_t pd = SIZE_MAX, low = SIZE_MAX;
    for (size_t n = 0; n < out.n; ++n) { if (out.u[n] == Unit::Pd) pd = n; if (out.u[n] == Unit::PdLow) low = n; }
    if (pd != SIZE_MAX && low != SIZE_MAX && low < pd) std::swap(out.u[pd], out.u[low]);
    return out;
}
// Where planned rows go. A function pointer plus a context instead of a template parameter: the planner is big, and one copy per
// caller (build, preview, replan_pd) cost about 4 KB of text. The callable must outlive the call (a temporary lambda does).
class AddRef {
public:
    template <class F, class = std::enable_if_t<!std::is_same_v<std::remove_cvref_t<F>, AddRef>>>
    AddRef(F&& f) : ctx_(const_cast<void*>(static_cast<const void*>(&f))),
                    fn_([](void* c, const Step& s) { (*static_cast<std::remove_reference_t<F>*>(c))(s); }) {}
    void operator()(const Step& s) const { fn_(ctx_, s); }
private:
    void* ctx_;
    void (*fn_)(void*, const Step&);
};
namespace {
struct PlanCtx {
    Unit unit;
    Options o;
    const Input& in;
    AddRef out;
    bool light;
    bool light_open = false;
    uint8_t light_pos = 0;
    Algorithm light_alg = Algorithm::Count;
    uint32_t light_pdo = 0;
    size_t n_low = 0;
    uint8_t low_pos = 0;
    unsigned sweep_budget = 0;
};

[[gnu::noinline]] void plan_add(PlanCtx& ctx, Step s) {
    if (!ctx.light) { ctx.out(s); return; }
    if (is_mi_chain(s)) { ctx.out(s); return; }   // nothing requested: the chain outcome is read at any level
    if (s.group == Group::Pd) {
        const bool same = s.pos && s.algorithm == ctx.light_alg && s.pos == ctx.light_pos;
        ctx.light_alg = s.algorithm; ctx.light_pos = s.pos;
        if (same) return;
        if (s.detail == kDetailEprOffered) { s.reason = Reason::Advertised; s.detail = 0; }   // listed in SPR mode, never entered
        else if (s.verdict == Verdict::Pending) {
            const auto p = pd::decode_pdo(s.pdo);
            s.verdict = Verdict::Skip; s.reason = Reason::Advertised; s.mv = p.max_mv; s.ma = p.max_ma;
            s.continuous = s.hunt = false; s.detail = 0;
        }
        ctx.out(s); return;
    }
    if (s.verdict != Verdict::Pending) { ctx.out(s); return; }
    const bool above_only = s.group == Group::Afc || s.group == Group::Qc3Plus ||
                            s.algorithm == Algorithm::Qc2ClassB || s.algorithm == Algorithm::Qc3ClassB;
    if (!above_only && s.mv == 5000) { if (!ctx.light_open) ctx.out(s); ctx.light_open = true; return; }   // a later 5 V row adds nothing
    if (s.group == Group::Ufcs && s.pdo) {   // a mode the adapter reported: one row, at its top
        if (s.pdo == ctx.light_pdo) return;
        ctx.light_pdo = s.pdo;
        if (UfcsMode m; ufcs_mode_of(s, m)) { s.mv = uint16_t(m.max_mv); s.ma = uint16_t(m.max_ma); }
        s.verdict = Verdict::Skip; s.reason = Reason::Advertised; s.continuous = false;
        ctx.out(s); return;
    }
    s.verdict = Verdict::Skip;
    s.reason = s.group == Group::Scp && ctx.in.fc_caps && ctx.in.fc_caps->scp.valid ? Reason::Advertised : Reason::LightLevel;
    ctx.out(s);
}

[[gnu::noinline]] void plan_emit(PlanCtx& ctx, const Step& s) {
    const bool low = low_programmable(s);
    if (ctx.unit == Unit::PdLow ? low : !low) plan_add(ctx, s);
}

[[gnu::noinline]] void plan_skip(PlanCtx& ctx, Step& s, Reason r) {
    s.verdict = Verdict::Skip; s.reason = r;
    if (r == Reason::OverCap && (s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs)) s.detail = monitor_detail(ctx.in.cap_mv);
    plan_emit(ctx, s);
}

[[gnu::noinline]] void plan_level(PlanCtx& ctx, Step& s, unsigned mv, bool continuous = false, unsigned ma = 0, bool offered = true) {
    s.mv = static_cast<uint16_t>(mv); s.continuous = continuous; s.ma = static_cast<uint16_t>(ma);
    s.reason = limit_reason(mv, ctx.in, s.algorithm);
    if (s.reason == Reason::None && !offered) s.reason = Reason::NoOffer;   // outside what the charger reported
    s.verdict = s.reason == Reason::None ? Verdict::Pending : Verdict::Skip;
    plan_add(ctx, s);
}

[[gnu::noinline]] void plan_levels(PlanCtx& ctx, Step& s, const uint16_t* mvs, size_t count, bool cont_rest, bool cont_first = false) {
    for (size_t n = 0; n < count; ++n) plan_level(ctx, s, mvs[n], n ? cont_rest : cont_first);
}

[[gnu::noinline]] uint16_t pdo_current(const pd::Pdo& p, const Input& in, unsigned mv, unsigned ceiling = kPlanRowMa) {
    return static_cast<uint16_t>(std::min({ceiling, unsigned(pd::current_limit_ma(p, mv)),
                                           unsigned(used_cable_ma(in))}) / 50 * 50);
}

[[gnu::noinline]] void plan_emit_sweep(PlanCtx& ctx, Step& s, const pd::Pdo& p, unsigned cap, bool pps) {
    if (!ctx.sweep_budget) return;
    const auto pr = ctsweep::point_range(p.min_mv, p.max_mv, cap);
    if (!ctsweep::point_count(pr)) { s.detail = kDetailSweep; plan_skip(ctx, s, limit_reason(p.max_mv, ctx.in, s.algorithm)); return; }
    const unsigned cable = cable_ma(ctx.in);
    for (int k = 0; k < (pps ? 2 : 1); ++k) {
        const auto part = pps ? ctsweep::part_range(pr, k != 0) : pr;
        const unsigned n = ctsweep::point_count(part);
        if (!n || !ctx.sweep_budget) continue;
        Step w = s;
        w.mv = static_cast<uint16_t>(part.lo);
        w.ma = static_cast<uint16_t>(ctsweep::point_ma(p, part.lo, cable));
        w.continuous = false;
        w.verdict = w.ma ? Verdict::Pending : Verdict::Skip;
        w.reason = w.ma ? Reason::None : Reason::NoOffer;
        w.detail = kDetailSweep;   // the runner's marker; it survives finish(), so the results strip finds the row
        plan_emit(ctx, w);
        ctx.sweep_budget -= std::min(n, ctx.sweep_budget);
    }
}
[[gnu::noinline]] Reason base_fault_core(const Input& in) {
    return in.vbus_mv < 0 || in.cap_mv < 5000 ? Reason::NoMonitor
         : in.loop_lock ? Reason::Loop
         : in.inline_load ? Reason::Inline
         : Reason::None;
}
[[gnu::noinline]] Reason base_fault(const Input& in) {
    const Reason r = base_fault_core(in);
    return r != Reason::None ? r : in.fake ? Reason::Fake : Reason::None;
}
[[gnu::noinline]] bool pd_released(const pd::SinkStatus& pd) {
    return !pd::user_hold(pd) && !pd.epr_mode &&
           (pd.state == pd::SinkState::Idle || pd.state == pd::SinkState::Failed ||
            (pd.state == pd::SinkState::Ready && pd.mv == 5000));
}
}  // namespace

// The rows of one unit, in plan order. Pd and PdLow share the PD plan: the low programmable rows (below 5 V) are the PdLow unit.
void plan_unit(Unit unit, const pd::SourceCaps& caps, Options o, const Input& in, AddRef out) {
    // Light (Options::level, with_level): VBUS stays at 5 V. The PD plan asks nothing: every PDO / APDO the source lists (the 5 V
    // one too) is one Advertised row naming the whole offer (by its top), read from the list in hand. Each legacy unit's first
    // 5 V row (its handshake) runs; every other offer is one Advertised row (a UFCS mode by its top, an SCP level inside the limits
    // the adapter reported); a legacy level nothing advertises, and a protocol only told apart above 5 V (AFC, QC3+, QC Class B),
    // is a LightLevel skip. No hunts, walks, sweeps or full-current rows (one row per offer). Rows that already carry a reason
    // (not offered, over the monitor, the cable) keep it.
    PlanCtx ctx{unit, o, in, out, o.level == Level::Light};
    ctx.sweep_budget = (o.pps_sweep || o.avs_sweep) ? ctsweep::kMaxPoints : 0;   // the deep sweeps' point record, shared by the APDOs
    const bool pd_unit = unit == Unit::Pd || unit == Unit::PdLow;
    for (const auto algorithm : kPlanOrder) {
        if (!o.contains(algorithm)) continue;
        Step s; s.algorithm = algorithm; s.group = group(algorithm);
        if (pd_unit ? s.group != Group::Pd : s.group != static_cast<Group>(unit)) continue;
        const unsigned cap = std::min<unsigned>(in.cap_mv, cable_limit_mv(in, algorithm));
        if (algorithm >= Algorithm::Pd30Revision && algorithm <= Algorithm::Pd32Revision) { plan_skip(ctx, s, Reason::RevisionUntestable); continue; }
        if (s.group != Group::Pd) {
            // pattern qc-redetect-after-reset (power strip USB-A, 2026-11-07): a QC row that opened its own session started 0.5-0.7 s
            // after the previous row's reset of D+/D- and got no handshake, right after the same charger passed the row before it
            // (QC2 12 V after 9 V, the QC3 Class B 5 V after the QC3 unit). So one HVDCP handshake per unit: the first QC2 row (5 V)
            // and the first QC3 row open the session, every later level is a retarget in it (QC2: the D+/D- levels, QC3: pulses), and
            // the Class B rows of the unit (default-selected, own algorithms) go on in the same session when their Class A rows are in the plan. A Class B row
            // selected alone opens its own. The engine takes 20 V in a session that started as QC2 / QC3 (qc2_levels, clamp_qc3_mv).
            static constexpr uint16_t kQc3b[] = {5000, 12000, 20000, 12000, 5000};
            static constexpr uint16_t kQc3[] = {5000, 9000, 12000, 9000, 5000};
            static constexpr uint16_t kQc2[] = {5000, 9000, 12000};
            static constexpr uint16_t kAfc[] = {9000, 12000};
            static constexpr uint16_t kVivo[] = {5000, 9000, 11000, 5000};
            static constexpr uint16_t kUfcsDef[] = {5000, 9000, 11000, 5000, 12000};
            if (algorithm == Algorithm::Qc2ClassB) { plan_level(ctx, s, 20000, o.contains(Algorithm::Qc2)); continue; }   // Class B (20 V) fixed level
            if (algorithm == Algorithm::Qc3ClassB) { plan_levels(ctx, s, kQc3b, std::size(kQc3b), true, o.contains(Algorithm::Qc3)); continue; }
            switch (s.group) {
            case Group::Qc2: plan_levels(ctx, s, kQc2, std::size(kQc2), true); break;
            case Group::Qc3:
            case Group::Qc3Plus: plan_levels(ctx, s, kQc3, std::size(kQc3), true); break;
            case Group::Afc: plan_levels(ctx, s, kAfc, std::size(kAfc), true); break;
            case Group::Fcp: plan_levels(ctx, s, kQc2, std::size(kQc2), true); break;   // 5000, 9000, 12000
            case Group::Ufcs: {
                const size_t modes = in.fc_caps ? std::min<size_t>(in.fc_caps->ufcs_n, in.fc_caps->ufcs.size()) : 0;
                if (!modes) {
                    plan_levels(ctx, s, kUfcsDef, std::size(kUfcsDef), true);
                    plan_level(ctx, s, 20000, true, kUfcs60wMa);
                    break;
                }
                const ufcs::Cap* by_min[ufcs::kMaxCaps];
                for (size_t n = 0; n < modes; ++n) {   // lowest mode first (a handful of entries: insertion sort)
                    const ufcs::Cap* c = &in.fc_caps->ufcs[n];
                    size_t j = n;
                    for (; j > 0 && by_min[j - 1]->min_mv > c->min_mv; --j) by_min[j] = by_min[j - 1];
                    by_min[j] = c;
                }
                unsigned last = 5000;
                auto ma_of = [](const ufcs::Cap& c) { return static_cast<unsigned>(std::clamp(kUfcsLevelMa, c.min_ma, std::max(c.min_ma, c.max_ma))); };
                auto tag = [&](const ufcs::Cap& c) { s.pdo = pack_ufcs_mode(c.min_mv, c.max_mv, c.min_ma, c.max_ma); };
                auto next_mv = [&](unsigned mv, const ufcs::Cap& c) { if (mv != last) { last = mv; tag(c); plan_level(ctx, s, mv, true, ma_of(c)); } };
                const ufcs::Cap* home = by_min[0];
                for (size_t n = 0; n < modes; ++n)
                    if (by_min[n]->min_mv <= 5000 && 5000 <= by_min[n]->max_mv) home = by_min[n];
                tag(*home); plan_level(ctx, s, 5000);
                for (size_t n = 0; n < modes; ++n) {
                    const ufcs::Cap& c = *by_min[n];
                    const unsigned bottom = static_cast<unsigned>(std::max(c.min_mv, 0));
                    const unsigned top = std::min<unsigned>(static_cast<unsigned>(std::max(c.max_mv, 0)), cap);
                    next_mv(bottom, c);
                    if (bottom >= cap || top <= bottom) continue;
                    const unsigned mid = (bottom + top) / 2 / 100 * 100;
                    if (mid > bottom && mid < top) next_mv(mid, c);
                    next_mv(top, c);
                }
                next_mv(5000, *home);
                break;
            }
            case Group::Vivo: plan_levels(ctx, s, kVivo, std::size(kVivo), false); break;
            case Group::Scp: {
                if (!in.fc_caps || !in.fc_caps->scp.valid) {
                    plan_level(ctx, s, 5000); plan_level(ctx, s, 10000, false, kScpLowMa);
                    plan_level(ctx, s, 4500, false, 5000); plan_level(ctx, s, 5000, false, 4500);
                    break;
                }
                const auto& lim = in.fc_caps->scp;
                auto inside = [&](unsigned mv) { return int(mv) <= lim.max_mv; };
                const auto ask = [&](unsigned ma) { return std::min<unsigned>(ma, static_cast<unsigned>(lim.max_ma)); };
                const unsigned low_ma = ask(kScpLowMa);
                const unsigned top = std::min<unsigned>(static_cast<unsigned>(lim.max_mv), cap);
                plan_level(ctx, s, 5000);
                plan_level(ctx, s, 10000, false, low_ma, inside(10000));
                if (top > 5000 && top != 10000) plan_level(ctx, s, top, false, low_ma, inside(top));
                plan_level(ctx, s, 4500, false, ask(5000), inside(4500));
                plan_level(ctx, s, 5000, false, ask(4500), inside(5000));
                break;
            }
            default: plan_level(ctx, s, 5000); break;
            }
            continue;
        }
        // No source list: a self-powered K2 that heard PD in this attach knows the source gave it up (its caps went unanswered
        // while the K2 was PD-silent), which is a different finding from a charger that never advertised PD.
        if (!caps.valid) { plan_skip(ctx, s, in.self_powered && in.pd_seen_attach ? Reason::PdGaveUp : Reason::NoOffer); continue; }
        const unsigned wire = algorithm == Algorithm::Pd2Fixed ? 1 : 2;
        if (caps.spec_rev != wire) {
            if ((algorithm == Algorithm::Pd2Fixed || algorithm == Algorithm::Pd3Fixed) &&
                o.contains(Algorithm::Pd2Fixed) && o.contains(Algorithm::Pd3Fixed) &&
                (caps.spec_rev == 1 || caps.spec_rev == 2)) continue;
            plan_skip(ctx, s, Reason::RevisionUntestable); continue;
        }
        if ((algorithm == Algorithm::Epr || algorithm == Algorithm::EprAvs) && !caps.epr) {
            // The charger listed its EPR levels without an entry (EPR_Get_Source_Cap in SPR mode): one row per level, named, never run.
            const pd::SourceCaps* info = in.epr_info;
            bool listed = false;
            for (size_t i = 7; info && epr_capable_caps(caps) && i < std::min<size_t>(info->num_pdos, info->pdos.size()); ++i) {
                const auto& p = info->pdos[i];
                if (!p.raw || p.type != (algorithm == Algorithm::Epr ? pd::PdoType::Fixed : pd::PdoType::Avs)) continue;
                listed = true;
                s.pos = static_cast<uint8_t>(i + 1); s.pdo = p.raw; s.mv = p.max_mv; s.ma = 0; s.continuous = false;
                s.avs = p.type == pd::PdoType::Avs; s.detail = kDetailEprOffered;
                plan_skip(ctx, s, Reason::CableLimit);
            }
            if (listed) continue;
            // An answer with no EPR PDO at all (positions 8+ all zero; zero words are padding, USB PD R3.2 EPR_Source_Capabilities) from a source whose PDO 1 has the EPR
            // bit: the list exists but the port holds it back without an EPR cable. The row says that instead of "not offered".
            bool any_epr = false;
            for (size_t i = 7; info && i < std::min<size_t>(info->num_pdos, info->pdos.size()); ++i) any_epr |= info->pdos[i].raw != 0;
            const bool withheld = info && epr_capable_caps(caps) && !any_epr && !epr_cable_unfit(in);
            if (withheld) s.detail = kDetailEprWithheld;
            plan_skip(ctx, s, epr_capable_caps(caps) && (withheld || epr_cable_unfit(in)) ? Reason::CableLimit : Reason::NoOffer); continue;
        }
        bool found = false;
        // The fixed rows (SPR fixed, and the EPR fixed ones in their own section) run from the top voltage down (2026-11-17: a shared-port
        // splitter re-splits its budget a few seconds after it announces and withdraws the high levels, so those are asked first; the 5 V
        // row, the attach contract's own level, comes last). Each row still starts from the released 5 V contract, so every request is an
        // up-step from 5 V. The Results grid keeps ascending voltages (result_layout fixed_row); ranges and sweeps keep their order.
        const size_t n_pdo = std::min<size_t>(caps.num_pdos, caps.pdos.size());
        const bool top_down = algorithm == Algorithm::Pd2Fixed || algorithm == Algorithm::Pd3Fixed || algorithm == Algorithm::Epr;
        for (size_t k = 0; k < n_pdo; ++k) {
            const size_t i = top_down ? n_pdo - 1 - k : k;
            const auto& p = caps.pdos[i];
            if (!p.raw) continue;
            const bool epr = i >= 7;
            const bool belongs = algorithm == Algorithm::Epr ? epr && p.type == pd::PdoType::Fixed :
                algorithm == Algorithm::EprAvs ? epr && p.type == pd::PdoType::Avs :
                algorithm == Algorithm::Pps ? !epr && p.type == pd::PdoType::Pps :
                algorithm == Algorithm::SprAvs ? !epr && p.type == pd::PdoType::Avs :
                !epr && p.type != pd::PdoType::Pps && p.type != pd::PdoType::Avs;
            if (!belongs) continue;
            found = true;
            s.pos = static_cast<uint8_t>(i + 1);
            s.pdo = p.raw;
            s.mv = p.max_mv;
            s.ma = 0;
            s.continuous = false; s.verdict = Verdict::Pending; s.reason = Reason::None; s.detail = 0;
            s.avs = p.type == pd::PdoType::Avs;
            if ((algorithm == Algorithm::Epr || algorithm == Algorithm::EprAvs) && !epr_cable(in)) { plan_skip(ctx, s, Reason::CableLimit); continue; }
            if ((s.avs && (p.raw >> 28) != (epr ? 0xD : 0xE)) ||
                (epr && p.type != pd::PdoType::Fixed && !s.avs)) {
                plan_skip(ctx, s, Reason::UnsupportedOffer); continue;
            }
            if (p.type != pd::PdoType::Fixed && p.type != pd::PdoType::Pps && !s.avs) {
                plan_skip(ctx, s, Reason::NotImplemented); continue;
            }
            if (!p.min_mv || p.max_mv < p.min_mv) { plan_skip(ctx, s, Reason::UnsupportedOffer); continue; }
            s.ma = pdo_current(p, in, s.mv);
            if (p.min_mv > cap || (p.type == pd::PdoType::Fixed && p.max_mv > cap)) {
                plan_skip(ctx, s, limit_reason(p.max_mv, in, algorithm)); continue;
            }
            if (p.type == pd::PdoType::Fixed) {
                if (!s.ma) plan_skip(ctx, s, Reason::NoOffer); else plan_emit(ctx, s);
                continue;
            }
            const unsigned quantum = s.avs ? 100 : 20;
            const unsigned lo = (p.min_mv + quantum - 1) / quantum * quantum;
            const unsigned hi = std::min<unsigned>(p.max_mv, cap) / quantum * quantum;
            if (lo > hi) { plan_skip(ctx, s, limit_reason(p.max_mv, in, algorithm)); continue; }
            const unsigned mid0 = ((lo + hi) / 2) / quantum * quantum;
            const unsigned mid = std::max(mid0, std::min<unsigned>(5000 / quantum * quantum, hi));
            const unsigned low_mid = std::max(lo, ((lo + mid) / 2) / quantum * quantum);
            const auto walk_row = [&] {
                if (mid <= 5000 || mid + kStepWalkCount * quantum > hi) return;
                Step w = s; w.mv = static_cast<uint16_t>(mid); w.ma = pdo_current(p, in, mid); w.continuous = false;
                w.verdict = w.ma ? Verdict::Pending : Verdict::Skip;
                w.reason = w.ma ? Reason::None : Reason::NoOffer;
                w.detail = kDetailStepWalk;   // the runner's marker while the row is in flight
                plan_emit(ctx, w);
            };
            const unsigned targets[] = {mid, hi, mid, low_mid, lo};
            for (unsigned n = 0; n < 5; ++n) {
                s.mv = static_cast<uint16_t>(targets[n]); s.ma = pdo_current(p, in, s.mv); s.continuous = n != 0;
                s.verdict = s.ma ? Verdict::Pending : Verdict::Skip;
                s.reason = s.ma ? Reason::None : Reason::NoOffer;
                if (s.ma && low_programmable(s)) {
                    if (!ctx.n_low || ctx.low_pos != s.pos) s.continuous = false;   // the first one starts from a released 5 V contract
                    ++ctx.n_low; ctx.low_pos = s.pos;
                }
                plan_emit(ctx, s);
            }
            if (p.max_mv > cap) {
                s.mv = p.max_mv; s.ma = pdo_current(p, in, s.mv); s.continuous = false;
                plan_skip(ctx, s, limit_reason(p.max_mv, in, algorithm));
            }
            if (p.type == pd::PdoType::Pps) {
                const unsigned advertised = pd::current_limit_ma(p, static_cast<uint16_t>(hi)) / 50 * 50;
                const bool over5 = advertised > 5000;
                auto emit_full = [&](uint16_t ma) {
                    Step f = s; f.mv = static_cast<uint16_t>(hi); f.ma = ma; f.hunt = false;
                    f.continuous = false; f.verdict = Verdict::Pending; f.reason = Reason::None;
                    plan_emit(ctx, f);
                };
                if (over5 && over5_cable_ma(in) > 5000) {
                    emit_full(static_cast<uint16_t>(advertised));
                    continue;
                }
                const uint16_t full = pdo_current(p, in, hi, ~0u);
                if (full > pdo_current(p, in, hi)) emit_full(full);
                if (over5) {
                    s.mv = static_cast<uint16_t>(hi); s.ma = static_cast<uint16_t>(advertised); s.hunt = false; s.continuous = false;
                    plan_skip(ctx, s, Reason::CableLimit);
                }
                if (o.pps_sweep) walk_row();
                if (o.pps_sweep) plan_emit_sweep(ctx, s, p, cap, true);
            }
            if (s.avs && o.avs_sweep) {
                walk_row();
                plan_emit_sweep(ctx, s, p, cap, false);
            }
        }
        if (!found) {
            s = {}; s.algorithm = algorithm; s.group = Group::Pd;
            plan_skip(ctx, s, Reason::NoOffer);
        }
    }
    // The Xiaomi unit's chain row, last of the Pd unit: the modes it finds run in the same attach (the source keeps the unlocked list
    // only until the next detach). Without SVID 0x2717 it reads "no 0x2717 SVID" when its turn comes.
    // It rides the fixed or the PPS selection (the modes it finds are fixed or PPS levels) and carries the first one selected.
    if (unit == Unit::Pd && (o.contains(Algorithm::Pd3Fixed) || o.contains(Algorithm::Pps))) {
        Step m; m.group = Group::Pd; m.algorithm = o.contains(Algorithm::Pd3Fixed) ? Algorithm::Pd3Fixed : Algorithm::Pps; m.hunt = true;
        plan_add(ctx, m);
    }
}
// Row layout: the units in execution order, each with its own rows. The PD rows (Pd, PdLow) can be re-planned when the source list
// arrives late (replan_pd); `placeholders` stands one unplanned row in for each PD unit until then.
void plan_pd_unit(Unit u, const pd::SourceCaps& caps, Options o, const Input& in, AddRef add, bool placeholders) {
    if (!placeholders) { plan_unit(u, caps, o, in, add); return; }
    Step s; s.group = Group::Pd; s.algorithm = u == Unit::Pd ? Algorithm::Pd3Fixed : Algorithm::Pps; s.mv = u == Unit::Pd ? 5000 : 3300;
    add(s);
}
void plan_units(const pd::SourceCaps& caps, Options o, const Input& in, const UnitList& units, AddRef add, bool placeholders = false) {
    for (size_t n = 0; n < units.n; ++n) {
        if (mode(units.u[n]) == Mode::Pd) plan_pd_unit(units.u[n], caps, o, in, add, placeholders);
        else plan_unit(units.u[n], caps, o, in, add);
    }
}
// A cheap fingerprint of the row layout (what each row is, not how it went), to tell an identical re-plan from a changed one.
uint32_t layout_sig(std::span<const Step> rows) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
    mix(uint32_t(rows.size()));
    for (const auto& s : rows) { mix(uint32_t(s.algorithm)); mix(uint32_t(s.group)); mix(s.mv); mix(uint32_t(unit_of(s))); }
    return h;
}
}
UnitList unit_order(const Options& o) { return resolve(o); }
// The PD spec stage (spec_section.hpp) is planned for a Deep run that has the full Pd unit, on a build that links the engine.
static bool spec_planned(const Options& o, const UnitList& units) {
    return specsec::kBuilt && o.spec && o.level == Level::Deep && has_unit(units, Unit::Pd);
}
Summary Runner::preview(const pd::SourceCaps& caps, Options o, Input in) {
    Summary out;
    in.epr_info = nullptr;   // a preview plans from the source list alone
    in.fc_caps = nullptr;
    out.refusal = refusal(o, in, false, true);
    if (out.refusal == Reason::None && !safe5(in.vbus_mv)) {
        // VBUS outside vSafe5V: under a live PDO 1 contract (PS_RDY at position 1) that is the wrong-voltage case
        // (pattern shared-bus-split) and is recorded with the measured level - not a plain unsafe VBUS.
        out.refusal = pdo1_contract(in.pd) ? Reason::WrongVoltage : Reason::Release;
        out.refusal_mv = uint16_t(std::clamp(in.vbus_mv, 0, 65535));
    }
    const UnitList units = resolve(o);
    uint32_t seen = 0;
    auto count = [&](const Step& s) {
        if (out.total >= kMaxSteps) return;   // the Runner's add() drops past kMaxSteps: the preview counts the same rows
        // The Xiaomi chain row sends nothing: it counts as a test only on a source that lists SVID 0x2717 (its modes are found at
        // run time, so the preview cannot count them).
        const bool no_chain = is_mi_chain(s) && !in.pd.xiaomi_svid;
        ++out.total;
        if (s.verdict == Verdict::Skip || no_chain) ++out.skipped;
        else {
            ++out.runnable; out.max_mv = std::max(out.max_mv, s.mv);
            out.estimate_ms += kHoldMs + 1000;
            if (is_sweep(s)) {   // a sweep row is long by design: kSweepPointMs per measured point
                const unsigned cap = std::min<unsigned>(in.cap_mv, cable_limit_mv(in, s.algorithm));
                out.estimate_ms += ctsweep::plan_points(sweep_plan(s, cap, o.sweep_depth, caps)) * kSweepPointMs;
            }
            const unsigned u = static_cast<unsigned>(unit_of(s));
            if (!(seen & (1u << u))) { seen |= 1u << u; out.estimate_ms += 4000; }   // one detach + attach per unit
        }
    };
    plan_units(caps, o, in, units, count);
    if (o.level == Level::Light && has_unit(units, Unit::Pd) && out.total < kMaxSteps) {
        // Light asks the PD source nothing but its capabilities: the list in hand (and the EPR levels of an EPR-capable PDO 1, asked
        // once in SPR mode) is one test, riding the Pd unit's attach (the Xiaomi chain row's, which sends nothing).
        ++out.runnable; ++out.total;
        out.estimate_ms += 5000;
    }
    if (spec_planned(o, units)) {
        out.spec_checks = static_cast<uint8_t>(specsec::kCells);
        out.estimate_ms += specsec::estimate_ms(pdspec::scan(caps, in.cap_mv, false), o.pps_sweep, o.avs_sweep);
    }
    return out;
}
pd::SourceCaps nominal_caps() {
    pd::SourceCaps c; c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.num_pdos = 7;
    constexpr unsigned kVolts[] = {5000, 9000, 12000, 15000, 20000};
    for (unsigned n = 0; n < 5; ++n) c.pdos[n] = pd::decode_pdo(((kVolts[n] / 50) << 10) | (n == 4 ? 500 : 300));
    c.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (50u << 8) | 60);   // PPS 3.3-11 V 3 A
    c.pdos[6] = pd::decode_pdo(0xc0000000 | (200u << 17) | (50u << 8) | 60);   // PPS 3.3-20 V 3 A
    return c;
}
unsigned planned_tests(const pd::SourceCaps& caps, Options o, Input in) {
    const auto s = Runner::preview(caps.valid ? caps : nominal_caps(), o, in);
    return unsigned(s.runnable) + s.spec_checks;
}
void Runner::add(Step s) {
    if (count_ < steps_.size()) steps_[count_++] = s;
}
// The entry's own answer lands on every EPR row. A level the charger listed (kDetailEprOffered) keeps what it was heard to offer:
// it stays a CableLimit row and the Enter_Failed data joins its detail (kDetailEprOfferedRefused + data); any other answer
// (no contract, ...) leaves it alone.
void Runner::epr_skips(Reason r, uint8_t detail) {
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.algorithm != Algorithm::Epr && s.algorithm != Algorithm::EprAvs) continue;
        if (s.verdict == Verdict::Skip && s.detail == kDetailEprOffered) {
            if (r == Reason::EprRefused && detail >= kDetailEprFail && detail <= kDetailEprFail + 5)
                s.detail = uint8_t(kDetailEprOfferedRefused + (detail - kDetailEprFail));
            continue;
        }
        s.verdict = Verdict::Skip; s.reason = r; s.detail = detail;
    }
    for (const auto& s : steps()) if (s.verdict == Verdict::Skip) env_.record(s);
    pd_skips_recorded_ = true;
}
bool Runner::info_ready(const Input& in) const {
    return in.pd.epr_info == pd::EprInfo::Got && in.epr_info && in.epr_info->num_pdos > 7;
}
// Ask the charger for its EPR list without entering EPR mode (once per Pd attach). The answer (a list, a refusal, silence) changes
// no verdict by itself: it only names the levels the EPR rows stand for.
bool Runner::ask_info(const Input& in) {
    if (info_ == 3) return true;
    if (info_ == 0) { info_ = 1; info_ms_ = in.now; }
    if (info_ == 1) {
        const auto err = env_.epr_info_ask();
        if (err == pd::Err::Ok) { info_ = 2; info_ms_ = in.now; return false; }
        if (err == pd::Err::Busy && in.now - info_ms_ < kEprInfoBusyMs) return false;
        info_ = 3; return true;   // the sink cannot ask (or kept the line busy): no list, the old rows
    }
    if (in.pd.epr_info == pd::EprInfo::Pending && in.now - info_ms_ < pd::kEprInfoMs + 500) return false;
    info_ = 3; return true;
}
// With the informational list in hand the EPR rows stand for the levels the charger offers; without it nothing changes.
void Runner::epr_info_rows(const pd::SourceCaps& caps, const Input& in) {
    if (info_ != 3 || !info_ready(in) || !epr_capable_caps(caps)) return;
    info_rows_ = true;
    replan_pd(caps, in, false);
}
// An EPR row of an SPR list whose PDO 1 carries the EPR bit is skipped NoOffer by the plan only as a placeholder: the entry (or the
// informational list, or a refusal) rewrites it. The journal gets it only when the run ends with it still standing (final), never
// while the entry is open: a card read after a power loss would otherwise say "EPR: not offered" about a charger that offers EPR
// (Baseus 140 W on its own cable, 2026-11-17).
void Runner::record_pd_skips(bool final) {
    pd_skips_held_ = false;
    for (size_t n = 0; n < count_; ++n) {
        const Step& s = steps_[n];
        if (s.verdict != Verdict::Skip || mode(unit_of(s)) != Mode::Pd) continue;
        if (!final && s.reason == Reason::NoOffer && (s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) &&
            (evidence_.flags & EprCapable) && !(evidence_.flags & EprTable)) { pd_skips_held_ = true; continue; }
        env_.record(s);
    }
    pd_skips_recorded_ = true;
}
void Runner::build(const pd::SourceCaps& caps, Options o, Input in) {
    in.epr_info = nullptr;
    in.fc_caps = nullptr;   // nothing is known of this charger's SCP / UFCS limits until a row of the unit ran (replan_fc)
    fc_replanned_ = 0;
    count_ = index_ = 0;
    sweep_used_ = 0; n_sweep_rows_ = 0; sweep_n_ = 0; sweep_mv_ = sweep_lo_ = sweep_hi_ = 0;   // a fresh plan re-sweeps from scratch
    plan_units(caps, o, in, o.units, [this](Step s) { add(s); }, !caps.valid);
}
void Runner::caps_evidence(const pd::SourceCaps& caps) {
    evidence_.wire_rev = caps.valid ? caps.spec_rev : 0;
    evidence_.raw_header = caps.raw_header; evidence_.num_pdos = caps.valid ? caps.num_pdos : 0;
    evidence_.flags &= static_cast<uint8_t>(~(PpsOffer | EprTable | SprAvsOffer | EprCapable | PartialEpr));
    evidence_.flags |= (caps.epr ? EprTable : 0) | (caps.epr_partial ? PartialEpr : 0);
    for (size_t n = 0; caps.valid && n < std::min<size_t>(caps.num_pdos, caps.pdos.size()); ++n) {
        const auto& p = caps.pdos[n];
        if (n < 7 && p.type == pd::PdoType::Pps) evidence_.flags |= PpsOffer;
        if (n < 7 && p.type == pd::PdoType::Avs && (p.raw >> 28) == 0xE) evidence_.flags |= SprAvsOffer;
        if (n == 0 && p.type == pd::PdoType::Fixed && (p.raw & (1u << 23))) evidence_.flags |= EprCapable;
        // A cable nobody read: a source offering more than 3 A has seen a 5 A e-marker (it caps a 3 A cable at 3 A), so that is
        // the cable's current limit, not the 3 A default (CUKTECH 120W through its 6 A cable, far-end sink, 2026-11-06).
        if (n < 7 && !(evidence_.flags & PhysicalCable) && (p.type == pd::PdoType::Fixed || p.type == pd::PdoType::Pps) &&
            pd::current_limit_ma(p, p.max_mv) > 3000) evidence_.cable_ma = std::max<uint16_t>(evidence_.cable_ma, 5000);
    }
}
// The PD rows (Pd, PdLow) are planned from the caps of the first attach that hears a list: START's list is provisional (it
// came from a contract the first unit throws away) and an EPR entry rebuilds the tail from the EPR list.
void Runner::replan_pd(const pd::SourceCaps& caps, Input in, bool refresh_evidence) {
    if (plan_frozen_) return;
    if (!info_rows_) in.epr_info = nullptr;   // the informational EPR list shapes the rows only once it was asked for and heard
    // The PD rows sit between the other units' rows (the vendor modes ahead of them may be final already): those stay as they are,
    // the PD rows are planned again and sorted back into the unit order (stable: each unit keeps its plan order).
    const uint32_t before = layout_sig({steps_.data(), count_});
    size_t kept = 0;
    for (size_t n = 0; n < count_; ++n) if (mode(unit_of(steps_[n])) != Mode::Pd) steps_[kept++] = steps_[n];
    count_ = kept;
    for (size_t n = 0; n < n_order_; ++n)
        if (mode(order_[n]) == Mode::Pd) plan_pd_unit(order_[n], caps, opts_, in, [this](Step s) { add(s); }, false);
    auto rank = [this](const Step& s) {
        const Unit u = unit_of(s);
        for (size_t k = 0; k < n_order_; ++k) if (order_[k] == u) return k;
        return size_t(n_order_);
    };
    for (size_t i = 1; i < count_; ++i) {
        const Step s = steps_[i];
        const size_t r = rank(s);
        size_t j = i;
        while (j > 0 && rank(steps_[j - 1]) > r) { steps_[j] = steps_[j - 1]; --j; }
        steps_[j] = s;
    }
    if (refresh_evidence) caps_evidence(caps);
    late_pd_ = false; plan_sig_ = caps_sig(caps);
    const size_t first = pick();
    index_ = first == SIZE_MAX ? count_ : first;   // the rows moved: the cursor follows
    if (layout_sig({steps_.data(), count_}) != before) { ++revision_; env_.plan_changed(); }
    record_pd_skips();
}
// The SCP / UFCS rows after the unit's first row (its 5 V handshake, which read the adapter's limits / the source's modes) follow
// those: planned again, once per unit. The first row is the same row either way, and the rows ahead of it keep their place, so the
// cursor stays; only the unit's own block grows or shrinks and the units behind it move. Nothing changes when nothing was read.
// The planning half of replan_fc() (the fresh rows' array, ~460 B), out of line so its frame is gone before the Env calls.
size_t Runner::replan_fc_rows(const Input& in, size_t& first_out) {
    if (plan_frozen_ || !in.fc_caps) return 0;
    const Step& done = steps_[index_];
    if (done.group != Group::Scp && done.group != Group::Ufcs) return 0;
    const Unit u = unit_of(done);
    const uint8_t bit = done.group == Group::Scp ? 1 : 2;
    size_t first = 0;
    while (first < count_ && unit_of(steps_[first]) != u) ++first;
    if (first != index_ || (fc_replanned_ & bit)) return 0;   // only the unit's first row reads the limits
    fc_replanned_ |= bit;
    if (done.group == Group::Scp && in.fc_caps->scp.valid) evidence_.scp_ma = uint16_t(in.fc_caps->scp.max_ma);
    constexpr size_t kMaxFresh = 2 + 3 * ufcs::kMaxCaps;   // 5 V, three levels per mode, the step back to 5 V
    Step fresh[kMaxFresh];
    size_t nf = 0;
    static const pd::SourceCaps none{};
    plan_unit(u, none, opts_, in, [&](Step s) { if (nf < kMaxFresh) fresh[nf++] = s; });
    size_t end = first;
    while (end < count_ && unit_of(steps_[end]) == u) ++end;
    const size_t tail = count_ - end, new_end = first + nf;
    if (nf == 0 || new_end + tail > steps_.size()) return 0;
    // The first row is the same row either way except for the UFCS mode it now names (the baseline is judged in the mode that reaches 5 V).
    bool changed = nf != end - first || fresh[0].pdo != steps_[first].pdo;
    for (size_t n = 1; !changed && n < nf; ++n)
        changed = std::memcmp(&fresh[n], &steps_[first + n], sizeof(Step)) != 0;
    if (!changed) return 0;
    steps_[first].pdo = fresh[0].pdo;
    if (new_end < end) std::copy(steps_.begin() + end, steps_.begin() + count_, steps_.begin() + new_end);
    else std::copy_backward(steps_.begin() + end, steps_.begin() + count_, steps_.begin() + new_end + tail);
    for (size_t n = 1; n < nf; ++n) steps_[first + n] = fresh[n];
    count_ = new_end + tail;
    first_out = first;
    return nf;
}
void Runner::replan_fc(const Input& in) {
    size_t first = 0;
    const size_t nf = replan_fc_rows(in, first);
    if (nf == 0) return;
    ++revision_; env_.plan_changed();
    for (size_t n = 1; n < nf; ++n) if (steps_[first + n].verdict == Verdict::Skip) env_.record(steps_[first + n]);
}
// A source can list more PDOs within the attach (UGREEN X865: the SPR AVS APDO comes with the list re-sent after our
// Sink_Capabilities_Extended, ~360-390 ms after the first one, i.e. after the 5 V contract is Ready and inside the baseline):
// the rows follow the list in hand until a PD row has run.
void Runner::regrow_plan(const pd::SourceCaps& caps, const Input& in) {
    if (late_pd_ || !caps.valid || caps_sig(caps) == plan_sig_) return;
    for (size_t n = 0; n < count_; ++n)
        if (mode(unit_of(steps_[n])) == Mode::Pd && steps_[n].verdict != Verdict::Pending && steps_[n].verdict != Verdict::Skip) return;
    replan_pd(caps, in, true);
}
// A source that announces its list again mid-run (pattern budget-reannounce-mid-run: a shared-port splitter re-splitting its budget,
// Anker 2-way behind a Baseus 140 W, 20 V dropped while 15 V stayed at a higher current; also reannounce-after-cable-discovery, Baseus
// 70 W after the SOP' cable query, CUKTECH C1 after the Xiaomi auth with an APDO inserted ahead of ours; or the splitter's re-split
// delivered as a COMPLETE EPR_Source_Capabilities, 2026-11-09 run A6). Same wire revision and none of the excluded cases (VBUS dark,
// no list back within kCapsReturnMs, the preauth_tail window, the K2's own EPR entry (epr_stage_ == 2) or a partial EPR chunk, the
// hunt-Reject answer): the list is re-planned PER ROW instead of the unit ending as "offers changed":
//  - no PD row has run yet: the PD rows are planned again from the new list (replan_pd, extra APDOs get rows);
//  - a Pending row the new list still offers takes its new slot and PDO word; a row it no longer offers (type or voltage range gone,
//    a lower current, a dropped PDO, EPR without the EPR bit in PDO1) is skipped as Withdrawn ("withdrawn by source"), not
//    CapsChanged, and the unit goes on against the new list; a row a withdrawn PDO comes back for is Pending again ("back"), as long
//    as its unit has not ended;
//  - the row in flight: if its voltage (fixed) or target (PPS/AVS range) is still offered, it is RESTARTED at the new slot and
//    current word once the list has settled (rule 1/2: no Request until kListSettleMs unchanged; the restart is not a try -
//    flight_restart_, sent by poll_impl); if not offered, it ends as withdrawn and is released to 5 V (the sink's single-Request
//    gate serialises that release).
// A list that places no PD row of the RUNNING unit at all (pending or in flight) is a re-split like the others (2026-11-17,
// Anker 2-way splitter: every pending row gone after the 9 V row): every such row is withdrawn, the re-split is counted, and the
// unit stays open - next() holds it for kWithdrawnWaitMs for the offer to return, then closes it with the rows Withdrawn. Another
// wire revision and no list back within kCapsReturnMs stay CapsChanged (the caller's check below), as before.
// Only rows of the running unit (unit_of(s) == unit_) may be withdrawn here or counted as placeable (review 2026-11-09):
// a re-announce during the Pd unit must not withdraw the PdLow rows for good. Their pending rows are re-queued silently
// when the list still offers them, so they are not stale when their unit starts; the running unit's own rows are re-placed
// at their unit's start by this same pass (the sig guard below only skips the full re-plan, not the per-row pass).
// The re-plan is COUNTED (revision_, plan_changed(), Evidence::rebalanced, the replan log) only when a row was withdrawn,
// came back or moved slot - never on a re-announce that changes nothing for the plan (cable discovery before the first
// row, a current-only raise such as the Xiaomi post-auth 5 -> 6 A, the pre-auth short list): a word-only change still
// refreshes the row's PDO word (a fixed offer whose current dropped runs at the new, lower word).
void Runner::follow_list(const pd::SourceCaps& caps, const Input& in) {
    // A COMPLETE EPR_Source_Capabilities outside the K2's own EPR entry/exit (epr_stage_ == 2) is a re-announce like any
    // other (2026-11-09, run A6); only a partial chunk stays excluded.
    if (mode_ != Mode::Pd || late_pd_ || !caps.valid || caps.epr_partial || epr_stage_ == 2 ||
        caps.spec_rev != evidence_.wire_rev) return;
    if (in.vbus_mv >= 0 && in.vbus_mv < 1000) return;   // VBUS dark: excluded, the source is not announcing a budget
    // Where the list in hand offers a row: on an EPR list the EPR rows (pos >= 8) are placed on its positions 8+ like any
    // other row; on an SPR list an EPR row stays valid while PDO 1 still carries the EPR bit (the entry is made when the
    // row asks), else it is withdrawn.
    const auto offer_at = [&](const Step& s) -> int {
        return s.pos >= 8 ? (caps.epr ? find_offer(caps, s) : (epr_capable_caps(caps) ? 0 : -1)) : find_offer(caps, s);
    };
    bool started = false;
    for (const auto& s : steps()) {
        if (mode(unit_of(s)) != Mode::Pd) continue;
        if (s.verdict != Verdict::Pending && s.verdict != Verdict::Skip) started = true;
    }
    // An SPR list that still offers EPR, after the K2 entered EPR and heard the EPR list (epr_proof_), is the source's re-announce
    // after a Hard Reset that dropped EPR mode (Baseus 140 W on its own cable, 2026-11-17: no EPR_Request in time, the row went back to
    // pending): the plan built from the EPR list stays - planned from this list it would lose every EPR row as "not offered".
    if (!started && !plan_frozen_ && epr_stage_ == 0 && !(epr_proof_ && !caps.epr && epr_capable_caps(caps))) {
        if (caps_sig(caps) != plan_sig_) replan_pd(caps, in, true);   // no PD row has run yet: plan again from the new list
        return;
    }
    if (caps_sig(caps) == plan_sig_) {
        // The same list as the last re-plan: still run the per-row pass when a pending row of the RUNNING unit is
        // misplaced - its unit just started after a re-announce during another unit, and only the running unit's rows
        // are re-planned (review 2026-11-09). Otherwise nothing can change: leave the rows to the caller's check.
        bool stale = false;
        for (const auto& s : steps()) {
            if (s.verdict != Verdict::Pending || unit_of(s) != unit_ || mode(unit_of(s)) != Mode::Pd || is_mi_chain(s)) continue;
            const int at = offer_at(s);
            if (at < 0 || (s.pos < 8 && (size_t(at) != s.pos - 1u || caps.pdos[static_cast<size_t>(at)].raw != s.pdo))) { stale = true; break; }
        }
        if (!stale) return;
    }
    // The preauth_tail window (pattern preauth-list-after-reset): rows above a cut-short list are tolerated, not withdrawn, while
    // the source's chain re-runs.
    bool tail_wait = false;
    preauth_tail(in, tail_wait);
    // A list that places no row of the running unit at all is a re-split like any other (2026-11-17, Anker 2-way splitter: every
    // pending row gone after the 9 V row): its rows are withdrawn below, the unit stays open and next() waits for the offer.
    Step* flight0 = index_ < count_ && steps_[index_].verdict == Verdict::Running && mode(unit_of(steps_[index_])) == Mode::Pd
                       ? &steps_[index_] : nullptr;
    unsigned follow = 0, withdrawn = 0, back = 0;
    bool moved = false;       // any change, a refreshed PDO word included: the evidence follows the list
    bool plan_moved = false;  // a row was withdrawn, came back or moved slot: the plan itself changed
    const auto withdraw = [this](Step& s) { record_skip(s, Reason::Withdrawn); };
    // The row in flight first: its offer is looked up by voltage / range anywhere on its side of the SPR / EPR boundary,
    // not at the slot it asked - an offer that only moved slot re-queues the row (it finishes at the new slot), only an
    // offer that is gone ends it as withdrawn and releases to 5 V (the sink's single-Request gate serialises that release).
    Step* flight = flight0;
    if (flight) {
        Step& s = *flight;
        const int at = offer_at(s);
        if (at < 0) { finish(in, Verdict::Skip, Reason::Withdrawn); ++withdrawn; moved = plan_moved = true; flight = nullptr; }
        else if (s.pos >= 8 && !caps.epr) {
            ++follow;   // an EPR row of an SPR list that still offers EPR: the entry is made when the row asks
        } else {
            const uint32_t raw = caps.pdos[static_cast<size_t>(at)].raw;
            if (s.pos != at + 1) plan_moved = true;   // only a slot move changes the plan
            if (s.pos != at + 1 || s.pdo != raw) {
                take_offer(s, at, raw, in); moved = true;
                flight_restart_ = true;   // rule 2: restart the row in flight at the new slot / current word (not a try)
            }
            ++follow;
        }
    }
    const Step* flight_row = index_ < count_ ? &steps_[index_] : nullptr;   // index_ may be count_: no &steps_[count_]
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (mode(unit_of(s)) != Mode::Pd || &s == flight || is_mi_chain(s)) continue;   // the chain row has no offer to follow
        if (s.verdict == Verdict::Pending || s.verdict == Verdict::Running) {
            if (unit_of(s) != unit_) {
                // Another PD unit's row: never withdrawn here and never counted. Re-queue it silently when the list
                // still offers it, so it is not stale when its own unit starts.
                if (s.pos < 8) {
                    const int at = find_offer(caps, s);
                    if (at >= 0 && (s.pos != at + 1 || s.pdo != caps.pdos[static_cast<size_t>(at)].raw))
                        take_offer(s, at, caps.pdos[static_cast<size_t>(at)].raw, in);
                }
                continue;
            }
            if (s.pos >= 8) {
                if (!caps.epr) {
                    if (epr_capable_caps(caps)) continue;   // an EPR row of an SPR list that still offers EPR: the entry is made when the row asks
                    withdraw(s); ++withdrawn; moved = plan_moved = true; continue;
                }
                const int at8 = find_offer(caps, s);
                if (at8 >= 0) {
                    const uint32_t raw = caps.pdos[static_cast<size_t>(at8)].raw;
                    if (s.pos != at8 + 1) plan_moved = true;
                    if (s.pos != at8 + 1 || s.pdo != raw) { s.pos = static_cast<uint8_t>(at8 + 1); s.pdo = raw; moved = true; }
                    ++follow;
                } else { withdraw(s); ++withdrawn; moved = plan_moved = true; }
                continue;
            }
            const int at = find_offer(caps, s);
            if (at >= 0) {
                const uint32_t raw = caps.pdos[static_cast<size_t>(at)].raw;
                if (s.pos != at + 1) plan_moved = true;   // a slot move is a plan change
                if (s.pos != at + 1 || s.pdo != raw) { take_offer(s, at, raw, in); moved = true; }
                ++follow;
            } else if (s.pos > caps.num_pdos && tail_wait) {
                continue;   // the preauth tail: tolerated while the chain re-runs
            } else {
                withdraw(s); ++withdrawn; moved = plan_moved = true;
            }
        } else if (s.verdict == Verdict::Skip && s.reason == Reason::Withdrawn && &s != flight_row && unit_of(s) == unit_) {
            // A PDO that came back: the row is pending again (its unit has not ended).
            if (revive_withdrawn_row(s, caps)) { ++back; moved = plan_moved = true; }
        }
    }
    plan_sig_ = caps_sig(caps);
    if (back) wd_wait_ = false;   // an offer came back: a later re-split gets its own wait
    if (moved) caps_evidence(caps);   // the evidence follows the list only when a row actually moved (an EPR exit keeps EprTable)
    if (plan_moved) note_replan(follow, withdrawn, back);
}
Reason Runner::start(const pd::SourceCaps& caps, Options o, Input in, bool confirmed) {
    o.selected &= kAllAlgorithms;
    epr_proof_ = in.epr_proof = false;
    const Reason denied = refusal(o, in, busy(), confirmed);
    if (denied != Reason::None) {
        if (!run_id_) {
            if (reason_ != denied) ++revision_;
            reason_ = denied;
            refusal_mv_ = uint16_t(std::clamp(in.vbus_mv, 0, 65535));
        }
        return denied;
    }
    ++run_id_; ++revision_;
    info_ = 0; info_rows_ = false;
    o.units = resolve(o);
    opts_ = o;
    K2LOG("[CT] legacy tune quiet=%ums steady=%ums first=%ums gap=%ums hold=%ums chain=%u\n", unsigned(legacy_tune(LegacyKnob::QuietMs)),
          unsigned(legacy_tune(LegacyKnob::SteadyMs)), unsigned(legacy_tune(LegacyKnob::FirstWaitMs)), unsigned(legacy_tune(LegacyKnob::GapMs)),
          unsigned(legacy_tune(LegacyKnob::HoldMs)), unsigned(legacy_tune(LegacyKnob::Chain)));
    n_order_ = o.units.n; order_ = o.units.u;
    build(caps, o, in);
    evidence_ = {};
    spec_apdo_ = {};
    evidence_.selected = o.selected; evidence_.session = in.session; evidence_.dpdm = in.lines;
    evidence_.monitor_mv = std::min<uint32_t>(in.cap_mv, 65535); evidence_.cable_mv = used_cable_mv(in);
    evidence_.cable_ma = used_cable_ma(in);
    evidence_.reported = in.reported;
    evidence_.flags = (in.reported.valid ? Rmdo : 0) | (in.cable_observed ? PhysicalCable : 0) | (in.spoof_on ? SpoofOn : 0);
    caps_evidence(caps);
    // The PD tail is provisional (replan_pd) and the EPR rows need an entry first: their skips are journaled when the plan is final.
    late_pd_ = has_unit(o.units, Unit::Pd) || has_unit(o.units, Unit::PdLow);
    // EPR entry is wanted when the source may offer EPR rows it has not announced yet: no list at START, or an EPR-capable SPR list.
    // The Pd attach decides between the entry (a cable that can carry it) and the informational list only (a cable that cannot).
    epr_stage_ = (o.selected & (bit(Algorithm::Epr) | bit(Algorithm::EprAvs))) && late_pd_ &&
                 (!caps.valid || (!caps.epr && epr_capable_caps(caps))) ? 3 : 0;
    pd_skips_recorded_ = !late_pd_; pd_skips_held_ = false;
    for (size_t n = 0; n < count_; ++n) if (steps_[n].verdict == Verdict::Skip && (mode(unit_of(steps_[n])) != Mode::Pd || pd_skips_recorded_)) env_.record(steps_[n]);
    resumed_ = 0; reattach_ = 0; reattach_planned_ = false; loaded_ = false; plan_frozen_ = false;
    reset_run_state();
    selfpwr_ = in.self_powered; held_ = false; jwait_ms_ = 0;
    session_ = in.session; resets_ = link_resets(in.pd);
    ceiling_done_ = false; ceil_ms_ = ceil_stop_ms_ = ceil_low_ms_ = 0; ceil_why_ = 0;
    spec_done_ = false; spec_ms_ = spec_stop_ms_ = 0; spec_pending_ = spec_wanted();
    unit_time_.fill(0); time_ms_ = 0; time_pending_ = true;
    never_dark_ = in.no_cc;   // a port with no CC does not go dark: the first detach is the short one too
    own_rel_ = false; reann_ = 0;
    pre_ms_ = 0; retries_ = epr_tries_ = 0; settle_ms_ = settle_until_ = 0; auth_open_ = false;
    xiaomi_seen_ = xiaomi_reset_ = x_svid_ = x_authed_ = false; tail_at_ = 0; hr_count_ = link_resets(in.pd);
    last_ms_ = phase_ms_ = in.now; sag_cut_ = sag_open_ = false; busy_retry_at_ = 0;
    reason_ = Reason::None; refusal_mv_ = 0;
    probe_ = (has_unit(o.units, Unit::Pd) || has_unit(o.units, Unit::PdLow)) ? Probe::Pending : Probe::Off;
    state_ = State::Baseline;
    // An already elevated source is not safe to take over blindly. A reading inside a dip is the fall of a port cut: the Baseline waits for
    // the comeback (bounded, see kDipMinMv) instead of ending the run before it began.
    dip_open_ = dip_reading(in.vbus_mv); dip_ms_ = in.now; dip_back_ = false; dips_ = 0;   // a START inside the dark has its clock from now
    if (!safe5(in.vbus_mv) && !dip_reading(in.vbus_mv)) { unsafe(Reason::Release); return reason_; }
    return Reason::None;
}
// The level the rail starts the row from: a row in the live session of the one before it (a retarget) starts at the last passed row of
// its group; any other row opens its own session, which begins at the 5 V default.
static int rail_start_mv(std::span<const Step> steps, size_t index) {
    if (index >= steps.size() || !steps[index].continuous) return 5000;
    for (size_t n = index; n-- > 0;) {
        if (steps[n].group != steps[index].group) break;
        if (steps[n].verdict == Verdict::Pass) return int(steps[n].mv);
    }
    return 5000;
}
bool Runner::band_ok(const Step& s, const Input& in, uint32_t gap) {
    if (!averages_band(s)) {
        // The SCP 4.5 V row sits below the 5 V an unloaded output stays at, so VBUS alone cannot prove it: the adapter's own readback
        // must agree with the request whenever the session has read it.
        if (s.group == Group::Scp && s.mv == 4500 && in.fc_adapter_mv > 0 && std::abs(in.fc_adapter_mv - 4500) > 150) return false;
        return in_band(s, in.vbus_mv, from_mv_, rail_start_mv({steps_.data(), count_}, index_));
    }
    if (gap > 250) band_.clear();   // missing samples cannot be averaged across
    const int level = int(s.mv);
    if (in.vbus_mv < 0 || std::abs(in.vbus_mv - level) * 100 > level * int(kBandGrossPct)) { band_.clear(); return false; }
    band_.add(in.vbus_mv, gap);
    return in_band(s, band_.mean());
}
// pattern output-capped-below-advertised: follow the output of a programmable row whose contract is live (see kPlateauMs).
// The steady level is tracked whatever it is: capped() reads it only above kCappedFloorMv, floored() only for a low row.
// renewing: the sink re-requests the row's own level (the PPS keep-alive, every kPpsKeepAliveMs): `ready` drops between its Request
// and PS_RDY, and that window keeps the plateau while VBUS stays on it, for at most kPlateauRenewMs (a Request that stays unanswered
// longer restarts it, and ends the row through the paths of its own). A gap, a VBUS excursion or a level change still restart it.
void Runner::track_plateau(const Input& in, bool ready, bool renewing, uint32_t gap) {
    const int tol = std::max(100, int(plat_mv_) / 100);
    if (gap > 250 || in.vbus_mv < 0) { plat_mv_ = 0; plat_nr_ms_ = 0; return; }
    if (!ready) {
        if (renewing && plat_mv_ && std::abs(in.vbus_mv - int(plat_mv_)) <= tol && plat_nr_ms_ + gap <= kPlateauRenewMs) {
            plat_nr_ms_ = static_cast<uint16_t>(plat_nr_ms_ + gap);
            return;
        }
        plat_mv_ = 0; plat_nr_ms_ = 0; return;
    }
    plat_nr_ms_ = 0;
    if (!plat_mv_ || std::abs(in.vbus_mv - int(plat_mv_)) > tol) {
        plat_mv_ = static_cast<uint16_t>(std::min(in.vbus_mv, 65535)); plat_ms_ = in.now;
    }
}
bool Runner::capped(const Step& s, const Input& in, bool ready) const {
    if (!ready || s.group != Group::Pd || s.hunt || !(bit(s.algorithm) & kPpsAlgorithms)) return false;
    const int v = in.vbus_mv;
    if (v < kCappedFloorMv || v >= int(s.mv)) return false;
    if (plat_mv_ && in.now - plat_ms_ >= kPlateauMs) return true;
    int top = 0;   // the highest level a passed PD row of the run measured: a stall at it is that converter's ceiling
    for (size_t n = 0; n < count_; ++n) {
        const auto& o = steps_[n];
        if (o.group == Group::Pd && o.verdict == Verdict::Pass && o.measured_mv < s.mv) top = std::max<int>(top, o.measured_mv);
    }
    return top >= kCappedFloorMv && std::abs(v - top) * 100 <= top;
}
// The mirror of capped(), for the low edge (PdLow): a low programmable row whose contract is live but whose output
// sits steady ABOVE the band ceiling - the source cannot regulate below its floor (it advertises 3.3 V, holds 5 V).
bool Runner::floored(const Step& s, const Input& in, bool ready) const {
    if (!ready || !low_programmable(s) || s.hunt) return false;
    const int v = in.vbus_mv;
    if (v < 0 || v <= pd::programmable_ceiling_mv(int(s.mv))) return false;
    return plat_mv_ && std::abs(v - int(plat_mv_)) <= std::max(100, int(plat_mv_) / 100) && in.now - plat_ms_ >= kPlateauMs;
}
// The Request a PD row sends now: the step-walk row asks its current walk level, the sweep row its current point,
// every other row its own target.
pd::Choice Runner::pd_choice(const Step& s) const {
    return {s.pos, is_step_walk(s) ? walk_mv_ : is_sweep(s) ? sweep_mv_ : s.mv, s.ma, false};   // never an over-ask
}
void Runner::request(const Input& in) {
    auto& s = steps_[index_];
    // The Xiaomi chain row requests nothing: it waits (at 5 V, asked again at the next poll) while the chain is still running, then
    // reads its outcome and plans the modes it unlocked.
    if (is_mi_chain(s)) {
        if (mi_chain_wait(in)) { state_ = State::Baseline; stable_ = true; stable_ms_ = in.now - 500; pre_ms_ = 0; return; }
        mi_latch(in); return;
    }
    cut_start_ms_ = cut_back_ms_ = 0; row_hi_ = 0;   // the cut clocks of a row start with its request (track_cut)
    const auto limit = limit_reason(s.mv, in, s.algorithm);
    // A PD row above 5 A (a 6.25 A APDO's full-current row, a Xiaomi 6 A mode) needs a cable vouched for above 5 A.
    const bool over5 = s.group == Group::Pd && s.ma > 5000 && over5_cable_ma(in) <= 5000;
    // Light: the backstop behind the planner. Whatever a replan left in the plan, no PD Request goes out and a legacy row asks at 5 V only.
    const bool light = opts_.level == Level::Light && (s.group == Group::Pd || s.mv != 5000);
    if (light || limit != Reason::None || over5 || ((s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) && !epr_cable(in))) {
        s.verdict = Verdict::Skip;
        s.reason = light ? (s.group == Group::Pd ? Reason::Advertised : Reason::LightLevel) : limit == Reason::None ? Reason::CableLimit : limit;
        s.detail = s.reason == Reason::OverCap && (s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) ? monitor_detail(in.cap_mv) : uint8_t(0);
        env_.record(s);
        // A skipped step must not leave a contract held: hand it back and wait for vSafe5V.
        if (pd_owned_ || fc_owned_) { release(in, false); return; }
        next(in); return;
    }
    // No row passes on VBUS alone. 5 V is what every source gives by default, so a 5 V row proves nothing until the
    // engine has the charger's own answer (QC: the D+/D- short opening; VOOC/UFCS/VIVO/FCP/SCP: frames back).
    // The marker below names this row by its place in the journal's section files: with the plan's shape not yet on flash (a replan
    // is being written one file per tick) the row waits in the Baseline, the contract at 5 V. Only a request that starts a contract
    // waits: a chained retarget is already in one and has its marker covered by the file written before it.
    if (s.mv > 5000 && !pd_owned_ && !fc_owned_ && !env_.journal_ready()) {
        if (!jwait_ms_) jwait_ms_ = in.now | 1u;
        state_ = State::Baseline; stable_ = true; stable_ms_ = in.now - 500; pre_ms_ = 0;   // asked again at the next poll
        return;
    }
    if (jwait_ms_) { env_.journal_waited(in.now - jwait_ms_); jwait_ms_ = 0; }
    hot_ = false; busy_retry_at_ = 0;
    s.verdict = Verdict::Running;
    if (is_step_walk(s)) {   // the walk starts at the row's own level; each granted level gets a fresh steady wait
        walk_mv_ = s.mv; walk_n_ = 0; walk_prev_mv_ = -1; walk_base_mv_ = -1; walk_rebased_ = false;
        walk_steady_.clear(); walk_grant_ms_ = 0;
    }
    if (is_sweep(s)) sweep_begin(s, in);   // the sweep starts at the APDO's first point
    if (s.mv > 5000) env_.phase(Phase::Request, index_);   // journal marker BEFORE anything above 5 V
    phase_ms_ = in.now; stable_ = false; resets_ = link_resets(in.pd); band_.clear(); plat_mv_ = 0; plat_nr_ms_ = 0; from_mv_ = std::max(in.vbus_mv, 0);
    peak_lo_mv_ = 0xFFFF; peak_hi_mv_ = 0;
    row_accepted_ = row_ps_rdy_ = false; row_max_ma_ = 0;   // the needs-load latch is per Request (a retry starts it again)
    feed_ms_ = 0;
    prog_from_mv_ = pd::user_hold(in.pd) && in.pd.state == pd::SinkState::Ready && in.pd.pos == s.pos ? in.pd.mv : 0;
    row_timing_ = 0; row_ps_rdy_ms_ = 0;
    state_ = State::Requesting;
    if (s.group == Group::Pd) {
        pd_owned_ = true;
        // Restart rules, rule 1 (settle first): no Request until the list in hand has been unchanged for kListSettleMs.
        // The park rides the Busy-retry path below (busy_retry_at_); the row's own timeout bounds the wait, and the
        // restart is not a try (tries is set only in finish(), from source failures).
        if (!list_settled(in.now)) { busy_retry_at_ = list_ms_ + kListSettleMs; return; }
        send_pd_request(in, s, false);
    } else {
        // An SCP row joins the kept or live session of the row before it (scp_next picked it): same session, no release in between.
        const bool chained = s.group == Group::Scp && s.ma > 0 && legacy_tune(LegacyKnob::Chain) >= 1 && (in.fc_held || in.fc_state == fc::State::Active);
        const bool retarget = fc_owned_ && (s.continuous || chained);
        if (!race_seen_ && mode_ == Mode::Legacy) { race_seen_ = true; race_fast_ = silent_ && in.pd_quiet_ms < kLegacyQuietMs; race_idx_ = index_; }
        fc_owned_ = true;
        const auto err = env_.request_fc(proto(s.group), s.mv, retarget, s.ma);
        if (err != fc::Fail::None) finish(in, Verdict::Fail, Reason::Request, static_cast<uint8_t>(err));
        else if (s.group == Group::Scp && legacy_tune(LegacyKnob::Chain) >= 1) {
            bool more = false;   // a later SCP row of the unit still wants the session
            for (size_t n = index_ + 1; n < count_; ++n) more = more || (steps_[n].group == Group::Scp && steps_[n].verdict == Verdict::Pending);
            env_.fc_keep(more);
        }
    }
}
// The row leaves the plan (the rows after it move up; the cursor is picked again by the caller).
void Runner::drop_row(size_t i) {
    if (i >= count_) return;
    for (size_t n = i + 1; n < count_; ++n) steps_[n - 1] = steps_[n];
    steps_[--count_] = {};
    ++revision_; env_.plan_changed();
}
// Room for one more row at `at`: the rows from there move down one place (the inverse of drop_row).
bool Runner::insert_row(size_t at, const Step& s) {
    if (count_ >= kMaxSteps || at > count_) return false;
    for (size_t n = count_; n > at; --n) steps_[n] = steps_[n - 1];
    steps_[at] = s; ++count_;
    ++revision_; env_.plan_changed();
    return true;
}
// The Xiaomi chain row's turn (request()): how the chain ended, and the modes it unlocked, each as a row right behind this one. The
// list before is the one the sink kept when the chain finished (XiaomiTrace::pre), the list after is the one in hand (caps_now_,
// re-announced after the chain: mi_chain_wait gave it kXiaomiReannounceMs). A mode row asks its PDO at the new level
// (mi_unlock::test_point) through the ordinary request machinery: the cable rule of the full-current rows (used_cable_ma, a spoofed
// identity counts; above 5 A over5_cable_ma) and the voltage limits decide whether it can go out, else it is a skip that says why.
void Runner::mi_latch(const Input& in) {
    const pd::XiaomiTrace* t = in.xm;
    // The sink forgets the chain with a Hard Reset: the run's own record from before it stands in for the SVID.
    const bool svid = in.pd.xiaomi_svid || x_svid_ || xiaomi_seen_;
    const bool authed = in.pd.xiaomi_authed && t && t->authed;
    const uint8_t code = mi_unlock::chain_code(svid, authed, t ? t->last_step : uint8_t(0), authed ? t->finish : uint8_t(0));
    evidence_.mi_chain = code; evidence_.mi_n = 0;
    {
        auto& s = steps_[index_];
        s.verdict = mi_unlock::chain_done(code) ? Verdict::Pass : Verdict::Skip;
        s.reason = Reason::MiChain; s.detail = code;
        env_.record(s);
    }
    if (authed && t->n_pre && caps_now_.valid && !caps_now_.epr) {
        std::array<uint32_t, mi_unlock::kSprSlots> now{};
        const size_t n = std::min<size_t>({caps_now_.num_pdos, caps_now_.pdos.size(), now.size()});
        for (size_t i = 0; i < n; ++i) now[i] = caps_now_.pdos[i].raw;
        const auto d = mi_unlock::diff({t->pre.data(), std::min<size_t>(t->n_pre, t->pre.size())}, {now.data(), n});
        size_t at = index_ + 1;
        for (const auto& m : d.view()) {
            const pd::Pdo p = pd::decode_pdo(m.after);
            const auto pt = mi_unlock::test_point(m.after);
            Step r; r.group = Group::Pd; r.hunt = true; r.pos = m.pos; r.pdo = m.after; r.mv = pt.mv; r.ma = pt.ma;
            r.algorithm = p.type == pd::PdoType::Pps ? Algorithm::Pps : p.type == pd::PdoType::Avs ? Algorithm::SprAvs : Algorithm::Pd3Fixed;
            r.avs = p.type == pd::PdoType::Avs;
            const Reason limit = limit_reason(r.mv, in, r.algorithm);
            const unsigned cable = r.ma > 5000 ? over5_cable_ma(in) : used_cable_ma(in);
            if (opts_.level == Level::Light) { r.verdict = Verdict::Skip; r.reason = Reason::Advertised; }
            else if (limit != Reason::None) { r.verdict = Verdict::Skip; r.reason = limit; }
            else if (!r.ma || r.ma > cable) { r.verdict = Verdict::Skip; r.reason = Reason::CableLimit; }
            if (!insert_row(at, r)) break;
            evidence_.mi[evidence_.mi_n++] = m;
            if (r.verdict == Verdict::Skip) env_.record(steps_[at]);
            ++at;
        }
    }
    if (pd_owned_ || fc_owned_) { release(in, false); return; }
    next(in);
}
const mi_unlock::Mode* Runner::mi_mode(const Step& s) const {
    if (!is_mi_mode(s)) return nullptr;
    for (size_t n = 0; n < evidence_.mi_n; ++n) if (evidence_.mi[n].pos == s.pos && evidence_.mi[n].after == s.pdo) return &evidence_.mi[n];
    for (size_t n = 0; n < evidence_.mi_n; ++n) if (evidence_.mi[n].pos == s.pos) return &evidence_.mi[n];
    return nullptr;
}
uint32_t Runner::hold_ms() const {
    const uint32_t h = legacy_tune(LegacyKnob::HoldMs);
    return index_ < count_ && steps_[index_].group == Group::Pd ? std::max(h, kHoldFloorMs) : h;   // the capture of a PD row needs its floor
}
// The Deep checks' switches (the Tests list line sets the two apart); the step-walk and sweep rows have their own judgments.
// The 100 mV sweep step is the PPS class 1 and the AVS class 0 of the spec engine's plan; a walk quantum is class 0 of either.
static_assert(pdspec::step_mv(pdspec::ProgKind::Pps, 1) == int(ctsweep::kStepMv) && pdspec::step_mv(pdspec::ProgKind::Avs, 0) == int(ctsweep::kStepMv));
static_assert(pdspec::step_mv(pdspec::ProgKind::Pps, 0) == int(walk_step_mv(Algorithm::Pps)) &&
              pdspec::step_mv(pdspec::ProgKind::Avs, 0) == int(walk_step_mv(Algorithm::SprAvs)));
// A move is the spec stage's while the stage is still ahead, on a PPS or AVS APDO (EPR AVS included), with the K2 on its own supply (the
// engine refuses one fed by the charger). The wait is bounded: a capture that never freezes costs one move, not the row.
bool Runner::spec_feed(const Input& in, const Step& s, int from_mv, int to_mv, uint8_t cls, uint8_t flags) {
    if (feed_ms_) {
        if (env_.spec_feeding() && in.now - feed_ms_ < kSpecFeedMaxMs) return true;
        feed_ms_ = 0;
        return false;
    }
    const bool avs = s.algorithm == Algorithm::SprAvs || s.algorithm == Algorithm::EprAvs;
    if (!spec_pending_ || selfpwr_ || !s.pos || s.pos > 15 || (s.algorithm != Algorithm::Pps && !avs)) return false;
    if (!env_.spec_move({s.pos, avs, cls, from_mv, to_mv}, flags)) return false;
    feed_ms_ = in.now | 1u;
    return true;
}
bool Runner::prog_wanted(const Step& s) const {
    if (s.group != Group::Pd || s.hunt || s.detail == kDetailStepWalk || s.detail == kDetailSweep) return false;
    return s.algorithm == Algorithm::Pps ? opts_.pps_sweep : (s.algorithm == Algorithm::SprAvs || s.algorithm == Algorithm::EprAvs) && opts_.avs_sweep;
}
uint32_t Runner::hold_left_ms(uint32_t now) const {
    return state_ == State::Holding ? hold_ms() - std::min(hold_ms(), now - phase_ms_) : 0;
}
void Runner::skip_group_rows(Group g, bool raises_only, uint8_t detail) {
    for (size_t n = 0; n < count_; ++n) {
        auto& o = steps_[n];
        if (o.verdict != Verdict::Pending || o.group != g || (raises_only && o.mv <= 5000)) continue;
        record_skip(o, Reason::Request, detail);
    }
}
// pattern usb-a-dpdm-open: the DCP check read D+/D- open and the source never spoke PD, so a D+/D- unit that got no reply says nothing about
// the charger. Not while any D+/D- row has passed or failed (the lines carried data), nor on a PD source (the check does not run there).
bool Runner::lines_open_cause(const Input& in) const {
    if (evidence_.dpdm != Lines::Open || evidence_.num_pdos || caps_now_.valid || in.pd_seen_attach) return false;
    for (size_t n = 0; n < count_; ++n)
        if (steps_[n].group != Group::Pd && (steps_[n].verdict == Verdict::Pass || steps_[n].verdict == Verdict::Fail)) return false;
    return true;
}
void Runner::lines_open_sweep() {
    for (size_t n = 0; n < count_; ++n) {
        auto& o = steps_[n];
        if (o.group == Group::Pd || o.group >= Group::Count) continue;
        const bool silent = o.verdict == Verdict::Skip && o.reason == Reason::Request && no_reply(o.group, fc::Fail(o.detail));
        if (o.verdict != Verdict::Pending && !silent) continue;
        record_skip(o, Reason::LinesOpen, static_cast<int>(silent_code(o.group)));
    }
}
void Runner::release(const Input& in, bool stop) {
    stopping_ = stop;
    if (stop) skip_pending(reason_);
    // Our own D+/D- release of a PD-quiet silent attach: the source's announcement that may follow is its answer to it (own_rel_).
    own_rel_ = !stop && fc_owned_ && mode_ == Mode::Legacy && silent_ && attached_ && reann_ != 1 && in.pd_quiet_ms >= quiet_ms();
    if (own_rel_) own_rel_ms_ = in.now;
    reissue_release();
    // Nothing is held and VBUS never left vSafe5V: there is nothing to wait for.
    if (!stop && !pd_owned_ && !fc_owned_ && !hot_ && !recover_ && safe5(in.vbus_mv)) { next(in); return; }   // the sink may refuse and is asked again by poll(); the legacy engine always lets go
    state_ = State::Releasing; phase_ms_ = release_ms_ = in.now; stable_ = false; rel_hr_ = in.pd.hard_resets;
}
void Runner::reissue_release() {
    if (pd_owned_) env_.release(true);
    if (fc_owned_) env_.release(false);
}
void Runner::finish(const Input& in, Verdict v, Reason r, uint8_t detail) {
    if (cut_hold(in, v, r, detail) || race_hold(in, v, r, detail)) return;
    finish_row(in, v, r, detail);
}
// A unit that started inside the source's announcement (quiet under kLegacyQuietMs) and failed its first step with a race-type reason
// (the guard refused it, the handshake never came) is asked once more after real quiet; the row goes back to Pending and waits in
// Releasing (race_wait_), where a load behind the K2 still ends the run. "Unsupported" / "no response" are a charger's own answers
// (a silent UFCS or VIVO source would pay a second window for nothing): verdicts, not races.
bool Runner::race_hold(const Input& in, Verdict v, Reason r, uint8_t detail) {
    const auto why = static_cast<fc::Fail>(detail);
    const bool silent_skip = v == Verdict::Skip && index_ < count_ && never_answered(steps_[index_].group, why);   // a first step's "no reply" is held like the fail it was
    if (lines_open_cause(in)) return false;   // open lines: the retry cannot hear anything either
    if ((v != Verdict::Fail && !silent_skip) || r != Reason::Request || !race_fast_ || race_tried_ || race_wait_ || cut_wait_ || index_ != race_idx_ || index_ >= count_ ||
        mode_ != Mode::Legacy || pd_owned_ || stopping_ || !silent_ || in.inline_load || steps_[index_].group == Group::Pd) return false;
    if (why != fc::Fail::Inline && why != fc::Fail::NoHandshake) return false;
    race_tried_ = true; race_wait_ = true; race_ms_ = in.now;
    K2LOG("[CT] legacy retry %s after %s\n", name(unit_), fc::name(why));
    steps_[index_].verdict = Verdict::Pending; steps_[index_].reason = Reason::None;
    release(in, false);
    return true;
}
// kDipMinMv: the clocks of a dip, every poll. A dip opens at the first reading in the dip range and ends at the first vSafe5V reading
// (a comeback: counted, its tail session is adopted for kDipTailMs); a reading above vSafe5V or under kDipMinMv ends it uncounted (those
// keep their own verdicts). Dips are counted per stretch between two rows: a row that runs clears the count.
void Runner::track_dip(const Input& in) {
    const int v = in.vbus_mv;
    if (state_ == State::Requesting || state_ == State::Holding) dips_ = 0;
    if (dip_back_ && in.now - dip_back_ms_ >= kDipTailMs) dip_back_ = false;
    if (dip_reading(v)) {
        if (!dip_open_) { dip_open_ = true; dip_ms_ = in.now; dip_back_ = false; }
    } else if (dip_open_) {
        dip_open_ = false;
        if (safe5(v)) { dip_back_ = true; dip_back_ms_ = in.now; if (dips_ < 255) ++dips_; }
    }
}
// The 5 V check waits while VBUS is in a dip that has lasted less than kCutWindowMs and fewer than kCutRetries dips came back before it.
bool Runner::dip_wait(const Input& in) const {
    return dip_open_ && dip_reading(in.vbus_mv) && in.now - dip_ms_ < kCutWindowMs && dips_ < kCutRetries;
}
bool Runner::dip_adopts(const Input& in) const {
    return (dip_open_ && in.now - dip_ms_ < kCutWindowMs) || (dip_back_ && in.now - dip_back_ms_ < kDipTailMs);
}
// pattern usb-a-no-load-auto-off. Called at every poll of a Legacy attach: follows the falls of VBUS under kCutMv that start at the 5 V
// level (the sample before is not above kCutNoAboveMv), closes one when VBUS is back at vSafe5V and keeps the onsets of the ones that
// lasted kCutMinMs..kCutWindowMs (the cut period is read from them, see Evidence::cut_period_ds).
void Runner::track_cut(const Input& in) {
    const bool live = mode_ == Mode::Legacy && attached_ && in.vbus_mv >= 0 &&
        (state_ == State::Baseline || state_ == State::Requesting || state_ == State::Holding || state_ == State::Releasing);
    if (!live) { cut_dark_ = false; cut_last_mv_ = -1; return; }
    const int v = in.vbus_mv;
    if (state_ == State::Requesting || state_ == State::Holding) row_hi_ = std::max(row_hi_, v);
    if (v < kCutMv) {
        if (!cut_dark_ && cut_last_mv_ >= 0 && cut_last_mv_ <= kCutNoAboveMv) { cut_dark_ = true; cut_dark_ms_ = in.now ? in.now : 1; }
    } else if (cut_dark_ && safe5(v)) {
        cut_dark_ = false;
        cut_start_ms_ = cut_dark_ms_; cut_back_ms_ = in.now ? in.now : 1;
        const uint32_t dip = cut_back_ms_ - cut_start_ms_;
        if (dip >= kCutMinMs && dip <= kCutWindowMs) {
            if (n_cut_onsets_ == kCutOnsets) { std::copy(cut_onsets_.begin() + 1, cut_onsets_.end(), cut_onsets_.begin()); --n_cut_onsets_; }
            cut_onsets_[n_cut_onsets_++] = cut_start_ms_;
            // The period: the median gap between the onsets kept (the port does not cut a raised output, so a gap may be longer than
            // the period and a cut hidden in a handshake leaves none; the middle gap is the period the port keeps at 5 V).
            if (n_cut_onsets_ >= 2) {
                std::array<uint32_t, kCutOnsets> gap{};
                for (unsigned n = 1; n < n_cut_onsets_; ++n) gap[n - 1] = cut_onsets_[n] - cut_onsets_[n - 1];
                // Insertion sort: at most kCutOnsets - 1 gaps, and std::sort would link its introsort and heap code (about 470 B) for this one use.
                for (unsigned i = 1; i + 1 < n_cut_onsets_; ++i) {
                    const uint32_t x = gap[i];
                    unsigned j = i;
                    for (; j > 0 && gap[j - 1] > x; --j) gap[j] = gap[j - 1];
                    gap[j] = x;
                }
                const uint32_t mid = gap[(n_cut_onsets_ - 2) / 2];
                evidence_.cut_period_ds = mid / 100u <= 255u ? uint8_t((mid + 50u) / 100u) : uint8_t(0);
                K2LOG("[CT] cut period=%ums n=%u\n", unsigned(mid), unsigned(n_cut_onsets_));
            }
        }
    }
    cut_last_mv_ = v;
}
// finish() for a legacy row that fails (Lost, Timeout, Request, Voltage) while it sat at 5 V and a fall of VBUS under kCutMv is in
// progress or has come back since the row's request: the verdict is kept, the row goes back to Pending and waits in Releasing for
// the output (cut_resolve decides). A Lost always waits: its fall may not have shown in VBUS yet.
bool Runner::cut_hold(const Input& in, Verdict v, Reason r, uint8_t detail) {
    // A silent family (VOOC no answer, a unit that never answered) is a skip, but a port that cut its output mid-session explains the silence the same way a failure's does.
    const bool silent_family = v == Verdict::Skip && r == Reason::Request && index_ < count_ && no_reply(steps_[index_].group, fc::Fail(detail));
    if ((v != Verdict::Fail && !silent_family) || cut_wait_ || index_ >= count_ || mode_ != Mode::Legacy || pd_owned_ || steps_[index_].group == Group::Pd) return false;
    if (r != Reason::Lost && r != Reason::Timeout && r != Reason::Request && r != Reason::Voltage) return false;
    if (std::max(row_hi_, in.vbus_mv) > kCutNoAboveMv) return false;
    // The engine's failure may land on the very poll that reads the fall's first sample (4.65 V, not yet under kCutMv): a dip that has
    // just opened (track_dip) holds the row too, and cut_resolve keeps the verdict when the fall never reached the dark.
    const bool falling = dip_open_ && dip_reading(in.vbus_mv) && in.now - dip_ms_ < kCutWindowMs;
    if (r != Reason::Lost && !cut_dark_ && !cut_start_ms_ && !falling) return false;
    cut_wait_ = true; cut_final_ = cut_retry_idx_ == index_ && cut_tries_ >= kCutRetries;
    cut_v_ = v; cut_why_ = r; cut_detail_ = detail; cut_mv_ = in.vbus_mv;
    steps_[index_].verdict = Verdict::Pending; steps_[index_].reason = Reason::None;
    recover_ = true; sag_cut_ = true; session_ = in.session;
    release(in, false);
    return true;
}
// The held casualty's output is back and steady (Releasing), or the recovery window ran out. A fall of VBUS that lasted
// kCutMinMs..kCutWindowMs is the port's cut: the row is asked again (cut_retry_idx_, up to kCutRetries times), or, when
// its last retry was cut too, it reads "port cuts without load". Anything else keeps the verdict it would have had: VBUS not back in the window, or no fall at all.
void Runner::cut_resolve(Input in) {
    cut_wait_ = false;
    if (index_ >= count_ || steps_[index_].verdict != Verdict::Pending) return;
    const uint32_t dip = cut_start_ms_ && cut_back_ms_ ? cut_back_ms_ - cut_start_ms_ : 0;
    const bool cut = dip >= kCutMinMs && dip <= kCutWindowMs;
    if (cut && !cut_final_) {
        cut_tries_ = cut_retry_idx_ == index_ ? uint8_t(cut_tries_ + 1) : uint8_t(1);
        cut_retry_idx_ = index_;
        if (evidence_.cut_retries < 255) ++evidence_.cut_retries;
        K2LOG("[CT] cut retry row=%u dip=%ums try=%u\n", unsigned(index_), unsigned(dip), unsigned(cut_tries_));
        return;   // the row stays Pending: next() asks it again from a steady 5 V
    }
    in.vbus_mv = cut_mv_;
    if (cut) K2LOG("[CT] cut retry row=%u cut again after %u: skip\n", unsigned(index_), unsigned(cut_tries_));
    finish_row(in, cut ? Verdict::Skip : cut_v_, cut ? Reason::PortCuts : cut_why_, cut ? uint8_t(0) : cut_detail_);
}
void Runner::finish_row(const Input& in, Verdict v, Reason r, uint8_t detail) {
    auto& s = steps_[index_];
    prog_on_ = false;
    // The first D+/D- unit's "no reply" under open lines is the confirming attempt: it and every other D+/D- row take the one cause.
    const bool open_cause = v == Verdict::Skip && r == Reason::Request && s.group != Group::Pd && no_reply(s.group, fc::Fail(detail)) && lines_open_cause(in);
    if (open_cause) { r = Reason::LinesOpen; detail = uint8_t(silent_code(s.group)); }
    const bool sweep_row = is_sweep(s);   // read before detail is overwritten: the marker survives on sweep rows
    s.verdict = v; s.reason = r; s.detail = detail;
    if (sweep_row) { s.detail = kDetailSweep; sweep_close(s); }
    const bool vooc_family = s.group >= Group::Vooc && s.group <= Group::SuperVooc65;
    s.tries = vooc_family ? in.fc_tries : 0;
    if (vooc_family && v == Verdict::Pass && detail == uint8_t(fc::Fail::Detected) && in.fc_ident) evidence_.vooc_ident = in.fc_ident;
    if (s.group == Group::Pd) {
        // Item G: the row's PS_RDY quirks go to the run's evidence and the log (no verdict reads them, the card does not show them yet).
        evidence_.ps_rdy_max = std::max<uint8_t>(evidence_.ps_rdy_max, uint8_t(std::min<unsigned>(row_ps_rdy_ms_ / 20u, 255u)));
        if (row_timing_) {
            evidence_.timing |= row_timing_;
            K2LOG("[CT] ps_rdy row=%u flags=%u ms=%u\n", unsigned(index_), unsigned(row_timing_), unsigned(row_ps_rdy_ms_));
        }
        row_timing_ = 0; row_ps_rdy_ms_ = 0;
        // Attempts after a retry, the first one included. The first EPR row also carries the retries of the baseline entry
        // that made its list available (epr_tries_, consumed here).
        unsigned retried = pd_retry_ == index_ ? retries_ : 0u;
        if (s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) { retried += epr_tries_; epr_tries_ = 0; }
        s.tries = retried ? uint8_t(std::min(retried + 1u, 7u)) : uint8_t(0);
    }
    // pattern reject-then-reset: a source that has just Rejected is still settling; the next EPR entry waits for it.
    if (s.group == Group::Pd && v == Verdict::Fail && r == Reason::Request && detail == static_cast<uint8_t>(pd::SinkResult::Rejected))
        settle_until_ = in.now + kRejectSettleMs;
    // A fixed PD row that was judged on the window mean records that mean (a Pass, the Timeout of a level that never came in band,
    // a Voltage drift). A gross excursion cleared the window: the raw sample is the evidence there, as for every other ending.
    int measured = in.vbus_mv;
    if ((v == Verdict::Pass || r == Reason::Timeout || r == Reason::Voltage) && averages_band(s))
        if (const int mean = band_.mean(); mean >= 0) measured = mean;
    s.measured_mv = static_cast<uint16_t>(std::clamp(measured, 0, 65535));
    // A deep-sweep row reports the sweep, not the last sample: target = the setpoint of the peak, measured = the
    // peak itself (the row's range label still comes from the APDO; points and fold-back stay in the summary).
    if (sweep_row)
        if (const auto* rw = sweep_view().find(uint8_t(s.algorithm), s.pos, sweep_low(s)); rw && rw->sum.points) {
            s.mv = rw->sum.peak_at_mv;
            s.measured_mv = rw->sum.peak_mv;
        }
    env_.record(s);
    if (open_cause) lines_open_sweep();
    // pattern vooc-family-same-wake: VOOC, SVOOC and SVOOC65 open with the same wake and the K2 stops at the shared ident
    // (it answers nothing), so the first family session that ended on the ident ("detected"), on nothing heard ("no
    // answer") or on frames without an ident ("no response") decides the family's other rows: a pass for "detected", a skip
    // with that reason otherwise. They keep their names and run no session of their own.
    const auto family = [](Group g) { return g >= Group::Vooc && g <= Group::SuperVooc65; };
    const auto why = static_cast<fc::Fail>(detail);
    if (family(s.group) && (r == Reason::Request || (v == Verdict::Pass && r == Reason::None)) &&
        (why == fc::Fail::Detected || why == fc::Fail::NoAnswer || why == fc::Fail::NoResponse))
        for (size_t n = 0; n < count_; ++n) {
            auto& o = steps_[n];
            if (o.verdict != Verdict::Pending || !family(o.group) || o.group == s.group) continue;
            o.verdict = v == Verdict::Pass ? Verdict::Pass : Verdict::Skip;
            o.reason = v == Verdict::Pass ? Reason::None : Reason::Request;
            o.detail = detail; o.tries = s.tries; o.measured_mv = s.measured_mv;
            env_.record(o);
        }
    // The SVOOC ceiling stage (svooc_ceiling.hpp) follows a detected family once, after its session is released.
    if (family(s.group) && v == Verdict::Pass && why == fc::Fail::Detected && ceiling_wanted()) ceiling_pending_ = true;
    if (v == Verdict::Pass) replan_fc(in);
    // pattern qc3-20v-without-qc2-20v: QC3 Class B just reached 20 V, so an earlier "Class A only" QC2 Class B 20 V row was a real miss.
    if (v == Verdict::Pass && s.algorithm == Algorithm::Qc3ClassB && s.mv == 20000)
        for (size_t n = 0; n < count_; ++n) {
            auto& o = steps_[n];
            if (o.algorithm != Algorithm::Qc2ClassB || o.mv != 20000 || o.reason != Reason::ClassAOnly) continue;
            o.verdict = Verdict::Fail; o.reason = Reason::Qc3Only20V; env_.record(o);
        }
    // A Capped row is a ceiling the source accepted the contract up to: the levels after it prove their own things, so the chain
    // goes on like after a pass (and never skips them, below).
    if ((v == Verdict::Pass || r == Reason::Capped) && index_ + 1 < count_ && steps_[index_ + 1].continuous &&
        steps_[index_ + 1].verdict == Verdict::Pending) {
        ++index_; request(in);
    } else {
        // A row lost to a source drop (Reason::Lost) is no verdict on the levels after it: the source is recovered and they get their
        // turn, each in a session of its own (a bank that resets on one level, test_source_drop_recovers_on_a_non_pd_source).
        if ((v == Verdict::Fail || v == Verdict::Skip) && r != Reason::Lost && r != Reason::Capped &&
            r != Reason::NeedsLoad && r != Reason::PortCuts) {
            // Later retargets cannot prove anything after the initial handshake/contract failed (or the charger gave no response).
            // They are the levels of the same algorithm: the Class B rows that follow a QC2 / QC3 row in its session are rows of their
            // own algorithm and keep their verdict logic (a failed 12 V still leaves "Class A only" / the handshake failure to the
            // 20 V row); once the session is released they open a fresh one.
            for (size_t n = index_ + 1; n < count_ && steps_[n].continuous && steps_[n].algorithm == s.algorithm; ++n) {
                record_skip(steps_[n], r, detail);
            }
        }
        // The held session hops to the next row inside this call, so the raise rows a no-change end rules out are skipped here, before it
        // (the failure path's own skip comes after finish() returns).
        if (v == Verdict::Fail && s.group == Group::Scp && s.mv > 5000 && detail == uint8_t(fc::Fail::NoChange) && !cut_wait_)
            skip_group_rows(Group::Scp, true, detail);
        if (scp_next(in, v == Verdict::Pass) != SIZE_MAX) { next(in); return; }
        release(in, false);
    }
}
// One granted level of the step-walk row (is_step_walk): every level records VBUS; a later level's move of 2 quanta or more
// (walk_step_mv: 20 mV PPS, 100 mV AVS) ends the walk at once (rounded to a coarser quantum), otherwise the walk asks for the
// next quantum through the sink's single Request gate; after kStepWalkCount moves the total must be kStepWalkCount quanta +- one
// and the slope over all levels inside half a quantum .. one and a half. A jump is a CAUTION (Pass + StepResolution) with detail
// = the observed move in mV ; a total or trend that misses is the same with detail = the mean move (0 = "steps ignored"). A single
// move is not judged for direction: the output creeps after each PS_RDY.
void Runner::walk_step(const Input& in, const Step& eff) {
    (void)eff;
    auto& s = steps_[index_];
    const int q = int(walk_step_mv(s.algorithm));
    // walk-settle (ROADMAP "The PPS step walk fails a good source..."): the granted level is recorded only once VBUS is
    // steady (kWalkSteadySamples samples inside kWalkSteadyWindowMs within kWalkSteadyMv), or kWalkSteadyTimeoutMs after
    // the level was first seen granted, whichever comes first - the wait is bounded. A source that overshoots and settles
    // (the UGREEN 13.0 V base read 13007 mV settling to 12990) must not be recorded mid-settle: the first move would then
    // read ~0 or negative and a good source fails "steps ignored".
    if (!walk_grant_ms_) walk_grant_ms_ = in.now;
    walk_steady_.add(in.now, in.vbus_mv);
    if (!walk_steady_.steady() && in.now - walk_grant_ms_ < kWalkSteadyTimeoutMs) return;   // still settling: wait at this level
    // The level is the mean of the window, not the one sample that happens to close it: a source in burst mode swings every sample
    // by tens of mV around a level it holds well, and one sample read a +-swing as a jump (review item D). A steady window's mean is
    // within kWalkSteadyMv of every sample in it, so a clean source records what it recorded before.
    if (walk_n_ && spec_feed(in, s, int(walk_mv_) - q, walk_mv_, 0, walk_n_ >= 2 ? pdspec::kMoveChain : 0)) return;
    const int level = walk_steady_.mean(in.vbus_mv);
    walk_lv_[walk_n_] = level;
    if (walk_n_ == 0) {
        walk_prev_mv_ = walk_base_mv_ = level;   // the base level is granted, in band and steady
    } else {
        // Only a jump ends the walk at one step: the output creeps for ~2 s after each PS_RDY, so one move reads from <= 0 to
        // ~+36 mV on a good source; the total and the trend below judge the rest.
        const int move = level - walk_prev_mv_;
        if (walk_n_ == 1 && !walk_rebased_ && move <= -q) {
            // walk-rebase (qa-fixes item 1, ROADMAP "The PPS step walk's base can be an overshoot plateau"): +kStepMv was asked
            // and the output stands one quantum or more BELOW the base - the base was a plateau the source holds for a while
            // and then drops from (the UGREEN holds its 13.09 V overshoot for 0.5 to 1.1 s), not the settled level. Take this
            // level as the base once and walk on from it (walk_n_ restarts, the Request for the next +kStepMv goes out below);
            // a second drop is judged by the total and the trend like any other level.
            walk_rebased_ = true;
            walk_n_ = 0;
            walk_lv_[0] = walk_prev_mv_ = walk_base_mv_ = level;
        } else if (move >= 2 * q) {
            finish(in, Verdict::Pass, Reason::StepResolution, step_walk_detail(move));
            return;
        } else {
            walk_prev_mv_ = level;
        }
    }
    if (walk_n_ >= kStepWalkCount) {
        const int total = level - walk_base_mv_;
        // Least-squares slope per step over the kStepWalkCount + 1 levels, x = 0..n: slope = sum((2i - n) * y_i) / den with
        // den = n (n + 1) (n + 2) / 6 (35 for n = 5); kept in integers, compared against the band times den.
        constexpr int n = int(kStepWalkCount), den = n * (n + 1) * (n + 2) / 6;
        int num = 0;
        for (int i = 0; i <= n; ++i) num += (2 * i - n) * (walk_lv_[i] - walk_base_mv_);
        const bool trend_ok = 2 * num >= q * den && 2 * num <= 3 * q * den;
        if (std::abs(total - int(kStepWalkCount) * q) > q || !trend_ok)
            finish(in, Verdict::Pass, Reason::StepResolution, step_walk_detail(total / int(kStepWalkCount)));
        else
            finish(in, Verdict::Pass, Reason::None);
        return;
    }
    ++walk_n_;
    walk_mv_ = uint16_t(walk_mv_ + q);
    stable_ = false;
    walk_steady_.clear(); walk_grant_ms_ = 0;   // the next granted level waits for its own steady VBUS
    from_mv_ = std::max(in.vbus_mv, 0);         // a level up from where VBUS stands now
    row_accepted_ = row_ps_rdy_ = false;        // this level is its own Accept / PS_RDY exchange (latch_row)
    phase_ms_ = in.now;   // each granted level gets its own Requesting timeout (the steady wait is bounded per level)
    const auto err = env_.request_pd({s.pos, walk_mv_, s.ma, false});
    if (err == pd::Err::Busy) busy_retry_at_ = in.now + 200;
    else if (err != pd::Err::Ok) finish(in, Verdict::Fail, Reason::Request, 128 + static_cast<uint8_t>(err));
}
// The deep-sweep row starts (request()): the point plan comes from the row's own PDO word, clamped like the planner did (monitor
// ceiling and cable voltage limit), and the row's slice of the record is reset to holes. A resumed row re-sweeps from its first
// point: a power loss mid-sweep ends cleanly that way (the journal re-runs the row).
void Runner::sweep_begin(const Step& s, const Input& in) {
    const unsigned cap = std::min<unsigned>(in.cap_mv, cable_limit_mv(in, s.algorithm));
    sweep_plan_ = sweep_plan(s, cap, opts_.sweep_depth, caps_now_);
    sweep_lo_ = sweep_plan_.lo;
    sweep_hi_ = sweep_plan_.hi;
    sweep_mv_ = sweep_plan_.first();
    sweep_first_ = sweep_used_;
    sweep_n_ = sweep_pts_ = sweep_prev_mv_ = sweep_prev_lv_ = 0;
    sweep_refining_ = sweep_refined_ = false;
    sweep_scan_.clear();
    sweep_ref_ = {};
    const unsigned first = std::min<unsigned>(sweep_first_, ctsweep::kMaxPoints);
    std::fill_n(sweep_err_.data() + first, std::min(ctsweep::point_count({sweep_lo_, sweep_hi_}), ctsweep::kMaxPoints - first), ctsweep::kHole);
    stable_ = false;
    walk_steady_.clear();
}
// The Request of one sweep point (or step-back point) through the sink's single gate; the point is its own Accept / PS_RDY exchange (latch_row).
void Runner::sweep_ask(const Input& in, const Step& s, unsigned mv) {
    sweep_mv_ = uint16_t(mv);
    const auto ma = ctsweep::point_ma(pd::decode_pdo(s.pdo), mv, cable_ma(in));
    stable_ = false;
    phase_ms_ = in.now;
    from_mv_ = std::max(in.vbus_mv, 0);
    row_accepted_ = row_ps_rdy_ = false;
    const auto err = env_.request_pd({s.pos, sweep_mv_, uint16_t(ma), false});
    if (err == pd::Err::Busy) busy_retry_at_ = in.now + 200;
    else if (err != pd::Err::Ok) finish(in, Verdict::Fail, Reason::Request, 128 + static_cast<uint8_t>(err));
}
bool Runner::sweep_settled(const Input& in) const {
    return in.now - stable_ms_ >= kSweepSettleMs && walk_steady_.tail_steady(in.now, kSweepSettleMs);
}
// One granted sweep point (Accept + PS_RDY at sweep_mv_, VBUS steady): record measured - setpoint (a point off the 100 mV cell grid is
// only judged), then ask the next point of the depth's plan through the sink's single Request gate. A steady OUT-OF-BAND point is
// recorded too, not a stop - the sweep stops only on a lost source (the row's failure paths in poll_impl: no PS_RDY, Reject, Hard
// Reset, VBUS drop, withdrawn). The sink's own PPS keep-alive (kPpsKeepAliveMs, 5 s < 10 s) re-requests a held level between points.
// The row's first finding (fold or break, the live pd::TrackScan) is stepped back from once: the Refiner asks 100 mV then 20 mV
// points up from the last point that tracked, then the sweep goes on after the finding.
void Runner::sweep_point(const Input& in, const Step&) {
    auto& s = steps_[index_];
    const unsigned mv = sweep_mv_;
    const bool pps = s.algorithm == Algorithm::Pps;
    if (!sweep_refining_ && sweep_pts_ && !sweep_refined_) {   // the first point is the landing from the row before: not a sweep step
        // The spec stage judges moves of a 100 mV step or more (a 20 mV one is its walk class) and always the row's last one (coverage).
        const bool last = sweep_plan_.next(mv) == 0;
        const unsigned step = mv - sweep_prev_mv_;
        if (step >= ctsweep::kStepMv || last) {
            const uint8_t flags = uint8_t(pdspec::kMoveSweep | (sweep_pts_ >= 2 && sweep_fed_ ? pdspec::kMoveChain : 0) | (last ? pdspec::kMoveLast : 0));
            if (spec_feed(in, s, int(sweep_prev_mv_), int(mv), pps && step > ctsweep::kFineMv ? 1 : 0, flags)) return;
            sweep_fed_ = step >= ctsweep::kStepMv;
        } else sweep_fed_ = false;
    }
    // The point's level is the mean of the last kWalkSteadyWindowMs of samples (fed by poll_impl), not the one sample that closes it
    // (review item D: a source in burst mode swings every sample by tens of mV around a level it holds).
    const int level = sweep_settled(in) ? walk_steady_.tail_mean(in.now, kSweepSettleMs, in.vbus_mv) : walk_steady_.mean(in.vbus_mv);
    walk_steady_.clear();
    if (sweep_refining_) {
        sweep_ref_.feed(level);
        if (!sweep_ref_.done()) { sweep_ask(in, s, sweep_ref_.next()); return; }
        sweep_refining_ = false;
        if (!sweep_resume_mv_) finish(in, Verdict::Pass, Reason::None);
        else sweep_ask(in, s, sweep_resume_mv_);
        return;
    }
    if (const int c = ctsweep::plan_cell(sweep_plan_, mv); c >= 0 && sweep_first_ + unsigned(c) < ctsweep::kMaxPoints) {
        sweep_err_[sweep_first_ + unsigned(c)] = int16_t(std::clamp(level - int(mv), -32767, 32767));
        sweep_n_ = uint16_t(std::max<unsigned>(sweep_n_, unsigned(c) + 1u));
    }
    sweep_scan_.feed(int(mv), level);
    sweep_prev_mv_ = uint16_t(mv);
    sweep_prev_lv_ = uint16_t(std::clamp(level, 0, 65535));
    ++sweep_pts_;
    const unsigned next = sweep_plan_.next(mv);
    if (!sweep_refined_ && sweep_scan_.found()) {
        sweep_refined_ = true;
        sweep_resume_mv_ = uint16_t(next);
        sweep_ref_.begin(pps ? ctsweep::Kind::Pps : ctsweep::Kind::Avs, sweep_scan_.kind(), sweep_scan_.req_mv() - sweep_scan_.step_mv(),
                         sweep_scan_.from_mv(), sweep_scan_.req_mv(), sweep_scan_.to_mv());
        if (!sweep_ref_.done()) { sweep_refining_ = true; sweep_ask(in, s, sweep_ref_.next()); return; }
    }
    if (!next) finish(in, Verdict::Pass, Reason::None);   // the last point of the APDO: the row is done
    else sweep_ask(in, s, next);
}
// The sweep row closed (finish()): summarize its cells into the row table (keyed by algorithm + PDO slot), so the
// results strip and the card read one summary per APDO. Cells past the kMaxPoints budget were never recorded.
void Runner::sweep_close(Step& s) {
    const unsigned n = std::min<unsigned>(sweep_n_, ctsweep::kMaxPoints - std::min<unsigned>(sweep_first_, ctsweep::kMaxPoints));
    auto sum = ctsweep::summarize({sweep_err_.data() + std::min<unsigned>(sweep_first_, ctsweep::kMaxPoints), n}, sweep_lo_);
    if (const auto& l = sweep_ref_.result(); sweep_refined_ && l.found && sum.fold_mv) {   // the step-back's finer finding replaces the cell-grid one
        sum.fold_located = true; sum.fold_ok_mv = l.ok_mv; sum.fold_from_mv = l.bad_mv;
    }
    sweep_used_ = std::max(sweep_used_, uint16_t(std::min<unsigned>(sweep_first_ + n, ctsweep::kMaxPoints)));
    for (auto& r : std::span(sweep_rows_.data(), n_sweep_rows_))
        if (r.alg == uint8_t(s.algorithm) && r.pos == s.pos && (r.lo_mv < ctsweep::kSplitMv) == (sweep_lo_ < ctsweep::kSplitMv)) {
            r.n = uint16_t(n); r.lo_mv = sweep_lo_; r.hi_mv = sweep_hi_; r.sum = sum; return;   // a re-run of the same part
        }
    if (n_sweep_rows_ < sweep_rows_.size()) {
        auto& r = sweep_rows_[n_sweep_rows_++];
        r.pos = s.pos; r.alg = uint8_t(s.algorithm); r.lo_mv = sweep_lo_; r.hi_mv = sweep_hi_;
        r.first = sweep_first_; r.n = uint16_t(n); r.sum = sum;
    }
    if (sum.fold_mv && s.verdict == Verdict::Pass && s.reason == Reason::None) fold_caution(s, sum);
}
// User decision 2026-11-16: a voltage fold is a CAUTION. The sweep row that found it reads Reason::Folded on its Pass; so do the
// PASSED fixed/range rows of the same APDO (algorithm + slot) whose level is at or above the first folded setpoint and whose reading
// sits below the peak - the fixed 20 V row of the FH22 reads 19.43 V, inside its band, on the folded side. The planner runs an APDO's
// sweep after its own rows, so those rows are marked now, at the end of the APDO's rows (amend() lets the journal follow). A capped,
// failed, skipped, hunt or step-walk row keeps its own finding: it is no pass.
void Runner::fold_caution(Step& sweep, const ctsweep::Summary& sum) {
    sweep.reason = Reason::Folded;
    for (size_t n = 0; n < count_; ++n) {
        auto& o = steps_[n];
        if (&o == &sweep || is_sweep(o) || o.group != sweep.group || o.algorithm != sweep.algorithm || o.pos != sweep.pos) continue;
        if (o.verdict != Verdict::Pass || o.reason != Reason::None || o.hunt || o.detail == kDetailStepWalk) continue;
        if (o.mv < sum.fold_from_mv || o.measured_mv + pd::kTrackFoldMv >= sum.peak_mv) continue;
        o.reason = Reason::Folded;
        env_.amend(o);
    }
}
// The first pending row in execution order (the unit order, then the plan order inside a unit).
size_t Runner::pick() const {
    for (size_t k = 0; k < n_order_; ++k)
        for (size_t n = 0; n < count_; ++n)
            if (steps_[n].verdict == Verdict::Pending && unit_of(steps_[n]) == order_[k]) return n;
    return SIZE_MAX;
}
// The next SCP row of the unit when the session of the row that just ended can take it: kept after a detect / "no change" end (held),
// or live after a pass. Every release of a legacy session makes some sources announce their list again (pattern
// reannounce-after-own-release) and the next row would open with a new detection, so the rows ride one session.
size_t Runner::scp_next(const Input& in, bool passed) const {
    if (legacy_tune(LegacyKnob::Chain) < 1 || steps_[index_].group != Group::Scp || !fc_owned_ || !attached_ || ending_ || stopping_ || cut_wait_) return SIZE_MAX;
    if (!in.fc_held && !(passed && in.fc_state == fc::State::Active)) return SIZE_MAX;
    const size_t i = pick();
    return i != SIZE_MAX && unit_of(steps_[i]) == unit_ && steps_[i].group == Group::Scp && steps_[i].ma > 0 ? i : SIZE_MAX;
}
// The run's one scheduler: the unit in progress goes on with its next row; when it has none left it ends with its detach, and
// the next detached state starts the next unit (docs/CHARGER_TEST_FLOW.md).
void Runner::next(const Input& in) {
    ceil_recover_ = false;   // the release after the stage is over
    // The SVOOC ceiling stage runs once the VOOC family's session is let go: D+/D- free, VBUS back at 5 V, nothing drawn.
    if (ceiling_pending_ && attached_ && !ending_ && !pd_owned_ && !fc_owned_ && safe5(in.vbus_mv) && begin_ceiling(in)) return;
    const size_t i = pick();
    if (i != SIZE_MAX && attached_ && !ending_ && unit_of(steps_[i]) == unit_) { index_ = i; request(in); return; }
    // Restart rules, rule 4 (end-of-unit sweep): before the PD unit closes, re-check the Withdrawn rows ONCE against the
    // list in hand (after a settle) and run the ones that came back; Withdrawn stays only for rows still not offered then.
    if (attached_ && !ending_ && mode_ == Mode::Pd && (i == SIZE_MAX || unit_of(steps_[i]) != unit_) &&
        caps_now_.valid && list_settled(in.now)) {
        unsigned back = 0;
        for (auto& s : std::span<Step>(steps_.data(), count_)) {
            if (s.verdict == Verdict::Skip && s.reason == Reason::Withdrawn && unit_of(s) == unit_ &&
                revive_withdrawn_row(s, caps_now_)) ++back;
        }
        if (back) {
            wd_wait_ = false;   // a revival re-arms the wait for the next re-split
            note_replan(0, 0, back);
            const size_t j = pick();
            if (j != SIZE_MAX && attached_ && !ending_ && unit_of(steps_[j]) == unit_) { index_ = j; request(in); return; }
        }
    }
    index_ = i == SIZE_MAX ? count_ : i;
    if (attached_) {
        if (pd_owned_ || fc_owned_) { release(in, false); return; }   // Done only after the 5 V confirmation
        // A re-split left rows Withdrawn and the list in force still lacks them (or has not settled yet): the unit stays open, idle at
        // 5 V in the Baseline (every poll asks again, so a returned offer is picked up above), for kWithdrawnWaitMs from the first
        // time it would have closed - then it closes with the rows Withdrawn.
        if (mode_ == Mode::Pd && !ending_ && (i == SIZE_MAX || unit_of(steps_[i]) != unit_)) {
            unsigned withdrawn = 0;
            for (const auto& s : std::span<const Step>(steps_.data(), count_))
                if (s.verdict == Verdict::Skip && s.reason == Reason::Withdrawn && unit_of(s) == unit_) ++withdrawn;
            if (withdrawn) {
                if (!wd_wait_) { wd_wait_ = true; wd_wait_ms_ = in.now; K2LOG("[CT] withdrawn wait rows=%u\n", withdrawn); }
                if (in.now - wd_wait_ms_ < kWithdrawnWaitMs) {
                    index_ = count_;
                    if (state_ != State::Baseline) { state_ = State::Baseline; stable_ = false; phase_ms_ = in.now; pre_ms_ = 0; }
                    return;
                }
            }
        }
        if (probe_ == Probe::Pending && mode_ == Mode::Pd && safe5(in.vbus_mv)) {
            // The first Pd unit is done, back at a settled 5 V: ask the charger who it is, bounded (a failure changes no row).
            probe_ = Probe::Closing; state_ = State::Baseline; stable_ = false; phase_ms_ = in.now; return;
        }
        // The first Pd unit is done, its contract back at a settled 5 V and the charger asked who it is: the PD spec stage follows.
        if (spec_pending_ && mode_ == Mode::Pd && unit_ == Unit::Pd && !ending_ && probe_ == Probe::Off && safe5(in.vbus_mv) && begin_spec(in)) return;
        // Legacy -> Legacy keeps the PD-silent session: only the D+/D- cleanup (the release above) sits in between. Any PD traffic
        // since the attach, or a VBUS that is not at 5 V, means the session is gone: detach and attach again as before.
        if (i != SIZE_MAX && mode_ == Mode::Legacy && mode(unit_of(steps_[i])) == Mode::Legacy && legacy_active_ && silent_ &&
            safe5(in.vbus_mv) && in.pd_quiet_ms >= quiet_ms()) { continue_legacy(unit_of(steps_[i]), in); return; }
        ending_ = true; begin_detach(in); return;
    }
    // No PD row is pending yet, but the EPR list (heard at the Pd attach) will add some: the Pd unit still runs once.
    if (i == SIZE_MAX && epr_stage_ == 3 && unit_ != Unit::Pd && has_unit(opts_.units, Unit::Pd)) { begin_unit(Unit::Pd, in); return; }
    if (i == SIZE_MAX) { end_run(in); return; }
    begin_unit(unit_of(steps_[i]), in);
}
void Runner::skip_pending(Reason r) {
    // Every caller ends the run (a unit's own skips are skip_unit): close it before the first skip row is written.
    bool left = false;
    for (const auto& s : std::span<const Step>(steps_.data(), count_)) left |= s.verdict == Verdict::Pending;
    if (left) env_.closing(r);
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.verdict == Verdict::Pending) record_skip(s, r);
    }
    if (!pd_skips_recorded_ || pd_skips_held_) record_pd_skips(true);
}
// The rest of the running unit is over (its charger never fell back, the offers changed, PD showed up): the run goes on.
void Runner::skip_unit(Reason r) {
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.verdict == Verdict::Pending && unit_of(s) == unit_) record_skip(s, r);
    }
}
void Runner::skip_legacy_units(Reason r) {
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.verdict == Verdict::Pending && mode(unit_of(s)) == Mode::Legacy) record_skip(s, r);
    }
}
bool Runner::legacy_row_ran() const {
    for (size_t n = 0; n < count_; ++n)
        if ((steps_[n].verdict == Verdict::Pass || steps_[n].verdict == Verdict::Fail) && mode(unit_of(steps_[n])) == Mode::Legacy) return true;
    return false;
}
bool Runner::unit_row_ran() const {
    for (size_t n = 0; n < count_; ++n)
        if ((steps_[n].verdict == Verdict::Pass || steps_[n].verdict == Verdict::Fail) && unit_of(steps_[n]) == unit_) return true;
    return false;
}
bool Runner::legacy_row_cut() const {
    for (size_t n = 0; n < count_; ++n)
        if (steps_[n].verdict == Verdict::Fail && steps_[n].reason == Reason::Lost && mode(unit_of(steps_[n])) == Mode::Legacy) return true;
    return false;
}
// pattern pps-off-at-no-load (suspected): the per-poll latch of a PD row. Accepted = the sink waits in Transition (Accept seen, no
// PS_RDY yet) in this attach session; PS_RDY = Ready on this row's PDO; the current is the highest the port drew since the Request.
// The latches follow the level the Request out asks (level_mv): a step-walk level or a sweep point is its own Accept / PS_RDY exchange
// (walk_step / sweep_point restart row_accepted_ / row_ps_rdy_ before they ask), so a later level's PS_RDY is seen for itself.
void Runner::latch_row(const Input& in) {
    const auto& s = steps_[index_];
    if (in.session == session_ && in.pd.state == pd::SinkState::Transition) row_accepted_ = true;
    if (!row_ps_rdy_ && in.pd.state == pd::SinkState::Ready && in.pd.result == pd::SinkResult::Ok && in.pd.pos == s.pos &&
        in.pd.mv == level_mv(s)) {
        row_ps_rdy_ = true;
        note_ps_rdy(in, s);
    }
    row_max_ma_ = std::max(row_max_ma_, in.ibus_ma);
}
int Runner::level_mv(const Step& s) const { return is_step_walk(s) ? walk_mv_ : is_sweep(s) ? sweep_mv_ : int(s.mv); }
// The first Ready edge of a level: what its PS_RDY showed. Observation only (Evidence::timing, the [CT] ps_rdy line in finish()): the
// out-of-band reading uses the row's own band, the lateness the sink's Accept / PS_RDY stamps, the inferred flag the sink's own call.
void Runner::note_ps_rdy(const Input& in, const Step& s) {
    Step at = s;
    at.mv = uint16_t(level_mv(s));
    if (!in_band(at, in.vbus_mv, from_mv_)) row_timing_ |= PsRdyOutOfBand;
    if (in.pd.ps_rdy_inferred) { row_timing_ |= PsRdyInferred; return; }
    if (!in.pd.accept_at || in.pd.ps_rdy_at < in.pd.accept_at) return;   // no stamps (a Ready from before this exchange): nothing measured
    const uint32_t ms = in.pd.ps_rdy_at - in.pd.accept_at;
    if (ps_rdy_late(ms, s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs)) row_timing_ |= PsRdyLate;
    row_ps_rdy_ms_ = uint16_t(std::max<uint32_t>(row_ps_rdy_ms_, std::min<uint32_t>(ms, 65535)));
}
// A programmable row the source Accepted and never made ready, whose VBUS collapsed (under 85 % of the row's level, as a cut)
// while the port drew next to nothing: the source turned its output off for lack of load. A fixed row, a PS_RDY that was seen,
// a current drawn and a Hard Reset all stay what they were (Lost / Inline / HardReset).
bool Runner::needs_load(const Input& in) const {
    const auto& s = steps_[index_];
    const bool programmable = s.group == Group::Pd &&
        (s.algorithm == Algorithm::Pps || s.algorithm == Algorithm::SprAvs || s.algorithm == Algorithm::EprAvs);
    return programmable && row_accepted_ && !row_ps_rdy_ && row_max_ma_ < kNeedsLoadMaxMa &&
        in.vbus_mv >= 0 && in.vbus_mv < level_mv(s) * 85 / 100;
}
// The source hard-reset (or detached) in the middle of a PD row: its contract is gone. Wait for it to come back
// (recover_ lets Releasing wait for vSafe5V) and ask the row once more; a further loss ends it as before. An EPR row (the entry
// is what a Hard Reset interrupts: pattern epr-entry-hard-reset) gets kEprEntryRetries retries, each after a longer steady 5 V
// (kEprSettleMs + kEprSettleStepMs per earlier retry); every other row gets one.
bool Runner::can_retry(const Step& s) const {
    if (is_sweep(s)) return false;   // a sweep stops at the first lost source; re-running all its points would double the run
    const unsigned limit = s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs ? kEprEntryRetries : 1;
    return (pd_retry_ == index_ ? retries_ : 0u) < limit;
}
void Runner::retry_pd_row(const Input& in) {
    if (pd_retry_ != index_) retries_ = 0;
    pd_retry_ = index_; ++retries_; end_probe();
    const auto algo = steps_[index_].algorithm;
    settle_ms_ = algo == Algorithm::Epr || algo == Algorithm::EprAvs ? kEprSettleMs + kEprSettleStepMs * (retries_ - 1u) : 0;
    if (low_programmable(steps_[index_])) cut_index_ = index_;
    steps_[index_].verdict = Verdict::Pending; steps_[index_].reason = Reason::None;
    pd_owned_ = false; recover_ = true; session_ = in.session;
    release(in, false);
}
// pattern preauth-list-after-reset: a source whose Xiaomi auth chain was Hard Reset (a 6 A hunt Request answered with one) announces
// its fixed PDOs alone, before the chain runs again, and the full list (APDOs included) ~0.6 s later (Xiaomi 20K, 2026-11-07). The
// rows above the cut-short list are not "offers changed": they are tolerated while the chain has not finished again, at most
// kXiaomiAuthWaitMs from the first cut-short list. The positions the list does carry are still checked row by row by the caller.
// `waiting` is set while the window is open: the next row waits for the list instead of asking a position that is not there.
bool Runner::preauth_tail(const Input& in, bool& waiting) {
    if (!xiaomi_reset_ || in.pd.xiaomi_authed) return false;
    if (!tail_at_) tail_at_ = in.now;
    waiting = in.now - tail_at_ < kXiaomiAuthWaitMs;
    return waiting;
}
bool Runner::mi_chain_wait(const Input& in) {
    if (!in.pd.xiaomi_svid) return false;
    // Chain done: the source re-announces its list right after it; the chain row gives that list kXiaomiReannounceMs.
    if (in.pd.xiaomi_authed)
        return in.xm && !in.xm->reannounced && in.now - in.xm->authed_at < kXiaomiReannounceMs;
    const uint32_t key = in.session * 1000003u + link_resets(in.pd);
    if (!auth_open_ || auth_key_ != key) { auth_open_ = true; auth_key_ = key; auth_at_ = in.now; }
    return in.now - auth_at_ < kXiaomiAuthWaitMs;
}
void Runner::end_probe() {
    if (probe_ == Probe::Running) env_.probe_identity(false);
    if (probe_ != Probe::Pending) probe_ = Probe::Off;   // Pending = still wanted, asked once at the end of the first Pd unit
}
// A legacy row's Lost recovery ended (VBUS is back at vSafe5V): open the window in which the returning Rp's new attach session is
// adopted (pattern usb-a-cuts-on-low-dpdm, kSagAdoptMs). A recovery after a PD row never opens it; any other release closes it.
void Runner::end_sag_recovery(const Input& in) {
    sag_open_ = sag_cut_ && !stopping_; sag_cut_ = false; sag_ms_ = in.now;
}
void Runner::unsafe(Reason r) { end_probe(); end_legacy(); state_ = State::Unsafe; reason_ = r; stable_ = gone_ = false; skip_pending(r); }
// Whatever way a run ends, the sink comes back as a normal attach: Rd on and the PD policy after a detach; the PD policy and a
// rescan after a Legacy attach.
// A self-powered K2 never detached: the PD policy comes back with a Soft_Reset hold (no CC open, no rescan).
void Runner::end_legacy() {
    if (selfpwr_) { if (legacy_active_) env_.hold(Mode::Pd); detached_ = false; }
    else if (detached_) { detached_ = false; env_.attach(Mode::Pd); }
    else if (legacy_active_) { env_.legacy_only(false); env_.pd_rescan(); }
    legacy_active_ = false; ending_ = false;
}
void Runner::end_run(const Input& in) { if (lines_open_cause(in)) lines_open_sweep(); end_legacy(); if (!pd_skips_recorded_ || pd_skips_held_) record_pd_skips(true); state_ = State::Done; }
void Runner::begin_unit(Unit u, const Input& in) {
    if (selfpwr_ && self_path(u) == SelfPath::NeedsExternalPower) {   // a row that can cut the output cannot run on the output itself
        unit_ = u; skip_unit(Reason::NeedsExternalPower); next(in); return;
    }
    unit_ = u; mode_ = mode(u); wd_wait_ = false; race_reset();
    attached_ = silent_ = ending_ = false; attach_try_ = 0; pd_retry_ = cut_index_ = SIZE_MAX;
    rp_restart_ = false; contract_wrong_ = false; attach_max_mv_ = 0;
    pd_owned_ = fc_owned_ = recover_ = hot_ = sag_cut_ = sag_open_ = own_cut_ = false;   // a detach/attach boundary: the window does not cross it
    unit_ms_ = in.now; pre_ms_ = 0;
    if (mode_ == Mode::Legacy) env_.phase(Phase::Handover, index_);   // journal marker BEFORE the detach: a reboot from here is a skipped handover
    if (detached_) begin_attach(in); else begin_detach(in);
}
// The next Legacy unit inside the same PD-silent attach: no detach, a gap for the D+/D- cleanup, then its first handshake.
void Runner::continue_legacy(Unit u, const Input& in) {
    unit_ = u; own_cut_ = false; race_reset();
    pd_owned_ = fc_owned_ = recover_ = hot_ = false; pd_retry_ = cut_index_ = SIZE_MAX;
    unit_ms_ = in.now;
    session_ = in.session; resets_ = link_resets(in.pd); caps_gone_ms_ = in.now;
    state_ = State::Baseline; stable_ = true; phase_ms_ = in.now;   // kLegacyGapMs (+ kAfcPreMs) before the first row
    baseline_wait(legacy_tune(LegacyKnob::GapMs), u == Unit::Afc ? kAfcPreMs : 0, in.now, pre_ms_, stable_ms_);
}
// Detach: Rd off, the 5 V default, then wait for the source to drop its output (VBUS below vSafe0V for kDetachDarkHoldMs).
void Runner::begin_detach(const Input& in) {
    if (selfpwr_) {
        // The K2 runs on this source's VBUS through its pinned Rd: CC is never opened. The boundary is only a bookkeeping step; the
        // next attach is a hold (Soft_Reset or PD-silent). legacy_active_ stays so the run's end gives the PD policy back.
        attached_ = false; detached_ = true; hot_ = false; dark_ = dipped_ = stable_ = false;
        if (ending_) { ending_ = false; next(in); } else begin_attach(in);
        return;
    }
    state_ = State::Detach; att_ms_ = in.now; dark_ = dipped_ = sagged_ = stable_ = false;
    attached_ = legacy_active_ = false;
    env_.detach();
}
void Runner::poll_detach(const Input& in) {
    if (in.vbus_mv < pd::kVSafe0Mv) { dipped_ = true; if (!dark_) { dark_ = true; dark_ms_ = in.now; } } else dark_ = false;
    if (!safe5(in.vbus_mv)) sagged_ = true;
    // No list ever heard and VBUS never left 5 V: an always-on port (A-to-C cable, Rp present), not a charger that is slow to drop.
    const bool always_on = !sagged_ && !evidence_.num_pdos;
    const uint32_t limit = never_dark_ || always_on ? kDetachQuickMs : kDetachDarkMs;
    if (!((dark_ && in.now - dark_ms_ >= kDetachDarkHoldMs) || in.now - att_ms_ >= limit)) return;
    if (!dipped_) never_dark_ = true;   // a source that kept its output up (a USB-A port has no CC): later detaches do not wait long
    detached_ = true; hot_ = false;
    if (rel_rd_) {
        rel_rd_ = false; ending_ = false;
        if (in.vbus_mv >= kUnsafeLowMv && !safe5(in.vbus_mv)) { unsafe(Reason::Release); return; }   // end_legacy() gives Rd back
        pd_owned_ = fc_owned_ = recover_ = false;
        if (stopping_) { stop_run(State::Stopped); return; }
        next(in); return;
    }
    if (ending_) { ending_ = false; next(in); }
    else begin_attach(in);
}
void Runner::begin_attach(const Input& in) {
    state_ = State::Attach; att_ = Att::Caps; att_ms_ = in.now; stable_ = dark_ = false;
    rp_restart_ = false; contract_wrong_ = false; attach_max_mv_ = std::max(in.vbus_mv, 0);
    detached_ = false; legacy_active_ = mode_ == Mode::Legacy;
    wait_ = pd::VbusWait(); wait_.begin(in.now);
    if (selfpwr_) { env_.hold(mode_); return; }
    env_.identity(mode_ != Mode::Legacy);   // the Ra mode + spoof only for the PD units (pattern ra-disables-legacy)
    env_.attach(mode_);
}
// The unit's charger never came back: the rows fail with NoReattach, or, when no list was ever heard (not a PD source), are skipped.
// A PDO 1 contract whose PS_RDY was seen during the wait (contract_wrong_, latched in poll_attach) with VBUS outside vSafe5V
// fails WrongVoltage instead (pattern shared-bus-split), with the highest VBUS seen during the wait, not the reading at the timeout.
void Runner::fail_unit(const Input& in, const pd::SourceCaps& caps, Reason reason, int measured_mv) {
    // A source that never listed PD has nothing to re-attach to: the rows that are still pending (the Xiaomi chain row, which the
    // re-plan keeps) skip like the planned ones instead of failing "no PD after re-attach" (A-port, 2026-11-23).
    const bool no_pd = reason == Reason::NoReattach && !caps.valid && !evidence_.num_pdos;
    if (late_pd_ && !caps.valid && !evidence_.num_pdos) replan_pd(caps, in, true);   // no PD source at all: the plan says NoOffer
    const int measured = reason == Reason::WrongVoltage ? std::max(attach_max_mv_, 0)
                                                        : (measured_mv >= 0 ? measured_mv : in.vbus_mv);   // -1 default: the live reading
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.verdict != Verdict::Pending || unit_of(s) != unit_) continue;
        s.verdict = no_pd ? Verdict::Skip : Verdict::Fail;
        s.reason = !no_pd ? reason : in.self_powered && in.pd_seen_attach ? Reason::PdGaveUp : Reason::NoOffer;
        s.measured_mv = static_cast<uint16_t>(std::clamp(measured, 0, 65535)); env_.record(s);
    }
    ending_ = true; begin_detach(in);
}
void Runner::poll_attach(const Input& in, const pd::SourceCaps& caps, uint32_t gap) {   // gap: ms since the previous poll (poll_impl() has already moved last_ms_)
    if (mode_ == Mode::Legacy) {
        if (in.vbus_mv > attach_max_mv_) attach_max_mv_ = in.vbus_mv;
        // After the SVOOC ceiling's Rd cycle: an output that never came back in kRecoverMs is a lost source (the run's rows
        // are skipped "source lost", the ceiling's record stays), not a charger that kept PD up ("blocked by PD" after 80 s).
        if (ceil_cycled_ && attach_max_mv_ < kUnsafeLowMv && in.now - att_ms_ >= kRecoverMs) {
            ceil_cycled_ = false;
            stop_run(State::Stopped, Reason::Lost, true);
            return;
        }
        // PD-silent: caps are heard and recorded, never acknowledged. VBUS cycles around the source's Hard Resets are expected.
        if (late_pd_ && caps.valid) replan_pd(caps, in, true);
        if (stable_tick(safe5(in.vbus_mv) && in.pd_quiet_ms >= quiet_ms() && in.fc_state == fc::State::Idle,
                        in.now, gap, legacy_tune(LegacyKnob::SteadyMs))) {
            if (handover_.outcome == HandoverOutcome::NotTried) handover_ = {HandoverOutcome::FellBack, in.now - unit_ms_};
            attached_ = silent_ = true; session_ = in.session; resets_ = link_resets(in.pd); hot_ = false; caps_gone_ms_ = in.now;
            ceil_cycled_ = false; own_rel_ = false; reann_ = 0;
            // The first handshake waits 1000 ms after the PD-silent attach (the baseline settle is 500 of it).
            baseline_wait(legacy_tune(LegacyKnob::FirstWaitMs), unit_ == Unit::Afc ? kAfcPreMs : 0, in.now, pre_ms_, stable_ms_);
            state_ = State::Baseline; stable_ = true; phase_ms_ = in.now;
            return;
        }
        // Rp on the line and still no VBUS: the retry's Rd cycle now, not after the give-up wait.
        const bool rp_no_vbus = caps.active_cc && in.vbus_mv >= 0 && in.vbus_mv < pd::kVSafe0Mv && in.now - att_ms_ >= kLegacyNoVbusMs;
        if (rp_no_vbus && !selfpwr_ && !attach_try_ && handover_.outcome != HandoverOutcome::StayedPd) {
            K2LOG("[CT] Rp without VBUS %ums into the attach: Rd cycle\n", unsigned(in.now - att_ms_));
            attach_try_ = 1; begin_detach(in); return;
        }
        if (in.now - att_ms_ < (attach_try_ ? kLegacyRetryMs : kLegacyGiveUpMs)) return;
        if (!selfpwr_ && !attach_try_ && handover_.outcome != HandoverOutcome::StayedPd) { attach_try_ = 1; begin_detach(in); return; }   // once more, longer
        if (handover_.outcome == HandoverOutcome::NotTried) handover_ = {HandoverOutcome::StayedPd, in.now - unit_ms_};
        skip_unit(Reason::BlockedByPd);
        ending_ = true; begin_detach(in);
        return;
    }
    // The highest VBUS of the wait and the wrong-contract latch, updated on every poll of the wait (both phases): a
    // WrongVoltage failure records the max, not the reading at the timeout, and PS_RDY at PDO 1 seen with VBUS outside
    // vSafe5V stays latched even after the source tears the port down (pattern shared-bus-split: the splitter cuts
    // VBUS+Rp before kAttachContractMs runs out, so the fail sites below cannot re-ask the sink state).
    if (in.vbus_mv > attach_max_mv_) attach_max_mv_ = in.vbus_mv;
    if (in.pd.state == pd::SinkState::Ready && in.pd.pos == 1 && !safe5(in.vbus_mv)) contract_wrong_ = true;
    if (att_ == Att::Caps) {
        const auto st = wait_.update(in.now, in.vbus_mv, caps.active_cc != 0, caps.valid);
        if (st == pd::VbusWait::State::Waiting) return;
        if (st != pd::VbusWait::State::Ready || !caps.valid || !caps.active_cc) {
            fail_unit(in, caps, contract_wrong_ ? Reason::WrongVoltage : Reason::NoReattach); return;
        }
        att_ = Att::Contract; att_ms_ = in.now;
        if (late_pd_) replan_pd(caps, in, true);
    }
    // A source-side drop (Rp gone) during the attach wait restarts the wait once (pattern slow-rp-wake: the standby
    // pull-up answers ~1.1-1.7 s after a steady Rd) instead of letting the kAttachContractMs timer run on.
    if (!caps.active_cc && !rp_restart_) {
        rp_restart_ = true; att_ = Att::Caps; att_ms_ = in.now; wait_ = pd::VbusWait(); wait_.begin(in.now);
        return;
    }
    // Caps are in (the attach requested PDO 1): wait for the 5 V contract, bounded.
    const bool settled = caps.valid && safe5(in.vbus_mv) && in.pd.state == pd::SinkState::Ready;
    const bool late = in.now - att_ms_ >= kAttachContractMs;
    if (!settled && !(late && caps.valid && safe5(in.vbus_mv))) {
        if (late) {
            // PS_RDY at PDO 1 was seen during the wait with VBUS outside vSafe5V (contract_wrong_): the source holds
            // the wrong voltage on the 5 V contract (pattern shared-bus-split) - that is not "no PD after re-attach":
            // PD came back, the voltage was wrong.
            fail_unit(in, caps, contract_wrong_ ? Reason::WrongVoltage : Reason::NoReattach);
        }
        return;
    }
    regrow_plan(caps, in);
    attached_ = true; session_ = in.session; resets_ = link_resets(in.pd); hot_ = false; caps_gone_ms_ = in.now; ceil_cycled_ = false;
    // EPR PDOs only exist after EPR entry: an EPR-capable source on an EPR cable is entered first, then the rows are rebuilt.
    // A cable that cannot carry EPR gets no entry, only the informational list (stage 4).
    if (epr_stage_ == 3 && unit_ == Unit::Pd) decide_epr_stage(caps, in);
    state_ = State::Baseline; stable_ = false; phase_ms_ = in.now;   // the usual 500 ms of vSafe5V, then the first row
}
// The SVOOC ceiling stage (svooc_ceiling.hpp). One stage per run, queued by finish() once the VOOC family was detected and the
// plan holds an SVOOC row. NO LOAD: the guarded trigger asks the adapter for 1.5 / 4 / 6.3 / 10.9 A in protocol words only, and
// the highest ask the adapter went on under is the result. It runs at 5 V with the family session already released.
bool Runner::ceiling_wanted() const {
    if (opts_.svooc_cap == svooc_ceiling::Mode::Off || opts_.level != Level::Deep || ceiling_done_) return false;   // Deep only
    for (size_t n = 0; n < count_; ++n)
        if (steps_[n].group == Group::SuperVooc || steps_[n].group == Group::SuperVooc65) return true;
    return false;
}
bool Runner::begin_ceiling(const Input& in) {
    ceiling_pending_ = false; ceiling_done_ = true;
    evidence_.svooc_word = 0; evidence_.svooc_why = 0; evidence_.svooc_asks = 0; evidence_.svooc_vmax_mv = evidence_.svooc_stop_ms = 0;
    const uint8_t refused = env_.ceiling_start();
    if (refused) {
        evidence_.svooc_end = static_cast<uint8_t>(svooc_ceiling::End::Refused); evidence_.svooc_why = refused;
        K2LOG("[CT] svooc_cap refused why=%u\n", unsigned(refused));
        return false;
    }
    evidence_.svooc_end = static_cast<uint8_t>(svooc_ceiling::End::None);
    state_ = State::Ceiling; ceil_ms_ = in.now; ceil_stopping_ = false; ceil_end_ = svooc_ceiling::End::None; stable_ = false;
    ceil_why_ = 0;
    K2LOG("[CT] svooc_cap start\n");
    return true;
}
// What the trigger has shown so far goes to the evidence (the word only ever rises: the highest ask accepted).
void Runner::note_ceiling(const Input& in, svooc_ceiling::End e) {
    evidence_.svooc_end = static_cast<uint8_t>(e);
    evidence_.svooc_word = std::max(evidence_.svooc_word, in.svooc_word);
    evidence_.svooc_asks = std::max(evidence_.svooc_asks, in.svooc_asks);
    evidence_.svooc_vmax_mv = std::max(evidence_.svooc_vmax_mv, in.svooc_vmax_mv);
    if (in.svooc.commands) evidence_.svooc_stop_ms = in.svooc_last_ms;   // the adapter's last edge (nothing heard: 0)
    // The Runner's own guard names the cause; the trigger only saw its stop() (Abort::Stopped).
    evidence_.svooc_why = e == svooc_ceiling::End::Aborted ? (ceil_why_ ? ceil_why_ : in.svooc.abort) : uint8_t(0);
    K2LOG("[CT] svooc_cap end=%u word=%u asks=%u why=%u vmax=%umV stop=%ums\n", unsigned(evidence_.svooc_end),
          unsigned(evidence_.svooc_word), unsigned(evidence_.svooc_asks), unsigned(evidence_.svooc_why),
          unsigned(evidence_.svooc_vmax_mv), unsigned(evidence_.svooc_stop_ms));
}
void Runner::end_ceiling(const Input& in, svooc_ceiling::End e) {
    note_ceiling(in, e);
    ceil_stopping_ = false;
    // The adapter raised VBUS during the session: the release waits for a settled 5 V, and an adapter that drops its output
    // instead is brought back by an Rd cycle (poll_impl, Releasing).
    if (in.svooc_vmax_mv > pd::kVSafe5MaxMv) hot_ = true;
    ceil_recover_ = true; ceil_low_ms_ = 0;
    release(in, false);
}
void Runner::poll_ceiling(const Input& in) {
    if (!ceil_stopping_) {
        const Reason fault = base_fault(in);
        const auto e = fault != Reason::None ? svooc_ceiling::End::Aborted : svooc_ceiling::end_of(in.svooc, in.now - ceil_ms_);
        if (e == svooc_ceiling::End::None) return;
        if (fault != Reason::None)   // the record names the Runner's guard, in the trigger's own abort codes
            ceil_why_ = static_cast<uint8_t>(fault == Reason::Inline ? svooc::Abort::Sink :
                                             fault == Reason::NoMonitor ? svooc::Abort::NoMonitor : svooc::Abort::Stopped);
        if (in.svooc.running || fault != Reason::None) {   // judged, capped or faulted: ask the trigger to stop, its exit watch follows
            env_.ceiling_stop(); ceil_stopping_ = true; ceil_stop_ms_ = in.now; ceil_end_ = e;
            return;
        }
        end_ceiling(in, e);
        return;
    }
    if (!in.svooc.running || in.now - ceil_stop_ms_ >= svooc_ceiling::kExitGraceMs) end_ceiling(in, ceil_end_);
}
Step Runner::window_step() const {
    if (count_ == 0) return {};
    const size_t at = std::min(index_, count_ - 1);
    const size_t done = std::min(index_, count_);   // rows done before the index (the index sits on the next unit, or past the plan)
    switch (interlude()) {
    case Interlude::Spec:
        if (spec_hold_.what != SpecWhat::None) return spec_step(spec_hold_);
        return steps_[done ? done - 1 : 0];
    case Interlude::Probe: return steps_[done ? done - 1 : 0];
    default: return steps_[at];   // the ceiling's index did not move: the family row just finished
    }
}
// The PD spec stage (spec_section.hpp). One stage per run, queued at START for a Deep run with a Pd unit and begun by next() when the
// first Pd unit is over: its contract released to a settled 5 V, the charger asked who it is. NO LOAD, nothing above what the charger
// advertises: the engine runs only its Transitions and PPS / AVS families (specsec::kFamilyMask) and takes every level from the live
// list. It owns the link while it runs (the Runner asks no PDO itself); the Runner only watches the run's guards and the clock, and
// brings the source back to 5 V before the unit's detach.
bool Runner::spec_wanted() const { return !spec_done_ && spec_planned(opts_, opts_.units); }
bool Runner::begin_spec(const Input& in) {
    spec_pending_ = false; spec_done_ = true;
    evidence_.spec = {};
    const uint8_t refused = env_.spec_start();
    if (refused == 255) return false;   // a build without the engine: no stage, no section
    if (refused) {
        evidence_.spec = specsec::all_skipped(specsec::End::Refused, refused, specsec::sc::Skip::Refused);
        K2LOG("[CT] spec refused why=%u\n", unsigned(refused));
        env_.spec_done(evidence_.spec);
        return false;
    }
    state_ = State::Spec; spec_ms_ = in.now; spec_stopping_ = spec_fault_ = false; stable_ = false; spec_pt_ = spec_hold_ = {};
    K2LOG("[CT] spec start\n");
    return true;
}
void Runner::poll_spec(const Input& in) {
    spec_pt_ = in.spec_pt;
    if (spec_pt_.what == SpecWhat::Fixed || spec_pt_.what == SpecWhat::Pps || spec_pt_.what == SpecWhat::Avs) spec_hold_ = spec_pt_;
    if (!spec_stopping_) {
        if (!in.spec_running) { end_spec(in, specsec::End::Done); return; }
        const Reason fault = base_fault(in);
        if (fault != Reason::None || in.now - spec_ms_ >= specsec::kMaxMs) {   // a guard of the run, or the engine's own cap passed
            spec_fault_ = true; spec_stopping_ = true; spec_stop_ms_ = in.now;
            env_.spec_stop();
        }
        return;
    }
    if (!in.spec_running || in.now - spec_stop_ms_ >= specsec::kStopGraceMs)
        end_spec(in, spec_fault_ ? specsec::End::Fault : specsec::End::Cancelled);
}
// The engine is over (or abandoned): its cells are the stage's evidence. The source goes back to 5 V; a cancelled run stops there.
void Runner::end_spec(const Input& in, specsec::End e) {
    specsec::Result& r = evidence_.spec;   // filled in place: a local copy would add 112 B to the deepest stack path
    r = {};
    env_.spec_collect(r);
    if (e != specsec::End::Done || r.end == specsec::End::NotRun) r.end = e;
    const auto t = r.tally();
    K2LOG("[CT] spec end=%u why=%u pass=%u fail=%u skip=%u unconfirmed=%u\n", unsigned(r.end), unsigned(r.why), t.pass, t.fail, t.skip,
          unsigned(r.unconfirmed));
    env_.spec_done(evidence_.spec);
    const bool cancelled = spec_stopping_ && !spec_fault_;
    spec_stopping_ = false;
    // The engine's own release should have left a Ready contract at 5 V; anything else is handed back like a row's level.
    pd_owned_ = !safe5(in.vbus_mv) || in.pd.mv != 5000 || in.pd.state != pd::SinkState::Ready || r.unconfirmed;
    hot_ = !safe5(in.vbus_mv);
    release(in, cancelled);
}
void Runner::cancel(Input in) {
    if (!busy() || state_ == State::Unsafe || (state_ == State::Releasing && stopping_)) return;
    end_probe();
    ++revision_;
    reason_ = Reason::Cancelled;
    if (state_ == State::Spec) {   // the engine ends in order (its own return to 5 V); poll_spec() then stops the run
        spec_fault_ = false;
        if (!spec_stopping_) { env_.spec_stop(); spec_stopping_ = true; spec_stop_ms_ = in.now; }
        return;
    }
    if (state_ == State::Ceiling) {   // the trigger's exit watch brings the lines back to 5 V while the run is Releasing
        note_ceiling(in, svooc_ceiling::End::Cancelled);
        env_.ceiling_stop(); ceil_stopping_ = false;
        release(in, true);
        return;
    }
    if (state_ == State::Releasing) { cut_wait_ = race_wait_ = false; stopping_ = true; skip_pending(reason_); return; }
    if (state_ == State::Detach || state_ == State::Attach) { stop_run(State::Stopped, Reason::None, true); return; }
    if (index_ < count_ && steps_[index_].verdict == Verdict::Running) {
        auto& s = steps_[index_]; s.verdict = Verdict::Fail; s.reason = reason_;
        s.measured_mv = static_cast<uint16_t>(std::clamp(in.vbus_mv, 0, 65535)); env_.record(s);
    }
    release(in, true);
}
void Runner::restore(const Options& o, const Evidence& e, uint32_t run_id, std::span<const Step> steps, Phase phase,
                     size_t phase_index, uint8_t resumed, size_t attempt_unit, uint8_t attempts, bool planned_reattach) {
    count_ = std::min(steps.size(), steps_.size());
    std::copy_n(steps.begin(), count_, steps_.begin());
    evidence_ = e; spec_apdo_ = {}; run_id_ = run_id; ++revision_; resumed_ = resumed; reattach_planned_ = planned_reattach; loaded_ = true; plan_frozen_ = true;
    opts_ = o;
    // A replay without the meta record (power cut between the header and it) carries no level: a Light plan names itself by its
    // rows (Advertised / LightLevel skips exist only there), so it never resumes as Medium and asks above 5 V.
    if (opts_.level == Level::Medium)
        for (const auto& s : std::span<const Step>(steps_.data(), count_))
            if (s.verdict == Verdict::Skip && (s.reason == Reason::Advertised || s.reason == Reason::LightLevel)) { opts_ = with_level(opts_, Level::Light); break; }
    opts_.units = resolve(opts_); n_order_ = opts_.units.n; order_ = opts_.units.u;
    // A resumed run re-sweeps a pending sweep row from its first point: the record starts empty, the journaled row is the plan.
    sweep_used_ = 0; n_sweep_rows_ = 0; sweep_n_ = 0; sweep_mv_ = sweep_lo_ = sweep_hi_ = 0;
    reset_run_state();
    epr_stage_ = 0; info_ = 0; info_rows_ = false;
    pd_skips_recorded_ = true; probe_ = Probe::Off;
    ceiling_done_ = true; spec_done_ = true;
    for (auto& s : std::span<Step>(steps_.data(), count_)) if (s.verdict == Verdict::Running) s.verdict = Verdict::Pending;
    // The EPR proof (the source accepted EPR entry: it checked the cable itself) is not journaled as such, but its trace is: the EPR
    // list was heard (EprTable), or EPR rows were planned (a plan only holds them for a 50 V / 5 A cable). A cable the K2 reads live
    // after the reboot still wins (epr_proven needs !cable_observed). Without the latch the pending EPR rows skipped "cable limit".
    epr_proof_ = (e.flags & EprTable) != 0;
    for (const auto& s : std::span<const Step>(steps_.data(), count_))
        if ((s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) && s.verdict != Verdict::Skip) epr_proof_ = true;
    // A run started with no list heard planned one placeholder row per PD unit (evidence: no PDOs). Those rows are not a final plan:
    // the first list heard after the resume replans them, and the EPR entry is decided with it, as in an uninterrupted run.
    late_pd_ = false;
    if (e.num_pdos == 0 && (has_unit(opts_.units, Unit::Pd) || has_unit(opts_.units, Unit::PdLow)))
        for (const auto& s : std::span<const Step>(steps_.data(), count_))
            if (s.verdict == Verdict::Pending && mode(unit_of(s)) == Mode::Pd) late_pd_ = true;
    // The power can also go after that first plan and before the EPR entry: PDO 1 carries the EPR bit, no PD row has run, and the EPR
    // rows are still the SPR list's placeholders (skipped without an entry answer). The entry was not decided yet: it is decided again.
    bool entry_open = false;
    if ((e.flags & EprCapable) && !(e.flags & EprTable) && !late_pd_) {
        bool pd_ran = false, placeholders = false;
        for (const auto& s : std::span<const Step>(steps_.data(), count_)) {
            if (mode(unit_of(s)) != Mode::Pd) continue;
            if (s.verdict != Verdict::Pending && s.verdict != Verdict::Skip) pd_ran = true;
            if ((s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) && s.verdict == Verdict::Skip &&
                (s.reason == Reason::NoOffer || s.reason == Reason::CableLimit) && s.detail < kDetailEprOfferedRefused)
                placeholders = true;
        }
        entry_open = placeholders && !pd_ran && has_unit(opts_.units, Unit::Pd);
    }
    if ((late_pd_ || entry_open) && (opts_.selected & (bit(Algorithm::Epr) | bit(Algorithm::EprAvs)))) epr_stage_ = 3;
    plan_frozen_ = !late_pd_ && !(entry_open && epr_stage_ == 3);   // placeholders are no plan: the list (and the EPR list after the entry) replans them, as at START
    reason_ = Reason::Lost;
    if (phase != Phase::None && phase_index < count_ && steps_[phase_index].verdict == Verdict::Pending) {
        // The loss is not the charger's verdict: it is charged to the step's unit (the count survives in the journal). Under the
        // budget the marker is voided and the row asked again after the resume; the last one takes the unit's remaining rows out.
        const Unit u = unit_of(steps_[phase_index]);
        const unsigned lost = (attempt_unit == size_t(u) ? attempts : 0) + 1;
        if (lost < kRebootBudget) {
            env_.attempt(size_t(u), uint8_t(lost));
            env_.phase(Phase::None, phase_index);
        } else {
            // The unit cannot be tested on this supply: all its rows are skipped and the next unit runs.
            for (size_t n = phase_index; n < count_; ++n)
                if (steps_[n].verdict == Verdict::Pending && unit_of(steps_[n]) == u) record_skip(steps_[n], Reason::RebootLimit);
            reason_ = Reason::RebootLimit;
            if (phase == Phase::Handover) handover_ = {HandoverOutcome::Reboot, 0};
        }
    }
    index_ = 0;
    while (index_ < count_ && steps_[index_].verdict != Verdict::Pending) ++index_;
    list_tracking_ = false; flight_restart_ = wd_wait_ = false;   // the settle window is seeded by the first poll after the resume
    state_ = State::Stopped;
}
bool Runner::unfinished() const {
    for (const auto& s : steps()) if (s.verdict == Verdict::Pending || s.verdict == Verdict::Running) return true;
    return false;
}
void Runner::suspend() {
    // The unfinished step is not a verdict on the charger: back to Pending (its journal marker stays, so a reboot charges the
    // loss to it exactly like any other power loss). Nothing is released: there is no output left to hand back.
    for (auto& s : std::span<Step>(steps_.data(), count_)) if (s.verdict == Verdict::Running) s.verdict = Verdict::Pending;
    pd_owned_ = fc_owned_ = stopping_ = recover_ = false;
    if (state_ == State::Spec) { env_.spec_stop(); spec_stopping_ = false; }   // the engine has no link left to run on
    if (selfpwr_) {   // no output to talk to: give the PD policy back, but send nothing
        if (legacy_active_) env_.legacy_only(false);
        legacy_active_ = detached_ = ending_ = false;
    } else end_legacy();
    reason_ = Reason::Lost; suspended_ = true; state_ = State::Stopped;
}
bool Runner::resumable() const {
    if (!(loaded_ || suspended_) || state_ != State::Stopped || resumed_ >= kMaxResumes) return false;
    for (const auto& s : steps()) if (s.verdict == Verdict::Pending) return true;
    return false;
}
Reason Runner::resume(Input in, bool confirmed) {
    if (!resumable()) return Reason::Busy;
    const Reason denied = refusal({.selected = kAllAlgorithms}, in, busy(), confirmed);
    if (denied != Reason::None) return denied;
    // A resume on a boot whose /rd.rec said Reattach is the charger test's planned re-attach, not a power loss: it is
    // counted separately (the card's re-attach flag) and never in resumed_ (the card's resume counter, the reboot budget).
    if (reattach_planned_) { reattach_planned_ = false; if (reattach_ < 255) ++reattach_; }
    else ++resumed_;
    ++revision_;
    session_ = in.session; resets_ = hr_count_ = link_resets(in.pd);
    xiaomi_seen_ = xiaomi_reset_ = x_svid_ = x_authed_ = false; tail_at_ = 0;
    // A Pd unit goes on in the live attach (the K2 booted into a 5 V contract). A Legacy unit does what an uninterrupted run does:
    // its own detach and PD-silent attach (the Rd stays held through a detach on a charger-powered K2, so it cannot loop on its
    // own; a second loss in the handover ends the unit, see restore()). The units after it attach as usual.
    const size_t first = pick();
    unit_ = first == SIZE_MAX ? Unit::Count : unit_of(steps_[first]); mode_ = unit_ == Unit::Count ? Mode::Pd : mode(unit_);
    attached_ = first != SIZE_MAX && mode_ == Mode::Pd; detached_ = ending_ = silent_ = legacy_active_ = false; attach_try_ = 0; unit_ms_ = in.now;
    unit_time_.fill(0); time_ms_ = 0; time_pending_ = true;
    rp_restart_ = false; contract_wrong_ = false; attach_max_mv_ = 0;
    last_ms_ = phase_ms_ = in.now; stable_ = stopping_ = pd_owned_ = fc_owned_ = recover_ = sag_cut_ = sag_open_ = false;
    reason_ = Reason::None; loaded_ = false; suspended_ = false;
    selfpwr_ = in.self_powered; adopt_ = true; held_ = false; jwait_ms_ = 0;   // the page arms the spoof/cable now: that session change is not a loss
    probe_ = Probe::Off;   // a resumed run does not ask again
    state_ = State::Baseline;
    dip_open_ = dip_reading(in.vbus_mv); dip_ms_ = in.now; dip_back_ = false; dips_ = 0;
    if (!safe5(in.vbus_mv) && !dip_reading(in.vbus_mv)) { unsafe(Reason::Release); return reason_; }
    return Reason::None;
}
// The run's time per unit, one [CT] line when it ends (Done, Stopped): what each protocol cost, for the lab's speed work.
// (noinline: its 13 outgoing arguments stay out of poll()'s frame, above poll_impl's.)
[[gnu::noinline]] static void log_unit_time(const std::array<uint32_t, size_t(Unit::Count)>& t) {
    static_assert(unsigned(Unit::PdLow) + 1 == unsigned(Unit::Count), "the split line names every unit");
    K2LOG("[CT] unit ms PD %u QC2 %u QC3 %u QC3+ %u AFC %u FCP %u SCP %u VOOC %u SVOOC %u SVOOC65 %u UFCS %u VIVO %u PDlow %u\n",
          unsigned(t[0]), unsigned(t[1]), unsigned(t[2]), unsigned(t[3]), unsigned(t[4]), unsigned(t[5]), unsigned(t[6]),
          unsigned(t[7]), unsigned(t[8]), unsigned(t[9]), unsigned(t[10]), unsigned(t[11]), unsigned(t[12]));
}
void Runner::count_unit_time(uint32_t now) {
    if (busy()) {
        if (unit_ < Unit::Count && time_ms_ && now - time_ms_ < 2000) unit_time_[size_t(unit_)] += now - time_ms_;
        time_ms_ = now;
        return;
    }
    time_ms_ = 0;
    if (!time_pending_) return;
    time_pending_ = false;
    log_unit_time(unit_time_);
}
void Runner::poll(Input in, const pd::SourceCaps& caps) {
    const auto state = state_;
    const auto reason = reason_;
    const auto index = index_;
    count_unit_time(in.now);
    poll_impl(in, caps);
    if (!busy()) count_unit_time(in.now);
    // Stored rows change only with a state/index transition; timer-only polls do not invalidate reads.
    if (state_ != state || reason_ != reason || index_ != index) ++revision_;
}
[[gnu::noinline]] void Runner::reset_run_state() {
    handover_ = {}; unit_ = Unit::Count; mode_ = Mode::Pd;
    attached_ = detached_ = ending_ = silent_ = legacy_active_ = never_dark_ = dipped_ = dark_ = rel_rd_ = false; attach_try_ = 0;
    pd_skips_held_ = false;
    pd_owned_ = fc_owned_ = stopping_ = stable_ = recover_ = hot_ = false;
    pd_retry_ = cut_index_ = SIZE_MAX;
    cut_wait_ = cut_dark_ = false; cut_start_ms_ = cut_back_ms_ = 0; cut_retry_idx_ = SIZE_MAX; cut_tries_ = 0; cut_last_mv_ = -1; n_cut_onsets_ = 0; race_reset();
    selfpwr_ = suspended_ = adopt_ = false;
    ceiling_pending_ = ceil_stopping_ = ceil_recover_ = ceil_cycled_ = false;
    spec_pending_ = spec_stopping_ = spec_fault_ = false;
    rp_restart_ = contract_wrong_ = false; attach_max_mv_ = 0;
    list_tracking_ = flight_restart_ = wd_wait_ = false;
}

[[gnu::noinline]] bool Runner::stable_tick(bool cond, uint32_t now, uint32_t gap, uint32_t hold_ms) {
    if (!cond) { stable_ = false; return false; }
    if (!stable_ || gap > 250) { stable_ = true; stable_ms_ = now; }
    return now - stable_ms_ >= hold_ms;
}

[[gnu::noinline]] void Runner::stop_run(State st, Reason r, bool skip) {
    if (r != Reason::None) reason_ = r;
    if (skip) skip_pending(reason_);
    end_legacy();
    state_ = st;
    pd_owned_ = fc_owned_ = recover_ = gone_ = false;
}

[[gnu::noinline]] void Runner::record_skip(Step& s, Reason r, int detail) {
    s.verdict = Verdict::Skip;
    s.reason = r;
    if (detail >= 0) s.detail = static_cast<uint8_t>(detail);
    env_.record(s);
}

[[gnu::noinline]] void Runner::send_pd_request(const Input& in, const Step& s, bool restart) {
    const auto err = env_.request_pd(pd_choice(s));
    if (err == pd::Err::Busy) {
        if (restart) flight_restart_ = true;
        busy_retry_at_ = in.now + 200;
    } else if (err != pd::Err::Ok) {
        finish(in, Verdict::Fail, Reason::Request, 128 + static_cast<uint8_t>(err));
    }
}

[[gnu::noinline]] void Runner::fail_and_recover(const Input& in, Verdict v, Reason r, uint8_t detail, bool is_pd) {
    if (is_pd) {
        if (low_programmable(steps_[index_])) cut_index_ = index_;
        pd_owned_ = false;
    } else {
        sag_cut_ = true;
    }
    recover_ = true;
    session_ = in.session;
    finish(in, v, r, detail);
    if (state_ == State::Releasing) phase_ms_ = in.now;
}

[[gnu::noinline]] void Runner::decide_epr_stage(const pd::SourceCaps& caps, const Input& in) {
    epr_stage_ = !caps.epr && epr_capable_caps(caps) && caps.active_cc
        ? (epr_cable_unfit(in) || opts_.level == Level::Light ? 4 : 1) : 0;
    info_ = 0; info_rows_ = false;
}

[[gnu::noinline]] void Runner::epr_refused(const pd::SourceCaps& caps, const Input& in) {
    epr_stage_ = 0; epr_info_rows(caps, in);
    epr_skips(Reason::EprRefused, uint8_t(kDetailEprFail + std::min<unsigned>(in.pd.epr_fail, 7)));
}

[[gnu::noinline]] bool Runner::finish_class_b(const Input& in, const Step& s, uint8_t detail) {
    if (is_class_b_peak(s) && s.algorithm == Algorithm::Qc2ClassB && qc3_class_b_peak_passed({steps_.data(), count_})) {
        finish(in, Verdict::Fail, Reason::Qc3Only20V, detail);
        return true;
    }
    if (class_a_only(s, {steps_.data(), count_})) {
        finish(in, Verdict::Skip, Reason::ClassAOnly);
        return true;
    }
    return false;
}

[[gnu::noinline]] bool Runner::revive_withdrawn_row(Step& s, const pd::SourceCaps& caps) {
    const int at = s.pos >= 8 ? (caps.epr ? find_offer(caps, s) : (epr_capable_caps(caps) ? 0 : -1))
                              : find_offer(caps, s);
    if (at < 0) return false;
    if (s.pos < 8 || caps.epr) {
        const uint32_t raw = caps.pdos[static_cast<size_t>(at)].raw;
        s.pos = static_cast<uint8_t>(at + 1); s.pdo = raw;
    }
    s.verdict = Verdict::Pending; s.reason = Reason::None; env_.record(s);
    return true;
}

[[gnu::noinline]] void Runner::note_replan(unsigned follow, unsigned withdrawn, unsigned back) {
    ++revision_; env_.plan_changed();
    if (evidence_.rebalanced < 255) ++evidence_.rebalanced;
    env_.replan(follow, withdrawn, back);
}

[[gnu::noinline]] void Runner::poll_releasing(Input& in, const pd::SourceCaps& caps, uint32_t gap,
                                              bool offers_changed, bool legacy_pending, bool pd_active, bool tail_wait) {
    // A non-PD source that browned out (a bank asked for a level it does not have) comes back on its own:
    // wait for vSafe5V instead of ending the run. A PD contract that vanished still stops it.
    // pattern budget-reannounce-mid-run: the sink verifies the 5 V release against VBUS and, as the last resort, sends a
    // Hard Reset of its own. That dip is the recovery of this release (the port comes back at 5 V), not a lost source.
    if (pd_owned_ && in.pd.hard_resets != rel_hr_) {
        pd_owned_ = false; recover_ = true; phase_ms_ = in.now; stable_ = false; rel_hr_ = in.pd.hard_resets;
    }
    // The SVOOC ceiling stage is over and the adapter dropped its output instead of going back to 5 V (it waits for a new
    // sink): the run's own unit boundary brings it back, Rd off and on again, with the stage's result kept.
    if (ceil_recover_ && !stopping_ && !pd_owned_ && !fc_owned_ && in.vbus_mv >= 0 && in.vbus_mv < kUnsafeLowMv) {
        if (!ceil_low_ms_) ceil_low_ms_ = in.now ? in.now : 1;
        if (in.now - ceil_low_ms_ >= svooc_ceiling::kDropMs) {
            K2LOG("[CT] svooc_cap vbus=%dmV after the stage: Rd cycle\n", int(in.vbus_mv));
            ceil_recover_ = false; ceil_low_ms_ = 0; recover_ = false; ceil_cycled_ = true;
            ending_ = true; begin_detach(in);
            return;
        }
    } else ceil_low_ms_ = 0;
    bool dropped = in.session != session_ || (in.vbus_mv >= 0 && in.vbus_mv < 1000);
    if (dropped && pd_owned_ && !stopping_ && index_ < count_ && low_programmable(steps_[index_])) {
        // The low edge cost the source its output while the row was handed back: wait for it like a cut row.
        cut_index_ = index_; pd_owned_ = false; recover_ = true; phase_ms_ = in.now;
    }
    if (dropped && !pd_owned_ && !stopping_) {
        if (!recover_) { recover_ = true; phase_ms_ = in.now; }
        session_ = in.session; stable_ = false;
    } else if (recover_) session_ = in.session;
    // pattern output-reset-after-unanswered-scp: a PD-silent Legacy unit whose source cut its output and is back waits for the
    // announcement that may follow (kRestartCapsMs, kLegacyQuietMs after a frame, kRestartSettleMs at most) before its next row.
    if (!recover_ || pd_owned_ || mode_ != Mode::Legacy || !silent_ || !attached_) restart_dark_ = restart_wait_ = false;
    else if (in.vbus_mv >= 0 && in.vbus_mv < 1000) { restart_dark_ = true; restart_wait_ = false; }
    else if (restart_dark_ && safe5(in.vbus_mv)) { restart_dark_ = false; restart_wait_ = true; restart_ms_ = in.now; }
    if (restart_wait_) {
        const uint32_t back = in.now - restart_ms_;
        const bool heard = in.pd_quiet_ms < back;   // a frame since VBUS came back
        restart_wait_ = back < kRestartSettleMs && (heard ? in.pd_quiet_ms < quiet_ms() : back < (reann_ == 2 ? quiet_ms() : kRestartCapsMs));
    }
    // pattern reannounce-after-own-release: same wait, counted from our own release (no VBUS dip to count from).
    if (own_rel_) {
        const uint32_t since = in.now - own_rel_ms_;
        const bool heard = in.pd_quiet_ms < since;   // a frame since the release
        if (heard) reann_ = 2;
        else if (since >= kOwnReleaseCapsMs) reann_ = 1;
        own_rel_ = !stopping_ && silent_ && attached_ && mode_ == Mode::Legacy && !restart_wait_ && !restart_dark_ && since < kOwnReleaseSettleMs &&
                   (heard ? in.pd_quiet_ms < quiet_ms() : since < kOwnReleaseCapsMs);
    }
    // pattern legacy-start-races-announcement: the retried first step waits for real quiet (kLegacyQuietMs), at most kRestartSettleMs.
    if (race_wait_) race_wait_ = !stopping_ && silent_ && attached_ && mode_ == Mode::Legacy && in.now - race_ms_ < kRestartSettleMs && in.pd_quiet_ms < kLegacyQuietMs;
    const Reason changed = dropped && pd_owned_ ? Reason::Lost : in.loop_lock ? Reason::Loop :
        in.inline_load ? Reason::Inline : in.fake ? Reason::Fake :
        in.vbus_mv < 0 || in.cap_mv < 5000 ? Reason::NoMonitor : Reason::None;
    if (changed != Reason::None) { stopping_ = true; reason_ = changed; skip_pending(changed); }
    // Offers that changed or PD that showed up end the unit's rows, not the run.
    else if (!stopping_) {
        const Reason unit_stop = offers_changed ? Reason::CapsChanged :
            legacy_pending && pd_active && !restart_wait_ && !own_rel_ && !race_wait_ && !(restart_dark_ && reann_ == 2) ? Reason::PdActive : Reason::None;
        if (unit_stop != Reason::None) skip_unit(unit_stop);
    }
    if ((restart_wait_ || own_rel_ || race_wait_) && !stopping_) { stable_ = false; return; }
    // The next row is a PD row and the source has not announced its list again (it just came back from a cut or a Hard Reset):
    // a request now is refused ("no source caps") and would fail the row. Wait; offers_changed ends it after kCapsReturnMs.
    bool wait_caps = false;
    if (!stopping_ && mode_ == Mode::Pd) {
        const size_t i = pick();
        const bool pd_next = i != SIZE_MAX && steps_[i].group == Group::Pd && unit_of(steps_[i]) == unit_;
        wait_caps = !caps.valid && pd_next;
        if (pd_next && (steps_[i].algorithm == Algorithm::Epr || steps_[i].algorithm == Algorithm::EprAvs) &&
            !in.pd.epr_mode && in.now < settle_until_) wait_caps = true;
        if (pd_next && is_mi_chain(steps_[i]) && mi_chain_wait(in)) wait_caps = true;
        if (pd_next && tail_wait) wait_caps = true;
    }
    const bool released = (!pd_owned_ || pd_released(in.pd)) &&
                          (!fc_owned_ || in.fc_state == fc::State::Idle) &&
                          (!ceiling_pending_ || in.lines_free);
    const uint32_t need = recover_ ? std::max<uint32_t>(100u, settle_ms_) : (hot_ || pd_owned_) ? 500u : 0u;
    if (stable_tick(safe5(in.vbus_mv) && released && !in.inline_load && in.cap_mv >= 5000, in.now, gap, need) && !wait_caps) {
        if (cut_wait_) cut_resolve(in);
        end_sag_recovery(in);
        pd_owned_ = fc_owned_ = recover_ = false; cut_index_ = SIZE_MAX; settle_ms_ = 0;
        if (stopping_) stop_run(State::Stopped);
        else next(in);
    }
    if (state_ == State::Releasing && !in.pd.verifying && !(wait_caps && safe5(in.vbus_mv)) && !dip_wait(in) &&
        in.now - phase_ms_ >= (recover_ ? kRecoverMs : 8000)) {
        if (cut_wait_) cut_resolve(in);
        if (recover_ && in.vbus_mv >= 0 && in.vbus_mv < 1000 && cut_index_ < count_) {
            auto& cut = steps_[cut_index_];
            cut.verdict = Verdict::Fail; cut.reason = Reason::ShutOff; cut.measured_mv = 0; env_.record(cut);
            skip_pending(Reason::ShutOff); stop_run(State::Done); cut_index_ = SIZE_MAX;
        } else if (in.vbus_mv >= 0 && in.vbus_mv < kUnsafeLowMv) {
            stop_run(State::Stopped, Reason::Lost, true);
        } else if (safe5(in.vbus_mv) && !in.inline_load && in.cap_mv >= 5000) {
            end_sag_recovery(in);
            pd_owned_ = fc_owned_ = recover_ = false;
            if (stopping_) stop_run(State::Stopped); else next(in);
        } else if (fc_owned_ && !pd_owned_ && !selfpwr_ && mode_ == Mode::Legacy && !rel_rd_ && !in.inline_load && in.vbus_mv >= 0 && !safe5(in.vbus_mv)) {
            K2LOG("[CT] release left VBUS at %dmV: Rd off\n", int(in.vbus_mv));
            rel_rd_ = true; ending_ = true; begin_detach(in);
        } else unsafe(Reason::Release);
    }
}

[[gnu::noinline]] void Runner::poll_baseline(Input& in, const pd::SourceCaps& caps, uint32_t gap, bool pd_active) {
    if (epr_stage_ == 2 && link_resets(in.pd) != resets_) {
        resets_ = link_resets(in.pd); pd_owned_ = false; stable_ = false; phase_ms_ = in.now;
        if (epr_tries_ < kEprEntryRetries) { pre_ms_ = kEprSettleMs + kEprSettleStepMs * epr_tries_; ++epr_tries_; epr_stage_ = 1; }
        else { pre_ms_ = kEprSettleMs; epr_refused(caps, in); }
        return;
    }
    if (epr_stage_ < 2 && epr_tries_ && !safe5(in.vbus_mv) && in.vbus_mv >= 1000 && in.now - phase_ms_ < kRecoverMs) {
        stable_ = false; return;
    }
    if (dip_wait(in)) { stable_ = false; return; }
    if (!safe5(in.vbus_mv)) { unsafe(Reason::Release); return; }
    if (mode_ == Mode::Pd && epr_stage_ == 0) regrow_plan(caps, in);
    if (!stable_tick(true, in.now, gap, 500 + pre_ms_)) return;
    pre_ms_ = 0;
    if (probe_ == Probe::Closing) {
        if (in.pd.state == pd::SinkState::Ready) {
            probe_ = env_.probe_identity(true) ? Probe::Running : Probe::Off;
            probe_ms_ = in.now;
        } else if (in.now - phase_ms_ < kProbeMaxMs) return;
        else probe_ = Probe::Off;
    }
    if (probe_ == Probe::Running) {
        if (!in.probe_done && in.now - probe_ms_ < kProbeMaxMs) return;
        end_probe();
    }
    if (epr_stage_ == 4) {
        if (in.pd.state != pd::SinkState::Ready) {
            if (in.now - phase_ms_ < kEprReadyMs) return;
            epr_stage_ = 0;
        } else if (!ask_info(in)) return;
        else { epr_stage_ = 0; epr_info_rows(caps, in); }
    }
    if (epr_stage_ == 1) {
        if (in.pd.state != pd::SinkState::Ready) {
            if (in.now - phase_ms_ < kEprReadyMs) return;
            epr_stage_ = 0; epr_skips(Reason::NoContract);
        } else if (!epr_cable(in) && !ask_info(in)) {
            return;
        } else {
            const auto err = env_.request_pd({0, 0, 0});
            if (err == pd::Err::Ok) { epr_stage_ = 2; epr_ms_ = in.now; pd_owned_ = true; resets_ = link_resets(in.pd); return; }
            epr_refused(caps, in);
        }
    }
    if (epr_stage_ == 2) {
        if (caps.epr && !caps.epr_partial && in.pd.epr_mode && in.pd.state == pd::SinkState::Ready &&
            in.pd.result == pd::SinkResult::Ok && !in.pd.held) {
            epr_stage_ = 0; epr_proof_ = in.epr_proof = true;
            replan_pd(caps, in, true);
        } else if (in.pd.state == pd::SinkState::Failed || in.now - epr_ms_ >= 8000) {
            epr_refused(caps, in);
            release(in, false); return;
        } else return;
    }
    if (attached_ && pd_active) skip_unit(Reason::PdActive);
    next(in);
}

[[gnu::noinline]] void Runner::poll_active(Input& in, uint32_t gap) {
    const auto& s = steps_[index_];
    if (link_resets(in.pd) != resets_) {
        if (s.group == Group::Pd && can_retry(s)) { retry_pd_row(in); return; }
        fail_and_recover(in, Verdict::Fail, Reason::HardReset, 0, true);
        return;
    }
    const bool ready = s.group == Group::Pd ?
        in.pd.state == pd::SinkState::Ready && in.pd.result == pd::SinkResult::Ok && in.pd.pos == s.pos &&
        in.pd.mv == (is_step_walk(s) ? walk_mv_ : is_sweep(s) ? sweep_mv_ : s.mv) :
        in.fc_state == fc::State::Active;
    Step eff = s;
    if (is_step_walk(s)) eff.mv = walk_mv_;
    if (is_sweep(s)) eff.mv = sweep_mv_;
    if (s.group != Group::Pd && in.fc_state == fc::State::Failed && (in.fc_fail == fc::Fail::Detected || in.fc_fail == fc::Fail::NoVoltage)) {
        finish(in, Verdict::Pass, Reason::None, static_cast<uint8_t>(in.fc_fail));
        return;
    }
    if ((s.group == Group::Pd && (in.pd.state == pd::SinkState::Failed ||
        (in.pd.state == pd::SinkState::Ready && in.pd.result != pd::SinkResult::None && in.pd.result != pd::SinkResult::Ok))) ||
        (s.group != Group::Pd && in.fc_state == fc::State::Failed)) {
        if (s.group == Group::Qc3Plus && in.fc_fail == fc::Fail::NoResponse) {
            finish(in, Verdict::Skip, Reason::NoQc3Plus, static_cast<uint8_t>(in.fc_fail));
            return;
        }
        if (finish_class_b(in, s, static_cast<uint8_t>(in.fc_fail))) return;
        const auto group = s.group;
        const bool unanswered = never_answered(group, in.fc_fail);
        bool heard = false;
        for (size_t n = 0; n < index_; ++n) heard |= steps_[n].group == group && steps_[n].verdict == Verdict::Pass;
        const bool no_answer = silent_vooc(group, in.fc_fail) || (unanswered && !heard);
        finish(in, no_answer ? Verdict::Skip : Verdict::Fail, Reason::Request, group == Group::Pd ? static_cast<uint8_t>(in.pd.result) : static_cast<uint8_t>(in.fc_fail));
        if (cut_wait_ || race_wait_) return;
        const bool scp_ignores_raise = group == Group::Scp && in.fc_fail == fc::Fail::NoChange && s.mv > 5000;
        const bool dead_unit = unanswered && !(heard && in.fc_fail == fc::Fail::NoHandshake);
        if (dead_unit || scp_ignores_raise) skip_group_rows(group, !dead_unit, static_cast<uint8_t>(in.fc_fail));
        return;
    }
    const bool banded = band_ok(eff, in, gap);
    if (state_ == State::Requesting) {
        if (busy_retry_at_ && in.now >= busy_retry_at_) {
            if (!list_settled(in.now)) busy_retry_at_ = list_ms_ + kListSettleMs;
            else {
                busy_retry_at_ = 0;
                send_pd_request(in, s, false);
            }
        }
        if (ready && is_sweep(s)) walk_steady_.add(in.now, in.vbus_mv);
        if (stable_tick(ready && (banded || is_sweep(s)), in.now, gap, is_sweep(s) ? kSweepHoldMs : 500u) ||
            (ready && is_sweep(s) && sweep_settled(in))) {
            if (is_step_walk(s)) { walk_step(in, eff); return; }
            if (is_sweep(s)) { sweep_point(in, eff); return; }
            state_ = State::Holding; phase_ms_ = stable_ms_;
            prog_on_ = prog_wanted(s) && env_.prog_begin(s);
        }
        track_plateau(in, ready, s.group == Group::Pd && in.pd.held && in.pd.pos == s.pos && in.pd.mv == eff.mv &&
                      (in.pd.state == pd::SinkState::Requesting || in.pd.state == pd::SinkState::Transition), gap);
        if (is_class_b_peak(s) && in.vbus_mv >= 0) {
            peak_lo_mv_ = std::min<uint16_t>(peak_lo_mv_, static_cast<uint16_t>(std::min(in.vbus_mv, 65535)));
            peak_hi_mv_ = std::max<uint16_t>(peak_hi_mv_, static_cast<uint16_t>(std::min(in.vbus_mv, 65535)));
        }
        if (state_ == State::Requesting && ready && !banded && !is_sweep(s) && plat_mv_ && in.now - plat_ms_ >= kCappedPlateauMs &&
            capped(eff, in, ready)) {
            finish(in, Verdict::Fail, Reason::Capped);
            return;
        }
        const uint32_t timeout = s.group == Group::Qc2 ? fc::kQc2TransitionMs + 3000 : 12000;
        const bool stuck_at_12v = is_class_b_peak(s) && in.now - phase_ms_ >= kClassAStuckMs &&
                                  peak_lo_mv_ >= kClassAStuckLoMv && peak_hi_mv_ <= kClassAStuckHiMv;
        if (state_ == State::Requesting && (in.now - phase_ms_ >= timeout || stuck_at_12v)) {
            if (!finish_class_b(in, s, 0)) {
                finish(in, Verdict::Fail, capped(eff, in, ready) ? Reason::Capped :
                                  floored(eff, in, ready) ? Reason::Floored : Reason::Timeout);
            }
        }
    } else {
        const bool renewing = s.group == Group::Pd && in.pd.held && in.pd.pos == s.pos && in.pd.mv == s.mv &&
            (in.pd.state == pd::SinkState::Requesting || in.pd.state == pd::SinkState::Transition);
        if ((!ready && !renewing) || !banded) { finish(in, Verdict::Fail, Reason::Voltage); return; }
        if (gap > 250) phase_ms_ = in.now;
        if (in.now - phase_ms_ >= hold_ms()) {
            if (prog_on_ && prog_from_mv_ && prog_from_mv_ != s.mv && spec_feed(in, s, prog_from_mv_, s.mv, pdspec::kSlam, 0)) return;
            const uint8_t found = prog_on_ ? env_.prog_take(s) : uint8_t(0);
            finish(in, Verdict::Pass, found ? Reason::ProgCheck : Reason::None, found);
        }
    }
}

void Runner::poll_impl(Input& in, const pd::SourceCaps& caps) {
    if (!busy()) return;
    in.epr_proof = epr_proof_;
    if (in.pd.xiaomi_svid) xiaomi_seen_ = true;
    if (in.lines != Lines::Unknown) evidence_.dpdm = in.lines;   // the check lands on its own, often after the first D+/D- unit
    if (in.pd.oppo_svid) evidence_.seen |= SeenOppoSvid;   // latched: a Hard Reset in the run clears the sink's own flag
    if (link_resets(in.pd) != hr_count_) { hr_count_ = link_resets(in.pd); xiaomi_reset_ = xiaomi_seen_; }
    if (in.pd.xiaomi_authed) xiaomi_reset_ = false;   // the chain ran again: the list is the real one
    if (link_resets(in.pd) == resets_) { x_svid_ = in.pd.xiaomi_svid; x_authed_ = in.pd.xiaomi_authed; }
    // Held (the page is still arming the journal): the run has not begun, so nothing in the Baseline is judged and its clocks
    // restart at every poll, with the link as it is now. The PD engine keeps serving the source meanwhile.
    if (held_ && state_ == State::Baseline) {
        last_ms_ = phase_ms_ = in.now; stable_ = false; session_ = in.session; resets_ = link_resets(in.pd);
        return;
    }
    const uint32_t gap = in.now - last_ms_; last_ms_ = in.now;
    track_cut(in);
    track_dip(in);
    // Restart rules (2026-11-09), rule 1: every new list opens a settle window - no Request until the list in hand has
    // been unchanged for kListSettleMs, and each list arriving inside the window restarts it. Attach/Detach settle the
    // list on their own (poll_attach waits for caps + contract), so the window is tracked only in the run states; the
    // first run-state poll after start()/restore() seeds it (the START / resumed list is already settled).
    if (state_ != State::Detach && state_ != State::Attach) {
        caps_now_ = caps;
        if (!list_tracking_) { list_tracking_ = true; list_valid_ = caps.valid; list_sig_ = caps.valid ? caps_sig(caps) : 0; list_ms_ = 0; }
        else if (caps.valid && !caps.epr_partial && epr_stage_ != 2 &&
                 (!list_valid_ || caps_sig(caps) != list_sig_)) { list_ms_ = in.now; list_sig_ = caps_sig(caps); }
        list_valid_ = caps.valid;
    }
    // The sink refuses a release while Requesting/Transition/Aborting: ask again until it lets go.
    // Only the sink: an fc release restarts the D+/D- reset hold, so it is issued once (see release()).
    if ((state_ == State::Releasing || state_ == State::Unsafe) && in.now - release_ms_ >= 250) {
        release_ms_ = in.now; if (pd_owned_) env_.release(true);
    }
    if (in.vbus_mv >= 0 && !safe5(in.vbus_mv)) hot_ = true;   // VBUS left vSafe5V: the return needs a settle time
    // A self-powered K2 whose VBUS is gone is dying (or about to be reset): that is not the charger's verdict and not the end of
    // the run. Every unfinished row stays Pending, the journal stays open, the next steady VBUS resumes it.
    if (selfpwr_ && in.vbus_mv >= 0 && in.vbus_mv < 1000 && state_ != State::Detach && state_ != State::Attach && unfinished()) {
        suspend(); return;
    }
    if (state_ != State::Baseline) adopt_ = false;
    else if (adopt_ && in.session != session_ && in.vbus_mv >= 1000) session_ = in.session;
    if (state_ == State::Unsafe) {
        if (in.vbus_mv >= 0 && in.vbus_mv < kUnsafeLowMv) {
            if (!gone_ || gap > 250) { gone_ = true; stable_ms_ = in.now; }
            stable_ = false;
            if (in.now - stable_ms_ >= kUnsafeGoneMs) stop_run(State::Stopped);
            return;
        }
        gone_ = false;
        if (stable_tick(safe5(in.vbus_mv) && !in.inline_load && pd_released(in.pd) && in.fc_state == fc::State::Idle, in.now, gap, 500))
            stop_run(State::Stopped);
        return;
    }
    if (state_ == State::Detach || state_ == State::Attach) {
        const Reason fault = base_fault(in);
        if (fault != Reason::None) { stop_run(State::Done, fault, true); return; }
        if (state_ == State::Detach) poll_detach(in); else poll_attach(in, caps, gap);
        return;
    }
    if (state_ == State::Ceiling) { poll_ceiling(in); return; }
    if (state_ == State::Spec) { poll_spec(in); return; }
    bool offers_changed = false, legacy_pending = false, tail_seen = false, tail_wait = false;
    const bool dark = in.vbus_mv >= 0 && in.vbus_mv < 1000;
    if (caps.valid || dark) caps_gone_ms_ = in.now; else if (!caps_gone_ms_) caps_gone_ms_ = in.now;
    if ((late_pd_ || (epr_stage_ == 3 && unit_ == Unit::Pd)) && attached_ && mode_ == Mode::Pd && state_ == State::Baseline &&
        ((caps.valid && list_settled(in.now)) || (!caps.valid && !dark && in.now - caps_gone_ms_ >= kCapsReturnMs))) {
        if (late_pd_) replan_pd(caps, in, true);
        if (epr_stage_ == 3 && unit_ == Unit::Pd) decide_epr_stage(caps, in);
    }
    follow_list(caps, in);   // before the check below: a list that still offers every row is not "changed"
    for (const auto& s : steps()) {
        if ((s.verdict != Verdict::Pending && s.verdict != Verdict::Running) || unit_of(s) != unit_) continue;   // a unit watches its own rows
        if (s.group != Group::Pd) { legacy_pending = true; continue; }
        if (is_mi_chain(s)) continue;   // no offer of its own: the chain row reads whatever list the source holds
        if (!caps.valid && (dark || in.now - caps_gone_ms_ < kCapsReturnMs)) continue;
        if (caps.valid && !caps.epr && s.pos >= 8 && epr_capable_caps(caps) && caps.spec_rev == evidence_.wire_rev) continue;
        if (caps.valid && caps.spec_rev == evidence_.wire_rev && s.pos > caps.num_pdos) {   // the list is cut short above this row
            tail_seen = true;
            if (preauth_tail(in, tail_wait)) continue;
        }
        const bool covered = !caps.valid ? false : s.pos >= 8
            ? (caps.epr ? find_offer(caps, s) >= 0 : epr_capable_caps(caps))
            : find_offer(caps, s) >= 0;
        if (!caps.valid || caps.spec_rev != evidence_.wire_rev || !s.pos || !covered)
            offers_changed = true;
    }
    if (!tail_seen) tail_at_ = 0;
    if (epr_stage_ == 2) offers_changed = false;
    if (caps.valid && caps.epr && caps.epr_partial) offers_changed = false;
    if (flight_restart_) {
        if (index_ >= count_ || steps_[index_].verdict != Verdict::Running || steps_[index_].group != Group::Pd ||
            (state_ == State::Requesting && busy_retry_at_)) {
            flight_restart_ = false;
        } else {
            const Step& fs = steps_[index_];
            if (in.pd.held && in.pd.pos == fs.pos && in.pd.mv == fs.mv && in.pd.state == pd::SinkState::Ready) {
                flight_restart_ = false;
            } else if (list_settled(in.now) && in.pd.state != pd::SinkState::Requesting &&
                       in.pd.state != pd::SinkState::Transition) {
                flight_restart_ = false;
                send_pd_request(in, fs, true);
            }
        }
    }
    const bool pd_active = mode_ == Mode::Legacy &&
        (silent_ ? in.pd_quiet_ms < quiet_ms() : (caps.valid && caps.active_cc) || in.pd_traffic);
    if (state_ == State::Releasing) {
        poll_releasing(in, caps, gap, offers_changed, legacy_pending, pd_active, tail_wait);
        return;
    }
    const bool baseline = state_ == State::Baseline;
    if (!baseline && (state_ == State::Requesting || state_ == State::Holding) && index_ < count_ && steps_[index_].group == Group::Pd)
        latch_row(in);
    const bool legacy_row = !baseline && steps_[index_].group != Group::Pd && !pd_owned_;
    const int level_mv = legacy_row && steps_[index_].mv ? int(steps_[index_].mv) : 5000;
    if (legacy_row && in.session != session_ && in.vbus_mv >= level_mv * 85 / 100) { session_ = in.session; sag_cut_ = true; }
    if (sag_open_ && in.now - sag_ms_ >= kSagAdoptMs) sag_open_ = false;
    if (baseline && mode_ == Mode::Legacy && !sag_open_ && in.vbus_mv >= 0 && in.vbus_mv < 5500 && !safe5(in.vbus_mv) &&
        in.now - phase_ms_ < kRecoverMs) {
        if (!unit_row_ran() && !legacy_row_cut()) { sag_open_ = own_cut_ = true; sag_ms_ = in.now; }
    }
    const bool sag_wait = (legacy_row && (in.session != session_ || in.vbus_mv < 1000)) ||
                          (baseline && mode_ == Mode::Legacy && (in.session != session_ || !safe5(in.vbus_mv)));
    if (sag_open_ && sag_wait && !in.inline_load && in.cap_mv >= 5000) {
        if (safe5(in.vbus_mv)) session_ = in.session;
        else { stable_ = false; return; }
    }
    if (baseline && in.session != session_ && dip_adopts(in)) session_ = in.session;
    const Reason core = base_fault_core(in);
    const Reason fault = core != Reason::None ? core :
        !baseline && in.ibus_ma > kLoadAbortMa ? Reason::Inline :
        in.session != session_ || in.vbus_mv < 1000 ? Reason::Lost : in.fake ? Reason::Fake :
        in.vbus_mv > int(pd::overcap_abort_mv(in.cap_mv)) ? Reason::OverCap :
        in.vbus_mv > (baseline ? cable_mv(in) : measured_cable_limit_mv(in, steps_[index_].algorithm)) ? Reason::CableLimit :
        !baseline && ((steps_[index_].algorithm == Algorithm::Epr || steps_[index_].algorithm == Algorithm::EprAvs) && !epr_cable(in)) ? Reason::CableLimit :
        !baseline && steps_[index_].group != Group::Pd && steps_[index_].group != Group::Ufcs &&
            pd_active ? Reason::PdActive : offers_changed ? Reason::CapsChanged : Reason::None;
    if (fault != Reason::None) {
        if (fault == Reason::Lost && baseline && mode_ == Mode::Legacy && !selfpwr_ && in.vbus_mv >= 0 && in.vbus_mv < 1000 &&
            !unit_row_ran() && (own_cut_ || !legacy_row_ran())) {
            if (legacy_row_ran()) skip_unit(Reason::Lost); else skip_legacy_units(Reason::PdOnly);
            stable_ = false; ending_ = true; begin_detach(in);
            return;
        }
        if (fault == Reason::Lost && !baseline && steps_[index_].group != Group::Pd && !pd_owned_) {
            fail_and_recover(in, Verdict::Fail, fault, 0, false);
            return;
        }
        if (fault == Reason::Lost && (state_ == State::Requesting || state_ == State::Holding) &&
            steps_[index_].group == Group::Pd) {
            const auto& row = steps_[index_];
            const bool cut = low_programmable(row) && in.vbus_mv < int(row.mv) * 85 / 100;
            if (can_retry(row) && !cut) { retry_pd_row(in); return; }
            const bool nl = !cut && needs_load(in);
            fail_and_recover(in, nl ? Verdict::Skip : Verdict::Fail, nl ? Reason::NeedsLoad : cut ? Reason::CutRecovered : fault, 0, true);
            return;
        }
        if (fault == Reason::CableLimit && !baseline) {
            auto& s = steps_[index_];
            s.verdict = Verdict::Skip; s.reason = fault;
            s.measured_mv = static_cast<uint16_t>(std::clamp(in.vbus_mv, 0, 65535));
            env_.record(s);
            release(in, false);
            return;
        }
        if (fault == Reason::PdActive || fault == Reason::CapsChanged) {
            skip_unit(fault);
            if (!baseline) finish(in, Verdict::Fail, fault); else release(in, false);
            return;
        }
        reason_ = fault;
        if (!baseline) { finish(in, Verdict::Fail, fault); stopping_ = true; skip_pending(fault); }
        else release(in, true);
        return;
    }
    if (baseline) { poll_baseline(in, caps, gap, pd_active); return; }
    poll_active(in, gap);
}
}  // namespace k2::charger_test
