<!-- SPDX-License-Identifier: GPL-3.0-or-later
     Copyright (C) 2026 Yaraslau Furman -->
# Fast-charge protocols: what the firmware does and what was measured

Facts about the D+/D- and UFCS fast-charge protocols as this firmware uses them, plus wire captures taken from
the author's own chargers. The code is the reference for frame and timing constants (`include/k2/fastcharge/`);
this file records the decisions behind them and the evidence. Provenance tags: "measured" (own capture or
hardware run), "inferred" (reasoned from how real sinks behave), "reverse-engineered" (decoded from a wire log).

## 1. QC3 / QC3+ stepping loop (inferred, host-tested)

The pulse timing is in `include/k2/fastcharge/qc.hpp`. The loop around it counts steps and waits for the adapter
to finish before deciding again:

- After each batch VBUS must be stable (3 samples at least 30 ms apart within 50 mV, and no 15 mV movement for
  100 ms) before the next batch; 500 ms without movement also ends the wait. A fixed settle time is not used.
- QC3 sends full batches (4 pulses) while more than two pulses remain, then single pulses so each of the last
  steps is measured. QC3+ sends full batches (up to 50 pulses) far from the target, then half of what is left (at
  least 10), so the last ~200 mV come in one small batch.
- The aim is target - 100 mV (stop short, approach from below). The pass band is -300 / +150 mV around the target.
  Steps of 600 mV or more cannot land inside that band.
- The step is learned from the first probe batch (10 pulses) and then used for the expected pulse count; a level
  above (entry + expected pulses x step + one step, at least 100 mV) is `Overvoltage`, aborted.
- QC3+ pulses (100 in a row) that leave VBUS within 150 mV of the entry level mean the adapter does not do QC3+:
  "no QC3+ response", a skipped group, not a failure.
- After any QC session both lines are held at 0 V for 500 ms before the next detection starts. The Black Box
  records one `qc3-batch` mark per batch (VBUS and signed pulse count).

QC2.0 is 5/9/12 V and QC3.0 is 3.6-12 V. The Class A algorithms therefore have no 20 V rows (they fail on every
charger and are noise). QC3 ramps 5 -> 9 -> 12 -> 9 -> 5 V. Class B (20 V) sources are covered by two separate
algorithms, "QC2 Class B" (one 20 V row) and "QC3 Class B" (ramp 5 -> 12 -> 20 -> 12 -> 5 V). They are part of
the default selection (`kDefaultAlgorithms`); on a Class A charger they read as a neutral skip (`ClassAOnly`), and a
Class B row that finds the output stuck at 12 V ends early as the same skip (QC2: 3 s after the request with VBUS
inside 11.4-12.6 V the whole time; QC3: two settle batches that moved less than two pulses' worth while at 11.4 V or
more) instead of waiting out the 15 s / 12 s timers.

| Protocol | Class A | Class B | Step |
|---|---|---|---|
| QC2.0 | 5 / 9 / 12 V | + 20 V | fixed levels |
| QC3.0 | 3.6 - 12 V | 3.6 - 20 V | 200 mV |
| QC3+ | up to 12 V in this suite | up to 20 V in the spec | 20 mV |

Hardware example (2026-11-03): a no-name 30 W USB-A charger (card "NoName 30W USB-A", non-PD) is a real Class B
source. QC2 5/9/12/20 V all pass (20.16 V measured) and QC3 20 V plus the down-steps pass with the two Class B
algorithms enabled. It is the reference for what a real Class B source looks like.

## 2. SCP capability readout (2026-11-08)

The detect script reads the adapter's limit registers (0x90..0x97) in the unit's first SCP session.
`accp::scp_caps_decode` turns them into `accp::ScpCaps {min_mv, max_mv, min_ma, max_ma, valid}`; the decode is
fixed by the register dumps of a 20 V 6 A adapter, a 12 V adapter and an adapter that reads its limits but not
its type bits (Anker A2343, register 0x7E is not a gate there), all in `tests/test_accp.cpp`. `valid` needs a
plausible window (top level 3.3 V to 40 V, minimum not above maximum, at least 500 mA). The engine keeps the
result in `fc::Caps` (reset at the start of an SCP session, kept after it) and refuses any target above the
reported top level or current (`fc::Fail::OverLimit`): the K2 never asks an SCP adapter for more than it said it
can give, whatever the planner asked. `fctrig::caps()` hands the result to the charger test, which plans the SCP
rows from it (CHARGER_TEST_FLOW.md, "SCP and UFCS rows follow what the charger reported") and shows it as "SCP cap
20V 6A" in Results, on the card and in the card JSON. The extended-current register is not read on purpose: a NACK
there would abort the detect script on adapters without it. Not yet seen on a second adapter family.

A running SCP session needs ACCP traffic at least every 500 ms or the adapter drops back to 5 V; the engine polls
voltage and current every 100 ms.

## 3. OnePlus 15 + SUPERVOOC 120 W, passive D+/D- capture (2026-11-02)

Source: a 71,023-edge capture over 83.4 s (phone charging at 8.6 V / 4.55 A), decoded by `tools/vooc_edges.py`;
golden fixture `tests/fixtures/oneplus15_svooc_init.edges`. Passive only, nothing was driven.

- **Clock**: D+ idles high. The adapter pulls it low in short pulses, 25-40 us wide (histogram: 25 us 11.8k, 35 us
  11.4k, 30 us 3.6k), in groups of **9**, spaced 0.46 ms and 0.54 ms alternately (about 2 kHz). D- is the data
  line.
- **Bit sampling**: the bit latched by pulse k is the D- level just *before pulse k+1 falls* (the last bit: 300 us
  after the last pulse). A 1 us delay after the fall always reads the previous bit. D- never changes within 5 us of
  a fall; transitions are 28 us after the fall (adapter frame) or about 50 us after the rise (phone frame).
- **Frame**: 9 bits = an 8-bit command (adapter) or an 8-bit value plus 1 bit (phone), then 1 trailing bit that is
  0 in every command (3,081 of 3,090 pulse groups have 9 pulses; the others are the wake burst).
- **Pairs**: one adapter command, then 8.9-9.1 ms later the phone reply, using the same D+ clock. Each pair
  repeats about every 44 ms (0x82 poll), every 24-35 ms in the init script, every 165 ms in the stalled state. The
  "01" / "10" prefix seen by an earlier decoder was the D- idle level before pulse 1: 0 before an adapter frame,
  1 before a phone frame.
- **Wake burst**: before any frame, 40 clocks with both D+ and D- toggling together, 0.35 ms for the first then
  295-300 us half-periods; then 8-9 clocks at 515 and 523 ms; the first real command (0xA8) comes at 679 ms. Not
  data-like.

Commands seen: 0xA8 ident, 0x98 battery 1, 0x9E, 0x80, 0x94, 0x8A telemetry, 0x8E, 0x9A heartbeat, 0x82 current
limit, 0x84 voltage ramp, 0x86 battery 2, 0x88 cable R, 0x8C voltage step. 0x8E and 0x9E are status-like (reply
0x010).

Init script (identical three times, 1.3 s each: 0.68 s, 41.78 s, 61.57 s), command then reply word (9-bit, hex):
A8 > 00D; 98 > 073, 023; 9E, 80, 94, 98 > 010 each; 8A > 028; 98, 8E > 010; 9A > 030; 82 > 074, 00B; 84 > 071,
031; 86 > 06B, 03A; 88 > 05D, 042; 8A > 048, 052; 8C > 046, 066; 88 > 04D (accepted) or 00D (rejected). The real
phone sends data-bearing words for 98/86/82/84/88/8A, so the fixed reply bytes of the slave are a minimum that
satisfies the ident and keep-alive, not what a phone sends. The reply word is 9 bits wide: 0x00D is the ident
reply, 0x010 the keep-alive.

Steady state (0x82 poll every 44 ms): the reply is a slowly rising counter, 0x033 at 2 s, 0x03A at 5 s, 0x03D from
27 s (the 12 W stage), after the restart 0x035 at 62.6 s, 0x04F at 80 s (38 W stage). It looks like a battery or
state value; the occasional 0x07F is a sentinel (at 2.2, 3.7, 63.2, 64.7 s, in each ramp start).

Landmarks (first stage ~12 W, dip to ~2 W, ramp to ~38 W):
1. 0-39.69 s: first handshake accepted (88 > 04D at 1.42 s), 0x82 polling, replies 0x033 > 0x03D. Charging at ~12 W.
2. 39.69 s: the last two replies are 0x000 (phone silent), then D+ stops for 1.93 s. This is the drop to ~2 W
   (adapter reset).
3. 41.78-50.8 s: init script again; the final 0x88 is answered 0x00D instead of 0x04D (rejected), the adapter
   polls 0x88 every 165 ms and the phone answers 0x00D for 8.3 s.
4. 50.84-59.66 s: init again without the 0xA8 ident; the phone answers 0x88 with 0x000 (silence) for 8.3 s. Gap
   1.9 s.
5. 61.57 s: init a third time, 88 > 04D accepted, then 94 (silent), 84 x2, 86, 82 polling. Ramp from here; 0x82
   reply 0x035 > 0x04F by 80 s (38 W).

Marker commands: 0x88 and its reply word (04D accepted, 00D / 000 rejected) separate the 12 W stage from the dip
and ramp; the adapter drops D+ for about 2 s between attempts.

What an active engine needs (not done, needs an unplugged-phone test):
- 9 clocks per frame; the command is 8 bits + a 0, so the tester reply is a 9-bit word.
- Sample D- just before the next D+ fall (or 300 us after the last); shift out our D- bit about 50 us after the
  D+ rise of the same pulse.
- D+ is an input clock: pulse width 25-40 us, period alternating 0.46/0.54 ms, frame pairs 9 ms apart; the edge to
  trigger on is the D+ falling edge (which pin, PB0 or PA7, stays open).
- The reply comes 8.9-9.1 ms after the command, not immediately; D- is idle high between frames (a phone that is
  not answering leaves D- at 0 for the whole frame).
- Answer the 0x88 cable check with 0x04D to get the ramp; 0x00D rejects it.

### Phone-side slave on hardware (k2/vooc-slave, 2026-11-02, charger on the bottom port, top port empty)

- The adapter clocks D+ on **PA7** without any load (PB0 never sees a fall). It does not start every time: some
  attempts hear no frame ("needs load"), the next attempt after a 10 s rest usually does.
- The slave answers the init script A8, 98, 98, 9E, 80, 94, 98, 8A, 98, 8E, 9A, 82 with the golden words (about 11
  commands, 12 s). The session then ends: the next frame reads 0xFF (D- not driven) and the engine reports "no
  response". 0x88 (cable check, 0x04D) is never reached, VBUS stays at 5.2 V.
- Receive lessons: arm the frame start after 0.7 ms of D+ high (not 1.5 ms), listen in 15 ms slices (the next
  command comes 20-30 ms after a reply), and float D- while listening (the pull-down corrupted rising bits: 0x98
  read as 0x88, 0x9E as 0x9C). Floating D- idles high between frames and the adapter pulls it low about 10 us
  before the first fall.
- Open: why the adapter drops after the first 0x82 reply (0x074). The data words may be state-dependent (the
  capture is one accepted attempt), or the reply timing needs a closer match.
- Retired 2026-11-11: the engine no longer answers any adapter command (see "Detection by ident" below). The slave
  replies above are history; `vooc::tx_reply` remains only for the lab `vooc answer` op.

### OnePlus 15 + OnePlus 120 W, USB-A port, inline capture (2026-11-11)

Source: `runs/2026-11-11-op15-svooc/cap1.edges` (81,980 D+/D- edges over 95 s, 0 overruns, recorded passively with
`tools/sniff_stream.py`; the phone at ~85 % plugged in twice, the brick's own A-to-C cable, K2 inline: charger on the
bottom port, phone on the top plug), `power.tsv` (VBUS, current and line levels every ~0.4 s). Committed slices:
`tests/fixtures/op15_svooc_entry.edges` (0-17.5 s, entry and handshake) and `op15_svooc_replug.edges` (57.5-63.6 s,
steady state, unplug, second session). Decode with `python3 tools/vooc_edges.py file <edges> --summary` or
`--edges LO HI` (raw edges of a window). Times below are ms on the tool's axis (first edge = 0). Everything in this
subsection is observed unless tagged.

**Entry, first plug-in (who drives what):**
1. 0 ms: both lines high; D- falls at 0.89 ms, D+ at 7.34 ms. Then 14.69 s with both lines low (the phone not yet
   talking).
2. 14,699.36 ms: **wake burst**. D+ and D- rise and fall *together* (the two edges of each step 0.5-5 us apart),
   first half-period 246 us, then 293-301 us: 40 clocks (80 edges), ending with both lines low at 14,723.0 ms. The two
   lines moving as one is one driver seen through the brick's closed D+/D- short (the port is a DCP at idle:
   `[FC] dcp check ... result=2`). That the phone drives D+ is inferred: it is the only side that is active before the
   first command, and the K2 reproduces the burst by driving D+ alone (below).
3. 14,723.0-15,262.46 ms: both lines held low for 539.5 ms.
4. 15,262.462 ms: D- rises, D+ rises 5.3 us later. From here the two lines move independently: **the short is open**.
   This rise is pulse 1 of the first command, whose fall was the end of the burst (D+ held low), so an edge-based
   decoder has to start a frame on either D+ edge after a 2 ms quiet gap, not only on a fall.
5. That first frame is the ident `0xA8`, answered `0x000` (the phone silent). 165.6 ms later the adapter sends `0xA8`
   again and the phone answers `0x00D`.

The second plug-in repeats the shape: wake burst at 61,572.2 ms, first ident 561 ms later.

**Handshake** (session 1, ms from the first ident; command -> reply word; the names are the tool's guesses, see
`SVOOC_CMDS`): 0 A8 -> 000; 165.6 A8 -> 00D; 209.0 98 -> 073; 233.1 98 -> 023; 257.3 9E -> 010; 281.3 80 -> 010;
305.4 94 -> 010; 329.5 98 -> 010; 373.9 8A -> 028; 397.8 98 -> 010; 422.0 8E -> 010; 466.3 9A -> 030; 500.9 82 -> 074;
535.5 82 -> 00B; 570.2 84 -> 071; 604.8 84 -> 031; 639.5 86 -> 06B; 674.1 86 -> 03A; 708.8 88 -> 05D; 743.4 88 -> 042;
778.1 8A -> 048; 812.7 8A -> 052; 847.4 8C -> 046; 882.1 8C -> 066; 916.8 88 -> 04D; 960.1 94 -> 000. Then `0x84` about
ten more times and the steady `0x82` poll.

**Steady state:** only `0x82`, every 44.2 ms (median), 943 times in session 1 and 715 in session 2; the reply word rises
`0x04C` -> `0x06A` (session 1) and `0x055` -> `0x06B` (session 2), with an isolated `0x07F` early in each ramp. VBUS creeps
8.95 -> 9.08 V at a constant 3.60 A. Inferred, unproven: the reply is a battery-voltage reading and the adapter regulates
VBUS to about twice it. VBUS/current through the entry: 5.34 V 0.88 A -> 8.65 V 2.0 A -> 9.0 V 3.6 A in a few seconds.

**Guessed, not proven:** every command name (`ident` aside, which is the first command of every session), the meaning
of every reply word, the `0x07F` sentinel, and which ident (0xA8, 0xAC, 0xA6 in `vooc::is_ident`) belongs to which
adapter generation. 0xA8 opens the session on this 120 W brick, so one ident does not tell VOOC, SVOOC and SVOOC 65 W
apart.

### Detection by ident, no trigger (k2/vooc-detect, 2026-11-11)

Rule (operator, 2026-11-11): the K2 never answers a VOOC adapter. Any reply can lead a SuperVOOC adapter towards its
high-current output (11 V at up to 11 A), so the charger test detects and stops.

- Wake: odd tries play the phone's entry burst (80 steps of 300 us, D+ 3.3 V / 0 V, D- Hi-Z so the closed short carries
  it, ending low; reverse-engineered from the capture above); even tries the older 13-period train. 3 tries, 1.5 s
  listen after each (the ident came 0.54 s after the burst), 3 s rest between them.
- Listen: frames start on either D+ edge after D+ held one level for 2 ms (`vooc::FrameStart`). An all-ones word
  (0x1FF, command 0xFF) is the idle-high line, nothing heard (`vooc::heard`).
- The first ident ends the session at once as `Fail::Detected` and releases the lines (both 0 V for 500 ms, then the
  passive idle). Any other command is counted, never answered. VBUS above 5.5 V aborts (`Overvoltage`) and releases.
  The engine refuses VOOC inline even with `allow_inline` and aborts if inline appears mid-session.
- Outcomes: ident -> "detected"; only idle or no frames after 3 wakes -> `NeedsLoad` ("needs load", the old wording
  for "never clocked"); frames but no ident -> `NoResponse`. Rules for the charger-test rows:
  `docs/CHARGER_TEST_FLOW.md`.
- Not done: telling VOOC from SVOOC from SVOOC 65 W (needs answers), any voltage step.

### Inline monitor, passive (k2/vooc-detect, 2026-11-11)

While the K2 is inline (a far sink draws through it, `link::far_sink_draws`) and owns D+/D- for nothing else, the lines
float and the D+/D- edge ISR (`dpsniff`) feeds every edge to `vooc::Decoder` (`include/k2/fastcharge/vooc_decode.hpp`). It is
pure code over edge words and shares the tool's framing: frames split by a 2 ms gap, a first pulse over 1 ms timed
from its rise, command then reply within 12 ms, a new session after 1 s of silence, the handshake up to the first
pause over 60 ms, the steady `0x82` period from gaps under 200 ms. A host test (`tests/test_vooc_decode_isolation.py`)
proves the decoder header reaches no pin, register or driver.

- Cost: ~1,030 edges/s through the ISR in steady state (bursts of ~6.7 k edges/s for ~24 ms in the wake). RAM: the
  decoder is 224 B (its summary 152 B), no edge buffer.
- Summary: sessions, wake bursts, partial frames, edges, frames, pairs, handshake pairs, the ident and its reply,
  per-command counts with the last reply of each (16 commands, then `other`), and for `0x82` the last / min / max reply
  plus the last and mean period.
- Readout: `k2_remote.py --json fc vooc [--on|--off]` (FC op 4, a sub-op of command 0x32), CDC shell
  `fc vooc [on|off]`, `[VOOC] session N start` / `end` log lines, the Home protocol row `SVOOC 9.0V3.6A` (the measured
  VBUS and IBUS, not a rating, so no limit bar) and a raw `VOOC 0x82 0x06B raw` row on the Connection page (Sink body,
  with or without a PD contract). Reply words are printed as hex with no unit: their meaning is a guess.
- Stalled session (found on hardware 2026-11-11, OP15 at high battery): the phone keeps polling and the brick stops
  answering one command, which the summary used to show only as a growing pair count. `Summary::stalled()` is true
  when the trailing run of pairs is one command with a silent reply (`kNoReply` or the word 0x000) at least
  `kStallPolls` (16) long; any answered pair clears it and a session start resets it. The first silent 0xA8 ident and the
  two silent 0x94 pairs of the entry stay far below the threshold. Readout: FC op 4 page 0 is 57 bytes (flags bit 5, then
  `stall_cmd(1) stall_n(2)`), `fc vooc` prints `stalled on 0x88, 277 silent replies` in the tool and the shell, and the
  session-end `[VOOC]` log gains `session N end: stalled on 0x88, 277 silent replies`. Facts only: no cause is guessed.
  Fixture: `tests/fixtures/op15_svooc_stalled.edges` holds a prefix of the real stall (93 of the 277 silent polls).
- A PD contract does not stop the monitor (2026-11-11, a phone on the USB-C port holds a 5 V contract on CC and runs
  SuperVOOC on D+/D- at once). The gate is the far sink alone; a live proven session wins the Home protocol row over the
  PD contract text and the PD row returns when the session ends. The passive label (classifier) still says PD while no
  session is live. A USB data link under a contract trips the edge-storm guard like any other (test "PD contract + data
  traffic -> paused, no decode").
- Edge-storm guard (`include/k2/fastcharge/edge_guard.hpp`, `tests/test_edge_guard.cpp`). Inline is exactly where a
  USB 2.0 data link runs through the K2, and a full-speed link toggles D+/D- faster than the ISR can return. The ISR
  counts interrupts in two tumbling windows in O(1) (two subtractions, two compares, no division) and masks its own two
  EXTI lines the moment either is exceeded:
  - 1 ms, more than 32 interrupts. The capture peaks at 9 in any 1 ms (the wake burst): 3.5x margin. Worst case
    before the mask: 65 interrupts (32 in the running window plus 33 in the next).
  - 2^28 cycles (1,118 ms), more than 4,096 interrupts. The capture peaks at 1,340 in any 1 s: 3x margin. This window
    catches lighter traffic that stays under 32 per ms (frame-start packets every 1 ms).
  The main loop then pauses the monitor (`[VOOC] monitor paused: data traffic (N edges in M ms), retry in W ms`). The
  lines stay floating and nothing is decoded. After 1 s, then 5 s, then every 30 s it unmasks the lines for a 2 s probe.
  A quiet probe runs the monitor again, and 60 s without a storm resets the steps to 1 s. The Black Box `sniff arm` /
  `sniff stream` share the ISR and are guarded too: a storm ends them with end reason `storm` (6) and
  `[SNIFF] stopped: data traffic`. Readout: flags bit 4 `paused`, then storms, the last storm's interrupts and ms, and
  the wait left (`fc vooc`, shell `vooc guard ...`).
- While the monitor owns the lines (Sniff floating inputs), the D+/D- classifier reads `USB DATA` whatever the
  levels (PD still wins), and the window starts fresh when the monitor lets go.
- The monitor is on by default (a RAM switch, not saved). Not yet run on hardware.

## 4. UFCS

Wire: USART2 on PA2 (TX, wired to D-) and PA3 (RX, wired to D+). There is no pin swap; the pins are fixed in
hardware. Code: `include/k2/fastcharge/ufcs.hpp` (codec and timers), `fc::Proto::Ufcs` in
`src/fastcharge/fc_engine.cpp`, glue in `src/fastcharge/fctrig.cpp`.

**Handshake.** The sink sends four pulses on D- (2 ms, 8 ms, 2 ms, 8 ms; at most 3 attempts) and watches D+ pulled
high; both sides then switch D+/D- to UART mode within 1 ms. The sink must send its first Ping within 100 ms; the
source waits 110..120 ms and otherwise resets the sink. UART: 115200 baud default (also 57600, 38400); the stop
bits are a tuner field. Each packet starts with a 0xAA training byte, which is also the frame sync. Reset
signal: the data line held low for at least 2 ms; the engine's `reset_bus()` does this.

**Frame.** `AA`, header (16 bits big-endian), command, data frames add a length byte and the payload, then CRC-8
(poly 0x29, init 0, MSB first) over every byte after the sync. Header: receiver address [15:13] (1 = source, 2 =
sink, 3 = cable), message number [12:9], protocol version [8:3] (V1.0 = 1), message type [2:0] (0 control, 1
data). Our frames are addressed to the source (1); the source's are addressed to us (2).

**Messages.** Control: 0 Ping, 1 ACK, 2 NCK, 3 Accept, 4 Soft_Reset, 5 Power_Ready, 6 Get_Output_Capabilities,
7..0xE source/sink/cable/device/error info and the cable-detect trio, 0xF Exit_UFCS_Mode, 0x10
Get_Sink_Info_Extended. Data: 1 Output_Capabilities, 2 Request, 3..7 information messages, 8 Config_Watchdog, 9
Refuse, 0xA/0xB Verify_Request/Response, 0xC Power_Change, 0xD Sink_Information_Extended. The keep-alive is Ping
(0) and Accept is 3.

**Acknowledgement.** Every message except ACK and NCK is answered with an ACK that carries the same message
number, after at most 10 ms. An NCK (bad CRC) makes the sender retransmit at once; no ACK within 10 ms makes it
retransmit too, up to 3 times, then it sends Soft_Reset, which restarts both message counters at 0 and does not
leave UFCS mode. A Ping may be retransmitted without the retry limit. The message number counts 0..15 and returns
to 0 after the handshake, a Soft_Reset and a hardware reset. A retransmission keeps its message number.

**Exchange.** Sink Ping, source ACK; sink Get_Output_Capabilities, source ACK, source Output_Capabilities within
50 ms, sink ACK; sink Request, source ACK, source Accept, sink ACK, source Power_Ready within 550 ms of the
Accept, sink ACK. The sink answers a source message within 40 ms, with 2 ms between its ACK and its own message.
When Power_Ready does not come in time the sink sends the hardware reset. The source's watchdog defaults to 1 s:
any message from the sink restarts it, so the sink pings every 100 ms while it holds a level. Exit_UFCS_Mode
returns the source to its initial state (5 V). A source that does not know a sink request answers Refuse
(reasons 1 unknown, 2 unsupported, 3 busy, 4 out of range, 5 other); a sink answers the information requests it
does not implement (Get_Sink_Info, Get_Device_Info, Detect_Cable_Info, Verify_Request ...) the same way.

**Output_Capabilities.** 1..7 output modes of 8 bytes, big-endian: mode number [63:60], current step [59:57]
(000b..100b = 10..50 mA), voltage step [56] (0 = 10 mV, 1 = 20 mV), maximum voltage [55:40] and minimum voltage
[39:24] in 10 mV, maximum current [23:8] and minimum current [7:0] in 10 mA. **Request**: mode number [63:60],
requested voltage [31:16] in 10 mV, requested current [15:0] in 10 mA; the values are multiples of the mode's
steps and inside its ranges. A sink that does not know the cable assumes the worst case: at most 2 A
(`kNoCableMa`).

**Decoded from a Baseus FH22 charger (wire log of 2026-11-07).** Output_Capabilities payload
`18 02 26 01 54 01 2C 64 | 28 04 4C 02 26 01 90 64`:
- mode 1: 3.40..5.50 V, 1.00..3.00 A, steps 50 mA and 10 mV;
- mode 2: 5.50..11.00 V, 1.00..4.00 A, steps 50 mA and 10 mV (the 5.50 V boundary belongs to mode 1; a level
  above 11 V is not offered, so a 12 V request ends as `NoBand`, "level not offered").

Ping `AA 20 08 00 63` was ACKed `AA 40 08 01 DF`; Get_Output_Capabilities `AA 22 08 06 D6` was ACKed
`AA 42 08 01 9C`; our ACK of the Output_Capabilities (message number 0) is `AA 20 08 01 4A`; our Request
`AA 24 09 02 08 ...` was ACKed `AA 44 08 01 59` and answered with Accept `AA 42 08 03 CE`. Every byte is
reproduced by `tests/test_ufcs_frame.cpp`.

**Hardware status.** Verified on a Baseus FH22 charger (docs/CHARGER_QUIRKS.md): handshake, framing,
Output_Capabilities, Request, Accept, Power_Ready and the keep-alive, with 5, 9, 11 V and back to 5 V passing in
one session; and on a CUKTECH 10 charger's C1 up to 20 V at 3 A (60 W, the Request current is set per row).

**Timers in the engine.** The protocol limits are kept as constants; the engine waits the limit plus polling and
UART slack so a healthy source is never reset early: ACK 20 ms (limit 10), answer after an ACK 60 ms (50),
Power_Ready 600 ms (550), Ping every 100 ms (watchdog 1 s), retry count 3. A missing ACK that survives the retries
escalates Soft_Reset, then the hardware reset (the bus reset) and the failure `NoResponse` (before the Accept) or
`SourceLost` (after it). A missing Power_Ready is reported as `NoBand` (the target level was not reached);
`NoVoltage` stays the "handshake ok, no level" code of the SuperVOOC rows.

**Safety.** Nothing above 5 V without `confirmed`, the VBUS ceiling, the cable and inline guards, and the
overvoltage abort apply to every Request; after the Accept the VBUS band is judged only after Power_Ready.

**Levels from Output_Capabilities (2026-11-08).** The engine keeps the modes in `fc::Caps::ufcs` (`ufcs_n`
entries, reset at the start of a UFCS session). The charger test plans its UFCS levels from them (inferred): per
mode, the bottom, the middle (rounded down to 100 mV) and the top of the window, 1 A each (the mode's smallest
current when that is higher, never above its maximum), within the monitor ceiling and the cable limit. Modes that
reach above the ceiling (the 30 V band of a 100 W source) are asked up to the ceiling only. Before the first row
ran, and for a source that sent no usable modes, the fixed levels 5 / 9 / 11 V and back to 5 V apply.

**Open points.** Not yet seen on a source: a Refuse, an NCK, the source's own Ping, Get_Sink_Info and Get_Device_Info. Engine rule (TOPADRE 65 W, 2026-11-08,
Soft_Reset between our Accept and its Power_Ready): a Soft_Reset from the source in any live phase (Request, Power
wait, Keep) is ACKed, numbering restarts at 0 and the opening runs again (opener, Get_Output_Capabilities, the
Request for the current target); a second Soft_Reset before the next Power_Ready ends the attempt (`SourceLost`
once the Request was accepted, else `NoResponse`), never the 600 ms Power_Ready timer. The cable information
(Cable_Information, Detect_Cable_Info) is not read, so the current stays at 2 A.

### Bring-up diagnostics (inferred; firmware logging, no behaviour change)

A UFCS attempt prints these K2LOG lines (all carry `t` = millis; hex has no separators):

- `[UFCS] hs t pre rise=a,b fall=c,d fin class ok`: microseconds from each D- edge to the D+ edge it should cause,
  `pre` = D+ level before D- is touched, `class` = `mirror` (every edge within 100 us: a BC1.2 DCP short, not a
  UFCS source), `delayed` (a source that thinks) or `none`. Pass/fail is the D+ level test; the class is only a
  label.
- `[UFCS] open`, then `[UART2] tx|rx t n o HEX`: every frame sent and every raw chunk received (16 bytes per line,
  `o` = offset in the chunk), including bytes the parser later drops.
- `[UFCS] drop t why used have HEX`: bytes `ufcs_take` discarded and why (`nosync`, `type`, `length`, `crc`,
  `overflow`); `[UFCS] frame ... to=N`: a decoded frame and its receiver address (only frames to the sink, 2, are
  served: a frame to the source (1) or a cable (3) is our own transmission looping back through a DCP short, or
  someone else's, and is neither ACKed nor counted as a sign of life; when the opener's listen window ends with
  such an echo of our last frame and no frame to the sink at all, the session fails as `no handshake` and logs
  `[UFCS] echo: ...`; Chicony W20-045N1B, 2026-11-07, where the released D+ rose 0.64 ms after D- and passed the
  handshake check falsely); `[UFCS] retry`: a re-send with the pending bytes.
- `[UART2] err t framing noise overrun parity`: USART error flags counted since claim (a lower bound).

The ping listen window is 500 ms (tuner `listen`): 20 ms retransmits for the first three tries, then the
keep-alive pace.

**Tuner and probe (RAM only, no flashing between variants).** Every frame and timing choice stays a runtime
setting in `k2::ufcs::Tuner` (`include/k2/fastcharge/ufcs_tune.hpp`): header receiver address, the header version
[8:3], control and data type bits, opener and keep-alive command codes, retransmit interval and listen window,
whether the CRC covers the sync byte, CRC init, polynomial and bit order, stop bits (1 or 2), baud (1200..230400),
the delay between the handshake and the first frame (capped at 200 ms because it blocks the main loop), and the
D-/D+ drive during the UART phase. The defaults equal the shipped behaviour and reset at boot. `ufcs probe` (shell,
`k2_remote ufcs probe`) runs only the handshake and the opener exchange at 5 V and records handshake timings,
every TX frame, every raw RX byte with a timestamp and the UART error counts; `tools/ufcs_sweep.py` loops it over
a variant list.

## 5. VIVO FlashCharge

Frames are `0xAA, n, data[n], sum` over USART2 with both lines high-Z (no D+/D- handshake), 115200 baud, 8N2;
`sum` = (0x56 + all earlier bytes) & 0xFF. The charger announces itself with `AA 01 55 <sum>`; the K2 answers
`AA 03 08 55 00 60` and sends its frame sequence, each frame after the previous one was echoed (reply
`AA 02 08 <echo of the command> sum`) or after about 50 ms. The 5 V frame carries 2000 mA and 5000 mV in
`command 03`; later voltage steps use the same command with the voltage halfword in mV. Code and frame table:
`include/k2/fastcharge/vivo.hpp` (host-tested, `tests/test_vivo_frame.cpp`, `tests/test_fc_vivo.cpp`).

Release is the 5 V frame three times, then the UART is closed and the bus reset. Same safety contract as UFCS.
Not verified on a VIVO charger. Assumptions, stated in the engine: the voltage frame jumps from 5 V to 7 V and
ramps in 100 mV steps every 100 ms (a direct jump is not proven safe); the target is limited to 5 V or 7..11 V; 12
V and above are refused.
