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
enum class Global : uint8_t { None, PagePrev, PageNext };

// Long [+] = next page, long [-] = previous page. At the first / last page the paging press does
// nothing (no wrap), so it falls through to the page. The reboot is no event: it is the OK + Back
// chord the Ui watches on the key states (ui_calc.hpp RebootChord).
constexpr Global global_action(Event e, unsigned page, unsigned page_count) {
    if (e.press != Press::Long) return Global::None;
    switch (e.button) {
    case Button::Plus:  return page + 1 < page_count ? Global::PageNext : Global::None;
    case Button::Minus: return page > 0 ? Global::PagePrev : Global::None;
    default:            return Global::None;
    }
}

enum class Route : uint8_t { Unhandled, Global, Page, Pop };
struct Dispatch { Route route; Global global; };

// The 5 s BACK hold (the old reboot gesture) reaches no page or menu: their long BACK action has fired at 500 ms of the same
// press and must not fire again. The Ui acts on it itself in the trigger (disarm) and the charger test (stop) only.
constexpr bool back_hold(Event e) { return e.button == Button::Back && e.press == Press::Hold; }

// Global handler first; otherwise the page (anything with `bool on_event(Event)`) gets the event.
// An event the page declines stays unhandled.
// `paging_locked`: the page keeps long [+] / [-] for itself (the charger test's Waiting stage: the operator holds + to wake a
// charger, and a long + must not page away and cancel the wait). Only the paging presses are held back.
template <class P>
Dispatch dispatch(Event e, unsigned page, unsigned page_count, P &p, bool paging_locked = false) {
    if (back_hold(e)) return {Route::Unhandled, Global::None};
    const Global g = global_action(e, page, page_count);
    const bool paging = g == Global::PagePrev || g == Global::PageNext;
    if (g != Global::None && !(paging && paging_locked)) return {Route::Global, g};
    if (p.on_event(e)) return {Route::Page, Global::None};
    return {Route::Unhandled, Global::None};
}

// Routing while a submenu is open: the global paging is off, so long [+] / [-] belong to the menu.
// The menu sees every event but the BACK hold first; a short BACK it does not consume pops the menu
// (Route::Pop), so BACK can never be trapped by a menu.
template <class P>
Dispatch dispatch_modal(Event e, P &p) {
    if (back_hold(e)) return {Route::Unhandled, Global::None};
    if (p.on_event(e)) return {Route::Page, Global::None};
    if (e.press == Press::Short && e.button == Button::Back) return {Route::Pop, Global::None};
    return {Route::Unhandled, Global::None};
}

}  // namespace k2
