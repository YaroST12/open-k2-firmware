// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// D+/D- switch network driver. Four GPIOs (PA7/PB0 for D+, PB1/PB2 for D-) steer the USB-A port's data lines
// through a resistor network to 0 / 0.6 / 2.0 / 3.3 V; the ADC senses both lines. The passive idle (kIdle)
// releases both lines (all four pins analog inputs): the K2 drives nothing until a session, probe or shell
// command takes the lines, and every one of them gives the lines back through idle() or start_reset().
// The driver owns these pins; other code reads the state through drive() and read(), never the GPIO.
#pragma once
#include <array>
#include <cstddef>
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

// What both lines are set to at once.
struct Pair {
    Drive dp, dm;
    constexpr bool operator==(const Pair&) const = default;
};

// The passive idle: both lines released, nothing driven.
// Until 2026-11-11 the idle was the factory start-up pin pattern PA7=1 PB0=0 PB1=1 PB2=0, which drives both lines to
// 0.6 V. That is the HVDCP entry level on D+ and the QC2 12 V code on the pair: a Quick Charge adapter entered HVDCP
// after ~1.25 s of it, and any D- dip and return then raised VBUS to 12 V with nothing requested (hardware:
// CUKTECH 10 USB-A, 5.1 -> 11.75 V). The released pair is neither (see hvdcp_entry_level, qc2_code).
inline constexpr Pair kIdle{Drive::HiZ, Drive::HiZ};

// D+ driven to a level inside the HVDCP entry window (0.325 .. 2 V, docs/agents/QC_RESEARCH.md section 1): a
// Quick Charge adapter that sees D+ there for about 1 s leaves BC1.2 mode. 2.0 V sits on the window's top edge and
// counts as inside. An undriven line (HiZ, Listen, Sniff) is not a level the K2 sets.
constexpr bool hvdcp_entry_level(Drive dp) { return dp == Drive::V0p6 || dp == Drive::V2p0; }

// The pair is one of the QC2 voltage codes (5 V 0.6/0, 9 V 3.3/0.6, 12 V 0.6/0.6, 20 V 3.3/3.3).
constexpr bool qc2_code(Pair p) {
    return (p.dp == Drive::V0p6 && (p.dm == Drive::V0 || p.dm == Drive::V0p6)) ||
           (p.dp == Drive::V3p3 && (p.dm == Drive::V0p6 || p.dm == Drive::V3p3));
}

// Steps of the active DCP check, one per fast-charge poll (~20 ms each):
//   0 Clear: D+ 0 V, D- released. D+ below 0.325 V drops any charger left in HVDCP back to 5 V.
//   1 Ref:   D+ released, D- 0.6 V. The ADC reads D- as the reference level.
//   2 Pull:  D+ 0 V, D- 0.6 V. A D+/D- short pulls D- down with D+; open lines leave D- where it was.
// Then both lines return to kIdle. Through a closed short D+ follows D- to ~0.6 V for the one Ref poll, far
// below the ~1 s an HVDCP adapter needs, and no step is a QC2 code.
inline constexpr std::array<Pair, 3> kDcpProbe{{
    {Drive::V0, Drive::HiZ},
    {Drive::HiZ, Drive::V0p6},
    {Drive::V0, Drive::V0p6},
}};

// One timed step of a pulse / bit sequence: both lines at these levels for `us` microseconds.
struct Step {
    Drive dp, dm;
    std::uint32_t us;
};

struct Levels {
    bool pa7, pb0, sw1, sw2;  // PA7, PB0, PB1, PB2
};

// One bit per line that is not at kIdle (bit0 D+, bit1 D-); 0 = the lines are at the passive idle.
constexpr unsigned mismatch(Pair p) { return (p.dp != kIdle.dp ? 1u : 0u) | (p.dm != kIdle.dm ? 2u : 0u); }

// Short name of a drive for log lines ("hiz", "0", "0.6", "2.0", "3.3", "rx", "listen", "sniff").
const char* name(Drive d);

// Enable GPIOA/GPIOB clocks and put both lines at kIdle (the four pins analog inputs).
void init();

// Drive one line. Pins are written before their mode changes, so a line never glitches through
// the old level. Never exceeds what the pins can express: the caller decides when to use them.
void set_dp(Drive d);
void set_dm(Drive d);

// What set_dp / set_dm last applied to each line (the GPIO input register cannot tell a released
// line from a 0 V one, so the driver keeps it). Listen on D+ reads back as Sniff in the PB0 clock mode.
Pair drive();

// Both lines to kIdle now.
void idle();

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

// Pin sequence of the active DCP check (fastcharge_classify.hpp, dcp_check_result), steps from kDcpProbe.
// dcp_probe_step(i) applies step i and returns at once; the caller waits a poll between steps and reads D-
// with the ADC. Moving on from step i (to step i+1, or back to idle with dcp_probe_end(i)) only happens while
// the lines still hold step i: both calls return false when another owner rewrote them meanwhile, and then
// leave them alone.
bool dcp_probe_step(std::size_t i);
bool dcp_probe_end(std::size_t i);

// Full reset: discharge both lines at once, hold 0 V for 500 ms, then return both lines to the
// passive idle. Non-blocking: the hold ends in poll(). Safe to call repeatedly (restarts the hold).
void start_reset();
bool reset_busy();
void cancel_reset();
void poll();

// Current pin levels (input data register, valid for outputs and digital inputs; a released, analog pin reads 0).
Levels read();

}  // namespace k2::dpdm
