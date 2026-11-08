// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure model behind the PD trigger's confirm rule and the Identify page rows (host-tested in
// tests/test_ui_pdtrig.cpp). No drawing, no hardware.
//
// Trigger: the step wizard (ui_wizard.hpp) walks the PDOs. Anything above 5 V needs a confirm,
// decided by trigger_needs_confirm().
#pragma once
#include <cstdint>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2 {

// True when the request would put more than 5 V on VBUS. A request that cannot be planned (no
// caps, over the cap) needs none: it is refused anyway.
bool trigger_needs_confirm(const pd::SourceCaps &caps, pd::Choice c, uint32_t cap_mv);

// Identify page rows, one text row each. `tone` picks the colour on screen.
enum class RowTone : uint8_t { Head, Value, Text, Dim };
struct IdRow { StrBuf<24> text; RowTone tone = RowTone::Text; };
inline constexpr int kMaxIdRows = 24;
// Fills `rows` (kMaxIdRows) from the partner's identity, SVIDs, sink capabilities and skipped steps.
int identify_rows(const pd::IdStatus &st, const pd::VdmInfo &vdm, IdRow *rows);
// Result card of a finished identify (state Done with a valid identity): the device name (Apple
// model, else vendor, else "Device"), vendor and PD revision pills and what the sink asks for
// ("wants 20V 1.5A", the highest-power sink PDO). false while there is nothing to show.
struct IdCard { StrBuf<16> name; StrBuf<12> vendor; StrBuf<8> pd; StrBuf<16> wants; };
bool identify_card(const pd::IdStatus &st, const pd::VdmInfo &vdm, IdCard &out);
// Key / value rows under the card: VID, PID, product type and USB role when known, up to 4 SVIDs. Keys <= 9, values <= 11 chars.
int identify_kv_count(const pd::IdStatus &st, const pd::VdmInfo &vdm);
void identify_kv(const pd::IdStatus &st, const pd::VdmInfo &vdm, int i, StrBuf<12> &key, StrBuf<16> &val);
// Status line of the page: "OK to identify", "Discover SVIDs...", "Done", "Aborted".
// Result footer, no age: "Identified", or "No reply: SVIDs, Modes, Status" naming the unanswered steps.
void identify_result_text(const pd::IdStatus &st, StrBuf<64> &out);
void identify_status_text(const pd::IdStatus &st, StrBuf<24> &out);

// Setup hint: which port gets what, before a tool runs and when the live state makes it refuse.
// (The ports are unmarked and symmetric; the screen never says INPUT/OUTPUT.) INPUT is the charger side (the FUSB302 CC and the Rd sink face it); OUTPUT is the load side,
// passed through. Pure (host-tested): the pages fill SetupState from the link and the power monitor.
enum class Tool : uint8_t { Identify, Emarker, PdTrigger };
struct SetupState {
    bool attached = false;      // a source on INPUT (pd::Link)
    bool inline_mode = false;   // a live sink on OUTPUT (pd::Link::inline_mode)
    bool caps_valid = false;    // Source_Capabilities received
    bool vbus_known = false;
    int32_t vbus_mv = 0;
};
struct SetupHint {
    const char *line[4] = {"", "", "", ""};   // each line at most 22 characters
    int lines = 0;
    bool ready = false;                       // the setup allows the tool to do its full job
};
SetupHint setup_hint(Tool tool, const SetupState &st);

// Tool template content (flow diagram, checklist, footer) for the setup and refused states. Roles
// name the thing (K2, CBL, DEV, CHG), never a port position. `ready` equals setup_hint().ready.
struct SetupView {
    const char *top_role = "K2", *bottom_role = "";
    const char *check[3] = {"", "", ""};   // each at most 12 characters
    bool met[3] = {false, false, false};
    int n = 0;
    bool ready = false;
    const char *caption = "";              // S12 beside the diagram column
    const char *footer = "";               // at most 26 characters: the state or the reason
};
SetupView setup_view(Tool tool, const SetupState &st);

// ---- PD trigger list rows ---------------------------------------------------------------------------
// One PDO as a kit RowList row: the text "FIX 9V 3A" / "PPS 3.3-11V 5A" / "AVS 25.5-51.1V 5A" and the power as the tail
// ("27W"). Text and tail together never exceed kPdoLineCells (what the row has between the marker and the right edge):
// the amps lose precision first (4.99A -> 5A), then drop, so a row is never cut. Pure text; colours are the page's.
inline constexpr int kPdoLineCells = 22;
struct PdoLine {
    StrBuf<24> text;
    StrBuf<8> tail;
};
void pdo_line(const pd::Pdo &p, PdoLine &out);

}  // namespace k2
