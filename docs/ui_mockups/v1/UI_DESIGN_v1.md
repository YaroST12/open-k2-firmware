# K2 UI design (240 x 240 ST7789)

Status: design proposal, 2026-10-31. Nothing here is implemented yet. It is written against `k2/core`
(`include/k2/ui/ui*.hpp`, `src/ui/ui*.cpp`). The mockups are in `docs/ui_mockups/`: 32 PNGs at x2 (480 x 480),
rendered from the repo's JetBrains Mono TTF by `render_mockups.py`. `screens.md` holds, for every
screen, a grid sketch and a pixel table (x, y, w, h, font, token). That generated file is the
pixel-accurate part of this spec. This document holds the system, the rules and the reasons.

Re-render: `python3 docs/ui_mockups/render_mockups.py` (host only; it never touches the device).

Contents: 1 Context · 2 Design system · 3 Screen templates · 4 Pages and states · 5 Navigation map ·
6 Interaction rules · 7 Copy rules · 8 Critique of the current UI · 9 Ranked change list · 10 Open questions

---

## 1 Context

- **Users.** People who test USB-C power: they plug a charger, cable or phone into the K2 and glance at
  it. Most of them are perpetual intermediates. Two postures:
  - Home and PD are **sovereign**: open for minutes, read at a glance from 30-60 cm, and they must be
    dense.
  - Tools are **transient**: one task, one result, then back to Home. They must make the
    prerequisites obvious.
- **Input.** Four buttons on the right edge: BACK, MINUS, OK, PLUS. A short +/- press scrolls, a long
  press (0.5 s) changes page, and holding BACK for 5 s reboots. There is no touch, so there is no
  Fitts's-law cost, but each extra press costs time and each long press is a hidden mode.
- **Screen.** 240 x 240 RGB565. The label panel takes x 224..239, so the content area is
  **224 x 240**. Fonts are JetBrains Mono, anti-aliased, monospace:

  | id | px | advance | ascent | line | glyphs |
  |---|---|---|---|---|---|
  | `D48` | 48 | 29 | 35 | 35 | digits `0-9 . - :` only |
  | `T24` | 24 | 14 | 22 | 26 | ASCII + `° · Ω ← ↑ → ↓` |
  | `B16` | 16 | 10 | 14 | 17 | ASCII + `° · Ω ← ↑ → ↓` |
  | `S12` | 12 | 7 | 11 | 13 | ASCII only (no °, Ω or arrows) |

  Width budget for the 224 px content area minus 4 px gutters (216 px): `B16` fits 21 chars, `S12`
  fits 30, `T24` fits 15, `D48` fits 7.

## 2 Design system

### 2.1 Grid and spacing

- Base unit **2 px**. The spacing scale is 2 / 4 / 6 / 8 / 12 px (`sp-1` .. `sp-6`). Every x and y in
  the mockups is a multiple of 2.
- **Gutter** 4 px left (`x = 4`). **Right text edge** `x = 216`, since right-aligned numbers end there.
  `x = 217..220` is clear space and `x = 221..223` is the scrollbar. The panel starts at `x = 224`.
- Fixed vertical regions, the same on every screen:

  | region | y | height | font | purpose |
  |---|---|---|---|---|
  | Status bar | 0..17 | 18 | `S12` (text top y = 3) | title or breadcrumb, mini live V/A/flow, `n/N` or LAST badge |
  | Body | 20..219 | 200 | any | page content |
  | Footer | 222..239 | 18 | `S12` (text top y = 225) | state dot, one-line state or reason, next action |

- Row pitch: `B16` lists **20 px** (17 px line + 3 px leading), so 10 rows fit in the body. `S12` rows
  use 13-14 px. `T24` rows use 26 px. Two-line list items (name + hint) use **40 px**, so 5 fit in the
  body.
- Two-column layouts (Home secondary block, scan grid) split at **x = 4 / x = 116** (column 112 px). A
  right-aligned value in the left column ends at x = 106; in the right column it ends at x = 216.

### 2.2 Type scale (semantic)

| token | font | use |
|---|---|---|
| `type-meter` | D48 | the Home V/A/W values and the Cable R result. Digits only. |
| `type-headline` | T24 | units next to D48, the verdict or result of a tool, empty-state titles |
| `type-body` | B16 | list rows, key/value rows, checklist items, setup text |
| `type-caption` | S12 | status bar, footer, column heads, overlines (`STEP 1`, `TARGET`), one-line hints, panel labels |

Rules: at most **3 sizes per screen** (Home uses 4 because of the meter). Units are one step smaller
than their value. Overlines are S12 upper case, 3 words or fewer.

### 2.3 Colour tokens (RGB565)

Tier 1 is the raw value. Tier 2 is the semantic token that pages use. Never write a raw value in page
code; ui_style.hpp holds the tier-2 names.

| token | RGB565 | swatch | meaning | change from today |
|---|---|---|---|---|
| `bg` | `0x0000` | black | content background | = `kBg` |
| `surface` | `0x18E3` | dark grey | selected row, cards, wiring strip | new |
| `bar` | `0x08A9` | navy | status bar and footer background | = `kBar` |
| `panel` | `0x2104` | | label panel | = `kPanelBg` |
| `rule` | `0x2945` | | dividers, scroll track, bar tracks | new |
| `text` | `0xFFFF` | white | primary text | = `kText` |
| `dim` | `0x8410` | grey | labels, keys, hints | = `kDim` |
| `faint` | `0x4208` | dark grey | "no reading" dashes, unavailable items | new |
| `volt` | `0xFFE0` | yellow | voltage | = `kVolt` |
| `amp` | `0x07FF` | cyan | current | = `kAmp` |
| `watt` | `0xFD20` | orange | power | = `kWatt` |
| `energy` | `0x07E0` | green | Wh, and the active contract | = `kEnergy` |
| `charge` | `0x5D1F` | blue | Ah | new (Ah has no colour today) |
| `flow-fwd` | `0x07E0` | green | arrow → (IN to OUT) | = `kFlowFwd` |
| `flow-rev` | `0xF81F` | magenta | arrow ← (OUT to IN) | **was `kWatt` orange**, which clashed with W |
| `stale` | `0xC618` | light grey | LAST badge fill; frozen values | new; the badge was orange, which clashed with W |
| `ok` | `0x07E0` | green | state dot / READY | |
| `caution` | `0xFC00` | amber | needs action, waiting, partial | new (distinct from `watt`) |
| `error` | `0xF800` | red | cannot run, hardware missing | = `kWarn` |
| `info` / `focus` | `0x07FF` | cyan | focus bar, edit underline, pressed slot | = `kPanelHi` |

Colour is never the only signal (WCAG 1.4.1). Every state has a word as well: `READY`, `NO PD`,
`LAST`, `fwd`/`rev`, a footer sentence. The state dot (6 x 6 px square) is redundant with that word.

Contrast note: `dim` on `bg` is 5.3:1 and `dim` on `bar` is about 4.0:1. That is fine for S12
captions, which are secondary. `faint` (1.9:1) is used only for "no data" dashes, where low contrast
is the message.

### 2.4 Components

| component | anatomy | tokens | states |
|---|---|---|---|
| **StatusBar** | title (S12, ≤ 8 chars on rotation pages; `<Name` breadcrumb in submenus) · MiniReadout · PageIndex or LastBadge | `bar`, `text`, `dim` | live, none (`--.--V -.--A`, `faint`), stale |
| **MiniReadout** | `20.01V` (`volt`) `2.99A` (`amp`) `→` (B16, flow colour) | x = 60 on rotation pages, x = 118 in submenus | live / none / stale (all `stale`) |
| **LastBadge** | 64 x 14 box at (158, 2), fill `stale`, text `LAST m:ss` in `bg` | `stale` | shown only while data are frozen |
| **Footer** | 6 x 6 state dot at (4, 228) + S12 message at x = 14 | dot: `ok`/`caution`/`error` | always present; ≤ 30 chars |
| **MeterRow** | D48 value right-aligned to x = 178 · T24 unit at x = 182 on the D48 baseline | quantity colour | live / zero (`faint`) / none (`--.--`) / stale |
| **FlowArrow** | T24 `→` or `←` at (204, 79) + S12 `fwd`/`rev` caption at (200, 100) | `flow-fwd` / `flow-rev` | idle draws nothing (deadband, see memory "no sign as direction") |
| **KVRow** | B16 key at x = 4 (`dim`) · value right-aligned to 216 | value: quantity colour | |
| **ListRow2** | 40 px: B16 name at y + 2, S12 hint at y + 21, S12 tag right-aligned at y + 22 | selected: `surface` fill + 3 px `focus` bar at x = 0 | ready / caution / error tag |
| **WiringStrip** | 21 px `surface` band: `<what on IN> →IN K2 OUT→ <what on OUT>`, B16 | each side `ok` (present, correct), `caution` (present, wrong), `error` (missing, required), `dim` (must be empty) | |
| **Checklist** | 6 x 6 dot at x = 6 + B16 text at x = 18, pitch 20 | met = `ok` dot + `dim` text; unmet = `error`/`caution` dot + `text` text | the unmet items stand out |
| **ProgressBar** | 8 px track (`rule`) + fill | fill: state colour | determinate only; shows step n/N in the footer |
| **Scrollbar** | 3 px at x = 221, track `rule`, thumb `dim` (min 8 px) | | hidden when everything fits |
| **HoldCard** | 200 x 80 at (12, 80), `surface`, 2 px `error` top edge, T24 `Reboot in N`, progress, S12 `Release Back to cancel` | | appears 1 s into a BACK hold |
| **LabelPanel** | 4 slots 66/54/54/66 px, S12 rotated CCW, 2 px `line` dividers | pressed slot flashes `focus` 120 ms | empty slot = no action |

## 3 Screen templates

```
x:0         112          216 224 239
y:0   ┌─ StatusBar  title | mini V A → | n/N or LAST ─┐ ┌──┐
y:18  ├──────────────────────────────────────────────┤ │B │ slot 0   y 0..65
y:20  │                                              │ │  │
      │  Body (200 px)                               │ ├──┤
      │   template A: meter (Home)                   │ │- │ slot 1   y 66..119
      │   template B: list (PD, Diag, Lines, Tools)  │ ├──┤
      │   template C: tool (setup → run → result)    │ │OK│ slot 2   y 120..173
      │                                              │ ├──┤
y:219 │                                              │ │+ │ slot 3   y 174..239
y:222 ├─ Footer  ■ state / reason / next action ─────┤ │  │
y:239 └──────────────────────────────────────────────┘ └──┘
```

Template C (every tool submenu) always has the same four phases, in the same positions:

1. **Setup**: WiringStrip (y 22..42), Checklist (y 54..), one plain-language sentence on what the tool does,
   and the footer `Ready. OK: <verb>` or the first unmet prerequisite.
2. **Refused**: the same layout, with the failing checklist item in red and the footer saying why
   (`No PD source on IN. Plug one.`). The OK label disappears when the tool cannot run at all, and it
   stays when the user can override (`OK again forces it`).
3. **Running**: T24 headline + ProgressBar + step text. BACK cancels.
4. **Result**: a verdict card (`surface`, T24 headline in a quantity or verdict colour), then KVRows,
   scrolled with +/-, and the footer `Read 0:05 ago`.

## 4 Pages and states

The pixel tables and grid sketches are in `ui_mockups/screens.md`. The table below lists every screen and
its PNG, followed by notes on the decisions that are not obvious from the image.

| screen | file | state shown |
|---|---|---|
| Home | `home_live.png` | live, forward flow |
| | `home_reverse.png` | reverse flow (magenta ←, caption `rev`) |
| | `home_idle.png` | attached, no VBUS: zeros in `faint`, footer tells where to plug |
| | `home_stale.png` | after unplug: grey digits, LAST 0:42 badge, footer `Unplugged. Values frozen` |
| | `home_error.png` | no power meter: dashes, red footer `Meter not found. See Diag.` |
| | `home_reboot_hold.png` | BACK held: countdown card |
| PD | `pd_live.png` | contract strip + PDO list, active PDO marker |
| | `pd_wait.png` | empty state: `No PD source` + what to do + `OK: Rescan` |
| | `pd_stale.png` | frozen caps, LAST badge, `OK: clear` |
| Diag | `diag.png` | key/value list |
| Lines | `lines.png` | D+/D-/CC1/CC2/AUX with 0..3.3 V bars and a 0.4 V tick |
| Tools | `tools_ready.png`, `tools_caution.png`, `tools_blocked.png` | readiness tag per tool, and the reason for the selected tool in the footer |
| E-marker | `emarker_setup/sniff/scan/result/none.png` | setup, source present (listen mode), scanning, result, no e-marker |
| Identify | `identify_setup/refused/result.png` | |
| FC trigger | `fc_setup/run/scan/refused.png` | |
| PD trigger | `pdt_list/adjust/refused.png` | |
| Cable R | `cabler_setup/result/error.png` | |

### 4.1 Home: big meter (template A)

```
  0         60          120         180    224
0 [PD3.0 CC1  INA228 32C              1/5 ]   status bar: S12 dim, PD rev white, CC cyan
22   ██ 20.01 ██████████████████  V            D48 volt, right edge 178; T24 unit x 182
66   ██ 2.990 ██████████████████  A  →         flow arrow T24 (204,79), caption S12 `fwd` (200,100)
110  ██ 59.83 ██████████████████  W
152 ───────────────────────────────────────    rule
158 Wh   12.345     Ah    0.617               B16; values right edge 106 / 216
180 t  01:23:45     R     6.69Ω               load R = V/I (B16 has Ω)
202 D+0.60 D-0.60           QC3 9V            S12
222 [■ Live  avg 1s  Long OK: reset       ]   footer
```

Per-element rationale:
- **V/A/W in D48 at a 44 px pitch.** These three numbers are the reason the product exists. They take
  58 % of the body (Von Restorff, visual hierarchy). The pitch is 44 px instead of 50, which frees
  y 152..219 for a second tier of data without shrinking the digits.
- **Fixed decimals per magnitude** (V `xx.xx`, A `x.xxx`, W `xx.xx`, the current behaviour). Digits must
  not jump while the user reads them. Right alignment keeps the decimal point in a stable column.
- **Arrow beside A, not on its own row.** Direction belongs to the current (and so to the power). The
  `fwd`/`rev` caption makes it readable without knowing the colour code. The deadband shows nothing, so a
  noisy 0 mA never flickers between ← and →. **No minus sign anywhere** (memory: ui-no-sign-direction).
- **Ah and load R added.** Ah is what battery testers compare. Load R tells a resistor-load user whether
  the load is what they think. Both come from data the firmware already has (`meter`, `pm`).
- **The status bar keeps the PD rev, CC pin, meter chip and temperature,** shortened to S12 so it fits
  in 18 px, which gives 4 px back to the body.
- **The footer replaces the TIME/protocol line.** Time moved into the grid. The footer now carries the
  state (Live / No VBUS / Unplugged / Meter not found) and the hidden long-press action (`Long OK: reset`),
  which addresses the hidden-mode problem (H6, recognition over recall).
- **Stale.** All values turn `stale` grey, never blank. The user still needs the last reading, which is
  the point of a LAST state. The badge counts up (`LAST0:42`), the arrow is removed (there is no flow
  now), and the footer says so in words.
- **Idle and error differ visibly.** Idle shows `0.00` in `faint`, meaning "measured, and it is zero".
  Error shows `--.--`, meaning "nothing to measure with". The footer names the cause and where to look.

### 4.2 PD (template B)

- **Contract strip** (y 20..40, `surface`, `energy`): `4 FIX 20.00V 3.25A 65W`. This answers "what am I
  getting now" first, before the list of everything that is offered.
- Column head S12 at y 45. PDO rows B16 at y 60 + 20 i: 8 visible, scrollbar if more (EPR has up to 11 +
  VDM rows).
- The **active PDO** gets a 3 px green bar at x 0 and white text, so it is marked twice (colour + shape).
- PDO type colour stays (FIX `amp`, PPS `watt`, AVS `volt`, VAR `dim`), but the **type word is always
  printed**, so the colour is redundant.
- The footer carries `RX n · PD rev · Src max W · EPR yes/no`. The RX count is diagnostic; it moves out of the
  list.
- **Empty** (`pd_wait`): a T24 `No PD source` title, a B16 two-line instruction that names the port, and a
  cyan `OK: Rescan` action. This replaces the three dim lines "Waiting for PD... / Plug in a PD charger /
  or press Rescan", which had no hierarchy.
- **Stale**: the same list in `stale` grey, the LAST badge, and the footer `Unplugged 0:42 ago. OK: clear`.

### 4.3 Diag and 4.4 Lines (template B)

- Diag: 9 KVRows (key `dim` left, value right-aligned). Each value is coloured only when it is a
  measurement or a state. Today the row text is one free-format string; split it into key/value so the
  values line up.
- Lines: each line is a value plus a **100 px bar over 0..3.3 V**, with a faint tick at 0.4 V (the
  BC1.2/QC threshold region). The pattern (D+ = D- = 0.6 V, i.e. DCP short) is then visible without
  reading digits. The footer gives the decoded verdict (`D lines: DCP short detected`). BRD n/a and VREF
  move to the second block; the `n/a (no sensor)` row is removed. Do not show rows that can never have
  data (H8).

### 4.5 Tools hub (template B, ListRow2)

```
y  22 ▌Fast-charge trigger                 ← selected: surface fill + 3 px focus bar
   43  Source on IN                  READY  ← S12 hint (dim) + readiness tag (ok)
y  62  E-marker
   83  Cable on OUT                 SRC ON  ← caution: works in listen-only mode
y 102  Identify device
y 142  PD trigger
  163  PD charger on IN              NO PD  ← error
y 182  Cable resistance
222 [■ No PD source on IN. Plug one.     ]  ← footer: reason for the SELECTED tool
```

- Five tools in 40 px rows fit without scrolling, so every tool is visible at once (Hick's law and
  recognition over recall).
- The **setup hint is always visible** under the name. It answers "which port, what is connected"
  before the tool is opened.
- The **readiness tag** is computed live from the same inputs the tool's own guard uses (VBUS present,
  Source_Caps valid, live sink, meter online). The tool and the hub therefore cannot disagree.
- **Blocked tools still open.** OK on a blocked tool opens its Refused state, which explains and
  lets the user fix the setup while watching the checklist turn green. A dead OK button would be an
  error dead end (H9).

### 4.6 Tool submenus (template C)

The common rules are in §3. Tool-specific notes:

- **E-marker.** Setup: wiring `none →IN K2 OUT→ CABLE`, with the checklist Cable on OUT / No source
  on IN / VCONN available. With a source on IN, the wiring shows `SRC` in amber, the checklist shows
  that item in red, the OK label changes to `Listen` and the text explains why (`Can't drive VCONN with a source on IN`).
  Result: a **verdict card first** (`5A 50V EPR` in T24 + `Passive USB 3.2 Gen2 <2m` in S12). This is
  the answer the user came for. The raw VID/PID/XID/VDO rows follow and scroll with the -/+ labels
  `Up`/`Down`. The footer shows age and position (`Read 0:05 ago  6 of 14 rows`). The none state
  states the consequence: `Passive cable: 3A max (USB-C spec default)`.
- **Identify.** The result headline is the device model (`iPad Air 5`) when the VDM table knows it,
  else `VID 05AC Apple`. Refused (live sink): amber, overridable, `OK again forces it.`
- **FC trigger.** Two big T24 numbers side by side, `TARGET` and `VBUS NOW`, so the user can confirm
  that the request took effect without reading a sentence. -/+ step the target voltage, and long -/+
  changes the protocol (the footer hint says so). The auto-detect result is a 2 x 4 dot grid
  (ok / error / faint = not tried) with a summary `3 of 8 accepted`.
- **PD trigger.** The cursor row uses the selected-row pattern. Adjust mode (PPS/AVS) opens an
  inline card at y 160 with V and A in T24. The field being edited is underlined in `focus`, and the
  panel labels change to `-0.1`/`+0.1`. A long OK swaps between V and A, and the card overline says so.
- **Cable R.** A two-step recipe (`STEP 1 IDLE <0.20A`, `STEP 2 LOAD >=1.0A`) with each captured sample shown,
  then the result in **D48** (`110 mΩ`) + a grade word (`GOOD`/`OK`/`POOR` coloured `ok`/`caution`/`error`)
  + a consequence line (`Drop 0.33V at 3A = 1.0W lost`). The Kelvin mode is behind `+ Mode`, as today.

## 5 Navigation map

```
                 long +  ─────────────────────────────────────────►
   ┌──────┐   ┌────┐   ┌──────┐   ┌───────┐   ┌───────┐
   │ Home │◄─►│ PD │◄─►│ Diag │◄─►│ Lines │◄─►│ Tools │        (no wrap; the dim -/+ label at the ends)
   │ 1/5  │   │2/5 │   │ 3/5  │   │  4/5  │   │  5/5  │
   └──┬───┘   └─┬──┘   └──────┘   └───────┘   └───┬───┘
      │ long OK │ OK: Rescan (or clear if stale)    │ short -/+ : move selection
      ▼ reset   ▼                                   │ OK: open selected
   energy/time  caps re-read                        ▼
                                  ┌──────────────┬─────────┬──────────┬────────────┬──────────┐
                                  │ FC trigger   │E-marker │ Identify │ PD trigger │ Cable R  │  (modal)
                                  └──────┬───────┴────┬────┴────┬─────┴─────┬──────┴────┬─────┘
                                         │  Setup ─OK─► Running ─► Result ─OK─► Running │
                                         │  Refused ─(fix setup)─► Setup (live, no key) │
                                         └──────────── BACK: returns to Tools, selection kept

   Anywhere: BACK held 1 s → HoldCard countdown; release cancels; 5 s → reboot.
   Anywhere in rotation: short BACK → Home (one press home).
```

Long +/- inside a submenu: tool-specific (FC trigger: protocol; others: nothing). They never change
the page, because BACK is the only way out of a modal (consistency, H4).

## 6 Interaction rules

1. **Short BACK on a rotation page goes to Home.** Today Back is shown on every page but does
   nothing useful there. One press home is the cheapest escape hatch (H3).
2. **The labels show the long-press action when one exists**: `short/long` when it fits, otherwise
   the footer hint carries it (e.g. Home OK `Rst*`, footer `Long OK: reset`). A star on a label means
   "long press" in every panel.
3. **Pressed feedback.** The pressed slot flashes `focus` for 120 ms (well under the 400 ms Doherty
   threshold). A long press passes the 0.5 s threshold, and then the slot stays highlighted until
   release, so the user knows the long action fired.
4. **Destructive or overriding actions take two presses, no dialog**: `OK again` within 3 s (trigger
   above 5 V, Identify over a live sink). The footer counts down `OK again to confirm (3)`.
   Energy reset is undoable for 5 s: the footer shows `Reset. Long OK: undo`.
5. **Reboot hold**: at 1 s the HoldCard appears with a countdown 4..0 and a progress bar. Releasing
   cancels it. This makes a hidden 5 s timer visible, so a long press that is held too long by
   accident does not end in a surprise reboot.
6. **Stale lifecycle**: plug → live. Unplug → stale (badge + grey + footer) and the frozen
   values are kept indefinitely. OK on PD (or long OK on Home) clears them. Replug replaces them. The
   badge age caps at `LAST9:59+`.
7. **No modal pop-ups**, except the HoldCard. All feedback goes to the footer (modeless feedback).
8. **Refresh**: the meter updates at ~10 Hz (current `tick` cadence), but the digits change at
   most at 4 Hz (250 ms display filter) so they can be read. The footer says `avg 1s` when averaging is on.

## 7 Copy rules (UX writing)

- The footer pattern is `[state]. [cause or next action].` with at most 30 chars:
  `No VBUS. Source to IN.`, `Meter not found. See Diag.`, `Ready. OK: Scan`.
- Ports are always named **IN** and **OUT** (never "source side", "port 1" or "C1"). Things on them:
  `source`, `PD charger`, `cable`, `device`, `load`.
- Verbs on OK: `Scan`, `Listen`, `Run`, `Start`/`Stop`, `Send`, `Rescan`, `Open`, `Reset`. One verb per
  action everywhere (the e-marker footer must not say "read" when the label says "Scan").
- No blame: `Source on IN: listen only`, not "Invalid setup". No `!` except in the override prompt.
- Numbers always have units, and the unit has no space (`20.01V`, `3.00A`, `110mΩ`). Durations use
  `m:ss` or `hh:mm:ss`.
- Upper-case words only for overlines and tags (≤ 3 words): `READY`, `NO PD`, `STEP 1`, `TARGET`.
- Drop internal codes from primary lines: "PDO3" becomes the row index `3`, "SOP'" moves into a KV row.

## 8 Critique of the current UI (heuristic pass)

| # | finding | heuristic / law | severity |
|---|---|---|---|
| C1 | The reverse arrow and the LAST badge are both `kWatt` orange, the same colour as the W digits, so a glance cannot tell "reverse" from "power" or "stale" | Similarity, Von Restorff | 3 |
| C2 | Tools hub rows are names only. Which port and what must be connected are visible only after opening, and the reason a tool cannot run appears only after pressing OK | H6 recognition, H9 | 3 |
| C3 | Refusal copy is spread over 3-4 dim S16 lines with no visual difference from a hint (e.g. kSniff, kNone in PdTriggerPage). There is no checklist, and no red/amber cue for what is wrong | H9, visual hierarchy | 3 |
| C4 | Long-press actions (Home Reset, PD trigger V/A) are invisible except via a `lng` label slot that often truncates | H6, hidden modes | 2 |
| C5 | The 5 s BACK reboot has no visible progress, so a long hold reboots by surprise | H1, H5 | 3 |
| C6 | Home has no Ah, no load R and no state line. The TIME/protocol line wastes 22 px on a "TIME" label | H8, density | 2 |
| C7 | Rotation pages other than Home show no live V/A, so the user must page back to Home while reading the PD caps or running a tool | working memory | 2 |
| C8 | Lines rows are free-format strings: values do not align and there is no graphic for the threshold pattern | Prägnanz | 1 |
| C9 | The e-marker result opens on raw rows. The verdict (5A/50V/EPR) is line 1 of 14 and is not emphasised | visual hierarchy, peak-end | 2 |
| C10 | The status bar uses the 16 px font in 22 px, while the content is mostly 16 px too, so the bar does not read as chrome | Common region | 1 |

## 9 Ranked change list (for k2-dev)

Sizes: **S** ≤ ½ day, one file, no new state. **M** ≤ 2 days, new component or state. **L** > 2 days,
several pages or new logic. Ranked by user value per unit of effort; dependencies are noted.

| rank | change | size | files | done when |
|---|---|---|---|---|
| 1 | **Colour tokens**: add `kSurface, kRule, kFaint, kCharge, kCaution, kStale, kFlowRev=0xF81F`; repoint the reverse arrow (`kFlowRev`) and the LAST badge (`kStale` fill, `kBg` text) away from `kWatt` (C1) | S | `ui_style.hpp`, `ui_pages.cpp` (HomePage consts, `title_badge`) | grep finds no `kWatt` outside W and PPS |
| 2 | **Footer component** (y 222..239, dot + S12 message) on every page, fed by a per-page `footer()` returning `{text, level}`; Home footer replaces the TIME/proto line (C4, C6) | M | `ui_pages.hpp/.cpp` (PageBase), `ui.cpp` | every page in `screens.md` shows its footer text |
| 3 | **Tools hub readiness**: ListRow2 (name + setup hint + tag), and a `Readiness ready(ToolCtx&)` per tool that reuses the tool's existing guard; the footer gives the reason for the selected row (C2) | M | `ui_pages.cpp` (ToolsPage, each tool's guard extracted to a pure fn), host unit test for `ready()` | the tag matches the tool's refusal in all 5 x {no VBUS, no PD, live sink, meter off} cases |
| 4 | **Template C setup/refused**: WiringStrip + Checklist components; rewrite E-marker, Identify, FC trigger, PD trigger and Cable R setup/refused rows with them (C3) | L | new `ui_widgets.hpp/.cpp` (or in `ui_pages.cpp`), all 5 tool pages | refused states show a red item + footer reason; copy per §7 |
| 5 | **Reboot HoldCard**: countdown from 1 s, cancel on release (C5) | S | `ui.cpp` (global BACK handler), `ui_events` hold timing | holding Back shows 4..0; releasing at 3 leaves the page intact |
| 6 | **Status bar to S12 18 px + MiniReadout** on all non-Home pages (C7, C10) | M | `PageBase::title_bar/title_badge`, Home bar fields `kHomeBar` | PD/Tools/submenus show live V/A/→; body starts at y 20 |
| 7 | **Home re-grid**: rows y 22/66/110, rule at 152, Wh/Ah/t/R grid, S12 D-line + FC row; idle (`faint` zeros) vs error (`--.--`) | M | `HomePage` in `ui_pages.cpp`; Ah from `meter` (add `mah()` if missing) | matches `home_*.png` within ±1 px |
| 8 | **Stale everywhere it applies**: Home digits/Wh to `kStale` + badge + footer; PD footer `OK: clear`; age counter `m:ss`, capped | S | `HomePage::update`, `PdPage`, `title_badge` | after unplug, Home and PD both show LAST and grey values |
| 9 | **Short BACK → Home** on rotation pages; `*` long-press marker in labels | S | `ui.cpp` global handler, `ui_labels.cpp` | from any page, one short Back lands on Home |
| 10 | **E-marker verdict card** (T24 summary + S12 line) before the raw rows; `n of N rows` in the footer (C9) | S | `EmarkerPage::row/draw_static` | verdict visible without scrolling |
| 11 | **PD contract strip + active marker** (3 px bar), RX/rev/EPR moved to the footer, T24 empty state | S | `PdPage` | `pd_live/pd_wait.png` |
| 12 | **Lines bars** (0..3.3 V, 0.4 V tick) and KV alignment; drop the BRD `n/a` row; verdict in the footer (C8) | M | `LinesPage`, `lines_row_text` | bars track D+/D- in a QC handshake |
| 13 | **Cable R result in D48 + grade word + loss line** | S | `CableRPage` | `cabler_result.png` |
| 14 | **Pressed-slot flash and long-press latch** on the panel | S | `ui.cpp` panel draw | 120 ms flash visible in a screenshot burst |
| 15 | **Display filter 250 ms** for the D48 digits (the meter still samples at 10 Hz) | S | `HomePage::update` | digits change ≤ 4 times/s |
| 16 | **Undo for energy reset** (5 s) and `OK again (3)` countdown for overrides | M | `HomePage`, `PdTriggerPage`, `TriggerPage`, `IdentifyPage` | |

Suggested order: 1 → 2 → 6 (chrome first, because every later screen depends on it) → 3 → 4 → 7/8 → the rest.
Items 1, 5, 9, 10, 11 and 13 are independent S jobs and can run in parallel (max 4 agents).

## 10 Open questions (verify before implementing)

1. **Port names.** The design assumes the two connectors are called IN (source side) and OUT (sink
   side), and that "forward" means IN → OUT. Confirm against the enclosure markings. If the case is
   labelled differently, use its words.
2. **Slot geometry.** The real button centres are still unmeasured (`kSlotLenPx` comment). The mockups
   use the current 66/54/54/66.
3. **Ah and load R sources.** Is there an Ah accumulator in `meter`? Load R needs a current floor (show
   `--Ω` below 50 mA).
4. **Glyphs.** `≥` and `Ω` are missing from the S12 font. The copy avoids them in S12 (`>=`, and Ω only in
   B16/T24). Adding `Ω ← →` to the 12 px set in `tools/gen_font.py` would allow an arrow in S12
   footers (~200 B flash).
5. **Flash budget.** New widgets are code only. No new fonts are required.
