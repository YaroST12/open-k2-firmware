// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_tools.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/ui/text/tools.hpp"

namespace k2 {

const char *tool_hint(int tool) { return text::tools::kHints[tool_built(tool) ? tool : 0]; }

// Each tool owns its predicate (a function of its own) and declares what it is hurt by. Whether the far
// port is occupied is the same fact for all of them (SetupState::inline_mode); what it means differs:
//   Load:   the tool draws or asks for power, so a sink downstream would take what it asks for.
//   Inline: the tool only talks to the cable or the partner, so something downstream hides or hijacks the far end.
//   Nothing: the tool is passive, or measures under the user's own load.
namespace {
using namespace text::tools;

Readiness fc_trigger(const SetupState &st, bool) {   // no on-device inline override
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLoadAttached};
    return {};
}

Readiness pd_trigger(const SetupState &st, bool) {   // refuses without a PD source or with a load
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLoadAttached};
    if (!setup_hint(Tool::PdTrigger, st).ready) return {ToolLevel::Blocked, kTagNoPd, kNoPd};
    return {};
}

Readiness cable_r(const SetupState &, bool pm_online) {   // reads V and I from the power monitor; wants a load
    if (!pm_online) return {ToolLevel::Blocked, kTagNoPm, kNoPm};
    return {};
}

// Connection (reads what the engine holds; its active actions carry their own inline confirm), Black Box and PD diagnosis
// (passive reads), Report cards (read-only) and Diag (counters).
Readiness always_ready(const SetupState &, bool) { return {}; }

Readiness spoof(const SetupState &st, bool) {   // refused while a live sink is inline (Auto-CRC is off then)
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLiveSink};
    return {};
}

// PD Stress asks the charger for every level it offers: nothing may sit on the output, it reads VBUS and IBUS from the power
// monitor, and it needs a PD source list (the run itself refuses the same three; this only tags the row).
Readiness pd_stress(const SetupState &st, bool pm_online) {
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLoadAttached};
    if (!pm_online) return {ToolLevel::Blocked, kTagNoPm, kNoPm};
    if (!setup_hint(Tool::PdTrigger, st).ready) return {ToolLevel::Blocked, kTagNoPd, kNoPd};
    return {};
}

struct ToolRule {
    ToolHurt hurt;
    Readiness (*check)(const SetupState &, bool pm_online);
};

constexpr ToolRule kRules[kToolSlots] = {
    {ToolHurt::Load, fc_trigger},        // 0 FC trigger
    {ToolHurt::Load, pd_trigger},        // 1 PD trigger
    {ToolHurt::Load, spoof},             // 2 Spoof (shortcut into Cable read's Spoof list)
    {ToolHurt::Nothing, cable_r},        // 3 Cable R
    {ToolHurt::Nothing, always_ready},   // 4 Charger ID (reads; Refresh confirms on its own)
    {ToolHurt::Nothing, always_ready},   // 5 Cable read (its read carries the inline confirm)
    {ToolHurt::Nothing, always_ready},   // 6 Device ID (Identify carries the inline confirm)
    {ToolHurt::Nothing, always_ready},   // 7 Black Box
    {ToolHurt::Nothing, always_ready},   // 8 PD diagnosis (passive: reads the Black Box records)
    {ToolHurt::Nothing, always_ready},   // 9 Report cards
    {ToolHurt::Nothing, always_ready},   // 10 Diag
    {ToolHurt::Nothing, always_ready},   // 11 Device twins (the live proof refuses a live sink itself)
    {ToolHurt::Nothing, cable_r},        // 12 Loop probe (every reading comes from the power monitor; it refuses above 5.5 V itself)
    {ToolHurt::Load, pd_stress},         // 13 PD Stress
};

}  // namespace

ToolHurt tool_hurt_by(int tool) {
    return tool_built(tool) ? kRules[tool].hurt : ToolHurt::Nothing;
}

Readiness tool_readiness(int tool, const SetupState &st, bool pm_online) {
    if (!tool_built(tool)) return {};
    return kRules[tool].check(st, pm_online);
}

// ---- the Bench ------------------------------------------------------------------------------------------------------------

namespace bench {
namespace {
// Charger, Cable and Device on the Bench are info-only (their active tests live in the Tests carousel, ui_tests.cpp).
// Tests, Log and K2 are pages of their own: none of the Bench places carry inline tool actions here.
constexpr const char *kPlaceNames[kPlaces] = {"Meter", "Charger", "Cable", "Device", "Tests", "Log", "K2"};

void put_volts(StrBuf<kStatusChars + 1> &s, int32_t mv) {
    if (mv % 1000 == 0) s.put_u32(static_cast<uint32_t>(mv / 1000));
    else s.put_fixed(mv, mv % 100 == 0 ? 1 : 2);
    s.put_char('V');
}
void put_amps(StrBuf<kStatusChars + 1> &s, int32_t ma) {
    if (ma % 1000 == 0) s.put_u32(static_cast<uint32_t>(ma / 1000));
    else s.put_fixed(ma, ma % 100 == 0 ? 1 : 2);
    s.put_char('A');
}
// "1.0.1-6" of "1.0.1-6-e67edd0e[-dirty]": the release and the iteration, the hash stays in Boot info.
std::string_view short_version(const char *v) {
    const std::string_view s = v ? v : "";
    const size_t a = s.find('-');
    if (a == std::string_view::npos) return s;
    const size_t b = s.find('-', a + 1);
    return b == std::string_view::npos ? s : s.substr(0, b);
}
}  // namespace

Acts actions(Place) {
    return {nullptr, 0};
}

const char *place_name(Place p) { return kPlaceNames[static_cast<int>(p) < kPlaces ? static_cast<int>(p) : 0]; }

const char *act_hint(const Act &a) { return a.hint ? a.hint : a.kind == ActKind::Tool ? tool_hint(a.arg) : ""; }

Row row(Place p, const Facts &f) {
    Row r;
    auto &s = r.status;
    switch (p) {
    case Place::Meter:
        r.known = f.vbus;
        if (!f.vbus) { s.put("No VBUS"); break; }
        s.put_abs_fixed(f.mw, f.mw >= 99995 ? 1 : 2).put("W  ").put_fixed(f.mv, 2).put("V  ").put_abs_fixed(f.ma, 3).put_char('A');
        break;
    case Place::Charger:
        r.known = f.caps || f.fc;
        if (f.caps) {
            s.put(f.spec_rev >= 2 ? "PD3.x " : f.spec_rev == 1 ? "PD2.0 " : "PD ");
            s.put_u32((f.max_mw + 500) / 1000).put("W, ").put_u32(f.offers).put(f.offers == 1 ? " offer" : " offers");
        } else if (f.fc) {
            s.put(f.fc).put(" on the lines");
        } else {
            s.put("None on either port");
        }
        break;
    case Place::Cable:
        r.known = f.cable;
        if (!f.cable) { s.put("Not read"); break; }
        s.put_u32(f.cable_a).put("A ").put_u32(f.cable_v).put_char('V');
        if (f.cable_w) s.put(", ").put_u32(f.cable_w).put_char('W');
        break;
    case Place::Device:
        r.known = f.contract;
        if (!f.contract) { s.put("None inline"); break; }
        s.put("Asks ");
        put_volts(s, f.ctr_mv);
        s.put_char(' ');
        put_amps(s, f.ctr_ma);
        if (f.vbus && f.mw >= 1000) s.put(", gets ").put_u32(static_cast<uint32_t>((f.mw + 500) / 1000)).put_char('W');
        break;
    case Place::Tests:
        r.known = f.batt || f.caps || f.cable || f.contract;
        s.put((f.batt && !f.batt_status.view().empty()) ? f.batt_status.view()
                                                        : std::string_view("Charger, cable, batt, dev"));
        break;
    case Place::Log:
        r.known = true;
        s.put(f.diag.view().empty() ? std::string_view("No session") : f.diag.view());
        if (f.captures) s.put(", ").put_u32(f.captures).put(f.captures == 1 ? " capture" : " captures");
        break;
    case Place::K2:
        r.known = true;
        s.put(short_version(f.version)).put(f.hid ? ", HID on" : f.webusb ? ", WebUSB on" : ", HID off");
        break;
    }
    return r;
}

void fact(const Facts &f, StrBuf<28> &out) {
    if (f.caps) out.put("Charger on a port, PD");
    else if (f.fc) out.put("Charger on a port, ").put(f.fc);
    else if (f.vbus) out.put("Power on the line");
    else out.put("Nothing attached");
}

}  // namespace bench

}  // namespace k2
