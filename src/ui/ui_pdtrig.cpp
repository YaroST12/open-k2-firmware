// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_pdtrig.hpp"

#include <algorithm>

#include "k2/core/strpack.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_wizard.hpp"

namespace k2 {

namespace {
template <size_t N>
void put_volts(StrBuf<N> &s, uint32_t mv) {
    if (mv % 1000 == 0) s.put_u32(mv / 1000);
    else if (mv % 100 == 0) s.put_fixed(static_cast<int32_t>(mv), 1);
    else if (mv % 10 == 0) s.put_fixed(static_cast<int32_t>(mv), 2);
    else s.put_fixed(static_cast<int32_t>(mv), 3);
}
}  // namespace

bool trigger_needs_confirm(const pd::SourceCaps &caps, pd::Choice c, uint32_t cap_mv) {
    if (c.pos == 0) return false;  // reading the EPR list changes no voltage
    const pd::Plan p = pd::plan_request(caps, c, cap_mv, c.pos >= 8);
    return p.err == pd::Err::Ok && p.mv > pd::kVsafe5vMv;
}

// ---- Identify rows ----

namespace {
void add(IdRow *rows, int &n, RowTone tone, auto &&fill) {
    if (n >= kMaxIdRows) return;
    rows[n].text.clear();
    rows[n].tone = tone;
    fill(rows[n].text);
    n++;
}
}  // namespace

void identify_status_text(const pd::IdStatus &st, StrBuf<24> &out) {
    switch (st.state) {
        case pd::IdState::Idle: out.put(K2_P("OK: identify device")); break;
        case pd::IdState::Running: out.put(pd::id_step_name(st.step)).put("..."); break;
        case pd::IdState::Done:
            if (!st.ok_mask) out.put(K2_P("Device didn't answer"));
            else out.put(K2_P("Done, ")).put_u32(__builtin_popcount(st.ok_mask)).put(K2_P("/6 answered"));
            break;
        case pd::IdState::Aborted: {
            out.put(K2_P("Aborted: "));
            switch (st.why) {
                case pd::IdWhy::Detached: out.put(K2_P("unplugged")); break;
                case pd::IdWhy::HardReset: out.put(K2_P("hard reset")); break;
                case pd::IdWhy::Stopped: out.put(K2_P("stopped")); break;
                default: out.put(K2_P("link dropped")); break;
            }
            break;
        }
    }
}

void identify_result_text(const pd::IdStatus &st, StrBuf<64> &out) {
    if (!st.skipped_mask) { out.put(K2_P("Identified")); return; }
    out.put(K2_P("No reply:"));
    bool first = true;
    for (unsigned b = 0; b < 6; b++)
        if (st.skipped_mask & (1u << b)) {
            out.put(first ? " " : ", ").put(pd::id_step_short(static_cast<pd::IdStep>(b)));
            first = false;
        }
}

int identify_rows(const pd::IdStatus &st, const pd::VdmInfo &vdm, IdRow *rows) {
    int n = 0;
    const pd::Identity &id = vdm.identity;
    if (id.valid) {
        add(rows, n, RowTone::Head, [](auto &s) { s.put(K2_P("-- Identity --")); });
        // "VID xxxx " leaves 13 of the row's 22 cells: a longer name takes a row of its own, never a cut.
        const std::string_view vendor = id.vendor.empty() ? std::string_view("?") : id.vendor.view();
        const bool beside = vendor.size() <= 13;
        add(rows, n, RowTone::Value, [&](auto &s) { s.put("VID ").put_hex(id.vid, 4); if (beside) s.put_char(' ').put(vendor); });
        if (!beside) add(rows, n, RowTone::Value, [&](auto &s) { s.put(vendor); });
        add(rows, n, RowTone::Text, [&](auto &s) { s.put("PID ").put_hex(id.pid, 4).put(K2_P(" bcd ")).put_hex(id.bcd, 4); });
        if (id.model) add(rows, n, RowTone::Value, [&](auto &s) { s.put(id.model); });
    } else if (st.state == pd::IdState::Done) {
        add(rows, n, RowTone::Dim, [](auto &s) { s.put(K2_P("No identity answer")); });
    }
    const int svids = std::min<int>(vdm.num_svids, 4);
    for (int i = 0; i < svids; i++) {
        const pd::VendorName nm = pd::svid_name(vdm.svids[i]);
        const bool beside = nm.view().size() <= 12;   // "SVID xxxx " leaves 12 of 22; a longer name takes its own row
        add(rows, n, RowTone::Text, [&](auto &s) { s.put(K2_P("SVID ")).put_hex(vdm.svids[i], 4); if (beside) s.put_char(' ').put(nm.view()); });
        if (!beside) add(rows, n, RowTone::Text, [&](auto &s) { s.put(nm.view()); });
    }
    if (st.num_sink_pdos) {
        add(rows, n, RowTone::Head, [](auto &s) { s.put(K2_P("-- Sink wants --")); });
        for (int i = 0; i < st.num_sink_pdos; i++) {
            const pd::Pdo p = pd::decode_pdo(st.sink_pdos[i]);
            add(rows, n, RowTone::Text, [&](auto &s) {
                s.put_u32(i + 1).put_char(' ');
                s.put(p.type == pd::PdoType::Fixed ? "FIX " : p.type == pd::PdoType::Pps ? "PPS " : p.type == pd::PdoType::Avs ? "AVS " : "VAR ");
                if (p.type == pd::PdoType::Pps || p.type == pd::PdoType::Avs) { put_volts(s, p.min_mv); s.put_char('-'); }
                put_volts(s, p.max_mv);
                s.put_char('V');
                if (p.max_ma) { s.put_char(' ').put_fixed(int32_t(p.max_ma), 1, 0).put_char('A'); }
            });
        }
    }
    if (st.revision) {
        add(rows, n, RowTone::Text, [&](auto &s) {
            s.put("PD ").put_u32(st.revision >> 28).put_char('.').put_u32((st.revision >> 24) & 0xF)
             .put(K2_P(" ver ")).put_u32((st.revision >> 20) & 0xF).put_char('.').put_u32((st.revision >> 16) & 0xF);
        });
    }
    if (st.ok_mask & (1u << unsigned(pd::IdStep::Status))) add(rows, n, RowTone::Text, [](auto &s) { s.put(K2_P("Status: answered")); });
    if (st.skipped_mask) {
        add(rows, n, RowTone::Head, [](auto &s) { s.put(K2_P("-- No reply --")); });
        for (unsigned b = 0; b < 6; b++)
            if (st.skipped_mask & (1u << b))
                add(rows, n, RowTone::Dim, [&](auto &s) { s.put(pd::id_step_short(static_cast<pd::IdStep>(b))); });
    }
    return n;
}

namespace {
template <size_t N>
void put_cut(StrBuf<N> &s, const char *t, size_t max) {
    size_t i = 0;
    if (t) strpack::for_chars(t, [&](char c) { if (i++ < max) s.put_char(c); });
}
constexpr int kIdSvids = 4;
// Rows for the decoded product type and the USB role, each only when the ID header says something.
int kv_extra(const pd::Identity &id) {
    return (*pd::id_product_type(id.id_header) ? 1 : 0) + (*pd::id_usb_role(id.id_header) ? 1 : 0);
}
}

bool identify_card(const pd::IdStatus &st, const pd::VdmInfo &vdm, IdCard &out) {
    out = IdCard{};
    const pd::Identity &id = vdm.identity;
    if (st.state != pd::IdState::Done || !id.valid) return false;
    put_cut(out.name, id.model ? id.model : !id.vendor.empty() ? id.vendor.c_str() : K2_P("Device"), pd::kVendorMax);
    put_cut(out.vendor, id.vendor.c_str(), pd::kVendorMax);
    if (st.revision) out.pd.put("PD ").put_u32(st.revision >> 28).put_char('.').put_u32((st.revision >> 24) & 0xF);
    uint32_t best = 0;
    int bi = -1;
    for (int i = 0; i < st.num_sink_pdos; i++) {
        const pd::Pdo p = pd::decode_pdo(st.sink_pdos[i]);
        const uint32_t w = p.max_mv / 100 * (p.max_ma / 10);   // 0.1 V * 10 mA steps, enough to rank
        if (bi < 0 || w > best) { best = w; bi = i; }
    }
    if (bi >= 0) {
        const pd::Pdo p = pd::decode_pdo(st.sink_pdos[bi]);
        out.wants.put(K2_P("wants "));
        put_volts(out.wants, p.max_mv);
        out.wants.put_char('V');
        if (p.max_ma) out.wants.put_char(' ').put_fixed(int32_t(p.max_ma), 1, 0).put_char('A');
    }
    return true;
}

int identify_kv_count(const pd::IdStatus &, const pd::VdmInfo &vdm) {
    return 2 + kv_extra(vdm.identity) + std::min<int>(vdm.num_svids, kIdSvids);
}

void identify_kv(const pd::IdStatus &, const pd::VdmInfo &vdm, int i, StrBuf<12> &key, StrBuf<16> &val) {
    const int svids = std::min<int>(vdm.num_svids, kIdSvids);
    const pd::Identity &id = vdm.identity;
    const char *type = pd::id_product_type(id.id_header), *usb = pd::id_usb_role(id.id_header);
    if (i == 0) { key.put("VID"); val.put_hex(id.vid, 4); return; }
    if (i == 1) { key.put("PID"); val.put_hex(id.pid, 4); return; }
    i -= 2;
    if (*type) { if (i-- == 0) { key.put("Type"); val.put(type); return; } }
    if (*usb) { if (i-- == 0) { key.put("USB"); val.put(usb); return; } }
    if (i < svids) {
        const uint16_t sv = vdm.svids[i];
        key.put("SVID");
        val.put_hex(sv, 4).put_char(' ');
        const pd::VendorName nm = pd::svid_name(sv);
        size_t len = 0;
        while (nm.s[len] && nm.s[len] != '/' && len < 6) len++;   // "INTEL/TB" reads as "INTEL"
        put_cut(val, nm.c_str(), len);
    }
}

namespace {
constexpr SetupHint hint(const char *a, const char *b, const char *c, const char *d, int n, bool ready) {
    SetupHint h;
    h.line[0] = a; h.line[1] = b; h.line[2] = c; h.line[3] = d;
    h.lines = n; h.ready = ready;
    return h;
}
}

SetupHint setup_hint(Tool tool, const SetupState &st) {
    switch (tool) {
    case Tool::Identify:
        if (!st.attached) return hint(K2_P("Nothing connected."), K2_P("Plug the device in"), K2_P("(either port), wake it"), K2_P("then press Run."), 4, false);
        if (st.inline_mode) return hint(K2_P("Load on the other end"), K2_P("Asking would reach the"), K2_P("charger, not the load."), K2_P("OK twice to force it."), 4, false);
        return hint(K2_P("Device is on a port,"), K2_P("the other is empty."), K2_P("Press Run to ask it"), K2_P("who it is and needs."), 4, true);
    case Tool::Emarker:
        if (pd::emarker_mode(st.attached, st.vbus_known, st.vbus_mv) == pd::EmarkerMode::Vconn)
            return hint(K2_P("Cable into a port,"), K2_P("far end free."), K2_P("Press Scan: VCONN"), K2_P("powers its chip."), 4, true);
        if (st.inline_mode) return hint(K2_P("Inline: can't query,"), K2_P("listening only."), K2_P("Replug the charger end"), K2_P("while Listen runs."), 4, false);
        return hint(K2_P("Source on the cable."), K2_P("Scan only listens."), K2_P("Replug the charger end"), K2_P("then press Listen."), 4, true);
    case Tool::PdTrigger:
        if (st.inline_mode) return hint(K2_P("Load on the other end"), K2_P("Trigger needs one side"), K2_P("only: unplug the load."), "", 3, false);
        if (!st.attached) return hint(K2_P("Nothing connected."), K2_P("Plug a PD charger into"), K2_P("either port, the other"), K2_P("one empty."), 4, false);
        if (!st.caps_valid) return hint(K2_P("Charger gave no PD."), K2_P("Replug it into a port"), K2_P("then reopen this."), "", 3, false);
        return hint(K2_P("Charger on one port,"), K2_P("the other is empty."), "", "", 2, true);
    }
    return {};
}

SetupView setup_view(Tool tool, const SetupState &st) {
    SetupView v;
    v.ready = setup_hint(tool, st).ready;
    switch (tool) {
    case Tool::Identify:
        v.bottom_role = "DEV";
        v.n = 2;
        v.check[0] = K2_P("Device seen"); v.met[0] = st.attached;
        v.check[1] = K2_P("Other empty"); v.met[1] = !st.inline_mode;
        v.footer = !st.attached ? K2_P("Plug the device in") : st.inline_mode ? K2_P("Live sink: press twice") : K2_P("Ready");
        break;
    case Tool::Emarker:
        v.bottom_role = "CBL";
        v.n = 2;
        if (pd::emarker_mode(st.attached, st.vbus_known, st.vbus_mv) == pd::EmarkerMode::Vconn) {
            v.caption = "VCON";
            v.check[0] = K2_P("No source"); v.met[0] = true;
            v.check[1] = K2_P("Other empty"); v.met[1] = !st.inline_mode;
            v.footer = K2_P("Ready: VCONN powers it");
        } else {
            v.top_role = "CHG";
            v.check[0] = K2_P("Charger seen"); v.met[0] = true;
            v.check[1] = K2_P("Replug end"); v.met[1] = false;
            v.footer = st.inline_mode ? K2_P("Inline: listen only") : K2_P("Charger on: listen only");
        }
        break;
    case Tool::PdTrigger:
        v.bottom_role = "CHG";
        v.n = 3;
        v.check[0] = K2_P("Charger seen"); v.met[0] = st.attached;
        v.check[1] = K2_P("PD caps read"); v.met[1] = st.caps_valid;
        v.check[2] = K2_P("Other empty"); v.met[2] = !st.inline_mode;
        v.footer = st.inline_mode ? K2_P("Unplug the other device") : !st.attached ? K2_P("Plug PD charger in")
                 : !st.caps_valid ? K2_P("Charger gave no PD") : K2_P("Ready");
        break;
    }
    return v;
}

namespace {
// How the amps are worded: exact (3.25A, 5A; a whole number from 10 A up), rounded to a tenth, or left out.
enum class AmpsForm : uint8_t { Exact, Short, None };
void pdo_text(const pd::Pdo &p, AmpsForm form, StrBuf<24> &out) {
    const char *type = "VAR";
    switch (p.type) {
    case pd::PdoType::Fixed: type = "FIX"; break;
    case pd::PdoType::Pps: type = "PPS"; break;
    case pd::PdoType::Avs: type = "AVS"; break;
    case pd::PdoType::Battery: type = "BAT"; break;
    default: break;
    }
    out.put(type).put_char(' ');
    if (p.type == pd::PdoType::Pps || p.type == pd::PdoType::Avs) {
        put_volts(out, p.min_mv ? p.min_mv : 15000);
        out.put_char('-');
    }
    put_volts(out, p.max_mv);
    out.put_char('V');
    if (!p.max_ma || form == AmpsForm::None) return;
    out.put_char(' ');
    if (form == AmpsForm::Exact) {
        if (p.max_ma < 10000) put_volts(out, p.max_ma);
        else out.put_u32((p.max_ma + 500) / 1000);
        out.put_char('A');
    } else {
        StrBuf<8> a;
        put_short(a, p.max_ma, 'A');
        out.put(a.view());
    }
}
}  // namespace

static_assert(kPdoLineCells * static_cast<int>(kSmallAdvPx) <= kit::kTailX - kit::kTextX - 8, "a PDO row's text and tail run into each other");

void pdo_line(const pd::Pdo &p, PdoLine &out) {
    out = PdoLine{};
    out.tail.put_u32(p.max_mw / 1000).put_char('W');
    for (AmpsForm f : {AmpsForm::Exact, AmpsForm::Short, AmpsForm::None}) {
        out.text.clear();
        pdo_text(p, f, out.text);
        if (static_cast<int>(out.text.view().size() + out.tail.view().size()) <= kPdoLineCells) return;
    }
}

}  // namespace k2
