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
//      bit6 started anyway against a refusing guard, bit7 the planned re-attach; older cards hold 0 in bits 3..7), u8 resumed (how many times the run lost power and went on; older
//      cards hold 0 here), u8 src_flags (bit0 the source listed OPPO's SVID, bits 1..7 the SCP current limit the adapter reported in 50 mA units; older cards hold 0). Older formats decode with an empty cable.
//  178 ufcs[4] x 3 (format v6 only): a u24 per UFCS mode of the charger's Output_Capabilities, planner order (lowest minimum first), unused
//      slots all zero: bits 0..7 lo (100 mV, saturates at 25.5 V), bits 8..16 hi (100 mV), bits 17..23 the mode's maximum current in 50 mA
//      units (saturates at 6.35 A). A source listing more than four modes keeps the first four. Older formats decode with no modes.
//  190 repeat x 6 (format v7 only): u8 times (how many runs gave exactly the results this card holds; 1 = tested once, saturates at 255),
//      u16 last_boots and u24 last_uptime_s (the session boot counter and uptime of the newest of those runs; read only when times > 1,
//      zero otherwise; saturate at 65535 and 2^24 - 1). boots / uptime_s above stay those of the run that was stored first. Older
//      formats decode as times = 1 (their bytes at 190.. are a CRC and padding, never read).
//  198 spread (format v9 only): 4 ranges x 4 levels of int8 mV - the setpoint error (measured - target) of each passed level of an
//      APDO, sorted low to high, -128 = no level there. Older formats decode with no spread.
//  214 sweep (format v10 only): 4 ranges x 9 bytes - the deep sweep of that APDO (ROADMAP "PPS deep sweep"): int8 min / max /
//      worst error (mV, measured - setpoint) in bytes 0..2, then 45 bits in bytes 3..8: worst_dv | peak_dv<<9 | peak_at_dv<<18 |
//      fold_dv<<27 | fold_from_dv<<36, all in 100 mV units (0 = none; an all-zero block = no sweep on the range). Older
//      formats decode with no sweep.
//  v11 changes no payload byte: a Deep card may travel with the Deep block (k2/chargertest/report_card_deep.hpp: the PD spec
//      summary, the SVOOC ceiling stage and the per-range sweep error graphs). The block lives beside the card, not in it: the store
//      keeps it in the card's own ring record, the share link appends it to the schema v2 stream. The format byte says so.
//
// A re-run that gives the same results is counted, not stored (same_results, fold_repeat; the store applies them): the card keeps its
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
inline constexpr unsigned kSpreadLevels = 4;   // v9: the setpoint errors kept per APDO (the plan has at most 4 distinct levels)
inline constexpr int8_t kSpreadNone = -128;     // an empty spread slot
inline constexpr unsigned kNameMax = 20;
inline constexpr unsigned kReasonCount = unsigned(ct::Reason::Folded) + 1;   // 52 values fit the card's 6-bit reason field
static_assert(kReasonCount <= 64, "the card stores a reason in 6 bits");
inline constexpr uint8_t kMagic = 0xA6, kTypeCard = 1, kTypeDelete = 2;
inline constexpr uint8_t kFormat = 11;  // payload byte 33: v4 adds per-EPR-level verdicts, v5 the cable block (Card carries it), v6 the UFCS modes, v7 the repeat counter, v8 the source's mid-run re-balance count, v9 the per-level setpoint-error spread, v10 the deep-sweep summary per range, v11 the Deep block beside a Deep card (no payload change)
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
    uint8_t kind = 0;          // a built-in cable's emrec::builtin_info id (1 EPR 50V, 2 Xiaomi 6A, 4 OnePlus; 3 was "OnePlus mag", retired and recorded as 4), 0 = saved clone, 0xFF = real cable
    uint8_t flags = 0;         // bit0 observed, bit1 spoofed, bit2 the run was self-powered (the charger under test powered the K2),
                               // bits 3..5 the Ra guard's reading + 1 (0 = not judged), bit6 started anyway against a refusing reading,
                               // bit7 the run went through the charger test's planned re-attach (that reboot is not a resume)
    bool operator==(const Cable&) const = default;
};
// Card::src_flags (cable block byte 15): the source listed OPPO's SVID 0x22D9 in its Discover SVIDs answer. A note only: the K2 sends
// that SVID nothing.
inline constexpr uint8_t kSrcOppoSvid = 1;
inline constexpr uint8_t kCableObserved = 1, kCableSpoofed = 2, kCableSelfPowered = 4, kCableKindReal = 0xFF;
// The Ra guard's reading when the run's identity was started (bits 3..5 hold raguard::Topology + 1, 0 = not judged, as
// every card before this field) and bit 6: the user started anyway against a refusing reading (the "Cable connected" warning).
inline constexpr uint8_t kCableGuardShift = 3, kCableGuardMask = 7 << kCableGuardShift, kCableGuardBypassed = 0x40;
// Bit 7: the run went through the planned re-attach (the K2 blinked off once so the charger reads the cable at a real
// attach). Its reboot is not counted in Card::resumed.
inline constexpr uint8_t kCableReattach = 0x80;

// One UFCS mode of the charger's Output_Capabilities (v6): the voltage range and the mode's current limit. hi_dv == 0 marks an
// unused slot. Older cards hold no modes and show the UFCS volts alone.
struct UfcsRange {
    uint16_t lo_dv = 0, hi_dv = 0;   // 100 mV units (lo saturates at 25.5 V, hi at 51.1 V)
    uint16_t ma = 0;                 // the mode's maximum current, mA in 50 mA steps (saturates at 6.35 A)
    bool operator==(const UfcsRange&) const = default;
};

// A fresh card's spread: every slot kSpreadNone (the "no level there" mark), so hand-built cards encode like built ones.
constexpr std::array<std::array<int8_t, kSpreadLevels>, kMaxRanges> spread_none() {
    std::array<std::array<int8_t, kSpreadLevels>, kMaxRanges> a{};
    for (auto& x : a) x.fill(kSpreadNone);
    return a;
}

// Readable cause of a failed or skipped group: the engine's own code when the step stored one (pd::Err for PD steps
// stored as 128 + err, pd::SinkResult below that, fc::Fail for the fast-charge groups), else the step's reason.
inline bool group_handshake_ok(const GroupResult& r) { return r.verdict == GV::Pass && r.detail == uint8_t(fc::Fail::NoVoltage); }
inline bool group_detected(const GroupResult& r) { return r.verdict == GV::Pass && r.detail == uint8_t(fc::Fail::Detected); }
const char* group_why(unsigned group, const GroupResult& r);
struct Counts { unsigned pass = 0, fail = 0, skip = 0; };

struct Card {
    // Out of line (report_card.cpp): the zeroed arrays and the spread_none() fill are about 130 B, and nine pages and
    // stores each built a Card inline.
    Card();
    Identity id;
    uint8_t num_pdos = 0, spec_rev = 0, ev_flags = 0;
    std::array<GroupResult, kGroups> group{};
    uint8_t n_ranges = 0;
    uint16_t pd_ma = 0;      // highest current of any fixed PDO, mA in 50 mA steps (0 = unknown, older card)
    std::array<Range, kMaxRanges> range{};
    // V9: the setpoint error in mV (measured - target, saturated to +-127) of each passed level of the range, sorted low to
    // high; kSpreadNone in the slots past the levels the run reached. Indexed like `range`.
    std::array<std::array<int8_t, kSpreadLevels>, kMaxRanges> spread = spread_none();
    // V10: the deep sweep of each range (the PPS deep sweep): the error summary over the recorded points and the peak /
    // fold-back levels, in 100 mV units (0 = none). All zero = no sweep on that range (older formats decode to that).
    struct SweepSum {
        int8_t min_err = 0, max_err = 0, worst_err = 0;   // mV, measured - setpoint
        uint16_t worst_dv = 0, peak_dv = 0, peak_at_dv = 0, fold_dv = 0, fold_from_dv = 0;   // 100 mV, 0 = none
        bool operator==(const SweepSum&) const = default;
    };
    std::array<SweepSum, kMaxRanges> sweep{};
    std::array<EprLevel, kEprLevels> epr{};   // per-EPR-level verdicts (v4 only; older cards decode to all GV::None)
    std::array<UfcsRange, kMaxUfcs> ufcs{};   // the charger's UFCS modes, planner order, unused slots last (v6 only; see ufcs_modes())
    Cable cable{};             // the cable of the run (v5 only; older cards decode to an empty cable)
    uint8_t resumed = 0;       // times the run lost power and resumed (cable block byte 14; 0 on older cards)
    uint8_t rebalanced = 0;    // how often the source re-announced mid-run and the PD rows were re-planned (v8 byte 196; 0 on older cards): the "SOURCE rebalanced Nx" note
    ct::Level level = ct::Level::Medium;   // the run's level (v8 byte 197, the block's spare byte: 0 = Medium on every older card; no format bump)
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
               n_ranges == o.n_ranges && pd_ma == o.pd_ma && range == o.range && spread == o.spread && sweep == o.sweep && epr == o.epr && ufcs == o.ufcs && cable == o.cable && resumed == o.resumed && rebalanced == o.rebalanced && level == o.level && src_flags == o.src_flags && format == o.format &&
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
// "SCP cap 20V 6A" (or "SCP cap 6A"): the adapter's own readout, passive. false when the card holds no current.
bool scp_note(const Card& c, char* out, size_t cap);

// Quick Charge 4+/5 label of a source, from facts the card already holds (passive: no probe, and never a key for any
// behaviour). A Qualcomm USB vendor ID (Discover Identity or Source_Capabilities_Extended) together with a PPS APDO is
// "QC4+"; a rated power of 100 W or more, or an EPR AVS range, makes it "QC5". Anything else carries no label.
//
// The same passive reading names two more OEM modes whose wire is plain PD PPS (inferred, never a behaviour key):
//   Moto TurboPower: a Motorola (0x22B8) or Lenovo (0x17EF) vendor ID plus a PPS APDO advertising more than 5 A (20 V 6.25 A).
//   Samsung SFC 3.0: a PPS APDO reaching 20 V at 3 A or more.
//   Samsung SFC 2.0: PPS to 15 V at 3 A or more, or PPS covering 3.3-11 V at 4.05 A or more behind a 5 A cable.
// The SFC labels are a brand claim, so they need the brand's own VID: a probe that read vid 0 (or another vendor's)
// gets no Samsung label, however SFC-shaped the capability is. A Qualcomm ID keeps the QC label.
enum class QcLabel : uint8_t { None, Qc4Plus, Qc5, MotoTurbo, Sfc2, Sfc3 };
inline constexpr uint16_t kSamsungVid = 0x04E8;
inline constexpr uint16_t kQualcommVid = 0x05C6;
inline constexpr uint16_t kMotorolaVid = 0x22B8;
inline constexpr uint16_t kLenovoVid = 0x17EF;
inline constexpr unsigned kQc5MinWatts = 100;
inline constexpr unsigned kMotoMinMa = 5000;      // a PPS APDO above this is TurboPower class
inline constexpr unsigned kSfc2MaxCapMa = 4050;   // SFC 2.0 at 3.3-11 V

QcLabel qc_label(const Card& c);
const char* qc_label_name(QcLabel l);

// The PPS span the label rests on, "3.3-21V": the lowest minimum to the highest maximum of the card's PPS ranges.
// Returns the length written (0 and an empty string when the card has no PPS range).
size_t qc_pps_span_text(const Card& c, char* out, size_t cap);

// The PPS V/A span, e.g. "5-20V 5A": the lowest minimum to the highest maximum voltage of the card's PPS ranges,
// plus the maximum advertised current of those ranges. Returns the length written (0 and an empty string when
// the card has no PPS range or no current info).
size_t qc_pps_va_text(const Card& c, char* out, size_t cap);

// The highest-wattage fixed PDO V/A text, e.g. "20V 5A". Uses the highest voltage measured on fixed PDOs
// (group[pd].max_mv) and the highest advertised current of any fixed PDO (pd_ma). Returns the length written
// (0 and an empty string when the card has no fixed PDO current info or no measured voltage).
size_t qc_pd_va_text(const Card& c, char* out, size_t cap);

// The justification part of the QC label, e.g. "via PPS 5-20V 5A" or "via PD 20V 5A".
// Returns the length written (0 and an empty string when the card has no label).
size_t qc_label_justification(const Card& c, char* out, size_t cap);

// The fact line behind the label, e.g. "QC5 (via PD 20V 5A)" or "QC4+ (via PPS 5-20V 5A)".
// Returns the length written (0 and an empty string when the card has no label).
size_t qc_label_text(const Card& c, char* out, size_t cap);

uint32_t fingerprint(std::span<const uint32_t> pdos, uint8_t spec_rev);

// Identity of a source that advertised no PD caps (a bank on its legacy port): FNV-1a 32 over, for every passing
// step, the group and the level in 100 mV, behind a salt that no PDO fingerprint uses. The same charger keeps the
// same card while its answers stay the same; a different set of answered protocols is another charger.
uint32_t legacy_fingerprint(std::span<const ct::Step> steps);

// Firmware build id for the card: the commit token of the build's describe string (hex, first 8 digits,
// a leading "g" stripped); 0 when the text has none.
uint32_t fw_hash(std::string_view text);

constexpr bool valid_name(std::string_view n) { return emlib::name_valid(n) && n.size() <= kNameMax; }
void set_name(Card& c, std::string_view n);

// Brand of a USB-IF vendor ID, nullptr when it is not in the (short) table. Every entry is checked against the
// linux-usb.org usb.ids list except Anker, which is not in that list; unverified IDs are left out.
// Honor 0x339B, vivo 0x2D95 and Zimi/CUKTECH 0x2B01 are not in that list (checked against usb.ids 2026.06.26), so they stay out.
const char* brand(uint16_t vid);

// "<Brand> <PDP>W" for the card name, written to out (kNameMax + 1 bytes) and returned as its length (0 = nothing known).
// The brand is the VID's, else the manufacturer text (only the name alphabet, trimmed), else "VID xxxx"; the
// wattage suffix is left out when pdp_w is 0. The base is cut so the whole name stays within kNameMax.
size_t auto_name(char* out, uint16_t vid, std::string_view mfr, uint8_t pdp_w);

// Gives an unnamed card its automatic name from the identity it holds (+ the manufacturer text, which is not stored).
void set_auto_name(Card& c, std::string_view mfr);

// Carries what an earlier card of the same charger knew into the new one: a user name always wins over an automatic
// one, an automatic name is replaced by a newer automatic name, and an identity the run did not learn is kept.
void inherit(Card& c, const Card& prev);

namespace detail {
constexpr bool within(unsigned a, unsigned b, unsigned step) { return (a > b ? a - b : b - a) <= step; }
// One stored result (a group's or an EPR level's): everything but the measured maximum must match, the maximum within one step.
constexpr bool same_result(const GroupResult& a, const GroupResult& b) {
    return a.verdict == b.verdict && a.reason == b.reason && a.detail == b.detail && a.fail_mv == b.fail_mv && within(a.max_mv, b.max_mv, 100);
}
}  // namespace detail

// True when `run` gave the same results as the stored card `prev`: the rule written at the top of this file. Pure, symmetric
// except for the one-step tolerance, which is the same both ways.
bool same_results(const Card& prev, const Card& run);

// Turns `c`, a run that repeated the stored card `prev` (same_results), into the card to store: the stored rows and first-run time
// stay, the run counts, the repeat counter grows and last_* name the newest run. What only the repeat learned (an identity the
// first probe missed, an automatic name where the stored card had none) is added; a user name always stays. It works in place
// (no second Card on the stack: the save path runs on the UI task's small frame).
void fold_repeat(const Card& prev, Card& c);

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

// Fills `c` with the card of a finished run. The out-param form is for the one stack-gated caller
// (ChargerTestSession::save_card): returning a Card by value puts a second Card on that frame. Never inlined: its
// per-group tally arrays are dead once it returns, but inlined they would keep their slots in save_card's frame
// all through the store write below it (tools/stack_depth.py).
[[gnu::noinline]] void build_into(Card& c, std::span<const ct::Step> steps, const ct::Evidence& ev, ct::HandoverInfo h, Identity id, Context cx,
                                  ct::State state, ct::Reason reason, const Cable& cable = {}, uint8_t resumed = 0,
                                  ctsweep::View sweep = {});

Card build(std::span<const ct::Step> steps, const ct::Evidence& ev, ct::HandoverInfo h, Identity id, Context cx,
           ct::State state, ct::Reason reason, const Cable& cable = {}, uint8_t resumed = 0, ctsweep::View sweep = {});

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
inline constexpr unsigned kV8 = kRepeat + 6;                             // 196: v8 block, 2 bytes (rebalanced u8 + spare)
inline constexpr unsigned kV9 = kV8 + 2;                               // 198: v9 spread block, kMaxRanges * kSpreadLevels bytes
inline constexpr unsigned kV10 = kV9 + kMaxRanges * kSpreadLevels;    // 214: v10 deep-sweep summary, kMaxRanges * 9 bytes
}  // namespace off
inline constexpr unsigned kSweepBytes = 9;   // v10 per range: int8 min/max/worst error, then 5 x 9-bit 100 mV fields in 6 bytes
inline constexpr unsigned kPayload = off::kV10 + kMaxRanges * kSweepBytes;   // 250 (v9 cards are 214: their CRC/padding sits in the sweep bytes and is not read)
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
size_t encode_card(std::span<uint8_t> out, const Card& c);

// Parses a payload (the bytes after the 4 byte record header). False on a wrong length or an out of range field.
// Any even payload from 144 (v3) to kPayload (v10) is accepted; an older payload's trailing bytes are the CRC/padding of
// the shorter record and are not read.
bool decode_card(std::span<const uint8_t> payload, Card& c);

}  // namespace k2::report_card
