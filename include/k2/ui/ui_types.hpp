// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <cstdint>

namespace k2 {

// The pages of the normal rotation (long [+] / [-] page through them).
enum class Page : uint8_t { Home, Detail, ChargerTest, Lines, Tools, Settings, Count };
// Submenus opened from Tools or Settings: modal, they have no place in the rotation. The remote
// reports a showing submenu as page index kMenuPageBase + Menu. Connection (Source / Cable / Sink) and Diag
// are Tools entries; Emarker is the saved-cables list opened from Connection's Cable tab; Twin is the Device twins list;
// LoopProbe is the HID-cable loop probe (Tools); PdStress the renegotiation stress tool (Tools); Diagnosis is the Black Box
// diagnosis of the PD session; Battery the efficiency recorder (Bench > Battery).
enum class Menu : uint8_t { Trigger, Emarker, PdTrigger, CableR, BlackBox, Spoof, UsbLink, Autorotate, Cards, Share, Connection, Diag, QrTest, Twin, LoopProbe, PdStress, Diagnosis, Battery, Count };
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
enum class BusyWhy : uint8_t { None, TestRunning, FcRelease, QcTrigger, PdHold, Epr, PdRequest, EmScan, Identify, TwinRun, PdStress, LoopProbe };   // new values are appended: the numbers are on the wire

// Ui::Session::flags (and the wire byte): one bit per transient chrome state the screens show.
inline constexpr uint8_t kSessionHoldCard = 1 << 0;       // OK + Back held: the "Reboot in N" card
inline constexpr uint8_t kSessionRotationNotice = 1 << 1; // the rotation notice strip
inline constexpr uint8_t kSessionEmScanning = 1 << 2;     // an e-marker scan is running
inline constexpr uint8_t kSessionEmListening = 1 << 3;    // the passive listen window after a scan is open
inline constexpr uint8_t kSessionEmQuiet = 1 << 4;        // the last listen window heard no cable reply
inline constexpr uint8_t kSessionIdNoDevice = 1 << 5;     // Identify was refused: nothing on CC
// Ui::Session::sub: the Connection page's tab (bits 1:0, conn::Tab), its pop-up (bit 2) and its details page (bit 3).
inline constexpr uint8_t kSubTabMask = 3, kSubPopup = 1 << 2, kSubDetails = 1 << 3;

// Per-pass LCD budget: how much one main-loop paint slice may take. The unit is a text pixel (rendered, then on the wire,
// ~2.3 us on the shipped /4 divider: 440 px/ms measured); a solid fill only costs wire time, a quarter of that, so a fill
// passes its pixel count with `fill` set and is weighed accordingly. Two bounds, whichever bites first:
//  - the pixel cap (kDefaultPx units), always on: the bound where no clock is set (the host UI tests, render_for_capture);
//  - arm()'d: the measured time of the pass (a microsecond clock, wrap-safe), so a slice stops asking for work once the pass
//    would end later than `limit_us`, counting whatever the pass already did before its first paint call.
// A pass always admits its first item however large (force_first), so a paint makes progress every pass.
// Nullptr on UiContext::budget (e.g. render_for_capture) renders atomically without slicing.
struct PaintBudget {
    static constexpr int kDefaultPx = 4500;   // ~10 ms of text at the measured rate; the cap where no clock is armed
    static constexpr int kStripRows = 16;     // <= 3840 px per strip clear in both 224 and 240 orientations
    static constexpr uint32_t kPassUs = 8000; // the time one pass may spend painting (armed budgets)
    static constexpr int kUnitUs10 = 23;      // 2.3 us per unit, in tenths: what the time guard assumes an item will cost
    int cap = kDefaultPx;
    int spent = 0;
    bool more = false;
    uint32_t (*clock_us)() = nullptr;
    uint32_t start_us = 0;
    uint32_t limit_us = 0;

    // Starts the time guard: the pass began now and must not paint past limit_us from here.
    void arm(uint32_t (*clock)(), uint32_t limit) {
        clock_us = clock;
        limit_us = limit;
        start_us = clock();
    }
    static int units(int px, bool fill) { return fill ? (px + 3) / 4 : px; }
    bool admit(int px, bool force_first = true, bool fill = false) {
        if (px <= 0) return true;
        const int cost = units(px, fill);
        if (force_first && spent == 0) {
            spent += cost;
            return true;
        }
        if (spent + cost <= cap && (!clock_us || elapsed_us() + static_cast<uint32_t>(cost) * kUnitUs10 / 10 <= limit_us)) {
            spent += cost;
            return true;
        }
        more = true;
        return false;
    }
    // Units still admissible: the tighter of the cap and the time left.
    int left() const {
        int n = cap > spent ? cap - spent : 0;
        if (clock_us) {
            const uint32_t e = elapsed_us();
            const int t = e >= limit_us ? 0 : static_cast<int>((limit_us - e) * 10 / kUnitUs10);
            if (t < n) n = t;
        }
        return n;
    }
    void defer() { more = true; }
    uint32_t elapsed_us() const { return clock_us() - start_us; }
};

}  // namespace k2
