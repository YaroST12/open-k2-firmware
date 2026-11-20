// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The places of the page ring (Charger, Cable, Device, Battery, Log, K2): their icons and the tool slots their action rows
// open. The tables and the row texts are pure (bench:: in ui_tools.cpp).
#include "k2/ui/text/tools.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"

namespace k2 {

namespace {
using bench::Place;
constexpr Icon kPlaceIcons[bench::kPlaces] = {Icon::Meter, Icon::Plug, Icon::Cable, Icon::Phone, Icon::Battery, Icon::Report,
                                              Icon::Chip};
}  // namespace

const char *tool_name(int i) {
    static_assert(text::tools::kSlots == kToolSlots);
    return text::tools::kNames[tool_built(i) ? i : 0];   // the hub's copy: include/k2/ui/text/tools.hpp
}

Menu tool_menu(int i) {
    constexpr Menu kMenus[kToolSlots] = {Menu::Trigger, Menu::PdTrigger, Menu::Spoof, Menu::CableR,
                                         Menu::Connection, Menu::Connection, Menu::Connection,
                                         Menu::BlackBox, Menu::Diagnosis, Menu::Cards, Menu::Diag,
                                         Menu::Twin, Menu::LoopProbe, Menu::PdStress};
    return kMenus[tool_built(i) ? i : 0];
}

Icon place_icon(Place p) { return kPlaceIcons[static_cast<int>(p) < bench::kPlaces ? static_cast<int>(p) : 0]; }

// A tool opens its menu over the page OK was pressed on (BACK comes back to it); a page replaces everything (Test charger,
// Settings).
void open_bench_act(Ui &ui, const bench::Act &a, uint32_t now_ms) {
    switch (a.kind) {
    case bench::ActKind::Tool: ui.push_menu(tool_menu(a.arg), now_ms); break;
    case bench::ActKind::Page: ui.goto_page(a.arg, now_ms); break;
    case bench::ActKind::Menu: ui.push_menu(static_cast<Menu>(a.arg), now_ms); break;
    }
}

}  // namespace k2
