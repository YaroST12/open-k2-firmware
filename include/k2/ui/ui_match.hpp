// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Wants vs Offers: pure match logic between the charger's Source_Capabilities, the sink's sniffed
// Request and (when known) the sink's capabilities. Passive only: nothing here drives CC.
#pragma once

#include <cstdint>
#include <span>

#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2 {

enum class Tri : uint8_t { Unknown, No, Yes };

// Fixed-point append helpers shared by the Connection page rows: "3A", "1.5A", "0.5A" / "5V", "20.2V".
template <size_t N>
void put_a(StrBuf<N> &s, uint32_t ma) {   // "3A", "1.5A", "3.25A"
    if (ma % 1000 == 0) s.put_u32(ma / 1000);
    else if (ma % 100 == 0) s.put_fixed(int32_t(ma / 100 * 100), 1);
    else if (ma % 10 == 0) s.put_fixed(int32_t(ma), 2);
    else s.put_fixed(int32_t(ma), 3);
    s.put_char('A');
}

template <size_t N>
void put_v(StrBuf<N> &s, uint32_t mv) {
    if (mv % 1000 == 0) s.put_u32(mv / 1000);
    else if (mv % 100 == 0) s.put_fixed(int32_t(mv), 1);
    else if (mv % 10 == 0) s.put_fixed(int32_t(mv), 2);
    else s.put_fixed(int32_t(mv), 3);
}

struct MatchInput {
    const pd::SourceCaps &caps;
    pd::Contract contract;             // the sniffed Request (valid + pos)
    Tri mismatch;                      // RDO Capability Mismatch bit; Unknown when not retained
    std::span<const uint32_t> sink;    // raw sink PDOs (Sink_Capabilities); empty when not seen
    uint32_t offered = ~0u;            // bit i: position i+1 is in the charger's latest caps (OfferBook)
};

// The offers seen from one source, append-only: a charger that re-sends fewer PDOs without a detach (it
// drops PPS after a hard reset) keeps its earlier ones, flagged as not offered now. Call clear() on a
// new attach; the book is by PDO position, in the charger's own order.
struct OfferBook {
    pd::SourceCaps caps;
    uint32_t offered = 0;
    void clear() { caps = pd::SourceCaps{}; offered = 0; }
    void merge(const pd::SourceCaps &latest);
};

enum class RowState : uint8_t {
    Unknown,      // no sink data to judge against ("?")
    Selected,     // the RDO points here
    SelectedLow,  // selected but the charger gives less current than the sink wants
    Ok,           // the sink has a compatible PDO
    Low,          // compatible but the charger current is below the sink's need
    No,           // the sink cannot use this PDO
    Gone,         // seen earlier from this source, not in its latest Source_Capabilities
};

enum class Verdict : uint8_t {
    NoCaps,        // no Source_Capabilities seen
    NoRequest,     // caps seen but no Request (device attached before the K2)
    OutsideCaps,   // Request position beyond the offered PDOs
    WantsHigherV,  // the sink's top voltage exceeds the charger's
    CurrentLimited,
    MismatchBit,   // RDO Capability Mismatch bit set, no better explanation
    SinkUnknown,   // request seen, sink capabilities not
    Matches,
};

enum class Severity : uint8_t { Ok, Dim, Caution };

inline constexpr int kFlagMismatchBit = 1;
inline constexpr int kFlagWantsHigherV = 2;
inline constexpr int kFlagCurrentLimited = 4;
inline constexpr int kFlagOutsideCaps = 8;

struct MatchResult {
    bool caps_seen = false, request_seen = false, sink_ok = false;
    int rows = 0;
    int gone = 0;   // rows no longer offered
    RowState row[pd::kMaxPdos]{};
    int want_pos = 0;
    uint32_t want_mv = 0, want_ma = 0, want_mw = 0;
    uint32_t need_ma = 0, have_ma = 0;  // selected row: what the sink wants / the charger gives
    uint32_t src_max_mv = 0, src_max_mw = 0, sink_max_mv = 0;
    int flags = 0;
    uint32_t typec_ma = 0;   // source without PD (Rp only): its Type-C current, 0 when unknown
    bool typec_rp = false;   // a Type-C Rp advertisement exists at all (a source is attached), even when the current is unknown
    Verdict verdict = Verdict::NoCaps;
};

// ---- the Source tab's no-PD rows: the Type-C advertisement of a source that sent no caps ----

// True when the active CC line at `cc_mv` is driven by a source's Rp: with our Rd on the line the
// Rp/Rd divider reads 0.4..1.7 V, without it the open pull-up reads ~3.3 V; an idle net sits below 200 mV.
constexpr bool cc_line_has_rp(std::int32_t cc_mv) { return cc_mv >= 200; }

// The Rp current a line at `cc_mv` advertises through the Rp/Rd divider (the bands the Lines page
// names): 200..660 mV default USB (500/900 mA), 660..1230 mV 1.5 A, 1230..2200 mV 3 A. 0 when the line
// is idle or open above 2.2 V (no Rd on it: the level does not encode the current).
constexpr std::uint32_t typec_ma_from_cc_mv(std::int32_t cc_mv) {
    if (cc_mv < 200) return 0;
    if (cc_mv < 660) return 500;
    if (cc_mv < 1230) return 1500;
    if (cc_mv < 2200) return 3000;
    return 0;
}

// The Type-C row's value: the measured VBUS when it is known ("20.2V"), then the Rp advertisement
// ("USB default" for a 500 mA default-USB Rp, otherwise "3A" / "1.5A"); nothing when neither is known.
// Always within the 19-cell value column of the Connection page.
void typec_row_text(std::int32_t vbus_mv, std::uint32_t typec_ma, StrBuf<28> &out);

// The Source tab's Legacy row lists D+/D- fast-charge protocols only. The PD-family verdicts (Pd,
// PdPe, PdMtk, PdLikely) are CC verdicts: never "legacy", never shown next to "PD none".
constexpr bool legacy_row_protocol(fastcharge::Protocol p) {
    using P = fastcharge::Protocol;
    return p != P::Pd && p != P::PdPe && p != P::PdMtk && p != P::PdLikely;
}

// The top badge of the PD test page: how well the sink's contract fits what the source offers.
enum class Agreement : uint8_t { NoContract, Partial, Full, TypeCOnly };
Agreement agreement(const MatchResult &r);

MatchResult match_compute(const MatchInput &in);
Severity verdict_severity(Verdict v);
bool gone_footer(const MatchResult &r, StrBuf<32> &out);              // "2 offers not sent now"; false when none
void match_footer(const MatchResult &r, StrBuf<32> &out);             // one line, <= 30 characters
const char *request_state_name(const pd::Contract &c);               // "", Requested, Accepted, Ready, Rejected
// RDO flags as whole names, most abnormal first ("Mismatch, USB comm"), at most `room` cells; " +" when some did not fit.
void rdo_flags_text(uint8_t flags, std::size_t room, StrBuf<32> &out);
void sink_summary(std::span<const uint32_t> sink, StrBuf<32> &out);   // "Sink caps 5 9 15 20V +PPS"

// ---- The sink's wants list: every Sink_Capabilities PDO of the current attach, lined up with the offers ----

// Where the list came from: the source asked Get_Sink_Cap and the K2 sniffed the answer, or Identify asked.
enum class WantsSrc : uint8_t { None, Sniffed, Identify };
struct WantsList {
    std::span<const uint32_t> pdos;
    WantsSrc src = WantsSrc::None;
};
// `sniffed` is cleared on every attach by the driver; `ident_current` = Identify ran in this attach session.
WantsList pick_wants(const pd::SinkCapsSeen &sniffed, bool ident_current, const pd::IdStatus &id);
const char *wants_src_name(WantsSrc s);   // "sniffed" / "from Identify" / ""

// Flags of the first Sink_Capabilities PDO (a fixed one; USB PD 3.1 table 6.17).
struct SinkFlags {
    bool valid = false;
    bool dual_role_power = false, higher_cap = false, unconstrained = false, usb_comm = false, dual_role_data = false;
    uint8_t fr_swap = 0;   // bits 24:23: 0 none, 1 default USB power, 2 1.5 A, 3 3 A
};
SinkFlags sink_flags(std::span<const uint32_t> sink);
// Every set flag on up to two 31-cell lines, including all three distinct FR swap levels.
void sink_flags_lines(const SinkFlags &f, StrBuf<34> &l1, StrBuf<34> &l2);

// One display row: the charger's offer (src) and the sink PDO that lines up with it by voltage (sink); -1 = none.
// Sink PDOs that fit no free row follow after the offers; `unmet` marks the ones no offer can meet at all.
struct WantsRow {
    int8_t src = -1, sink = -1;
    bool contract = false;   // the sniffed Request points at this offer (both columns)
    bool unmet = false;
};
struct WantsLayout {
    WantsRow row[2 * pd::kMaxPdos];
    int n = 0;
    int unmet = 0;
};
// `offered`: bit i = offer i+1 is in the charger's latest caps; `want_pos` = the Request position (0 = none).
WantsLayout wants_layout(const pd::SourceCaps &caps, uint32_t offered, std::span<const uint32_t> sink, int want_pos);

}  // namespace k2
