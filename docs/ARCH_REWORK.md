# Main-loop architecture rework

Phase 1 is measurement and design only: the firmware behaves as before, apart from the counters below. Phase 2 is the
plan at the end. Its steps are ordered so that each one keeps the firmware working and testable through the world
replay.

## The problem in one paragraph

The firmware runs one cooperative loop (`src/core/main_loop.cpp`, ARCHITECTURE §2.2). Every 20 ms it runs `Ui::tick`,
and a page paint runs to completion inside that call. A full page costs about 1.07 us of SPI wire time per pixel at
/8, plus about 1.65 us of CPU rendering per pixel (calibrated on the bench). A full repaint (clear plus page,
115 200 px) therefore holds the pass for about 215 ms.

During a paint only the LCD idle hook runs, from inside `lcd.cpp`. It runs `fusb302::service`, the UFCS and HVDCP
services and the unattached CC scan. Everything else waits for the paint to end: the buttons, `fusb302::poll`,
`power::poll`, the link layer in `Ui::tick`, the Black Box VBUS samples, `usb::poll` and the remote.

The hook itself does not run on a time base. It runs every 2048 pixels streamed and every 468 pixels inside a DMA wait,
and `stream_begin()` zeroes the pixel count (`src/drivers/lcd.cpp:404`). A paint made of many windows under 2048 px
each (text fields) therefore never runs it. On Detail the PD service then pauses for 47 ms.

## Instruments added in phase 1

| Instrument | What it measures | Where |
|---|---|---|
| `k2::loopstat::stats` | The gap between main-loop passes and the gap between 1 ms PD services (histogram <=1 2 5 10 20 50 100 >100 ms, max). The longest pass per page and the pixels it sent | `include/k2/core/loop_stats.hpp`, noted in `src/core/main_loop.cpp` (pass top, the 1 ms block, the idle hook, the pass end) |
| `k2_remote.py --json debug loop [--page N] [--reset]` | The same counters from the device (DebugLcd op 1, 58 bytes) | `include/k2/core/remote_proto.hpp` LoopSnapshot, `src/core/remote.cpp` `Remote::debug_loop` |
| `host-build/test_paint_survey` | Every page and state, every UI call timed with the world replay's LCD cost model. Per state: the worst call (ms, px), the calls over 20 ms, and the longest stretch with no idle-hook round | `tests/test_paint_survey.cpp`, report `runs/paint-survey/report.txt` |
| World replay gap table | The firmware's own counters over each episode window, summed per scenario, episode and page | `tests/test_world_replay.cpp`, end of `runs/world-replay/report.txt` |
| `tools/loop_survey.py` | The device numbers, page by page (read-only) | below |

Both host checks print `OVER` against their ceilings and pass. `K2_GAP_GATE=1` makes them fail, and both are red today,
as they should be. When phase 2 brings them under the ceilings, the variable goes into `make gate`.

- Ceiling for a pass gap and for a single UI call: 20 ms. That is one slow-task period (`kLoopMs`). The power poll,
  `fusb302::poll` and the 20 ms button debounce all assume it.
- Ceiling for the PD service gap: 10 ms, a third of tSenderResponse.

Cost of the instruments: text +612 B, bss +312 B against 79866c91. The counters add a few `micros()` and `millis()`
reads per pass.

## Host measurements

### Worst paint per page (`test_paint_survey`)

Each state is 250 ticks of 20 ms with the values moving, plus the page switch and a forced repaint. "PD svc gap" is the
longest stretch inside one call with no idle-hook round, so with no `fusb302::service`.

| Page | State | Worst call | Its px | Which call | Calls > 20 ms | PD svc gap |
|---|---|---:|---:|---|---:|---:|
| Home | live 20 V | 214.5 ms | 115 200 | forced repaint | 18 of 252 | 13.8 ms |
| Home | idle | 214.5 ms | 115 200 | forced repaint | 2 of 252 | 13.8 ms |
| Detail | live / idle | 135.0 ms | 87 024 | forced repaint | 2 of 252 | **48.1 ms** |
| Charger (setup page) | live | 156.4 ms | 114 542 | forced repaint | 2 of 252 | 15.4 ms |
| Lines | live | 128.7 ms | 91 880 | forced repaint | 2 of 252 | 13.6 ms |
| Tools | live | 160.2 ms | 114 575 | forced repaint | 2 of 252 | 14.5 ms |
| Settings | live | 162.8 ms | 114 098 | forced repaint | 2 of 252 | 14.3 ms |
| Connection (PD) | live | 209.9 ms | 141 376 | switch in | 2 of 252 | 19.8 ms |
| Diag | live | 140.0 ms | 89 192 | forced repaint | 2 of 252 | **45.8 ms** |
| Charger test | wizard | 159.2 ms | 113 937 | pick cable | 4 of 16 | 15.7 ms |
| Charger test | running (267 s, all units) | 144.3 ms | 97 173 | confirm START | 17 of 13 372 | **41.0 ms** |
| Charger test | results | 157.5 ms | 111 168 | forced repaint | 9 of 135 | 29.3 ms |

Every page goes over the ceiling on a switch or a repaint. Two observations:

- A forced repaint sends twice the panel area (`Ui::repaint` = `lcd::fill(kBg)` + `redraw`, `src/ui/ui.cpp:1032`).
  The Connection switch sends 2.45 times the panel area.
- On Home a live tick goes over 20 ms in 18 of 252 calls: the value cells repaint whole.

### Gap distribution per world replay scenario

These are the firmware's own counters over the episode window, summed over the 10 loop phases. Buckets are <=1, <=2,
<=5, <=10, <=20, <=50, <=100 and >100 ms.

| Scenario / episode / page | Pass gap histogram | Pass max | PD service gap histogram | Svc max |
|---|---|---:|---|---:|
| h-side-inline/rise2538.6/Home | 26342 887 188 42 103 14 0 40 | 194 | 26342 893 241 1174 46 14 0 0 | 30 |
| h-side-inline/rise2538.6/Detail | 32740 1209 123 30 60 30 0 0 | 47 | (same) | 47 |
| late-attach-cycles/rise166.8/Home | 27206 926 196 26 56 21 0 40 | 186 | 27206 933 244 1063 40 10 0 0 | 29 |
| late-attach-cycles/rise166.8/Detail | 32886 1345 67 54 48 30 0 0 | 29 | (same) | 29 |
| late-attach-cycles/rise420.8/Home | 25245 840 148 29 61 39 0 50 | 187 | 25245 855 209 1360 64 10 0 0 | 30 |
| late-attach-cycles/rise420.8/Detail | 31879 1269 102 97 60 45 0 0 | 44 | (same) | 44 |
| late-attach-cycles/rise424.7/Home | 26746 938 178 37 48 33 0 40 | 187 | ... | 30 |
| late-attach-cycles/rise431.5/Home | 25712 804 143 15 52 33 0 50 | 193 | ... | 28 |
| late-attach-cycles/rise435.4/Home | 27137 958 186 38 39 26 0 40 | 187 | ... | 30 |
| late-attach-cycles/rise490.8/Home | 27275 882 187 29 45 20 0 40 | 194 | ... | 29 |
| late-attach-cycles/rise495.8/Home | 27148 877 187 26 58 18 0 40 | 194 | ... | 30 |
| late-attach-cycles/rise500.6/Home | 27748 883 194 41 55 19 0 39 | 194 | ... | 30 |
| (the other Detail rows) | | 44-47 | | 44-47 |

All 18 scenario pages are over the ceilings. The full table is at the end of `runs/world-replay/report.txt`.

- **Home** has 4 to 5 passes per run over 100 ms (the 150-190 ms repaint as VBUS appears). Its PD service gap stays
  near 30 ms, because the large Home cells stream more than 2048 px per window. 30 ms is above tSenderResponse
  (27 ms minimum).
- **Detail** never has a long pass, but its service gap equals its pass gap: on Detail the idle hook never runs.
- **The `[LINK]` verdict undercount** is a loop-gap bug, not a PD-service bug. In this run set it shows on 15 Home runs
  and 0 Detail runs. Detail's PD service is worse, but its pass gap stays under 50 ms. The link layer samples driver
  state in `Ui::tick` (`src/ui/ui.cpp:1135` `link_.update(in)`). Corrected in phase 2 (E2): the counter it samples
  (`fusb302::rx_count()`) was already monotonic, so no increment was lost; the count's baseline was. `Link` took it at
  the attach edge, the first tick that saw VBUS, and on Home that tick came after a 200 ms repaint during which the
  driver had decoded the new source's caps, so they counted for nobody.

## Duty table

The paths are as of the branch head. "Serves" says who runs the duty while a paint holds the pass:

- **hook**: the LCD idle hook, at the cadence measured above;
- **ISR**: the duty keeps running;
- **loop**: the duty waits for the paint to end.

The **Risk** column judges each duty against the measured gaps: a pass gap of up to 194 ms, a PD service gap of 30 ms
on Home and up to 48 ms on Detail and Diag. The worst blocking driver call is listed separately.

| Duty | Deadline (source) | Path today | Serves during a paint | Risk |
|---|---|---|---|---|
| GoodCRC to a received PD message | tReceiverResponse 15 ms (`pd_spec_timing.hpp:15`) | Chip AUTO_CRC (`update_crc` `fusb302.cpp:465-476`), off in passive mode (`pd_passive.hpp:294-297`) | Chip | none |
| Request after Source_Capabilities (active sink) | Source tSenderResponse 27..50 ms (`pd_spec_timing.hpp:16-17`); the K2 rule is "within ~25 ms" | `fusb302::service` (`fusb302.cpp:2489`) → `active_tick` (`:1923`, called at `:2558`) → `Sink::on_message` → `send_request` (`pd_sink.cpp:734`, `:428`) | hook | **at risk**: PD service gap 30 ms (Home), 41-48 ms (Detail, Diag, charger test running) |
| Hard Reset received | Act before the source's tSrcRecover; tolerant | INTERRUPTA I_HARDRST latched at every read (`fusb302.cpp:255`, `:335`, `:396`, `:1570`), handled in `service` (`:2501-2518`) | hook | low (latched, never lost; acted on up to one service gap late) |
| PS_RDY wait | K2 give-up 1500 / 4000 ms (`pd_sink.hpp:139-140`) | `Sink::tick` from `service` | hook | none |
| PPS keep-alive | Spec: at least every 10 s; K2 every 5 s (`pd_sink.hpp:143`) | `Sink::tick` from `service` | hook | none |
| EPR keep-alive | tSinkEPRKeepAlive 250..500 ms; K2 400 ms (`pd_sink.hpp:144`) | `Sink::tick` from `service` | hook | thin: 400 ms + gap must stay < 500 ms, so the service gap must stay < 100 ms. Met by paints (≤48 ms), not by the blocking calls below |
| RX FIFO drain (passive sniff and active) | FUSB302 RX FIFO, 80 bytes ≈ 2 Source_Capabilities frames of 7 PDOs | `service` drains up to 8 rounds; INT_N edge time-stamped in `EXTI15_10_IRQHandler` (`fusb302.cpp:3136`) → `on_int_n` (`:2031`); `service` is not gated on `g_irq` | hook (ISR stamps only) | medium on Detail/Diag: 48 ms holds up to ~3 frames on a chatty source |
| Unattached CC scan | Catch the source's caps ~130 ms after VBUS rises; 500 ms cadence (`pd_scanpace.hpp:26`) | `poll` → `scan_if_due` (`fusb302.cpp:2432`, `:2377`); during a paint `idle_hook` → `scan_service` every ≥10 ms when unattached (`main_loop.cpp:181-186`, `fusb302.cpp:2706`) | hook | low. But the scan itself blocks: `detect_cc_orientation` (`fusb302.cpp:608`) has delay_ms(2) per line, ~6 ms unpinned and ~12 ms pinned |
| Rd hold on a self-powered boot | Source tPDDebounce 10..20 ms. The world replay's bpr-kept gap is 9.2 ms | `bootpd::early` (`src/core/boot_pd.cpp:37-57`), `self_power_bringup` (`:20-35`: service every ms, poll every 10 ms for 3 s); afterwards the chip register holds Rd | Before the main loop, no paint | none from paints (boot path) |
| Rd hold after a detach | kRdHoldMs 1500 ms (`pd_passive.hpp:16`) | `fusb302::poll` (20 ms slow tasks) | loop | none (the hold gets ≤ 0.2 s longer) |
| INA226 sampling | Conversion 16 × (140 + 140 us) ≈ 4.5 ms (config 0x4407, `power_monitor.cpp:75`, `:100`) | `power::poll` in slow tasks (`main_loop.cpp:101`, `power_monitor.cpp:117`); `fast_vbus_mv` direct read in the hook when unattached (`power_monitor.cpp:170`) | loop | medium: no average update and no `vbus_fast_mv` refresh for up to 194 ms. The charger test judges steps on `vbus_fast_mv`, and it gates its own paint while a step is in flight (`kFlightPx`, `ui_pages.hpp:495`) |
| Black Box VBUS/IBUS samples | 50 ms / 100 mV / 20 mA gate | `pdtrace::vbus_sample` in slow tasks (`main_loop.cpp:104`) | loop | medium: holes of up to 194 ms in the VBUS timeline. PD events themselves are stamped in `service` |
| Transition capture | 1 kHz timeline while armed | `transcap::poll` every pass, in the hook (`main_loop.cpp:188`) and in the delay_ms wait hook | hook | low |
| USB HID / CDC | Host polls every 1 ms; enumeration timeouts in the hundreds of ms | ISRs USB_LP / HP only queue events (`usb.cpp:232`, `:238`); `tud_task` in `usb::poll` (`usb.cpp:118`, every pass); `usb::service` from LCD serve and `delay_ms` (`usb.cpp:140`) | hook-like (LCD serve) | low for enumeration. Log reports wait and drop under `drop=`. `poll_gap_max` resets at every heartbeat (`usb.cpp:218`) |
| Remote commands and dead-man lease | Lease 30 s default, `kSafeHoldMs` 5 s (`include/k2/core/lease.hpp`) | `Remote::poll` every pass (`remote.cpp:581`) | loop | none (gap ≪ lease) |
| Buttons | Debounce 20 ms, long 500 ms, hold 5 s (`buttons.hpp:13-15`) | Sampled every 1 ms in `pass` (`main_loop.cpp:230-262`) | loop | **at risk**: a press shorter than a paint (Home repaint 215 ms) is never seen. A press that ends inside a paint is debounced late |
| `[LINK]` verdict caps count | One count per Source_Capabilities | `Ui::tick` → `link_.update` (`src/ui/ui.cpp:1135`) samples driver state | loop | was **broken**: undercount on 15 Home runs; fixed by E2 (the count starts at the detach) |
| QC2/QC3 handshake (HVDCP) | Levels for 100 / 100 / 50 ms, drop poll 10 ms, 2 s timeout | `fctrig::service_hs` from the hook (`fc_engine.cpp:1005`) and `poll_step` | hook | low (deadline-driven, tolerant) |
| QC3 / QC3+ pulse trains | 2250 us hold/gap (QC3), 40/50 us (QC3+) (`qc.hpp:57-58`) | `dpdm::play` busy-waits on DWT with IRQs masked when ≤ 20 ms (`dpdm.cpp:25`, `:115-128`), from `poll_step` | loop (bursts ≤ 18 ms between 20 ms gaps) | none for the pulses (masked). The burst itself masks SysTick, USB and EXTI for up to 18 ms |
| FCP / SCP / AFC single-wire exchange | 162 us bits, slave ping 0.3-4 ms after a 2 ms wait (`include/k2/fastcharge/wire.hpp:17-23`) | `exchange_once` (`fctrig.cpp:49`) blocks ~10-30 ms per exchange, IRQs masked in `wait_dm` / `capture_dm`; SCP keep-alive every 100 ms; all from `poll` | loop | low for the wire (masked). The exchange and keep-alive are not covered by `paint_critical` (`fc_engine.hpp:157`) |
| UFCS handshake | Signal timing in ms (`fctrig.cpp:128`) | `ufcs_open` from `poll`, blocking ~50 ms+; paint gated (`UfcsOpen` in `paint_critical`) | loop (gated) | low |
| UFCS messages | The K2 ACKs every message within tACKReceive 10 ms (`ufcs.hpp:30`); UART RX is a 260-byte DMA ring (`uart2.hpp:15`), ~22 ms of bytes at 115 200 | `service_ufcs` from the hook and `poll` (`fc_engine.cpp:950`) | hook | **at risk** (inferred): Ping..Keep are not paint-gated, and a 30-48 ms service gap exceeds the 10 ms ACK window |
| VOOC receive slices | 15 ms listen slice per poll, IRQs masked (`fc_engine.cpp:33`, `dpdm.cpp:206-217`) | `poll` only; `VoocListen` is paint-gated | loop (gated) | low. The masked 15 ms slice is itself a SysTick, USB and EXTI blackout |
| VIVO frames | One frame every 50 ms, then a 100 ms tick | `poll` only; `Vivo*` phases are paint-gated | loop (gated) | low |
| D+/D- edge sniffer | Edge timestamps | `EXTI2_IRQHandler` / `EXTI9_5_IRQHandler` (`dpsniff.cpp:417-418`) → `isr_body` (`:56`); drained by `dpsniff::poll` in slow tasks | ISR | none |
| Watchdog | none | No watchdog runs. IWDG is used only as a one-shot reset (`sysreset.cpp:54-76`) | n/a | a hang is a hang |

Blocking driver calls that stall the pass whatever the paint does:

- `transmit` hard reset: delay_ms(5) (`fusb302.cpp:300`);
- `detect_cc_orientation`: ~6-12 ms, twice a second while unattached;
- `scan_orientation`: delay_ms(40) (`:377`);
- the legacy-only detach: delay_ms(300) (`:1285`);
- the cable exchange busy loop (`:1566`);
- FCP exchanges: 10-30 ms;
- QC3 bursts: ≤ 18 ms masked;
- VOOC slices: 15 ms masked.

`delay_ms` keeps USB and the transition capture running (`systick.cpp:62`), but not the PD service.

No NVIC priority is set anywhere, so every IRQ runs at the reset priority and none preempts another.

## Options

The flash and RAM figures are estimates (inferred) against today's image: text 551 348 B and 19 912 B of free RAM
above .bss.

### A. Budgeted renderer (generalise `kFlightPx`)

Every page paints through a pixel budget per pass, about 4 000 px (≈ 11 ms). The rest is queued as dirty items and
painted on the next passes. The charger test already does this while a step is in flight (`Budget`,
`ui_charger_test.cpp:2196`).

- **Fixes:** the pass gap for every duty marked "loop": buttons, the link verdict count, power and Black Box samples,
  `fusb302::poll`, USB, remote. It also bounds the PD service gap, because no single call is long.
- **Flash:** +2-4 KB (a dirty-item queue in the kit, resumable repaints).
- **RAM:** ≈ 0.3-0.6 KB (the queue). It fits the page arena.
- **Risk:** medium.
  - A page shows half old and half new content for a few passes. That is visible only on full repaints, which already
    take 150-215 ms today.
  - Every page has to express its paint as items. Home's cells and the charger-test grid already do. Detail, Lines,
    Tools, Connection and Diag do not.
- **Work:** large, one page at a time; each page is independent and testable with `test_paint_survey`.

### B. PD and power duties in interrupts

FUSB302 INT_N on EXTI (already wired, `fusb302.cpp:3136`). Either an ISR-driven PD state machine, or a SysTick or
PendSV-driven `service()`.

- **Fixes:** the PD service gap, the Request deadline, the RX FIFO drain, and the UFCS ACK if fctrig moves too.
  It does not fix buttons, the link verdict count, the INA226 samples or the Black Box holes (all loop duties).
- **Flash:** +1-3 KB. **RAM:** < 0.5 KB.
- **Risk:** high.
  - `fusb302` is reached from the main loop by `poll`, the scan, the cable exchange, e-marker, spoof, the remote and the
    charger-test runner. Each of those would need a lock or a hand-off, or the ISR would tear a transaction on the
    bit-banged bus.
  - The FUSB302 and INA226 buses are separate pins (PB10/PB11 and PB8/PB9), so the bus-sharing worry in the brief is
    only about FUSB302 users.
  - A frame read is ~35 bytes × 43 us ≈ 1.5 ms of bit-banging inside an ISR. With no NVIC priorities that blocks USB.
  - The proven PD rules ("one owner of the Request", "latch I_HARDRST wherever INTERRUPTA is read") become concurrency
    rules.
- **Work:** large; the hardest to test (the world replay is single-threaded; it would need an interrupt model).

### C. A + B

Both fixes, both costs. B's extra coverage over A is the PD service gap during the blocking driver calls (`delay_ms`
inside `fusb302`, the FCP exchanges). Those are better fixed where they are, by making the calls state machines.

### D. LCD SPI over DMA with line or chunk buffers

DMA2 channel 2 already sends runs of 32 px or more (`lcd.cpp:412`, `kDmaMinPixels` in `lcd_timing.hpp:54`), but the CPU spins in `dma_wait`. The source is
the caller's stack line (80 px in `gfx_text`, `src/ui/gfx.cpp:70-79`).

Rendering chunk N+1 while chunk N is on the wire changes the cost per pixel from CPU + wire (2.72 us) to max(CPU, wire)
(1.65 us), about 39 % less. The 4 bpp font path renders into the line (`render_row_span`, `src/ui/text.cpp:61`) and
keeps working; it only needs two static chunk buffers instead of a stack line.

- **Fixes:** nothing in the duty table on its own. A Home repaint goes from ~215 to ~130 ms, still 6× over the
  ceiling. It makes A's budget buy ~1.6× more pixels per pass.
- **Flash:** +0.5-1 KB. **RAM:** 2 × 240 px × 2 B = 960 B, or 2 × 80 px = 320 B.
- **Risk:** low to medium (buffer lifetime across calls, the DMA stall fallback).
- **Work:** small to medium, contained in `gfx` and `lcd`.

### E. Cheaper steps that A, B and D leave on the table

The measurements point at three small, contained changes:

1. **A time-based service cadence (E1).**
   - The change: run the idle hook when a millisecond has passed, checked at every stream call and inside the long
     rendering loops. Stop zeroing the count in `stream_begin`, and drop the pixel count.
   - Result (inferred from the model): the PD service gap during a paint becomes ~1 ms plus the longest single stream
     call (~0.7 ms).
   - Fixes: the Request deadline, the RX FIFO drain and the UFCS ACK during paints, without touching the PD engine.
   - Cost: tens of bytes, low risk. The hook already runs at stream call points today, so no new code becomes reentrant.
2. **Events counted where they happen (E2).**
   - The change: the link verdict counts Source_Capabilities from a driver counter (a serial number bumped in
     `service`), not from samples in `Ui::tick`.
   - Fixes: the undercount, whatever the pass gap. Small; host-tested by the existing world expectations
     (`verdict_caps_min`).
3. **Buttons latched at 1 ms (E3).**
   - The change: run the button detectors from the idle hook as well, with the events queued and delivered by the pass.
     Or sample the raw levels in SysTick into a small ring.
   - Fixes: presses lost during paints. Small.

There is also the overdraw (fixed in phase 2, step 4): a repaint clears the panel and then paints it opaque again (`ui.cpp:1033`), which is 61 ms of
wire time per repaint. Skipping the clear for pages that paint every pixel is a small step towards A.

### Recommendation

**E1 + E2 + E3 first, then A, with D as an optional speed-up. Not B.**

- E1 to E3 fix every duty marked "at risk" or "broken today" except the loop-duty samples (INA226 average, Black Box,
  slow tasks). The steps are small and independent, each is measurable by the gates already built here, and none adds
  concurrency.
- A then removes the long passes themselves, which E1 to E3 only work around. It is the only option that brings the
  pass gap under 20 ms.
- B buys little over E1. Since E1 makes the hook run on time, a timer-driven PD service only adds coverage inside the
  blocking driver calls. Its price is a concurrency model around a hardware-proven PD engine, and the single-threaded
  world replay could no longer test it.

## Phase 2 plan

After every step: `make gate` is green; the world replay and the paint survey show the step's effect in
their tables; `tools/loop_survey.py` on hardware confirms it. When a ceiling holds, its `K2_GAP_GATE` check moves into
`make gate`, so a step cannot be undone silently.

1. **E1: time-based idle hook.**
   - Change: `lcd.cpp` serves on elapsed time instead of on pixels, and `stream_begin` no longer zeroes the count.
     `tests/fake_lcd.cpp` follows.
   - Expected: the service gap is ≤ 2 ms in both surveys (Detail 47 → ~2, Home 30 → ~2).
   - Then: gate the PD service ceiling (10 ms) in the world replay.
   - **Done (k2/arch-e1).** `lcd::ServicePacer` (`lcd_timing.hpp`, host-tested in `test_lcd_timing`) runs a round once
     `kServiceUs` (1000 µs) has passed since the last round ended, on the cycle counter; `stream_begin` no longer
     resets it, `pump()` forces a round, a round is never nested. The fake LCD uses the same class on the virtual clock.
     Paint survey, PD service gap max (ms), before → after:

     | Page | Before | After |
     |---|---:|---:|
     | Home live / idle | 13.8 / 13.8 | 1.1 / 1.1 |
     | Detail live / idle | 48.1 / 48.1 | 1.2 / 1.1 |
     | Charger | 15.4 | 1.2 |
     | Lines | 13.6 | 1.2 |
     | Tools | 14.5 | 1.2 |
     | Settings | 14.3 | 1.2 |
     | Connection | 19.8 | 1.3 |
     | Diag | 45.8 | 1.2 |
     | CT wizard / running / results | 15.7 / 41.0 / 29.3 | 1.3 / 1.3 / 1.3 |

     The 10 ms PD service ceiling is now enforced in `test_paint_survey` (part of `make gate`). The world replay's
     service max went from 28-30 ms (Home) and 29-47 ms (Detail) to 23-24 ms on all 18 pages, and stays report-only
     there: every remaining gap over 10 ms (one per phase, 13-24 ms) is a blocking CC scan (`detect_cc_orientation`,
     from the pass or from the hook's `scan_service`), which is step 6. The cost: each 1 ms round reads the FUSB302
     over I2C, so the longest pass grows (Home 186-194 → 211-232 ms, Detail 29-47 → 36-68 ms in the model).
     `kServiceUs` is the knob if the hardware shows the same.
2. **E2: caps counted at the driver.**
   - Change: a driver-side serial number per Source_Capabilities. The link layer counts the serial numbers it has
     not yet seen.
   - Expected: the 15 undercounting Home runs reach the expected `verdict_caps`.
   - Then: turn on `verdict_caps_min` for the late-attach episodes.
   - **Done (k2/arch-e1), with a corrected diagnosis.** The driver serial existed already (`fusb302::rx_count()`,
     monotonic since boot) and `Link` already counted deltas of it. The bug was the baseline: `set_attached(true)` took
     it at the first tick that saw VBUS, after a Home repaint during which the caps had arrived (`[DEBUG-e2]` trace of
     `late-attach-cycles:rise420.8:Home:53`: caps 1 decoded at 420928 while the link was detached with a stale 165 mV
     sample, caps 2 inside the 400 ms tick gap, attach edge at 421462 with baseline 2 → `caps=0`). The baseline is now
     the counter at the last detach (the same one `waiting()` uses). `test_pd_verdict` covers it; `verdict_caps>=1` is
     on for the 8 late-attach and the h-side-inline episodes. World replay: `verdict_caps=0` on 15 runs (11 after E1)
     → 0; 110 runs count 1, 70 count 2. With the fix reverted the scenario expects fail 11 runs.
3. **E3: buttons from the hook.**
   - Change: the detectors also run in the idle hook, and their events are queued for the pass.
   - Test: a host test pressing during a forced repaint (tests/test_ui_* with g_lcd_timing).
4. **Overdraw: no clear before an opaque repaint.**
   - Expected: the forced repaint drops by ~57 600 px. Paint survey numbers go down; screenshots stay identical
     (render tests).
   - **Done (k2/arch-e1).** `Ui::repaint` of an opaque page or menu (`page_opaque` / `menu_opaque`: Home, Detail, Lines,
     Tools, Settings, Diag, UsbLink) skips `lcd::fill(kBg)` and paints over the screen (`paint_incoming(blank=false)`,
     the path an opaque switch already takes). The rest (Charger test, Connection, the other menus, the full-panel QR
     pages) keep the clear: their paint relies on a kBg ground. `test_ui_capture` `test_repaint_opaque` repaints every
     walk page over a magenta screen in the four rotations and requires a clean paint's pixels and no clear (opaque) or
     exactly one (the rest); red before (57 600 px cleared). Pixels sent by a forced repaint, before → after: Home
     115 200 → 57 600, Detail 87 024 → 58 800, Diag 89 192 → 58 272; the paint survey's worst call: Home 214.5 →
     153.0 ms, Detail 135.0 → 106.6, Lines 128.7 → 95.3, Tools 160.2 → 99.9, Settings 162.8 → 102.6, Diag 140.0 →
     108.4. All 543 host render dumps (`K2_DUMP`, 17 UI tests) are byte-identical. `test_ui_overdraw` now holds an
     opaque page's repaint to the opaque switch bound; the tests that use a repaint as the clean reference clear first.
5. **A, page by page.**
   - Change: a kit-level `Budget` (`kFlightPx` generalised) and a dirty-item queue. Home first (cells are already
     items), then the charger test outside a step, Connection, Detail, Diag, Lines, Tools, Settings.
   - Each page lands when its rows in `test_paint_survey` are under 20 ms.
   - When every row is, the paint-survey ceiling goes into the gate, and the world replay's pass-gap ceiling (20 ms)
     follows.
6. **Blocking drivers as state machines.**
   - Scope: `detect_cc_orientation`, the legacy-only 300 ms detach and `scan_orientation` in `fusb302`.
   - Exposure: the world replay already exercises the scan path.
7. **D (optional).** Double-buffered chunk DMA in `gfx_text` and the widgets, if the budgets of step 5 feel slow on
   the device.

## Hardware measurement (for the coordinator)

`tools/loop_survey.py` is read-only.

- It sends only `ping`, `state`, `page`, `debug loop` and `debug lcd`, and refuses anything else before any USB
  traffic.
- It holds the device lock, runs every call as a subprocess with a timeout, and ends on Home.
- It needs USB HID on and a firmware with `debug loop` (this branch).

For each page it does the following:

1. Zero the counters and open the page.
2. Dwell.
3. Read the counters: the page's worst pass (ms, px), the pass gap and PD service gap histograms, the paint totals as
   a delta, and the diag `poll_gap_max_ms`.

```
python3 tools/loop_survey.py                      # home detail chargertest lines tools settings connection diag, 10 s each
python3 tools/loop_survey.py --dwell 20 --pages home,detail
```

- Output goes to `runs/loop-survey/<git>/survey.json` and `report.md`.
- A mid-run reboot (uptime against the wall clock) fails the run, with a note.
- For a single reading: `python3 tools/k2_remote.py --json debug loop --page 1 [--reset]`.

The host numbers above are the prediction. In particular:

- Detail and Diag should show a PD service max near their pass max (≈ 45 ms); after E1 the paint part of it is
  ~1-2 ms, and what stays over 10 ms is the CC scan (13-24 ms, unattached only) and other blocking driver calls;
- Home should show passes over 100 ms on a repaint (open Home from another page), after E1 ~10-15 % longer;
- the shell's `lcd show` prints `service_us=1000` after E1 (`service_px` / `dma_service_px` before).

Two differences from the host:

- The device includes CPU work outside the stream calls, which the host model does not charge.
- The device numbers also include the remote's own passes.
