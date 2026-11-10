// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// D+/D- switch network driver. Four GPIOs steer the USB-A port's data lines between the
// charger-protocol front end and the ADC sense inputs. The passive idle state
// is PA7=1, PB0=0, PB1=1, PB2=0: it DRIVES both lines to a 0.6 V level through the switch network
// (hardware: 653/653 mV at boot, 592/591 mV behind a phone), which the ADC senses. Only the Hi-Z
// drive releases a line to a plain analog input.
// The driver owns these pins; other code reads the state through read(), never the GPIO.
#pragma once
#include <cstdint>
#include <span>

namespace k2::dpdm {

// Level a line is driven to. The 0.0 / 0.6 / 2.0 / 3.3 V levels come from the two
// switch pins of the line; HiZ releases both pins to analog; RxPulldown (D- only) is the ACCP/AFC
// receive state; Listen (D+ only) makes PA7 a floating digital input to follow an adapter-driven clock
// (VOOC) with PB0 released. There is no 2.7 V primitive.
enum class Drive : std::uint8_t { HiZ, V0, V0p6, V2p0, V3p3, RxPulldown, Listen, Sniff };

// How one line's two switch pins are set: Analog (both analog inputs), Out (push-pull, a/b levels
// of the first/second pin: PA7/PB0 for D+, PB1/PB2 for D-) or RxPull (first pin low output, second
// pin input with pull-down; D- only) or In (first pin floating input, second analog; D+ only).
enum class Kind : std::uint8_t { Analog, Out, RxPull, In, InBoth };
// Sniff is the passive listen of k2/fastcharge/dpsniff.hpp: D+ = In (PA7 floating input), D- = InBoth (PB1 and
// PB2 floating inputs). Nothing is driven and no pull-down is applied.
struct PinPair {
    Kind kind;
    bool a, b;
};

constexpr PinPair dp_pins(Drive d) {   // D+: PA7, PB0 (RxPulldown does not exist on D+: released)
    switch (d) {
    case Drive::V0:   return {Kind::Out, false, false};
    case Drive::V0p6: return {Kind::Out, true, false};
    case Drive::V2p0: return {Kind::Out, false, true};
    case Drive::V3p3: return {Kind::Out, true, true};
    case Drive::Listen: return {Kind::In, false, false};
    case Drive::Sniff:  return {Kind::In, false, false};
    default:          return {Kind::Analog, false, false};
    }
}

constexpr PinPair dm_pins(Drive d) {   // D-: PB1, PB2
    switch (d) {
    case Drive::V0:          return {Kind::Out, false, false};
    case Drive::V0p6:        return {Kind::Out, true, false};
    case Drive::V2p0:        return {Kind::Out, false, true};
    case Drive::V3p3:        return {Kind::Out, true, true};
    case Drive::RxPulldown:  return {Kind::RxPull, false, false};
    case Drive::Sniff:       return {Kind::InBoth, false, false};
    default:                 return {Kind::Analog, false, false};
    }
}

// One timed step of a pulse / bit sequence: both lines at these levels for `us` microseconds.
struct Step {
    Drive dp, dm;
    std::uint32_t us;
};

struct Levels {
    bool pa7, pb0, sw1, sw2;  // PA7, PB0, PB1, PB2
};

inline constexpr Levels kPassiveIdle{true, false, true, false};

// One bit per pin that differs from the passive idle state (bit0 PA7, bit1 PB0, bit2 PB1, bit3 PB2).
constexpr unsigned mismatch(const Levels& l) {
    return (l.pa7 != kPassiveIdle.pa7 ? 1u : 0u) | (l.pb0 != kPassiveIdle.pb0 ? 2u : 0u) |
           (l.sw1 != kPassiveIdle.sw1 ? 4u : 0u) | (l.sw2 != kPassiveIdle.sw2 ? 8u : 0u);
}

// Enable GPIOA/GPIOB clocks, make the four pins push-pull outputs and drive kPassiveIdle.
void init();

// Drive one line. Pins are written before their mode changes, so a line never glitches through
// the old level. Never exceeds what the pins can express: the caller decides when to use them.
void set_dp(Drive d);
void set_dm(Drive d);

// Play a step list with 1 us resolution from the core cycle counter. Interrupts are masked while a
// list of up to ~20 ms runs (pulse widths of 40 us need it) and the lost SysTick ms are re-counted.
void play(std::span<const Step> steps);

// Busy-wait `us` microseconds from the core cycle counter (systick::delay_us when it is missing).
void delay_us(std::uint32_t us);
// Free-running 240 MHz core cycle counter (DWT), the time base of the edge capture.
std::uint32_t cycles();

// D- read as a digital bit (PB2), meaningful in the RxPulldown state.
bool read_dm_bit();

// Busy-wait until D- reads `level`; returns microseconds waited, or -1 on timeout. Polls every ~2 us.
int wait_dm(bool level, std::uint32_t timeout_us);

// Sample D- (PB2) every `period_us` into `out` (1 = high), interrupts masked; sample count must keep
// the whole capture under ~20 ms.
void capture_dm(std::span<std::uint8_t> out, std::uint32_t period_us);

// Same wait for D+ (PA7 level as seen on the input data register).
int wait_dp(bool level, std::uint32_t timeout_us);

// Adapter-clocked frames (VOOC). The adapter pulses D+ low 9 times per frame; D- is the data
// line, latched just before the next fall. Needs D+ in Listen and D- in RxPulldown. Both calls mask
// interrupts for the frame (up to ~20 ms) and give the SysTick ticks back afterwards.
//   rx_command: one adapter command, 9 bits (8 command bits + a trailing 0), or -1 when no frame
//               started within first_timeout_us.
//   tx_reply:   the 9 bit phone reply, driven in the clock of the next frame (D- 50 us after each D+ rise);
//               false when that clock did not come. D- returns to RxPulldown either way.
int rx_command(std::uint32_t first_timeout_us);
bool tx_reply(std::uint16_t word);

// Which pin carries the adapter clock: PA7 (our sniffer and Listen) or PB0 (the EXTI0 input).
// PB0 mode makes both D+ pins floating inputs in Listen.
enum class ClockPin : std::uint8_t { Pa7, Pb0 };
void set_clock_pin(ClockPin c);
ClockPin clock_pin();
// Falling D+ edges seen on each pin during the next `window_us` (interrupts masked), for pin diagnosis.
struct EdgeCount { std::uint32_t pa7, pb0; };
EdgeCount count_dp_edges(std::uint32_t window_us);

// Both lines to 0 V right now; any HVDCP / AFC / FCP / SCP charger falls back to 5 V.
void discharge();

// Pin sequence of the active DCP check (fastcharge_classify.hpp, dcp_check_result): D+ is driven to 0 V
// with D- left on its idle 0.6 V. Both calls return at once; the caller waits between them and reads D-
// with the ADC. dcp_probe_end() restores the passive idle only when the pins still hold the probe state
// (returns false when something else rewrote them meanwhile, and then leaves them alone).
void dcp_probe_begin();
bool dcp_probe_end();

// Full reset: discharge both lines at once, hold 0 V for 500 ms, then restore the passive
// idle state. Non-blocking: the hold ends in poll(). Safe to call repeatedly (restarts the hold).
void start_reset();
bool reset_busy();
void cancel_reset();
void poll();

// Current pin levels (input data register, valid for outputs too).
Levels read();

}  // namespace k2::dpdm
