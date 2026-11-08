// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Input events: the debounced button state (physical or remote-injected) becomes
// {button, Short | Long}; the global handler sees an event first, then the active page.
// Pure: no hardware. There is no Repeat press: the detector reports only Short and Long.
#pragma once
#include <cstdint>
#include <optional>
#include "k2/drivers/buttons.hpp"
#include "k2/ui/ui_layout.hpp"

namespace k2 {

enum class Press : uint8_t { Short, Long, Hold };
struct Event { Button button; Press press; };

constexpr std::optional<Event> to_event(Button b, ButtonEvent ev) {
    switch (ev) {
    case ButtonEvent::Short: return Event{b, Press::Short};
    case ButtonEvent::Long:  return Event{b, Press::Long};
    case ButtonEvent::Hold:  return Event{b, Press::Hold};
    case ButtonEvent::None:  break;
    }
    return std::nullopt;
}

// What the global handler does with an event.
enum class Global : uint8_t { None, PagePrev, PageNext, Reboot };

// Long [+] = next page, long [-] = previous page, [BACK] held for 5 s = reboot (a 500 ms long
// BACK is no gesture and falls through to the page). At the first / last page the paging press
// does nothing (no wrap), so it falls through to the page.
constexpr Global global_action(Event e, unsigned page, unsigned page_count) {
    if (e.press == Press::Hold) return e.button == Button::Back ? Global::Reboot : Global::None;
    if (e.press != Press::Long) return Global::None;
    switch (e.button) {
    case Button::Plus:  return page + 1 < page_count ? Global::PageNext : Global::None;
    case Button::Minus: return page > 0 ? Global::PagePrev : Global::None;
    default:            return Global::None;
    }
}

enum class Route : uint8_t { Unhandled, Global, Page, Pop };
struct Dispatch { Route route; Global global; };

// Global handler first; otherwise the page (anything with `bool on_event(Event)`) gets the event.
// An event the page declines stays unhandled.
template <class P>
Dispatch dispatch(Event e, unsigned page, unsigned page_count, P &p) {
    const Global g = global_action(e, page, page_count);
    if (g != Global::None) return {Route::Global, g};
    if (p.on_event(e)) return {Route::Page, Global::None};
    return {Route::Unhandled, Global::None};
}

// Routing while a submenu is open: the global paging is off, so long [+] / [-] belong to the menu.
// Only the BACK hold (reboot) stays global. The menu sees every other event first; a short BACK
// it does not consume pops the menu (Route::Pop), so BACK can never be trapped by a menu.
template <class P>
Dispatch dispatch_modal(Event e, P &p) {
    if (e.press == Press::Hold && e.button == Button::Back) return {Route::Global, Global::Reboot};
    if (p.on_event(e)) return {Route::Page, Global::None};
    if (e.press == Press::Short && e.button == Button::Back) return {Route::Pop, Global::None};
    return {Route::Unhandled, Global::None};
}

}  // namespace k2
