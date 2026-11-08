// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_match.hpp"

#include <algorithm>

namespace k2 {

namespace {

bool augmented(pd::PdoType t) { return t == pd::PdoType::Pps || t == pd::PdoType::Avs; }
bool usable(const pd::Pdo &p) { return p.type != pd::PdoType::Unknown; }

// Compatible = same class (basic vs augmented) and overlapping voltage spans.
bool compatible(const pd::Pdo &src, const pd::Pdo &snk) {
    return usable(src) && usable(snk) && augmented(src.type) == augmented(snk.type) &&
           src.min_mv <= snk.max_mv && snk.min_mv <= src.max_mv;
}

void put_amps(StrBuf<32> &o, uint32_t ma) {
    o.put_u32(ma / 1000);
    uint32_t f = ma % 1000;
    if (f == 0) { o.put_char('A'); return; }
    o.put_char('.');
    char d[3] = {char('0' + f / 100), char('0' + f / 10 % 10), char('0' + f % 10)};
    int n = 3;
    while (n > 1 && d[n - 1] == '0') n--;
    for (int i = 0; i < n; i++) o.put_char(d[i]);
    o.put_char('A');
}

void put_volts(StrBuf<32> &o, uint32_t mv) {
    if (mv % 1000 == 0) o.put_u32(mv / 1000);
    else o.put_fixed(int32_t(mv), 1);
}

}  // namespace

void OfferBook::merge(const pd::SourceCaps &latest) {
    if (!latest.valid || latest.num_pdos == 0) return;
    const int n = std::min<int>(latest.num_pdos, int(pd::kMaxPdos));
    const pd::SourceCaps keep = caps;
    // One source only: the lists must agree position by position over the common part (so PDO1 is
    // identical too). A list that is neither a prefix nor an extension of the book is another charger.
    bool same = keep.valid && keep.num_pdos > 0;
    for (int i = 0; same && i < std::min<int>(n, keep.num_pdos); i++) same = latest.pdos[i].raw == keep.pdos[i].raw;
    if (!same) { caps = latest; caps.num_pdos = static_cast<uint8_t>(n); offered = (1u << n) - 1; return; }
    caps = latest;   // header fields (spec rev, EPR, rx count) follow the latest message
    for (int i = n; i < keep.num_pdos; i++) caps.pdos[i] = keep.pdos[i];
    caps.num_pdos = static_cast<uint8_t>(std::max<int>(n, keep.num_pdos));
    offered = (1u << n) - 1;
}

MatchResult match_compute(const MatchInput &in) {
    MatchResult r;
    const pd::SourceCaps &caps = in.caps;
    r.caps_seen = caps.valid && caps.num_pdos > 0;
    r.request_seen = in.contract.valid && in.contract.pos > 0;
    if (r.request_seen) {
        r.want_pos = in.contract.pos;
        r.want_ma = in.contract.max_ma ? in.contract.max_ma : in.contract.op_ma;
    }
    if (!r.caps_seen) { r.verdict = Verdict::NoCaps; return r; }

    r.rows = std::min<int>(caps.num_pdos, int(pd::kMaxPdos));
    for (int i = 0; i < r.rows; i++) {
        if (!(in.offered >> i & 1)) { r.gone++; continue; }
        r.src_max_mv = std::max<uint32_t>(r.src_max_mv, caps.pdos[i].max_mv);
        r.src_max_mw = std::max<uint32_t>(r.src_max_mw, caps.pdos[i].max_mw);
    }

    pd::Pdo snk[pd::kMaxPdos];
    int nsnk = 0;
    for (uint32_t raw : in.sink) {
        if (nsnk >= int(pd::kMaxPdos)) break;
        pd::Pdo p = pd::decode_pdo(raw);
        if (!usable(p)) continue;
        snk[nsnk++] = p;
        r.sink_max_mv = std::max<uint32_t>(r.sink_max_mv, p.max_mv);
    }
    r.sink_ok = nsnk > 0;

    if (!r.request_seen) { r.verdict = Verdict::NoRequest; }

    const bool inside = r.request_seen && r.want_pos <= r.rows && usable(caps.pdos[r.want_pos - 1]) &&
                        (in.offered >> (r.want_pos - 1) & 1);
    if (r.request_seen) {
        if (inside) {
            const pd::Pdo &sel = caps.pdos[r.want_pos - 1];
            r.want_mv = in.contract.mv ? in.contract.mv : sel.max_mv;
        } else {
            r.want_mv = in.contract.mv;
            r.flags |= kFlagOutsideCaps;
        }
        r.want_mw = r.want_mv * r.want_ma / 1000;
    }

    for (int i = 0; i < r.rows; i++) {
        const pd::Pdo &src = caps.pdos[i];
        const bool selected = r.request_seen && i + 1 == r.want_pos;
        if (!(in.offered >> i & 1)) { r.row[i] = RowState::Gone; continue; }
        if (!usable(src)) continue;  // unused slot stays Unknown
        uint32_t need = 0;
        bool comp = false;
        for (int k = 0; k < nsnk; k++)
            if (compatible(src, snk[k])) { comp = true; need = std::max<uint32_t>(need, snk[k].max_ma); }
        if (selected && !(comp && r.sink_ok)) need = r.want_ma;
        const bool low = (comp || selected) && need > src.max_ma;
        if (selected) {
            r.row[i] = low ? RowState::SelectedLow : RowState::Selected;
            r.need_ma = need;
            r.have_ma = src.max_ma;
            if (low) r.flags |= kFlagCurrentLimited;
        } else if (!r.sink_ok) {
            r.row[i] = RowState::Unknown;
        } else {
            r.row[i] = !comp ? RowState::No : low ? RowState::Low : RowState::Ok;
        }
    }

    if (r.sink_ok && r.sink_max_mv > r.src_max_mv) r.flags |= kFlagWantsHigherV;
    if (in.mismatch == Tri::Yes) r.flags |= kFlagMismatchBit;

    if (!r.request_seen) return r;
    if (r.flags & kFlagOutsideCaps) r.verdict = Verdict::OutsideCaps;
    else if (r.flags & kFlagWantsHigherV) r.verdict = Verdict::WantsHigherV;
    else if (r.flags & kFlagCurrentLimited) r.verdict = Verdict::CurrentLimited;
    else if (r.flags & kFlagMismatchBit) r.verdict = Verdict::MismatchBit;
    else if (!r.sink_ok) r.verdict = Verdict::SinkUnknown;
    else r.verdict = Verdict::Matches;
    return r;
}

Agreement agreement(const MatchResult &r) {
    if (r.typec_ma && !r.caps_seen) return Agreement::TypeCOnly;
    if (!r.request_seen) return Agreement::NoContract;
    constexpr int kPartial = kFlagMismatchBit | kFlagWantsHigherV | kFlagCurrentLimited | kFlagOutsideCaps;
    return (r.flags & kPartial) ? Agreement::Partial : Agreement::Full;
}

Severity verdict_severity(Verdict v) {
    switch (v) {
        case Verdict::Matches: return Severity::Ok;
        case Verdict::SinkUnknown: return Severity::Dim;
        default: return Severity::Caution;
    }
}

bool gone_footer(const MatchResult &r, StrBuf<32> &out) {
    out.clear();
    if (r.gone <= 0) return false;
    out.put_u32(uint32_t(r.gone)).put(r.gone == 1 ? " offer" : " offers").put(" not sent now");
    return true;
}

void match_footer(const MatchResult &r, StrBuf<32> &out) {
    out.clear();
    if (r.typec_ma && !r.caps_seen) {
        out.put("No PD: Type-C ");
        put_amps(out, r.typec_ma);
        out.put(" source");
        return;
    }
    switch (r.verdict) {
        case Verdict::NoCaps: out.put("Waiting for Source_Caps"); break;
        case Verdict::NoRequest: out.put("Replug device on the charger"); break;
        case Verdict::OutsideCaps: out.put("Request PDO").put_u32(uint32_t(r.want_pos)).put(" not in caps"); break;
        case Verdict::WantsHigherV:
            out.put("Wants ");
            put_volts(out, r.sink_max_mv);
            out.put("V, charger max ");
            put_volts(out, r.src_max_mv);
            out.put_char('V');
            break;
        case Verdict::CurrentLimited:
            out.put("Charger gives ");
            put_amps(out, r.have_ma);
            out.put(", wants ");
            put_amps(out, r.need_ma);
            if (out.view().size() > 30) {
                out.clear();
                out.put("Gives ");
                put_amps(out, r.have_ma);
                out.put(", wants ");
                put_amps(out, r.need_ma);
            }
            break;
        case Verdict::MismatchBit: out.put("Capability mismatch bit set"); break;
        case Verdict::SinkUnknown: out.put("Sink caps not seen"); break;
        case Verdict::Matches: out.put("Request matches the offer"); break;
    }
}

void sink_summary(std::span<const uint32_t> sink, StrBuf<32> &out) {
    out.clear();
    bool any = false, pps = false, avs = false;
    StrBuf<32> list;
    for (uint32_t raw : sink) {
        pd::Pdo p = pd::decode_pdo(raw);
        if (!usable(p)) continue;
        any = true;
        if (p.type == pd::PdoType::Pps) pps = true;
        else if (p.type == pd::PdoType::Avs) avs = true;
        else {
            list.put_char(' ');
            put_volts(list, p.max_mv);
        }
    }
    if (!any) { out.put("Sink caps not seen"); return; }
    out.put("Sink caps").put(list.view());
    if (!list.view().empty()) out.put_char('V');
    if (pps) out.put(" +PPS");
    if (avs) out.put(" +AVS");
}

const char *request_state_name(const pd::Contract &c) {
    if (!c.valid) return "";
    if (c.ready) return "Ready";
    if (c.rejected) return "Rejected";
    if (c.accepted) return "Accepted";
    return "Requested";
}

void rdo_flags_text(uint8_t flags, std::size_t room, StrBuf<32> &out) {
    out.clear();
    struct F { uint8_t bit; const char *name; };
    static constexpr F kFlags[] = {   // the abnormal one first, so a short row still shows it
        {pd::kRdoFMismatch, "Mismatch"}, {pd::kRdoFGiveBack, "Give-back"}, {pd::kRdoFUsbComm, "USB comm"},
        {pd::kRdoFNoSuspend, "No suspend"}, {pd::kRdoFEpr, "EPR"},
    };
    // Whole names only; what does not fit is dropped silently (the list is most-important-first), never a
    // bare "+". Empty output is fine: the caller checks for it.
    std::size_t used = 0, shown = 0;
    for (const F &f : kFlags) {
        if (!(flags & f.bit)) continue;
        const std::size_t need = (shown ? 2 : 0) + std::string_view(f.name).size();
        if (used + need > room) break;
        if (shown) out.put(", ");
        out.put(f.name);
        used += need;
        shown++;
    }
}


// ---- wants list ----

WantsList pick_wants(const pd::SinkCapsSeen &sniffed, bool ident_current, const pd::IdStatus &id) {
    if (sniffed.valid && sniffed.n > 0) return {std::span<const uint32_t>(sniffed.raw.data(), sniffed.n), WantsSrc::Sniffed};
    if (ident_current && (id.ok_mask >> int(pd::IdStep::SinkCap) & 1) && id.num_sink_pdos > 0)
        return {std::span<const uint32_t>(id.sink_pdos.data(), id.num_sink_pdos), WantsSrc::Identify};
    return {};
}

const char *wants_src_name(WantsSrc s) {
    return s == WantsSrc::Sniffed ? "sniffed" : s == WantsSrc::Identify ? "from Identify" : "";
}

SinkFlags sink_flags(std::span<const uint32_t> sink) {
    SinkFlags f;
    if (sink.empty() || (sink[0] >> 30) != 0) return f;   // flags live in a fixed first PDO only
    const uint32_t r = sink[0];
    f.valid = true;
    f.dual_role_power = r >> 29 & 1;
    f.higher_cap = r >> 28 & 1;
    f.unconstrained = r >> 27 & 1;
    f.usb_comm = r >> 26 & 1;
    f.dual_role_data = r >> 25 & 1;
    f.fr_swap = uint8_t(r >> 23 & 3);
    return f;
}

void sink_flags_lines(const SinkFlags &f, StrBuf<34> &l1, StrBuf<34> &l2) {
    l1.clear();
    l2.clear();
    const char *tok[6];
    int n = 0;
    if (f.dual_role_power) tok[n++] = "Dual-role";
    if (f.dual_role_data) tok[n++] = "DR data";
    if (f.usb_comm) tok[n++] = "USB comm";
    if (f.unconstrained) tok[n++] = "Unconstrained";
    if (f.higher_cap) tok[n++] = "Higher cap";
    if (f.fr_swap) tok[n++] = f.fr_swap == 1 ? "FR USB" : f.fr_swap == 2 ? "FR1.5A" : "FR3A";
    bool second = false;
    for (int i = 0; i < n; i++) {
        const size_t need = std::string_view(tok[i]).size();
        StrBuf<34> &cur = second ? l2 : l1;
        const size_t used = cur.view().size();
        if (used + (used ? 1 : 0) + need > 31) {
            if (second) break;   // a third line does not exist
            second = true;
            i--;
            continue;
        }
        if (used) cur.put_char(' ');
        cur.put(tok[i]);
    }
}

namespace {
// Can the offer supply the sink PDO's voltage: same class and span, or a fixed sink inside an offered range.
bool offer_fits(const pd::Pdo &src, const pd::Pdo &snk) {
    if (!usable(src) || !usable(snk)) return false;
    const bool a_src = augmented(src.type), a_snk = augmented(snk.type);
    if (snk.type == pd::PdoType::Fixed) {
        if (src.type == pd::PdoType::Fixed) return src.max_mv == snk.max_mv;
        return a_src && src.min_mv <= snk.max_mv && snk.max_mv <= src.max_mv;
    }
    if (a_snk) return a_src && src.min_mv <= snk.max_mv && snk.min_mv <= src.max_mv;
    // Battery / variable: a fixed offer inside the sink's span, or an augmented range overlapping it.
    if (src.type == pd::PdoType::Fixed) return snk.min_mv <= src.max_mv && src.max_mv <= snk.max_mv;
    return a_src && src.min_mv <= snk.max_mv && snk.min_mv <= src.max_mv;
}
}  // namespace

WantsLayout wants_layout(const pd::SourceCaps &caps, uint32_t offered, std::span<const uint32_t> sink, int want_pos) {
    WantsLayout w;
    const int rows = std::min<int>(caps.num_pdos, int(pd::kMaxPdos));
    for (int i = 0; i < rows; i++) {
        w.row[i].src = int8_t(i);
        w.row[i].contract = want_pos == i + 1;
    }
    w.n = rows;
    for (size_t j = 0; j < sink.size() && j < pd::kMaxPdos; j++) {
        const pd::Pdo snk = pd::decode_pdo(sink[j]);
        if (!usable(snk)) continue;
        int fits = 0, place = -1, exact = -1;
        for (int i = 0; i < rows; i++) {
            if (!(offered >> i & 1) || !offer_fits(caps.pdos[i], snk)) continue;
            fits++;
            if (w.row[i].sink >= 0) continue;
            if (place < 0) place = i;
            if (exact < 0 && caps.pdos[i].type == snk.type) exact = i;
        }
        const int at = exact >= 0 ? exact : place;
        if (at >= 0) { w.row[at].sink = int8_t(j); continue; }
        WantsRow &x = w.row[w.n++];
        x.sink = int8_t(j);
        x.unmet = fits == 0;
        if (x.unmet) w.unmet++;
    }
    return w;
}

}  // namespace k2
