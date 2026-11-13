// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The page set the switch walks visit (test_ui_overdraw prices every switch, test_ui_capture proves each one pixel-exact):
// the six rotation pages, then the Tools / Settings submenus that open without a record or parameters. `index` is what
// Ui::goto_page takes. `opaque` mirrors Ui::page_opaque / menu_opaque: a switch into it paints once, with no erase pass.
#pragma once
#include <cstdint>

#include "k2/ui/ui.hpp"
#include "ui_nav.h"

namespace k2 {

struct WalkDest {
    const char *name;
    uint8_t index;
    bool opaque;
};

constexpr WalkDest kWalkDests[] = {
    {"Home", 0, true}, {"Detail", 1, true}, {"Charger", 2, true}, {"Lines", 3, true}, {"Tools", 4, true}, {"Settings", 5, true},
    {"Connection", kPdPage, false}, {"Diag", kDiagPage, true},
    {"Trigger", kMenuPageBase + static_cast<uint8_t>(Menu::Trigger), false},
    {"PdTrigger", kMenuPageBase + static_cast<uint8_t>(Menu::PdTrigger), false},
    {"CableR", kMenuPageBase + static_cast<uint8_t>(Menu::CableR), false},
    {"BlackBox", kMenuPageBase + static_cast<uint8_t>(Menu::BlackBox), false},
    {"Spoof", kMenuPageBase + static_cast<uint8_t>(Menu::Spoof), false},
    {"UsbLink", kMenuPageBase + static_cast<uint8_t>(Menu::UsbLink), true},
    {"Cards", kMenuPageBase + static_cast<uint8_t>(Menu::Cards), false},
};
constexpr unsigned kWalkN = sizeof kWalkDests / sizeof kWalkDests[0];

// A destination reached from a menu goes through its base page first (Ui::goto_page pops, then pushes): two switches, so the
// one-paint bound applies to page destinations from anywhere and to menu destinations from a rotation page only.
constexpr bool walk_one_switch(unsigned from, unsigned to) { return kWalkDests[to].index < 6 || kWalkDests[from].index < 6; }

}  // namespace k2
