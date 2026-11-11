// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD world for the host world-replay test (tests/test_world_replay.cpp): a recorded bench session (a scenario written by
// tools/trace2scenario.py) played against the real FUSB302 driver, the real power-monitor driver and the real main loop
// (src/core/main_loop.cpp) on a virtual clock. The fakes are the chips and nets, never the firmware:
//
//   - FUSB302: a register file; STATUS0 COMP / BC_LVL from the world's CC level on the line SWITCHES0 measures, against
//     MEASURE's MDAC (x 42 mV); an RX FIFO that takes a frame only if, at the frame's time, the receiver is routed to its
//     line (SWITCHES1 TXCC of that line, POWER receiver bits, CONTROL1 ENSOP1/ENSOP2 for SOP'/SOP''); PD_RESET, SW_RES
//     and RX_FLUSH empty the FIFO. Every frame's fate (delivered, or lost and why) is recorded.
//   - INA226 at 0x40 (die id 0x2260, bus register from the world's VBUS); the accelerometer address NACKs.
//   - ADC: CC1/CC2/D+/D- from the world's steps.
//   - Clock: virtual nanoseconds. I2C costs bus time per byte, delay_us() costs its measured hardware length, the LCD
//     costs wire time plus a calibrated CPU share per pixel (tests/fake_lcd.h LcdTiming). millis()/micros() reads cost a
//     little so a polling loop always moves on.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace world {

struct VbusPoint { double t; double mv; };
struct RpStep { double t; int cc1, cc2; };            // the level the measure block sees on each line (mV)
struct AdcStep { double t; int cc1, cc2, dp, dm; };   // what the ADC reads on the nets (mV)
struct Frame {
    double t;                      // recording time the frame was complete on the wire (ms)
    int line;                      // 1 or 2
    std::vector<std::uint8_t> raw; // token (0xE0 SOP, 0xC0 SOP', 0xA0 SOP'') + header LE + data + CRC
    bool tpl;                      // filled in from a template recording (the K2 missed it there)
};
struct Episode { std::string name; double t0, t1, rise; int line; };
struct Expect {
    std::string episode;
    std::string first_sop;     // first decoded SOP message (SOP'/SOP'' ignored: a source's cable look comes first)
    int caps_min = -1;         // Source_Capabilities the driver decoded in the episode ("[PD] Source_Capabilities received")
    int verdict_caps_min = -1; // the [LINK] verdict's caps= (the link layer's count this attach); opt-in, see the test
    std::string verdict;       // [LINK] pd verdict token that must appear in the episode
};
struct Scenario {
    std::string name, recording, fw, mode = "inline";
    std::vector<VbusPoint> vbus;
    std::vector<RpStep> rp;
    std::vector<AdcStep> adc;
    std::vector<Frame> frames;
    std::vector<Episode> episodes;
    std::vector<Expect> expects;
};
bool load_scenario(const std::string& path, Scenario& out, std::string& err);

const char* msg_name(const std::vector<std::uint8_t>& raw);   // "Source_Capabilities", "PS_RDY", ...
const char* sop_name(std::uint8_t token);                       // "SOP", "SOP'", "SOP''"

// Timing model. Every number is in the report; see docs/TESTING.md "World replay" for where each comes from.
struct Timing {
    double fusb_byte_us = 43.0;     // FUSB302 bus (bit-banged ~400 kHz plus GPIO work): 1-byte write = 3 bytes ~130 us (trace)
    double ina_byte_us = 50.0;      // INA bus (~200 kHz)
    double delay_us_factor = 2.79;  // delay_us(1000) on hardware: the scan's delay_ms(2) + read + write = 5.85 ms (trace)
    double clock_read_us = 0.2;     // one millis() / micros() / ticks() read
    double pass_us = 15.0;          // one main-loop pass outside the calls modelled here
    double px_us = 1.0667;          // LCD wire time per pixel at /8 (lcd_timing.hpp wire_us)
    double lcd_call_us = 2.0;       // per stream_* call
    double lcd_window_us = 8.0;     // per window (CASET/RASET/RAMWR)
    double px_cpu_us = 0.0;         // CPU per streamed pixel (rendering), calibrated on the 421.4 s Home repaint
};

// One run: the world from world_t0 (recording ms) on; the firmware's clock starts at 0 there.
void start(const Scenario& sc, double world_t0_ms, const Timing& t);
double world_ms();          // current recording time
double fw_ms();             // the firmware's clock
void spend_us(double us);   // advance the clock (processes world events on the way)
void wfi();                 // sleep to the next SysTick (1 ms boundary)

struct FrameFate {
    int index;                 // into Scenario::frames
    bool delivered = false;    // reached the RX FIFO
    bool read = false;         // the driver popped its token
    std::string lost;          // why it never reached the FIFO ("unrouted: TXCC=..", "flushed", "overflow", "before boot")
    double fw_read_ms = -1;
};
const std::vector<FrameFate>& fates();
struct LogLine { double world_ms; std::string text; };
const std::vector<LogLine>& logs();
unsigned tx_starts();      // TX_START writes (an inline sniffer must never transmit)

}  // namespace world
