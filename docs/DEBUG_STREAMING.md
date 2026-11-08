# Debug streaming

Goal: the coordinator reads one recorded log of a hardware run (charger test, PD session, cable check) instead of
pulling rings off the device afterwards. This page has two parts: what the device holds and how it was read before
(the inventory), and the one live stream that now carries the debugging-relevant sources, with how to record and read it.

Short version:

```
python3 tools/k2_stream.py record --out runs/ct-NAME --until-run-end     # terminal 1, CDC port, no device lock
python3 tools/ct_start.py --wait 560                                      # terminal 2, HID, holds the device lock
python3 tools/k2_stream.py view runs/ct-NAME --kind run,gap               # afterwards
```

The stream needs the debug shell (CDC), which exists in the HID and HID+Drive modes (`docs/ARCHITECTURE.md`, "Debug
shell"). Left port, C-to-A cable. The CDC port takes no device lock (`/tmp/k2-device.lock` guards HID only), so the
recorder runs next to `ct_start.py`, `k2_remote.py` and `pd_trace.py`.

## 1. Inventory: what the device holds and how it was read

"Live under ct_start" asks: can a host read it while `ct_start.py` holds the HID lock for the whole run (minutes)?
Everything read through `k2_remote.py` / `pd_trace.py live|dump` is HID and takes the lock, so it waits behind the run
or has to be read after it. "Stream" is the kind that now carries it.

| Source | Size / change rate | Read today | What is lost | Live under ct_start | Stream |
|---|---|---|---|---|---|
| K2LOG text lines (HID report, `usb::log*`) | 64 B reports, 16-slot queue, hundreds of lines per run | `k2_flasher.py monitor` or a HID reader (needs the HID endpoint), `k2_remote log` | Queue full drops the oldest (`[USB] drop=`); the monitor needs the lock | No (HID) | `log` (binary form, below) |
| Text log ring (`LogRing<2048>`, LOG.TXT 3072 B on the drive) | 2 KB, about 30 lines | USB drive file, `store save-log` | Wraps in seconds on a busy run; gone on reset | No (the drive link needs a replug) | not streamed (section 5) |
| Binary K2LOG ring (`BinLog<1024>`, QR "LOG" record) | 1 KB, about 150 entries of 5-12 B | QR share on the device, `k2_qr.py` | Whole oldest entries dropped (counted); gone on reset | No (QR is photographed) | `log` |
| Black Box live ring (`pdtrace`, 8192 B) | PD messages, events, VBUS samples; a few KB/s on a chatty source | `pd_trace.py live` / `dump` (cmd 0x70, HID) | Wraps in seconds; counted since the stream work (`records_dropped`) | No (HID) | `trace` |
| Black Box captures (3 slots x 2048 B, 32 LZ4 files in /bb) | Trigger-frozen windows | `pd_trace.py captures` / `capture N` | Slots evicted oldest first; files survive power-off | After the run only | batch only (`k2_qr.py`, `pd_trace.py`) |
| Transition capture (`transcap`, 1920 samples at 1 kHz) | 1.92 s ring, trigger plus post window | `pd_trace.py transition list|dump` | Overwritten by the next trigger | After the run only | batch only |
| Boot trace and bootinfo (50 ms for 3 s, backup registers) | 60 samples, once per boot | `k2_remote bootinfo`, `[BOOT]` lines, boot QR | Only the last seven samples survive a reset | Needs HID | `Hello` + boot `Text` header |
| Heartbeat (`[SYS] Heartbeat`, every 2 s) | One line per 2 s | The log monitor | With the log | No (HID) | `Beat` (1 s, with counters) |
| Power monitor and ADC (VBUS, IBUS, CC1, CC2, D+, D-) | INA226 about 4.5 ms per value, ADC sweep 100 per average; read on demand | `k2_remote state`, `adc-raw`, `ripple` (HID) | Nothing is stored; only the instant of the call exists | No (HID) | `power` |
| Charger-test Runner (state, rows, verdicts, reasons) | Few events per row, about 2-12 rows per unit | `k2_remote charger-test status` / `results` polled by `ct_start.py`; the `/jr` journal after the run | A poll sees only the state at that moment, a fast row is missed; journal has the outcome, not the timing | Only through ct_start's own poll | `run` |
| Charger journal (`/jr`, littlefs) | Records per row/phase, survives reset | `k2_remote`, drive mode | Written for resume, not for timing | After the run | not streamed |
| Report cards (`/cards`, littlefs) | One card per charger | `k2_remote cards show` | Overwritten by the newest run of that charger | After the run | not streamed |
| Fast-charge engine and D+/D- sniffer (`dpsniff`, `edges`, 4096 edge words) | Edges at up to tens of kHz while a protocol runs | `sniff stream` (CDC, `tools/sniff_stream.py`), `k2_remote fc status` | Buffer fills at 4096 words; the sniff stream has its own framing (sync 0xA5) | Yes (CDC) | separate protocol |
| E-marker spoof (`emk_spoof`: counters, `unmatched()`) | Counters since `arm()` | `k2_remote spoof`, `unmatched` | Only the counts; the unmatched message itself is in the Black Box | No (HID) | `trace` (the messages), counters not streamed |
| Link and inline state (`link_guard`, `pd_link`) | Changes on plug events | `k2_remote state` | Instant only | No (HID) | `trace` events, `power` (VBUS/CC) |
| Session record (backup registers DR22..DR37) | 16 halfwords | `[SESS]` boot line | Survives reset, one record | n/a | `Text` boot header |

Rows with "No (HID)" were the reason a charger run could not be watched: the run itself kept the lock. The CDC port had
exactly two readers (`sniff stream`, `trace stream`); the stream puts the rest on it.

## 2. The stream

One framing, the one `trace stream` introduced: `0xA6, kind, len, payload, crc16` (CRC-16/CCITT-FALSE over kind, len and
payload; shell text never contains 0xA6, a reader resyncs on it and drops a bad CRC). `trace stream` / `trace stop` still
work and are an alias of `stream on trace`. The pure code is `include/k2/core/dbg_stream.hpp` (host test
`tests/test_dbg_stream.cpp`), the hardware glue `src/core/dbg_stream_hw.cpp`, the shell command `stream on [log|trace|power[=Hz]|run|all] [old]`
and `stream off` (`stream off` prints `stream stats ...`).

| Kind | Name | Payload |
|---|---|---|
| 1, 2, 3 | Start, Data, Lost | the trace kinds, unchanged (PD records exactly as the ring lays them out) |
| 4 | Hello | version, kind mask, power Hz, K2 ms, history flag, firmware hash (the `git describe` of the build) |
| 5 | Text | K2 ms, tag (1 = boot info line), up to 56 bytes: the boot header after Hello |
| 6 | Log | base ms, seq, whole K2LOG entries in the binary ring's own encoding |
| 7 | Power | K2 ms, window count, valid, VBUS mV, IBUS mA, CC1, CC2, D+, D- mV (0xFFFF = not read); a window of more than one sample adds span ms and VBUS/IBUS min and max |
| 8 | Run | K2 ms, event code, a, b, c, d (`include/k2/chargertest/run_events.hpp`) |
| 9 | Gap | source (2 log, 3 run), count, bytes: what the ring overwrote before it was sent |
| 10 | Beat | K2 ms, stack free, MSP free, page, flags, USB log drops, nine u32 counters (sent and lost per kind, stalls) |

How each kind is made:

- **Log.** Not text: the entries are the binary K2LOG records (`k2/core/binlog.hpp`: length, delta ms, message id, varint
  arguments). The host decodes them with the `tools/logdb.json` of the commit named in the Hello (`k2_qr.logdb_for`), so a
  dirty or unknown build says so on the first timeline line and reads ids it cannot know as `msg#<id> ?`.
- **Trace.** Records straight from the Black Box ring; timestamps are microseconds from the same clock as `millis()`, so
  they merge with every other kind by absolute K2 time.
- **Power.** The main loop samples VBUS, IBUS, both CC lines and D+/D- at the requested rate (`power=1..100`, default 20 Hz).
  It never queues samples: while the FIFO has no room the samples fold into one window (min and max of VBUS and IBUS,
  newest values), and an unchanged window is skipped except for a keepalive.
- **Run.** The charger-test page emits one 12-byte event per stage change, row start, row end (verdict, reason, measured mV),
  row end details (tries, requested mV and mA), unit attach/detach/hold, identity arm/disarm, suspend/resume, run start
  and end, and each PD phase it saw. Names are parsed on the host from `charger_test.hpp` and `ui_pages.hpp`.
  The rows a failure skips come as ONE `RowSkips` event per run of equal reason (rows a..b, reason c, d rows; the host
  prints `rows 43..58 end Skip reason=Inline (16 rows)`) instead of one row end each, and the page writes one `[TEST] rows
  from N skipped` log line per range: 55 skip rows in one pass overflowed both the 16-event run ring and the 1 KB log
  ring (hardware 2026-11-07). Any other event closes an open range first, so the order is kept.
- **Beat.** Once a second: the device's own counters, so the host can compare what was sent with what arrived even if the
  `stream stats` line never comes (the port was closed first).

### Bandwidth and priority

The CDC data endpoint is a 16 B bulk pipe: the sustained ceiling is about 16 KB/s, shared with `sniff stream` and any
shell text. A frame goes out only when the FIFO has room for a worst-case one (96 B), at most two frames per kind and
eight per main-loop pass, and the loop never waits. Order in one pass:

1. control (Hello, boot Text),
2. run events,
3. a power window that has waited 500 ms (so power cannot starve),
4. trace and log, taking turns at going first,
5. Beat (1 s),
6. power.

When the pipe is saturated, power is decimated first (folded into min/max windows), trace and log fall behind in their rings,
and a ring that wraps is reported by a Lost (trace) or Gap (log, run) frame with the exact count. Nothing from trace, log
or run is dropped silently; the host additionally counts a log sequence jump (frames lost on the wire), a bad CRC, and
compares the device's own sent counters with what it received (section 4).

### Why no compression

The stream is not LZ4 compressed. LZ4 needs a hash table (512 B on the stack, `lz::compress`) and a block buffer, gains
little on 6 to 12 byte records that are already domain encoded, adds latency (a record waits for its block), and turns one
lost block into many lost records. Instead: ids and delta timestamps, zigzag/varint arguments, repeat collapsing as in the
Black Box (`RxRepeat`), and change-only or windowed min/max power samples when saturated. LZ4 stays for batch records only
(QR share, stored captures).

### Cost and memory

Pure code over rings that already exist, so no new buffers: the multiplexer holds cursors and counters; a frame is built on
the caller's stack and the ring is the queue. New `.bss` is the run event ring (16 events, 192 B) and the multiplexer
state. Nothing runs in an ISR: the charger page pushes events from the main loop, and `service()` is called once per pass
with the free room of the CDC FIFO and a bounded frame count. `tests/test_dbg_stream.cpp` checks the per-pass frame bound,
the priorities, the saturation and loss accounting, and that a stalled reader costs no work.

## 3. Recording a session

```
python3 tools/k2_stream.py record --out runs/NAME [--kinds all|log,trace,power=50,run] [--old]
                                [--seconds N] [--until-run-end] [--settle 3] [--port /dev/ttyACM0]
                                [--echo run,log,start | --quiet]
```

It sends `stream on ...`, writes everything to the session folder as it arrives, and on Ctrl-C, `--seconds` or the run end
(`--until-run-end` stops `--settle` seconds after the End event) sends `stream off`, reads the `stream stats` line and prints
a completeness summary. Exit code 0 only when every device counter matches what arrived. `--old` also sends what the rings
still hold (history first). Do not start a second reader on the port (the recorder takes `TIOCEXCL`).

Session folder:

| File | Content |
|---|---|
| `timeline.txt` | everything merged, by absolute K2 time (ms since the K2 booted); first line names the firmware hash and the K2-clock-to-wall mapping |
| `log.jsonl`, `run.jsonl`, `power.jsonl`, `trace.jsonl`, `beat.jsonl`, `boot.jsonl`, `gaps.jsonl` | one row per record, each with `t_ms` (absolute K2), `wall` (host time of arrival), `kind`, `text` and the decoded fields |
| `trace.raw` | the Black Box records as the ring holds them (`pd_trace.py decode` reads it) |
| `meta.json` | Hello (hash, mask, Hz), logdb source and warning, K2 ms 0 as wall time, counters, `complete`, summary lines, boot text |

Read it:

```
python3 tools/k2_stream.py view runs/NAME                          # the whole timeline
python3 tools/k2_stream.py view runs/NAME --kind run,gap           # one kind at a time (start,boot,trace,log,power,run,beat,gap)
python3 tools/k2_stream.py view runs/NAME --grep 'row 7|reason='   # regex over the timeline lines
python3 tools/k2_stream.py view runs/NAME --from 62000 --to 65000 --wall
python3 tools/k2_stream.py view runs/NAME --summary                # completeness only
grep -E ' RUN | GAP ' runs/NAME/timeline.txt                       # plain grep works: every line starts with time and kind
```

Every hole is a row in the timeline: `GAP ... DEVICE LOST n log entries` (a ring wrapped), `GAP ... n log entries missing between
frames` (lost on the wire), `GAP ... bad CRC`, `GAP ... device lost n records` (trace). A session with no GAP rows and the
summary reading COMPLETE has everything the device sent.

## 4. Known limits

- The binary log ring is fed only by `K2LOG`. Lines written with plain `usb::log()` / `log_fmt()` (text) are not in the stream.
  `log on` (the older shell command) streams chip-log text unframed and is unchanged.
- No repeat collapsing for log lines yet; a chatty loop shows up as many Log entries (the ring and Gap accounting still
  hold).
- Run events carry no sequence number, so frames lost on the wire are detected only by the `stream stats` / heartbeat
  comparison (the summary says how many are missing, not which).
- The stream shares the 16 B CDC pipe with `sniff stream`; run both and the sniff edges stall first.
- A charger-test run that fails a group emits its skip rows in one burst; the run ring (16 events) reports an overflow as
  a Gap frame, never silently.
- The K2 clock is boot-relative (ms); the wall time of a row is the host's arrival time, the K2-to-wall mapping in
  `meta.json` is estimated from the Hello.

## 5. What is deliberately not streamed

The LOG.TXT text ring, the `/jr` journal, report cards and stored captures are batch records: they are read after a run
(drive mode, `k2_remote`, `pd_trace.py captures`). The D+/D- edge stream stays its own protocol (`tools/sniff_stream.py`,
sync 0xA5) because its rate is orders of magnitude above the rest.
