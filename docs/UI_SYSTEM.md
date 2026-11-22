# K2 UI system

The design language of the K2 screen, measured from the charger test.

- **Reference.** The charger-test flow is the language every page speaks. The user named it the reference.
- **Sources.** Geometry and colours come from the code: `include/k2/ui/kit.hpp`, `src/ui/kit.cpp`,
  `include/k2/ui/ui_style.hpp`, `include/k2/ui/ui_widgets.hpp`, `src/ui/ui_charger_test.cpp`.
- **Renders.** `runs/ui-all/png/*.png`, 2x scale. Names in this file are files in that folder.
- **Where a render and the code disagree, the code wins.** Section 8 lists every disagreement found.
- **Related.** `docs/UI_DESIGN.md` holds the hardware facts, the navigation map and the input language.
  The skill `k2-micro-gui` holds the work loop and the paint budgets.

All coordinates are logical content pixels: x 0..207 on twin-rail pages and x 0..223 on Home.

## 0 Fieldbook (2026-11-12)

The Fieldbook rework (concept `runs/ui-rework-fable/CONCEPT.md`, branch `k2/fieldbook`) replaced the paging hub with a
Meter and six places (seven since the Battery place, 2026-11-13); the Bench list itself became the ring of 0.2 on 2026-11-20. Where this section and a later one differ, this section is current; the later sections
keep the measured geometry of the parts that did not change.

### 0.1 Theme

Classic is the only palette (user decision 2026-11-13: Night and Paper are gone, there is no Theme row). It is one
`constexpr Palette` in `include/k2/ui/theme.hpp`; every token of `ui_style.hpp` is a compile-time constant read from it,
so the old palette tables, the boot-time copy and the Settings reboot prompt are gone. The theme bits in the stored
settings (byte 6, bits 4..5) are written 0 and ignored on read, so a record that names Night or Paper loads as Classic.
Classic keeps cyan for amps and focus.

Flow trends are the pre-Fieldbook drawing (restored 2026-11-13 from `3c59f9e1`, `paint_qty` in `src/ui/ui_home.cpp`; the
trend painter `src/ui/home_cell.cpp` never changed): a black field, the quantity's colour for its digits, unit and status
edge, a dark tint column (`kVoltT`, `kAmpT`, `kWattT`) under a brighter 1 px level line (`kVoltL`, `kAmpL`, `kWattL`), and a
1 px `kRule` cursor gap. Stale is the grey pair (`kPanelBg` tint, `kFaint` line); idle draws no trend. Over the limit is
white on `kError` with a `kErrD` bar track, near is the orange chip with orange digits and bar. `tests/test_ui_flow_trend.cpp`
renders the charge, ramp, idle, LAST and unplugged scenes, asserts the line and tint pixels (and their absence after LAST
and unplugged) and the digit/tint contrast (WCAG 2.x); `K2_DUMP=dir` writes the Flow scenes for `tools/panel2png.py`.

### 0.2 Navigation (ring, 2026-11-22)

One carousel of ten pages (Home through K2, with the unified 3-slice `6 LINK` page and dedicated `7 TOOLS` menu). `include/k2/ui/ui_ring.hpp` is the pure model (`tests/test_ui_ring.cpp`); `Ui::ring_*` wires it.

```
Home > Lines > Numbers > Graph > Ripple > Link > Tools > Battery > Log > K2 > (Home)
```

| state | PLUS / MINUS | OK | short Back | long Back |
|---|---|---|---|---|
| Ring view (the page is shown, the masthead carries its numbered name) | previous / next page, wrapping | enter the page (Lines and Numbers only look: no entry) | Home | the page chosen in Settings > Long Back |
| Entered (the page's own rows and actions) | scroll the rows (or step Charger / Cable / Sink slice on Link) | the focused slice / row's action | up one level; at the top level back to the ring view | the ring view on the same page |

- Boot lands on Home in the ring view. `Ui::goto_page`, the remote `page N`, `open_connection`, `open_place` and a started
  charger test land entered (the page was asked for by name).
- Home entered keeps Home 2c: PLUS / MINUS move the hero, OK cycles the span, long OK resets the energy, Back exits to the ring.
  Graph entered: OK = Zoom, PLUS / MINUS blank.
- `6 LINK` is `ConnectionPage` in 3-slice Overview mode (`View::Overview`): full-height `208 x 238` (no permanent bottom footer),
  three equal bands (`CHARGER` 79 px, `CABLE` 79 px, `SINK` 80 px) showing consistent `W` heroes alongside `V`/`A` when known
  (including live debounced `W`/`V`/`A` on inline links when K2 attached mid-session without sniffing PD caps or Requests),
  debounced port sense, provenance (`PROBED`, `PEAK`, `SNIFFED`, `LIVE`, `3A?`), cable resistance (`mΩ`), and bottleneck
  highlights. Entering (`OK`) enables Direct Slice Focus (`-/+` steps across the three slices, `OK` opens that slice's
  `View::Actions` page).
- Slice Actions (`View::Actions`): compact slice header (`y 2..44`), pinned top `Reset & re-read` section (`Act::ResetLink`,
  separated by a 2 px `RowSpec::section_before` rule — merges `Read again` on Charger; inline links including mid-session
  links arm a 2-press Hard Reset confirm refused when bus-powered; standalone K2-owned contracts send sink Hard Reset when
  host-powered; other standalone/detached states run rescan + survey), followed by the slice's own interrogation & tool entries
  (including `Spec & offers` / `Chip details` / `Sink details` opening the full scrollable `View::Spec` sheet).
- `7 TOOLS` is `ToolsPage` (`Menu::Tools`, appended right before `Menu::Count` to preserve wire page IDs): full-height `kit::RowList`
  putting `Request voltage` (`Menu::PdTrigger`) and `Fast charge` (`Menu::Trigger`) at the top (moved out of `6 LINK` Charger
  Actions), followed by `Cable R`, `Spoof cable`, `Report cards`, `Black Box`, and opt-in bench tools with live readiness tags/notes.
- Passive offer stability (`pd::HeldOffers::capture_settled`): 800 ms quiet-wire settle gate + high-water-mark protection prevents
  rapid multi-port renegotiation flicker and stops transient low fallback offers (e.g. 15 W / 60 W) from overwriting a known peak.
- Battery is `BatteryPage`, Log is `DiagnosisPage::as_place`, K2 is `SettingsPage`. Pages with no actions (Battery, Log, K2, Meter
  faces) are scroll-only when entered.
- A tool page opened from a place or `7 TOOLS` pops back to that page; Back from a tool over Lines exits to the
  owner's ring page (`Menu::Trigger` and `Menu::PdTrigger` belong to `Pos::Tools`). A ring walk skips a target while `ports_busy()`
  refuses a menu target (charger test, release guard, twin, PD stress, PD spec, loop probe).
- ChargerTest: Back goes up to the Charger slice's Actions; long Back stops the run and shows the Link ring page.
- Long Back target: `/prefs.cfg` optional 9th byte (written only when non-zero and below 10; older firmware reads a 9-byte
  file as defaults). Settings row `Long Back` cycles the ten positions; Home is the default.
- Long PLUS is not used. The OK + Back 3 s reboot chord and the Back-Hold cancel are unchanged.
- Ring vs entered is visible: the masthead (slot 0 of the opposite rail) shows the position and name (`6 LINK`, `7 TOOLS`) in the
  ring view and the page's own title (`LINK`, `CHARGER`, `CABLE`, `DEVICE`, `TOOLS`) when entered. Session flag `RingView` (bit 6)
  is reported by GET_STATE (`ring_view`).
- Remote page names: `page N` is 0..6 for Home, Detail, ChargerTest, Lines, Settings, Graphs, Ripple; a showing submenu is 7 + its
  `Menu` index (`Menu::Tools` = index 20 -> page 27, `"tools/tools"`). Menu page names keep the `tools/` prefix.

### 0.3 Place page (Charger, Cable, Device)

| part | geometry |
|---|---|
| Identity | y22..67, fixed: headline font 24 at (8, 24), chips (14 px, font 12) beside it while they fit before x216, one line font 12 at y52, a dashed line (4 on, 2 off) at y66 |
| List | from y68, 7 slots of 20 px; whole-row scroll; a 4 px thumb lane at x220..223 |
| Fact rows | the tool's own rows (offers, contract, e-marker fields, request) in ground, no hairlines |
| Perforation | a dashed line between the fact rows and the actions, only when fact rows precede them |
| Action rows | text x8; focus = inverse fill (Classic: the 3 px bar); a blocked tool `kInk3` with its tail word right-aligned to x216; a drill chevron when ready |
| Keys | reading mode (no row focused): Dn/Up scroll by 6 rows, Dn takes the first action once it is on screen; OK runs the place's own read. With a row focused: Up/Dn move, Up past the first row lets go |
| Fact line | the focused action's blocker in its tone, or its hint |

The empty state is the identity in dim (`No charger`, its line) over the action rows. No Details page: the fact table is
on the place page.

### 0.4 Log place and K2 place

- **Log**: the PD diagnosis list under a 48 px stamp (y22..69): the verdict's fill (fail red, warn orange, a healthy link
  green, else `kSurface`), its 12 px glyph in a 20 px cell at x4, the word (`screen::word`, at most 16 chars) in font 24
  from x30. No repetition: when the word is the top finding's label (`screen::names_top_finding`) the list starts at the
  second finding, and that finding's evidence note is the stamp's sub-line (font 12, y+31, from x30; its time is not
  shown); with no finding to name the word is centred and the list holds every row. The list follows from y72; the last
  row is `Black Box` (chevron, note `N captures kept`), the only row OK opens.
- Status words that start a phrase are sentence case (`None on either port`, `Not read`, `No session, 3 captures`);
  units and values keep their case (`mV`, `5.27V`).
- **K2**: the version in font 16 at (8, 25), `up 1h05m  USB HID+WebUSB` (this boot's mode, `usbcfg::describe`) in font 12
  at y46, a dashed line at y63, the settings list from y66 (the USB Link row's note is `usbcfg::link_note`: `HID+WebUSB
  active`, `USB link off` or `HID+Drive after a replug`, the truth the USB Link page's footer states), then `System state` (the Diag page) and `Boot info` (this boot's K2B1 record shared as QR codes).
- A RowList that starts below an identity block or a stamp ends above y220; the page fills that gap once
  (`kit::fill_list_gap`).

### 0.4a Battery place

The battery efficiency recorder (`docs/BATTERY_RECORDER.md`), `BatteryPage` (`src/ui/ui_battery.cpp`), its texts and rows
pure in `k2/ui/battery_view.hpp` (`tests/test_ui_battery.cpp`).

| part | geometry and copy |
|---|---|
| Identity | the place identity block (`kit::Ident`, `kit::paint_identity`, 0.3): `No run` dim with `Charge, discharge or round trip`; `Charging` / `Discharging` with a `1 of 2` / `2 of 2` chip on a round trip and `Same port, 1h05m12s, 5.431Wh in`; `Charged` + `1 of 2` and the swap instruction between the phases; done: the efficiency (`80.0%`, Ok chip `Round trip`) or the Wh (`Charged` / `Discharged` chip, outline `Cut short` after a power loss) and `Run 7, 2h01m, Same port` |
| List | kit RowList from y68 (`kit::kPlaceListY`): the facts first (In / Out in `kEnergy` with the mAh in the note, Time with the gaps or the split in the note, Efficiency with "so far" while the discharge runs), then the actions. Idle or done: Start charge, Start discharge, Start round trip, Wiring (`Same port` / `Moved`), End below (`50mA`), End after (`60s`, `2m`..), Saved runs (chevron), Share as QR when done. A run in progress: End charge / End discharge, Finish run (a round trip's charge), Start discharge (between), End below, End after |
| Saved runs | identity `Saved runs` / `Newest first, OK opens one`; rows `#7 Round trip` with the efficiency, the Wh or `cut` as tail and `6.115Wh in, 5.001Wh out, 19m` as note; OK opens the run (its identity, facts and Share as QR); BACK returns to the list, then to the main view |
| Keys | Up/Dn move; OK runs the row (strip verb Start, Stop, Toggle, Cycle, Open, Share; View in Saved runs); End and Finish arm first (row text `kCaution`, strip `Confirm`, Caution footer `OK again ends the charge`) and run on a second OK within 5 s; moving off disarms |
| Footer | Info `Nothing measuring` / `Ends under 50mA for 60s` / `Discharge starts by itself`; Ok `Round trip 80.0% efficient` / `Run 7 saved`; Caution `Run not saved to flash`, `Phase cut by a power loss`; Error `No power monitor`; a refusal (`Last run not saved yet`) for 2.5 s |
| Running dot | while a run measures or waits: a 5 x 5 `kFocus` square at content (219, 0) on every page but a QR code (`Ui::draw_release_notice`); the page repaints whole once when the run stops |
| Bench row | `No run`, `Charging 1h05m, 12.34Wh`, `Charged 12.34Wh, waiting`, `Round trip 80.0%`, `Discharged 9.87Wh`, `Cut short, 12.34Wh`; battery icon |

### 0.5 Changed parts

| part | Fieldbook |
|---|---|
| Running verdict cell | 2 px `kAccent` outline along the cell's edge, no dot (5.9) |
| Reboot HoldCard | square orange (`kCaution`) card x10..213, y70..153, 2 px `kInk` frame, `Reboot in N` font 24 and `release to cancel` font 12 in `kSpotInk`, a 10 px `kSpotInk` bar filling along the bottom |
| Chips | `kit::draw_chip` (solid) and `kit::draw_chip_frame` (1 px outline), width `kit::chip_w` = 6 px a char + 8 |
| Dashed line | `kit::draw_dashed` (4 on, 2 off) |

### 0.6 Not done yet

- QR page tiers (concept 6.1: framed up to a 180 px box, cleared up to 224, bare above). Every code is still the
  full-panel bare page.
- Empty-state words are still upper case in the tabbed Who's attached page (`include/k2/ui/text/connection.hpp`); the place
  pages show the same words.

## 1 Principles

1. **One language.** A page states what it shows. The kit draws it. No page draws its own variant of a kit part.
2. **Edge to edge.** Full-width rows, 1 px hairlines, 2 px rules between sections. No cards, no shadows, no
   rounded boxes. The only rounded shape is a tail pill (the reboot HoldCard is square since Fieldbook, 0.5).
3. **One line, and it fits.** Text is never cut and never wraps. A row that needs more opens a second line.
4. **Colour is an exception.** Unit colours name quantities. Amber and red mean an abnormal state. Green means a
   state that is good. Every state also has a shape or a word.
5. **One thing per key.** OK does the one verb on the strip. PLUS and MINUS move or change. BACK retreats, and it
   stops a live run first.
6. **The footer states one fact.** It never names a key.
7. **A number has a fixed place.** An absent value is a dim `--` in the same box.
8. **Paint only what changed.** There is no framebuffer. Every part has a signature and repaints on a change only.

## 2 Grid

### 2.1 Canvas

| item | value | source |
|---|---|---|
| Panel | 240 x 240 | `kScreenW`, `kScreenH` |
| Standard content | 208 x 240 portrait / 240 x 208 landscape | `ContentViewport`, `kTwinContentW`, `kTwinContentH` |
| Home content | 224 x 240 portrait / 240 x 224 landscape | single-strip Home |
| Button strip | 16 px thick, on the button edge | `kPanelThickness` |
| Opposite rail | 16 px on the opposing edge | `OppRail` |
| Strip slots | 70, 58, 58, 54 px (mirrored on Left/Bottom) | `kSlotLens` |
| Portrait (rotation 0, 2) | content drawn 1:1 between rails | |
| Landscape (rotation 1, 3) | content mapped between top/bottom rails | glyphs and markers keep their native size |

Design standard pages in 208 x 240. Home keeps 224 x 240. Check all four rotations. A 1 px line can vanish in landscape, so the kit draws
markers at native size (`draw_marker`).

### 2.2 Vertical bands

| y | height | band | owner |
|---|---|---|---|
| 0..1 | 2 | content margin | `kBodyY` = 2 |
| 2..239 | 238 | full-height page body | `kFooterY` = 240 |
| floating near bottom | 16 | transient action/error toast, 4 px from the edge | `draw_toast` |

There is no permanent horizontal TopBar or Footer. Page names, wizard dots and live telemetry live on `OppRail`.

### 2.3 Columns

| x | use | source |
|---|---|---|
| 0..2 | focus bar, 3 px `kFocus` | `draw_row_cb` |
| 4 | title, footer text, InfoBlock tag | `kFooterTextX`, `kInfoTagX` |
| 6 | key of a KvTable, body text, hero overline, bars | `KvLayout::label_x` |
| 8..19 | row marker, 12 x 12 | `kMarkX`, `kMarkSize` |
| 28 | row text and row note | `kTextX` |
| 52 | InfoBlock value | `kInfoValueX` |
| 60 | ResultGrid rule; labels end at x55 | `kResultLabelW` |
| 72 | KvTable value, short keys | `kKvValueX` |
| 92 | KvTable value, long keys | Step detail `kDet` |
| 104 | dim second column of a row | `kCol2X` |
| 112 | centre rule of a two-column row | Waiting |
| 198 | right edge of a row tail, count or chevron | `kTailX` |
| 202 | right edge of right-aligned body text and note box | `kNoteMaxPx` = 174 from x28 |
| 205..207 | scroll lane, 3 px | `ListView::kBarW` |

Gaps: 8 px between a row text and its tail (`kTailGap`; 6 px before a dim tail, so a 5-char second-column value fits beside a margin tail). 4 px before the second column.

### 2.4 Row heights

| row | height | text top | rule |
|---|---|---|---|
| List row, KvTable row | 26 | y+6 (font 16) | 1 px `kRule` on the top line of the row |
| Open list row | 40 | y+6, note at y+24 (font 12) | same |
| InfoBlock row | 20 | y+2 (font 16) | none |
| ResultGrid row, volts only | 26 | centred in 25 px | last line `kBg` |
| ResultGrid row with currents | 40 | 24 px cell, bracket text at y+25 | last line `kBg` |
| Section rule | 2 | | `kRule`, counted in the row that follows |
| AlertBand, verdict band | 36 | word at y+6 (font 24) | starts at y22 |
| ActionCell | 64 | centred | y156..219 |

`row_text_y(pitch, font_h)` = 1 + (pitch - font_h) / 2 gives the text top inside any row.

### 2.5 TopBar geometry

| part | place | style |
|---|---|---|
| Band | y0..19, full width | `kBar` |
| Icon (menu pages only) | x4, y4, 12 x 12 | `kDim` |
| Title | x4 (x20 behind an icon), y4 | font 12, `kText` |
| Wizard step | `n/3 ` in front of the title | same run of text |
| Role badge (`PAS`, `SNK`, `Rd pin CC2`) | right-aligned to x132, y4 | font 12, `kDim`, behind a 1 px `kFaint` rule 5 px to its left |
| Flow chevron | x140..149 | `kFlow`, only while current flows |
| Volts | x152, 5 characters | font 12, `kVolt` |
| Multiplication sign | x184 | font 12, `kDim` |
| Amps | x190, 5 characters, ends at x219 | font 12, `kAmp` |
| `LAST` pill (stale) | right-aligned to x222, y2, 14 px tall | `kStale` fill, black text, replaces the readout |
| Bottom edge | y20 | 1 px `kRule`, or the progress rule |
| Progress rule | y20..21, three segments of 74 px, 1 px gaps (x0..73, x75..148, x150..223) | done and current `kFocus`, to do `kRule` |

Title width: 101 px (16 characters) with a `PAS` or `SNK` badge and no icon. 85 px (14 characters) behind an
icon. A value too wide for its 5-character slot reads `OVR`.

Home has no TopBar and no Footer.

### 2.6 Footer geometry

| part | place | style |
|---|---|---|
| Band | y222..239, full width | fill by tone, section 4.3 |
| Text | x4, y223 | font 16, at most 27 characters (x4..219), `kFooterCells` |

The band is composed in one row buffer and sent once. It repaints only when the tone or the text changes
(`draw_footer_cached`): 224 x 18 = 4032 px.

### 2.7 Strip geometry

| part | value |
|---|---|
| Thickness | 16 px, fill `kPanelBg` |
| Slots | 66, 54, 54, 66 px. Rotation 0, top to bottom: BACK, MINUS, OK, PLUS |
| Divider | 2 px `kPanelLine` on the leading edge of slots 2..4 |
| Label | font 12, centred between a 4 px pad at both ends |
| Room | 9 characters in a 66 px slot, 7 in a 54 px slot |
| Pressed | the slot turns `kPanelHi` for at least 120 ms (`kPressFlashMs`); a long press stays lit until release |
| Long press | `short/long` when both fit, else the short word. A long-only action ends in `*` |

## 3 Type

All fonts are Atkinson Hyperlegible Mono, 4 bpp, monospaced.

| size | name in code | advance | line height | glyphs | use |
|---|---|---|---|---|---|
| 12 | `kLabelFont` (`font_12`) | 6 | 12 | ASCII + arrows | TopBar, strip, row note, overline, grid labels and brackets, hints. Never a primary value |
| 16 | `kSmall` (`font_16`) | 8 | 16 | ASCII + arrows | rows, keys and values, footer, body lines, cell labels in cells 22 px and taller |
| 24 | `kMid` (`font_24`) | 12 | 24 | ASCII + arrows | alert word, verdict word, action verb, hero unit, close-up primary values |
| 48 | `kBig` (`font_48`) | 24 | 30 | `0-9 . - space` | walk-by value: VBUS while waiting and running, Home ledger |
| 64 | `kHero` (`font_64`) | 32 | 39 | digits | Home hero only |

Rules:
- At most three sizes on a screen, plus one hero.
- A unit sits on its value's baseline (`top_for_baseline`), 3 px behind a hero.
- Stacked numbers align on the decimal point.
- Viewing levels: Home, Waiting and Running are walk-by, values at least 48 px. Every other page is close-up:
  primary value at least 24 px, body 16 px.
- A new size goes through `tools/gen_font.py`, digits only where possible.

## 4 Colour

### 4.1 Tokens (`include/k2/ui/ui_style.hpp`)

| token | RGB565 | tint | use |
|---|---|---|---|
| `kBg` | 0x0000 | | ground |
| `kBar` | 0x08A9 | | TopBar, Info footer |
| `kRule` | 0x2945 | | hairlines, section rules, bar and gauge tracks |
| `kSurface` | 0x18E3 | | band behind a verdict with no fill (Step detail), `NO VBUS` pill |
| `kSurface2` | 0x2124 | | HoldCard overlay |
| `kText` | 0xFFFF | | values, row text |
| `kDim` | 0x8410 | `kDimT` 0x2104 | keys, notes, tails, placeholders in a list |
| `kFaint` | 0x4208 | `kFaintT` 0x1082 | "no data", pending cells, zero hero |
| `kVolt` | 0xFFE0 yellow | `kVoltT` 0x2120 | volts |
| `kAmp` | 0x07FF cyan | `kAmpT` 0x0124 | amps |
| `kWatt` | 0xFB2C coral | `kWattT` 0x3083 | watts. Never a warning colour |
| `kEnergy` | 0x07E0 green | `kEnergyT` 0x0120 | Wh |
| `kCharge` | 0x5D1F blue | `kChargeT` 0x0866 | Ah |
| `kOhm` | 0xB5B6 silver | | ohms |
| `kDPlus` / `kDMinus` | 0xFB20 / 0x2D7F | 0x3080 / 0x0108 | D+ and D- tags |
| `kFlow` | 0x07E0 | | flow chevron, one colour for both directions |
| `kOk` | 0x07E0 green | `kOkT` 0x0120 | pass, good state |
| `kCaution` | 0xFC00 amber | `kCautionT` 0x2900 | abnormal, look |
| `kError` | 0xF800 red | `kErrorT` 0x3000 | failure, blocked |
| `kStale` | 0xC618 | `kStaleT` 0x2104 | frozen values, LAST pill |
| `kFocus` | 0x07FF cyan | `kFocusT` 0x0146 | focus bar and fill, picked marker, progress, running dot |
| `kPanelBg` / `kPanelHi` / `kPanelLine` | 0x2104 / 0x07FF / 0x8410 | | strip ground, pressed slot, divider |

### 4.2 Rules

| rule | detail |
|---|---|
| Unit colours are information | A value takes its unit's colour where the unit matters: `5.00V` in `kVolt` in a Steps tail, Step detail and the hero |
| Exception colour | Amber and red appear only for an abnormal state. A normal value is never amber or red |
| Green is a state | Pass, ok, active. Never a label or a tag |
| Redundant coding | A verdict is a fill plus a word or a shape. An alert is a fill, a triangle and a word. A picked marker is solid, an unpicked one hollow |
| Text levels | `kText` value, `kDim` key or note, `kFaint` no data |
| Focus | `kFocusT` fill on the row and a 3 px `kFocus` bar at x0..2 |
| A never-offered mode | no fill, `kDim` ink. It is never red |
| Stale | grey everywhere plus the `LAST` pill |

### 4.3 Footer tones (`footer_style`)

| tone | fill | text | use | seen in |
|---|---|---|---|---|
| Info | `kBar` | `kText` | a neutral fact, a count, progress | `b_ct_algorithms.png`, `b_ct_running.png` |
| Ok | `kOkT` | `kOk` | a good or active state | `mock_loop_done.png` |
| Caution | `kCautionT` | `kCaution` | the user should look; an armed confirm | `b_ct_confirm.png`, `b_ct_steps.png` |
| Error | `kErrorT` | `kError` | a failure or a blocked action | `mock_tools.png` |

An instruction, an empty list and a state in progress are Info, never Ok.

## 5 Components

One implementation each, in `k2::kit`. Painters are stateless. Caches are small value types.

### 5.1 TopBar: `kit::draw_top_bar`

| | |
|---|---|
| Purpose | Names the page and the wizard step. Carries the live V x A on every page except Home |
| Geometry | section 2.5 |
| States | plain (hairline); wizard step 1..3 (progress rule); stale (`LAST` pill) |
| Repaints | the left band when the title or step changes; the readout area x140..223 on its own signature |
| Seen in | `b_ct_algorithms.png` (1/3), `b_ct_cable.png` (2/3), `b_ct_review.png` (3/3), `b_ct_running.png` (plain) |

Charger-test titles: `1/3 Tests`, `1/3 Custom`, `1/3 Entries`, `2/3 Spoof cable`, `3/3 Review`, `Charger test`
(warning, waiting, running), `Results`, `<unit> steps`, `Step detail`, `Changes`.

### 5.2 Footer: `draw_footer_cached`

| | |
|---|---|
| Purpose | One fact about the screen: what is picked, how far the run is, what failed |
| Geometry | section 2.6 |
| States | Info, Ok, Caution, Error |
| Repaints | the whole band, only when tone or text changes |
| Seen in | every charger-test render |

A short message may take the footer for 2.5 s and then give it back (a refusal, a result).

### 5.3 Strip: `PanelLabels`

| | |
|---|---|
| Purpose | Says what each of the four keys does now |
| Geometry | section 2.7 |
| States | idle, pressed, dim (shown but unavailable), blank (the key does nothing) |
| Repaints | a slot when its text changes; the pressed slot on press and release |
| Seen in | `b_ct_algorithms.png` (`Back`, `Up/Prev`, `Pick`, `Dn/Next`), `b_ct_review.png` (`Start`), `b_ct_confirm.png` (`Confirm`) |

Vocabulary: section 6.2.

### 5.4 RowList: `kit::RowList` (one instance, `ctx_.rows`)

| | |
|---|---|
| Purpose | Every list: choices, settings, steps, actions |
| Row | 26 px. Rule on the top line. Ground from y+1 |
| Marker | 12 x 12 at x8, y+7 |
| Text | font 16 at x28, y+6 |
| Second column | font 16, `kDim`, at x104 |
| Tail | font 16, right-aligned to x214: a value, a count, a 6 x 9 drill chevron, or a 14 px pill in font 12 |
| Note | font 12, `kDim`, at x28, y+24, at most 190 px (31 characters). Only on the focused row |
| Open row | 40 px, only when the focused row has a note |
| Scroll thumb | 3 px at x221..223, `kDim` thumb on a 0x2104 track, only when the list overflows; eases 160 ms |
| Window | (222 - y - 14) / 26 rows; 7 at y24 |

Markers (`kit::Marker`):

| marker | off | on | other |
|---|---|---|---|
| `Radio` (pick one) | hollow 1 px frame, `kDim` | solid 12 x 12, `kFocus` | no circles on this panel |
| `Box3` (group) | frame | frame + 6 x 6 block | part: frame + 6 x 2 bar |
| `Check` | frame | frame + check | |
| `Verdict` | | a label-less VerdictCell, section 5.9 | |
| `Icon` | 12 x 12 icon, `kDim` | `kFocus` on the focused row | a tone colour when the page sets one |
| `None` | | | the text stays at x28 |

Every marker state rewrites its whole 12 x 12 box.

States of a row:

| state | look |
|---|---|
| idle | `kBg` ground |
| focus | `kFocusT` ground from x3, `kFocus` bar x0..2, opens to 40 px when it has a note |
| picked | marker solid; independent of focus |
| caution choice | text in `kCaution` (`Start anyway`) |
| data | the text is external (a saved name): fitted with `fit_data`, counted, not failed |
| yielding tail | a hint tail is dropped when the name would not fit beside it; the name then runs to the scroll lane (24 characters) |

Repaints: a row repaints when its signature changes (marker, state, colours, texts, focus). A focus move
repaints two rows. A change of the open height re-lays the rows below it.

The Tests step's preset tails count what the run would make at the selected Depth (`ct::planned_tests`: runnable tests plus spec checks;
the real plan with caps, `ct::nominal_caps()` without), cached by input key. The Charger and Cable tabs keep their last read over sleep
and detach (same charger, same cable) and, empty, say `No offers read` / `Tests: Light, PD` and `Not read` / `Actions: Read e-marker`.

Seen in: `b_ct_algorithms.png` (radio, count tail, note, chevron), `b_ct_cable.png` (second column),
`b_ct_steps.png` (verdict marker, volt tail, thumb), `ra_guard_refuse.png` (choices, no marker).

### 5.5 KvTable: `kit::kv_static`, `kv_cells`, `kv_row`, `kv_row_sel`

| | |
|---|---|
| Purpose | A review: fixed keys, one value each |
| Row | 26 px, rule on the top line, one more rule closing the table |
| Key | font 16, `kDim`, x6 |
| Value | font 16, left edge at a page-fixed column: x72 (18 characters) or x92 (16 characters) |
| Value colour | `kText`; the unit colour for a measured value; `kCaution` for the abnormal row; `kDim` or `kFaint` for `--` |
| Settable row | `kv_row_sel`: a row the cursor can stand on. Focused, it takes the RowList focus look (`kFocusFill` ground, the 3 px `kFocusBar` when the theme has one, else the on-focus inks); OK changes its value in place, the strip says `Change` and the footer explains the value. A choice of a few words is a SegmentedControl instead (below) |
| Repaints | one row (224 x 25) when its key, value, colour or focus changes |
| Seen in | `b_ct_review.png` (x72: Tests, Spoof; the Level choice under them is a SegmentedControl since 2026-11-16), `b_ct_detail.png` (x92: Target, Measured, Window, Reason, Header or Mode, Scope) |

**SegmentedControl: `kit::draw_segmented`** (user decision 2026-11-16: the Review step's Level must obviously be a control).

| | |
|---|---|
| Purpose | A pick of 2 to 4 short words that is plainly a control (the charger test's Level: Light, Medium, Deep) |
| Block | `kit::kSegH` = 34 px, full width: a label row (`kSegLabelH` 14 px, font 12, `kInk2`, x6) and a segment bar (`kSegBarH` 20 px) |
| Segments | equal width across x6..w-6, 2 px apart (`kit::seg_span`); the word centred in font 16 |
| Picked | solid `kFocus`, dark `kSpotInk` word |
| Others | 2 px frame (`kRule`; `kFocus` when the control holds the cursor), normal ink; the frame is painted after the word so a landscape mapping that loses rows never opens it |
| Focused | the whole block takes the RowList focus look (`kFocusFill` ground, the 3 px `kFocusBar`); OK steps to the next segment and wraps, the footer says what the pick does |
| Repaints | the block (224 x 34) when the pick or the focus changes; the page caches it by signature |
| Seen in | the charger test's Review (`runs/ui-sweep-hero/review-*.png`); Settings > Test level is still a list row |

### 5.6 ActionCell: `kit::ActionCell`

| | |
|---|---|
| Purpose | The one thing OK does on a review page |
| Geometry | full width, y156, 64 px (y156..219); the charger test's Review uses `init(c, 172, 48)` (y172..219) under its warning. A 1 px `kRule` at y154 closes the table above |
| Idle | `kFocusT` fill, 2 px `kFocus` outline, verb in font 24 `kText` centred. Optional hint under it: 11 px `kCaution` triangle + font 12 `kCaution` text |
| Idle, not focused | when the cursor stands on a settable KvTable row instead (`draw_idle(..., focused = false)`): `kBg` fill, 2 px `kRule` outline, verb and hint in `kInk2`, so the focus is never on two things at once |
| Armed | `kCaution` fill, black word `CONFIRM` in font 24, black font-12 line `closes in 5s`, 4 px black bar along the bottom that drains to the left over 5 s (`Ui::kConfirmWindowMs`) |
| Repaints | the whole cell on a state or text change; the bar repaints only the columns it leaves |
| Seen in | `b_ct_review.png` (idle, hint `other port must be empty`), `b_ct_confirm.png` (armed) |

### 5.7 AlertBand: `kit::draw_alert_band`

| | |
|---|---|
| Purpose | Stops the flow for something the user must decide |
| Geometry | full width, y22, 36 px (`kAlertH`). 20 px black triangle at x8, y+8. Word in font 24, black, at x36, y+6 |
| Fill | `kCaution`, or `kError` for an error |
| Below | lines in font 16, `kText`, at x6, 20 px apart (y64, y84). Then a RowList of choices (y110) |
| Repaints | static |
| Seen in | `ra_guard_refuse.png` |

### 5.8 Verdict band (Step detail)

| | |
|---|---|
| Purpose | The verdict of the one item a detail page shows |
| Geometry | full width, y22..57 (36 px). 20 x 20 verdict glyph at x6, y30. Word in font 24 at x32, y28. Item name in font 16 right-aligned to x218, y32 |
| Fill | the verdict's fill; `kSurface` for a verdict with no fill |
| Words | `PASS`, `FAIL`, `CAPPED`, `SKIP`, `RUNNING`, `PENDING`; a Light run's listing reads `LISTED` (a level the source offers; the PD family's on the green Pass look, a UFCS or SCP level on the Noted look) or `NOT RUN` (a legacy level above 5 V, Noted) (`result_layout::verdict_word`) |
| Seen in | `b_ct_detail.png` (FAIL), `ufcs_cuktech_detail.png` (PASS) |

It is drawn by the page today (`paint_detail`). It belongs in the kit beside AlertBand: same box, same word place.

### 5.9 VerdictCell: `kit::draw_verdict`, `verdict_style`

| verdict | fill | ink | shape with no label | extra with a label |
|---|---|---|---|---|
| Pending | none | `kFaint` | 2 x 2 dot | |
| Running | none | `kFaint` | hollow `kAccent` square (the glyph box, 2 px deep) | 2 px `kAccent` outline along the cell's edge, steady, no dot (Fieldbook, user 2026-11-12) |
| Pass | `kOkT` | `kOk` | check | |
| Fail | `kError` solid | black | cross | |
| Partial | `kCautionT` | `kCaution` | tilde | 2 px `kCaution` bar on the bottom edge |
| None, Stopped | none | `kDim` | 2 px dash | |
| Skip | none | `kDim` | 2 px dash | 1 px strike through the label |
| Noted | none | `kText` | upright 2 px `kDim` bar | 2 px `kDim` bar down the cell's left edge (`kNotedBarW`); never struck |

- **A Light run records without measuring.** What the PD family advertises (a Skip with reason `Advertised` in the PD group:
  a fixed PDO, a PPS or AVS APDO, an EPR level) is drawn with the **Pass look** (`kOkT` fill, `kOk` ink): "we have it", nothing was
  requested (`result_layout::advertised`, `vmap` in `ui_charger_test.cpp`). Counts stay honest ("1 pass, 3 listed"; the
  roll-up counts them as pass for tone only, `Tally::adv`), the footer word is "advertised", the Step detail band says `LISTED`.
  The card agrees: the PD group and a range with nothing measured read green and say "Advertised" (the sheet's Verdict word,
  the JSON/summary "✓ advertised", the page's `advertised` word); the card's head is green when PD is listed and nothing failed.
- **Noted** is what a Light run records about everything that is not the PD family: a legacy group that answered at 5 V
  ("Detected"), a UFCS mode or SCP level listed from adapter data (Advertised, still Noted), a legacy level not asked
  (`LightLevel`). It is never green and never red (`result_layout::Tone::Noted`, `report_card::tone`); the head reads Noted
  when only legacy groups answered. The results page (`docs/site/r/card.js`) uses the same tone: no fill, a grey left bar,
  the glyph `i`.
  A `PortCuts` skip ("port cuts without load": a USB-A port that cut its own 5 V twice in one row) is a plain Skip,
  not Noted: the row was not measured and nothing was advertised.

- Label font: 16 in a cell 22 px or taller, else 12. The label is centred.
- Glyph size: the cell's short side minus 4, at most 12.
- Sizes in use: 12 x 12 (row marker, Results counters), 14 x 14 (Waiting checklist), 20 x 20 (verdict band),
  grid cells.
- Repaints: one cell. The running outline moves alone: its rim (`kit::run_mark_px(w, h)`, 2t(w + h) - 4t^2 with
  t = `kRunFrame` = 2) to erase on the old cell, the new cell's rim to paint. The rim never touches a grid label
  (`kit::mark_hits_label`; a cell where it would repaints its row instead).
- Seen in: `flight_step15.png` (pass, fail, pending, running outline), `ufcs_cuktech_running.png` (partial, skip),
  `b_ct_results.png` (none, skip strike).

### 5.10 ResultGrid: `kit::ResultGrid`, `draw_result_row`

| | |
|---|---|
| Purpose | Every step of a run as one cell; shared by Running and Results |
| Label strip | x0..59. Font 12, `kDim`, right-aligned to x55, at most 52 px. 1 px `kRule` at x60 |
| Cells | x61..223, up to 5 per row, equal width, 1 px `kBg` gaps |
| Row, volts only | 26 px: a 25 px cell |
| Row with currents | 40 px: a 24 px cell, then one bracket per run of equal current: font 12 `kDim` text at y+25, a line at y+31 with 3 px end ticks |
| Section | a 2 px `kRule` above the first row of a family; no header text. A Deep run's PD spec section follows the same rule (above the PPS block, the AVS block and the observer; cells carry the worst value, 8-character labels from `spec_text.hpp`) |
| Range chip with a sweep (PPS, SPR AVS, EPR AVS) | ONE chip per APDO, no separate Sweep row: the chip's verdict is the worst of the APDO's range steps and its sweep (Fail > Partial / Caution / Capped > Pass; Skip or none only when every part is; a sweep part that is pending or running makes the chip Pending, so the running dot sits on it). A small dim `S` (font 12, `kDim`, `kSweepGap` = 6 px, `kSweepMarkW` = 12 px = gap + glyph, the grid's `Metrics::sweep`; measured in the fold scene's four rotations: 7 px in the portrait ones, 12 in the landscape ones, because the landscape mapping scales positions by 240/224 but not glyph widths, so a gap only ever grows there; `test_ui_charger_test` asserts at least 6 px in all four) a fixed 6 px (one font-12 advance) after the label says the chip carries a sweep; the bracket under the chip stays. The `S` is a second hole in the fill, so every pixel is still streamed once. Opening the row (Steps) lists the range steps and the sweep entry; the Results footer of the row reads the sweep (fold text, `peak X V@Y V`, or `sweep failed` when a failed sweep recorded no point) when it has something to say |
| PD block (Running and Results) | ONE block for every PD step, the label `PD` on its first row only (the continuation rows have none). Order: the fixed levels as volt chips with their current brackets (PD 2.0, PD 3.x and the EPR levels that ran, ascending), then each PPS range as a chip `PPS 5-11V` over its bracket `5A`, then each SPR and EPR AVS range as `AVS 15-28V`; the kind word lives in the chip (counted in its width). A range chip has a line to itself. A kind the source never offered is dropped (no `None` row); a source that offered no PD at all folds to `No reply: PD` (never ran: `Skipped: PD`). The EPR "offered / entered" row and chip are gone from the grid: the levels that ran are volt chips, a skipped EPR level that is not a listing (cable, refused, over the cap) and an all-skip EPR AVS range are not drawn under the Results fold, and those reasons are on the PD Steps page. OK on any line of the block opens ONE Steps page with every PD step (fixed, PPS, AVS, spec); the footer names the focused chip's kind. **Layout B** (Results of a Deep run with its spec stage): a range line takes its kind as the label (`PPS | 3.3-21V 5A | shape`) and the range's spec word as a cell of its own (Pass fill for "spec", Caution or Fail for a group word, no fill for "-"), under a 2 px rule above the block's first range line (CHARGER_TEST_FLOW "PD spec section"). Rows differ in height (26 / 40 px): scroll and bottom-follow work on lines |
| Lone cell that never ran | spells its reason: `None`, `no reply`, `needs load` |
| OEM level row (Results, VOOC family) | the detection chip, then one chip per power level by its watts (`SVOOC 65W`, cell `SVOOC` over the bracket `65W`). A level is never a pass: `claimed` is `Noted` (framed), `not reached` is none (dim), `not tested` is skip (struck). Footer: `up to 120W claimed` (`docs/CHARGER_TEST_FLOW.md`, OEM power levels) |
| Focus (Results) | the label strip turns `kFocusT` with the 3 px bar; label and bracket text turn `kFocus` |
| Scroll | by pixels, whole rows only, up to 8 rows on screen |
| Follow (Running) | the window's bottom sits on the row of the step in flight (`anchor_bottom`). PLUS and MINUS scroll by hand; the follow resumes after 5 s or on the next step |
| Repaints | a row when its shape changes; single cells when only verdicts change; the running outline alone (`sync_dots`) |
| Seen in | `flight_step15.png`, `flight_deep_scrolled.png`, `results_end.png`, `results_scrolled2.png`, `fold_results.png` (merged PPS chip) |

### 5.11 ProgressBar: `kit::ProgressBar`

| | |
|---|---|
| Purpose | Steps done of total. The Running view counts rows the run executes (`result_layout::run_progress`: Pass and Fail over every row that is not a Skip), so plan-time skips are not work and a fresh run reads empty |
| Geometry | 3 px tall. Running: x6, y44, 212 px |
| Colours | `kRule` track, `kFocus` fill |
| Repaints | only the span that changed, between steps |
| Seen in | `b_ct_running.png`, `ufcs_cuktech_running.png` |

### 5.12 HeroReadout: `kit::draw_hero`

| | |
|---|---|
| Purpose | The one live value of a walk-by screen |
| Geometry | font 48 digits right-aligned in a fixed box; unit in font 24 on the baseline, 3 px behind |
| Running | right edge x196, top y54, 4 characters (a sweep row: the signed error, 3 characters, right edge x90, top y72; see Live run, sweep row) |
| Waiting | right edge x170, top y34, 5 characters, overline `VBUS` in font 12 at x6, y26 |
| Colours | `kVolt` when up; `kCaution` between 0.5 V and the start threshold; `kFaint` at 0 |
| Format | 2 decimals below 10 V, 1 above; `--` when unknown |
| Repaints | changed characters only, at most 4 times a second |
| Seen in | `b_ct_running.png`, `start_waiting.png` |

### 5.13 PassBandGauge and RangeGauge: `kit::draw_pass_band`, `draw_range_gauge`

| | |
|---|---|
| Purpose | Where the reading sits against the step's target |
| Geometry | 12 px strip at x6, y98, 210 px. 4 px `kRule` track in the middle |
| Pass band | `kOk` core 4 px in a `kOkT` window 8 px, +-5 % of the target, at least 5 px wide |
| Needle | 2 px white, full strip height |
| Scale | one per run: 0 to 110 % of the plan's top target |
| Range (sweeps) | two dotted `kOk` lines at the range ends, `kOkT` between, a 6 px `kFocus` cap at the setpoint; on a sweep row the strip sits at y116 (not y98) |
| Repaints | the old and new needle columns; the whole strip once per target (2520 px) |
| Seen in | `b_ct_running.png`, `ufcs_cuktech_running.png` |

### 5.14 InfoBlock: `kit::draw_info_row`

| | |
|---|---|
| Purpose | Tag and value lines where a table's rules would be too heavy |
| Geometry | 20 px rows (`kInfoPitch`). Tag font 16 `kDim` at x4. Value font 16 at x52, up to 21 characters |
| Repaints | the whole row is wiped and redrawn (224 x 20) |
| Seen in | Home `b_home_flat.png` (y196..239), Loop probe `mock_loop_running.png` |

Values use the protocol row formats of section 6.4.

### 5.15 Line cells and checklist (Waiting)

| part | geometry |
|---|---|
| Two-column line row | 26 px rows at y86 and y112. Tags font 16 `kDim` at x6 and x118. Values font 16 at x42 and x154, 8 characters. 1 px rule at x112 and under each row |
| Timeout bar | 2 px at y138..139, `kDim` fill on `kRule`, drains to the left |
| Checklist row | 26 px rows at y141, y167, y193. 14 x 14 VerdictCell at x8, y+6 (pass or none). Text font 16 at x28, y+5: `kText` when met, `kDim` when open |

Seen in `start_waiting.png`, `wait_line_moved.png`. These are drawn by the page today. A second page that needs
them should move them into the kit first.

### 5.16 Patterns

**Wizard.** `b_ct_algorithms.png`, `b_ct_cable.png`, `b_ct_review.png`
- Title `n/3 Name` and the progress rule. Three steps at most (two is supported).
- BACK goes one step back. On step 1 it leaves.
- No "Next" row. The step's own action advances.

**Pick and advance.** `b_ct_algorithms.png`, `b_ct_cable.png`
- A RowList with `Radio` markers. The solid one is the stored pick.
- OK on a row picks it and goes to the next step. Strip: `Pick`.
- The focused row shows what the choice means in its note. The tail shows a count or a rating.
- A row that opens a sub-list carries the chevron. Strip: `Open`.
- Footer: `Picked: <name>`.

**Review, arm, confirm.** `b_ct_review.png`, `b_ct_confirm.png`, `planned_reattach_confirm.png`
- KvTable of what will happen, then an ActionCell with the verb.
- First OK arms: amber cell, `CONFIRM`, 5 s bar. Footer turns Caution and says what the second press does
  (`Press again to start`). Strip: `Confirm`.
- Second OK inside the window starts. Any other key, or the window running out, disarms.
- PLUS and MINUS have no short action on a review page; their short labels are blank. The exception is a settable
  settable control (a `kv_row_sel` row, or the charger test's Level SegmentedControl): PLUS and MINUS move between it and
  the ActionCell, which keeps the focus by default; OK on the control changes the value in place.
- A hazard that does not block rides in the cell as the hint line.
- **Review reminder (charger test, 2026-11-21, the user's decision).** Review step 3/3 has no alert band: the ActionCell hint `other port must be empty`
  under START is the only reminder (the K2 cannot see a device there; START's arm + CONFIRM is the acknowledgement). The five kv rows
  (Tests, Depth, Spoof, Peak, Power) lay up under the rule at y154 above the 64 px START ticket. `test_ui_no_truncation` covers it in four rotations; renders
  `mock_ct_review*`, `mock_ct_confirm*`.

**Alert with choices.** `ra_guard_refuse.png`
- AlertBand, two body lines saying why, then the choices as a RowList. The first choice is the default.
- The risky choice is in `kCaution`. BACK cancels; there is no Cancel row.
- Footer in Caution states the safe way (`Direct plug is the sure way`).

**Wait.** `start_waiting.png`, `wait_line_moved.png`, `wait_no_charger.png`
- Hero, the line cells, a draining timeout bar, a checklist.
- The footer gives a cause only when the lines prove it. Strip: `Cancel` on OK.

**Live run.** `b_ct_running.png`, `flight_step15.png`

| y | part |
|---|---|
| 25 | `n/N steps` font 16 at x6; elapsed `m:ss` font 16 `kDim` right-aligned to x218 |
| 44..46 | ProgressBar |
| 52 | family, font 12 `kDim`, x6 |
| 68 | step name, font 16, x6, 11 characters |
| 54..83 | hero, right edge x196 |
| 88 | `target 5V`, font 12 `kDim`, x6 |
| 98..109 | gauge |
| 112..113 | 2 px rule |
| 114..221 | ResultGrid, following the step |

- The step name has its own line. It never shares a row with the hero.
- Footer: `<unit>: step i of n`. Strip: `Stop` on OK.

**Live run, sweep row (PPS, AVS)**. The signed ERROR is the hero (user decision 2026-11-16). Geometry and numbers live in
`include/k2/ui/run_layout.hpp` (`kSweepRun`, the `runlay` constants), the formatting in `include/k2/ui/sweep_hero.hpp`;
`tests/test_ui_sweep_live.cpp` checks every box below in 4 rotations.

| y | part |
|---|---|
| 25, 44..46 | as above |
| 48 | step name font 16 at x6; family font 12 `kDim` at x122 |
| 65 | `act` label (12 px, `kDim`, x120), the act value in font 24 (`kVolt`, x144, 6 characters, right-aligned on the point), unit `V` in font 12 |
| 72..101 | ERROR hero: up to three digits of font 48, right edge x90; unit `mV` (24 px, on the digits' baseline, x93); the sign in font 24 just left of the first digit, 4 px below the hero's top (`kSignDy`), `kSignGap` 3 px clear of the digit cell |
| 90 | `set` label, the set value in font 24 (`kText`, x144), unit `V`: stacked under act, both on the decimal point |
| 116..127 | gauge (range gauge, cap at the setpoint) |
| 130..131 | 2 px rule |
| 132..221 | ResultGrid |

- Colour by size of the error: `kOk` up to 100 mV, `kCaution` up to 300 mV, `kError` above (`chargertest::kErrOkMv`,
  `kErrCautionMv`). The sign is the hero's colour. Zero reads `0` with no sign.
- Format: below 1000 mV the digits are mV; from 1000 mV the hero turns to volts (`1.2` with unit `V`, whole volts from
  10 V, capped at 99). Font 48 has no `+`, so the sign is drawn in font 24 (a '+' glyph in font 48 would cost flash and a
  4-cell hero with its unit and the act / set block does not fit in 224 px).
- Repaints: the hero is a `Field` (char-diff) updated at most twice a second; a change of colour repaints the whole field
  (and the sign cell); the act and set values are Fields of their own.
- No overlap in any rotation: in landscape (240 x 224) a text cell keeps its native height while the fill under it is
  squeezed, so a box under a text cell needs a gap of at least h / 14 rows. `runlay::sweep_clear` is a `static_assert`
  for both mappings and the host test walks the pixels. (The earlier 3-decimal VBUS hero with a `set / err` line under it lost its
  last ink row in rot 1 and 3 this way.)
- Outside a sweep row the Running screen is unchanged (the 4-character VBUS hero).

**Live result grid.** `results_end.png`, `b_ct_results_sel.png`

| y | part |
|---|---|
| 22..45 | three counter cells, 75 px pitch, rules at x74 and x149: 12 x 12 glyph at x+2, y28; `N pass`, `N fail`, `N skip` font 16 at x+16, y26 in `kOk`, `kError`, `kDim` |
| 46 | 1 px rule |
| 51 | best line, font 16, x6, 26 characters: the PD version first, then the best modes, two spaces apart, trailing modes dropped before the version (`PD3.0  PPS 11V 5A  Mi 55W`; `PD3.2*` when the charger's Revision answer is under its offer; `PD3.0  No passing mode` in `kDim`; no version without PD) |
| 70..71 | 2 px rule |
| 72..221 | ResultGrid with a unit focus |

- Footer: the focused unit's counts. Tone follows the unit: Caution when it has a fail.
- OK opens the unit's Steps. Strip: `Steps`. Long OK shares the card.

**Detail drill-in.** `b_ct_results.png` to `b_ct_steps.png` to `b_ct_detail.png`
- Grid unit, then a RowList of its items (verdict marker, name, measured value in the unit colour, note with the
  reason), then one item: verdict band and KvTable.
- Each level's title names what it shows. BACK goes one level up.
- A detail page has no cursor: the PLUS and MINUS labels are blank.

## 6 Copy

### 6.1 Footer voice

- One fact, at most 27 characters. Pattern: `[state]. [cause or action]`.
- Never a key name. Say what will happen, not which key does it: `Press again to start`.
  The one allowed key name is the hold-plus hint on Waiting (`No VBUS. Hold + few secs`).
- Sentence case. No closing full stop on a single clause.
- A count carries its unit: `25 steps, 5 failed`.
- Constant footers are checked at compile time with `all_fit`.

| screen | footer |
|---|---|
| pick list | `Picked: All tests` |
| group list | `4 of 6 families on` |
| armed | `Press again to start` |
| alert | `Direct plug is the sure way` |
| running | `FCP: step 1 of 3` |
| results | `FCP: 1 pass, 1 fail` |
| steps | `25 steps, 5 failed`; a PD grid row's page `PPS: 3 pass, 1 caution` (the row's name, then every non-zero count that fits the 27 cells; a skip or listed count that does not fit is left off, a pass, fail or caution count that would not fit sends the name away) |
| detail | `Kept until the next start` |

### 6.2 Strip vocabulary (`label_violation`, `kOkVerbs`)

| slot | text | rule |
|---|---|---|
| BACK | `Back` | `Stop` only while a run it would stop is live |
| MINUS / PLUS on a list | `Up` / `Dn` | long press `Prev` / `Next` on a rotation page |
| MINUS / PLUS on a value | `-` / `+` | never mixed with `Up` / `Dn` |
| MINUS / PLUS on a page with no cursor | blank | |
| OK | one verb from `kOkVerbs` | names the focused item's action |
| OK, second press | `Confirm` | |
| long press | `word*` or `short/long` | long OK is `Share` where a screen has one thing to share |

Charger-test verbs: `Pick`, `Open`, `Toggle`, `Set`, `Next`, `Start`, `Confirm`, `Cancel`, `Stop`, `View`,
`Steps`, `Diff`, `Resume`.

### 6.3 Row copy

- **Row text.** A name. Sentence case. No verb unless the row is an action (`Start anyway`).
- **Note.** Lower case start, no full stop, at most 31 characters. It says what the row means or why it is in
  its state: `Everything, QC Class B 20V too`, `FAIL: timeout`.
- **Tail.** A number with no label when the column is obvious (`15`), else a value (`5.0V`), else a short word.
- **Upper case** only for the alert word, the verdict word, the action verb and pills.
- **Ports** are "top port" and "bottom port".
- **Placeholders.** `--` in `kDim` or `kFaint`. Never a screen full of them.

### 6.4 Number formats

| quantity | format | example |
|---|---|---|
| Volts, hero | 2 decimals below 10 V, 1 above | `5.00`, `20.0` |
| Volts, measured in a table | 2 decimals | `5.00V` |
| Volts, measured in a list tail | 1 decimal | `5.0V` |
| Volts, a level | trimmed | `5V`, `4.5V`, `12.5V` |
| Amps, a level | trimmed | `3A`, `2.25A` |
| Milliamps | whole | `0mA` |
| Watts | whole | `100W` |
| Watts, an OEM level or chip | 0.1 W below 100 W, whole from 100 W, at most 5 characters | `22.5W`, `65W`, `120W` |
| Range | hyphen, no spaces, one unit | `4.75-5.25V`, `3.3-21V` |
| Elapsed | `m:ss` | `0:04` |
| Duration | 1 decimal | `1.5s` |
| Count | number, space, unit word | `41 run, 5 skip`, `1/46 steps` |
| Step of | words | `step 1 of 3` |

- A unit takes no space from its number: `20.01V`.
- Several quantities in one value take one space between them: `20V 5A 100W`.

Protocol rows (InfoBlock, Home): a fixed level has no space, a range has one space before its current.

| source | row |
|---|---|
| PD fixed / EPR | `PD 20V5A` / `EPR 28V5A` |
| PPS | `PPS 3.3-21V 5A` |
| AVS | `AVS 15-28V 140W` |
| QC2 | `QC2 9V` |
| QC3 / QC3+ | `QC3 3.6-12V` |
| AFC / FCP | `FCP 9V2A` |
| SCP | `SCP 10V2.25A` |
| UFCS | `UFCS 5-20V 5A` |
| VOOC / SVOOC | `SVOOC 10V6.5A` |
| Type-C only | `CC 5V3A` |
| BC1.2 | `DCP 5V1.5A` |

### 6.5 No truncation

- Text cut to `...` is a build failure.
- Constant strings: `static_assert(all_fit(table, advance, box_px))` beside the table (`trunc.hpp`).
- Runtime strings: host builds log every cut; every UI test fails on one; `test_ui_no_truncation` walks every
  page in four rotations.
- External data (saved names, vendor strings) goes through `fit_data`: counted, not failed.
- The fix is to reword, or to let the focused row open its second line. Never a smaller font.

## 7 Page audit

What each page outside the charger test does differently, and how to align it. These are proposals. Where there
is a real choice, the options are listed and none is marked.

### 7.1 Ranked by payoff

| # | page | change | why it pays |
|---|---|---|---|
| 1 | Device twins | the decided redesign, section 9 | names are cut today (`Galaxy S2...`); no way to see why a device is limited |
| 2 | Who's attached | empty states, table rows and footers in the kit language | the most text-heavy page; three different looks on one page |
| 3 | Tools | focused row opens its hint; calmer blocked rows | the hub is opened most often; the hints are written and not shown |
| 4 | every page | strip pair spelling, footer fill in landscape, who reads `Stop` | small, visible on every screen |
| 5 | Loop probe, PD trigger setup | verdict marks for step results; the Waiting checklist instead of the card | brings two more run pages into the run language |
| 6 | Lines | plain tags, one verdict row | removes pills from a normal state and a literal `...` |
| 7 | Detail | words spelled out, fewer placeholders | read-only page, low traffic |
| 8 | Settings, Diag, PD diagnosis | footer wording, blank keys | already close |

### 7.2 Every page

| finding | where seen | proposal |
|---|---|---|
| The list pair has four spellings: `Up/Prev` + `Dn/Next`, `Up/Prev` + `Dn`, `Up/Prv` + `Dn`, `Up` + `Dn` | `mock_tools.png`, `mock_settings.png`, `mock_twin_list.png`, `mock_diag.png` | Rotation pages: `Up/Prev`, `Dn/Next`. Menus: `Up`, `Dn`. `list_strip()` adds the long alternative `Prv` even on a menu, where no long press exists; drop it there |
| A tinted footer stops at panel x224 in landscape; the last 16 px stay `kBar` | `b_ct_confirm.png`, `mock_tools.png`, `mock_diag.png` | `draw_footer` fills `s.w` (224) inside a 240 px band. Fill the band's width |
| Who reads `Stop`: charger test puts `Stop` and `Cancel` on OK and keeps `Back`; twins and Loop probe put `Stop` on BACK and blank OK | `b_ct_running.png`, `mock_twin_running.png`, `mock_loop_running.png` | Option A: the charger-test way everywhere (OK stops, BACK reads `Back` and stops first). Option B: BACK reads `Stop` everywhere, OK is free for `View`. The reference is A |
| Table row pitch: 26 (kit), 25 (Detail), 22 (Diag), 20 (Who's attached, PD trigger). Key font 16 (kit) or 12 | `b_ct_review.png`, `mock_detail.png`, `mock_diag.png`, `mock_pd_cable.png` | Option A: 26 px and font-16 keys everywhere; dense pages scroll or merge rows. Option B: allow one dense variant in the kit (22 px, font-12 key) and name it |
| A footer names a key | `mock_pd_source.png`: `OK > Refresh to read all` | State the fact: `Caps not read yet` |

### 7.3 Who's attached (Source, Cable, Sink)

Renders: `mock_pd_source.png`, `mock_pd_sink.png`, `mock_pd_cable.png`, `mock_pd_cable_no_chip.png`,
`mock_pd_cable_not_probed.png`, `mock_pd_cable_chip_silent.png`.

| diverges | proposal |
|---|---|
| Empty states are an upper-case font-24 headline (`NO CHIP FOUND`) and three lines of dim prose. Upper case is for alerts and verdicts | Use the verdict band: 36 px, `kSurface`, dash glyph, the word (`NO CHIP`). One body line in font 16. Keep the rows that still hold a fact (Type-C, Draw) |
| The footer repeats the headline (`No chip found` under `NO CHIP FOUND`) | The footer gives the next fact: `Plain cable, no e-marker` |
| Body prose names a key (`then OK -> Read cable.`) | Drop it. The strip says `Actions` |
| Table rows are 20 px with font-12 keys; 8 rows on the Cable tab | With the kit table: merge Current, Voltage and EPR into `Rating 5A 20V 100W`; six rows fit under the tabs. Or keep the dense variant of 7.2 |
| The tab strip looks like the wizard's progress rule (same 74 px segments, 2 px underline) but means "one of three" | Option A: keep; tabs have names in them, the rule has none. Option B: fill the active tab with `kFocusT` so it reads as focus, not progress |
| Source tab with no PD is one line and an empty body | Show what is known as rows (`Type-C 5V3A`, the BC1.2 or QC row) in InfoBlock format |

### 7.4 Tools

Superseded by the ring and the place pages (0.2, 0.3); the hub list below no longer exists.

Renders: `mock_tools.png`, `mock_tools_end.png`.

| diverges | proposal |
|---|---|
| The focused row does not open. `tool_hint()` holds a note for every tool and the page does not use it | Give each row its hint as the note (`charger on top or bottom`). The window stays 7 rows |
| Blocked rows carry a red upper-case word (`LOAD`) as plain text; with a load attached three rows and the footer are red at once | Option A: icon in the tone colour, tail word in `kDim`, the footer alone carries the red. Option B: the kit tail pill for the exception (`kErrorT` fill), as `ARMED` on the cable list |
| The footer reads `Ready` in the Ok tone for a ready tool | Info tone with a fact, or the blocker reason when blocked. A ready tool needs no green band |

### 7.5 Loop probe

Renders: `mock_loop_idle.png`, `mock_loop_confirm.png`, `mock_loop_running.png`, `mock_loop_cc.png`,
`mock_loop_done.png`, `mock_loop_refused.png`.

Already speaks the language: KvTable, ActionCell, arm and confirm, ProgressBar and InfoBlock while running.

| diverges | proposal |
|---|---|
| Step results are words in the value (`done 1.5s`, `skip none seen`, `running` in cyan) | A verdict mark per row (`kv_ground` already takes marks): check, dash, running dot. The value keeps the number: `1.5s`, `none seen` |
| The footer does not change when armed (`Cable into own HID port`, Info) | Caution, `Press again to start` |
| `START` again after a run, under a green `Done. Result saved` | Keep. Option: the verb `RUN AGAIN` |

### 7.6 PD trigger, FC trigger, Cable R, Spoof

No current render in `runs/ui-all`. Read from the code and the 2026-11-08 renders in `runs/user-guide/`.

| page | diverges | proposal |
|---|---|---|
| PD trigger, setup | the legacy rounded card with a flow diagram and a checklist (`draw_checklist` / `draw_flow_diagram`, removed) | the Waiting checklist rows of section 5.15 |
| PD trigger, steps | uses the wizard bar, KvTable at 20 px, AlertBand | move the tables to 26 px, or to the dense variant |
| FC trigger | wizard bar and RowList; tails are dim words (`levels`, `adjustable`) | fine. Add the note on the focused row |
| Cable R | three table rows and an empty body | a hero for the result (font 48, `kOhm`) once measured; the rows above it |
| Spoof | two table rows, then a radio list | fine. It is the charger test's cable list |

### 7.7 Lines

Render: `mock_lines.png`.

| diverges | proposal |
|---|---|
| The four line tags are solid colour pills. A pill is for an exception | Plain font-16 tags in the line's colour at x6 |
| A dim `...` pill stands for "no verdict yet". It reads as cut text | One row `Sees` with a dim `--`, then the verdict word |
| `Rp` and `MCU` use font-12 keys at 26 px | font-16 keys, or fold them into InfoBlock rows |

### 7.8 Detail

Render: `mock_detail.png`. The grid (8 rows of 25 px, two columns) is an approved decision.

| diverges | proposal |
|---|---|
| Cut words: `Unkn`, `FastChg` | `Unknown`, `Fast chg`; both fit their cells |
| Idle, most cells are placeholders in several spellings (`-.-/-.-`, `--`, a lone dash) | One placeholder, `--`, in `kFaint` |
| No footer and no OK action | Option A: keep (a pure instrument). Option B: 7 rows of 26 px and a footer for the stale state; frees a row by dropping the parked USB and Alt cells |

### 7.9 Home

Renders: `b_home_flat.png`, `b_home_idle.png`, `mock_home_diag_case1.png`. Home 2c is an approved design and a
walk-by screen with its own grid.

| diverges | proposal |
|---|---|
| The diagnosis word is red text on black (`Power short`). The reference marks a failure with a fill | Option A: keep (a word plus a colour). Option B: tint the row `kErrorT` like an Error footer |
| Nothing else | The InfoBlock rows already use the kit and the protocol row formats |

### 7.10 Settings, USB Link, Autorotate

Settings is the K2 place since Fieldbook (0.4): identity block on top, System state and Boot info rows at the end.

Display (Settings row, tail `100%, 60s/5m`) opens a page of three rows: Brightness (OK enters an adjust mode where [+] / [-]
step 10 % live, dim at a limit, OK or BACK leaves it), Dim after and Screen off after (pick lists; BACK keeps the old value).
Renders: `mock_settings_display.png`, `mock_display.png`, `mock_display_adjust.png`, `mock_display_dim_pick.png`,
`mock_display_off_pick.png`.

Renders: `mock_settings.png`, `mock_settings_end.png`, `mock_usblink.png`, `mock_autorotate.png`, `mock_place_k2.png`.

| diverges | proposal |
|---|---|
| Footer `Opens a settings page` describes what OK does | A fact instead. The row's note already says what it is; the footer can read `Remembered across boots` on every row |
| Autorotate picks with OK and stays (`Apply`) | Fine. It is a pick list with no next step |
| `ON` / `OFF` tails | A user decision (`UI_DESIGN.md` 3.3). Keep |

### 7.11 Diag and PD diagnosis

The PD diagnosis is the Log place since Fieldbook (0.4); Diag is K2 > System state.

Renders: `mock_diag.png`, `mock_diagnosis_case1.png`, `mock_diagnosis_case1_r3.png`, `mock_diagnosis_empty.png`,
`mock_place_log.png`, `mock_place_log_case1.png`, `mock_place_log_ramp.png`.

| page | diverges | proposal |
|---|---|---|
| Diag | 22 px rows, right-aligned values, `Up` / `Dn` on a page with no cursor | Blank the pair when the table does not scroll |
| PD diagnosis | one row uses an icon among verdict markers (`Data lanes`) | Give it a verdict too, or the dash |
| PD diagnosis | everything else | It is the Steps list. No change |

### 7.12 Black Box, Report cards, Saved cables

No current render in `runs/ui-all`. From the 2026-11-08 renders in `runs/user-guide/`.

| page | diverges | proposal |
|---|---|---|
| Black Box | the footer in the old render is cut (`3 captures. Idle, not sa...`) | Check it against the gate; reword to 27 characters |
| Black Box timeline | a colour-coded message log in upper case and many colours | Out of scope here: it is a log view. Keep the colours to the message direction only |
| Report cards | `Back/Del` on BACK | Delete belongs on a row action with a confirm (section 9.6) |
| Saved cables | RowList with chevrons, then a table | Fine |

### 7.13 Decisions (user, 2026-11-12)

Scope: every interactive page takes the unified design; the read-only instrument pages (Home, Detail) keep theirs.

1. **Stop key: BACK reads `Stop`** on every page while a run is in flight (charger test, twins proof, Loop probe, PD
   trigger, FC trigger). OK is free: `View` where there is something to open, otherwise blank. BACK stops first; a
   second BACK leaves.
2. **Tables: one named dense variant.** The kit table stays 26 px with font-16 keys; `kit::kDenseRowH` 22 px with
   font-12 keys is the dense variant, chosen per page on purpose (Who's attached, PD trigger steps, Diag).
3. **Tools, blocked rows: option A.** Icon in the tone colour, the tail word lower case in `kDim`; only the footer
   carries the red.
4. **Who's attached tabs: option B.** The active tab is filled `kFocusT`, so it reads as focus, not progress.
5. **Who's attached is split into three tools** (later the same day; the tabbed page did not work well). Tools lists
   `Charger ID`, `Cable read` and `Device ID`. Each is `ConnectionPage` locked to one tab (`lock()`):
   - no tab strip, rows from `kBodyY`; Plus/Minus never switch the tab;
   - OK runs the tool's own action (`Refresh`, `Read`, `Ask`); the other actions are plain rows under the result
     (RowList look: label at x10, focus `kFocusT` + 3 px bar), walked with Up/Dn; Up past the first row returns to the
     tool's action; the list scrolls by whole rows;
   - a greyed action row names its reason in the footer while focused; the tool's own action says it only on OK (it
     would repeat the band);
   - long OK still shares (QR).
   `K2_WHOS_ATTACHED=1` brings the tabbed page back behind the three rows; the host tests build with it and switch the
   single tools on with `Ui::set_single_tools`. Renders: `runs/ui-split/sheet.png`.
   Rows stay 20 px in the single tools too (user, 2026-11-12): nine results and one action row show before scrolling.

## 8 Code against renders, labs and the old skill

| item | code (wins) | other source |
|---|---|---|
| TopBar title | font 12 at x4, y4 | the lab and the skill said font 16 |
| Progress rule | y20..21 | the lab drew it at y21..22 |
| Row hairline | top line of the row | the lab drew it on the bottom line |
| Strip | 16 px thick | the skill said 20 px |
| Review keys | Tests, **Spoof**, then the **Level** SegmentedControl (2026-11-15: Level took Header's place; Peak and Power dropped for the other-port warning; 2026-11-16: a segment bar) | the skill said Cable |
| Step 2 title | `2/3 Spoof cable` | the skill said Cable |
| Cable warning | band `Cable connected`; choices `Start anyway`, `Start, skip spoof`; BACK cancels | the skill said `Cable on plug` and a `Cancel` row |
| Step detail rows | Target, Measured, Window or Reached, Reason, Header or Mode (a `Revision` row, caution ink, `3.1 < AVS 3.2`, when the answer is under the offer), Scope | the skill listed a `Settled` row |
| Results best line | no `Best:` word; the PD version leads: `PD3.0  PPS 11V 5A  Mi 55W` | the skill said `Best: ...` |
| VerdictCell lead | 12 x 12 marker; 14 x 14 and 20 x 20 in two places | the skill said 22 x 22 |
| AlertBand | 36 px | the skill said about 34..40 |
| Stop while running | BACK reads `Stop` everywhere (decision 7.13.1; the charger test changes) | the code had OK reads `Stop` on the charger test |
| Landscape renders | positions are mapped 224 x 240 to 240 x 224 | measure from code, not from a landscape png |
| Tinted footer in landscape | the fill covers 224 of 240 px | a defect, section 7.2 |

## 9 Device twins

The first page built from this document. The user decided the design. A gap is filled with the system's rule and
marked **(system default)**.

Flow: Tools, Device twins (list), OK on a twin, twin detail.

### 9.1 List

Title `Device twins` with the phone icon. Kit RowList at y24. One row per twin, then `Learn from device`.

```
y0   +--------------------------------------------------------+
     | [i] Device twins        | PAS        > 5.00V x 0.00A   |  TopBar
y20  +--------------------------------------------------------+  hairline
y24  |= [#] Google Pixel 9                                    |  focused row, 40 px
     |=     gets 33 W, device max                             |  note, font 12 dim, y48
y64  |  [ ] Lenovo ThinkPad X1 Carbon                         |  26 px
y90  |  [=] OnePlus 15                                        |
y116 |  [=] Samsung Galaxy S24 Ultra                          |
y142 |  [v] Apple 140W                                        |  proved on this charger
y168 |  (c) Learn from device                                 |  chip icon
     |                                                        |
y222 | PD 20V3A: 2 of 5 full                                  |  Footer, Info
y239 +--------------------------------------------------------+
       [#] solid   [=] half-filled   [ ] hollow   [v] VerdictCell pass   = focus bar
```

| part | spec |
|---|---|
| Name | font 16 at x28, `kText`, `data = true` (`fit_data`). No tail and no second column, so the name runs to the scroll lane: 24 characters, the longest the library stores. It is never cut |
| Marker | 12 x 12 at x8, y+7. Codes the prediction for the charger on the port by shape |
| Solid square | full power: `present::is_full()` |
| Half-filled | limited: 1 px frame, the lower six rows solid **(system default: the lower half, like a level)** |
| Hollow | 5 V only: `Kind::FiveV`. Tested before "full" |
| Marker ink | `kText` in every state **(system default: a prediction is neutral; cyan stays the pick colour, amber and red stay verdicts)** |
| No contract | a dim 2 px dash **(system default: the None glyph)** |
| No charger | no marker; note `no charger to predict from` **(system default)** |
| After a live proof on this charger | a kit VerdictCell: pass (`kOkT`, check), fail (`kError`, cross), stopped (dim dash). A proof the K2 skipped keeps the prediction shape **(system default)** |
| Another charger attached | the proofs no longer apply; markers return to shapes (today's `hide_proof_`) |
| Kit change | one new marker, `Marker::Level` with state 0 hollow, 1 half, 2 solid. Every state rewrites the 12 x 12 box |

Focused row, 40 px, note in font 12 `kDim` at x28:

| state | note |
|---|---|
| full | `gets 33 W, device max` |
| limited | `gets 36 of 55 W, cable` or `gets 36 of 55 W, charger` |
| 5 V only | `5 V only, device`, `5 V only, charger`, `5 V only, cable` |
| no contract | `no contract, charger` **(system default)** |
| no charger | `no charger to predict from` |
| proved | `proved 36 W` |
| refused | `charger refused 12V` |
| other failures **(system default)** | `no PS_RDY at 12V`, `out of band 7.4V`, `charger sent no caps`, `charger reset at 12V`, `unsafe VBUS, stopped` |
| stopped **(system default)** | `stopped` |
| Learn row | `reads a device alone on the K2` |

The longest note, `gets 100 of 240 W, charger`, is 26 characters; the box holds 31.

Footer, Info tone:

| state | footer |
|---|---|
| charger on the port | `<top offer>: <full> of <n> full`, for example `PD 20V3A: 2 of 5 full` |
| no charger | `No charger: no predictions` |
| no charger list, a device inline | the list gets a last row `Ask charger again` (rotate icon, note `resets the link, it relists`, OK label `Ask`). Armed: `Resets the link. Confirm` (Caution); sent: `Asking the charger again`; then for 2.5 s `Charger offered again` (Ok) or `Charger did not offer` / `Reset not sent` / `Tool or test running` (Caution) |
| empty library | `No twin yet. Learn one` |
| learning | `Reading the device`, then for 2.5 s `Twin learned`, `Twin updated`, `Already known`, `Plug a device in alone`, `Device did not answer`, `Library full` |
| learning inline (a device behind its charger) | armed: `Resets the link. Confirm` (Caution, OK label `Confirm`); running: `Resetting the link` / `Asking the device` (Back label `Stop`); then for 2.5 s `Twin learned`, `Learned. Contract changed` (Caution), `Device did not answer`, `Stopped. Link reset`; refusals `Tool or test running`, `K2 holds the contract`, `K2 runs on this VBUS` |
| nothing to ask (VBUS dark, no charger listing, no device alone) | `No power: add a charger` (Caution, 2.5 s) |

- The top offer uses the protocol row format of section 6.4.
- It is the charger's highest-power fixed or EPR level **(system default: a range row such as
  `PPS 3.3-21V 5A: 10 of 12 full` is 29 characters and would be cut; the worst fixed case,
  `PD 20V3.25A: 12 of 12 full`, is 26)**.
- `full` counts the solid markers.

Strip: `Back`, `Up`, `Open` (a twin) or `Learn` (the last row), `Dn`.

### 9.2 Detail

```
y0   +--------------------------------------------------------+
     | Twin                    | PAS        > 5.00V x 0.00A   |  TopBar
y20  +--------------------------------------------------------+
y24  | Source                                  OnePlus 15     |  header: dim "Source" left, the name right (font 16)
y44  | gets 36W                   |             wants 55W     |  font 24 values, font 12 words, grade ink
y70  | ###########################+#########..............    |  bar, 3 px, x6..217; the hairline runs under it
y76  | 12V3A                      |    limit: cable 3A        |  font 16, limit font 12 dim
y96  +========================================================+  2 px rule
y100 | 5 9 15 20V                 |         5V 9V 12V         |  facts: charger left (fixed voltages; over the column a span "5-48V 144W", never a current), device right (20 px rows)
y120 | PPS 5-21V 3A               |      PPS 3.3-11V 5A       |  only when either side lists a range
y140 |                            |             USB 2.0 480M  |  device only
y160 +--------------------------------------------------------+
     |= Prove live                                            |  RowList, focused row
     |=   asks the charger for 12V3A                          |
y222 | Basis: its sink caps                                   |  Footer, Info
y239 +--------------------------------------------------------+
```

**The split.** Under the header the block has two sides and one 1 px `kRule` hairline at x104 between them, from the
gets line down through the bar (the bar crosses it) and the contract line to the end of the facts rows. Everything
left of the hairline is the charger's (the header word `Source`), everything right of it is the device's (the header
carries its name):

| side | content |
|---|---|
| left, left-aligned from x6 (room x6..100) | `gets`, the contract `12V3A`, the offer: fixed span `5-20V 3A`, then `PPS 5-21V 3A` |
| right, right-aligned to x218 (room x110..218) | `wants`, the limit text, the device's fixed levels `5V 9V 12V`, then `PPS 3.3-11V 5A`, then `USB 2.0 480M` |

- There are no tags in the facts rows: the header is the tag. A pair of cells in one row takes font 16 when both fit
  their halves (11 and 13 characters), else both drop to font 12, so a row never mixes sizes.
- The limit text is the longest right-hand string, `limit: cable 3.25A` (108 px at font 12): it stops 6 px right
  of the hairline. Cable limits carry no parentheses.
- Header: the name takes font 16 right-aligned; one that does not fit beside `Source` drops to font 12 on the same
  baseline (24 characters fit at font 12), then `fit_data`.

Gets and wants block:

| y | part | spec |
|---|---|---|
| 44..69 | gets | word `gets` (`proved` after a live proof) font 12 `kDim` at x6, on the value's baseline. Value font 24 at x6 + word + 4: the watts, packed `36W` |
| 44..69 | wants | value font 24 right-aligned to x218. Word `wants` font 12 `kDim`, right-aligned 4 px left of the value |
| 70..75 | bar | 3 px at y71, x6, 212 px. Track `kRule` = what it wants. Fill = what it gets, in the grade ink. Kit ProgressBar with a fill colour argument **(system default: the kit bar's span and height)** |
| 76..95 | contract | font 16 at x6: volts in `kVolt`, amps in `kAmp`: `12V3A` |
| 76..95 | limit | font 12 `kDim`, right-aligned to x218, top y81: `limit: cable 3A`, `limit: device max`, `limit: charger` |
| 96..99 | rule | 2 px `kRule` |

**Grade ink.** Both numbers (`gets` and `wants`) and the bar fill take one colour, the grade of the prediction
(`twin::grade`, `predict.hpp`): the power it gets (`out.mw`) against the power it wants (`twin::want_mw`):

| grade | rule | ink |
|---|---|---|
| Full | gets at least 100 % of wants | `kOk` |
| Partial | gets at least 60 % | `kCaution` |
| Poor | below 60 %, or no contract | `kError` |

- By power ratio only, whatever the kind: a 5 V device that gets its 15 W is Full (green); a charger that falls back
  to 5 V for a 55 W device is Poor by the ratio (27 %). Only `Kind::None` (no contract) is Poor without a ratio.

- No prediction yet (no charger on the plug): `gets --` stays `kFaint` and `wants` is neutral `kText`, never red.
- The grade is a second cue, not the only one: the contract, the limit text and the list note say the same in words.
- While a proof runs, these lines hold still (section 9.4); the grade repaints once, when it ends.
- Both values at font 24 fit their halves: `proved 100W` ends at x94 (the hairline is at x104) and `wants 240W` starts at
  x136 **(system default: the words are font-12 labels, the values font 24, because a close-up primary value is at
  least 24 px)**.
- No charger: `gets --` in `kFaint`, an empty bar, contract `--`, no limit text **(system default)**.

Facts rows (20 px, from y100): two to three rows.

| row | charger (left) | device (right) |
|---|---|---|
| fixed levels | `5-20V 3A` | `5V 9V 12V` |
| PPS range, only when either side lists one | `PPS 5-21V 3A` | `PPS 3.3-11V 5A` |
| USB | none | `USB 2.0 480M` |

- No charger: the left fixed cell is a faint `--` **(system default)**. USB not read: `USB --`.
- The action list starts at y140 or y160 for two or three rows.

Action rows: a kit RowList right under the last facts row.

| row | note on focus | strip verb | marker |
|---|---|---|---|
| `Prove live` | `asks the charger for 12V 3A` | `Run` | none; a VerdictCell after a proof |
| `Re-learn from device` | `reads the device alone on the K2` | `Learn` | none |
| `Delete twin` | `removes it from the K2` | `Delete` | none |

- The list starts at y140 or y160 for two or three facts rows. It shows three, two or one row (the page also
  scrolls whole lines off the top so the focused action fits);
  the kit thumb appears when rows are hidden **(system default: a RowList windows and scrolls; section 9.8,
  question 2)**.
- `Prove live` with nothing to prove (no charger, or 5 V only) is in `kDim` and OK answers in the footer:
  `No charger to run against` **(system default)**.
- Rename stays off the device.

Footer, Info tone **(system default)**: the basis of the prediction. `Basis: its sink caps`, or
`Basis: its sniffed Request` when the prediction comes from a Request seen on the wire. With no charger:
`No charger: no predictions`.

Strip: `Back`, `Up`, the row's verb, `Dn`.

TopBar: `Twin`. The name sits in the first body line, right of the `Source` word (see 9.8, question 1); a longer name is
fitted with `fit_data` **(system default)**.

### 9.3 Prove live: armed

First OK on `Prove live` asks the runner without the confirm. A refusal shows in the footer for 2.5 s
(`Tool or test running`, `Device inline: unplug it`, `Power monitor not ready`, `No charger to run against`).
Otherwise the row arms, as the charger test's START does.

```
y150 | USB   2.0 480M                                         |
y170 +########################################################+  ActionCell, armed
     |#                      CONFIRM                         #|  kCaution fill, black font 24
     |#                    closes in 5s                      #|  font 12
     |#===========================================           #|  4 px black bar, drains left
y220 +--------------------------------------------------------+
y222 | Asks the charger for 12V                               |  Footer, Caution
```

| part | spec |
|---|---|
| Cell | the kit ActionCell, armed state. It takes the action list's box: y = list top, height = 220 - list top (90, 70 or 50 px). The 50 px case still holds the word, the line and the bar |
| Window | 5 s (`Ui::kConfirmWindowMs`) |
| Footer | Caution: `Asks the charger for 12V` (the contract's volts) |
| Strip | `Back`, blank, `Confirm`, blank |
| Disarm | any other key, or the window running out. The list returns with `Prove live` focused |

### 9.4 Proving

```
y170 | ------------------------------------.................. |  timeout bar, 2 px
y174 | Asked 12V 3A                                           |  InfoBlock
y194 | VBUS  11.98V                                           |  live, kVolt
y222 | Live proof running                                     |  Footer, Info
```

| part | spec |
|---|---|
| Box | the action list's box. The gets block and the InfoBlock stay |
| Timeout bar | 2 px at the list top, `kDim` on `kRule`, drains over the proof's time limit **(system default: the Waiting bar)** |
| Rows | InfoBlock at list top + 4: `Asked` the request, `VBUS` the live reading in `kVolt`, at most 4 updates a second **(system default)** |
| Footer | Info: `Live proof running` |
| Strip | BACK reads `Stop`; the other three are blank (today's behaviour; section 7.2 lists the choice) |
| BACK | stops the proof and asks 5 V back. Footer for 2.5 s: `Stopped. 5 V back` |
| Leaving the page | stops a proof this page started |

### 9.5 Proved

```
y24  | proved 36 W                                wants 55 W  |  word changes from "gets"
...
y170 |= [v] Prove live                                        |  VerdictCell pass
     |=     proved 36 W at 12.0V                              |
y222 | Proved 36 W at 12.0V                                   |  Footer, Ok
```

| outcome | block word | `Prove live` marker | row note | footer |
|---|---|---|---|---|
| granted | `proved` | pass | `proved 36 W at 12.0V` | Ok: `Proved 36 W at 12.0V` |
| refused | `gets` | fail | `charger refused 12V` | Error: `Charger refused 12V` |
| other failure | `gets` | fail | the list note of 9.1 | Error: the same words, capital first |
| stopped | `gets` | stopped (dim dash) | `stopped` | Info: `Stopped. 5 V back` |

- The measured volts in the note and footer are **(system default)**; the list note stays the decided
  `proved 36 W`.
- The result stays until another charger is attached. Back on the list, the twin's marker is the VerdictCell.

### 9.6 Delete twin: confirm

```
y170 +########################################################+
     |#                      CONFIRM                         #|
     |#                    closes in 5s                      #|
     |#===========================================           #|
y222 | Deletes this twin for good                             |  Footer, Caution
```

| part | spec |
|---|---|
| Cell | the armed ActionCell of 9.3, same box, same 5 s window **(system default: one confirm look for every armed action)** |
| Footer | Caution: `Deletes this twin for good` |
| Strip | `Back`, blank, `Confirm`, blank |
| Second OK | deletes, returns to the list, footer for 2.5 s: `Twin deleted` |
| Any other key | disarms |

### 9.7 Re-learn and auto-name

Re-learn from device **(system default: no confirm; it reads and never asks for power)**:

| step | screen |
|---|---|
| OK | footer `Reading the device`; BACK reads `Stop` |
| read done, same device | footer for 2.5 s, Ok: `Twin updated`, or Info: `Already known` |
| another device answered | the twin is kept. Caution: `Other device: twin kept` |
| nothing alone on the K2 | Caution: `Plug a device in alone` |
| no answer | Caution: `Device did not answer` |

Auto-name for a new twin (decided): vendor plus the most watts it wants, then ` 2`, ` 3` on repeats.

| case | name |
|---|---|
| vendor and sink levels known | `Google 33W` |
| a second one with the same name | `Google 33W 2` |
| vendor not in the name table | `VID 1234 33W` **(system default: today's fallback plus the watts)** |
| no vendor ID | `Device 33W` **(system default)** |
| no sink levels read | the vendor alone: `Google` **(system default)** |
| longer than 24 characters | the vendor part is shortened; the watts and the suffix stay **(system default)** |

### 9.8 Decisions on the open questions (user, 2026-11-12)

These override the defaults above where they differ.

1. **Long names.** The TopBar on the detail reads `Twin`. The full name is the first body row, font 16 at x6
   (`fit_data`, never cut at 24 characters). Every block below moves down by that row.
2. **Actions.** The whole detail page scrolls (PLUS/MINUS walk the action rows, the page follows the focus);
   the info stays above the actions.
3. **Units are packed everywhere on the twins pages**, as in the protocol rows: `gets 36W`, `wants 55W`,
   `12V3A`, `5V only, device`, `proved 36W`, `limit: cable 3A`.
4. **Marker ink.** Solid (full) in `kText`; half-filled (limited) and hollow (5 V only) in `kCaution`, a second
   cue next to the shape.

### 9.9 Wake on prove (user, 2026-11-12)

`Prove live` (page) and `twin run ... confirm` (shell, remote) work against a charger that has not offered yet: the
run presents Rd itself, wakes the charger, reads its caps at 5 V, predicts, then proves as before. Opening the page
or arming presents nothing: Rd goes on only at the confirm.

| when | what |
|---|---|
| Wakeable | a charger waits on the plug (`WaitKind::ChargerWaiting`), or VBUS is up (1 V or more) with no caps; never a device inline, never a `DualRolePort` (a phone toggling its Rp). Anything else keeps `No charger to run against` |
| Before the confirm | list footer, detail footer and the list note read `Charger has not offered` / `charger has not offered`; the Prove row is not dimmed and its note reads `wakes the charger first`; armed, the footer reads `Wakes the charger. Confirm` |
| Waking | footer `Waking charger`, note `waking the charger`, proof box `Step` / `Waking charger` (nothing is asked of the charger yet); `Back` reads `Stop` |
| Caps arrive | the runner takes the charger's 5 V contract (never more before the proof), the page repredicts and the proof runs as in 9.4 |
| No offer | after 8 s (`kWakeMs`) the slot is `NoCaps`, the run ends with reason `no offer`, Rd goes back at once, footer `Charger did not offer` (Caution) |
| `Back` while waking | the wake ends, Rd goes back, footer `Stopped. Charger released` |
| Rd afterwards | the page keeps the woken Rd after a finished proof (without it the caps vanish and the verdicts would hide) and gives it back in `leave()`; a run the shell started releases it at its end |

All footers are 27 characters at most and checked by `static_assert`; none names a key.

## 10 Twin-rail walkthrough

1. Start in `Home`: the single 224 x 240 content body keeps the button strip on the physical button edge.
2. Step the ring to Charger, Cable or Device and press `OK`. The opposite rail shows the place name; `OK` enters
   `Actions` and `Back` returns to `Info`, then to the ring. Short `Back` in the ring is Home; `Long Back` goes to the
   Settings target.
3. Check the four rotations. The button rail and opposite telemetry rail must frame one body rectangle:
   208 x 240 in portrait and 240 x 208 in landscape.
4. Move list focus. A focused row opens to 40 px, carries its explanation in the note, and never truncates
   fixed copy.
5. Trigger an action. Routine facts remain inline; only a transient result, confirmation or error uses a
   bottom toast, which expires and restores the underlying body.
6. For capture QA, compare every strip with the live panel (`test_ui_capture` / `test_ui_rotation`) before
   taking hardware screenshots. Then check all rotations on the connected K2 and verify the heartbeat
   `stack_free`.
