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
struct LogLine { double world_ms; std::string text; int life = 0; };
const std::vector<LogLine>& logs();
unsigned tx_starts();      // TX_START writes (an inline sniffer must never transmit)

// ---- The reboot world: power loss, cold boot and a charger that serves a valid Rd ----
// The K2 lives on the charger's VBUS. Each firmware life is a fork of the test's run process: when the K2's rail falls below
// the brown-out level the life ends (_exit), and the next one boots cold from the same pristine image, so every static starts
// over as after a power-on reset. What a real power loss keeps is kept in shared memory: the flash volume (the /rd.rec marker,
// the stores), the backup registers (the session record DR22..DR37 that vouches for the marker, the DR21 pin mark) while the dark
// is shorter than PowerModel::bpr_hold_ms, the FUSB302's registers while its own supply held, and the world itself (clock,
// charger, VBUS, log). SRAM (.noinit, the diagnostic probe) is lost in every dark unless a scenario says otherwise. Every number
// is in the report and in docs/TESTING.md.
struct ChargerModel {               // a PD source on one CC line (its Rp), the e-marker look not modelled
    int line = 2;                   // the K2 line the charger's CC wire lands on
    std::vector<std::uint8_t> caps; // Source_Capabilities data objects, 4 bytes each
    double cc_debounce_ms = 150;    // tCCDebounce: Rd present this long before VBUS goes on (100-200 ms)
    double pd_debounce_ms = 15;     // tPDDebounce: Rd gone this long while attached = a detach, VBUS off (10-20 ms)
    double rise_mv_per_ms = 250;    // VBUS slew on
    double fall_mv_per_ms = 20;     // VBUS decay off, with the K2 as the load (0 V ~300 ms after the cut, CHARGER_QUIRKS)
    double first_caps_ms = 100;     // first Source_Capabilities after VBUS reaches vSafe5V
    double caps_period_ms = 150;    // tTypeCSendSourceCap while nobody acknowledges
    int caps_count = 50;            // nCapsCount: then no PD (VBUS stays at 5 V)
    double sender_response_ms = 27; // tSenderResponse: acknowledged caps with no Request = Hard Reset
    double accept_ms = 6, ps_rdy_ms = 90;   // Request -> Accept -> PS_RDY (runs/selfpwr-drop/e8: 5.9 ms, 90.1 ms)
    double hr_off_ms = 800;         // a Hard Reset: VBUS off, then back after tSrcRecover (0.66-1 s) without a CC check
};
struct PowerModel {                 // the K2's supply (all inferred, see docs/TESTING.md)
    double dropout_mv = 300;        // 3.3 V rail = supply - dropout, capped at 3300
    double mcu_bor_mv = 2700;       // the MCU runs at or above this rail level (cold boot on the way up)
    double fusb_por_mv = 2000;      // the FUSB302 keeps its registers at or above this rail level; below it they are lost
    double boot_ms = 1.0;           // reset to the first instruction of boot() (clock start-up)
    double bpr_hold_ms = 5000;      // the backup registers survive a dark (rail below mcu_bor_mv) up to this long, a longer one clears
                                    // them. Measured: kept over a 1 s dark, lost after a minutes-long unplug; the limit is unmeasured.
                                    // The default keeps them over the Hard Reset's ~0.7 s dark; 0 loses them in any dark.
    bool sram_kept = false;         // .noinit survives a dark: never on this board (0 of 32 probe words after 1 s), a switch only
};
struct RebootEvent { double t; enum Kind { HardReset, Unplug, Replug } kind; };
struct RebootScenario {
    std::string name;
    ChargerModel ch;
    PowerModel pm;
    bool hid_loop = false;          // a C-to-C loop from the K2's receptacle to its HID port: the HID port's Rd is on the bus and
                                    // its VBUS feeds the K2's aux rail (masks the software bridging: the charger always sees an Rd)
    double plus_until_ms = 0;       // PLUS held from power-up until then (a key mode boot; the key's Rd on CC2 while held)
    double end_ms = 10000;
    std::vector<RebootEvent> events;
};
void reboot_start(const RebootScenario& sc, const Timing& t);   // the K2 unpowered at world time 0
bool reboot_wait_boot();      // the run's process, K2 dead: run the world until the rail can boot it (false: the scenario ended)
void life_begin();            // the life's process, before boot(): firmware clock 0, the backup registers and .noinit as the dark left them
void life_end();              // the life reached the scenario's end alive (the caller then exits 0)
void note(const char* text);  // a line in the world's log, in the firmware log's order, tagged with this life (a test's own observation)
inline constexpr int kLifeDied = 3;   // a life's exit status when the rail fell below the brown-out level
bool noinit_found();          // the .noinit section of this executable was located and fits the world's copy (sram_kept can be modelled)
bool plus_held();             // the PLUS key at this moment
struct ChargerStats {
    int attaches = 0, detaches = 0, hard_resets = 0, contracts = 0, requests = 0, caps_sent = 0, caps_acked = 0;
    int raise_requests = 0;         // Requests for an object above 1 (above vSafe5V) the charger heard
    double rd_gap_max_ms = 0, rd_gap_max_at = 0;   // the longest time an attached charger with VBUS at 5 V saw no Rd
    double first_attach_ms = -1;
    bool attached_at_end = false;
};
struct RebootResult {
    int lives = 0;
    bool alive_at_end = false;
    double last_boot_ms = -1;
    ChargerStats ch;
};
RebootResult reboot_result();   // after the last life (the run's process)

}  // namespace world
