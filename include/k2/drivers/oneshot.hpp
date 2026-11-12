// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One-shot microsecond timer on TMR7 (a basic timer on APB1: 120 MHz bus, 240 MHz timer clock, divided to 1 MHz). arm_us()
// calls a handler once from the TMR7 interrupt (IRQ 55) after the given time; arming again replaces the pending shot. The
// interrupt runs at the default priority, the same as the D+/D- edge EXTI interrupts, so neither preempts the other: a
// handler and an edge ISR that share state need no masking between them, only against the main loop.
// The first user is the guarded SuperVOOC trigger (k2/fastcharge/svooc_trig.hpp): its wire's deadlines (the D- release
// after a reply's hold, a quiet frame, a slot that never came) run from here, not from the main loop.
#pragma once
#include <cstdint>

namespace k2::oneshot {

using Handler = void (*)();

inline constexpr std::uint32_t kMinUs = 2;        // shorter requests fire after this
inline constexpr std::uint32_t kMaxUs = 65'535;   // the 16-bit period; longer requests fire early (the handler re-arms)

// Call `h` from the interrupt `us` microseconds from now (clamped to kMinUs..kMaxUs). Safe from the main loop and from
// an interrupt of the same priority. The first call sets the timer up.
void arm_us(std::uint32_t us, Handler h);
void cancel();                    // no shot pending afterwards
std::uint32_t fired();            // shots taken since boot (diagnostics)

}  // namespace k2::oneshot
