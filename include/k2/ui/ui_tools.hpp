// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools hub v2, pure logic: which of the tools can do its full job right now, the tag and the
// footer reason for it. Every verdict is derived from the same setup_hint() the tool's own page uses,
// so the hub never disagrees with the refusal inside the tool.
#pragma once
#include <cstdint>
#include "k2/core/build_config.h"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_pdtrig.hpp"
#include "k2/ui/ui_types.hpp"

namespace k2 {

enum class ToolLevel : uint8_t { Ready, Caution, Blocked };

struct Readiness {
    ToolLevel level = ToolLevel::Ready;
    const char *tag = "ready";       // the row's tail when not ready, at most 6 characters (text/tools.hpp)
    const char *reason = "";         // footer sentence when not ready, at most 27 characters; empty when ready
};

constexpr int kToolSlots = 14;       // the per-tool tables: PD Stress is the last slot, so a K2_PDSTRESS=0 build only shows one row less
constexpr int kToolCount = K2_PDSTRESS ? 14 : 13;   // rows shown: FC trigger, PD trigger, Spoof, Cable R, Charger ID, Cable read, Device ID, Black Box, PD diagnosis, Report cards, Diag, Device twins, Loop probe, PD Stress (ToolsPage::tool_menu maps to Menu)
constexpr int kToolSpoof = 2;        // the Spoof row opens Cable read's Spoof list, not a menu of its own
constexpr int kToolChargerId = 4;    // Charger ID, Cable read, Device ID: the Connection page on one tab each (Tab order)
constexpr int kToolCableRead = 5;
constexpr int kToolDeviceId = 6;
constexpr int kToolLoopProbe = 12;   // the HID-cable loop probe (needs the power monitor, like Cable R)
constexpr int kToolPdStress = 13;    // renegotiation stress: a PD charger, the power monitor, nothing on the output
constexpr int kToolRows = 13;        // one row per tool (the list scrolls: not all of them fit above the footer at once)

// Port-neutral setup hint under the tool name (at most 24 characters; never says which port).
const char *tool_hint(int tool);

// What the tool is hurt by when the far port is occupied (SetupState::inline_mode). Load: it draws or
// asks for power (the tag is LOAD, blocked). Inline: it only talks to the cable or the partner, so a
// device downstream hides or hijacks the far end (tag INLINE, caution). Nothing: passive or load-tolerant.
enum class ToolHurt : uint8_t { Nothing, Load, Inline };
ToolHurt tool_hurt_by(int tool);

// `pm_online`: the power monitor answers (Cable R needs it).
Readiness tool_readiness(int tool, const SetupState &st, bool pm_online);

// ---- the Bench (Fieldbook, CONCEPT 7): seven places, each a list of actions ----------------------------------------------
// The Bench replaced the page carousel and the Tools hub: Meter (back to the faces), Charger, Cable, Device, Battery, Log,
// K2. Every tool of the old hub is one place's action; a place adds the pages that used to sit in the carousel (Test
// charger, Settings) and the lists that hid behind a tool (Saved cables). Battery is the efficiency recorder's page
// (BatteryPage, k2/ui/ui_battery.hpp). Pure: the tables and the row texts; ToolsPage paints them.
namespace bench {

enum class Place : uint8_t { Meter, Charger, Cable, Device, Battery, Log, K2 };
inline constexpr int kPlaces = 7;
// A row's second line (font 12, at most kStatusChars) and whether its node is solid (something known) or hollow.
inline constexpr int kStatusChars = 28;

enum class ActKind : uint8_t { Tool, Page, Menu };   // arg: a tool slot (ui_tools), a Page, a Menu
struct Act {
    const char *name;
    ActKind kind;
    uint8_t arg;
    const char *hint;   // the fact line while the row is focused; nullptr = the tool's own (tool_hint)
};
struct Acts { const Act *a; int n; };

Acts actions(Place p);
const char *place_name(Place p);
const char *act_hint(const Act &a);

// What a Bench row reads. The Ui fills it from the drivers; the texts below are pure.
struct Facts {
    bool vbus = false;                  // the meter reads a live VBUS
    int32_t mv = 0, ma = 0, mw = 0;
    bool caps = false;                  // PD source capabilities heard
    uint8_t spec_rev = 0;               // their header's revision (2 = PD 3.x)
    uint32_t max_mw = 0;
    uint8_t offers = 0;
    const char *fc = nullptr;           // the passive fast-charge verdict's short name; nullptr when none
    bool cable = false;                 // an e-marker read
    uint8_t cable_a = 0, cable_v = 0;
    uint16_t cable_w = 0;
    bool contract = false;              // a PD contract in force
    int32_t ctr_mv = 0, ctr_ma = 0;
    StrBuf<17> diag;                    // the PD diagnosis word (home::format_diag); empty without a session
    uint8_t captures = 0;               // Black Box captures held
    const char *version = "";           // kFwVersion
    bool hid = false;                   // the HID function enumerates
    bool webusb = false;                // the WebUSB vendor function enumerates (K2_WEBUSB builds)
    bool batt = false;                  // the battery recorder has a run since boot (running or done)
    StrBuf<kStatusChars + 1> batt_status;   // its line (battery::view::bench_status); empty = "No run"
};
struct Row { bool known = false; StrBuf<kStatusChars + 1> status; };
Row row(Place p, const Facts &f);
// The Bench's fact line (at most 27 characters): what is attached.
void fact(const Facts &f, StrBuf<28> &out);

}  // namespace bench

}  // namespace k2
