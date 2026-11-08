// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Navigation helpers for the UI tests after the Connection redesign: the old PD page is now Tools > Connection
// and the old Diag page is a Tools submenu, so neither has a carousel page number any more. goto_page() still
// takes their page indices (they are what the remote and the web page send), so the tests go through these.
#pragma once
#include "k2/ui/ui.hpp"

namespace k2 {

constexpr uint8_t kPdPage = kConnTabAlias;                                   // "pd": Connection, opened on the Source tab
constexpr uint8_t kDiagPage = kMenuPageBase + static_cast<uint8_t>(Menu::Diag);

inline bool on_pd(const Ui &ui) { return ui.menu() == Menu::Connection; }
inline bool on_diag(const Ui &ui) { return ui.menu() == Menu::Diag; }

}  // namespace k2
