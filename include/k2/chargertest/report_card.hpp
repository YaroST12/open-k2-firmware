// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger report card: the compact, fixed-size record kept for every finished charger-test run.
//
// Pure logic over values and buffers (host-tested by tests/test_report_card.cpp). The flash store that keeps
// the cards is k2/chargertest/report_card_store.hpp; the identity inputs (PDO list, VID/PID) come from the caller.
//
// Identity: fp = FNV-1a 32 over the little endian bytes of the raw PDO list, then the PD spec revision byte
// (0 is reserved for "no identity"). VID/PID and the charger's rated power (pdp_w) come from the source probe
// the charger test runs at 5 V (k2/pd/pd_probe.hpp: Discover Identity, Manufacturer_Info, Source_Cap_Extended);
// each is 0 when the charger did not answer. The user-visible name belongs to the identity: renaming and
// re-testing keep it. A card without a user name gets "<Brand> <PDP>W" (auto_name) and is flagged name_auto, so
// the next probe may improve it, but a name the user typed is never replaced.
//
// Record (journal-style framing, all little endian):
//   0 u8 0xA6   1 u8 type (1 card)   2 u8 payload length (even)   3 u8 0   4.. payload   crc16 CCITT (bbstore::crc16)
// Payload (kPayload bytes):
//   0 fp u32   4 vid u16   6 pid u16   8 card_no u16   10 runs u16   12 boots u32   16 uptime_s u32   20 fw u32
//   24 handover_ds u16   26 spec_rev   27 num_pdos   28 handover outcome   29 final state   30 final reason
//   31 n_ranges   32 evidence flags   33 format (0 = first cards, 1 = packed groups below, 2 = pdp and flags)
//   34 name[22] (NUL padded; 20 characters used) with the two spare bytes in format 2: 54 pdp_w (0 = unknown), 55 flags (bit0 name_auto)
//   56 groups[12] x 4: byte0 = verdict<<6 | reason, then a u24: bits 0..8 max_dv, 9..17 fail_dv (100 mV units, 9 bits each),
//      18..23 detail (bit 23 = a pd::Err, which the step stores as 128 + err; bits 18..22 the code). Format 0 had
//      byte1 = detail and u16 max_mv, and no failing target; decode migrates it.
//  104 ranges[4] x 10: u8 algorithm, u8 APDO position (0 in older cards), u16 lo, hi, max, fail (all in 100 mV units).
//      lo uses bits 0..8 for the voltage and bits 9..15 for the advertised max current in 50 mA units (format 3; 0 = unknown).
//      Byte 55 (flags) bit0 is name_auto and bits 1..7 are the highest fixed PDO current in 50 mA units (format 3).
//  144 epr[3] x 6: u16 target_dv (100 mV units, the EPR level: 280/360/480) + the same 4-byte group packing as groups
//      (byte0 = verdict<<6 | reason&0x3F, u24 = max_dv(9b) | fail_dv(9b)<<9 | packed detail<<18). Format v4 only.
//  162 cable x 16 (format v5 only): u32 seq (e-marker library entry used, 0 = none/read-only cable), u16 vid, u16 pid,
//      u16 max_mv, u16 max_ma (the cable limits the run used), u8 kind (a built-in cable id 1..4, 0 = saved clone, 0xFF = a real cable),
//      u8 flags (bit0 observed via SOP'/SOP'', bit1 spoofed, bit2 self-powered run, bits 3..5 Ra guard reading + 1,
//      bit6 started anyway against a refusing guard; older cards hold 0 in bits 3..6), u8 resumed (how many times the run lost power and went on; older
//      cards hold 0 here), u8 src_flags (bit0 the source listed OPPO's SVID, bits 1..7 the SCP current limit the adapter reported in 50 mA units; older cards hold 0). Older formats decode with an empty cable.
//  178 ufcs[4] x 3 (format v6 only): a u24 per UFCS mode of the charger's Output_Capabilities, planner order (lowest minimum first), unused
//      slots all zero: bits 0..7 lo (100 mV, saturates at 25.5 V), bits 8..16 hi (100 mV), bits 17..23 the mode's maximum current in 50 mA
//      units (saturates at 6.35 A). A source listing more than four modes keeps the first four. Older formats decode with no modes.
//  190 repeat x 6 (format v7 only): u8 times (how many runs gave exactly the results this card holds; 1 = tested once, saturates at 255),
//      u16 last_boots and u24 last_uptime_s (the session boot counter and uptime of the newest of those runs; read only when times > 1,
//      zero otherwise; saturate at 65535 and 2^24 - 1). boots / uptime_s above stay those of the run that was stored first. Older
//      formats decode as times = 1 (their bytes at 190.. are a CRC and padding, never read).
//
// A re-run that gives the same results is counted, not stored (same_results, repeat_of; the store applies them): the card keeps its
// rows, runs goes up (the charger was tested again), times goes up and last_* name the newest run. Two runs are "the same" when
// every stored result row is equal: verdict, reason and detail of each group, the failing targets (fail_mv, fail_dv), each range's
// offer and each EPR level, the UFCS modes, the PD offer facts (PDO count, spec revision, evidence flags, highest fixed current, source
// flags), the handover outcome, the final state and reason, the firmware build and the cable's identity (vid, pid, kind, limits).
// The measured maxima (group max_mv, range max_dv, EPR max_mv) compare within one stored step (100 mV): the card keeps no more
// resolution than that, so a level that sits on a rounding boundary and fell into the next step is noise, not a change. The
// comparison is always against the card that is stored (never against the previous repeat), so a slow drift is a new run once it
// reaches two steps. Never compared: boots, uptime_s, the repeat block, card number and run count, the name, vid / pid / pdp_w (a
// probe that answered once and not the next time is no change; the stored card learns what the repeat adds), the handover time
// (handover_ds), the number of power-loss resumes and the cable block's library entry number and flag byte.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/core/bbstore.hpp"
#include "k2/chargertest/charger_test.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/pd/pd_sink.hpp"

namespace k2::report_card {

namespace ct = k2::charger_test;

inline constexpr unsigned kGroups = unsigned(ct::Group::Count);
inline constexpr unsigned kMaxRanges = 4;
inline constexpr unsigned kNameMax = 20;
inline constexpr unsigned kReasonCount = unsigned(ct::Reason::Capped) + 1;
static_assert(kReasonCount <= 64, "the card stores a reason in 6 bits");
inline constexpr uint8_t kMagic = 0xA6, kTypeCard = 1, kTypeDelete = 2;
inline constexpr uint8_t kFormat = 7;   // payload byte 33: v4 adds per-EPR-level verdicts, v5 the cable block (Card carries it), v6 the UFCS modes, v7 the repeat counter
inline constexpr unsigned kMaxUfcs = 4;

// Card verdict of one protocol group: None = the group was not part of the run.
enum class GV : uint8_t { None, Pass, Fail, Skip };

struct Identity {
    uint32_t fp = 0;   // 0 = unknown
    uint16_t vid = 0, pid = 0;
    uint8_t pdp_w = 0;   // rated power of the source in watts (Source_Cap_Extended PDP, else the largest PDO); 0 = unknown
    bool operator==(const Identity&) const = default;
};
struct Context {
    uint32_t fw = 0, boots = 0, uptime_s = 0;
};
struct GroupResult {
    GV verdict = GV::None;
    uint16_t max_mv = 0;     // highest voltage measured on a passing step, kept in 100 mV (fixed supplies; ranges have their own)
    uint8_t detail = 0;      // of the first failing (or skipped) step, as in ct::Step
    uint8_t reason = 0;      // ct::Reason of that step
    uint16_t fail_mv = 0;    // target of the first failing fixed step, 100 mV; 0 = none (or an old card)
    bool operator==(const GroupResult&) const = default;
};
struct Range {
    uint8_t alg = 0;         // ct::Algorithm (Pps, SprAvs, EprAvs)
    uint8_t pos = 0;         // 1 based PDO position of the APDO: two APDOs can share alg and lo
    uint16_t lo_dv = 0, hi_dv = 0;   // offered range, 100 mV
    uint16_t max_dv = 0;     // highest voltage reached, 100 mV: a passed step's measured level, or a Capped step's ceiling
    uint16_t fail_dv = 0;    // first failing target, 100 mV (0 = none failed)
    uint16_t ma = 0;         // advertised max current of the APDO, mA in 50 mA steps (0 = unknown, older card)
    bool operator==(const Range&) const = default;
};

// Per-EPR-level result: one entry for each fixed EPR voltage (28 V, 36 V, 48 V).
// The target voltage in 100 mV units (280, 360, 480) and the group-style verdict/reason/detail.
struct EprLevel {
    uint16_t dv = 0;         // target voltage in 100 mV units (280, 360, 480)
    GroupResult r{};         // verdict, max_mv, detail, reason, fail_mv for this specific EPR level
    bool operator==(const EprLevel&) const = default;
};
inline constexpr unsigned kEprLevels = 3;

// The cable the run went through (v5): the saved/spoofed e-marker library entry when one was armed, else the
// physically read cable's identity. All-zero = no cable recorded (older formats).
struct Cable {
    uint32_t seq = 0;          // emlib entry number, 0 = none
    uint16_t vid = 0, pid = 0; // e-marker identity
    uint16_t max_mv = 0, max_ma = 0;   // limits the run used (mV, mA)
    uint8_t kind = 0;          // a built-in cable's emrec::builtin_info id (1 EPR 50V, 2 CUKTECH 6A, 4 OnePlus; 3 was "OnePlus mag", retired and recorded as 4), 0 = saved clone, 0xFF = real cable
    uint8_t flags = 0;         // bit0 observed, bit1 spoofed, bit2 the run was self-powered (the charger under test powered the K2),
                               // bits 3..5 the Ra guard's reading + 1 (0 = not judged), bit6 started anyway against a refusing reading
    bool operator==(const Cable&) const = default;
};
// Card::src_flags (cable block byte 15): the source listed OPPO's SVID 0x22D9 in its Discover SVIDs answer. A note only: the K2 sends
// that SVID nothing.
inline constexpr uint8_t kSrcOppoSvid = 1;
inline constexpr uint8_t kCableObserved = 1, kCableSpoofed = 2, kCableSelfPowered = 4, kCableKindReal = 0xFF;
// The Ra guard's reading when the run's identity was started (bits 3..5 hold raguard::Topology + 1, 0 = not judged, as
// every card before this field) and bit 6: the user started anyway against a refusing reading ("Cable on plug" warning).
inline constexpr uint8_t kCableGuardShift = 3, kCableGuardMask = 7 << kCableGuardShift, kCableGuardBypassed = 0x40;

// One UFCS mode of the charger's Output_Capabilities (v6): the voltage range and the mode's current limit. hi_dv == 0 marks an
// unused slot. Older cards hold no modes and show the UFCS volts alone.
struct UfcsRange {
    uint16_t lo_dv = 0, hi_dv = 0;   // 100 mV units (lo saturates at 25.5 V, hi at 51.1 V)
    uint16_t ma = 0;                 // the mode's maximum current, mA in 50 mA steps (saturates at 6.35 A)
    bool operator==(const UfcsRange&) const = default;
};

// Readable cause of a failed or skipped group: the engine's own code when the step stored one (pd::Err for PD steps
// stored as 128 + err, pd::SinkResult below that, fc::Fail for the fast-charge groups), else the step's reason.
inline bool group_handshake_ok(const GroupResult& r) { return r.verdict == GV::Pass && r.detail == uint8_t(fc::Fail::NoVoltage); }
inline bool group_detected(const GroupResult& r) { return r.verdict == GV::Pass && r.detail == uint8_t(fc::Fail::Detected); }
inline const char* group_why(unsigned group, const GroupResult& r) {
    // QC3+ the charger does not do (no movement, or QC3-sized steps): the engine code is "no response", the group says "no QC3+".
    if (r.reason == uint8_t(ct::Reason::NoQc3Plus)) return "no QC3+";
    if (r.detail) {
        if (group == unsigned(ct::Group::Pd)) {
            if (const char* epr = ct::epr_detail_name(r.detail)) return epr;   // an EPR skip: the monitor ceiling or Enter_Failed's reason
            if (r.detail >= 128 && unsigned(r.detail) - 128u <= unsigned(pd::Err::BadChoice)) return pd::err_name(pd::Err(r.detail - 128));
            if (r.detail <= unsigned(pd::SinkResult::BadChoice)) return pd::result_name(pd::SinkResult(r.detail));
        } else if (r.detail <= unsigned(fc::Fail::Detected)) {
            return fc::name(fc::Fail(r.detail));
        }
    }
    return ct::name(ct::Reason(r.reason));
}
struct Counts { unsigned pass = 0, fail = 0, skip = 0; };

struct Card {
    Identity id;
    uint8_t num_pdos = 0, spec_rev = 0, ev_flags = 0;
    std::array<GroupResult, kGroups> group{};
    uint8_t n_ranges = 0;
    uint16_t pd_ma = 0;      // highest current of any fixed PDO, mA in 50 mA steps (0 = unknown, older card)
    std::array<Range, kMaxRanges> range{};
    std::array<EprLevel, kEprLevels> epr{};   // per-EPR-level verdicts (v4 only; older cards decode to all GV::None)
    std::array<UfcsRange, kMaxUfcs> ufcs{};   // the charger's UFCS modes, planner order, unused slots last (v6 only; see ufcs_modes())
    Cable cable{};             // the cable of the run (v5 only; older cards decode to an empty cable)
    uint8_t resumed = 0;       // times the run lost power and resumed (cable block byte 14; 0 on older cards)
    uint8_t src_flags = 0;     // what the source's own messages showed (cable block byte 15; 0 on older cards): kSrcOppoSvid, bits 1..7 the SCP limit (scp_cap_ma)
    uint8_t format = kFormat;   // payload byte 33 this card was decoded from (kFormat when built): renderers gate v4/v5 fields on it
    ct::HandoverOutcome handover = ct::HandoverOutcome::NotTried;
    uint16_t handover_ds = 0;
    ct::State state = ct::State::Done;
    ct::Reason reason = ct::Reason::None;
    uint32_t fw = 0, boots = 0, uptime_s = 0;
    uint16_t card_no = 0, runs = 0;
    uint8_t times = 1;           // v7: runs that gave exactly these results (1 = once); boots / uptime_s are the first of them
    uint16_t last_boots = 0;     // v7: the newest of those runs (valid when times > 1)
    uint32_t last_uptime_s = 0;  // v7: its uptime, 24 bits on the card
    char name[kNameMax + 1] = {};
    bool name_auto = false;   // name came from auto_name(), not from the user
    Counts counts() const {
        Counts n;
        for (const auto& g : group) {
            n.pass += g.verdict == GV::Pass; n.fail += g.verdict == GV::Fail; n.skip += g.verdict == GV::Skip;
        }
        return n;
    }
    bool operator==(const Card& o) const {
        return id == o.id && num_pdos == o.num_pdos && spec_rev == o.spec_rev && ev_flags == o.ev_flags && group == o.group &&
               n_ranges == o.n_ranges && pd_ma == o.pd_ma && range == o.range && epr == o.epr && ufcs == o.ufcs && cable == o.cable && resumed == o.resumed && src_flags == o.src_flags && format == o.format &&
               handover == o.handover && handover_ds == o.handover_ds &&
               state == o.state && reason == o.reason && fw == o.fw && boots == o.boots && uptime_s == o.uptime_s &&
               card_no == o.card_no && runs == o.runs && times == o.times && last_boots == o.last_boots && last_uptime_s == o.last_uptime_s && std::memcmp(name, o.name, sizeof name) == 0 && name_auto == o.name_auto;
    }
};

// How many UFCS modes the card holds: the leading used slots of Card::ufcs. 0 for a card from before v6 (it never recorded
// them) and for a run whose UFCS rows were planned without the charger's Output_Capabilities.
constexpr unsigned ufcs_modes(const Card& c) {
    unsigned n = 0;
    if (c.format >= 6) while (n < kMaxUfcs && c.ufcs[n].hi_dv != 0) ++n;
    return n;
}

// The passive note for a source that listed OPPO's SVID (nothing is sent to it); nullptr when it did not.
// 33 characters: it fits one 6 px row of the results lines and the card's fact line.
constexpr const char* oppo_note(const Card& c) { return (c.src_flags & kSrcOppoSvid) ? "OPPO SVID 0x22D9 (discovery only)" : nullptr; }

// The SCP current limit the adapter reported to the SCP unit, in mA (src_flags bits 1..7, 50 mA steps; 0 = not read, or a card
// from before the bits meant anything). The voltage of the note is the highest SCP level that passed, which is the adapter's own
// top level whenever the planned top row passed; a failed or absent group names the current alone.
constexpr unsigned scp_cap_ma(const Card& c) { return c.format >= 5 ? unsigned(c.src_flags >> 1) * 50u : 0u; }
namespace detail {
// value (milli-units) as "6", "2.25" or "5.5": whole part, then the decimals that are not trailing zeros, at most `dec` of them.
inline size_t put_milli(char* out, size_t cap, unsigned v, unsigned dec) {
    size_t n = 0;
    auto put = [&](char ch) { if (n + 1 < cap) out[n++] = ch; };
    char whole[8]; unsigned w = v / 1000, k = 0;
    do { whole[k++] = char('0' + w % 10); w /= 10; } while (w && k < sizeof whole);
    while (k) put(whole[--k]);
    unsigned frac = v % 1000, scale = 100;
    char digits[3]; unsigned nd = 0;
    for (unsigned i = 0; i < dec && i < 3; ++i, scale /= 10) { digits[nd++] = char('0' + frac / scale % 10); }
    while (nd && digits[nd - 1] == '0') --nd;
    if (nd) { put('.'); for (unsigned i = 0; i < nd; ++i) put(digits[i]); }
    out[n] = 0;
    return n;
}
}  // namespace detail
// "SCP cap 20V 6A" (or "SCP cap 6A"): the adapter's own readout, passive. false when the card holds no current.
inline bool scp_note(const Card& c, char* out, size_t cap) {
    if (cap == 0) return false;
    out[0] = 0;
    const unsigned ma = scp_cap_ma(c);
    if (ma == 0) return false;
    size_t n = 0;
    auto put = [&](const char* s) { while (*s && n + 1 < cap) out[n++] = *s++; };
    put("SCP cap ");
    const GroupResult& g = c.group[unsigned(ct::Group::Scp)];
    if (g.verdict == GV::Pass && g.max_mv >= 1000) {
        n += detail::put_milli(out + n, cap - n, g.max_mv, 1); put("V ");
    }
    n += detail::put_milli(out + n, cap - n, ma, 2); put("A");
    out[n] = 0;
    return true;
}

// Quick Charge 4+/5 label of a source, from facts the card already holds (passive: no probe, and never a key for any
// behaviour). A Qualcomm USB vendor ID (Discover Identity or Source_Capabilities_Extended) together with a PPS APDO is
// "QC4+"; a rated power of 100 W or more, or an EPR AVS range, makes it "QC5". Anything else carries no label.
//
// The same passive reading names two more OEM modes whose wire is plain PD PPS (inferred, never a behaviour key):
//   Moto TurboPower: a Motorola (0x22B8) or Lenovo (0x17EF) vendor ID plus a PPS APDO advertising more than 5 A (20 V 6.25 A).
//   Samsung SFC 3.0: a PPS APDO reaching 20 V at 3 A or more.
//   Samsung SFC 2.0: PPS to 15 V at 3 A or more, or PPS covering 3.3-11 V at 4.05 A or more behind a 5 A cable.
// Samsung's modes carry no vendor ID, so they rest on the capability alone; a Qualcomm ID keeps the QC label.
enum class QcLabel : uint8_t { None, Qc4Plus, Qc5, MotoTurbo, Sfc2, Sfc3 };
inline constexpr uint16_t kQualcommVid = 0x05C6;
inline constexpr uint16_t kMotorolaVid = 0x22B8;
inline constexpr uint16_t kLenovoVid = 0x17EF;
inline constexpr unsigned kQc5MinWatts = 100;
inline constexpr unsigned kMotoMinMa = 5000;      // a PPS APDO above this is TurboPower class
inline constexpr unsigned kSfc2MaxCapMa = 4050;   // SFC 2.0 at 3.3-11 V

inline QcLabel qc_label(const Card& c) {
    bool pps = false, epr = false, over5 = false, sfc3 = false, sfc2_15 = false, sfc2_11 = false;
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        const Range& r = c.range[i];
        epr = epr || r.alg == uint8_t(ct::Algorithm::EprAvs);
        if (r.alg != uint8_t(ct::Algorithm::Pps)) continue;
        pps = true;
        over5 = over5 || r.ma > kMotoMinMa;
        sfc3 = sfc3 || (r.hi_dv >= 200 && r.ma >= 3000);
        sfc2_15 = sfc2_15 || (r.hi_dv >= 150 && r.ma >= 3000);
        sfc2_11 = sfc2_11 || (r.lo_dv && r.lo_dv <= 33 && r.hi_dv >= 110 && r.ma >= kSfc2MaxCapMa);
    }
    if (!pps) return QcLabel::None;
    if (c.id.vid == kQualcommVid) return c.id.pdp_w >= kQc5MinWatts || epr ? QcLabel::Qc5 : QcLabel::Qc4Plus;
    if ((c.id.vid == kMotorolaVid || c.id.vid == kLenovoVid) && over5) return QcLabel::MotoTurbo;
    if (sfc3) return QcLabel::Sfc3;
    if (sfc2_15 || (sfc2_11 && c.cable.max_ma >= 5000)) return QcLabel::Sfc2;
    return QcLabel::None;
}
inline const char* qc_label_name(QcLabel l) {
    switch (l) {
    case QcLabel::Qc5: return "QC5";
    case QcLabel::Qc4Plus: return "QC4+";
    case QcLabel::MotoTurbo: return "Moto TurboPower";
    case QcLabel::Sfc2: return "Samsung SFC 2.0";
    case QcLabel::Sfc3: return "Samsung SFC 3.0";
    default: return "";
    }
}

// The PPS span the label rests on, "3.3-21V": the lowest minimum to the highest maximum of the card's PPS ranges.
// Returns the length written (0 and an empty string when the card has no PPS range).
inline size_t qc_pps_span_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    unsigned lo = 0xFFFF, hi = 0;
    bool any = false;
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        if (c.range[i].alg != uint8_t(ct::Algorithm::Pps)) continue;
        any = true;
        lo = c.range[i].lo_dv < lo ? c.range[i].lo_dv : lo;
        hi = c.range[i].hi_dv > hi ? c.range[i].hi_dv : hi;
    }
    if (!any) return 0;
    size_t n = 0;
    auto put = [&](const char* s) { while (*s && n + 1 < cap) out[n++] = *s++; };
    auto put_dv = [&](unsigned dv) {   // 100 mV units: 33 -> "3.3", 210 -> "21"
        char b[8]; unsigned k = 0;
        if (dv >= 100) b[k++] = char('0' + dv / 100 % 10);
        b[k++] = char('0' + dv / 10 % 10);
        if (dv % 10) { b[k++] = '.'; b[k++] = char('0' + dv % 10); }
        b[k] = 0;
        put(b);
    };
    put_dv(lo); put("-"); put_dv(hi); put("V");
    out[n] = 0;
    return n;
}

// The PPS V/A span, e.g. "5-20V 5A": the lowest minimum to the highest maximum voltage of the card's PPS ranges,
// plus the maximum advertised current of those ranges. Returns the length written (0 and an empty string when
// the card has no PPS range or no current info).
inline size_t qc_pps_va_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    unsigned lo = 0xFFFF, hi = 0;
    uint16_t max_ma = 0;
    bool any = false;
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        if (c.range[i].alg != uint8_t(ct::Algorithm::Pps)) continue;
        any = true;
        if (c.range[i].lo_dv < lo) lo = c.range[i].lo_dv;
        if (c.range[i].hi_dv > hi) hi = c.range[i].hi_dv;
        if (c.range[i].ma > max_ma) max_ma = c.range[i].ma;
    }
    if (!any) return 0;
    size_t n = 0;
    auto put = [&](const char* s) { while (*s && n + 1 < cap) out[n++] = *s++; };
    auto put_dv = [&](unsigned dv) {   // 100 mV units: 33 -> "3.3", 210 -> "21"
        char b[8]; unsigned k = 0;
        if (dv >= 100) b[k++] = char('0' + dv / 100 % 10);
        b[k++] = char('0' + dv / 10 % 10);
        if (dv % 10) { b[k++] = '.'; b[k++] = char('0' + dv % 10); }
        b[k] = 0;
        put(b);
    };
    put_dv(lo); put("-"); put_dv(hi);
    // Append current in A: max_ma is in mA
    if (max_ma > 0) {
        put("V ");
        const uint16_t whole_a = max_ma / 1000;
        const uint16_t frac_ma = max_ma % 1000; // 0..999, multiple of 50
        char b[8]; unsigned k = 0;
        if (whole_a >= 10) b[k++] = char('0' + whole_a / 10);
        b[k++] = char('0' + whole_a % 10);
        if (frac_ma > 0) {
            b[k++] = '.';
            b[k++] = char('0' + frac_ma / 100);
            b[k++] = char('0' + (frac_ma % 100) / 10);
        }
        b[k++] = 'A'; b[k] = 0;
        put(b);
    } else {
        put("V");
    }
    out[n] = 0;
    return n;
}

// The highest-wattage fixed PDO V/A text, e.g. "20V 5A". Uses the highest voltage measured on fixed PDOs
// (group[pd].max_mv) and the highest advertised current of any fixed PDO (pd_ma). Returns the length written
// (0 and an empty string when the card has no fixed PDO current info or no measured voltage).
inline size_t qc_pd_va_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    if (c.pd_ma == 0) return 0; // unknown current
    const uint16_t volts_mv = c.group[unsigned(ct::Group::Pd)].max_mv; // in mV
    if (volts_mv == 0) return 0;
    const uint16_t volts = volts_mv / 1000; // convert to V (integer)
    const uint16_t amps_ma = c.pd_ma; // in mA
    const uint16_t whole_a = amps_ma / 1000;
    const uint16_t frac_ma = amps_ma % 1000;
    size_t n = 0;
    auto put = [&](const char* s) { while (*s && n + 1 < cap) out[n++] = *s++; };
    char b[16]; unsigned k = 0;
    if (volts >= 10) b[k++] = char('0' + volts / 10);
    b[k++] = char('0' + volts % 10);
    b[k++] = 'V'; b[k++] = ' ';
    if (whole_a >= 10) b[k++] = char('0' + whole_a / 10);
    b[k++] = char('0' + whole_a % 10);
    if (frac_ma > 0) {
        b[k++] = '.';
        b[k++] = char('0' + frac_ma / 100);
        b[k++] = char('0' + (frac_ma % 100) / 10);
    }
    b[k++] = 'A'; b[k] = 0;
    put(b);
    out[n] = 0;
    return n;
}

// The justification part of the QC label, e.g. "via PPS 5-20V 5A" or "via PD 20V 5A".
// Returns the length written (0 and an empty string when the card has no label).
inline size_t qc_label_justification(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    const QcLabel l = qc_label(c);
    if (l == QcLabel::None) return 0;
    size_t n = 0;
    auto put = [&](const char* s) { while (*s && n + 1 < cap) out[n++] = *s++; };
    if (l == QcLabel::Qc5) {
        char pd_va[20];
        if (qc_pd_va_text(c, pd_va, sizeof pd_va)) {
            put("via PD "); put(pd_va);
        } else {
            // Fallback to PPS span when PD info is missing
            char span[16];
            if (qc_pps_span_text(c, span, sizeof span)) {
                put("Qualcomm VID, PPS "); put(span);
            }
        }
    } else { // Qc4Plus, Moto TurboPower, Samsung SFC: all read off the PPS range
        char pps_va[20];
        if (qc_pps_va_text(c, pps_va, sizeof pps_va)) {
            put("via PPS "); put(pps_va);
        } else {
            // Fallback to PPS span
            char span[16];
            if (qc_pps_span_text(c, span, sizeof span)) {
                put(l == QcLabel::Qc4Plus ? "Qualcomm VID, PPS " : "PPS "); put(span);
            }
        }
    }
    out[n] = 0;
    return n;
}

// The fact line behind the label, e.g. "QC5 (via PD 20V 5A)" or "QC4+ (via PPS 5-20V 5A)".
// Returns the length written (0 and an empty string when the card has no label).
inline size_t qc_label_text(const Card& c, char* out, size_t cap) {
    if (cap == 0) return 0;
    out[0] = 0;
    const QcLabel l = qc_label(c);
    if (l == QcLabel::None) return 0;
    size_t n = 0;
    auto put = [&](const char* s) { while (*s && n + 1 < cap) out[n++] = *s++; };
    put(qc_label_name(l));
    char justification[32];
    if (qc_label_justification(c, justification, sizeof justification)) {
        put(" ("); put(justification); put(")");
    }
    out[n] = 0;
    return n;
}

constexpr uint32_t fingerprint(std::span<const uint32_t> pdos, uint8_t spec_rev) {
    uint32_t h = 2166136261u;
    auto put = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
    for (uint32_t p : pdos) for (unsigned i = 0; i < 4; ++i) put(uint8_t(p >> (8 * i)));
    put(spec_rev);
    return h ? h : 1;
}

// Identity of a source that advertised no PD caps (a bank on its legacy port): FNV-1a 32 over, for every passing
// step, the group and the level in 100 mV, behind a salt that no PDO fingerprint uses. The same charger keeps the
// same card while its answers stay the same; a different set of answered protocols is another charger.
constexpr uint32_t legacy_fingerprint(std::span<const ct::Step> steps) {
    uint32_t h = 2166136261u;
    auto put = [&h](uint8_t b) { h = (h ^ b) * 16777619u; };
    put('L'); put('G'); put('C'); put('Y');
    for (const auto& s : steps)
        if (s.verdict == ct::Verdict::Pass) { put(uint8_t(s.group)); put(uint8_t(s.mv / 100)); put(uint8_t(s.mv / 25600)); }
    return h ? h : 1;
}

// Firmware build id for the card: the commit token of the build's describe string (hex, first 8 digits,
// a leading "g" stripped); 0 when the text has none.
constexpr uint32_t fw_hash(std::string_view text) {
    auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    size_t end = text.size();
    while (end > 0) {
        size_t begin = end;
        while (begin > 0 && text[begin - 1] != '-') --begin;
        std::string_view tok = std::string_view(text.data() + begin, end - begin);   // no substr: it can throw
        if (tok.size() >= 8 && tok.front() == 'g' && hex(tok[1]) >= 0) tok.remove_prefix(1);
        bool ok = tok.size() >= 7;
        for (char c : tok) ok = ok && hex(c) >= 0;
        if (ok) {
            uint32_t v = 0;
            for (size_t i = 0; i < tok.size() && i < 8; ++i) v = v << 4 | uint32_t(hex(tok[i]));
            return v;
        }
        end = begin ? begin - 1 : 0;
    }
    return 0;
}

constexpr bool valid_name(std::string_view n) { return emlib::name_valid(n) && n.size() <= kNameMax; }
inline void set_name(Card& c, std::string_view n) {
    c.name[0] = 0;
    const size_t k = n.size() < kNameMax ? n.size() : kNameMax;
    std::memcpy(c.name, n.data(), k);
    c.name[k] = 0;
}

// Brand of a USB-IF vendor ID, nullptr when it is not in the (short) table. Every entry is checked against the
// linux-usb.org usb.ids list except Anker, which is not in that list; unverified IDs are left out.
// Honor 0x339B, vivo 0x2D95 and Zimi/CUKTECH 0x2B01 are not in that list (checked against usb.ids 2026.06.26), so they stay out.
constexpr const char* brand(uint16_t vid) {
    switch (vid) {
        case 0x05AC: return "Apple";
        case 0x04E8: return "Samsung";
        case 0x18D1: return "Google";
        case 0x2717: return "Xiaomi";
        case 0x12D1: return "Huawei";
        case 0x17EF: return "Lenovo";
        case 0x413C: return "Dell";
        case 0x03F0: return "HP";
        case 0x2A70: return "OnePlus";
        case 0x22D9: return "OPPO";
        case 0x0B05: return "ASUS";
        case 0x050D: return "Belkin";
        case 0x22B8: return "Motorola";
        case 0x19D2: return "ZTE";
        case 0x291A: return "Anker";
        default: return nullptr;
    }
}

// "<Brand> <PDP>W" for the card name, written to out (kNameMax + 1 bytes) and returned as its length (0 = nothing known).
// The brand is the VID's, else the manufacturer text (only the name alphabet, trimmed), else "VID xxxx"; the
// wattage suffix is left out when pdp_w is 0. The base is cut so the whole name stays within kNameMax.
inline size_t auto_name(char* out, uint16_t vid, std::string_view mfr, uint8_t pdp_w) {
    if (vid == 0xFFFF) vid = 0;   // placeholder VID: never a brand or a "VID FFFF" name
    char base[kNameMax + 1] = {};
    size_t n = 0;
    if (const char* b = brand(vid)) { n = std::strlen(b); std::memcpy(base, b, n); }
    else {
        for (char c : mfr) {
            if (n >= kNameMax) break;
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            if (ok || (c == ' ' && n > 0)) base[n++] = c;
        }
        while (n && base[n - 1] == ' ') base[--n] = 0;
        if (n == 0 && vid) {
            static constexpr char hx[] = "0123456789ABCDEF";
            std::memcpy(base, "VID ", 4);
            for (unsigned i = 0; i < 4; ++i) base[4 + i] = hx[(vid >> (12 - 4 * i)) & 15];
            n = 8;
        }
    }
    char suffix[6] = {};
    size_t sn = 0;
    if (n && pdp_w) {
        suffix[sn++] = ' ';
        if (pdp_w >= 100) suffix[sn++] = char('0' + pdp_w / 100);
        if (pdp_w >= 10) suffix[sn++] = char('0' + pdp_w / 10 % 10);
        suffix[sn++] = char('0' + pdp_w % 10);
        suffix[sn++] = 'W';
    }
    if (n + sn > kNameMax) n = kNameMax - sn;
    while (n && base[n - 1] == ' ') --n;
    std::memcpy(out, base, n);
    std::memcpy(out + n, suffix, sn);
    out[n + sn] = 0;
    return n ? n + sn : 0;
}

// Gives an unnamed card its automatic name from the identity it holds (+ the manufacturer text, which is not stored).
inline void set_auto_name(Card& c, std::string_view mfr) {
    char buf[kNameMax + 1] = {};
    if (auto_name(buf, c.id.vid, mfr, c.id.pdp_w) == 0) return;
    set_name(c, buf);
    c.name_auto = true;
}

// Carries what an earlier card of the same charger knew into the new one: a user name always wins over an automatic
// one, an automatic name is replaced by a newer automatic name, and an identity the run did not learn is kept.
inline void inherit(Card& c, const Card& prev) {
    if (prev.name[0] && !prev.name_auto) { std::memcpy(c.name, prev.name, sizeof c.name); c.name_auto = false; }
    else if (c.name[0] == 0) { std::memcpy(c.name, prev.name, sizeof c.name); c.name_auto = prev.name_auto; }
    if (c.id.vid == 0 && c.id.pid == 0) { c.id.vid = prev.id.vid; c.id.pid = prev.id.pid; }
    if (c.id.pdp_w == 0) c.id.pdp_w = prev.id.pdp_w;
}

namespace detail {
constexpr bool within(unsigned a, unsigned b, unsigned step) { return (a > b ? a - b : b - a) <= step; }
// One stored result (a group's or an EPR level's): everything but the measured maximum must match, the maximum within one step.
constexpr bool same_result(const GroupResult& a, const GroupResult& b) {
    return a.verdict == b.verdict && a.reason == b.reason && a.detail == b.detail && a.fail_mv == b.fail_mv && within(a.max_mv, b.max_mv, 100);
}
}  // namespace detail

// True when `run` gave the same results as the stored card `prev`: the rule written at the top of this file. Pure, symmetric
// except for the one-step tolerance, which is the same both ways.
inline bool same_results(const Card& prev, const Card& run) {
    if (prev.id.fp != run.id.fp || prev.num_pdos != run.num_pdos || prev.spec_rev != run.spec_rev || prev.ev_flags != run.ev_flags ||
        prev.pd_ma != run.pd_ma || prev.src_flags != run.src_flags || prev.fw != run.fw || prev.handover != run.handover ||
        prev.state != run.state || prev.reason != run.reason || prev.n_ranges != run.n_ranges || prev.ufcs != run.ufcs)
        return false;
    const Cable &a = prev.cable, &b = run.cable;
    if (a.vid != b.vid || a.pid != b.pid || a.max_mv != b.max_mv || a.max_ma != b.max_ma || a.kind != b.kind) return false;
    for (unsigned g = 0; g < kGroups; ++g) if (!detail::same_result(prev.group[g], run.group[g])) return false;
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        const Range &x = prev.range[i], &y = run.range[i];
        if (x.alg != y.alg || x.pos != y.pos || x.lo_dv != y.lo_dv || x.hi_dv != y.hi_dv || x.ma != y.ma || x.fail_dv != y.fail_dv || !detail::within(x.max_dv, y.max_dv, 1))
            return false;
    }
    for (unsigned i = 0; i < kEprLevels; ++i)
        if (prev.epr[i].dv != run.epr[i].dv || !detail::same_result(prev.epr[i].r, run.epr[i].r)) return false;
    return true;
}

// The card that results from a run that repeated the stored card `prev` (same_results): the stored rows and first-run time stay,
// the run counts, the repeat counter grows and last_* name the newest run. What only the repeat learned (an identity the first
// probe missed, an automatic name where the stored card had none) is added; a user name always stays.
inline Card repeat_of(const Card& prev, const Card& run) {
    Card m = prev;
    m.format = kFormat;
    m.runs = prev.runs == 0xFFFF ? prev.runs : uint16_t(prev.runs + 1);
    m.times = prev.times == 0xFF ? prev.times : uint8_t(prev.times + 1);
    m.last_boots = run.boots > 0xFFFF ? uint16_t(0xFFFF) : uint16_t(run.boots);
    m.last_uptime_s = run.uptime_s > 0xFFFFFF ? 0xFFFFFFu : run.uptime_s;
    if (!(m.name[0] && !m.name_auto) && run.name[0]) { std::memcpy(m.name, run.name, sizeof m.name); m.name_auto = run.name_auto; }
    if (m.id.vid == 0 && m.id.pid == 0) { m.id.vid = run.id.vid; m.id.pid = run.id.pid; }
    if (m.id.pdp_w == 0) m.id.pdp_w = run.id.pdp_w;
    return m;
}

namespace detail {
constexpr uint16_t dv_round(unsigned mv) { return uint16_t((mv + 50) / 100); }
constexpr void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
constexpr void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
constexpr uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
constexpr uint32_t get32(const uint8_t* p) { return get16(p) | (uint32_t(get16(p + 2)) << 16); }
}  // namespace detail

namespace detail {
// Advertised current of a PPS / AVS APDO in mA, 50 mA steps, saturating at the 7 bit field (6.35 A).
constexpr uint16_t apdo_ma(ct::Algorithm alg, uint32_t raw, unsigned hi_dv) {
    unsigned ma = 0;
    if (alg == ct::Algorithm::Pps) ma = (raw & 0x7Fu) * 50u;
    else if (alg == ct::Algorithm::SprAvs) ma = std::max((raw >> 10) & 0x3FFu, raw & 0x3FFu) * 10u;
    else if (alg == ct::Algorithm::EprAvs && hi_dv) ma = (raw & 0xFFu) * 10000u / hi_dv;   // PDP watts over the top voltage
    ma = ma / 50 * 50;
    return uint16_t(ma > 6350 ? 6350 : ma);
}
}  // namespace detail

// Convert a card decoded from an older format (or held in memory before this firmware) to the current format:
// the format bit is the migration marker — fields the old format did not carry stay zeroed (e.g. the cable).
inline void migrate_to_current(Card& c) { if (c.format < kFormat) c.format = kFormat; }

inline Card build(std::span<const ct::Step> steps, const ct::Evidence& ev, ct::HandoverInfo h, Identity id, Context cx,
                  ct::State state, ct::Reason reason, const Cable& cable = {}, uint8_t resumed = 0) {
    Card c;
    c.id = id; c.num_pdos = ev.num_pdos; c.spec_rev = ev.wire_rev; c.ev_flags = ev.flags; c.cable = cable; c.resumed = resumed;
    c.src_flags = uint8_t(((ev.seen & ct::SeenOppoSvid) ? kSrcOppoSvid : 0) | (std::min<unsigned>(ev.scp_ma / 50u, 127u) << 1));
    c.handover = h.outcome; c.handover_ds = h.ms / 100 > 65535 ? uint16_t(65535) : uint16_t(h.ms / 100);
    c.state = state; c.reason = reason; c.fw = cx.fw; c.boots = cx.boots; c.uptime_s = cx.uptime_s;
    std::array<unsigned, kGroups> passes{}, fails{}, skips{};
    std::array<bool, kGroups> have_fail{}, have_skip{}, no_response{};   // no_response: a QC3+ skip "no QC3+ response" (the 5 V baseline passed)
    std::array<GroupResult, kGroups> first_fail{}, first_skip{};
    std::array<bool, kGroups> proven{};           // a pass with real evidence (a voltage reached) outranks detected
    std::array<uint8_t, kGroups> pass_detail{};   // fc::Fail::Detected: a pass that only proved the adapter answered
    for (const auto& s : steps) {
        const unsigned g = unsigned(s.group);
        if (g >= kGroups) continue;
        const bool range_step = (ct::kPpsAlgorithms & ct::bit(s.algorithm)) != 0;
        // The failing target belongs to a fixed step; a range step reports its own fail_dv.
        const GroupResult why{GV::None, 0, s.detail, uint8_t(s.reason), range_step ? uint16_t(0) : uint16_t(detail::dv_round(s.mv) * 100u)};
        if (s.verdict == ct::Verdict::Pass) {
            ++passes[g];
            if (s.detail != uint8_t(fc::Fail::Detected) && s.detail != uint8_t(fc::Fail::NoVoltage)) proven[g] = true;
            else if (s.detail == uint8_t(fc::Fail::NoVoltage)) pass_detail[g] = s.detail;   // the stronger result wins
            else if (pass_detail[g] == 0) pass_detail[g] = s.detail;
            // The group max is the fixed-supply maximum (100 mV); a range step is reported by its own range.
            if (!range_step && detail::dv_round(s.measured_mv) * 100u > c.group[g].max_mv) c.group[g].max_mv = uint16_t(detail::dv_round(s.measured_mv) * 100u);
        } else if (s.verdict == ct::Verdict::Fail) {
            ++fails[g];
            // The first failing fixed step names the group; a range-only failure is the fallback.
            if (!have_fail[g] || (!range_step && first_fail[g].fail_mv == 0)) { have_fail[g] = true; first_fail[g] = why; }
        } else if (s.verdict == ct::Verdict::Skip) {
            ++skips[g]; if (!have_skip[g]) { have_skip[g] = true; first_skip[g] = why; }
            if (s.reason == ct::Reason::NoQc3Plus) no_response[g] = true;
        }
        // UFCS rows name the mode they sample: one entry per distinct mode, the planner's order (a source lists up to 7, the card keeps 4).
        if (ct::UfcsMode m; ct::ufcs_mode_of(s, m) && m.max_mv != 0) {
            const UfcsRange r{uint16_t(std::min<unsigned>(m.min_mv / 100u, 255u)), uint16_t(std::min<unsigned>(m.max_mv / 100u, 511u)),
                              uint16_t(std::min<unsigned>(m.max_ma / 50u, 127u) * 50u)};
            unsigned n = 0;
            while (n < kMaxUfcs && c.ufcs[n].hi_dv != 0 && !(c.ufcs[n] == r)) ++n;
            if (n < kMaxUfcs && c.ufcs[n].hi_dv == 0) c.ufcs[n] = r;
        }
        // Fixed PDOs (type bits 00): the highest advertised current of any of them.
        if (g == unsigned(ct::Group::Pd) && !range_step && s.pdo && (s.pdo >> 30) == 0) {
            const unsigned ma = std::min<unsigned>((s.pdo & 0x3FFu) * 10u / 50u * 50u, 6350u);
            if (ma > c.pd_ma) c.pd_ma = uint16_t(ma);
        }
        // Continuous (range) steps: one entry per algorithm and PDO; the PDO word carries the offered limits.
        if (s.continuous && s.pdo && (ct::kPpsAlgorithms & ct::bit(s.algorithm))) {
            // PPS and EPR AVS carry min V in 15:8 and max V from bit 17 (100 mV). The PD 3.2 SPR AVS APDO has no
            // voltage fields: it starts at 9 V and ends at 20 V when its 20 V current (9:0) is set, else at 15 V.
            const bool spr_avs = s.algorithm == ct::Algorithm::SprAvs;
            const uint16_t lo = spr_avs ? uint16_t(90) : uint16_t((s.pdo >> 8) & 0xFF);
            const uint16_t hi = spr_avs ? uint16_t((s.pdo & 0x3FFu) ? 200 : 150)
                                        : uint16_t((s.pdo >> 17) & (s.algorithm == ct::Algorithm::EprAvs ? 0x1FFu : 0xFFu));
            Range* rg = nullptr;
            for (unsigned i = 0; i < c.n_ranges; ++i) if (c.range[i].alg == uint8_t(s.algorithm) && c.range[i].pos == s.pos) rg = &c.range[i];
            if (!rg && c.n_ranges < kMaxRanges) {
                rg = &c.range[c.n_ranges++];
                rg->alg = uint8_t(s.algorithm); rg->pos = s.pos; rg->lo_dv = lo; rg->hi_dv = hi;
            }
            if (rg && !rg->ma) rg->ma = detail::apdo_ma(s.algorithm, s.pdo, rg->hi_dv);
            // max_dv = the highest level the output reached: a passed step's, or the ceiling a Capped step measured (the source
            // accepted the level but stopped below it), so "max 19.4 V of 21 V" survives in the card without a format change.
            if (rg && (s.verdict == ct::Verdict::Pass || s.reason == ct::Reason::Capped)) { const uint16_t m = detail::dv_round(s.measured_mv); if (m > rg->max_dv) rg->max_dv = m; }
            if (rg && s.verdict == ct::Verdict::Fail && rg->fail_dv == 0) rg->fail_dv = detail::dv_round(s.mv);
        }
    }
    // V4: collect per-EPR-level verdicts for Algorithm::Epr steps in group Pd
    // EPR fixed supplies are at most 3: 28 V, 36 V, 48 V
    for (const auto& s : steps) {
        if (s.group == ct::Group::Pd && s.algorithm == ct::Algorithm::Epr && !s.continuous) {
            const uint16_t target_dv = uint16_t(detail::dv_round(s.mv));   // 100 mV units: 280, 360, 480
            // Find the matching EPR level entry by target voltage
            for (unsigned i = 0; i < kEprLevels; ++i) {
                if (c.epr[i].dv == target_dv) {
                    // Already have an entry for this voltage: update with first fail, then pass
                    if (s.verdict == ct::Verdict::Fail && c.epr[i].r.verdict != GV::Fail) {
                        c.epr[i].r = {GV::Fail, 0, s.detail, uint8_t(s.reason), uint16_t(detail::dv_round(s.mv) * 100u)};
                    } else if (s.verdict == ct::Verdict::Pass) {
                        if (detail::dv_round(s.measured_mv) * 100u > c.epr[i].r.max_mv) {
                            c.epr[i].r.max_mv = uint16_t(detail::dv_round(s.measured_mv) * 100u);
                        }
                        if (c.epr[i].r.verdict == GV::None) c.epr[i].r.verdict = GV::Pass;
                    } else if (s.verdict == ct::Verdict::Skip && c.epr[i].r.verdict == GV::None) {
                        c.epr[i].r = {GV::Skip, 0, s.detail, uint8_t(s.reason), 0};
                    }
                    break;
                }
                if (c.epr[i].dv == 0) {
                    // Empty slot: claim it
                    c.epr[i].dv = target_dv;
                    if (s.verdict == ct::Verdict::Fail) {
                        c.epr[i].r = {GV::Fail, 0, s.detail, uint8_t(s.reason), uint16_t(detail::dv_round(s.mv) * 100u)};
                    } else if (s.verdict == ct::Verdict::Pass) {
                        c.epr[i].r = {GV::Pass, uint16_t(detail::dv_round(s.measured_mv) * 100u), 0, uint8_t(s.reason), 0};
                    } else if (s.verdict == ct::Verdict::Skip) {
                        c.epr[i].r = {GV::Skip, 0, s.detail, uint8_t(s.reason), 0};
                    }
                    break;
                }
            }
        }
    }
    for (unsigned g = 0; g < kGroups; ++g) {
        GroupResult& r = c.group[g];
        r.verdict = fails[g] ? GV::Fail : no_response[g] ? GV::Skip : passes[g] ? GV::Pass : skips[g] ? GV::Skip : GV::None;
        if (r.verdict == GV::Fail) { r.detail = first_fail[g].detail; r.reason = first_fail[g].reason; r.fail_mv = first_fail[g].fail_mv; }
        else if (r.verdict == GV::Pass) r.detail = proven[g] ? uint8_t(0) : pass_detail[g];
        else if (r.verdict == GV::Skip) { r.detail = first_skip[g].detail; r.reason = first_skip[g].reason; }
    }
    return c;
}

// ---- codec -------------------------------------------------------------------------------------------------
namespace off {
inline constexpr unsigned kName = 34, kNameBytes = 22;
inline constexpr unsigned kPdp = kName + kNameMax, kFlags = kPdp + 1;   // 54, 55: the two bytes after the name characters
inline constexpr unsigned kGroups = kName + kNameBytes;                  // 56
inline constexpr unsigned kRanges = kGroups + 4 * report_card::kGroups;  // 104
inline constexpr unsigned kEpr = kRanges + 10 * kMaxRanges;              // 144: start of EPR levels
inline constexpr unsigned kCable = kEpr + 6 * kEprLevels;                // 162: v5 cable block, 16 bytes
inline constexpr unsigned kUfcs = kCable + 16;                           // 178: v6 UFCS modes, kMaxUfcs x 3 bytes
inline constexpr unsigned kRepeat = kUfcs + 3 * kMaxUfcs;                // 190: v7 repeat block, 6 bytes
}  // namespace off
inline constexpr unsigned kPayload = off::kRepeat + 6;                   // 196 (190 + 6 for the v7 repeat block)
inline constexpr uint8_t kFlagNameAuto = 1;
inline constexpr unsigned kRecordOverhead = 6, kMaxRecord = kRecordOverhead + kPayload;
static_assert(kPayload % 2 == 0 && kPayload <= 250);

namespace detail {
// Group voltages are kept in 100 mV and saturate at 51.1 V (9 bits); the detail needs 6 bits (see the layout above).
constexpr uint32_t group_dv(unsigned mv) { const unsigned d = dv_round(mv > 65000 ? 65000 : mv); return d > 511 ? 511 : d; }
constexpr uint32_t pack_detail(uint8_t d) { return d >= 128 ? (0x20u | ((d - 128u) & 0x1Fu)) : (d & 0x1Fu); }
constexpr uint8_t unpack_detail(uint32_t d) { return uint8_t(d & 0x20 ? 128 + (d & 0x1F) : d & 0x1F); }
}  // namespace detail

// Frames the card as a record into out (>= kMaxRecord bytes); returns the record size.
inline size_t encode_card(std::span<uint8_t> out, const Card& c) {
    using namespace detail;
    uint8_t* b = out.data();
    std::memset(b, 0, kMaxRecord);
    b[0] = kMagic; b[1] = kTypeCard; b[2] = kPayload; b[3] = 0;
    uint8_t* p = b + 4;
    put32(p + 0, c.id.fp); put16(p + 4, c.id.vid); put16(p + 6, c.id.pid);
    put16(p + 8, c.card_no); put16(p + 10, c.runs);
    put32(p + 12, c.boots); put32(p + 16, c.uptime_s); put32(p + 20, c.fw);
    put16(p + 24, c.handover_ds);
    p[26] = c.spec_rev; p[27] = c.num_pdos; p[28] = uint8_t(c.handover); p[29] = uint8_t(c.state); p[30] = uint8_t(c.reason);
    p[31] = c.n_ranges; p[32] = c.ev_flags; p[33] = kFormat;
    std::memcpy(p + off::kName, c.name, kNameMax);
    p[off::kPdp] = c.id.pdp_w; p[off::kFlags] = uint8_t((c.name_auto && c.name[0] ? kFlagNameAuto : 0) | (c.pd_ma / 50 > 127 ? 127 : c.pd_ma / 50) << 1);
    for (unsigned g = 0; g < kGroups; ++g) {
        uint8_t* q = p + off::kGroups + 4 * g;
        const uint32_t v = group_dv(c.group[g].max_mv) | group_dv(c.group[g].fail_mv) << 9 | pack_detail(c.group[g].detail) << 18;
        q[0] = uint8_t(unsigned(c.group[g].verdict) << 6 | (c.group[g].reason & 0x3F));
        q[1] = uint8_t(v); q[2] = uint8_t(v >> 8); q[3] = uint8_t(v >> 16);
    }
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        uint8_t* q = p + off::kRanges + 10 * i;
        q[0] = c.range[i].alg; q[1] = c.range[i].pos;
        put16(q + 2, uint16_t((c.range[i].lo_dv & 0x1FF) | (c.range[i].ma / 50 > 127 ? 127 : c.range[i].ma / 50) << 9)); put16(q + 4, c.range[i].hi_dv); put16(q + 6, c.range[i].max_dv); put16(q + 8, c.range[i].fail_dv);
    }
    // V4: EPR levels (3 entries of 6 bytes each: u16 dv + 4-byte group packing)
    for (unsigned i = 0; i < kEprLevels; ++i) {
        uint8_t* q = p + off::kEpr + 6 * i;
        put16(q, c.epr[i].dv);
        const uint32_t v = group_dv(c.epr[i].r.max_mv) | group_dv(c.epr[i].r.fail_mv) << 9 | pack_detail(c.epr[i].r.detail) << 18;
        q[2] = uint8_t(unsigned(c.epr[i].r.verdict) << 6 | (c.epr[i].r.reason & 0x3F));
        q[3] = uint8_t(v); q[4] = uint8_t(v >> 8); q[5] = uint8_t(v >> 16);
    }
    // V5: cable block (16 bytes)
    {
        uint8_t* q = p + off::kCable;
        put32(q, c.cable.seq); put16(q + 4, c.cable.vid); put16(q + 6, c.cable.pid);
        put16(q + 8, c.cable.max_mv); put16(q + 10, c.cable.max_ma);
        q[12] = c.cable.kind; q[13] = c.cable.flags; q[14] = c.resumed; q[15] = c.src_flags;
    }
    // V6: the UFCS modes, a u24 each (bits 0..7 lo_dv, 8..16 hi_dv, 17..23 current in 50 mA); an unused slot is all zero
    for (unsigned i = 0; i < kMaxUfcs; ++i) {
        uint8_t* q = p + off::kUfcs + 3 * i;
        const UfcsRange& r = c.ufcs[i];
        const uint32_t v = r.hi_dv == 0 ? 0u : std::min<unsigned>(r.lo_dv, 255u) | std::min<unsigned>(r.hi_dv, 511u) << 8 | std::min<unsigned>(r.ma / 50u, 127u) << 17;
        q[0] = uint8_t(v); q[1] = uint8_t(v >> 8); q[2] = uint8_t(v >> 16);
    }
    // V7: the repeat block: times u8, last_boots u16, last_uptime_s u24 (all saturating)
    {
        uint8_t* q = p + off::kRepeat;
        const uint32_t up = c.last_uptime_s > 0xFFFFFFu ? 0xFFFFFFu : c.last_uptime_s;
        q[0] = c.times; put16(q + 1, c.last_boots);
        q[3] = uint8_t(up); q[4] = uint8_t(up >> 8); q[5] = uint8_t(up >> 16);
    }
    put16(b + 4 + kPayload, bbstore::crc16(0xFFFF, {b, 4 + kPayload}));
    return kMaxRecord;
}

// Parses a payload (the bytes after the 4 byte record header). False on a wrong length or an out of range field.
inline bool decode_card(std::span<const uint8_t> payload, Card& c) {
    using namespace detail;
    if (payload.size() != kPayload) return false;
    const uint8_t* p = payload.data();
    Card d;
    d.id.fp = get32(p); d.id.vid = get16(p + 4); d.id.pid = get16(p + 6);
    d.card_no = get16(p + 8); d.runs = get16(p + 10);
    d.boots = get32(p + 12); d.uptime_s = get32(p + 16); d.fw = get32(p + 20);
    d.handover_ds = get16(p + 24);
    d.spec_rev = p[26]; d.num_pdos = p[27];
    if (p[28] > uint8_t(ct::HandoverOutcome::InContract) || p[29] > uint8_t(ct::State::Attach) || p[30] >= kReasonCount || p[31] > kMaxRanges) return false;
    d.handover = ct::HandoverOutcome(p[28]); d.state = ct::State(p[29]); d.reason = ct::Reason(p[30]);
    if (p[33] > kFormat) return false;
    d.format = p[33];
    d.n_ranges = p[31]; d.ev_flags = p[32];
    std::memcpy(d.name, p + off::kName, kNameMax); d.name[kNameMax] = 0;
    if (p[33] >= 2) { d.id.pdp_w = p[off::kPdp]; d.name_auto = (p[off::kFlags] & kFlagNameAuto) && d.name[0]; }
    if (p[33] >= 3) d.pd_ma = uint16_t((p[off::kFlags] >> 1) * 50);   // older cards: spare bytes ignored
    for (unsigned g = 0; g < kGroups; ++g) {
        const uint8_t* q = p + off::kGroups + 4 * g;
        if ((q[0] & 0x3F) >= kReasonCount) return false;
        if (p[33] == 0) {   // first cards: detail byte, u16 max in mV, no failing target
            d.group[g] = {GV(q[0] >> 6), uint16_t(dv_round(get16(q + 2)) * 100u), q[1], uint8_t(q[0] & 0x3F), 0};
        } else {
            const uint32_t v = q[1] | uint32_t(q[2]) << 8 | uint32_t(q[3]) << 16;
            d.group[g] = {GV(q[0] >> 6), uint16_t((v & 0x1FF) * 100), unpack_detail(v >> 18), uint8_t(q[0] & 0x3F), uint16_t((v >> 9 & 0x1FF) * 100)};
        }
    }
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        const uint8_t* q = p + off::kRanges + 10 * i;
        if (q[0] >= unsigned(ct::Algorithm::Count)) return false;
        const uint16_t lo = get16(q + 2);
        d.range[i] = {q[0], q[1], p[33] >= 3 ? uint16_t(lo & 0x1FF) : lo, get16(q + 4), get16(q + 6), get16(q + 8),
                      p[33] >= 3 ? uint16_t((lo >> 9) * 50) : uint16_t(0)};
    }
    // V4: EPR levels (format >= 4)
    if (p[33] >= 4) {
        for (unsigned i = 0; i < kEprLevels; ++i) {
            const uint8_t* q = p + off::kEpr + 6 * i;
            if ((q[2] & 0x3F) >= kReasonCount) return false;
            const uint32_t v = get16(q + 3) | uint32_t(q[5]) << 16;
            d.epr[i] = {get16(q),
                        {GV(q[2] >> 6), uint16_t((v & 0x1FF) * 100), unpack_detail(v >> 18), uint8_t(q[2] & 0x3F), uint16_t((v >> 9 & 0x1FF) * 100)}};
        }
    } else {
        // Older formats: EPR entries are all GV::None
        for (unsigned i = 0; i < kEprLevels; ++i) d.epr[i] = {0, {GV::None, 0, 0, 0, 0}};
    }
    // V5: the cable block; older formats decode to an empty cable (migrate_to_current keeps the marker bit)
    if (p[33] >= 5) {
        const uint8_t* q = p + off::kCable;
        d.cable = {get32(q), get16(q + 4), get16(q + 6), get16(q + 8), get16(q + 10), q[12], q[13]};
        d.resumed = q[14];
        d.src_flags = q[15];   // cards written before it was assigned hold 0 here (the byte was reserved)
    }
    // V6: the UFCS modes; older formats keep them empty (their padding is never read as modes)
    if (p[33] >= 6) {
        for (unsigned i = 0; i < kMaxUfcs; ++i) {
            const uint8_t* q = p + off::kUfcs + 3 * i;
            const uint32_t v = q[0] | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16;
            if ((v >> 8 & 0x1FF) != 0) d.ufcs[i] = {uint16_t(v & 0xFF), uint16_t(v >> 8 & 0x1FF), uint16_t((v >> 17) * 50)};
        }
    }
    // V7: the repeat block; older formats were run once (their bytes here are a CRC and padding)
    if (p[33] >= 7) {
        const uint8_t* q = p + off::kRepeat;
        d.times = q[0] ? q[0] : uint8_t(1);
        d.last_boots = get16(q + 1);
        d.last_uptime_s = q[3] | uint32_t(q[4]) << 8 | uint32_t(q[5]) << 16;
    }
    c = d;
    return true;
}

}  // namespace k2::report_card
