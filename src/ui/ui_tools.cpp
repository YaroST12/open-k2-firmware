// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_tools.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/ui/text/tools.hpp"

namespace k2 {

const char *tool_hint(int tool) { return text::tools::kHints[tool < 0 || tool >= kToolCount ? 0 : tool]; }

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
    return tool < 0 || tool >= kToolCount ? ToolHurt::Nothing : kRules[tool].hurt;
}

Readiness tool_readiness(int tool, const SetupState &st, bool pm_online) {
    if (tool < 0 || tool >= kToolCount) return {};
    return kRules[tool].check(st, pm_online);
}

// ---- the Bench ------------------------------------------------------------------------------------------------------------

namespace bench {
namespace {
using K = ActKind;
constexpr uint8_t pg(Page p) { return static_cast<uint8_t>(p); }

// Tool slots (ui_tools.hpp / text/tools.hpp): 0 FC trigger, 1 PD trigger, 2 Spoof, 3 Cable R, 4 Charger ID, 5 Cable read,
// 6 Device ID, 7 Black Box, 8 PD diagnosis, 9 Report cards, 10 Diag, 11 Device twins, 12 Loop probe, 13 PD Stress.
// Charger, Cable and Device are the place pages (the Connection page in place mode, conn::place_actions): their own reads
// (Charger ID, Cable read with Spoof and Saved cables, Device ID) are the page itself; these are the tools it adds.
constexpr Act kCharger[] = {
    {"Test charger", K::Page, pg(Page::ChargerTest), "grades every offer"},
    {"Request voltage", K::Tool, 1, nullptr},
    {"Fast charge", K::Tool, 0, nullptr},
    {"Report cards", K::Tool, 9, nullptr},
#if K2_PDSTRESS
    {"Stress test", K::Tool, 13, nullptr},
#endif
};
constexpr Act kCable[] = {
    {"Resistance", K::Tool, 3, nullptr},
    {"Loop probe", K::Tool, 12, nullptr},
};
constexpr Act kDevice[] = {
    {"Twins", K::Tool, 11, nullptr},
};
// Battery, Log and K2 are pages of their own (the efficiency recorder, the PD diagnosis with its Black Box row, the settings
// with System state and Boot info): they add no Bench tools.
constexpr const char *kPlaceNames[kPlaces] = {"Meter", "Charger", "Cable", "Device", "Battery", "Log", "K2"};

template <size_t N> constexpr Acts acts(const Act (&a)[N]) { return {a, static_cast<int>(N)}; }

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

Acts actions(Place p) {
    switch (p) {
    case Place::Charger: return acts(kCharger);
    case Place::Cable: return acts(kCable);
    case Place::Device: return acts(kDevice);
    case Place::Battery:
    case Place::Log:
    case Place::K2:
    case Place::Meter: break;
    }
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
    case Place::Battery:
        r.known = f.batt;
        s.put(f.batt_status.view().empty() ? std::string_view("No run") : f.batt_status.view());
        break;
    case Place::Log:
        r.known = true;
        s.put(f.diag.view().empty() ? std::string_view("No session") : f.diag.view());
        if (f.captures) s.put(", ").put_u32(f.captures).put(f.captures == 1 ? " capture" : " captures");
        break;
    case Place::K2:
        r.known = true;
        s.put(short_version(f.version)).put(f.hid ? ", HID on" : ", HID off");
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
