// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/report_card_json.hpp"

#include "k2/chargertest/step_label.hpp"
#include "k2/core/textbuf.hpp"
#include "k2/pd/ra_guard.hpp"

namespace k2::report_card {

namespace {
constexpr const char* kVerdict[] = {"none", "pass", "fail", "skip"};
constexpr const char* kHandover[] = {"not tried", "fell back", "stayed PD", "fault", "reboot", "in PD contract"};
}  // namespace

// The card name is restricted to [A-Za-z0-9 -_.] by valid_name, so it needs no escaping.
std::size_t render_json(const Card& c, std::span<char> out) {
    TextBuf t(out);
    t.put("{\"no\":%u,\"name\":\"%s\",\"fp\":\"%08x\",\"vid\":%u,\"pid\":%u,",
          unsigned(c.card_no), c.name, unsigned(c.id.fp), unsigned(c.id.vid), unsigned(c.id.pid));
    // Identity extras only when known, so a card without them renders as before (tools/report_card.py does the same).
    if (const char* b = brand(c.id.vid)) t.put("\"brand\":\"%s\",", b);
    if (c.id.pdp_w) t.put("\"pdp_w\":%u,", unsigned(c.id.pdp_w));
    if (c.name_auto) t.put("\"name_auto\":true,");
    // The passive OEM label's bare name (tools/report_card.py and the results page write the same); the fact line behind it is UI text.
    if (const QcLabel l = qc_label(c); l != QcLabel::None) t.put("\"qc\":\"%s\",", qc_label_name(l));
    t.put("\"runs\":%u,\"boots\":%u,\"uptime_s\":%u,", unsigned(c.runs), unsigned(c.boots), unsigned(c.uptime_s));
    // Repeats of the stored run (v7), only when there were any: how many identical runs and when the newest one was.
    if (c.times > 1)
        t.put("\"times\":%u,\"last_boots\":%u,\"last_uptime_s\":%u,", unsigned(c.times), unsigned(c.last_boots), unsigned(c.last_uptime_s));
    // v8: how often the source re-announced its list mid-run and the PD rows were re-planned (budget-reannounce-mid-run).
    if (c.format >= 8 && c.rebalanced)
        t.put("\"rebalanced\":%u,", unsigned(c.rebalanced));
    t.put("\"fw\":\"%08x\",\"state\":\"%s\",\"reason\":\"%s\",\"pdos\":%u,\"spec_rev\":%u,", unsigned(c.fw), ct::name(c.state),
          ct::name(c.reason), unsigned(c.num_pdos), unsigned(c.spec_rev));
    t.put("\"handover\":\"%s\",\"handover_ms\":%u,\"groups\":{", kHandover[unsigned(c.handover)], unsigned(c.handover_ds) * 100u);
    bool first = true;
    for (unsigned g = 0; g < kGroups; ++g) {
        const GroupResult& r = c.group[g];
        if (r.verdict == GV::None) continue;
        t.put("%s\"%s\":{\"verdict\":\"%s\"", first ? "" : ",", ct::name(ct::Group(g)), kVerdict[unsigned(r.verdict)]);
        first = false;
        if (g == unsigned(ct::Group::Pd) && c.pd_ma) t.put(",\"max_ma\":%u", unsigned(c.pd_ma));
        if (r.max_mv) t.put(",\"max_mv\":%u", unsigned(r.max_mv));
        if (r.fail_mv) t.put(",\"fail_mv\":%u", unsigned(r.fail_mv));
        if (r.reason) t.put(",\"reason\":\"%s\"", ct::name(ct::Reason(r.reason)));
        if (r.detail) t.put(",\"detail\":%u,\"why\":\"%s\"", unsigned(r.detail), group_why(g, r));
        t.put("}");
    }
    t.put("},\"ranges\":[");
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        const Range& r = c.range[i];
        t.put("%s{\"alg\":\"%s\",\"lo_mv\":%u,\"hi_mv\":%u,\"max_mv\":%u,\"fail_mv\":%u", i ? "," : "",
              ct::name(ct::Algorithm(r.alg)), unsigned(r.lo_dv) * 100u, unsigned(r.hi_dv) * 100u, unsigned(r.max_dv) * 100u,
              unsigned(r.fail_dv) * 100u);
        if (r.ma) t.put(",\"max_ma\":%u", unsigned(r.ma));
        if (c.format >= 9) {   // v9: the setpoint error in mV per passed level, low to high (key only when there is one)
            bool any = false;
            for (unsigned k = 0; k < kSpreadLevels; ++k) any |= c.spread[i][k] != kSpreadNone;
            if (any) {
                t.put(",\"err_mv\":[");
                bool first_err = true;
                for (unsigned k = 0; k < kSpreadLevels; ++k) {
                    if (c.spread[i][k] == kSpreadNone) continue;
                    t.put("%s%d", first_err ? "" : ",", int(c.spread[i][k]));
                    first_err = false;
                }
                t.put("]");
            }
        }
        if (c.format >= 10 && c.sweep[i].peak_dv) {   // v10: the deep sweep of this APDO (key only when it ran)
            const auto& w = c.sweep[i];
            t.put(",\"sweep\":{\"min_mv\":%d,\"max_mv\":%d,\"worst_mv\":%d,\"worst_at_mv\":%u,\"peak_mv\":%u,\"peak_at_mv\":%u",
                  int(w.min_err), int(w.max_err), int(w.worst_err), unsigned(w.worst_dv) * 100u,
                  unsigned(w.peak_dv) * 100u, unsigned(w.peak_at_dv) * 100u);
            if (w.fold_dv) t.put(",\"fold_mv\":%u,\"fold_from_mv\":%u", unsigned(w.fold_dv) * 100u, unsigned(w.fold_from_dv) * 100u);
            t.put("}");
        }
        t.put("}");
    }
    t.put("]");
    if (c.format >= 4) {   // v4 cards carry per-EPR-level verdicts; older formats render as before
        t.put(",\"epr_levels\":{");
        bool first_epr = true;
        for (unsigned i = 0; i < kEprLevels; ++i) {
            const EprLevel& e = c.epr[i];
            if (e.dv == 0) continue;   // no EPR at this level
            if (!first_epr) t.put(",");
            first_epr = false;
            t.put("\"%uV\":{\"verdict\":\"%s\"", e.dv * 100u, kVerdict[unsigned(e.r.verdict)]);
            if (e.r.max_mv) t.put(",\"max_mv\":%u", unsigned(e.r.max_mv));
            if (e.r.fail_mv) t.put(",\"fail_mv\":%u", unsigned(e.r.fail_mv));
            if (e.r.reason) t.put(",\"reason\":\"%s\"", ct::name(ct::Reason(e.r.reason)));
            if (e.r.detail) t.put(",\"detail\":%u,\"why\":\"%s\"", unsigned(e.r.detail), group_why(unsigned(ct::Group::Pd), e.r));
            t.put("}");
        }
        t.put("}");
    }
    if (c.format >= 5) {   // v5 cards carry the cable of the run; older formats render as before
        const Cable& cb = c.cable;
        t.put(",\"cable\":{");
        if (cb.seq) t.put("\"seq\":%u,", unsigned(cb.seq));
        t.put("\"vid\":%u,\"pid\":%u,\"max_mv\":%u,\"max_ma\":%u,\"kind\":%u,\"observed\":%u,\"spoofed\":%u",
              unsigned(cb.vid), unsigned(cb.pid), unsigned(cb.max_mv), unsigned(cb.max_ma), unsigned(cb.kind),
              unsigned((cb.flags & kCableObserved) != 0), unsigned((cb.flags & kCableSpoofed) != 0));
        if (cb.flags & kCableSelfPowered) t.put(",\"self_powered\":1");   // only then: every other card renders as before
        // The Ra guard's reading when the identity was started (cards before this field: absent), and "started anyway".
        if (const unsigned g = (cb.flags & kCableGuardMask) >> kCableGuardShift; g && g <= 4)
            t.put(",\"guard\":\"%s\"", raguard::name(raguard::Topology(g - 1)));
        if (cb.flags & kCableGuardBypassed) t.put(",\"guard_bypassed\":1");
        if (cb.flags & kCableReattach) t.put(",\"reattach\":1");   // only then: every other card renders as before
        t.put("}");
        if (c.src_flags & kSrcOppoSvid) t.put(",\"oppo_svid\":1");   // only then: every other card renders as before
        if (const unsigned ma = scp_cap_ma(c)) t.put(",\"scp_ma\":%u", ma);   // the adapter's SCP current limit, when it was read
    }
    if (const unsigned n = ufcs_modes(c)) {   // v6 cards carry the UFCS modes (voltage range and the mode's current); none on older cards
        t.put(",\"ufcs\":[");
        for (unsigned i = 0; i < n; ++i) {
            const UfcsRange& u = c.ufcs[i];
            t.put("%s{\"lo_mv\":%u,\"hi_mv\":%u", i ? "," : "", unsigned(u.lo_dv) * 100u, unsigned(u.hi_dv) * 100u);
            if (u.ma) t.put(",\"max_ma\":%u", unsigned(u.ma));
            t.put("}");
        }
        t.put("]");
    }
    t.put("}");
    return t.size();
}

namespace {
// Volts the way Python's %g prints them at 10 mV resolution: 5, 9, 20, 3.3, 4.75.
void volts(TextBuf& t, unsigned mv) {
    t.put("%u", mv / 1000);
    if (mv % 1000) {
        t.put(".%u", (mv % 1000) / 100);
        if (mv % 100) t.put("%u", (mv % 100) / 10);
    }
}
constexpr unsigned kHandshakeMv = 5500;   // a group whose highest passing voltage is 5 V got a handshake, not a charge level
}  // namespace

std::size_t render_summary(const Card& c, std::span<char> out) {
    TextBuf t(out);
    const Counts n = c.counts();
    t.put("Card %u \xC2\xB7 %s \xC2\xB7 %u PDOs \xC2\xB7 %u runs", unsigned(c.card_no), c.name[0] ? c.name : "(unnamed)",
          unsigned(c.num_pdos), unsigned(c.runs));
    if (const char* b = brand(c.id.vid)) t.put(" \xC2\xB7 %s", b);
    t.put(" %04X:%04X", unsigned(c.id.vid), unsigned(c.id.pid));
    if (c.id.pdp_w) t.put(" \xC2\xB7 %u W", unsigned(c.id.pdp_w));
    t.put(" \xC2\xB7 fw %08x\n", unsigned(c.fw));
    t.put("%u pass, %u fail, %u skip \xC2\xB7 %s", unsigned(n.pass), unsigned(n.fail), unsigned(n.skip), ct::name(c.state));
    if (c.reason != ct::Reason::None) t.put(" (%s)", ct::name(c.reason));
    t.put("\nIdentity %08X \xC2\xB7 boot %u \xC2\xB7 uptime %u s", unsigned(c.id.fp), unsigned(c.boots), unsigned(c.uptime_s));
    if (c.times > 1)
        t.put(" \xC2\xB7 same result x%u, last boot %u uptime %u s", unsigned(c.times), unsigned(c.last_boots), unsigned(c.last_uptime_s));
    if (c.format >= 8 && c.rebalanced)
        t.put(" \xC2\xB7 source rebalanced x%u", unsigned(c.rebalanced));
    t.put("\n");
    if (c.handover != ct::HandoverOutcome::NotTried) {
        t.put("Handover: %s after ", kHandover[unsigned(c.handover)]);
        t.put("%u.%u s\n", unsigned(c.handover_ds) / 10, unsigned(c.handover_ds) % 10);
    }
    t.put("\n");
    bool any_skip = false;
    for (unsigned g = 0; g < kGroups; ++g) {
        const GroupResult& r = c.group[g];
        if (r.verdict == GV::Skip && r.reason == uint8_t(ct::Reason::NoQc3Plus)) { t.put("- %s: no QC3+\n", ct::name(ct::Group(g))); continue; }   // not folded into "skipped"
        if (r.verdict == GV::Skip) { any_skip = true; continue; }
        if (r.verdict != GV::Pass && r.verdict != GV::Fail) continue;
        t.put("- %s: ", ct::name(ct::Group(g)));
        const bool handshake = r.max_mv && r.max_mv <= kHandshakeMv;
        if (group_handshake_ok(r)) {
            t.put("\xE2\x9C\x93 handshake ok");
        } else if (group_detected(r)) {
            t.put("\xE2\x9C\x93 detected");
        } else if (r.verdict == GV::Pass) {
            if (handshake) t.put("\xE2\x9C\x93 5 V only");
            else if (r.max_mv) { t.put("\xE2\x9C\x93 up to "); volts(t, r.max_mv); t.put(" V"); if (g == 0 && c.pd_ma) { t.put(", "); volts(t, c.pd_ma); t.put(" A"); } }
            else t.put("\xE2\x9C\x93");
        } else {
            if (handshake) t.put("5 V only, ");
            else if (r.max_mv) { t.put("\xE2\x9C\x93 up to "); volts(t, r.max_mv); t.put(" V, "); }
            const char* why = r.detail ? group_why(g, r) : r.reason ? ct::name(ct::Reason(r.reason)) : "";
            t.put("\xE2\x9C\x97");
            if (r.fail_mv) { t.put(" "); volts(t, r.fail_mv); t.put(" V"); }
            if (*why) t.put(r.fail_mv ? " (%s)" : " %s", why);
        }
        t.put("\n");
    }
    if (any_skip) {
        t.put("- ");
        bool first = true;
        for (unsigned g = 0; g < kGroups; ++g) {
            if (c.group[g].verdict != GV::Skip || c.group[g].reason == uint8_t(ct::Reason::NoQc3Plus)) continue;
            t.put("%s%s", first ? "" : ", ", ct::name(ct::Group(g)));
            first = false;
        }
        t.put(": skipped\n");
    }
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        const Range& r = c.range[i];
        // "- PPS 3.3-11V 3A: ...", the same words as the Results chip (step_label).
        ct::Step row;
        row.algorithm = ct::Algorithm(r.alg);
        t.put("- %s %s: ", step_label::protocol(row).c_str(),
              step_label::range(unsigned(r.lo_dv) * 100u, unsigned(r.hi_dv) * 100u, r.ma).c_str());
        if (r.fail_dv) {
            t.put("\xE2\x9C\x97 at "); volts(t, unsigned(r.fail_dv) * 100u); t.put(" V");
            if (r.max_dv) { t.put(", reached "); volts(t, unsigned(r.max_dv) * 100u); t.put(" V"); }
        } else if (r.max_dv >= r.hi_dv) {
            t.put("\xE2\x9C\x93 whole range");
        } else if (r.max_dv) {
            t.put("reached "); volts(t, unsigned(r.max_dv) * 100u); t.put(" V");
        } else {
            t.put("no step measured");
        }
        t.put("\n");
    }
    return t.size();
}

}  // namespace k2::report_card
