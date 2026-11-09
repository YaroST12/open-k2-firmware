// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/charger_test.hpp"
#include "k2/fastcharge/accp.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/fastcharge/qc.hpp"
#include <algorithm>
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
        "PD2.0 fixed", "PD3.x fixed", "PD3.1+ EPR", "PD3.2+ SPR AVS", "PPS (PD3.x)",
        "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC 5V handshake", "SVOOC 5V handshake",
        "SVOOC65 5V handshake", "UFCS", "VIVO", "PD3.0 revision", "PD3.1 revision", "PD3.2 revision", "EPR AVS", "QC2 Class B", "QC3 Class B"
    };
    static_assert(std::size(names) == unsigned(Algorithm::Count));
    return a < Algorithm::Count ? names[unsigned(a)] : "?";
}
const char* name(Group g) {
    static constexpr const char* names[] = {"PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", "SVOOC", "SVOOC65", "UFCS", "VIVO"};
    return g < Group::Count ? names[static_cast<unsigned>(g)] : "?";
}
const char* name(Verdict v) {
    static constexpr const char* names[] = {"WAIT", "RUN", "PASS", "FAIL", "SKIP"};
    return names[static_cast<unsigned>(v)];
}
const char* name(Reason r) {
    static constexpr const char* names[] = {
        "", "disabled", "not advertised", "over voltage cap", "not implemented", "confirm no load",
        "no power monitor", "no source", "load attached", "tool busy", "fake offers",
        "request failed", "timeout", "voltage outside band", "hard reset", "source lost",
        "cancelled", "5V release failed", "offers changed", "PD active: use QC port",
        "revision cannot be forced", "select at least one test", "physical cable limit", "unsupported PDO layout",
        "charger stayed in PD", "rebooted during request", "rebooted during handover", "EPR entry refused",
        "no contract", "source shut off (needs replug)", "no PD after re-attach", "VBUS cut, source recovered",
        "blocked by PD", "no QC3+ response", "rebooted 3x", "needs external power", "PD gave up before START",
        "Class A only (12 V max)", "not unlocked", "20 V via QC3 only", "PD only (output off)", "capped below target"
    };
    static_assert(std::size(names) == unsigned(Reason::Capped) + 1);
    return names[static_cast<unsigned>(r)];
}
const char* name(State s) {
    static constexpr const char* names[] = {"Setup", "Verify 5V", "Negotiating", "Hold", "Return to 5V", "Complete", "Stopped", "UNSAFE: unplug source", "Detach charger", "Initializing..."};
    return names[static_cast<unsigned>(s)];
}
const char* name(Unit u) {
    static constexpr const char* names[] = {"PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", "SVOOC", "SVOOC65", "UFCS", "VIVO", "PD low"};
    static_assert(std::size(names) == unsigned(Unit::Count));
    return u < Unit::Count ? names[static_cast<unsigned>(u)] : "?";
}
const char* name(Mode m) { return m == Mode::Pd ? "PD" : "legacy"; }
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
    case kDetailEprFail + 1: return "EPR: source rejects cable";
    case kDetailEprFail + 2: return "EPR: source has no VCONN";
    case kDetailEprFail + 3: return "EPR: refused (RDO)";
    case kDetailEprFail + 4: return "EPR: source unable now";
    case kDetailEprFail + 5: return "EPR: 5V PDO lacks EPR bit";
    case kDetailEprFail: return "EPR: entry refused";
    case kDetailMonitor36: return "EPR: above K2's 36V monitor";
    case kDetailMonitor48: return "EPR: above K2's 48V monitor";
    case kDetailMonitorOther: return "EPR: above the K2's monitor";
    case kDetailEprOffered: return "EPR offered: needs EPR cable";
    case kDetailEprWithheld: return "EPR list withheld: cable-gated";
    case kDetailEprOfferedRefused: return "EPR offered; entry: refused";
    case kDetailEprOfferedRefused + 1: return "EPR offered; entry: cable rejected";
    case kDetailEprOfferedRefused + 2: return "EPR offered; entry: no VCONN";
    case kDetailEprOfferedRefused + 3: return "EPR offered; entry: refused (RDO)";
    case kDetailEprOfferedRefused + 4: return "EPR offered; entry: unable now";
    case kDetailEprOfferedRefused + 5: return "EPR offered; entry: 5V PDO lacks EPR bit";
    case kDetailNoChain: return "not unlocked (no OEM chain)";
    case kDetailChainUnfinished: return "not unlocked (chain unfinished)";
    case kDetailCableCap: return "not unlocked (3 A cable)";
    case kDetailVendorRefused: return "not unlocked (vendor not accepted)";
    case kDetailSourceReset: return "not unlocked (source reset)";
    default: return nullptr;
    }
}
const char* explanation(const Step& s) {
    if (s.reason == Reason::RevisionUntestable) {
        if (s.algorithm == Algorithm::Pd30Revision) return "No independent PD3.0 test";
        if (s.algorithm == Algorithm::Pd31Revision) return "No independent PD3.1 test";
        if (s.algorithm == Algorithm::Pd32Revision) return "No independent PD3.2 test";
    }
    const bool epr_row = s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs;
    // Every EPR skip names its cause (results page, card): the row's `detail` says which monitor ceiling (OverCap) or which
    // Enter_Failed data the source sent (EprRefused), see kDetailEprFail.
    if (epr_row && s.reason == Reason::OverCap) return epr_detail_name(s.detail) ? epr_detail_name(s.detail) : "EPR: above the K2's monitor";
    if (s.reason == Reason::NoOffer && s.pos) return "No allowed current at target";
    if (epr_row && s.reason == Reason::CableLimit)
        return epr_detail_name(s.detail) && (s.detail == kDetailEprOffered || s.detail == kDetailEprWithheld || s.detail >= kDetailEprOfferedRefused)
                   ? epr_detail_name(s.detail) : "EPR: cable not 50V 5A";
    if (epr_row && s.reason == Reason::NoOffer) return "EPR: not offered";
    if (s.reason == Reason::EprRefused) return epr_detail_name(s.detail) ? epr_detail_name(s.detail) : "EPR: entry refused";
    if (s.reason == Reason::Capped) return "output capped";   // the Detail value column is 16 cells; name() is the long form
    if (s.reason == Reason::NoContract) return "EPR: no PD contract";
    if (s.reason == Reason::NotUnlocked) return epr_detail_name(s.detail) ? epr_detail_name(s.detail) : name(s.reason);
    if (s.reason == Reason::NoOffer && s.algorithm == Algorithm::SprAvs) return "SPR AVS not advertised";
    if (s.reason == Reason::NoOffer && s.algorithm == Algorithm::Pps) return "PPS not advertised";
    if (s.reason == Reason::UnsupportedOffer && s.avs)
        return s.pos < 8 ? "EPR AVS in an SPR slot" : "SPR APDO in an EPR slot";
    return name(s.reason);
}
const char* name(HuntCause c) {
    switch (c) {
    case HuntCause::None: return "none";
    case HuntCause::NoChain: return "no-chain";
    case HuntCause::ChainStalled: return "chain-stalled";
    case HuntCause::InsideChain: return "inside-chain";
    case HuntCause::RequestHeld: return "request-held";
    case HuntCause::FixedPdo: return "fixed-pdo";
    case HuntCause::CableCap: return "cable-cap";
    case HuntCause::ApdoStale: return "apdo-stale";
    case HuntCause::CableCapped5A: return "capped-5a";
    case HuntCause::RefusedDespiteOffer: return "refused-offer";
    case HuntCause::Unexplained: return "unexplained";
    case HuntCause::Count: break;
    }
    return "?";
}
const char* explanation(HuntCause c) {
    switch (c) {
    case HuntCause::None: return "not refused";
    case HuntCause::NoChain: return "no OEM chain listed";
    case HuntCause::ChainStalled: return "OEM chain stopped early";
    case HuntCause::InsideChain: return "asked inside the chain";
    case HuntCause::RequestHeld: return "request held back";
    case HuntCause::FixedPdo: return "fixed PDO never offers 6 A";
    case HuntCause::CableCap: return "cable under 5 A";
    case HuntCause::ApdoStale: return "no caps after the auth";
    case HuntCause::CableCapped5A: return "APDO still 5 A after auth";
    case HuntCause::RefusedDespiteOffer: return "refused an offered 6 A";
    case HuntCause::Unexplained: return "evidence incomplete";
    case HuntCause::Count: break;
    }
    return "?";
}
// Order matters: each cause is only reached when every one above it is ruled out (the table in docs/CHARGER_TEST_FLOW.md).
HuntCause classify_hunt(const HuntEvidence& e) {
    const auto has = [&e](unsigned f) { return (e.flags & f) != 0; };
    if (!has(HfSvid)) return HuntCause::NoChain;
    if (has(HfSent) && has(HfInChain)) return HuntCause::InsideChain;
    if (has(HfHeld) && !has(HfSent)) return HuntCause::RequestHeld;
    if (!has(HfAuthed)) return HuntCause::ChainStalled;
    if (!has(HfSent)) return HuntCause::Unexplained;
    if (e.type == static_cast<uint8_t>(pd::PdoType::Fixed)) return HuntCause::FixedPdo;
    if (e.apdo_ma >= e.req_ma) return HuntCause::RefusedDespiteOffer;
    if (!has(HfReannounced)) return HuntCause::ApdoStale;
    return e.cable_ma >= 5000 ? HuntCause::CableCapped5A : HuntCause::CableCap;
}
namespace {
constexpr uint32_t kHoldMs = 1000;
// "A Hard Reset happened on the link", whoever sent it: the sink counts the ones the K2 sent (hard_resets) and the ones the source
// sent (source_hard_resets) apart, and the runner only ever compares this sum with a value it took from the same sum.
uint32_t link_resets(const pd::SinkStatus& s) { return s.hard_resets + s.source_hard_resets; }
uint16_t sat16(uint32_t v) { return static_cast<uint16_t>(std::min<uint32_t>(v, 65535)); }

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
bool epr_proven(Input in) { return !in.cable_observed && (in.pd.epr_mode || in.epr_proof); }
uint16_t cable_mv(Input in) { return epr_proven(in) ? 50000 : std::min<unsigned>(in.cable_mv, in.cable_observed ? 50000 : 20000); }
constexpr unsigned kScpLowMa = 2250;   // Huawei SCP 10 V 2.25 A (22.5 W)
constexpr unsigned kUfcs60wMa = 3000;   // UFCS 20 V row: 20 V 3 A = 60 W
constexpr int kUfcsLevelMa = 1000;      // the levels planned from Output_Capabilities ask 1 A (or the mode's smallest current)
uint16_t cable_ma(Input in) { return epr_proven(in) ? 5000 : std::min<unsigned>(in.cable_ma, in.cable_observed ? 5000 : 3000); }
// PDO1 of the SPR list carries "EPR Mode Capable" (bit 23): the source offers EPR PDOs only after EPR_Mode entry.
bool epr_capable_caps(const pd::SourceCaps& c) {
    return c.valid && c.num_pdos && c.pdos[0].type == pd::PdoType::Fixed && (c.pdos[0].raw & (1u << 23));
}
// EPR rows need a 50 V / 5 A cable: one that was read as such, or one the source accepted EPR entry over (epr_proven).
bool epr_cable(Input in) { return (in.cable_observed || epr_proven(in)) && cable_mv(in) == 50000 && cable_ma(in) == 5000; }
// The cable limit a hunt row is judged against and the evidence records. Nothing physical read (and no EPR proof): a spoofed identity
// is the cable the source saw, so its VDO limits count; only a cable nobody presented falls back to the 20 V / 3 A default.
bool spoof_cable(Input in) { return !in.cable_observed && !epr_proven(in) && in.spoof_on && in.spoof_ma != 0; }
uint16_t used_cable_mv(Input in) { return spoof_cable(in) ? std::min<uint16_t>(in.spoof_mv ? in.spoof_mv : 20000, 50000) : cable_mv(in); }
uint16_t used_cable_ma(Input in) { return spoof_cable(in) ? std::min<uint16_t>(in.spoof_ma, 5000) : cable_ma(in); }
// What the cable is vouched for above 5 A (0 = nothing): a cable VDO tops out at 5 A, so only a read cable that says more, or the
// spoofed identity the run presents, lets a row ask beyond it (a PPS APDO of 6.25 A, e.g. Motorola TurboPower).
uint16_t over5_cable_ma(Input in) { return spoof_cable(in) ? in.spoof_ma : in.cable_observed ? in.cable_ma : uint16_t(0); }
// A 6 A hunt row the source refused or ignored: the OEM mode is not unlocked, not refused. The detail says what is missing, from the
// SVID 0x2717 chain first, then the cable (an unread cable counts as 3 A, see cable_ma; a spoofed identity counts as its own VDO says,
// and a 5 A one means the source did not accept it). 0 = nothing explains it: a real failure.
uint8_t not_unlocked_detail(Input in) {
    if (!in.pd.xiaomi_svid) return kDetailNoChain;
    if (!in.pd.xiaomi_authed) return kDetailChainUnfinished;
    if (used_cable_ma(in) < 5000) return kDetailCableCap;
    return spoof_cable(in) ? kDetailVendorRefused : uint8_t(0);
}
// The same for a hunt row the source answered with a Hard Reset. The sink cleared its chain state with the reset, so `in` carries the
// run's own snapshot from before it. The chain links and a cable the K2 read keep their reasons; an unread cable (counted as 3 A only
// by default) and a proven 5 A cable with the chain done explain nothing, so the row says what happened.
uint8_t hunt_reset_detail(Input in) {
    const uint8_t why = not_unlocked_detail(in);
    const bool assumed_cable = !in.cable_observed && !spoof_cable(in);   // nobody read it, nobody presented one: the 3 A is a default
    return (why == kDetailCableCap && assumed_cable) || why == 0 ? kDetailSourceReset : why;
}
// A cable that was read and is not 50 V / 5 A: no EPR entry is attempted. An unread cable does not block the attempt (the START
// skips the SOP' scan while a source's Rp is on the line), the source's own cable check answers it.
bool epr_cable_unfit(Input in) { return in.cable_observed && !epr_cable(in); }
// FNV-1a over the list (valid, count, raw PDO words): tells "the same offer list" from a list that grew or changed.
uint32_t caps_sig(const pd::SourceCaps& c) {
    uint32_t h = 2166136261u;
    const auto mix = [&h](uint32_t v) { for (unsigned n = 0; n < 4; ++n) { h ^= (v >> (8 * n)) & 0xFF; h *= 16777619u; } };
    mix(c.valid); mix(c.num_pdos);
    for (size_t n = 0; c.valid && n < std::min<size_t>(c.num_pdos, c.pdos.size()); ++n) mix(c.pdos[n].raw);
    return h;
}
// Does the new word still cover the row's planned one? Same type and voltage range, current limit only
// grew (CUKTECH C1 after the Xiaomi auth unlock re-announces its PPS APDOs with 5 A -> 6 A, hardware
// 2026-11-04): the planned row is still offered, so the list is not "changed" for that row.
bool pdo_covers(uint32_t now_raw, uint32_t planned_raw) {
    const pd::Pdo now = pd::decode_pdo(now_raw), was = pd::decode_pdo(planned_raw);
    return now.type == was.type && now.type != pd::PdoType::Unknown &&
           now.min_mv == was.min_mv && now.max_mv == was.max_mv && now.max_ma >= was.max_ma;
}
// Where the list in hand offers what a planned row asked for: the row's own slot first, then any slot on the same side of the
// SPR / EPR boundary (a source that announces again after the cable discovery can insert an APDO ahead of the one we planned).
// -1 when nothing offers it any more.
int find_offer(const pd::SourceCaps& caps, const Step& s) {
    if (!caps.valid || !s.pos || !s.pdo) return -1;
    const size_t n = std::min<size_t>(caps.num_pdos, caps.pdos.size());
    const auto offers = [&](size_t i) { return caps.pdos[i].raw == s.pdo || pdo_covers(caps.pdos[i].raw, s.pdo); };
    if (s.pos <= n && offers(s.pos - 1u)) return s.pos - 1;
    const bool epr = s.pos >= 8;
    for (size_t i = epr ? 7 : 0, end = epr ? n : std::min<size_t>(n, 7); i < end; ++i)
        if (offers(i)) return static_cast<int>(i);
    return -1;
}
// Does the list in hand advertise what a 6 A hunt row asks? The row's PDO word is the slot follow_list / adopt_list keep current:
// a known type, the row's voltage inside the slot's range and a current limit of at least the row's (a PPS APDO's max current; a fixed
// PDO only if it really offers 6 A). The row sends its Request only then (decision 2026-11-07, docs/CHARGER_QUIRKS.md).
bool hunt_advertised(const Step& s) {
    if (!s.pdo) return false;
    const pd::Pdo p = pd::decode_pdo(s.pdo);
    return p.type != pd::PdoType::Unknown && s.mv >= p.min_mv && s.mv <= p.max_mv && p.max_ma >= s.ma;
}
uint16_t cable_limit_mv(Input in, Algorithm algorithm) {
    const auto limit = cable_mv(in);
    // SPR PPS permits a 21 V request without promoting the physical cable to EPR.
    return algorithm == Algorithm::Pps && limit == 20000 ? 21000 : limit;
}
uint16_t measured_cable_limit_mv(Input in, Algorithm algorithm) {
    const auto limit = cable_limit_mv(in, algorithm);
    // The 20 V SPR cable class permits the 5% VBUS tolerance on a 20 V target, and on the 21 V PPS ceiling (22.05 V).
    return limit == 20000 ? 21000 : limit == 21000 ? 22050 : limit;
}
Reason limit_reason(unsigned mv, Input in, Algorithm algorithm) {
    return !pd::fits_monitor(mv, in.cap_mv) ? Reason::OverCap : mv > cable_limit_mv(in, algorithm) ? Reason::CableLimit : Reason::None;
}
Reason refusal(Options o, Input in, bool busy, bool confirmed) {
    return busy || in.busy ? Reason::Busy : !(o.selected & kAllAlgorithms) ? Reason::EmptySelection :
        !confirmed ? Reason::NeedConfirm : in.fake ? Reason::Fake : in.inline_load ? Reason::Inline :
        in.vbus_mv < 0 || in.cap_mv < 5000 ? Reason::NoMonitor : cable_mv(in) < 5000 ? Reason::CableLimit :
        in.vbus_mv < 1000 ? Reason::NoSource : Reason::None;
}
bool in_band(const Step& s, int mv) {
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
    if (s.algorithm == Algorithm::Pps || s.algorithm == Algorithm::SprAvs || s.algorithm == Algorithm::EprAvs)
        return mv >= int(s.mv) - std::max(100, int(s.mv) / 20) && mv <= pd::programmable_ceiling_mv(int(s.mv));   // an unloaded source cannot sink down
    if (s.group == Group::Pd) return std::abs(mv - int(s.mv)) <= std::max(100, int(s.mv) / 20);
    return mv * 100 >= int(s.mv) * 85 && mv * 100 <= int(s.mv) * 110;
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
bool qc2_never_handshook(std::span<const Step> steps) {
    bool failed = false;
    for (const auto& x : steps) {
        if (x.algorithm != Algorithm::Qc2) continue;
        if (x.verdict == Verdict::Pass) return false;
        failed |= x.verdict == Verdict::Fail && x.reason == Reason::Request;
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
    if (!in.n) { for (const Unit u : kDefaultUnits) if (unit_selected(u, o)) out.add(u); return out; }
    for (size_t n = 0; n < in.n; ++n) if (!has_unit(out, in.u[n]) && unit_selected(in.u[n], o)) out.add(in.u[n]);
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
// The rows of one unit, in plan order. Pd and PdLow share the PD plan: the low programmable rows (below 5 V) are the PdLow unit.
void plan_unit(Unit unit, const pd::SourceCaps& caps, Options o, Input in, AddRef add) {
    const bool pd_unit = unit == Unit::Pd || unit == Unit::PdLow;
    size_t n_low = 0; uint8_t low_pos = 0;
    bool hunt_11v = false, hunt_20v = false;   // one 6 A probe per target voltage, not per PDO
    // Does a PPS APDO that this plan runs cover `mv`? (The 6 A hunt then belongs to that APDO, not to a fixed PDO.)
    auto pps_covers = [&](unsigned mv) {
        if (!o.contains(Algorithm::Pps) || caps.spec_rev != 2) return false;
        const unsigned pps_cap = std::min<unsigned>(in.cap_mv, cable_limit_mv(in, Algorithm::Pps));
        for (size_t i = 0; i < std::min<size_t>(std::min<size_t>(caps.num_pdos, caps.pdos.size()), 7); ++i) {
            const auto& p = caps.pdos[i];
            if (p.raw && p.type == pd::PdoType::Pps && p.min_mv && p.min_mv <= mv && mv <= std::min<unsigned>(p.max_mv, pps_cap)) return true;
        }
        return false;
    };
    auto emit = [&](const Step& s) {   // the PD plan is generated whole; each of its two units keeps its own rows
        const bool low = low_programmable(s);
        if (unit == Unit::PdLow ? low : !low) add(s);
    };
    for (const auto algorithm : kPlanOrder) {
        if (!o.contains(algorithm)) continue;
        Step s; s.algorithm = algorithm; s.group = group(algorithm);
        if (pd_unit ? s.group != Group::Pd : s.group != static_cast<Group>(unit)) continue;
        const unsigned cap = std::min<unsigned>(in.cap_mv, cable_limit_mv(in, algorithm));
        auto skip = [&](Reason r) {
            s.verdict = Verdict::Skip; s.reason = r;
            if (r == Reason::OverCap && (algorithm == Algorithm::Epr || algorithm == Algorithm::EprAvs)) s.detail = monitor_detail(in.cap_mv);
            emit(s);
        };
        if (algorithm >= Algorithm::Pd30Revision && algorithm <= Algorithm::Pd32Revision) { skip(Reason::RevisionUntestable); continue; }
        if (s.group != Group::Pd) {
            auto level = [&](unsigned mv, bool continuous = false, unsigned ma = 0, bool offered = true) {
                s.mv = static_cast<uint16_t>(mv); s.continuous = continuous; s.ma = static_cast<uint16_t>(ma);
                s.reason = limit_reason(mv, in, algorithm);
                if (s.reason == Reason::None && !offered) s.reason = Reason::NoOffer;   // outside what the charger reported
                s.verdict = s.reason == Reason::None ? Verdict::Pending : Verdict::Skip;
                add(s);
            };
            // pattern qc-redetect-after-reset (power strip USB-A, 2026-11-07): a QC row that opened its own session started 0.5-0.7 s
            // after the previous row's reset of D+/D- and got no handshake, right after the same charger passed the row before it
            // (QC2 12 V after 9 V, the QC3 Class B 5 V after the QC3 unit). So one HVDCP handshake per unit: the first QC2 row (5 V)
            // and the first QC3 row open the session, every later level is a retarget in it (QC2: the D+/D- levels, QC3: pulses), and
            // the opt-in Class B rows of the unit go on in the same session when their Class A rows are in the plan. A Class B row
            // selected alone opens its own. The engine takes 20 V in a session that started as QC2 / QC3 (qc2_levels, clamp_qc3_mv).
            if (algorithm == Algorithm::Qc2ClassB) { level(20000, o.contains(Algorithm::Qc2)); continue; }   // opt-in Class B (20 V) fixed level
            if (algorithm == Algorithm::Qc3ClassB) {   // opt-in Class B ramp 5/12/20/12/5
                const unsigned targets[] = {5000, 12000, 20000, 12000, 5000};
                for (unsigned n = 0; n < 5; ++n) level(targets[n], n != 0 || o.contains(Algorithm::Qc3));
                continue;
            }
            switch (s.group) {
            case Group::Qc2: level(5000); level(9000, true); level(12000, true); break;   // Class B 20 V: opt-in Qc2ClassB
            case Group::Qc3:
            case Group::Qc3Plus: {
                const unsigned targets[] = {5000, 9000, 12000u, 9000, 5000};   // QC3 spec range 3.6-12 V
                for (unsigned n = 0; n < 5; ++n) level(targets[n], n != 0);
                break;
            }
            // AFC 12 V is a retarget in the 9 V session (pattern afc-redetect-after-reset): as a fresh session it started 0.7 s
            // after our own reset, before the charger had redone its D+/D- detection, and got no AFC answer (Baseus 140W,
            // 2026-11-07; the Xiaomi bank's 12 V row the same way). The engine sends the 12 V byte in the live session.
            case Group::Afc: level(9000); level(12000, true); break;
            case Group::Fcp: level(5000); level(9000); level(12000); break;
            // One UFCS session for the whole unit: each level is a new Request in it (8.2.4.2). A fresh session per row made the
            // Baseus FH22 charger cut VBUS on the Exit and the next row's handshake ran into the dark source (2026-11-07).
            // 11 V is the top of the common 5.5-11 V UFCS mode (the Baseus FH22 charger: 11 V 4 A = its 44 W). 12 V last: a level
            // the source does not offer fails its row and skips the rest of the session, so the step back down to 5 V is
            // proven first. 20 V asks 3 A: the 60 W mode (5-20 V or 11-21 V at 3 A, e.g. the CUKTECH 10 charger's C1); the
            // Request names the current, no load draws it. A source without a mode up to 20 V fails 12 V and skips it.
            // With the charger's own Output_Capabilities (fc_caps, known only once the unit's first row read them) the levels are the
            // bottom, middle and top of each mode instead, at 1 A (the mode's smallest current when that is higher), within the
            // monitor ceiling and the cable: a mode wholly above that is one named skip row, never a request.
            case Group::Ufcs: {
                const size_t modes = in.fc_caps ? std::min<size_t>(in.fc_caps->ufcs_n, in.fc_caps->ufcs.size()) : 0;
                if (!modes) {
                    level(5000); level(9000, true); level(11000, true); level(5000, true); level(12000, true);
                    level(20000, true, kUfcs60wMa);
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
                // Every row names the mode it samples (pack_ufcs_mode): the Results chip shows its current, the Detail page its range.
                auto tag = [&](const ufcs::Cap& c) { s.pdo = pack_ufcs_mode(c.min_mv, c.max_mv, c.min_ma, c.max_ma); };
                auto next = [&](unsigned mv, const ufcs::Cap& c) { if (mv != last) { last = mv; tag(c); level(mv, true, ma_of(c)); } };
                const ufcs::Cap* home = by_min[0];   // the mode the baseline and the step back to 5 V are judged in: the one that reaches it
                for (size_t n = 0; n < modes; ++n)
                    if (by_min[n]->min_mv <= 5000 && 5000 <= by_min[n]->max_mv) home = by_min[n];
                tag(*home); level(5000);
                for (size_t n = 0; n < modes; ++n) {
                    const ufcs::Cap& c = *by_min[n];
                    const unsigned bottom = static_cast<unsigned>(std::max(c.min_mv, 0));
                    const unsigned top = std::min<unsigned>(static_cast<unsigned>(std::max(c.max_mv, 0)), cap);
                    next(bottom, c);
                    if (bottom >= cap || top <= bottom) continue;
                    const unsigned mid = (bottom + top) / 2 / 100 * 100;
                    if (mid > bottom && mid < top) next(mid, c);
                    next(top, c);
                }
                next(5000, *home);
                break;
            }
            case Group::Vivo: level(5000); level(9000); level(11000); level(5000); break;   // 11 V: unidentified-charger cap
            // SCP (docs/FASTCHARGE_PROTOCOLS.md section 2): the handshake (ping, Reg 0x80/0x7E type bits, limits
            // 0x90..0x97) writes no current; the 10 V row asks 2.25 A (22.5 W). The
            // direct-charge rows (4.5 V 5 A, 5 V 4.5 A) only set the limit: the suite draws no current (a load aborts the
            // row, see load_fault), so the cable's current rating gates nothing. Each row is its own session: a detect-only session ends after its reads.
            // With the adapter's own limits (fc_caps.scp, read in the unit's first session) the rows stay inside them: a level or a
            // current the adapter did not report is a NoOffer row, and the top level it reported joins the plan, within the monitor.
            case Group::Scp: {
                if (!in.fc_caps || !in.fc_caps->scp.valid) {
                    level(5000); level(10000, false, kScpLowMa); level(4500, false, 5000); level(5000, false, 4500);
                    break;
                }
                const auto& lim = in.fc_caps->scp;
                auto inside = [&](unsigned mv, unsigned ma) {
                    return int(mv) >= lim.min_mv && int(mv) <= lim.max_mv && int(ma) <= lim.max_ma;
                };
                const unsigned low_ma = std::min<unsigned>(kScpLowMa, static_cast<unsigned>(lim.max_ma));
                const unsigned top = std::min<unsigned>(static_cast<unsigned>(lim.max_mv), cap);
                level(5000);
                level(10000, false, low_ma, inside(10000, low_ma));
                if (top > 5000 && top != 10000) level(top, false, low_ma, inside(top, low_ma));
                level(4500, false, 5000, inside(4500, 5000));
                level(5000, false, 4500, inside(5000, 4500));
                break;
            }
            default: level(5000); break;
            }
            continue;
        }
        // No source list: a self-powered K2 that heard PD in this attach knows the source gave it up (its caps went unanswered
        // while the K2 was PD-silent), which is a different finding from a charger that never advertised PD.
        if (!caps.valid) { skip(in.self_powered && in.pd_seen_attach ? Reason::PdGaveUp : Reason::NoOffer); continue; }
        const unsigned wire = algorithm == Algorithm::Pd2Fixed ? 1 : 2;
        if (caps.spec_rev != wire) {
            if ((algorithm == Algorithm::Pd2Fixed || algorithm == Algorithm::Pd3Fixed) &&
                o.contains(Algorithm::Pd2Fixed) && o.contains(Algorithm::Pd3Fixed) &&
                (caps.spec_rev == 1 || caps.spec_rev == 2)) continue;
            skip(Reason::RevisionUntestable); continue;
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
                skip(Reason::CableLimit);
            }
            if (listed) continue;
            // An answer with no EPR PDO at all (positions 8+ all zero; zero words are padding, USB PD R3.2 EPR_Source_Capabilities) from a source whose PDO 1 has the EPR
            // bit: the list exists but the port holds it back without an EPR cable. The row says that instead of "not offered".
            bool any_epr = false;
            for (size_t i = 7; info && i < std::min<size_t>(info->num_pdos, info->pdos.size()); ++i) any_epr |= info->pdos[i].raw != 0;
            const bool withheld = info && epr_capable_caps(caps) && !any_epr && !epr_cable_unfit(in);
            if (withheld) s.detail = kDetailEprWithheld;
            skip(epr_capable_caps(caps) && (withheld || epr_cable_unfit(in)) ? Reason::CableLimit : Reason::NoOffer); continue;
        }
        bool found = false;
        for (size_t i = 0; i < std::min<size_t>(caps.num_pdos, caps.pdos.size()); ++i) {
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
            if ((algorithm == Algorithm::Epr || algorithm == Algorithm::EprAvs) && !epr_cable(in)) { skip(Reason::CableLimit); continue; }
            if ((s.avs && (p.raw >> 28) != (epr ? 0xD : 0xE)) ||
                (epr && p.type != pd::PdoType::Fixed && !s.avs)) {
                skip(Reason::UnsupportedOffer); continue;
            }
            if (p.type != pd::PdoType::Fixed && p.type != pd::PdoType::Pps && !s.avs) {
                skip(Reason::NotImplemented); continue;
            }
            if (!p.min_mv || p.max_mv < p.min_mv) { skip(Reason::UnsupportedOffer); continue; }
            // The current a row asks: the offer's limit at `mv` and the cable's, never above `ceiling` (the rows stay at 1 A
            // except the full-current PPS row below, which asks the APDO's whole advertised current).
            auto current = [&](unsigned mv, unsigned ceiling = kPlanRowMa) {
                return static_cast<uint16_t>(std::min({ceiling, unsigned(pd::current_limit_ma(p, mv)),
                                                       unsigned(cable_ma(in))}) / 50 * 50);
            };
            s.ma = current(s.mv);
            if (p.min_mv > cap || (p.type == pd::PdoType::Fixed && p.max_mv > cap)) {
                skip(limit_reason(p.max_mv, in, algorithm)); continue;
            }
            if (p.type == pd::PdoType::Fixed) {
                if (!s.ma) skip(Reason::NoOffer); else emit(s);
                // 6 A hunt: probe 20 V at 6 A (120 W) even when the fixed PDO advertises less - the row passes on the
                // source's Accept + PS_RDY at 20 V, not on the granted current. Only when no PPS APDO of this plan
                // covers 20 V: the OEM 6 A modes (Xiaomi, CUKTECH) unlock on the APDO, a fixed PDO never offers 6 A and
                // answers Reject, which is noise on the card (hardware 2026-11-06). The APDO planner owns that row then.
                if (s.verdict != Verdict::Skip && p.max_mv == 20000 && !hunt_20v && !pps_covers(20000)) {
                    hunt_20v = true;
                    Step h = s; h.ma = 6000; h.hunt = true; h.continuous = false;
                    emit(h);
                }
                continue;
            }
            const unsigned quantum = s.avs ? 100 : 20;
            const unsigned lo = (p.min_mv + quantum - 1) / quantum * quantum;
            const unsigned hi = std::min<unsigned>(p.max_mv, cap) / quantum * quantum;
            if (lo > hi) { skip(limit_reason(p.max_mv, in, algorithm)); continue; }
            // Step up first, ending at the minimum: a no-load source drains slowly, so down-steps come last and from
            // above the 5 V contract the row starts from.
            const unsigned mid0 = ((lo + hi) / 2) / quantum * quantum;
            const unsigned mid = std::max(mid0, std::min<unsigned>(5000 / quantum * quantum, hi));
            const unsigned low_mid = std::max(lo, ((lo + mid) / 2) / quantum * quantum);
            const unsigned targets[] = {mid, hi, mid, low_mid, lo};
            for (unsigned n = 0; n < 5; ++n) {
                s.mv = static_cast<uint16_t>(targets[n]); s.ma = current(s.mv); s.continuous = n != 0;
                s.verdict = s.ma ? Verdict::Pending : Verdict::Skip;
                s.reason = s.ma ? Reason::None : Reason::NoOffer;
                if (s.ma && low_programmable(s)) {
                    if (!n_low || low_pos != s.pos) s.continuous = false;   // the first one starts from a released 5 V contract
                    ++n_low; low_pos = s.pos;
                }
                emit(s);
            }
            if (p.max_mv > cap) {
                s.mv = p.max_mv; s.ma = current(s.mv); s.continuous = false;
                skip(limit_reason(p.max_mv, in, algorithm));
            }
            // 6 A hunt, every source: probe 11 V 6 A (66 W) and 20 V 6 A (120 W) PPS even when the APDO
            // advertises 5 A (OEM unlocked modes, e.g. Xiaomi Mi Turbo after the SVID 0x2717 auth chain,
            // Lenovo 7 A); a Reject is a clean "not granted" answer, the row passes on the reply voltage.
            if (p.type == pd::PdoType::Pps) {
                const unsigned hi_cap = std::min<unsigned>(p.max_mv, cap);
                for (unsigned mv : {11000u, 20000u}) {
                    if (mv < p.min_mv || mv > hi_cap) continue;
                    if (mv == 11000 && hunt_11v) continue;   // already probed on an earlier APDO
                    if (mv == 20000 && hunt_20v) continue;   // already probed on an earlier APDO
                    (mv == 11000 ? hunt_11v : hunt_20v) = true;
                    Step h = s; h.mv = static_cast<uint16_t>(mv); h.ma = 6000; h.hunt = true;
                    h.continuous = false; h.verdict = Verdict::Pending; h.reason = Reason::None;
                    emit(h);
                }
                // Full-current row, last of the APDO: the top voltage at the whole advertised current, clamped to the cable
                // (3 A unread, see cable_ma). The 1 A rows prove the voltage steps; this one proves the source holds the voltage
                // at its rated current (11 V 5 A, 20 V 2 A). Not a hunt: it asks what the APDO offers, so it is judged like the
                // other PPS rows. Skipped when it would repeat the 1 A row at the top.
                // An APDO above 5 A has no cable that carries it: the row asks the advertised current for a cable vouched for
                // above 5 A. Any other cable leaves a neutral CableLimit skip that keeps the advertised current for the chip,
                // after the row at what the cable does hold (the 5 A, below), so the source's rated current is still proven.
                const unsigned advertised = pd::current_limit_ma(p, static_cast<uint16_t>(hi)) / 50 * 50;
                const bool over5 = advertised > 5000;
                if (over5 && over5_cable_ma(in) > 5000) {
                    Step f = s; f.mv = static_cast<uint16_t>(hi); f.ma = static_cast<uint16_t>(advertised); f.hunt = false;
                    f.continuous = false; f.verdict = Verdict::Pending; f.reason = Reason::None;
                    emit(f);
                    continue;
                }
                const uint16_t full = current(hi, ~0u);
                if (full > current(hi)) {
                    Step f = s; f.mv = static_cast<uint16_t>(hi); f.ma = full; f.hunt = false;
                    f.continuous = false; f.verdict = Verdict::Pending; f.reason = Reason::None;
                    emit(f);
                }
                if (over5) {
                    s.mv = static_cast<uint16_t>(hi); s.ma = static_cast<uint16_t>(advertised); s.hunt = false; s.continuous = false;
                    skip(Reason::CableLimit);
                }
            }
        }
        if (!found) {
            s = {}; s.algorithm = algorithm; s.group = Group::Pd;
            skip(Reason::NoOffer);
        }
    }
}
// Row layout: the units in execution order, each with its own rows. The PD rows (Pd, PdLow) can be re-planned when the source list
// arrives late (replan_pd); `placeholders` stands one unplanned row in for each PD unit until then.
void plan_pd_unit(Unit u, const pd::SourceCaps& caps, Options o, Input in, AddRef add, bool placeholders) {
    if (!placeholders) { plan_unit(u, caps, o, in, add); return; }
    Step s; s.group = Group::Pd; s.algorithm = u == Unit::Pd ? Algorithm::Pd3Fixed : Algorithm::Pps; s.mv = u == Unit::Pd ? 5000 : 3300;
    add(s);
}
void plan_units(const pd::SourceCaps& caps, Options o, Input in, const UnitList& units, AddRef add, bool placeholders = false) {
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
Summary Runner::preview(const pd::SourceCaps& caps, Options o, Input in) {
    Summary out;
    in.epr_info = nullptr;   // a preview plans from the source list alone
    in.fc_caps = nullptr;
    out.refusal = refusal(o, in, false, true);
    if (out.refusal == Reason::None && !safe5(in.vbus_mv)) out.refusal = Reason::Release;
    const UnitList units = resolve(o);
    uint32_t seen = 0;
    auto count = [&](const Step& s) {
        ++out.total;
        if (s.verdict == Verdict::Skip) ++out.skipped;
        else {
            ++out.runnable; out.max_mv = std::max(out.max_mv, s.mv);
            out.estimate_ms += kHoldMs + 1000;
            const unsigned u = static_cast<unsigned>(unit_of(s));
            if (!(seen & (1u << u))) { seen |= 1u << u; out.estimate_ms += 4000; }   // one detach + attach per unit
        }
    };
    plan_units(caps, o, in, units, count);
    return out;
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
bool Runner::info_ready(Input in) const {
    return in.pd.epr_info == pd::EprInfo::Got && in.epr_info && in.epr_info->num_pdos > 7;
}
// Ask the charger for its EPR list without entering EPR mode (once per Pd attach). The answer (a list, a refusal, silence) changes
// no verdict by itself: it only names the levels the EPR rows stand for.
bool Runner::ask_info(Input in) {
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
void Runner::epr_info_rows(const pd::SourceCaps& caps, Input in) {
    if (info_ != 3 || !info_ready(in) || !epr_capable_caps(caps)) return;
    info_rows_ = true;
    replan_pd(caps, in, false);
}
void Runner::record_pd_skips() {
    for (size_t n = 0; n < count_; ++n)
        if (steps_[n].verdict == Verdict::Skip && mode(unit_of(steps_[n])) == Mode::Pd) env_.record(steps_[n]);
    pd_skips_recorded_ = true;
}
void Runner::build(const pd::SourceCaps& caps, Options o, Input in) {
    in.epr_info = nullptr;
    in.fc_caps = nullptr;   // nothing is known of this charger's SCP / UFCS limits until a row of the unit ran (replan_fc)
    fc_replanned_ = 0;
    count_ = index_ = 0;
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
void Runner::replan_fc(Input in) {
    if (plan_frozen_ || !in.fc_caps) return;
    const Step& done = steps_[index_];
    if (done.group != Group::Scp && done.group != Group::Ufcs) return;
    const Unit u = unit_of(done);
    const uint8_t bit = done.group == Group::Scp ? 1 : 2;
    size_t first = 0;
    while (first < count_ && unit_of(steps_[first]) != u) ++first;
    if (first != index_ || (fc_replanned_ & bit)) return;   // only the unit's first row reads the limits
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
    if (nf == 0 || new_end + tail > steps_.size()) return;
    // The first row is the same row either way except for the UFCS mode it now names (the baseline is judged in the mode that reaches 5 V).
    bool changed = nf != end - first || fresh[0].pdo != steps_[first].pdo;
    for (size_t n = 1; !changed && n < nf; ++n)
        changed = std::memcmp(&fresh[n], &steps_[first + n], sizeof(Step)) != 0;
    if (!changed) return;
    steps_[first].pdo = fresh[0].pdo;
    if (new_end < end) std::copy(steps_.begin() + end, steps_.begin() + count_, steps_.begin() + new_end);
    else std::copy_backward(steps_.begin() + end, steps_.begin() + count_, steps_.begin() + new_end + tail);
    for (size_t n = 1; n < nf; ++n) steps_[first + n] = fresh[n];
    count_ = new_end + tail;
    ++revision_; env_.plan_changed();
    for (size_t n = 1; n < nf; ++n) if (steps_[first + n].verdict == Verdict::Skip) env_.record(steps_[first + n]);
}
// A source can list more PDOs within the attach (UGREEN X865: the SPR AVS APDO comes with the list re-sent after our
// Sink_Capabilities_Extended, ~360-390 ms after the first one, i.e. after the 5 V contract is Ready and inside the baseline):
// the rows follow the list in hand until a PD row has run.
void Runner::regrow_plan(const pd::SourceCaps& caps, Input in) {
    if (late_pd_ || !caps.valid || caps_sig(caps) == plan_sig_) return;
    for (size_t n = 0; n < count_; ++n)
        if (mode(unit_of(steps_[n])) == Mode::Pd && steps_[n].verdict != Verdict::Pending && steps_[n].verdict != Verdict::Skip) return;
    replan_pd(caps, in, true);
}
// A source that announces its list again with every planned row still on offer (CUKTECH C1 after the Xiaomi auth: PPS 5 A -> 6 A;
// pattern reannounce-after-cable-discovery, Baseus 70 W after the SOP' cable query: PPS 3.3 A -> 5 A, an APDO inserted ahead of ours)
// has not changed its offer: the rows follow the new list instead of ending the unit as "offers changed". No row has run yet: the PD
// rows are planned again from the new list (extra APDOs get rows). A row has run: the rows still to run take the slot and the
// word of the new list. A list that no longer offers a row (type, voltage range, a lower current, a dropped PDO, another
// revision) is not followed: the caller's check ends the unit as before, and so does a row in flight whose slot moved (its contract
// belongs to the old slot).
void Runner::follow_list(const pd::SourceCaps& caps, Input in) {
    if (mode_ != Mode::Pd || late_pd_ || !caps.valid || caps.epr || caps.epr_partial || epr_stage_ == 2 ||
        caps.spec_rev != evidence_.wire_rev || caps_sig(caps) == plan_sig_) return;
    bool started = false;
    for (const auto& s : steps()) {
        if (mode(unit_of(s)) != Mode::Pd) continue;
        if (s.verdict != Verdict::Pending && s.verdict != Verdict::Skip) started = true;
        if (s.verdict != Verdict::Pending && s.verdict != Verdict::Running) continue;
        if (s.pos >= 8) { if (!epr_capable_caps(caps)) return; continue; }   // an EPR row of an SPR list that still offers EPR
        const int at = find_offer(caps, s);
        if (at < 0 || (s.verdict == Verdict::Running && at != s.pos - 1)) return;
    }
    if (!started && !plan_frozen_ && epr_stage_ == 0) { replan_pd(caps, in, true); return; }
    bool changed = false;
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (mode(unit_of(s)) != Mode::Pd || s.pos >= 8 || (s.verdict != Verdict::Pending && s.verdict != Verdict::Running)) continue;
        const int at = find_offer(caps, s);
        if (at < 0) continue;
        const uint32_t raw = caps.pdos[static_cast<size_t>(at)].raw;
        if (s.pos != at + 1 || s.pdo != raw) { s.pos = static_cast<uint8_t>(at + 1); s.pdo = raw; changed = true; }
    }
    plan_sig_ = caps_sig(caps);
    if (changed) { caps_evidence(caps); ++revision_; env_.plan_changed(); }
}
// pattern reject-then-reduced-list (Baseus 140W, JT-P18 USB-C): the source Rejects the 6 A over-offer probe and announces a shorter list
// (no PPS APDO, PDO1 without the EPR bit). That list is its answer to our probe, not a charger that changed under the run. Is the
// running hunt row's own Request the one that was Rejected? (The sink's result, or its Request trace when the result has moved on.)
bool Runner::hunt_own_reject(const Input& in) const {
    if (state_ != State::Requesting || index_ >= count_ || busy_retry_at_) return false;
    const Step& s = steps_[index_];
    if (!s.hunt || s.group != Group::Pd) return false;
    if (in.pd.result == pd::SinkResult::Rejected) return true;
    return in.xm && in.xm->req_n != hunt_seq_ && in.xm->answer == pd::ReqAnswer::Reject;
}
// Take the list announced after our own hunt Reject as the new offer. A row that has not run follows it (its slot and word as in
// follow_list); a row it no longer offers (type or voltage range gone, a lower current, a dropped PDO, EPR without the EPR bit in PDO1)
// is skipped as "not offered". The rows still to run then go on against this list. The running row is the hunt row itself: the caller
// ends it.
void Runner::adopt_list(const pd::SourceCaps& caps) {
    if (mode_ != Mode::Pd || late_pd_ || !caps.valid || caps.epr || caps.epr_partial || epr_stage_ == 2 || caps_sig(caps) == plan_sig_) return;
    caps_evidence(caps);   // the wire revision and the offer flags (PPS, EPR) of the list now in hand
    // Like a plan that found no PDO for the row: no slot (pos 0), so the card says "PPS not advertised" / "EPR: not offered".
    const auto gone = [this](Step& s) { s.verdict = Verdict::Skip; s.reason = Reason::NoOffer; s.pos = 0; s.pdo = 0; env_.record(s); };
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (mode(unit_of(s)) != Mode::Pd || s.verdict != Verdict::Pending) continue;
        if (s.pos >= 8) {
            if (!epr_capable_caps(caps)) gone(s);   // an SPR list that still offers EPR: the entry is made when the row asks
            continue;
        }
        const int at = find_offer(caps, s);
        if (at < 0) { gone(s); continue; }
        s.pos = static_cast<uint8_t>(at + 1); s.pdo = caps.pdos[static_cast<size_t>(at)].raw;
    }
    plan_sig_ = caps_sig(caps);
    ++revision_; env_.plan_changed();
}
Reason Runner::start(const pd::SourceCaps& caps, Options o, Input in, bool confirmed) {
    o.selected &= kAllAlgorithms;
    epr_proof_ = in.epr_proof = false;
    const Reason denied = refusal(o, in, busy(), confirmed);
    if (denied != Reason::None) {
        if (!run_id_) {
            if (reason_ != denied) ++revision_;
            reason_ = denied;
        }
        return denied;
    }
    ++run_id_; ++revision_;
    info_ = 0; info_rows_ = false; n_hunt_ = 0; hunt_seq_ = 0;
    o.units = resolve(o);
    opts_ = o;
    n_order_ = o.units.n; order_ = o.units.u;
    build(caps, o, in);
    evidence_ = {};
    evidence_.selected = o.selected; evidence_.session = in.session;
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
    pd_skips_recorded_ = !late_pd_;
    for (size_t n = 0; n < count_; ++n) if (steps_[n].verdict == Verdict::Skip && (mode(unit_of(steps_[n])) != Mode::Pd || pd_skips_recorded_)) env_.record(steps_[n]);
    resumed_ = 0; loaded_ = false; plan_frozen_ = false;
    selfpwr_ = in.self_powered; suspended_ = adopt_ = held_ = false; jwait_ms_ = 0;
    session_ = in.session; resets_ = link_resets(in.pd);
    handover_ = {}; unit_ = Unit::Count; mode_ = Mode::Pd;
    attached_ = detached_ = ending_ = silent_ = legacy_active_ = never_dark_ = dipped_ = dark_ = false; attach_try_ = 0;
    pre_ms_ = 0;
    pd_retry_ = cut_index_ = SIZE_MAX; retries_ = epr_tries_ = 0; settle_ms_ = settle_until_ = hunt_reject_until_ = 0; auth_open_ = false;
    xiaomi_seen_ = xiaomi_reset_ = x_svid_ = x_authed_ = false; tail_at_ = 0; hr_count_ = link_resets(in.pd);
    last_ms_ = phase_ms_ = in.now; stable_ = stopping_ = pd_owned_ = fc_owned_ = recover_ = hot_ = sag_cut_ = sag_open_ = false; busy_retry_at_ = 0;
    reason_ = Reason::None;
    probe_ = (has_unit(o.units, Unit::Pd) || has_unit(o.units, Unit::PdLow)) ? Probe::Pending : Probe::Off;   // ask the charger who it is once, at the end of the first Pd unit
    state_ = State::Baseline;
    // An already elevated source is not safe to take over blindly.
    if (!safe5(in.vbus_mv)) { unsafe(Reason::Release); return reason_; }
    return Reason::None;
}
bool Runner::band_ok(const Step& s, const Input& in, uint32_t gap) {
    if (!averages_band(s)) return in_band(s, in.vbus_mv);
    if (gap > 250) band_.clear();   // missing samples cannot be averaged across
    const int level = int(s.mv);
    if (in.vbus_mv < 0 || std::abs(in.vbus_mv - level) * 100 > level * int(kBandGrossPct)) { band_.clear(); return false; }
    band_.add(in.vbus_mv, gap);
    return in_band(s, band_.mean());
}
// pattern output-capped-below-advertised: follow the output of a programmable row whose contract is live (see kPlateauMs).
void Runner::track_plateau(const Input& in, bool ready, uint32_t gap) {
    if (!ready || gap > 250 || in.vbus_mv < kCappedFloorMv) { plat_mv_ = 0; return; }
    if (!plat_mv_ || std::abs(in.vbus_mv - int(plat_mv_)) > std::max(100, int(plat_mv_) / 100)) {
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
void Runner::request(Input in) {
    auto& s = steps_[index_];
    const auto limit = limit_reason(s.mv, in, s.algorithm);
    // A PD row above 5 A that is no OEM hunt (a 6.25 A APDO's full-current row) needs a cable vouched for above 5 A.
    const bool over5 = s.group == Group::Pd && !s.hunt && s.ma > 5000 && over5_cable_ma(in) <= 5000;
    if (limit != Reason::None || over5 || ((s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) && !epr_cable(in))) {
        s.verdict = Verdict::Skip; s.reason = limit == Reason::None ? Reason::CableLimit : limit;
        s.detail = s.reason == Reason::OverCap && (s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) ? monitor_detail(in.cap_mv) : uint8_t(0);
        env_.record(s);
        // A skipped step must not leave a contract held: hand it back and wait for vSafe5V.
        if (pd_owned_ || fc_owned_) { release(in, false); return; }
        next(in); return;
    }
    // A 6 A hunt row asks only for what the list in hand advertises: never an over-ask (docs/CHARGER_QUIRKS.md, 6 A hunt rows).
    if (s.group == Group::Pd && s.hunt && !hunt_advertised(s)) { skip_unadvertised_hunt(in); return; }
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
    hot_ = false; busy_retry_at_ = 0; hunt_reject_until_ = 0;   // the next row: a list announced now is no answer to the old hunt row
    s.verdict = Verdict::Running;
    if (s.mv > 5000) env_.phase(Phase::Request, index_);   // journal marker BEFORE anything above 5 V
    phase_ms_ = in.now; stable_ = false; resets_ = link_resets(in.pd); band_.clear(); plat_mv_ = 0;
    state_ = State::Requesting;
    if (s.group == Group::Pd) {
        pd_owned_ = true;
        if (s.hunt) hunt_seq_ = in.xm ? in.xm->req_n : uint8_t(0);
        const auto err = env_.request_pd({s.pos, s.mv, s.ma, s.hunt});
        // Busy = identify/probe/prep still running (CUKTECH C1: the first 5 V row raced the probe, flaky fail):
        // retry within the row's normal timeout instead of failing instantly.
        if (err == pd::Err::Busy) { busy_retry_at_ = in.now + 200; return; }
        if (err != pd::Err::Ok) finish(in, Verdict::Fail, Reason::Request, 128 + static_cast<uint8_t>(err));
    } else {
        const bool retarget = fc_owned_ && s.continuous;
        fc_owned_ = true;
        const auto err = env_.request_fc(proto(s.group), s.mv, retarget, s.ma);
        if (err != fc::Fail::None) finish(in, Verdict::Fail, Reason::Request, static_cast<uint8_t>(err));
    }
}
// The row leaves the plan (the rows after it move up; the cursor is picked again by the caller).
void Runner::drop_row(size_t i) {
    if (i >= count_) return;
    for (size_t n = i + 1; n < count_; ++n) steps_[n - 1] = steps_[n];
    steps_[--count_] = {};
    ++revision_; env_.plan_changed();
}
// A 6 A hunt row whose list does not advertise its 6 A ends without any Request (no hold, no refusal evidence, no failure):
//  - the source never listed SVID 0x2717: nothing to unlock, nothing to report: the row is dropped from the plan, so the card, the
//    summary and the counts never carry it;
//  - the source did: a neutral "not unlocked" skip says which link was missing (not_unlocked_detail; 0 = the chain ran, the cable is
//    fit and the list still stayed at its own limit).
void Runner::skip_unadvertised_hunt(Input in) {
    if (!in.pd.xiaomi_svid && !xiaomi_seen_) drop_row(index_);
    else {
        // The sink forgot the chain with a Hard Reset: the run's own snapshot from before it stands in.
        if (!in.pd.xiaomi_svid) { in.pd.xiaomi_svid = x_svid_; in.pd.xiaomi_authed = x_authed_; }
        auto& s = steps_[index_];
        s.verdict = Verdict::Skip; s.reason = Reason::NotUnlocked; s.detail = not_unlocked_detail(in);
        env_.record(s);
    }
    if (pd_owned_ || fc_owned_) { release(in, false); return; }   // a skipped row must not leave a contract held
    next(in);
}
// Snapshot of a refused hunt row, from the sink's trace and the run's own cable limit; classified, handed to the environment, kept in
// the side table. Reads only: the verdict the caller is about to write does not depend on it.
void Runner::note_hunt_refusal(const Input& in, bool timeout, uint16_t extra_flags) {
    HuntEvidence e;
    e.step = static_cast<uint8_t>(index_);
    e.flags = (in.pd.xiaomi_svid ? HfSvid : 0) | (in.pd.xiaomi_authed ? HfAuthed : 0) | (in.cable_observed ? HfCableRead : 0) |
              (epr_proven(in) ? HfCableInferred : 0) | (timeout ? HfTimeout : 0) | extra_flags;
    e.cable_ma = used_cable_ma(in);
    if (const pd::XiaomiTrace* t = in.xm) {
        e.last_step = t->last_step;
        if (t->opener_sent) e.flags |= HfOpener;
        if (t->authed && in.pd.xiaomi_authed) e.authed_ms = sat16(in.now - t->authed_at);
        if (t->reannounced) e.flags |= HfReannounced;
        if (t->req_held) e.flags |= HfHeld;
        if (t->req_n != hunt_seq_) {   // a Request went out since this row asked
            e.flags |= HfSent | (t->req_in_chain ? HfInChain : 0);
            e.apdo = t->req_pdo; e.rdo = t->req_rdo; e.req_mv = t->req_mv; e.req_ma = t->req_ma;
            e.caps = t->req_caps; e.pos = t->req_pos; e.type = static_cast<uint8_t>(t->req_type);
            e.apdo_ma = pd::current_limit_ma(pd::decode_pdo(t->req_pdo), t->req_mv);
            e.answer = static_cast<uint8_t>(t->answer);
            if (t->answer != pd::ReqAnswer::None) e.answer_ms = sat16(t->answer_at - t->req_at);
        }
    }
    e.cause = static_cast<uint8_t>(classify_hunt(e));
    env_.hunt_evidence(steps_[index_], e);
    hunt_[n_hunt_ < kHuntSlots ? n_hunt_++ : kHuntSlots - 1] = e;
}
const HuntEvidence* Runner::hunt_evidence_for(size_t step) const {
    for (size_t n = 0; n < n_hunt_; ++n) if (hunt_[n].step == step) return &hunt_[n];
    return nullptr;
}
uint32_t Runner::hold_ms() const {
    return kHoldMs;
}
uint32_t Runner::hold_left_ms(uint32_t now) const {
    return state_ == State::Holding ? hold_ms() - std::min(hold_ms(), now - phase_ms_) : 0;
}
void Runner::release(Input in, bool stop) {
    stopping_ = stop;
    if (stop) skip_pending(reason_);
    reissue_release();
    // Nothing is held and VBUS never left vSafe5V: there is nothing to wait for.
    if (!stop && !pd_owned_ && !fc_owned_ && !hot_ && !recover_ && safe5(in.vbus_mv)) { next(in); return; }   // the sink may refuse and is asked again by poll(); the legacy engine always lets go
    state_ = State::Releasing; phase_ms_ = release_ms_ = in.now; stable_ = false; rel_hr_ = in.pd.hard_resets;
}
void Runner::reissue_release() {
    if (pd_owned_) env_.release(true);
    if (fc_owned_) env_.release(false);
}
void Runner::finish(Input in, Verdict v, Reason r, uint8_t detail) {
    auto& s = steps_[index_];
    s.verdict = v; s.reason = r; s.detail = detail;
    s.tries = s.group >= Group::Vooc && s.group <= Group::SuperVooc65 ? in.fc_tries : 0;
    if (s.group == Group::Pd) {
        // Attempts after a retry, the first one included. The first EPR row also carries the retries of the baseline entry
        // that made its list available (epr_tries_, consumed here).
        unsigned retried = pd_retry_ == index_ ? retries_ : 0u;
        if (s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs) { retried += epr_tries_; epr_tries_ = 0; }
        s.tries = retried ? uint8_t(std::min(retried + 1u, 7u)) : uint8_t(0);
    }
    // pattern reject-then-reset: a source that has just Rejected is still settling; the next EPR entry waits for it.
    // A hunt row the source Rejected and the run reads as "not unlocked" is the same Reject.
    if (s.group == Group::Pd && ((v == Verdict::Fail && r == Reason::Request && detail == static_cast<uint8_t>(pd::SinkResult::Rejected)) ||
                                 (r == Reason::NotUnlocked && in.pd.result == pd::SinkResult::Rejected)))
        settle_until_ = in.now + kRejectSettleMs;
    // The source that Rejected a hunt row may announce a (shorter) list after the row ended: poll_impl adopts it while Releasing.
    if (s.group == Group::Pd && s.hunt && in.pd.result == pd::SinkResult::Rejected) hunt_reject_until_ = in.now + kCapsReturnMs;
    // A fixed PD row that was judged on the window mean records that mean (a Pass, the Timeout of a level that never came in band,
    // a Voltage drift). A gross excursion cleared the window: the raw sample is the evidence there, as for every other ending.
    int measured = in.vbus_mv;
    if ((v == Verdict::Pass || r == Reason::Timeout || r == Reason::Voltage) && averages_band(s))
        if (const int mean = band_.mean(); mean >= 0) measured = mean;
    s.measured_mv = static_cast<uint16_t>(std::clamp(measured, 0, 65535));
    env_.record(s);
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
        if ((v == Verdict::Fail || v == Verdict::Skip) && r != Reason::Lost && r != Reason::Capped) {
            // Later retargets cannot prove anything after the initial handshake/contract failed (or the charger gave no response).
            // They are the levels of the same algorithm: the Class B rows that follow a QC2 / QC3 row in its session are rows of their
            // own algorithm and keep their verdict logic (a failed 12 V still leaves "Class A only" / the handshake failure to the
            // 20 V row); once the session is released they open a fresh one.
            for (size_t n = index_ + 1; n < count_ && steps_[n].continuous && steps_[n].algorithm == s.algorithm; ++n) {
                steps_[n].verdict = Verdict::Skip; steps_[n].reason = r; steps_[n].detail = detail; env_.record(steps_[n]);
            }
        }
        release(in, false);
    }
}
// The first pending row in execution order (the unit order, then the plan order inside a unit).
size_t Runner::pick() const {
    for (size_t k = 0; k < n_order_; ++k)
        for (size_t n = 0; n < count_; ++n)
            if (steps_[n].verdict == Verdict::Pending && unit_of(steps_[n]) == order_[k]) return n;
    return SIZE_MAX;
}
// The run's one scheduler: the unit in progress goes on with its next row; when it has none left it ends with its detach, and
// the next detached state starts the next unit (docs/CHARGER_TEST_FLOW.md).
void Runner::next(Input in) {
    const size_t i = pick();
    if (i != SIZE_MAX && attached_ && !ending_ && unit_of(steps_[i]) == unit_) { index_ = i; request(in); return; }
    index_ = i == SIZE_MAX ? count_ : i;
    if (attached_) {
        if (pd_owned_ || fc_owned_) { release(in, false); return; }   // Done only after the 5 V confirmation
        if (probe_ == Probe::Pending && mode_ == Mode::Pd && safe5(in.vbus_mv)) {
            // The first Pd unit is done, back at a settled 5 V: ask the charger who it is, bounded (a failure changes no row).
            probe_ = Probe::Closing; state_ = State::Baseline; stable_ = false; phase_ms_ = in.now; return;
        }
        // Legacy -> Legacy keeps the PD-silent session: only the D+/D- cleanup (the release above) sits in between. Any PD traffic
        // since the attach, or a VBUS that is not at 5 V, means the session is gone: detach and attach again as before.
        if (i != SIZE_MAX && mode_ == Mode::Legacy && mode(unit_of(steps_[i])) == Mode::Legacy && legacy_active_ && silent_ &&
            safe5(in.vbus_mv) && in.pd_quiet_ms >= kLegacyQuietMs) { continue_legacy(unit_of(steps_[i]), in); return; }
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
        if (s.verdict != Verdict::Pending) continue;
        s.verdict = Verdict::Skip; s.reason = r; env_.record(s);
    }
    if (!pd_skips_recorded_) record_pd_skips();
}
// The rest of the running unit is over (its charger never fell back, the offers changed, PD showed up): the run goes on.
void Runner::skip_unit(Reason r) {
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.verdict != Verdict::Pending || unit_of(s) != unit_) continue;
        s.verdict = Verdict::Skip; s.reason = r; env_.record(s);
    }
}
void Runner::skip_legacy_units(Reason r) {
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.verdict != Verdict::Pending || mode(unit_of(s)) != Mode::Legacy) continue;
        s.verdict = Verdict::Skip; s.reason = r; env_.record(s);
    }
}
bool Runner::legacy_row_ran() const {
    for (size_t n = 0; n < count_; ++n)
        if ((steps_[n].verdict == Verdict::Pass || steps_[n].verdict == Verdict::Fail) && mode(unit_of(steps_[n])) == Mode::Legacy) return true;
    return false;
}
// The source hard-reset (or detached) in the middle of a PD row: its contract is gone. Wait for it to come back
// (recover_ lets Releasing wait for vSafe5V) and ask the row once more; a further loss ends it as before. An EPR row (the entry
// is what a Hard Reset interrupts: pattern epr-entry-hard-reset) gets kEprEntryRetries retries, each after a longer steady 5 V
// (kEprSettleMs + kEprSettleStepMs per earlier retry); every other row gets one.
bool Runner::can_retry(const Step& s) const {
    const unsigned limit = s.algorithm == Algorithm::Epr || s.algorithm == Algorithm::EprAvs ? kEprEntryRetries : 1;
    return (pd_retry_ == index_ ? retries_ : 0u) < limit;
}
void Runner::retry_pd_row(Input in) {
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
bool Runner::hunt_auth_wait(const Input& in, const Step& row) {
    if (!in.pd.xiaomi_svid) return false;
    // Chain done: the source re-announces its list (the APDOs at 6 A) right after it. A row the list in hand does not advertise yet
    // gives that list kXiaomiReannounceMs from the chain's end before it is skipped.
    if (in.pd.xiaomi_authed)
        return in.xm && !in.xm->reannounced && !hunt_advertised(row) && in.now - in.xm->authed_at < kXiaomiReannounceMs;
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
void Runner::end_run(Input) { end_legacy(); if (!pd_skips_recorded_) record_pd_skips(); state_ = State::Done; }
void Runner::begin_unit(Unit u, Input in) {
    if (selfpwr_ && self_path(u) == SelfPath::NeedsExternalPower) {   // a row that can cut the output cannot run on the output itself
        unit_ = u; skip_unit(Reason::NeedsExternalPower); next(in); return;
    }
    unit_ = u; mode_ = mode(u);
    attached_ = silent_ = ending_ = false; attach_try_ = 0; pd_retry_ = cut_index_ = SIZE_MAX;
    pd_owned_ = fc_owned_ = recover_ = hot_ = sag_cut_ = sag_open_ = false;   // a detach/attach boundary: the window does not cross it
    unit_ms_ = in.now; pre_ms_ = 0;
    if (mode_ == Mode::Legacy) env_.phase(Phase::Handover, index_);   // journal marker BEFORE the detach: a reboot from here is a skipped handover
    if (detached_) begin_attach(in); else begin_detach(in);
}
// The next Legacy unit inside the same PD-silent attach: no detach, a gap for the D+/D- cleanup, then its first handshake.
void Runner::continue_legacy(Unit u, Input in) {
    unit_ = u;
    pd_owned_ = fc_owned_ = recover_ = hot_ = false; pd_retry_ = cut_index_ = SIZE_MAX;
    unit_ms_ = in.now;
    pre_ms_ = u == Unit::Afc ? kAfcPreMs : 0;
    session_ = in.session; resets_ = link_resets(in.pd); caps_gone_ms_ = in.now;
    state_ = State::Baseline; stable_ = true; stable_ms_ = in.now; phase_ms_ = in.now;   // kLegacyGapMs (+ kAfcPreMs) before the first row
}
// Detach: Rd off, the 5 V default, then wait for the source to drop its output (VBUS below vSafe0V for kDetachDarkHoldMs).
void Runner::begin_detach(Input in) {
    if (selfpwr_) {
        // The K2 runs on this source's VBUS through its pinned Rd: CC is never opened. The boundary is only a bookkeeping step; the
        // next attach is a hold (Soft_Reset or PD-silent). legacy_active_ stays so the run's end gives the PD policy back.
        attached_ = false; detached_ = true; hot_ = false; dark_ = dipped_ = stable_ = false;
        if (ending_) { ending_ = false; next(in); } else begin_attach(in);
        return;
    }
    state_ = State::Detach; att_ms_ = in.now; dark_ = dipped_ = stable_ = false;
    attached_ = legacy_active_ = false;
    env_.detach();
}
void Runner::poll_detach(Input in) {
    if (in.vbus_mv < pd::kVSafe0Mv) { dipped_ = true; if (!dark_) { dark_ = true; dark_ms_ = in.now; } } else dark_ = false;
    const uint32_t limit = never_dark_ ? kDetachQuickMs : kDetachDarkMs;
    if (!((dark_ && in.now - dark_ms_ >= kDetachDarkHoldMs) || in.now - att_ms_ >= limit)) return;
    if (!dipped_) never_dark_ = true;   // a source that kept its output up (a USB-A port has no CC): later detaches do not wait long
    detached_ = true; hot_ = false;
    if (ending_) { ending_ = false; next(in); }
    else begin_attach(in);
}
void Runner::begin_attach(Input in) {
    state_ = State::Attach; att_ = Att::Caps; att_ms_ = in.now; stable_ = dark_ = false;
    detached_ = false; legacy_active_ = mode_ == Mode::Legacy;
    wait_ = pd::VbusWait(); wait_.begin(in.now);
    if (selfpwr_) { env_.hold(mode_); return; }
    env_.identity(mode_ != Mode::Legacy);   // the Ra mode + spoof only for the PD units (pattern ra-disables-legacy)
    env_.attach(mode_);
}
// The unit's charger never came back: the rows fail with NoReattach, or, when no list was ever heard (not a PD source), are skipped.
void Runner::fail_unit(Input in, const pd::SourceCaps& caps) {
    if (late_pd_ && !caps.valid && !evidence_.num_pdos) replan_pd(caps, in, true);   // no PD source at all: the plan says NoOffer
    for (auto& s : std::span<Step>(steps_.data(), count_)) {
        if (s.verdict != Verdict::Pending || unit_of(s) != unit_) continue;
        s.verdict = Verdict::Fail; s.reason = Reason::NoReattach;
        s.measured_mv = static_cast<uint16_t>(std::clamp(in.vbus_mv, 0, 65535)); env_.record(s);
    }
    ending_ = true; begin_detach(in);
}
void Runner::poll_attach(Input in, const pd::SourceCaps& caps, uint32_t gap) {   // gap: ms since the previous poll (poll_impl() has already moved last_ms_)
    if (mode_ == Mode::Legacy) {
        // PD-silent: caps are heard and recorded, never acknowledged. VBUS cycles around the source's Hard Resets are expected.
        if (late_pd_ && caps.valid) replan_pd(caps, in, true);
        if (safe5(in.vbus_mv) && in.pd_quiet_ms >= kLegacyQuietMs && in.fc_state == fc::State::Idle) {
            if (!stable_ || gap > 250) { stable_ = true; stable_ms_ = in.now; }
            if (in.now - stable_ms_ >= kLegacySteadyMs) {
                if (handover_.outcome == HandoverOutcome::NotTried) handover_ = {HandoverOutcome::FellBack, in.now - unit_ms_};
                attached_ = silent_ = true; session_ = in.session; resets_ = link_resets(in.pd); hot_ = false; caps_gone_ms_ = in.now;
                // The first handshake waits 1000 ms after the PD-silent attach (the baseline settle is 500 of it).
                pre_ms_ = kLegacyFirstWaitMs - 500 + (unit_ == Unit::Afc ? kAfcPreMs : 0);
                state_ = State::Baseline; stable_ = true; stable_ms_ = in.now; phase_ms_ = in.now;
                return;
            }
        } else stable_ = false;
        if (in.now - att_ms_ < (attach_try_ ? kLegacyRetryMs : kLegacyGiveUpMs)) return;
        if (!selfpwr_ && !attach_try_ && handover_.outcome != HandoverOutcome::StayedPd) { attach_try_ = 1; begin_detach(in); return; }   // once more, longer
        if (handover_.outcome == HandoverOutcome::NotTried) handover_ = {HandoverOutcome::StayedPd, in.now - unit_ms_};
        skip_unit(Reason::BlockedByPd);
        ending_ = true; begin_detach(in);
        return;
    }
    if (att_ == Att::Caps) {
        const auto st = wait_.update(in.now, in.vbus_mv, caps.active_cc != 0, caps.valid);
        if (st == pd::VbusWait::State::Waiting) return;
        if (st != pd::VbusWait::State::Ready || !caps.valid || !caps.active_cc) { fail_unit(in, caps); return; }
        att_ = Att::Contract; att_ms_ = in.now;
        if (late_pd_) replan_pd(caps, in, true);
    }
    // Caps are in (the attach requested PDO 1): wait for the 5 V contract, bounded.
    const bool settled = caps.valid && safe5(in.vbus_mv) && in.pd.state == pd::SinkState::Ready;
    const bool late = in.now - att_ms_ >= kAttachContractMs;
    if (!settled && !(late && caps.valid && safe5(in.vbus_mv))) {
        if (late) fail_unit(in, caps);
        return;
    }
    regrow_plan(caps, in);
    attached_ = true; session_ = in.session; resets_ = link_resets(in.pd); hot_ = false; caps_gone_ms_ = in.now;
    // EPR PDOs only exist after EPR entry: an EPR-capable source on an EPR cable is entered first, then the rows are rebuilt.
    // A cable that cannot carry EPR gets no entry, only the informational list (stage 4).
    if (epr_stage_ == 3 && unit_ == Unit::Pd) {
        epr_stage_ = !caps.epr && epr_capable_caps(caps) && caps.active_cc ? (epr_cable_unfit(in) ? 4 : 1) : 0;
        info_ = 0; info_rows_ = false;
    }
    state_ = State::Baseline; stable_ = false; phase_ms_ = in.now;   // the usual 500 ms of vSafe5V, then the first row
}
void Runner::cancel(Input in) {
    if (!busy() || state_ == State::Unsafe || (state_ == State::Releasing && stopping_)) return;
    end_probe();
    ++revision_;
    reason_ = Reason::Cancelled;
    if (state_ == State::Releasing) { stopping_ = true; skip_pending(reason_); return; }
    if (state_ == State::Detach || state_ == State::Attach) { skip_pending(reason_); end_legacy(); state_ = State::Stopped; return; }
    if (index_ < count_ && steps_[index_].verdict == Verdict::Running) {
        auto& s = steps_[index_]; s.verdict = Verdict::Fail; s.reason = reason_;
        s.measured_mv = static_cast<uint16_t>(std::clamp(in.vbus_mv, 0, 65535)); env_.record(s);
    }
    release(in, true);
}
void Runner::restore(const Options& o, const Evidence& e, uint32_t run_id, std::span<const Step> steps, Phase phase,
                     size_t phase_index, uint8_t resumed, size_t attempt_unit, uint8_t attempts) {
    count_ = std::min(steps.size(), steps_.size());
    std::copy_n(steps.begin(), count_, steps_.begin());
    evidence_ = e; run_id_ = run_id; ++revision_; resumed_ = resumed; loaded_ = true; plan_frozen_ = true;
    opts_ = o; opts_.units = resolve(o); n_order_ = opts_.units.n; order_ = opts_.units.u;
    handover_ = {}; unit_ = Unit::Count; mode_ = Mode::Pd; epr_stage_ = 0; info_ = 0; info_rows_ = false; n_hunt_ = 0; hunt_seq_ = 0;
    attached_ = detached_ = ending_ = silent_ = legacy_active_ = late_pd_ = dark_ = false; pd_skips_recorded_ = true;
    pd_owned_ = fc_owned_ = stopping_ = stable_ = recover_ = hot_ = false; probe_ = Probe::Off; pd_retry_ = cut_index_ = SIZE_MAX;
    selfpwr_ = suspended_ = adopt_ = false;
    for (auto& s : std::span<Step>(steps_.data(), count_)) if (s.verdict == Verdict::Running) s.verdict = Verdict::Pending;
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
                if (steps_[n].verdict == Verdict::Pending && unit_of(steps_[n]) == u) {
                    steps_[n].verdict = Verdict::Skip; steps_[n].reason = Reason::RebootLimit; env_.record(steps_[n]);
                }
            reason_ = Reason::RebootLimit;
            if (phase == Phase::Handover) handover_ = {HandoverOutcome::Reboot, 0};
        }
    }
    index_ = 0;
    while (index_ < count_ && steps_[index_].verdict != Verdict::Pending) ++index_;
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
    ++resumed_; ++revision_;
    session_ = in.session; resets_ = hr_count_ = link_resets(in.pd);
    xiaomi_seen_ = xiaomi_reset_ = x_svid_ = x_authed_ = false; tail_at_ = 0;
    // A Pd unit goes on in the live attach (the K2 booted into a 5 V contract). A Legacy unit does what an uninterrupted run does:
    // its own detach and PD-silent attach (the Rd stays held through a detach on a charger-powered K2, so it cannot loop on its
    // own; a second loss in the handover ends the unit, see restore()). The units after it attach as usual.
    const size_t first = pick();
    unit_ = first == SIZE_MAX ? Unit::Count : unit_of(steps_[first]); mode_ = unit_ == Unit::Count ? Mode::Pd : mode(unit_);
    attached_ = first != SIZE_MAX && mode_ == Mode::Pd; detached_ = ending_ = silent_ = legacy_active_ = false; attach_try_ = 0; unit_ms_ = in.now;
    last_ms_ = phase_ms_ = in.now; stable_ = stopping_ = pd_owned_ = fc_owned_ = recover_ = sag_cut_ = sag_open_ = false;
    reason_ = Reason::None; loaded_ = false; suspended_ = false;
    selfpwr_ = in.self_powered; adopt_ = true; held_ = false; jwait_ms_ = 0;   // the page arms the spoof/cable now: that session change is not a loss
    probe_ = Probe::Off;   // a resumed run does not ask again
    state_ = State::Baseline;
    if (!safe5(in.vbus_mv)) { unsafe(Reason::Release); return reason_; }
    return Reason::None;
}
void Runner::poll(Input in, const pd::SourceCaps& caps) {
    const auto state = state_;
    const auto reason = reason_;
    const auto index = index_;
    poll_impl(in, caps);
    // Stored rows change only with a state/index transition; timer-only polls do not invalidate reads.
    if (state_ != state || reason_ != reason || index_ != index) ++revision_;
}
void Runner::poll_impl(Input in, const pd::SourceCaps& caps) {
    if (!busy()) return;
    in.epr_proof = epr_proof_;
    if (in.pd.xiaomi_svid) xiaomi_seen_ = true;
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
    // The sink refuses a release while Requesting/Transition/Aborting: ask again until it lets go.
    // Only the sink: an fc release restarts the D+/D- reset hold, so it is issued once (see release()).
    if ((state_ == State::Releasing || state_ == State::Unsafe) && in.now - release_ms_ >= 250) {
        release_ms_ = in.now; if (pd_owned_) env_.release(true);
    }
    if (in.vbus_mv >= 0 && !safe5(in.vbus_mv)) hot_ = true;   // VBUS left vSafe5V: the return needs a settle time
    // A self-powered K2 whose VBUS is gone is dying (or about to be reset): that is not the charger's verdict and not the end of
    // the run. Every unfinished row stays Pending, the journal stays open, the next steady VBUS resumes it.
    // (A planned detach/attach is not a loss: Detach/Attach already treat the VBUS dip as part of the plan.)
    // A run with nothing left to measure is just finishing: that stays the normal end.
    if (selfpwr_ && in.vbus_mv >= 0 && in.vbus_mv < 1000 && state_ != State::Detach && state_ != State::Attach && unfinished()) {
        suspend(); return;
    }
    if (state_ != State::Baseline) adopt_ = false;
    else if (adopt_ && in.session != session_ && in.vbus_mv >= 1000) session_ = in.session;
    if (state_ == State::Unsafe) {
        // Unsafe protects against a source left above vSafe5V. VBUS under kUnsafeLowMv (the vSafe5V low end) for kUnsafeGoneMs
        // (stable_ms_ times it; the vSafe5V exit below re-arms it itself) means the unplug happened or the source went to
        // standby: nothing elevated is left, so it ends Unsafe as Stopped. VBUS above vSafe5V never does. Without this a replug did
        // nothing (the exit below needs VBUS back in vSafe5V, so with nothing attached the run stayed "unsafe" and OK read "Stop"
        // until a reboot, GaN3 65W 2026-11-08), and a bank parked at a 2 V standby never cleared either (REMAX 10K, 2026-11-08).
        if (in.vbus_mv >= 0 && in.vbus_mv < kUnsafeLowMv) {
            if (!gone_ || gap > 250) { gone_ = true; stable_ms_ = in.now; }
            stable_ = false;
            if (in.now - stable_ms_ >= kUnsafeGoneMs) { end_legacy(); state_ = State::Stopped; pd_owned_ = fc_owned_ = gone_ = false; }
            return;
        }
        gone_ = false;
        if (safe5(in.vbus_mv) && !in.inline_load && !in.pd.held && !in.pd.epr_mode &&
            (in.pd.state == pd::SinkState::Idle || in.pd.state == pd::SinkState::Failed ||
             (in.pd.state == pd::SinkState::Ready && in.pd.mv == 5000)) && in.fc_state == fc::State::Idle) {
            if (!stable_ || gap > 250) { stable_ = true; stable_ms_ = in.now; }
            if (in.now - stable_ms_ >= 500) { end_legacy(); state_ = State::Stopped; pd_owned_ = fc_owned_ = false; }
        } else stable_ = false;
        return;
    }
    if (state_ == State::Detach || state_ == State::Attach) {
        // The detach and the attach are the plan: VBUS dips and the attach session changes are not a loss here.
        const Reason fault = in.vbus_mv < 0 || in.cap_mv < 5000 ? Reason::NoMonitor : in.inline_load ? Reason::Inline :
            in.fake ? Reason::Fake : Reason::None;
        if (fault != Reason::None) { reason_ = fault; skip_pending(fault); end_legacy(); state_ = State::Done; return; }
        if (state_ == State::Detach) poll_detach(in); else poll_attach(in, caps, gap);
        return;
    }
    bool offers_changed = false, legacy_pending = false, tail_seen = false, tail_wait = false;
    // A source whose output is dark (VBUS gone) announces nothing: that is "source lost", not "offers changed". The grace for the
    // list to come back restarts when VBUS does. Lenovo C135, 2026-11-03: the output drained 20 V -> 0 in 9 s after a Hard Reset.
    const bool dark = in.vbus_mv >= 0 && in.vbus_mv < 1000;
    if (caps.valid || dark) caps_gone_ms_ = in.now; else if (!caps_gone_ms_) caps_gone_ms_ = in.now;
    // pattern reject-then-reduced-list: the list announced after the hunt row's own Reject (the row is still asking, or being released)
    // is the source's answer to the probe: the rows follow it, the ones it dropped are skipped as not offered.
    const bool hunt_rejected = hunt_own_reject(in);
    if (hunt_rejected) in.pd.result = pd::SinkResult::Rejected;   // the trace says Rejected: the row ends as a refused hunt row
    if (hunt_rejected || (state_ == State::Releasing && in.now < hunt_reject_until_)) adopt_list(caps);
    follow_list(caps, in);   // before the check below: a list that still offers every row is not "changed"
    for (const auto& s : steps()) {
        if ((s.verdict != Verdict::Pending && s.verdict != Verdict::Running) || unit_of(s) != unit_) continue;   // a unit watches its own rows
        if (hunt_rejected && &s == &steps_[index_]) continue;   // the Rejected hunt row ends below, whatever list the source announced
        if (s.group != Group::Pd) { legacy_pending = true; continue; }
        // A hard reset drops the list until the source re-announces it: that is not a change; identical words resume.
        if (!caps.valid && (dark || in.now - caps_gone_ms_ < kCapsReturnMs)) continue;
        // A release in EPR mode ends with EPR_Mode Exit and the source announces its SPR list again (Baseus 140W, 2026-11-04):
        // the EPR rows (position 8 and up) are not in it, but the sink enters EPR again when one is requested. They stay valid
        // while the SPR list still offers EPR (PDO 1 carries the EPR bit) and its words are unchanged.
        if (caps.valid && !caps.epr && s.pos >= 8 && epr_capable_caps(caps) && caps.spec_rev == evidence_.wire_rev) continue;
        if (caps.valid && caps.spec_rev == evidence_.wire_rev && s.pos > caps.num_pdos) {   // the list is cut short above this row
            tail_seen = true;
            if (preauth_tail(in, tail_wait)) continue;
        }
        if (!caps.valid || caps.spec_rev != evidence_.wire_rev || s.pos > caps.num_pdos ||
            !s.pos || (!pdo_covers(caps.pdos[s.pos - 1].raw, s.pdo) && caps.pdos[s.pos - 1].raw != s.pdo))
            offers_changed = true;
    }
    if (!tail_seen) tail_at_ = 0;   // the full list is back: the next cut-short one gets a window of its own
    if (epr_stage_ == 2) offers_changed = false;   // the list is being replaced by the EPR one
    // pattern epr-entry-hard-reset: chunk 0 of the EPR list is live as a partial list (6 PDOs) while chunk 1 is awaited; a source that
    // withholds chunk 1 and Hard Resets leaves that window open. The rows above it are not "changed", the entry is incomplete:
    // the Hard Reset check below retries the row. (A stalled entry without a reset ends in the row's own timeout.)
    if (caps.valid && caps.epr && caps.epr_partial) offers_changed = false;
    // A Legacy unit sits PD-silent: any PD frame is PD traffic. (A resumed unit has no such proof: it asks the sink.)
    const bool pd_active = mode_ == Mode::Legacy &&
        (silent_ ? in.pd_quiet_ms < kLegacyQuietMs : (caps.valid && caps.active_cc) || in.pd_traffic);
    if (state_ == State::Releasing) {
        // A non-PD source that browned out (a bank asked for a level it does not have) comes back on its own:
        // wait for vSafe5V instead of ending the run. A PD contract that vanished still stops it.
        // pattern budget-reannounce-mid-run: the sink verifies the 5 V release against VBUS and, as the last resort, sends a
        // Hard Reset of its own. That dip is the recovery of this release (the port comes back at 5 V), not a lost source.
        if (pd_owned_ && in.pd.hard_resets != rel_hr_) {
            pd_owned_ = false; recover_ = true; phase_ms_ = in.now; stable_ = false; rel_hr_ = in.pd.hard_resets;
        }
        bool dropped = in.session != session_ || (in.vbus_mv >= 0 && in.vbus_mv < 1000);
        if (dropped && pd_owned_ && !stopping_ && index_ < count_ && low_programmable(steps_[index_])) {
            // The low edge cost the source its output while the row was handed back: wait for it like a cut row.
            cut_index_ = index_; pd_owned_ = false; recover_ = true; phase_ms_ = in.now;
        }
        if (dropped && !pd_owned_ && !stopping_) {
            if (!recover_) { recover_ = true; phase_ms_ = in.now; }
            session_ = in.session; stable_ = false;
        } else if (recover_) session_ = in.session;
        const Reason changed = dropped && pd_owned_ ? Reason::Lost :
            in.inline_load ? Reason::Inline : in.fake ? Reason::Fake :
            in.vbus_mv < 0 || in.cap_mv < 5000 ? Reason::NoMonitor : Reason::None;
        if (changed != Reason::None) { stopping_ = true; reason_ = changed; skip_pending(changed); }
        // Offers that changed or PD that showed up end the unit's rows, not the run.
        else if (!stopping_) {
            const Reason unit_stop = offers_changed ? Reason::CapsChanged : legacy_pending && pd_active ? Reason::PdActive : Reason::None;
            if (unit_stop != Reason::None) skip_unit(unit_stop);
        }
        // The next row is a PD row and the source has not announced its list again (it just came back from a cut or a Hard Reset):
        // a request now is refused ("no source caps") and would fail the row. Wait; offers_changed ends it after kCapsReturnMs.
        bool wait_caps = false;
        if (!stopping_ && mode_ == Mode::Pd) {
            const size_t i = pick();
            const bool pd_next = i != SIZE_MAX && steps_[i].group == Group::Pd && unit_of(steps_[i]) == unit_;
            wait_caps = !caps.valid && pd_next;
            // pattern reject-then-reset: an EPR entry does not follow a Reject at once (the source is still settling).
            if (pd_next && (steps_[i].algorithm == Algorithm::Epr || steps_[i].algorithm == Algorithm::EprAvs) &&
                !in.pd.epr_mode && in.now < settle_until_) wait_caps = true;
            // A 6 A hunt row does not go out while the source's Xiaomi auth chain is still running: the APDO limits rise after it.
            if (pd_next && steps_[i].hunt && hunt_auth_wait(in, steps_[i])) wait_caps = true;
            // pattern preauth-list-after-reset: the list is cut short, the rest of it follows (preauth_tail).
            if (pd_next && tail_wait) wait_caps = true;
        }
        const bool released = (!pd_owned_ || (!in.pd.held && !in.pd.epr_mode &&
                              (in.pd.state == pd::SinkState::Idle ||
                               (in.pd.state == pd::SinkState::Ready && in.pd.mv == 5000) ||
                               in.pd.state == pd::SinkState::Failed))) &&
                              (!fc_owned_ || in.fc_state == fc::State::Idle);
        if (safe5(in.vbus_mv) && released && !in.inline_load && in.cap_mv >= 5000) {
            if (!stable_ || gap > 250) { stable_ = true; stable_ms_ = in.now; }
            // Settle only when VBUS actually moved or a PD contract was held; the source being back ends a recovery.
            if (!wait_caps && in.now - stable_ms_ >= (recover_ ? std::max<uint32_t>(100u, settle_ms_) : (hot_ || pd_owned_) ? 500u : 0u)) {
                end_sag_recovery(in);
                pd_owned_ = fc_owned_ = recover_ = false; cut_index_ = SIZE_MAX; settle_ms_ = 0;
                if (stopping_) { end_legacy(); state_ = State::Stopped; }
                else next(in);
            }
        } else stable_ = false;
        // The sink is still verifying VBUS after its 5 V release (retries, then its own Hard Reset): its window is bounded by the
        // sink, so this one is not cut short at 8 s.
        if (state_ == State::Releasing && !in.pd.verifying && !(wait_caps && safe5(in.vbus_mv)) &&
            in.now - phase_ms_ >= (recover_ ? kRecoverMs : 8000)) {
            if (recover_ && in.vbus_mv >= 0 && in.vbus_mv < 1000 && cut_index_ < count_) {
                // pattern latch-off-low-pps: the source shut its output off on the low edge and stays dead until a replug.
                // That row carries the reason; the run is complete (everything before it ran), not "source lost".
                auto& cut = steps_[cut_index_];
                cut.verdict = Verdict::Fail; cut.reason = Reason::ShutOff; cut.measured_mv = 0; env_.record(cut);
                skip_pending(Reason::ShutOff); end_legacy(); state_ = State::Done; recover_ = false; cut_index_ = SIZE_MAX;
            } else if (in.vbus_mv >= 0 && in.vbus_mv < kUnsafeLowMv) {
                // Nothing came back, or the port dropped to a standby level under vSafe5V (2.05 V on a REMAX 10K USB-A after the
                // reset): nothing elevated is left, so there is nothing to unplug. The source is lost, the run stops here.
                reason_ = Reason::Lost; skip_pending(reason_); end_legacy(); state_ = State::Stopped; recover_ = false;
                pd_owned_ = fc_owned_ = false;
            } else if (safe5(in.vbus_mv) && !in.inline_load && in.cap_mv >= 5000) {
                // The release check is not answering but VBUS sits at 5 V: carry on, do not stop the run.
                end_sag_recovery(in);
                pd_owned_ = fc_owned_ = recover_ = false;
                if (stopping_) { end_legacy(); state_ = State::Stopped; } else next(in);
            } else unsafe(Reason::Release);
        }
        return;
    }
    // A new attach session on a legacy (non-PD) row only counts as a lost source when VBUS also fell below the row's own
    // level (-15 %): a charger that glitched the CC line but still holds its target keeps the row going. The check is
    // relative to the row, so a 4.5 V SCP row or a 5 V VOOC row that sags to 4.5-4.7 V is not "source lost".
    const bool baseline = state_ == State::Baseline;
    const bool legacy_row = !baseline && steps_[index_].group != Group::Pd && !pd_owned_;
    const int level_mv = legacy_row && steps_[index_].mv ? int(steps_[index_].mv) : 5000;
    // A session change the row survived (VBUS back within 15 % of its level) is adopted. It is also the sign of a port that sags on
    // D+/D- lows (pattern usb-a-cuts-on-low-dpdm, JT-P18 USB-A 2026-11-07: every row's own 0 V reset sags VBUS to ~3.6 V and drops
    // the cable's Rp): sag_cut_ opens the adopt window after this row's release too, so the next reset's tail session is not "lost".
    if (legacy_row && in.session != session_ && in.vbus_mv >= level_mv * 85 / 100) { session_ = in.session; sag_cut_ = true; }
    // pattern usb-a-cuts-on-low-dpdm: a new session shortly after a legacy row's sag recovery is that sag's tail (the Rp of an A-to-C
    // cable returns after VBUS), not a lost source. VBUS back at vSafe5V: adopt it; still climbing (1 V up to there): wait, bounded by
    // the window; under 1 V or past the window it stays Lost below.
    if (sag_open_ && in.now - sag_ms_ >= kSagAdoptMs) sag_open_ = false;
    // A source that never gives up PD in the PD-silent attach revives it on its own (GaN3 65W A2412-65CC, 2026-11-08): its Hard Reset
    // drops VBUS under vSafe5V (or to 0 V) before the first Baseline has run a row, and its caps follow. The sink has driven nothing
    // that could cost it the output (the attach holds no contract), so the sag is the source's: the Baseline waits for it in the same
    // adopt window as a legacy row's sag tail, bounded by kRecoverMs from the Baseline's start. Past that, VBUS still down is
    // "5V release failed" / Lost as before; VBUS above vSafe5V never waits. Only before the first legacy row has run: after one, a
    // sag may be the row's own and its recovery owns the window (sag_open_ above).
    if (baseline && mode_ == Mode::Legacy && !sag_open_ && in.vbus_mv >= 0 && in.vbus_mv < 5500 && !safe5(in.vbus_mv) &&
        in.now - phase_ms_ < kRecoverMs) {
        // a legacy row that ran (Pass/Fail): the sag may be its own, its recovery owns the window
        if (!legacy_row_ran()) { sag_open_ = true; sag_ms_ = in.now; }
    }
    // The JT-P18 USB-A's reset sag dips the fast VBUS reading under 1 V for an instant (0.65 V) while the 2 s samples stay at
    // 3.8-4.9 V: inside the window a dip waits too, and only VBUS still down when the window closes is a loss.
    // In the Baseline (the next row's 5 V check) the sag shows first as VBUS below vSafe5V with the session unchanged (JT-P18, the
    // release check then read "5V release failed"): there any VBUS outside vSafe5V waits too.
    const bool sag_wait = (legacy_row && (in.session != session_ || in.vbus_mv < 1000)) ||
                          (baseline && mode_ == Mode::Legacy && (in.session != session_ || !safe5(in.vbus_mv)));
    if (sag_open_ && sag_wait && !in.inline_load && in.cap_mv >= 5000) {
        if (safe5(in.vbus_mv)) session_ = in.session;
        else { stable_ = false; return; }
    }
    const Reason fault = in.vbus_mv < 0 || in.cap_mv < 5000 ? Reason::NoMonitor : in.inline_load ? Reason::Inline :
        !baseline && in.ibus_ma > kLoadAbortMa ? Reason::Inline :   // the suite draws no current: a load aborts
        in.session != session_ || in.vbus_mv < 1000 ? Reason::Lost : in.fake ? Reason::Fake :
        in.vbus_mv > int(in.cap_mv) ? Reason::OverCap :
        in.vbus_mv > (baseline ? cable_mv(in) : measured_cable_limit_mv(in, steps_[index_].algorithm)) ? Reason::CableLimit :
        !baseline && ((steps_[index_].algorithm == Algorithm::Epr || steps_[index_].algorithm == Algorithm::EprAvs) && !epr_cable(in)) ? Reason::CableLimit :
        !baseline && steps_[index_].group != Group::Pd && steps_[index_].group != Group::Ufcs &&
            pd_active ? Reason::PdActive : offers_changed ? Reason::CapsChanged : Reason::None;
    // UFCS is exempt from the PD-active fault: a source that ignores UFCS starts PD cable discovery (SOP') on CC ~280 ms after the
    // handshake (Baseus FH22 charger, 2026-11-07), inside the engine's listen window. That traffic is the source falling back, not proof
    // the row cannot run: the engine's own answer (no response) is the result, and the unit's pd_active check applies between rows.
    if (fault != Reason::None) {
        // pattern silent-sink-output-off (GaN3 65W A2412-65CC, 2026-11-08): a PD source that switches its output off when the sink
        // stays silent on PD, and only comes back on a PD attach. VBUS dark in a PD-silent Baseline, past the adopt window above and
        // before any legacy row has run, is that answer, not a lost or unsafe source (a source that had answered a row and then
        // loses VBUS stays Lost / recovers). Every legacy unit of the run is skipped (a later PD-silent attach would do the same),
        // the sink detaches and the next unit is the normal PD attach.
        if (fault == Reason::Lost && baseline && mode_ == Mode::Legacy && !selfpwr_ && in.vbus_mv >= 0 && in.vbus_mv < 1000 &&
            !legacy_row_ran()) {
            skip_legacy_units(Reason::PdOnly);
            stable_ = false; ending_ = true; begin_detach(in);
            return;
        }
        if (fault == Reason::Lost && !baseline && steps_[index_].group != Group::Pd && !pd_owned_) {
            // A legacy row cost the source its output: fail the row, wait for 5 V in Releasing, carry on.
            recover_ = true; sag_cut_ = true; session_ = in.session;
            finish(in, Verdict::Fail, fault);
            if (state_ == State::Releasing) phase_ms_ = in.now;
            return;
        }
        if (fault == Reason::Lost && (state_ == State::Requesting || state_ == State::Holding) &&
            steps_[index_].group == Group::Pd) {
            // pattern cut-and-recover-low-pps: the source's output collapsed on the low edge. Asking again only cuts it again
            // (the Lenovo C135 comes back by itself ~3.3 s later): that row is recorded at once, as a cut; Releasing turns it
            // into ShutOff when nothing ever returns.
            const auto& row = steps_[index_];
            const bool cut = low_programmable(row) && in.vbus_mv < int(row.mv) * 85 / 100;
            if (can_retry(row) && !cut) { retry_pd_row(in); return; }
            // Lost again on the retry: this row fails, the others still get their turn once the source is back
            // (Releasing waits for it; a source that never returns still ends the run there, as Lost).
            if (low_programmable(steps_[index_])) cut_index_ = index_;
            pd_owned_ = false; recover_ = true; session_ = in.session;
            finish(in, Verdict::Fail, cut ? Reason::CutRecovered : fault);
            if (state_ == State::Releasing) phase_ms_ = in.now;
            return;
        }
        if (fault == Reason::CableLimit && !baseline) {
            // The cable limit never ends the run: skip the row, hand the contract back and carry on.
            auto& s = steps_[index_];
            s.verdict = Verdict::Skip; s.reason = fault;
            s.measured_mv = static_cast<uint16_t>(std::clamp(in.vbus_mv, 0, 65535));
            env_.record(s);
            release(in, false);
            return;
        }
        if (fault == Reason::PdActive || fault == Reason::CapsChanged) {
            // Unit-scoped: this unit's rows end here, the detach follows and the next unit gets its turn.
            skip_unit(fault);
            if (!baseline) finish(in, Verdict::Fail, fault); else release(in, false);
            return;
        }
        reason_ = fault;
        if (!baseline) { finish(in, Verdict::Fail, fault); stopping_ = true; skip_pending(fault); }
        else release(in, true);
        return;
    }
    if (baseline) {
        if (epr_stage_ == 2 && link_resets(in.pd) != resets_) {
            // pattern epr-entry-hard-reset in the baseline entry: the source reset in the middle of it. Enter again once its SPR
            // contract has been steady for the settle time (pre_ms_ counts from the next vSafe5V), at most kEprEntryRetries times.
            resets_ = link_resets(in.pd); pd_owned_ = false; stable_ = false; phase_ms_ = in.now;
            if (epr_tries_ < kEprEntryRetries) { pre_ms_ = kEprSettleMs + kEprSettleStepMs * epr_tries_; ++epr_tries_; epr_stage_ = 1; }
            else {   // gave up: the other rows follow once the source has settled
                pre_ms_ = kEprSettleMs; epr_stage_ = 0; epr_info_rows(caps, in);
                epr_skips(Reason::EprRefused, uint8_t(kDetailEprFail + std::min<unsigned>(in.pd.epr_fail, 7)));
            }
            return;
        }
        // The reset left VBUS sagging below vSafe5V while the source re-arms: not an unsafe source, wait (bounded) for it to return.
        if (epr_stage_ < 2 && epr_tries_ && !safe5(in.vbus_mv) && in.vbus_mv >= 1000 && in.now - phase_ms_ < kRecoverMs) {
            stable_ = false; return;
        }
        if (!safe5(in.vbus_mv)) { unsafe(Reason::Release); return; }
        if (mode_ == Mode::Pd && epr_stage_ == 0) regrow_plan(caps, in);   // the list may still grow during the baseline
        if (!stable_ || gap > 250) { stable_ = true; stable_ms_ = in.now; }
        if (in.now - stable_ms_ >= 500 + pre_ms_) {
            pre_ms_ = 0;
            if (probe_ == Probe::Closing) {
                if (in.pd.state == pd::SinkState::Ready) {   // a live contract: the source is quiet and ACKs
                    probe_ = env_.probe_identity(true) ? Probe::Running : Probe::Off;
                    probe_ms_ = in.now;
                } else if (in.now - phase_ms_ < kProbeMaxMs) return;   // wait a little for the contract to come back
                else probe_ = Probe::Off;
            }
            if (probe_ == Probe::Running) {
                if (!in.probe_done && in.now - probe_ms_ < kProbeMaxMs) return;   // the charger is being asked; vSafe5V is watched above
                end_probe();
            }
            if (epr_stage_ == 4) {   // the cable cannot carry EPR: no entry, only the levels the charger lists
                if (in.pd.state != pd::SinkState::Ready) {
                    if (in.now - phase_ms_ < kEprReadyMs) return;
                    epr_stage_ = 0;
                } else if (!ask_info(in)) return;
                else { epr_stage_ = 0; epr_info_rows(caps, in); }
            }
            if (epr_stage_ == 1) {
                if (in.pd.state != pd::SinkState::Ready) {
                    if (in.now - phase_ms_ < kEprReadyMs) return;   // the sink is re-contracting (probe, hard reset): wait for Ready
                    epr_stage_ = 0; epr_skips(Reason::NoContract);   // no contract to enter from
                } else if (!epr_cable(in) && !ask_info(in)) {
                    return;   // an unread cable: the levels are asked first, so they survive a refused entry
                } else {
                    const auto err = env_.request_pd({0, 0, 0});   // enter EPR mode and read the list, request nothing
                    if (err == pd::Err::Ok) { epr_stage_ = 2; epr_ms_ = in.now; pd_owned_ = true; resets_ = link_resets(in.pd); return; }
                    epr_stage_ = 0; epr_info_rows(caps, in);
                    epr_skips(Reason::EprRefused, uint8_t(kDetailEprFail + std::min<unsigned>(in.pd.epr_fail, 7)));
                }
            }
            if (epr_stage_ == 2) {
                if (caps.epr && !caps.epr_partial && in.pd.epr_mode && in.pd.state == pd::SinkState::Ready &&
                    in.pd.result == pd::SinkResult::Ok && !in.pd.held) {
                    epr_stage_ = 0; epr_proof_ = in.epr_proof = true;
                    replan_pd(caps, in, true);   // the EPR list replaces the plan's PD rows
                } else if (in.pd.state == pd::SinkState::Failed || in.now - epr_ms_ >= 8000) {
                    epr_stage_ = 0; epr_info_rows(caps, in);
                    epr_skips(Reason::EprRefused, uint8_t(kDetailEprFail + std::min<unsigned>(in.pd.epr_fail, 7)));
                    release(in, false); return;   // leave EPR if it was entered, then carry on with the other rows
                } else return;
            }
            if (attached_ && pd_active) skip_unit(Reason::PdActive);   // a Legacy attach that sees PD live: its rows cannot run
                                                                       // (a resumed unit is not attached yet: next() detaches and attaches it)
            next(in);
        }
        return;
    }
    const auto& s = steps_[index_];
    if (link_resets(in.pd) != resets_) {
        // pattern hard-reset-on-6a-over-offer: a source that takes the 6 A over-offer as a Hard Reset (Xiaomi 20K, hardware 2026-11-07)
        // did not unlock, the same as one that rejects it. The row is a Skip, not a retry (a second Request gets the same reset, and
        // the re-announced list is the pre-auth one) and not a run-level HardReset fail.
        if (s.hunt && s.group == Group::Pd) {
            Input snap = in;   // the sink cleared the chain state and its trace with the reset: use the run's snapshot
            snap.pd.xiaomi_svid = x_svid_; snap.pd.xiaomi_authed = x_authed_; snap.xm = nullptr;
            note_hunt_refusal(snap, false, HfHardReset);
            pd_owned_ = false; recover_ = true; session_ = in.session;
            finish(in, Verdict::Skip, Reason::NotUnlocked, hunt_reset_detail(snap));
            if (state_ == State::Releasing) phase_ms_ = in.now;
            return;
        }
        if (s.group == Group::Pd && can_retry(s)) { retry_pd_row(in); return; }
        // The row failed twice to a source reset: it is marked, the source's return is awaited, the other rows go on.
        if (low_programmable(steps_[index_])) cut_index_ = index_;
        pd_owned_ = false; recover_ = true; session_ = in.session;
        finish(in, Verdict::Fail, Reason::HardReset);
        if (state_ == State::Releasing) phase_ms_ = in.now;
        return;
    }
    const bool ready = s.group == Group::Pd ?
        in.pd.state == pd::SinkState::Ready && in.pd.result == pd::SinkResult::Ok && in.pd.pos == s.pos && in.pd.mv == s.mv :
        in.fc_state == fc::State::Active;
    // SVOOC adapter that clocked and answered SVOOC frames but stops before the cable check: detected and "handshake ok" (cable check accepted, no ramp) are passes.
    if (s.group != Group::Pd && in.fc_state == fc::State::Failed && (in.fc_fail == fc::Fail::Detected || in.fc_fail == fc::Fail::NoVoltage)) {
        finish(in, Verdict::Pass, Reason::None, static_cast<uint8_t>(in.fc_fail));
        return;
    }
    if ((s.group == Group::Pd && (in.pd.state == pd::SinkState::Failed || hunt_rejected ||
        (in.pd.state == pd::SinkState::Ready && in.pd.result != pd::SinkResult::None && in.pd.result != pd::SinkResult::Ok))) ||
        (s.group != Group::Pd && in.fc_state == fc::State::Failed)) {
        if (s.group == Group::Qc3Plus && in.fc_fail == fc::Fail::NoResponse) {   // VBUS never left 5 V: the charger does not do QC3+
            finish(in, Verdict::Skip, Reason::NoQc3Plus, static_cast<uint8_t>(in.fc_fail));
            return;
        }
        // the 20 V level of a Class B row: the charger stopped answering below it (a Class A charger). Without a QC2 handshake in the
        // run the row is an ordinary failure below (class_a_only).
        if (is_class_b_peak(s) && s.algorithm == Algorithm::Qc2ClassB && qc3_class_b_peak_passed({steps_.data(), count_})) {
            finish(in, Verdict::Fail, Reason::Qc3Only20V, static_cast<uint8_t>(in.fc_fail));
            return;
        }
        if (class_a_only(s, {steps_.data(), count_})) {
            finish(in, Verdict::Skip, Reason::ClassAOnly);   // no engine detail: the reason is the whole explanation
            return;
        }
        if (s.hunt) note_hunt_refusal(in, false);
        if (s.hunt) if (const uint8_t why = not_unlocked_detail(in)) { finish(in, Verdict::Skip, Reason::NotUnlocked, why); return; }
        const auto group = s.group;
        finish(in, Verdict::Fail, Reason::Request, group == Group::Pd ? static_cast<uint8_t>(in.pd.result) : static_cast<uint8_t>(in.fc_fail));
        // AFC / FCP / SCP the charger never answered (pattern qc-only-unacked-rise): the protocol is not spoken, so none of its
        // other levels can prove anything and none of them needs an engine session of its own.
        // A UFCS source that never answered the opening exchange is the same case (no response to its listen window).
        if (((group == Group::Afc || group == Group::Fcp || group == Group::Scp) && in.fc_fail == fc::Fail::Unsupported) ||
            (group == Group::Ufcs && in.fc_fail == fc::Fail::NoResponse))
            for (size_t n = 0; n < count_; ++n) {
                auto& o = steps_[n];
                if (o.group != group || o.verdict != Verdict::Pending) continue;
                o.verdict = Verdict::Skip; o.reason = Reason::Request; o.detail = static_cast<uint8_t>(in.fc_fail); env_.record(o);
            }
        return;
    }
    const bool banded = band_ok(s, in, gap);
    if (state_ == State::Requesting) {
        if (busy_retry_at_ && in.now >= busy_retry_at_) {
            busy_retry_at_ = 0;
            const auto err = env_.request_pd({s.pos, s.mv, s.ma, s.hunt});
            if (err == pd::Err::Busy) busy_retry_at_ = in.now + 200;
            else if (err != pd::Err::Ok) finish(in, Verdict::Fail, Reason::Request, 128 + static_cast<uint8_t>(err));
        }
        if (ready && banded) {
            if (!stable_ || gap > 250) { stable_ = true; stable_ms_ = in.now; }
            if (in.now - stable_ms_ >= 500) { state_ = State::Holding; phase_ms_ = stable_ms_; }
        } else stable_ = false;
        track_plateau(in, ready, gap);
        const uint32_t timeout = s.group == Group::Qc2 ? fc::kQc2TransitionMs + 3000 : 12000;
        if (state_ == State::Requesting && in.now - phase_ms_ >= timeout) {
            if (is_class_b_peak(s) && s.algorithm == Algorithm::Qc2ClassB && qc3_class_b_peak_passed({steps_.data(), count_}))
                finish(in, Verdict::Fail, Reason::Qc3Only20V);   // VBUS never reached 20 V, but QC3 Class B got there
            else if (class_a_only(s, {steps_.data(), count_}))
                finish(in, Verdict::Skip, Reason::ClassAOnly);   // VBUS never reached 20 V: not a Class B charger
            else {
                if (s.hunt) note_hunt_refusal(in, true);
                if (const uint8_t why = s.hunt ? not_unlocked_detail(in) : uint8_t(0)) finish(in, Verdict::Skip, Reason::NotUnlocked, why);
                else finish(in, Verdict::Fail, capped(s, in, ready) ? Reason::Capped : Reason::Timeout);
            }
        }
    } else {
        const bool renewing = s.group == Group::Pd && in.pd.held && in.pd.pos == s.pos && in.pd.mv == s.mv &&
            (in.pd.state == pd::SinkState::Requesting || in.pd.state == pd::SinkState::Transition);
        if ((!ready && !renewing) || !banded) { finish(in, Verdict::Fail, Reason::Voltage); return; }
        if (gap > 250) phase_ms_ = in.now;  // Missing samples cannot establish a continuous hold.
        if (in.now - phase_ms_ >= hold_ms()) finish(in, Verdict::Pass, Reason::None);
    }
}
}  // namespace k2::charger_test
