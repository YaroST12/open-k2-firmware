// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The page ring: ten pages in one loop (Home, Lines, Numbers, Graph, Ripple, Link, Tools, Battery, Log, K2).
// Pure (host-tested in tests/test_ui_ring.cpp); the Ui owns the one flag "ring view" and acts on what these return.
//
// In the ring view the keys only move: + next, - previous, OK enters the page, Back goes Home, long Back jumps to the page
// chosen in Settings. An entered page owns the keys (the page's own scroll and actions); the Back it does not take leaves to
// the ring on the same page. Lines and Numbers have nothing to act on, so they are view-only: OK does nothing there.
#pragma once
#include <cstdint>
#include "k2/core/strpack.hpp"
#include "k2/ui/rail_title.hpp"
#include "k2/ui/ui_events.hpp"
#include "k2/ui/ui_labels.hpp"
#include "k2/ui/ui_types.hpp"

namespace k2::ring {

enum class Pos : uint8_t { Home, Lines, Numbers, Graph, Ripple, Link, Tools, Battery, Log, K2 };
inline constexpr int kCount = 10;

constexpr Pos next(Pos p) { return static_cast<Pos>((static_cast<int>(p) + 1) % kCount); }
constexpr Pos prev(Pos p) { return static_cast<Pos>((static_cast<int>(p) + kCount - 1) % kCount); }

// Only the pages with something to do can be entered.
constexpr bool can_enter(Pos p) { return p != Pos::Lines && p != Pos::Numbers; }

// The masthead text in the ring view: the page's own rail name (at most 4 characters), so an entered page keeps its place.
constexpr const rail::Title &mast_name(Pos p) {
    static constexpr rail::Title kNames[kCount] = {rail::kHome, rail::kLine, rail::kNum, rail::kGraph, rail::kRipple, rail::kLink, rail::kTools, rail::kBattery, rail::kLog, rail::kK2};
    return kNames[static_cast<int>(p) < kCount ? static_cast<int>(p) : 0];
}

// The plain name for the Settings row's tail (at most 7 characters: the row's tail room).
constexpr const char *name(Pos p) {
    constexpr const char *kNames[kCount] = {K2_P("Home"), K2_P("Lines"), K2_P("Numbers"), K2_P("Graphs"), K2_P("Ripple"), K2_P("Link"), K2_P("Tools"), K2_P("Battery"), "Log", "K2"};
    return kNames[static_cast<int>(p) < kCount ? static_cast<int>(p) : 0];
}

// The persisted long-Back target (uiprefs): anything out of range is Home.
constexpr Pos sanitize_target(unsigned v) { return v < static_cast<unsigned>(kCount) ? static_cast<Pos>(v) : Pos::Home; }

// Where the page sits in the ring: the base page, the first open menu (Menu::Count = none) and the Connection tab.
// Total: every combination lands in the ring, so any opener (remote, charger test, rescan) leaves a defined position.
constexpr Pos owner(Page base, Menu first, uint8_t /*tab*/) {
    Pos own = Pos::Home;
    if (base == Page::Detail) own = Pos::Numbers;
    else if (base == Page::Lines) own = Pos::Lines;
    else if (base == Page::Graphs) own = Pos::Graph;
    else if (base == Page::Ripple) own = Pos::Ripple;
    else if (base == Page::Settings) own = Pos::K2;
    else if (base == Page::ChargerTest) own = Pos::Link;
    switch (first) {
    case Menu::Trigger: case Menu::PdTrigger: case Menu::Tools: return Pos::Tools;
    case Menu::Cards: case Menu::PdStress:
    case Menu::Emarker: case Menu::CableR: case Menu::Spoof: case Menu::LoopProbe:
    case Menu::Twin: case Menu::Connection: return Pos::Link;
    case Menu::Battery: return Pos::Battery;
    case Menu::BlackBox: case Menu::Diagnosis: return Pos::Log;
    case Menu::Diag: case Menu::UsbLink: case Menu::Autorotate: case Menu::Memory: case Menu::Display: case Menu::HomeBoxes: return Pos::K2;
    case Menu::Share: case Menu::QrTest: case Menu::Count: break;
    }
    return own;
}

// What showing a ring position builds: a base page, or a menu over any base page (Connection Overview for Link).
struct Target { Page page; Menu menu; uint8_t tab; };
constexpr Target target(Pos p) {
    switch (p) {
    case Pos::Home: return {Page::Home, Menu::Count, 0};
    case Pos::Lines: return {Page::Lines, Menu::Count, 0};
    case Pos::Numbers: return {Page::Detail, Menu::Count, 0};
    case Pos::Graph: return {Page::Graphs, Menu::Count, 0};
    case Pos::Ripple: return {Page::Ripple, Menu::Count, 0};
    case Pos::Link: return {Page::Lines, Menu::Connection, 0};
    case Pos::Tools: return {Page::Lines, Menu::Tools, 0};
    case Pos::Battery: return {Page::Lines, Menu::Battery, 0};
    case Pos::Log: return {Page::Lines, Menu::Diagnosis, 0};
    case Pos::K2: return {Page::Settings, Menu::Count, 0};
    }
    return {Page::Home, Menu::Count, 0};
}

enum class Do : uint8_t { None, Show, Enter, Leave };
struct Step { Do what; Pos to; };

// A key in the ring view. Every other press (long OK, long +/-, holds) is consumed with no effect.
constexpr Step view_key(Pos cur, Event e, Pos long_back) {
    if (e.press == Press::Short) {
        if (e.button == Button::Plus) return {Do::Show, next(cur)};
        if (e.button == Button::Minus) return {Do::Show, prev(cur)};
        if (e.button == Button::Ok) return {can_enter(cur) ? Do::Enter : Do::None, cur};
        if (e.button == Button::Back) return {cur == Pos::Home ? Do::None : Do::Show, Pos::Home};
    } else if (e.press == Press::Long && e.button == Button::Back) {
        return {long_back == cur ? Do::None : Do::Show, long_back};
    }
    return {Do::None, cur};
}

// The strip in the ring view: Prev / Next move, Enter dims where the page has nothing to do, Back goes Home and its long press
// (named Jump) goes to the chosen page; nothing is named when the long press would land where the short one does or on the
// page already in view.
constexpr Strip view_strip(Pos cur, Pos long_back) {
    const bool jump = long_back != cur && long_back != Pos::Home;
    sl::Id back = jump ? sl::HomeJump : sl::Home;
    if (cur == Pos::Home) back = jump ? sl::Jump : sl::None;
    return {{back, sl::Prev, can_enter(cur) ? sl::Enter : sl::EnterDim, sl::Next}};
}

// A Back the entered page did not take: a short one at the page's top level, or a long one anywhere, leaves to the ring.
// A short one one level down is an ordinary pop of that level.
constexpr Do entered_back(Press p, bool top_level) {
    if (p == Press::Long) return Do::Leave;
    return top_level ? Do::Leave : Do::None;
}

}  // namespace k2::ring
