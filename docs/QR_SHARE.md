# QR share

The K2 shows a record (a boot report, a saved cable, the source capabilities, a charger report card) as one or more QR codes
that a phone can scan and a host tool can decode back into the record, with no USB link and no Wi-Fi. This file is the frame
format and the record schemas. Code: `include/k2/ui/qrshare.hpp` (frame, base45, CRC, parser), `include/k2/ui/qrview.hpp` (layout and
drawing), `include/k2/ui/qr.hpp` (the symbol generator).

## Text form

Every QR symbol carries one ASCII line, alphanumeric mode (the 45-character QR set):

    K2:<tag><schema>:<base45 of the frame>

`<tag>` is the record type (below), `<schema>` its version as 1 to 3 decimal digits. The prefix is readable in any scanner app,
and a part can be pasted into a text box as it is.

## Frame (before base45), little endian

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `'K'`, `'2'` |
| 2 | 1 | type (below) |
| 3 | 1 | schema (record version) |
| 4 | 1 | index: part number, 0 based |
| 5 | 1 | count: parts in the record, 1 to 4 |
| 6 | 2 | record_len: bytes in the whole record |
| 8 | 4 | record_crc: CRC-32 (zlib, poly 0xEDB88320) of the whole record |
| 12 | n | payload: this part's slice of the record |
| 12 + n | 4 | frame_crc: CRC-32 of everything before it |

Slices are equal: with `S = ceil(record_len / count)`, part `i` carries bytes `[i*S, min((i+1)*S, record_len))`. A reader
can therefore check a part's length from `(record_len, count, index)` and join the parts in any order; parts of one record
share type, schema, count, record_len and record_crc, so parts of different records never mix. A record is at most 1320 bytes
and at most 4 parts; one part carries at most 330 payload bytes, which keeps every part within QR version 12 (level L) at 3 px
per module with one text line under the box.

## Types

| Type | Tag | Record |
|---|---|---|
| 1 | BI | boot info: the K2B1 text (`k2/core/bootinfo.hpp`), ASCII |
| 2 | EMK | saved cable |
| 3 | PDC | source capabilities |
| 4 | CARD | charger report card |
| 5 | ID | firmware identity: `fw=1.0 g=<git> p=<proto>` |
| 6 | LOG | the newest K2LOG entries, binary (see Log record) |
| 7 | LOOP | the HID-cable loop probe result, binary (see Loop probe record) |

Bit 7 of the type byte (`0x80`, `kPackedFlag`) is the **LZ4 flag**: the record is an LZ4 block (`k2/core/lz.hpp`), not the raw
bytes. The part's `record_len` and `record_crc` are those of the packed block, so a reader assembles the parts first and inflates
once. A frame without the flag is the record as built, so every record made before the flag stays valid. The tag is the same
with or without it. The K2 never inflates (the decoder is on the host); it packs only when the block is at most 75 % of the
record (`qrshare::pack()`), which is decided per record, so a small or dense record goes out plain.

## Record schemas

Schema numbers: 1 for every type as laid out below; the card and the e-marker (cable) record are additionally sent as
**schema 2**, the bit-packed form below, which is what their K2 data frames (`K2:CARD2:...`, `K2:EMK2:...`) and their version '2'
links carry. A reader takes both: `tools/k2_qr.py` expands a schema 2 CARD/EMK frame back to the schema 1 record and decodes it
as before. Builders: `include/k2/ui/qrshare_records.hpp` (pure code, `tests/test_qrshare_records.cpp`). The payloads
reuse the serialisers the firmware already has, so an import on the K2 can call the parsers that exist.

| Type | Layout (little endian) | Size |
|---|---|---|
| BI | the `K2B1` text of `k2/core/bootinfo.hpp`, ASCII: `K2B1 fw=1.0 g=<git[-dirty]> p=<proto> t=<uptime> ...` | about 170 B |
| EMK | schema 1: `flags` u8 (bit0: a quirk block follows), `name[24]` (the user's name, NUL padded), the 66-byte e-marker record (`k2/pd/emrec.hpp`), then the quirk block (`k2/pd/emk_quirk.hpp` `encode()`) when bit0 is set. Schema 2: the bit-packed record below | 91 B plain, at most 510 B as schema 1; 44 B to 189 B as schema 2 |
| PDC | `num_pdos` u8, `flags` u8 (bit0 EPR caps, bit1 EPR chunks incomplete), `spec_rev` u8 (1 PD 2.0, 2 PD 3.x), `active_cc` u8 (0 none, 1 CC1, 2 CC2), raw message header u16, then `num_pdos` raw PDO words u32 by position (an unused slot is 0) | 6 + 4 per PDO, at most 50 B |
| CARD | schema 1: the report card as the store keeps it (`k2/chargertest/report_card.hpp` `encode_card()`: 0xA6, type, payload length, 0, the payload, CRC-16; card format 8). Schema 2: the bit-packed record below | 204 B as schema 1 (formats 3 to 7: 150 to 198 B); 68 to 188 B as schema 2 |
| ID | ASCII `fw=<version> g=<git, -dirty when dirty> p=<remote proto>`, the strings PING answers | at most 64 B |
| LOG | `git_len` u8, the git string, `flags` u8 (bit0: older entries were dropped), `base_ms` u32 (uptime of the entry before the first one), `now_ms` u32 (uptime when the record was made), then whole log entries to the end | 10 + git + entries, at most 510 B |
| LOOP | the loop probe record, version 1 (`k2/loopprobe/loop_probe.hpp` `encode()`, layout under "Loop probe record") | 399 B, two parts |

### Schema 2: the bit-packed card and e-marker record

The two big kinds have a second, bit-packed schema: an LSB-first bit stream, varint = 7 value bits + 1 continuation bit. It drops
what the schema 1 layout repeats or reserves (the unused group and range slots, the e-marker record's 24 reserved zero bytes) and
keeps the v1 card's CRC-16 at the end, so the integrity check is unchanged; every field is at most as wide as in schema 1, so
packing in place never grows the record. The full layout is the comment at the top of `include/k2/ui/qrshare_records.hpp`
(`pack_v2`), with host twins `expand_card_v2` / `expand_emk_v2` in `tools/k2_qr.py` and in `docs/site/r/card.js` (the results
page). Summary:

- **Card v2** (replaces the 204 B schema 1 record): schema u8 = 2; `fp` u32, `vid` u16, `pid` u16; varint `card_no`, `runs`,
  `boots`, `uptime_s`; `fw` u32; varint `handover_ds`; `spec_rev` u2, `num_pdos` u4, `handover` u3, `state` u4, `reason` u6;
  `n_ranges` u3, `ev_flags` u8; `name_len` u5 (at most 20) and the name as u7 chars; `pdp_w` u8, `name_auto` u1, `pd_ma50` u7;
  12 group results (verdict u2, reason u6, detail u6, has_max u1 [+ max_dv u9], has_fail u1 [+ fail_dv u9]); `n_ranges` ranges
  (alg u5, pos u8, lo_dv u9, hi_dv u9, max_dv u9, fail_dv u9, ma50 u7); 3 EPR slots (dv code u2: 0 none, 1 = 280, 2 = 360,
  3 = 480, then a group result); `cable_present` u1 [+ varint seq, vid u16, pid u16, max_mv u16, max_ma u16, kind code u3
  (0 = 0, 1 = 1, 2 = 2, 3 = 4, 4 = 0xFF), flags u8, resumed u8, src_flags u8]; `n_ufcs` u3 and its modes (lo_dv u8, hi_dv u9,
  ma50 u7); varint times [+ last_boots u16, last_uptime_s u24 when times > 1]; varint rebalanced (v8: the source's mid-run re-balance count, 0 on a format-7 record); crc16 u16 of the v1 record.
  The `reason` u6 fields hold the `ct::Reason` index (6 bits, so up to 64 codes; 45 are defined, 0..44, listed in the
  REASONS table of `tools/report_card.py`; 42 = "withdrawn by source", a row the source's mid-run re-announce dropped,
  pattern `budget-reannounce-mid-run`; 43 = "wrong voltage on 5V contract", a granted 5 V contract (PS_RDY at PDO 1)
  held at another voltage on VBUS when the attach wait ran out, pattern `shared-bus-split`; 44 = "needs load", a
  programmable level the source accepted and dropped at no load, a skip, pattern `pps-off-at-no-load`).
- **E-marker v2** (replaces 91 + 3 + 52 per quirk row B): schema u8 = 2; `name_len` u5 (at most 24) and the name as u7 chars;
  flags u8; the e-marker record: `name_len` u5 (at most 20) and its name as u7 chars, `spec_rev` u2, five VDOs u32, checksum u8;
  `has_quirks` u1 [+ flags u2, unmatched u2, count u4, then per row: sop u2, kind u2, nreq u2, nrep u3, mask_is_standard u1,
  hdr_val u32, hdr_mask u32 when the mask is not the standard SVDM one, nreq x (req_val u32, req_mask u32), nrep x rep u32)].

Measured on the golden vectors and the captured cards: a format-8 card is 68 to 109 B as schema 2 (the synthetic all-limits worst
case 188 B) against 204 B as schema 1; the 8-row quirk cable is 189 B as schema 2 against 510 B raw and 239 B LZ4 as schema 1.

### Log record

Every `K2LOG` call (`include/k2/core/log.hpp`) is already an id plus arguments: the message ids are append-only in `tools/logdb.json`,
so the map of a firmware version is that file at its git commit. `usb::log_packed()` also pushes each call into a binary ring
(`include/k2/core/binlog.hpp`, 1 KB in RAM, `usb::log_bin()` reads it), one entry per call:

    len u8 | dt varint | id varint | args...

- `len`: bytes after it (at most 63).
- `dt`: ms since the previous entry (32 bit wrap safe); `base_ms` in the record head plus the sum of the `dt`s is the uptime.
- `id`: the message id.
- args, in the order of the format string, by the argument's kind (2 bits per argument in the call signature, which the host
  reads from the format string of that id): signed = zigzag varint, unsigned (`%u`, `%x`) = varint, string = length byte plus up
  to 28 bytes. An argument takes the room it needs; when the next one does not fit the 63 bytes, the entry stops before it.

The ring drops whole oldest entries; the record carries the newest ones that fit 510 bytes, oldest first, and `older` says
whether anything before them was left out. A typical entry is 6 to 12 bytes, against about 60 for the text line.

Plain text lines written through `usb::log` / `log_fmt` (a few in `fusb302`, `fault`, `dfu`) have no id and are not in the ring;
they stay in the text ring (`LOG.TXT`, now 2 KB) and on the HID channel. This is deliberate.

    python3 tools/k2_qr.py decode --lines 'K2:LOG1:...'
    t=+120ms [BOOT] OK
    t=+125ms [BBSTORE] entries=3 used=672 free=5376

The host finds the map by the git string in the record head: `git show <hash>:tools/logdb.json` first, the working tree's file
with a warning on stderr when the commit is unknown or the build was `-dirty`. Ids the map does not know print as `msg#<id>`.

## Compression

Question: does a compressor pay for a 330 B QR part? Candidates measured on real payloads (the golden vectors plus a
1 KB and two 500 B captures of the log ring), bytes after compression:

| Payload | Raw | LZ4 | deflate (reference) | heatshrink (w8 l4) |
|---|---|---|---|---|
| boot info | 173 | 174 | 136 | 168 |
| identity | 26 | 28 | 28 | 29 |
| EMK plain | 91 | 56 | 45 | 47 |
| EMK with 8 quirk rows | 510 | 239 | 171 | 190 |
| PDC SPR | 26 | 28 | 29 | 29 |
| PDC EPR | 46 | 44 | 44 | 45 |
| card (format 5, 184 B; format 8 measured only with LZ4: 204 B to 109 B) | 184 | 107 | 86 | 94 |
| log, 1024 B ring | 1023 | 580 | 454 | 504 |
| log, 510 B record | 499 | 260 | 220 | 217 |
| log, 1024 B ring (second capture) | 1013 | 445 | 327 | 358 |

Decision: **LZ4 behind the frame flag, per record, only when it saves at least 25 %.** The K2 already links an LZ4 block
coder (`k2/core/lz.hpp`, used by the Black Box store), so no library flash is added; the host inflater is 25 lines of Python
(`tools/k2_qr.py`, no new pip dependency). deflate is the reference ceiling, 15 to 30 % smaller than LZ4 on the large records,
but the K2 has no deflate encoder and would need a vendored one. heatshrink lands within 10 to 25 % of deflate (and beats it on
one log capture) but is another vendored library, its flash would go over the text headroom, and it saves only about 20 to 60 B
over LZ4 on a payload that already fits; tamp was not adopted for the same reason. Domain encoding does most of the work
already: the log record's ids, varints and zigzag arguments shrink the log before any compressor sees it, and the e-marker
record is a binary struct. Small records (boot info, identity, PDC) stay plain: LZ4 does not pay there and the 75 % rule keeps
them so. The two that pack are the EMK record with quirks (510 to 239 B: two parts of 415 characters become one of 391, QR version 11 to 10)
and the card (format 8: 204 to 109 B, 338 to 197 characters, QR version 10 to 7; the format 5 card was 184 to 107 B, 309 to 194 characters).
Golden vectors with the flag: `emk_rows_lz`, `card_lz` in `tests/fixtures/qrshare.tsv`.

### Loop probe record

The result of the HID-cable loop probe (`docs/LOOP_PROBE.md`), the same bytes the K2 keeps in `/lp/last.rec` behind an 8-byte
file head (`LPR`, version, length u16, CRC-16/CCITT-FALSE u16). Raw readings only, little endian, exactly 399 bytes:

| Field | Layout |
|---|---|
| head | `version` u8 (1), `flags` u8 (bit0: in progress, the CC step had started), `fw[30]` (NUL padded), `start_ms` u32 (uptime at START) |
| steps | 5 x (`status` u8: 0 not run, 1 done, 2 skipped, 3 aborted, 4 interrupted; `why` u8; `t_ms` u32 from start; `dur_ms` u16), in the order Baseline, Drop, D+/D-, E-marker, CC |
| baseline | `n` u8, then avg/min/max i16 of VBUS mV, IBUS mA, aux mV, D+ mV, D- mV, CC1 mV, CC2 mV; HID port at start and end (`bits` u8, `frame` u16, `resets` u16); `active_cc` u8, link flags u8, guard u8, BC_LVL u8 |
| drop | `n` u8, 8 x (`load` u8, samples u8, VBUS avg/min/max i16, IBUS avg/min/max i16, aux i16) |
| D+/D- | `n` u8, 6 x (`drive` u8 = D+ level << 4 \| D- level, bus D+ mV i16, bus D- mV i16, HID bits u8, VBUS max mV i16); `pull_method` u8, `n` u8, 2 x (`on` u8, bus D+ mV i16, bus D- mV i16, HID bits u8) |
| e-marker | `valid` u8, `num_vdos` u8, 6 x VDO u32, `seq` u16, `read_mode` u8 |
| CC | `refusal` u8, `switches0` u8 as found, CC switch pins u8, `flags` u8 (bit0 restored, bit1 VBUS abort, bit2 disturbed), `n` u8, 5 x (`cfg` u8: 0x80 as found, 1 Rd CC1 only, 2 Rd CC2 only, 0 none; CC1 u16 and CC2 u16 in 12-bit ADC counts; BC_LVL u8: bits 1:0 CC1, 3:2 CC2, bit4 valid; VBUS mV u16) |

Unpacked it is two parts. The share view packs it like any record when LZ4 saves a quarter: a run with skipped steps and empty
rows does (the host fixture of `tests/test_ui_loop_probe.cpp` packs 399 to 203 B, one part), a run of varied readings may not.
The LZ4 flag in the type byte tells the decoder either way. `tools/k2_qr.py decode` prints every
field with its unit in the key name (`tools/loop_probe_rec.py`, the same decoder `k2_remote.py loop-probe` uses on the file).

## Measured capacity per part

Text length of one part is `len("K2:<tag><schema>:") + 3 * ceil(frame / 2)` with the base45 rule above; the smallest QR version
(level L) that holds it is read from the capacity table. Measured on the golden vectors of `tests/fixtures/qrshare.tsv`:

| Vector | Record | Parts | Text per part | QR version |
|---|---|---|---|---|
| identity | 26 B | 1 | 70 | 3 |
| pdcaps SPR (5 PDOs) | 26 B | 1 | 71 | 3 |
| pdcaps EPR (10 PDOs) | 46 B | 1 | 101 | 4 |
| emk plain | 91 B | 1 | 169 | 6 |
| boot | 173 B | 1 | 291 | 9 |
| card (format 8) | 204 B | 1 | 338 | 10 |
| emk with 8 quirk rows | 510 B | 2 (255 B each) | 415 | 11 |
| loop probe result | 399 B | 2 (200 + 199 B) | 333 | 9 |
| emk with 8 quirk rows, LZ4 flag | 239 B packed | 1 | 391 | 10 |
| card, LZ4 flag | 109 B packed | 1 | 197 | 7 |

The worst case is 330 payload bytes per part: a 346 byte frame, 519 base45 characters, 527 with the prefix, version 12 (65
modules, 73 with the 4 module quiet zone). The share view draws the code over the whole panel (look policy under "Link code
geometry"): a version 12 frame is a 219 px box at 3 px per module, centred.

## On the K2

- **Share view** (`Menu::Share`, remote page 18, `src/ui/ui_share.cpp`): one QR code, full screen and centred on the whole panel
  (the page hides the button strip, `PageIf::hides_strip`), the smallest version that holds the text, at the module size the
  look policy gives that version. The caption (the title and `part i/n` for a K2 data frame, `Scan: ...` for a link) sits in the
  margin under the code when that holds a 14 px line (versions up to 11 and 14 to 20; not at 12, 13 or 21); otherwise it is
  left out and **long OK** shows a text view instead (what the code is, its QR version and module size, the wide-camera hint
  for a code that needs it, the keys); long OK or BACK returns to the code, any other key acts on it. The parts of a multi
  part record cycle every 1.5 s; [+] / [-] page and stop the cycling, OK holds or resumes it, BACK returns. The screen repaints
  only when the part changes. The record lives in a static 510 byte buffer; each paint encodes its part in the shared scratch
  arena and gives it back: no heap, no page member for the encoder. The arena holds, per paint, either a link (code and temp,
  2 x 1280 B, the link text built at the front of temp and the pixel line over temp after the encode) or a K2 data frame (its
  text, code and temp, about 1.6 KB); a union of the two, 2560 B of the 2816 B arena, so bss is unchanged.
  A link of more than 644 characters (version 18 and up) first wipes the panel and says `Preparing code...` in the middle,
  then encodes with the generator's poll hook set to `lcd::pump` (USB and the idle hook are served once per mask candidate,
  eight times), wipes and draws the code.
- **Long OK = Share** (`docs/UI_DESIGN.md`) on: a saved cable (Who's attached, Actions, Saved cables), the Who's attached Source tab, a report
  card (Tools, Report cards, detail), the charger test Results page (the card of that run), the Diag page (the log) and the
  Loop probe page (Tools, Loop probe: the last result; a finished run opens the share view by itself).
  A record that LZ4 shrinks by 25 % or more goes out packed (the flag above); the share view packs in place in the scratch arena.
- Left out: built-in chips and the identity record have no opener (the LOG head carries the git string and uptime; no text
  headroom left for a second chooser; the identity and boot-info links and page views exist and are tested, so an opener is
  all that is missing); Black Box rings and captures are several KB, far over the 1320 B record limit (use `tools/pd_trace.py`).
- **Multi-part links are deferred.** A record that needs more than one code (the log; a record whose link passes the one-code
  cap of 929 characters, QR version 21) has no link form: the page would have to gather the parts of one record (links with a part number, kept in the
  browser until the set is complete). The 8-row quirk cable now fits one code as a version '2' link (189 B schema 2 record,
  302 characters, QR version 11), so every cable with up to 8 quirk rows has a link; until the rest fit, they are the K2 data
  frames and `tools/k2_qr.py`.
- **A record that fits one link opens as a link, not as the K2 data frame.** The kinds with a link form are the report card,
  a cable (e-marker record), the source's PDOs, the firmware identity and the boot info (table under "Link form"). Long OK on the
  charger test Results page, the Cards detail, a saved cable, a live e-marker read or the Who's attached Source tab opens the Share view
  showing one code that holds a web address; a phone camera opens the results page (`docs/site/r/`) and the page shows the
  record. OK in the Share view switches to the K2 data frame (what `tools/k2_qr.py` imports) and back; the strip names the code
  the press switches to (`Link` / `K2 data`). The caption under the link follows the kind: `Scan: full results` (card),
  `Scan: cable details`, `Scan: charger offers`, `Scan: firmware info` (identity and boot info). A record that does not fit
  one link (the log) has no link form and no toggle: it opens as the K2 data frames, as before. The card and the cable go out
  as version '2' links (their schema 2 record, packed in place in the share record buffer); the other kinds stay version '1'.
- **Boot info**: a key-mode boot whose key (PLUS or OK) is held 3 s from the plug-in shows the BI record as a QR code on the boot
  screen (`docs/ui_mockups/bootscreen.png`); a key released sooner goes straight to the normal UI, and a comeback boot (no key)
  never shows it. The remote `bootinfo` command has the record either way. It stays
  the K2 data frame: the boot info as a link is 290 characters, QR version 11, which is 61 modules, and at the screen's 3 px
  per module that is 207 px beside the 40 px text block, over the 240 px panel. The results page reads that data frame: scan it
  with a text scanner and paste the `K2:BI1:...` text into the page (or after its `#`), and it opens the boot report. The link
  form of the boot info is for the Share view. The boot screen's block of code and text is centred like the share code
  (a few pixels off the old position).
- Log lines: `[SHARE] type=<n> <bytes>B in <parts> part(s)` when a share opens, `[SHARE] nothing to share (<bytes> bytes)` when
  the record is empty or too large.

## On the host

`tools/k2_qr.py` is a second implementation of the frame format in Python, cross-checked against the firmware by the golden
vectors (`tests/fixtures/qrshare.tsv`, read by `tests/test_k2_qr.py` and `tests/test_qrshare_records.cpp`).

    python3 tools/k2_qr.py decode 'K2:EMK1:...' 'K2:EMK1:...'     # parts in any order, JSON out
    python3 tools/k2_qr.py decode @parts.txt                      # one part per line
    python3 tools/k2_qr.py decode-image part1.png part2.png       # scans with pyzbar, else the zbarimg program
    python3 tools/k2_qr.py encode --type emk --hex <record hex>   # one K2:... text per line

Exit codes: 0 ok, 1 incomplete set (the JSON names the `missing` parts), 2 bad input, 3 `decode-image` with no QR reader installed.
A phone scanner shows the same `K2:...` text; paste it into `decode`.

`python3 tools/k2_remote.py bootinfo [--json]` reads the boot info over USB (remote command 0x35); it carries the same text as
the BI record.

## Link form (one code, one record)

    https://yarost12.github.io/open-k2-firmware/r/#<version><kind>.<base64url>

- The base is one constant (`qrshare::link::kBase`, `include/k2/ui/qrshare_link.hpp`). Everything after `#` is the data: a
  browser never sends the fragment to the server, so the page decodes it on the phone and nothing is logged anywhere.
- `<version>` is the link version digit. `1` means schema 1 of every record below. `2` means the bit-packed schema 2 record
  (section "Schema 2" above) of the card and the cable, uncompressed, with the same kind letters as version '1'. Version '1'
  links stay valid; a page that does not know a version digit, or a version '2' form of a kind that has none, says so instead
  of guessing. `<kind>` is one letter: lower case is the record as the K2 builds it for the K2 data frame
  (`include/k2/ui/qrshare_records.hpp`), upper case is its LZ4 block, used when LZ4 saves at
  least 25 %, the same rule and the same block as the frame's packed flag (on a version '2' link the record is already the v2
  form, never packed). The card's packed letter is `z` (kept from the first
  version of the link, so links already in circulation read the same). `<base64url>` is RFC 4648 section 5 without padding.

  | Letter (as built / packed) | Record | Frame type |
  |---|---|---|
  | `c` / `z` | charger report card: as the store keeps it (`0xA6`, type, length, 0, payload (card format 3 to 7), CRC-16/CCITT-FALSE (little endian); the page checks the CRC and the length) on version '1'; its schema 2 record on version '2' | CARD |
  | `e` / `E` | cable: the EMK record (name, e-marker record, quirk block when present) on version '1'; its schema 2 record on version '2' | EMK |
  | `p` / `P` | the source's PDOs | PDC |
  | `i` / `I` | firmware identity | ID |
  | `b` / `B` | boot info | BI |
  | `t` | a plain ASCII text record, NUL padded, read up to the first NUL; version '1' only, never packed — the code of the QR scan test (below) | – |

- Which records get a link form is one rule (`qrshare::link::fits()`): the record that goes into the link (packed or not, or
  already a v2 record) must make a link of at most 929 characters, the byte-mode capacity of QR version 21 at level L (101
  modules, 2 px each on the panel, read by the main camera in the owner's scans). The base (with the `#`) is 47 characters and `<version><kind>.` 3,
  which leaves 879 characters of base64url: at most 659 record bytes. Every record an opener builds is at most the 510 byte
  share buffer, so the largest link in practice is 730 characters, QR version 19; versions 20 and 21 are headroom. (Before the
  look policy the limit was one version 11 code, 321 characters and 203 bytes: a cable with many quirk rows, a 510 byte
  record, had no link until now.) That holds for the card (as a version '2' link: 68 to 109 B for the golden and captured cards,
  141 to 196 characters, and 187 B, 300 characters, in the synthetic all-limits worst case, so the v2 card always fits with
  room; as a version '1' link the format-8 card is 204 B and makes a 322 character link, one character to spare, and formats
  3 to 6 were at most 196 B, 312 characters, and still decode), the identity (at most 64 B), the PDOs (at most 50 B), the boot info (about 180 B, 290 characters) and a
  cable with up to 8 quirk rows (189 B as a version '2' record, a 302 character link; as a version '1' link a cable with up to
  4 quirk rows is 261 characters packed). The log has no link form. The text is built at paint time in the share view's
  scratch text buffer: no new resident buffer.
- The page (`docs/site/r/`) reads the fragment and shows, for every kind, a header, a row of tiles and a full view in sections:
  - card (version '1' or '2' — both give the same view): one badge per capability (what the charger achieved, with a glyph and a word besides the colour) above the full results;
  - cable (version '1' or '2'): name, type, maker, current, voltage, USB speed, EPR, whether the record check value matches, the five identity words, and
    the quirk policy and rows when the cable has them;
  - source: one tile per PDO (voltage, current, power), the full PDO table, EPR, the PD revision;
  - identity and boot info: firmware version, build (a `-dirty` build is amber), remote protocol; boot mode, keys held, reset
    cause, VBUS and CC voltages, the Rd move, the first seconds' writes;
  - text (`t`): one tile, "QR scan test" with the text the code carried.

  A missing or damaged fragment gives a plain message and a paste box. **The page reads the K2 data frame too**
  (`K2:<tag><schema>:<base45>`, what a phone scanner shows for the boot screen's code or the Share view's "K2 data" code): paste
  it into the box, or put it after the `#` (`.../r/#K2:BI1:...`; a browser's `%20` for a base45 space is read back). The
  reader in `docs/site/r/card.js` (`splitFrames()`, called by `splitLink()`) is the JS twin of `tools/k2_qr.py`: base45, the
  frame and record CRC-32, the tag to kind map (BI, EMK, PDC, CARD, ID), packed (LZ4) frames, and schema 2 card and cable frames
  expanded to the schema 1 record; the record then goes through the same decoders and views as a link of the same kind.
  A record of several parts takes one `K2:` line per part, in any order (blank lines and CRLF are fine); with a part missing
  the page says how many parts the record has and which are missing. A part is checked on its own and the parts are checked
  against each other (same record, no duplicate part), so a mixed or damaged set is refused. The log (LOG) is not a record
  the page shows: it says to use `tools/k2_qr.py`. A frame of a schema the page does not read says so. A link of a letter
  the page does not know, or of a version it does not read, says so. It makes no network request.
  Tests: `tests/test_dataframes.mjs` (the boot screen's real frame, every single-part golden of `tests/fixtures/qrshare.tsv`,
  two-part cables, damaged and mixed sets).
- A card holds no date. The page shows the card number, run number, boot count and K2 uptime instead. A charger tested again with
  the same result (`docs/CHARGER_TEST_FLOW.md`, "Repeated runs") keeps one card; the Run section then adds a "Same result" row
  (`N runs, last at boot B, up <time>`) and the header says `same result xN`. The repeat count is the most the format keeps (255).
- **QR scan test.** The `qrtest` remote command (`tools/k2_remote.py qrtest list` prints the fit table — version, px, quiet
  zone, box size, byte-mode characters, record bytes; `qrtest <version> <px> [quiet]` shows a full-screen code of that
  geometry, `qrtest stop` ends it) fills the panel with a valid max-length kind-'t' link: the record is the ASCII text
  `QR scan test v<NN> <N>px` NUL-padded to the version's record cap, so the link is exactly the longest one that version
  holds and the page (which reads the text up to the first NUL) labels it. Scan each row of the table with a phone and note
  which read and from how far; the owner's scan matrix is in `docs/HW_TEST_PROCEDURES.md`.
- Decoders: `docs/site/r/card.js` (the link, LZ4, the version '2' expansion, the card) and `docs/site/r/records.js` (the other
  kinds, the 't' text) are the JavaScript
  twins of `tools/k2_qr.py`, key for key. Golden vectors: `tests/fixtures/qrlink.tsv` (written by `tests/test_qrshare_link.cpp`
  from the firmware's own encoder; a card row also holds the card as the firmware's JSON renderer writes it), read by
  `tests/test_qrlink.py` (Python, via `tools/k2_qr.py`) and `tests/test_cardpage.mjs` (the card), and
  `tests/fixtures/qrlink_decoded.json`, what the Python decoders give for every other kind (`K2_QRLINK_WRITE=1` rewrites it),
  which `tests/test_recordspage.mjs` requires the page's decoders to give too.

### Link code geometry

One link code is byte mode at level L: the smallest version that holds the link, drawn **full screen and centred** on the
240 by 240 px panel (the page hides the button strip). The box is `(modules + 2 * quiet) * px`, modules = 4 * version + 17,
and the box and its caption line (14 px) are centred as one block (the box alone when the caption is not shown). Byte-mode
capacity at level L and the largest record whose link fits it, measured on the vendored generator:

| QR version | modules | byte-L chars | max record B |
|---|---|---|---|
| 11 | 61 | 321 | 203 |
| 12 | 65 | 367 | 237 |
| 13 | 69 | 425 | 281 |
| 14 | 73 | 458 | 306 |
| 15 | 77 | 520 | 352 |
| 17 | 85 | 644 | 445 |
| 18 | 89 | 718 | 501 |
| 20 | 97 | 858 | 606 |
| 21 | 101 | 929 | 659 |
| 24 | 113 | 1171 | 840 |

#### Look policy

One table (`qrview::look_for`) gives the module size and quiet zone of each version; the share view and the QR test page both
use it. It follows what the owner's phone cameras read:

| QR version | px per module | quiet | box (px) | camera |
|---|---|---|---|---|
| 1 to 13 | 3 | 4 | up to 231 (v13) | main |
| 14 to 21 | 2 | 4 | 162 (v14) to 218 (v21) | main |
| 22 and 23 | 2 | 4 | 226, 234 | wide camera only; hint `Use the wide camera` |
| 24 | 2 | 2 | 234 | wide camera only; hint `Use the wide camera` |
| 25 and up | none | | | not drawn |

The hint is for the text view (long OK) and the margin under the code when it holds a line; v22 to v24 are reached only by a
record over 929 characters, which the link rule does not produce today (the cap is version 21), so that row is the policy
for a later, bigger cap. Owner scans (qrtest rig, 2026-11-09): v13 at 3 px / quiet 4 (231 px, 425 characters) read with the
main camera; v18 at 2 px / quiet 4 (194 px, about 670 characters) read comfortably with the main camera; v21 at 2 px (about
930 characters) read; v24 at 2 px / quiet 2 (234 px, 1171 characters) read with the ultrawide camera only. (The v21 scan was
logged at a 210 px box; at quiet 4 the box is 218 px.) A 2 px, quiet 2 version 25 would be 238 px at quiet 1: out of the
policy. The look table is checked on the host for every version it can pick: the box is centred within 1 px (`test_qrview`,
`test_ui_share`), and decodes for the sizes the vendored decoder reaches (up to version 24).

The 14 px caption fits under the code only when the box leaves it room (versions up to 11 and 14 to 20); at 12, 13 and 21 the
margin is too small and the caption is left out: long OK shows the text view instead. Encoding a link of more than 644
characters (version 18 and up) takes a noticeable moment on the device, so the page says `Preparing code...` first and keeps
USB served through the encode (the poll hook runs once per mask candidate; the time per encode on the K2 is not measured yet).

## Capacity

Measured by `tests/test_qrshare.cpp` (`K2_DUMP=dir` prints the table), level L, alphanumeric mode:

| Version | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Alphanumeric characters | 25 | 47 | 77 | 114 | 154 | 195 | 224 | 279 | 335 | 395 | 468 | 535 |

Base45 turns 2 bytes into 3 characters, and an alphanumeric QR character costs 5.5 bits, so base45 text is about 3 % larger
than the same bytes in byte mode. The text form is chosen for the readable prefix and the plain copyable text, not for
density.
