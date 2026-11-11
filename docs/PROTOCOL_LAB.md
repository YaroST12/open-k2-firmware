# Protocol lab

One command family on the CDC debug shell (`/dev/ttyACM0`, USB shell enabled in Tools -> USB link) to
drive every charging protocol by hand or from a script. Grammar: `include/k2/core/lab.hpp`, `src/core/lab.cpp`
(data-driven table, host-tested in `tests/test_lab.cpp`). Hardware glue: `src/core/shell_hw.cpp`. Client:
`tools/proto_lab.py`.

Status: built and host-tested, NOT yet verified on hardware.

## Rules

- Anything that can raise VBUS needs a trailing `confirm` or `--confirm`. Without it the shell answers
  `err: this can raise VBUS: ...` and does nothing. The HID remote follows the same rule: `k2_remote action fc-scan`
  (the QC scan) is answered `need-confirm` unless `--confirm` rides along; the other actions never raise VBUS. `k2_remote pd attach [mv [ma]]` follows it too: no target means 5 V, a target above
  5 V needs `--confirm` (the `pd request` flag), and inline or during a charger test it is refused with no Rd and nothing sent.
- Refused while a charger-test run, the sniff or a scan is active. Line-driving commands also refuse with
  a sink attached or without a power monitor.
- Lab commands (not `fc trigger`) abort at 12 V VBUS and drop the lines; the INA ceiling applies as for `dp`/`dm`.
- A session drop (port closed, USB lost) sends `fc reset` and releases D+/D- to HiZ.
- The shell (and so `tools/proto_lab.py`) never arms the HID dead-man lease: only a HID/HTTP command accepted by the
  dispatcher does (`pd request` / `pd attach` / `fc trigger` above 5 V, `fc scan`; see docs/ARCHITECTURE.md, LEASE 0x4C).
  A lab session that also holds a voltage armed through `k2_remote.py` must keep talking over HID (`--hold`) or the K2
  stops after the lease (30 s by default); closing the shell port releases only what the shell started and neither
  stops the lease nor a charger test.
- Commands run in the main loop: long ones (dump, `--watch`, `--hold-ms`) block the UI meanwhile.
- Numbers are decimal or `0x` hex. Voltages are mV (max 36000), currents mA.

## Output

One line per result: `name key=val ...`, or with `--json` `{"cmd":"name","key":val}`. Then `ok`, or
`err: reason`. `--log raw|decoded` is accepted on every command (raw bytes are always printed as `rx=` hex).

## Options

| Option | Range | Used by |
|---|---|---|
| `--retries N` | 1..10 (default 3) | accp * |
| `--timeout-ms N` | 1..60000 (default 1000) | vooc listen/answer |
| `--pulse-us N`, `--gap-us N` | 20..20000 | qc3/qc3+ up/down |
| `--hold-ms N` | 1..10000 | dpdm (hold, then release) |
| `--watch N` | 10..10000 | vbus |
| `--hold` / `--release` | flag | accp, qc up/down, dpdm, vooc: keep the lines driven (default) / release to HiZ afterwards |
| `--confirm` | flag | any raising command |
| `--json` | flag | all |
| `--hs-ms`, `--frame-gap-us`, `--sop` | accepted by the grammar, refused at run time: no engine timing hooks, no PD injection |

An option a command does not use is refused, never silently ignored. Engine-driven requests (qc2,
qc3 set, afc, fcp, scp, vooc wake, ufcs request, vivo request) keep the suite's timing; the mA of
afc/ufcs/vivo is reported as `ma_applied=0` (fixed by the engine); scp takes the mA.

## Commands

| Command | Does | Example |
|---|---|---|
| `accp ping` | ACCP master/slave ping | `accp ping --retries 5` |
| `accp read <reg> [n]` | read 1..64 registers, one line each (reg, ack, val, rx, tries, us) | `accp read 0x80 4` |
| `accp write <reg> <val> confirm` | write a register, ACK checked | `accp write 0xA0 0xC0 confirm` |
| `accp dump <from> <to>` | read a range | `accp dump 0x00 0xFF --json` |
| `qc2 <mv> [confirm]` | QC2 engine to 5/9/12 V | `qc2 9000 confirm` |
| `qc3 set <mv>` | QC3 engine | `qc3 set 9000 confirm` |
| `qc3 up\|down [n]` | n pulses on the continuous baseline (set with `dpdm 0.6 3.3 confirm` first) | `qc3 up 3 --pulse-us 2250 --gap-us 2250 confirm` |
| `qc3+ set\|up\|down` | QC3+ (default 40/50 us) | `qc3+ up 20 confirm` |
| `afc <mv> [ma]` | AFC engine | `afc 9000 1670 confirm` |
| `fcp <mv>` | Huawei FCP engine | `fcp 9000 confirm` |
| `scp <mv> <ma>` | Huawei SCP engine with the current | `scp 9000 3000 confirm` |
| `vooc wake` | VOOC wake train via the engine | `vooc wake confirm` |
| `vooc listen` | receive one command frame (`--timeout-ms`) | `vooc listen --timeout-ms 500` |
| `vooc answer <cmd> <reply>` | receive a frame, reply when its command matches | `vooc answer 0xA8 0x0C confirm` |
| `vooc sniff` | starts the edge stream | `vooc sniff` |
| `ufcs request <mv> <ma>`, `vivo request <mv> <ma>` | engine requests | `ufcs request 9000 2000 confirm` |
| `dpdm <dp> <dm>` | levels `hiz 0 0.6 3.3`; `--hold-ms` then release | `dpdm 0.6 3.3 confirm` |
| `vbus [--watch ms]` | mv, ma, dp, dm samples (every 100 ms when watching) | `vbus --watch 2000` |
| `pd caps` | the cached source capabilities | `pd caps` |
| `pd request <pos> [mv] [ma]`, `pd pps <mv> <ma>`, `pd avs <mv> <ma>`, `pd epr` | sink engine request (it owns GoodCRC, retries, timeouts); asynchronous, read the outcome with `state` / `pd caps` | `pd pps 9000 2000 confirm` |
| `pd get source-cap`, `pd vdm <word>` | re-read source caps; run the Identify sequence (Discover Identity, SVIDs, Modes) | `pd get source-cap` |
| `pd reread [mask]` | the after-contract conversation (Get_Source_Cap_Extended, Discover Identity, Discover SVIDs, Get_Source_Cap), one try each; mask 1 caps-ext, 2 identity, 4 svids, 8 get-cap, 16 second get-cap (default 15). PD mode only, refused with no CC, inline, in a charger test or while the sink is busy. Result: `[REREAD]` log line, Black Box mark, remote `pd-reread` | `pd reread 8` |
| `ufcs ping\|caps\|raw <hex>` | USART2 frames (CRC-8), reply decoded; `--timeout-ms` (default 300) | `ufcs caps` |
| `pd hardreset` | PD hard reset (refused with a sink) | `pd hardreset` |
| `ufcs show`, `ufcs set <field> <value>`, `ufcs reset` | the UFCS frame/timing tuner (RAM only, defaults at boot = the published standard, confirmed on a real source); no confirm, it never touches the bus. Fields: `addr version ctl data opener keepalive retry listen crcsync crcinit crcpoly crclsb stop baud delay dm dp` (`show` prints value, default and range). Refused while a probe, trigger, scan, sniff or charger test runs | `ufcs set version 2` |
| `ufcs probe`, `ufcs result` | handshake plus the opener exchange at 5 V with the current tuner; never raises VBUS and never sends a Request. `probe` starts it (refused inline, off 5 V, without a monitor, while the previous bus reset hold lasts); `result` prints `ufcs.result` (verdict, class, counts, UART errors), `ufcs.hs` (edge timings us, VBUS) and one `ufcs.ev` line per TX frame / RX chunk with its ms since the first frame | `ufcs probe` |
| `lcd show`, `lcd spi <4\|8>` | the LCD SPI3 clock divider: 8 = 15 MHz (the default and the only verified value), 4 = 30 MHz. RAM only: never persisted, back to 8 at every boot. No confirm and no VBUS effect, so it also works during a charger test. `spi` reprograms SPI3 (waits for the wire to go idle, SPE off while BR changes), moves the stream service intervals with the clock (polled 2048 px at /8, 4096 px at /4 = the same ~2.2 ms of wire time; DMA wait 468 / 937 px), then forces a full redraw. Refused for any other divider and while a screenshot or stream is open. Both print `div mhz service_px dma_service_px dma transfers stalls paint_px paint_ms fs_ms blocked_ms`; `k2_remote.py --json debug lcd` carries the same `spi_div` and paint totals, and `blocked_ms` (u16: the longest time the charger-test page sat blocked from painting in the current or last run; absent on an older firmware). A clock the panel wiring cannot carry shows as garbage after the redraw: `lcd spi 8` restores it | `lcd spi 4` |

### UFCS sweep (tools/ufcs_sweep.py)

The same tuner and probe are on the HID remote (`k2_remote.py ufcs status|show|get|set|reset|probe|stop|result`,
command 0x52). `tools/ufcs_sweep.py` runs the probe over a list of tuner variants against a real source: per
variant it resets the tuner, applies `--set` baseline values and the variant, probes, reads the blob, then waits
for the source's bus reset hold and for VBUS to be back in 4.5..5.5 V before the next. It stops on a reboot,
an inline / no-monitor refusal or VBUS leaving the window, and always leaves the tuner at its defaults.
Output: `runs/ufcs-sweep/<timestamp>/table.txt` and `results.jsonl` (full decoded result per variant). A variant is
marked `<<< RX` when the source sent anything beyond the single 0x00 framing-error break the K2's own release makes.

- Default list (48, likeliest mixes first): `[8:3]` field 1,2,3,0 x opener 6,1,0 x stop bits 2,1 x CRC over sync no,yes.
- `--vary NAME=v1,v2` (repeatable) replaces the axes, `--axes field,opener` keeps some, `--set delay=50` fixes a value, `--limit N`, `--dry-run` lists the variants without touching the device.
- Only one device tool at a time (uses `/tmp/k2-device.lock`); a charger must be on the bottom port at 5 V, nothing inline.
- The K2 cannot swap TX and RX between D- and D+ (USART2 has fixed pins), so that variant is not in the list.

Parsed but answered `err: not implemented in this firmware`: `pd softreset|raw`, `vivo raw`, `cc rp|rd`
(no raw PD transmit path or CC pull control in the FUSB302 driver yet), and `--sop`. (The legacy `cc open`, `pd silent|reset|rescan` are unchanged.)

### PD Stress (not a lab command)

Renegotiation stress with a score is its own tool, not a lab sequence: `pdstress start ... confirm | status | result | stop` on the shell,
`k2_remote.py pd-stress ...` and `tools/pd_stress.py run|rescore|report` on the host. It stays inside the PD protocol (legal probes only) and
never adds load. See [`PD_STRESS.md`](PD_STRESS.md).

## Scripts (tools/proto_lab.py)

`proto_lab.py list`; `proto_lab.py script <name> [--mv --ma]`; `proto_lab.py run "<cmd>" ...`.
Scripts: accp-dump, scp-sweep, fcp-steps, qc3-steps, qc2, afc, vooc, ufcs, vivo, pd. Add one to the
`SCRIPTS` dict. Each run writes `build/lab/<name>-<time>.log` (TX/RX lines and a VBUS/IBUS sample after
each command) and ends with `fc reset`, `dp hiz`, `dm hiz`.
