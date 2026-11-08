// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <cstdint>

namespace k2 {

// The pages of the normal rotation (long [+] / [-] page through them).
enum class Page : uint8_t { Home, Detail, ChargerTest, Lines, Tools, Settings, Count };
// Submenus opened from Tools or Settings: modal, they have no place in the rotation. The remote
// reports a showing submenu as page index kMenuPageBase + Menu. Connection (Source / Cable / Sink) and Diag
// are Tools entries; Emarker is the saved-cables list opened from Connection's Cable tab.
enum class Menu : uint8_t { Trigger, Emarker, PdTrigger, CableR, BlackBox, Spoof, UsbLink, Autorotate, Cards, Share, Connection, Diag, Count };
inline constexpr uint8_t kMenuPageBase = static_cast<uint8_t>(Page::Count);
// Remote-only page indices that open Connection on a tab (kConnTabAlias + Source / Cable / Sink): the old "pd",
// "emarker" and "identify" page names land here. Ui::page_index() never reports them (Connection reports itself).
inline constexpr uint8_t kConnTabAlias = kMenuPageBase + static_cast<uint8_t>(Menu::Count);
enum class UiAction : uint8_t { None, Reboot };
// Page actions; the values are the ACTION ids of the remote protocol.
enum class Action : uint8_t { PdRescan = 1, EmarkerScan = 2, EnergyReset = 3,
                           FcScan = 4, FcReset = 5,   // FcScan / FcReset: the remote's fc scan / fc reset (the scan needs the confirm)
                           Identify = 6 };

}  // namespace k2
