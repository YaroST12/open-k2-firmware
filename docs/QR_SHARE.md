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

Bit 7 of the type byte (`0x80`, `kPackedFlag`) is the **LZ4 flag**: the record is an LZ4 block (`k2/core/lz.hpp`), not the raw
bytes. The part's `record_len` and `record_crc` are those of the packed block, so a reader assembles the parts first and inflates
once. A frame without the flag is the record as built, so every record made before the flag stays valid. The tag is the same
with or without it. The K2 never inflates (the decoder is on the host); it packs only when the block is at most 75 % of the
record (`qrshare::pack()`), which is decided per record, so a small or dense record goes out plain.

## Record schemas

All schema numbers are 1. Builders: `include/k2/ui/qrshare_records.hpp` (pure code, `tests/test_qrshare_records.cpp`). The payloads
reuse the serialisers the firmware already has, so an import on the K2 can call the parsers that exist.

| Type | Layout (little endian) | Size |
|---|---|---|
| BI | the `K2B1` text of `k2/core/bootinfo.hpp`, ASCII: `K2B1 fw=1.0 g=<git[-dirty]> p=<proto> t=<uptime> ...` | about 170 B |
| EMK | `flags` u8 (bit0: a quirk block follows), `name[24]` (the user's name, NUL padded), the 66-byte e-marker record (`k2/pd/emrec.hpp`), then the quirk block (`k2/pd/emk_quirk.hpp` `encode()`) when bit0 is set | 91 B plain, at most 510 B |
| PDC | `num_pdos` u8, `flags` u8 (bit0 EPR caps, bit1 EPR chunks incomplete), `spec_rev` u8 (1 PD 2.0, 2 PD 3.x), `active_cc` u8 (0 none, 1 CC1, 2 CC2), raw message header u16, then `num_pdos` raw PDO words u32 by position (an unused slot is 0) | 6 + 4 per PDO, at most 50 B |
| CARD | the report card as the store keeps it (`k2/chargertest/report_card.hpp` `encode_card()`: 0xA6, type, payload length, 0, the payload, CRC-16; card format 6) | 196 B (formats 3 to 5: 150 to 184 B) |
| ID | ASCII `fw=<version> g=<git, -dirty when dirty> p=<remote proto>`, the strings PING answers | at most 64 B |
| LOG | `git_len` u8, the git string, `flags` u8 (bit0: older entries were dropped), `base_ms` u32 (uptime of the entry before the first one), `now_ms` u32 (uptime when the record was made), then whole log entries to the end | 10 + git + entries, at most 510 B |

### Log record

Every `K2LOG` call (`include/k2/core/log.hpp`) is already an id plus arguments: the message ids are append-only in `tools/logdb.json`,
so the map of a firmware version is that file at its git commit. `usb::log_packed()` also pushes each call into a binary ring
(`include/k2/core/binlog.hpp`, 1 KB in RAM, `usb::log_bin()` reads it), one entry per call:

    len u8 | dt varint | id varint | args...

- `len`: bytes after it (at most 48).
- `dt`: ms since the previous entry (32 bit wrap safe); `base_ms` in the record head plus the sum of the `dt`s is the uptime.
- `id`: the message id.
- args, in the order of the format string, by the argument's kind (2 bits per argument in the call signature, which the host
  reads from the format string of that id): signed = zigzag varint, unsigned (`%u`, `%x`) = varint, string = length byte plus up
  to 12 bytes.

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
| card (format 5, 184 B; format 6 measured only with LZ4: 196 B to 107 B) | 184 | 107 | 86 | 94 |
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
and the card (format 6: 196 to 107 B, 327 to 194 characters, QR version 9 to 6; the format 5 card was 184 to 107 B, 309 to 194 characters).
Golden vectors with the flag: `emk_rows_lz`, `card_lz` in `tests/fixtures/qrshare.tsv`.

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
| card (format 6) | 196 B | 1 | 327 | 9 |
| emk with 8 quirk rows | 510 B | 2 (255 B each) | 415 | 11 |
| emk with 8 quirk rows, LZ4 flag | 239 B packed | 1 | 391 | 10 |
| card, LZ4 flag | 107 B packed | 1 | 194 | 6 |

The worst case is 330 payload bytes per part: a 346 byte frame, 519 base45 characters, 527 with the prefix, version 12 (65
modules, 73 with the 4 module quiet zone). The share view draws into the content area beside the key labels (224 by 240 px
portrait, 240 by 224 landscape) and keeps one text line under the code: 3 px per module in portrait up to version 12 and in
landscape up to version 11; version 12 in landscape drops to 2 px (the camera-safe 3 px of the boot screen does not fit there).

## On the K2

- **Share view** (`Menu::Share`, remote page 18, `src/ui/ui_share.cpp`): title, one QR code, `part i/n`. The parts of a multi part
  record cycle every 1.5 s; long [+] / [-] page and stop the cycling, OK holds or resumes it, BACK returns. The screen repaints
  only when the part changes. The record lives in a static 510 byte buffer; each paint encodes its part in the shared scratch
  arena (text, encoder workspace and one pixel line, about 2.1 KB) and gives it back: no heap, frames under 1024 bytes.
- **Long OK = Share** (`docs/UI_DESIGN.md`) on: a saved cable (Connection, Actions, Saved cables), the Connection Source tab, a report
  card (Tools, Report cards, detail), the charger test Results page (the card of that run) and the Diag page (the log).
  A record that LZ4 shrinks by 25 % or more goes out packed (the flag above); the share view packs in place in the scratch arena.
- Left out: built-in chips and the identity record have no opener (the LOG head carries the git string and uptime; no text
  headroom left for a second chooser; the identity and boot-info links and page views exist and are tested, so an opener is
  all that is missing); Black Box rings and captures are several KB, far over the 1320 B record limit (use `tools/pd_trace.py`).
- **Multi-part links are deferred.** A record that needs more than one code (a cable whose quirk rows do not fit one link even packed, such as the 8-row record; the log) has no link form:
  the page would have to gather the parts of one record (links with a part number, kept in the browser until the set is
  complete). That waits for a better compression of these records (the 8-row cable is 239 B after LZ4, a 369 character link,
  over one code). Until then they are the K2 data frames and `tools/k2_qr.py`.
- **A record that fits one link opens as a link, not as the K2 data frame.** The kinds with a link form are the report card,
  a cable (e-marker record), the source's PDOs, the firmware identity and the boot info (table under "Link form"). Long OK on the
  charger test Results page, the Cards detail, a saved cable, a live e-marker read or the Connection Source tab opens the Share view
  showing one code that holds a web address; a phone camera opens the results page (`docs/site/r/`) and the page shows the
  record. OK in the Share view switches to the K2 data frame (what `tools/k2_qr.py` imports) and back; the strip names the code
  the press switches to (`Link` / `K2 data`). The caption under the link follows the kind: `Scan: full results` (card),
  `Scan: cable details`, `Scan: charger offers`, `Scan: firmware info` (identity and boot info). A record that does not fit
  one link (a cable with many quirk rows, the log) has no link form and no toggle: it opens as the K2 data frames, as before.
- **Boot info**: a key-mode boot shows the BI record as a QR code on the boot screen (`docs/ui_mockups/bootscreen.png`). It stays
  the K2 data frame: the boot info as a link is 290 characters, QR version 11, which is 61 modules, and at the screen's 3 px
  per module that is 207 px beside the 40 px text block, over the 240 px panel. The link form of the boot info is for the Share
  view only (nothing opens it yet; see "Left out").
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

    https://yarost12.github.io/open-k2-firmware/r/#1<kind>.<base64url>

- The base is one constant (`qrshare::link::kBase`, `include/k2/ui/qrshare_link.hpp`). Everything after `#` is the data: a
  browser never sends the fragment to the server, so the page decodes it on the phone and nothing is logged anywhere.
- `1` is the link version, and means schema 1 of every record below. `<kind>` is one letter: lower case is the record as the K2
  builds it for the K2 data frame (`include/k2/ui/qrshare_records.hpp`), upper case is its LZ4 block, used when LZ4 saves at
  least 25 %, the same rule and the same block as the frame's packed flag. The card's packed letter is `z` (kept from the first
  version of the link, so links already in circulation read the same). `<base64url>` is RFC 4648 section 5 without padding.

  | Letter (as built / packed) | Record | Frame type |
  |---|---|---|
  | `c` / `z` | charger report card, as the store keeps it: `0xA6`, type, length, 0, payload (card format 3 to 6), CRC-16/CCITT-FALSE (little endian); the page checks the CRC and the length | CARD |
  | `e` / `E` | cable: the EMK record (name, e-marker record, quirk block when present) | EMK |
  | `p` / `P` | the source's PDOs | PDC |
  | `i` / `I` | firmware identity | ID |
  | `b` / `B` | boot info | BI |

- Which records get a link form is one rule (`qrshare::link::fits()`): the record that goes into the link (packed or not) must
  make a link of at most 321 characters, the byte-mode capacity of QR version 11 at level L (61 modules, 3 px each on the
  panel). The base (with the `#`) is 47 characters and `1<kind>.` 3, which leaves 271 characters of base64url: at most 203 record bytes. That holds for the card (format 6, the
  worst card is 196 B and makes a 312 character link: a QR version 11 code; formats 3 to 5 were at most 184 B, 296 characters,
  and still decode; the UFCS mode block of format 6 is the 12 bytes at payload offset 178), the identity (at most 64 B), the PDOs (at most 50 B), the boot info (about 180 B, 290 characters) and a
  cable with up to 4 quirk rows (261 characters packed; the 8-row record is 239 B packed, a 369 character link, so it has none).
  The log has no link form. The text is built at paint time in the share view's scratch text buffer: no new resident buffer.
- The page (`docs/site/r/`) reads the fragment and shows, for every kind, a header, a row of tiles and a full view in sections:
  - card: one badge per capability (what the charger achieved, with a glyph and a word besides the colour) above the full results;
  - cable: name, type, maker, current, voltage, USB speed, EPR, whether the record check value matches, the five identity words, and
    the quirk policy and rows when the cable has them;
  - source: one tile per PDO (voltage, current, power), the full PDO table, EPR, the PD revision;
  - identity and boot info: firmware version, build (a `-dirty` build is amber), remote protocol; boot mode, keys held, reset
    cause, VBUS and CC voltages, the Rd move, the first seconds' writes.

  A missing or damaged fragment gives a plain message and a paste box; pasting the K2 data frame (`K2:CARD1:...`) says it is the
  data code and asks for the link. A link of a letter the page does not know says so. It makes no network request.
- A card holds no date. The page shows the card number, run number, boot count and K2 uptime instead.
- Decoders: `docs/site/r/card.js` (the link, LZ4, the card) and `docs/site/r/records.js` (the other kinds) are the JavaScript
  twins of `tools/k2_qr.py`, key for key. Golden vectors: `tests/fixtures/qrlink.tsv` (written by `tests/test_qrshare_link.cpp`
  from the firmware's own encoder; a card row also holds the card as the firmware's JSON renderer writes it), read by
  `tests/test_qrlink.py` (Python, via `tools/k2_qr.py`) and `tests/test_cardpage.mjs` (the card), and
  `tests/fixtures/qrlink_decoded.json`, what the Python decoders give for every other kind (`K2_QRLINK_WRITE=1` rewrites it),
  which `tests/test_recordspage.mjs` requires the page's decoders to give too.

## Capacity

Measured by `tests/test_qrshare.cpp` (`K2_DUMP=dir` prints the table), level L, alphanumeric mode:

| Version | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Alphanumeric characters | 25 | 47 | 77 | 114 | 154 | 195 | 224 | 279 | 335 | 395 | 468 | 535 |

Base45 turns 2 bytes into 3 characters, and an alphanumeric QR character costs 5.5 bits, so base45 text is about 3 % larger
than the same bytes in byte mode. The text form is chosen for the readable prefix and the plain copyable text, not for
density.
