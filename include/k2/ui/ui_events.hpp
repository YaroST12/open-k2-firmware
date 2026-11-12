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

// What the global fallback does with an event the page left alone. Fieldbook has no paging press (the Bench replaced the
// carousel): the one global move is a long BACK, straight to the Meter from anywhere. The reboot is no event: it is the
// OK + Back chord the Ui watches on the key states (ui_calc.hpp RebootChord).
enum class Global : uint8_t { None, ToMeter };

enum class Route : uint8_t { Unhandled, Global, Page, Pop };
struct Dispatch { Route route; Global global; };

// The 5 s BACK hold (the old reboot gesture) reaches no page or menu: their long BACK action has fired at 500 ms of the same
// press and must not fire again. The Ui acts on it itself in the trigger (disarm) and the charger test (stop) only.
constexpr bool back_hold(Event e) { return e.button == Button::Back && e.press == Press::Hold; }
constexpr bool long_back(Event e) { return e.button == Button::Back && e.press == Press::Long; }

// The page (anything with `bool on_event(Event)`) first; a long BACK it declines goes to the Meter, anything else it declines
// stays unhandled (the Ui steps a declined short BACK up one level itself).
template <class P>
Dispatch dispatch(Event e, P &p) {
    if (back_hold(e)) return {Route::Unhandled, Global::None};
    if (p.on_event(e)) return {Route::Page, Global::None};
    if (long_back(e)) return {Route::Global, Global::ToMeter};
    return {Route::Unhandled, Global::None};
}

// Routing while a submenu is open: the menu sees every event but the BACK hold first; a short BACK it does not consume
// pops the menu (Route::Pop), so BACK can never be trapped by a menu, and a long BACK it does not consume goes to the Meter.
template <class P>
Dispatch dispatch_modal(Event e, P &p) {
    if (back_hold(e)) return {Route::Unhandled, Global::None};
    if (p.on_event(e)) return {Route::Page, Global::None};
    if (e.press == Press::Short && e.button == Button::Back) return {Route::Pop, Global::None};
    if (long_back(e)) return {Route::Global, Global::ToMeter};
    return {Route::Unhandled, Global::None};
}

}  // namespace k2
