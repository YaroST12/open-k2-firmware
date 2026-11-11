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
enum class Menu : uint8_t { Trigger, Emarker, PdTrigger, CableR, BlackBox, Spoof, UsbLink, Autorotate, Cards, Share, Connection, Diag, QrTest, Count };
inline constexpr uint8_t kMenuPageBase = static_cast<uint8_t>(Page::Count);
// Remote-only page indices that open Connection on a tab (kConnTabAlias + Source / Cable / Sink): the old "pd",
// "emarker" and "identify" page names land here. Ui::page_index() never reports them (Connection reports itself).
inline constexpr uint8_t kConnTabAlias = kMenuPageBase + static_cast<uint8_t>(Menu::Count);
enum class UiAction : uint8_t { None, Reboot };
// Page actions; the values are the ACTION ids of the remote protocol.
enum class Action : uint8_t { PdRescan = 1, EmarkerScan = 2, EnergyReset = 3,
                           FcScan = 4, FcReset = 5,   // FcScan / FcReset: the remote's fc scan / fc reset (the scan needs the confirm)
                           Identify = 6 };

// What keeps an identity or a START out (Ui::busy_why); busy_reason() is its footer text. The values ride the remote's
// session block (GET_STATE part 1) as they are.
enum class BusyWhy : uint8_t { None, TestRunning, FcRelease, QcTrigger, PdHold, Epr, PdRequest, EmScan, Identify };

// Ui::Session::flags (and the wire byte): one bit per transient chrome state the screens show.
inline constexpr uint8_t kSessionHoldCard = 1 << 0;       // BACK held: the "Reboot in N" card
inline constexpr uint8_t kSessionRotationNotice = 1 << 1; // the rotation notice strip
inline constexpr uint8_t kSessionEmScanning = 1 << 2;     // an e-marker scan is running
inline constexpr uint8_t kSessionEmListening = 1 << 3;    // the passive listen window after a scan is open
inline constexpr uint8_t kSessionEmQuiet = 1 << 4;        // the last listen window heard no cable reply
inline constexpr uint8_t kSessionIdNoDevice = 1 << 5;     // Identify was refused: nothing on CC
// Ui::Session::sub: the Connection page's tab (bits 1:0, conn::Tab), its pop-up (bit 2) and its details page (bit 3).
inline constexpr uint8_t kSubTabMask = 3, kSubPopup = 1 << 2, kSubDetails = 1 << 3;

}  // namespace k2
