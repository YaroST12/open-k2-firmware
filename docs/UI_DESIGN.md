# K2 UI design (240 x 240 ST7789)

The screen design of the firmware: geometry, palette, components, navigation and the layout of every page. Last
reviewed against the code on 2026-11-08 (`include/k2/ui/`, `src/ui/`). The host capture harness
`tests/test_ui_capture.cpp` pins the layouts. Where this document and the code differ, the code is right; the
deliberate differences from the mock-ups are recorded in section 6. The first design (text-led, with IN/OUT port
naming) is kept in `ui_mockups/v1/`.

- Mockups: `docs/ui_mockups/*.png`, 32 screens at 240 x 240, saved at x2. They are rendered from the
  repo's JetBrains Mono TTF by `render_mockups.py`, with ligatures off as on the device.
- `ui_mockups/screens.md` is generated. For every screen it holds a grid sketch and a pixel table
  (x, y, w, h, font or corner radius, token). That file is the pixel-accurate part of the spec.
- This document holds the system, the rules, the reasons and the change list.
- Re-render on the host with `python3 docs/ui_mockups/render_mockups.py`. It never touches the device.

Changes from v1, driven by user feedback:
1. **More shape, less prose.** Outlined icon badges, pills, cards with accent stripes, gauges,
   sparklines and 12 px icons replace most sentences.
2. **No IN/OUT.** The ports are unmarked and either one can face the source. Direction is a vertical
   arrow between the physical top and bottom ports.
3. **Ah and load resistance on Home** (approved).

---

## 1 Hardware facts that drive the layout

| fact | source | confidence |
|---|---|---|
| With the screen upright, the **TOP edge has a male USB-C plug** in the power path | user, confirmed; the render shows the plug on top | confirmed |
| The **BOTTOM edge has a female USB-C receptacle**, the other end of the power path | user, confirmed | confirmed |
| The **LEFT edge has a female USB-C for the PC** (USB HID/logging, not in the power path). The label marks it "USB HID · NTC" | user; back-label drawing | confirmed |
| The **RIGHT edge has the buttons**, top to bottom BACK, -, OK, +. The + key carries a lightning mark ("press the lightning key to wake the PD screen"); - is also DFU | back-label drawing | confirmed |
| Firmware positive current (after `kInaForwardSign = -1`, `power_calc.hpp:63`) is power flowing **bottom → top**. Test: charger on the bottom port, Pixel on the top plug, firmware showed forward | user test | confirmed |
| Either port can face a source or a sink, because a device may hang off either one through a cable | user | confirmed |

Consequences for the design:
- At rotation 0, positive current draws **↑** (toward the physical top plug), negative **↓**.
  Rotating the screen rotates that physical direction; it does not reverse the measured current.
  In the deadband nothing is drawn.
- Ports are never called source, sink, input, output, forward or reverse. Text names them only by
  position: "top port", "bottom port", "either port".
- "Source" appears only when PD tells us which side is the source, from the PD role. It never comes
  from the port's position.
- The UI does not use the words "forward"/"reverse" (the firmware enum can keep them internally).

### 1.1 Gravity rotation and bench lock

The accelerometer (not a gyroscope) at INA-bus address `0x18` supplies gravity.
The firmware accepts LIS2DH12 identity `0x33` or SC7A20 identity `0x11`, configures 10 Hz sampling,
and requires one clear quadrant
for 300 ms before rotating. Diagonal/face-up postures, shocks and failed I2C reads retain the last
orientation. Missing hardware is harmless: the upright layout and manual gesture remain usable.

Hold physical **+ and - together** for 0.5 s: lock the current orientation. Release both, then repeat
to step through the other three quarter-turns; the fifth chord returns to AUTO at the latest stable
gravity orientation. Each chord shows a five-second mode notice and logs `[ROT]`.
The Home footer contains no +/- rotation help. The chord never pages or triggers anything; its result is remembered across boots like Settings > Autorotate.

**Settings > Autorotate** offers Auto and Manual 0/90/180/270 deg. Short +/- selects a row;
OK applies it immediately, and green `ACTIVE` marks the active mode. Manual ignores gravity; switching
back to Auto uses the latest stable posture even if it was tracked while locked. Choices are kept
across boots (stored in `/prefs.cfg`, applied before the first frame). The hidden +/- chord remains a shortcut to the same mode, not a separate policy.
The separate **Settings > Rotate buttons** toggle defaults to OFF and is remembered across boots. When ON, directional
keys follow the displayed strip: at rotations 2 and 3, physical +/- swap their logical actions,
including short and long presses; at rotations 0 and 1 they retain their original actions. BACK
and OK never change. The strip labels and press highlights stay beside their physical keys; the
physical +/- chord remains unchanged. A directional press held across a mapping change is ignored
until released to avoid an unintended action; a mapping change also clears pending trigger and
charger-test confirmations.

**Remote keys are logical.** A press injected over the remote protocol (`k2_remote.py press`, the
WebHID and HTTP dashboard, `ui_walk.py`, the QA scripts) always has its default-orientation meaning:
plus = next / down, minus = previous / up, OK, BACK, short and long. The rotation and the Rotate
buttons setting never remap it, and a remote plus + minus is two separate keys, never the rotation
chord (the gesture needs the physical keys). While a remote +/- is held, the strip lights the physical
key that carries that action in the current rotation. Physical keys keep the rules above.

**Settings > Smoothing** (fifth row, below Screenshot) calms the readouts and graphs with a One Euro
filter (docs/ARCHITECTURE.md, "Display smoothing"). OK cycles Off, Low, Medium (default), High; the
hint under the title names the active one (`OFF: raw readings`, `LOW: light calming`, `MEDIUM:
balanced`, `HIGH: steady, slower`). The choice applies at once, is remembered across boots, and
touches only what the screen shows: VBUS, current and power on Home, the Cable R page and the
sparklines and trends. The energy meter, the charger test and every VBUS step decision read
unfiltered or separately averaged values. A voltage drop shows within one to two samples at every
preset; the presets differ in how much of the +-20 mV jitter of a steady rail is left (Off 100 %,
Low 65 %, Medium 43 %, High 35 %).

| rotation / MADCTL | label edge | key order along screen edge | positive current points |
|---|---|---|---|
| 0 / `00` | right | BACK, -, OK, + | up |
| 1 / `60` | top | BACK, -, OK, + | left |
| 2 / `C0` | left | +, OK, -, BACK | down |
| 3 / `A0` | bottom | +, OK, -, BACK | right |

The male port remains the physical TOP and the female port the physical BOT in every diagram.
Landscape uses compact side-by-side symbols, with explicit TOP/BOT role labels in tool diagrams.
Home uses one small static arrow, pointing toward the receiving physical port in all four
orientations; idle is a short faint dash, and a frozen stale arrow is grey.
All pages retain their logical **224 x 240** grid: a scoped row-stream mapper fits it into the
physical content rectangle (240 x 224 for quarter-turns), so body and footer cannot clip under
the button strip. Glyph origins are mapped, but glyph pixels and character advances remain
native (no stretched columns or missing rows). Composed status/footer bands fit the mapped
rectangle while painting native-size text. Physical labels bypass the mapper entirely.
Captures use the same mapping as the LCD.
Host tests verify all pages/menus, four orientations, captures, physical highlights and flow shapes;
the restored sensor and rotated controller offsets still need hardware validation.

## 2 Users and posture

- **Home and Who's attached are sovereign**: glanced at for minutes from 30-60 cm. They should be dense, use
  colour for quantities, and keep numbers in stable columns.
- **Tools are transient**: one task, one verdict. Show the prerequisites as shapes (a port diagram
  and a checklist), not as paragraphs.
- **Input**: 4 buttons. Short +/- scrolls, a long press (0.5 s) pages, and holding BACK for 5 s reboots.
- **Style vocabulary**:
  - outlined rounded letter badges (V, A, W);
  - solid colour pills (CC1, PD3.x + W);
  - small coloured D+/D- tags;
  - dark rounded panels;
  - a status bar with the temperature and icons.
- **What v2 adds**:
  - gauges under each value;
  - the flow rail;
  - Wh/Ah/R/time together with a power sparkline;
  - a page indicator;
  - a footer that carries state and reasons.

## 3 Design system

### 3.1 Grid and spacing
- Content area **224 x 240** (x 0..223). The label panel is x 224..239, with slots 66/54/54/66 px and
  12 px labels rotated CCW in the upright posture; section 1.1 specifies its rotated geometry.
- The base unit is **2 px**. Spacing steps are 2/4/6/8/12. The gutter is 4 px and right-aligned
  numbers end at x = 216-220. The scrollbar is 3 px at x 221.
- Fixed regions:

  | region | y | content |
  |---|---|---|
  | Status bar | 0..17 | icon + title (or Home chips), mini V/A + flow ↑↓, page dots or LAST pill |
  | Body | 20..219 | cards (r = 5), 2-4 px apart |
  | Footer | 222..239 | tone-filled band, S12 message from x4 (≤ 27 chars), no icon |

- Rows: B16 lists 20-21 px, two-line list items 40 px (5 tools without scrolling), S12 13-14 px.
- **Tool layout**: a flow-diagram column at x 0..63 (the physical port picture) and a card column at
  x 68..223 (checklist, values or step cards).

### 3.2 Type scale
| token | font | use |
|---|---|---|
| `type-meter` | D48 (digits only) | Home V/A/W, Cable R result |
| `type-headline` | T24 | units, verdicts (`5A 50V 240W`), badge glyphs, request values |
| `type-body` | B16 | rows, checklist, values in cards |
| `type-caption` | S12 | pills, overlines (`TARGET`, `CONTRACT`), footer, panel |

At most 3 sizes per screen, plus D48 on Home. The glyph limits are known: S12 has no `° Ω ↑ ↓`, so
those glyphs appear only in B16/T24, or as icons.

### 3.3 Colour tokens (RGB565)
Each hue has a **tint** (about 15 % of the hue on black). Pills, tiles and selected rows use the tint as fill.

| token | value | tint | use |
|---|---|---|---|
| `bg` | 0x0000 | | background |
| `surface` | 0x18E3 | | cards, flow-diagram panel |
| `surface2` | 0x2124 | | overlays (HoldCard) |
| `bar` | 0x08A9 | | status bar, footer |
| `rule` | 0x2945 | | dividers, gauge tracks, sparkline grid |
| `text` / `dim` / `faint` | 0xFFFF / 0x8410 / 0x4208 | | primary / labels / "no data" |
| `volt` | 0xFFE0 yellow | 0x2120 | V, CC pill, VBUS trace |
| `amp` | 0x07FF cyan | 0x0124 | A, FIX PDO, protocol pills |
| `watt` | 0xFB2C coral | 0x3083 | W, PPS PDO, W bars (coral stays clear of `error`) |
| `energy` | 0x07E0 green | 0x0120 | Wh, active contract, PD pill |
| `charge` | 0x5D1F blue | 0x0866 | Ah |
| `ohm` | 0xB5B6 silver | | load R |
| `dplus` / `dminus` | 0xFB20 orange / 0x2D7F blue | | D+ / D- tags |
| `flow` | 0x07E0 green | | ↑/↓ chevrons. **One colour for both directions**, because direction is shape, not hue |
| `ok` / `caution` / `error` | green / 0xFC00 amber / 0xF800 red | tints | state dots, tags, check icons, footer band (tinted fill + text) |
| `stale` | 0xC618 light grey | 0x2104 | frozen values, LAST pill |
| `focus` | 0x07FF | 0x0146 | selected row fill + bar, edit underline, current page dot |

Colour is never the only signal (WCAG 1.4.1). Every state also has a shape (✓ / ✗ / ⚠ icon, arrow
direction) and a word (`READY`, `NO PD`, `LAST`).

#### Menu selection language
- Focus is the cyan-tinted row and cyan left rail; it moves with short +/- and is not an applied
  setting. OK performs the action named on the physical button strip; BACK retreats or stops as
  specified below. Keep text left aligned and reserve the right edge for the scrollbar.
- A persistent enabled/selected state uses green text **and a word**, independent of focus:
  charger algorithms use `ON` / `OFF` / `PART` and counts, cable identities (wizard and Tools -> Spoof,
  one list) use `USE` for the selected row and the amber `ARMED` for the identity answering now, and
  Autorotate uses `ACTIVE`. A selected identity is not necessarily armed; an active spoof may use a
  different identity (the card then reads "external identity"). Green indicates state, not permission to energize or proof of cable capabilities.
- Fit names within the available row width with the existing text fitter, leaving state prefixes
  visible. No checkbox or leading selection arrow is needed in these menus. Explicit warning and
  result icons remain appropriate elsewhere. USB Link shows `ON`/`OFF` as a right-aligned value (RowList tail), not a pill.
- Home, Tools, Who's attached and PD trigger remain the visual references. E-marker actions,
  FC protocol choices, charger setup, Spoof and settings now use the same focus treatment;
  read-only lists use scrolling rather than a false selectable focus.

### 3.4 Primitives and their drawing cost (no framebuffer)

The firmware draws through one 240-px line buffer (`gfx.hpp`): it opens a window, computes a row,
pushes it, and repeats. Every primitive below is defined so that **each row can be computed
independently in O(width)**. No primitive reads back pixels, and no primitive needs a second buffer.

| primitive | how a row is computed | cost per instance |
|---|---|---|
| Rounded rect (fill) r ≤ 8 | span = full width, minus a per-row inset from a 9-entry table for the top/bottom r rows | w x h pixels; 1 window |
| Outlined rect (frame, 1-2 px) | 4 thin rects (edges) + corner insets | ≈ 2(w+h)·t px; 4-8 windows |
| Icon 12 x 12 | 1-bit mask (18 B flash), fg/bg per bit | 144 px, 1 window |
| Badge (frame + glyph) | frame + one AA glyph | ≈ 800 px |
| Pill | rounded rect (tint) + S12 text on the tint bg (AA blends against the tint) | (len·7+8) x 14 px |
| Gauge (3-6 px) | 2 rects (track + fill) | ≤ 1 k px; redraw only the changed span |
| Chevron (10 x 6) | 2 diagonal runs per row, from a 6-row table | 60 px; static tool-diagram primitive |
| Port symbol 18 x 8 | rounded rect + inner rect | 144 px |
| Sparkline w x h | per row y: column x is lit if `y ≥ s[x]` (fill, at 25 % hue) or `s[x]` lies between y and the previous sample (line). Samples are kept as a ring of w bytes | w·h px; a full redraw of 116 x 20 ≈ 2.3 k px ≈ 1 ms at 40 MHz SPI; scroll = redraw |
| Range trace w x h | per column (2 px wide): rows between the lo and hi byte (0..255 of full scale) lit in `volt`; optional full-height mark columns (amber, red) where no band is drawn | w·h px, 192 x 26 = 5 k px; drawn once per detail repaint |
| Card | rounded rect + 3 px accent rect | w x h; drawn once per page |

Rules:
- Static parts (cards, frames, icons, labels) are drawn in `draw_static`.
- `update()` redraws only fields: digits, gauge fill deltas, the direction arrow on flow-state changes, the sparkline
  (at 1 Hz).
- Text that sits on a card or tint passes that background colour to the AA blender, as `Field`
  already does.
- Status/readout and footer bands compose background, text and icons in one 240-pixel scratch row
  before sending it. Each band uses one LCD window and sends each pixel once, rather than visibly
  clearing it and painting its foreground afterwards. Host regression measurements: a changing
  footer drops from 5723 to 4032 pixels (3 windows to 1); the live readout from 3684 to 2592
  (4 windows to 1). No full framebuffer or unverified SPI speed increase is used.
- Budget: a full Home repaint is about 57 k px; at the current 7.5 MHz SPI clock the pixel transfer
  alone takes about 122 ms (not the previous 25 ms estimate). Steady-state updates aim to stay under 6 k px
  per 100 ms tick.

### 3.5 Components
| component | anatomy (from `render_mockups.py`) | states |
|---|---|---|
| **StatusBar** | 12 px icon + S12 title; MiniReadout at x 100 (`20.01V 2.99A ↑`); page dots (5 x 5, pitch 8, right-aligned to 220) or static LAST pill (no age counter) | live / none / stale |
| **HomeChips** | ther icon + `32.8C`, solid `CC1` pill (volt), solid `PD3.0 65W` pill (energy), PC icon when HID is enumerated, page dots | live (solid) / stale (tinted grey) |
| **MeterRow** | 26 x 26 outlined badge (2 px, r 6) at x 2 · D48 value right-aligned at 178 · T24 unit at 180 · 3 px gauge at y + 38 over x 34..194, filled to value/contract | live / zero (faint) / none (`--.--`) / stale (grey) |
| **A badge** | the badge glyph is the **flow arrow ↑/↓** instead of "A" while current is outside the deadband | ↑ / ↓ / `A` in the deadband |
| **Direction** (Home) | one static 22 x 22 arrow at (8, 101) (Home v7, `kArrowX/Y` in `ui_home.cpp`), left of the IBUS digits | the IBUS cell; no USB-C port drawings. `draw_physical_arrow` rotates it to point at the receiving physical port in all four orientations (up at rotation 0 toward the top port, left at 1, down at 2, right at 3; the bottom port is the opposite) | no animation, repaint only when mode, direction or rotation changes; idle (inside the current deadband) = short faint dash (`draw_physical_dash`, 2 x 8 along the port axis); stale = the frozen direction in grey; no meter / no source = nothing |
| **FlowDiagram** (tools) | 64 x h panel: male port at the top, female port at the bottom, chevron column between, a role pill next to each port (`K2`, `CBL`, `DEV`, `?`), S12 caption mid (`VCON`, `1.2A`) | flow ↑ / ↓ / none |
| **Pill** | 14 px, r 3, S12; *solid* = state you should notice, *tint* = attribute | ok / caution / error / quantity / dim |
| **Card** | `surface`, r 5, optional 3 px accent stripe (colour = section meaning) | |
| **RowList** (kit) | `include/k2/ui/kit.hpp`, one shared instance `ctx_.rows` in the `UiContext` (one page is visible at a time; a page binds it with `init(surface, y, row_fn, ctx)` and re-applies count and selection in `update()`). 26 px rows, hairline at the row top, 12 px marker at x 8 (none / radio / box / check / verdict / icon), 16 px text at x 28, value or count right-aligned to x 214 (S16, 112 px max) or a drill chevron; selected = `focus_t` fill + 3 px cyan bar at x 0..2 and, when the row has a note, it opens to 40 px with a 12 px dim note line (rows below shift down, the thumb lane is the full width until the list overflows). `RowFn` fills a `RowSpec` per row (`data = true` makes the text a data cell: it is cut with `fit_data` and, past the width, ends in an ellipsis instead of failing the no-truncation gate; used for user-named identities and cable names). A focus move repaints the two rows involved, a value change one, idle writes nothing. Built on `ListView` (`set_focus_extra`, `set_full_lane`, `set_offset`) | selected / idle |
| **KvTable / VerdictCell / AlertBand / ActionCell / ProgressBar / InfoBlock / TopBar** (kit) | same header: `kv_static` + `kv_cells` (hairline rows, dim key, fixed value cell, optional per-cell width), `draw_verdict` (pass / partial / fail / none / skip / stopped / pending / running; partial = amber tint, ink and a 2 px bar under a label, a tilde as a glyph; skip = dim ink struck through by a 1 px line under a label, a dash as a glyph; `strike_px` is its extra cost, `draw_struck_text` the same strike for a name; running = the pending look plus a steady 5 x 5 px `kFocus` dot against the cell's top-right corner, clear of the label, `dot_px` its cost, 25 px, and `ResultGrid::sync_dots` moves it from cell to cell without repainting the rows), `draw_alert_band`, `ActionCell` (idle / armed with draining bar), `ProgressBar`, `draw_info_row`, `draw_top_bar` + `draw_progress_rule`. Footers use `draw_footer_cached`. The charger test adds three more: `ResultGrid` (60 px label strip, up to 5 verdict cells per row, 40 px rows carry current brackets, signature-cached slots, scroll by pixel height), `PassBandGauge` (target band over the live reading, on one 0..110 %-of-top-target scale per run so the band moves with each target) and `HeroReadout` (one big value with its unit, repainted only when the text or colour changes). `RowSpec` has a second column (`col2`) and a pill tail (the ARMED cable) | |
| **ListRow2** | outlined 24 x 24 icon badge, B16 name, S12 hint; selected = `focus_t` rounded fill + 3 px focus bar + solid status pill, otherwise an 8 px state dot (superseded everywhere by the kit `RowList`) | ready / caution / error |
| **Checklist** | ✓ (ok) or ✗ (error/caution) icon + B16 text ≤ 12 chars; met items dimmed | |
| **StepCard** | 22 x 22 numbered tile (ok = done, caution = to do) + S12 overline + B16 value | |
| **Gauge / GradeScale** | 3-6 px track + fill; grade = 3 coloured segments + ▲ marker | |
| **Sparkline** | `bg` box, 3 `rule` grid lines, 25 % fill + 1 px line | |
| **Footer** | 12 px icon (✓ ⚠ ⚡ ⏲) in the state colour + S12 message | |
| **HoldCard** | 200 x 84 overlay, 2 px error frame, ⚠ + `Reboot in N`, 6 px progress | |
| **Icons** | 25 glyphs, 12 x 12 1-bit (300 B): bolt, cable, phone, plug, ohm, therm, warn, check, cross, clock, chip, pc, brick, anon, pd, res, ... plus emark, blackbox, report, ghost, usb, rotate, keys, camera, wave for the Tools and Settings rows. Source: `ICONS` in `docs/ui_mockups/render_mockups.py`, generated by `tools/gen_icons.py` | |

## 4 Screens

| screen | PNG | what it shows |
|---|---|---|
| Home | `home_live` | ↑ flow (bottom → top), badges, gauges vs contract, rail, D+/D-, Wh/Ah/R/time, W sparkline |
| | `home_reverse` | ↓ flow: the static rail arrow points toward the bottom port; the current badge stays A |
| | `home_idle` | no VBUS: faint zeros, idle rail, footer `No VBUS on top or bottom port` |
| | `home_stale` | unplugged: grey values kept, LAST0:42 pill, the A badge back to "A", sparkline frozen |
| | `home_error` | no meter: dashes, red footer |
| | `home_reboot_hold` | BACK held: HoldCard |
| Who's attached | `conn_gal_rot{0,3}_<scene>` (`docs/ui_review_2026-11-06/connection/`; scenes source/cable/sink `_filled`, `_popup`, and the port-aware `_empty` variants) | header "Who's attached", tab strip Source / Cable / Sink, label/value rows, bottom status bar; see 5.1 |
| Diag | `diag` | kit KvTable of nine rows at a 22 px pitch (26 px would not fit above the footer; the one pitch deviation), S12 keys, S16 values, a verdict marker per group (USB / PD / SYS); the footer gives the worst state; reached from Tools |
| Lines | `lines` | D+/D-/CC1/CC2 tags + 0..3.3 V bars with 0.4 V and 2.0 V ticks; the decoded result in pills, then Rp and MCU as a kit KvTable; footer via the shared cache |
| Tools | `tools_ready` / `tools_caution` / `tools_blocked` | RowList, 26 px rows (7 visible plus a thumb), icon marker per tool, a chevron when ready, otherwise the readiness tag as value; the footer gives the reason |
| Settings | (host render `mock_settings`) | RowList, 26 px rows: icon marker, title, the current value as right-aligned tail; USB Link and Autorotate open their pages (chevron); Rotate buttons toggles directional remapping (remembered); Screenshot toggles remote capture; Smoothing cycles Off / Low / Medium / High; Wake sleeping port (last row, six rows in all, no scroll) toggles the idle Rd hold that asks a sleeping source, OFF by default; footer says whether the value is kept across boots |
| Autorotate | `mock_autorotate` | RowList with radio markers: Auto plus four Manual angles (0/90/180/270 deg); the selected row opens to show its note; OK applies; the solid square (hollow when not picked; the kit's pick-one marker has no circle) marks the current choice |
| E-marker | (live scan) | the cable scan and its result (rating, speed, VID:PID) are rows of the Who's attached Cable tab (Actions: Read cable); progress and refusals show in the page footer; no Tools row and no separate page. Saved cables below |
| Identify | (footer status) | Actions pop-up of the Who's attached Sink tab; its progress and result are footer text on that page and the ID rows fill in; no Tools row and no separate page |
| FC trigger | `fc_setup/edit/edit_coarse/review/armed/requesting/run/live_edit/live_review/live_edit_down/releasing/release_after_exit/safe/scan/refused` | staged protocol list → numeric draft → exact review → apply; live edit separates DRAFT, HELD and MEASURED. Every above-5V apply, including downward retargets, needs two short OKs within 3 s. Live retains the 0–20V, 10 s trace; refusal stays beside the reviewed request, never grants an inline bypass. Cleanup is measured asynchronously, including after menu exit |
| PD trigger | `wiz_offer*`, `wiz_voltage_pps*`, `wiz_current_*`, `wiz_confirm_*`, `wiz_live_*`, `pdtrigger_refused` | step wizard: dots plus `Step N/M: NAME`, one decision per screen. OFFER (list, [+]/[-] pick, OK next), VOLTAGE (PPS/AVS only: big value, ◀ ▶ markers, range bar; 20 mV / 100 mV steps, long [+]/[-] = 1 V), CURRENT (50 mA, long = 0.5 A, capped at the offer, default = offer max), CONFIRM (summary card `9.00V 3.00A = 27.0W`, offer power, above-5V caution band; OK twice above 5 V), LIVE (state banner Requesting / Accepted / PS_RDY / Rejected / Hard reset, REQUESTED vs MEASURED V and A). BACK goes one step back, exits from OFFER, releases to 5 V from LIVE. The footer states a fact about the step ("50mA steps", "Above 5V: send twice"), never the keys, at most 30 characters |
| USB link | `usblink_off`, `usblink_hid_pending` | RowList of three rows (HID, Drive, Network), ON/OFF as the value, the focused row opens to show its hint; OK toggles the selected row (Network turns HID on and Drive off, Drive on turns Network off, HID off turns Network off), [+]/[-] pick; footer "Replug to apply" until the next boot matches |
| Saved cables | `em_saved*`, `emarker_none` | RowList of saved cable identities (marker, name as a data cell, rating tail); the detail is a KvTable, a delete confirmation and an empty state are AlertBand pages; the e-marker live scan is on the Who's attached Cable tab, not here |
| Cable R | `cabler_wait/result/poor/fair/kelvin/kelvin_nofix/meter_missing` | kit KvTable (IDLE / LOAD, or LIVE / SENSE in Kelvin mode, plus `Port: load on top`), then S48 `110` + S24 `mΩ`, a verdict cell (green / amber / solid red) and a flat three-segment scale (66 x 6, lit in the grade colour, otherwise `rule`) with a detail line; a missing meter is the red `METER MISSING` AlertBand; footer facts only |
| Black Box | (no mockup yet) | state of the trace recorder: captures in RAM and in flash, the last trigger reason; OK freezes a capture by hand. Read on the host with `tools/pd_trace.py` |
| Spoof | `docs/ui_review_2026-11-06/wizard3/wiz3_rot*_spoof_*` | pick an identity from the shared cable list (Off, built-in chips, saved clones) and turn the SOP' answer on or off; the footer reads `Spoof on` / `Spoof off` style facts. Kit RowList (radio marker; the amber `ARMED` pill is the only tail pill, saved clones carry a dim `saved` tail) under a two-row KvTable (State; Asked / Replied counts) at y 24..76, list from y 76. Not verified on hardware |
| Report cards | `cards_empty/list/detail/detail_worst/detail_fail/confirm` (+ rot90/180/270) | the charger report cards saved by the charger test (section 4.4). Not verified on hardware |

### 4.0 Home v3 and the status bar v3 (replaces the layouts of 4.1 where they differ)

Home, top to bottom (224 x 240 content area):

| Rows (y) | Content |
|---|---|
| bar 0..17 | page name, three chevrons, boot mode and session time, page dots |
| 22 | line 1 `15.0V x 2.04A`, 24 px font, V yellow, A cyan |
| 48 | line 2 `= 30.6W`, 48 px digits, coral; `=` and `W` in the 24 px font |
| 84, 118, 152 | strips V, A, W (32 px high, 84 two-pixel columns), each auto-ranged to its own min/max with small min/max labels at x 198; one shared time axis; short OK cycles the window 10 s, 60 s, 10 min |
| 188 | negotiated row `data half / power half`, always both halves, unknown shown as `?` (data half: `usblink::format_ui`) |
| 205 | energy row `Wh 1.23  Ah 0.084`, no time |
| 222.. | footer (cause line, unchanged) |

Number format (`k2::readout::fmt3`, constexpr, host-tested): three total digits with a floating decimal
point, never mA or mW: `15.0V 5.08V 2.04A 0.12A 30.6W 100W 0.62W`.

Home idle and flat (host-tested in `tests/test_ui_home.cpp`, `tests/test_ui_perf.cpp`):

- **No source** (meter present, VBUS below 1 V): the hero shows real zeros (`0.00V 0.00A 0.00` plus the W glyph) in dim grey,
  not `--`; the info row says one dim `No source` (no `max` half); the top strip says `waiting for VBUS` in the
  faint colour and no axis label reads `--`; the footer says why (`Connect top or bottom port`, `No VBUS: hold
  PLUS at boot`, or `No VBUS`) and is repainted when that cause changes inside the state (a cable attaches).
  A steady tick paints 0 px.
- **Flat range** (a trace whose min equals max): the strip prints that one value centred vertically at the axis
  column instead of two equal labels.
- **No meter** (error state) keeps `--` in the hero: a zero would claim a measurement.
- An incremental repaint must equal a forced full repaint (0 differing pixels), checked for idle and flat in
  `test_convergence`; a steady idle or flat tick stays under 6000 px per 100 ms.

Status bar v3 (all rotation pages; menus keep the old bar): 6x12 font, page name at x 4, three animated
chevrons at x 56 (they run the way the power flows, speed and trail from the load against the largest
PDO, no motion while idle, never a sign), a middle field at x 78, page dots at the right.
Home middle field: `PASSIVE` / `SINK +` / `SINK OK` and the session time (grey when the reading is frozen).
Other pages: `19.1Vx2.04A=39.0W` (3 digits each) or `no meter`. The Home flow rail is removed.
Long OK on Home still resets the energy; short OK cycles the strip window.

Status bar v6 (current chrome): the live `fmt3(V) x fmt3(A)` readout carries one small static
physical-direction chevron in the 10 px slot x140..149 (before the V text at x152): `BandRow::chevron`
paints it inside the band stream so a band repaint cannot erase it. Same rule as Home: it points at the
receiving physical port (the 10 x 6 chevron becomes 6 x 10 in landscape), `kFlow` green, one colour for
both directions; no chevron while idle (inside the deadband), stale (LAST pill) or without a meter.
The readout repaints on a direction or rotation change as well as on a value change. Home has no bar
readout, so only the hero arrow shows there.

### 4.0.1 Who's attached tabs: rows per tab

The Source / Cable / Sink tabs of the Who's attached page (section 5.1) replace the former PD, E-marker and Identify
pages. The tab state machine and the match arithmetic are pure code in `k2::pdtabs` (`include/k2/pd/pd_tabs.hpp`,
host-tested in `tests/test_pd_tabs.cpp`); the rows are built in `src/ui/ui_connection_page.cpp`. Nothing scrolls:
every tab shows at most nine rows (key in S12 at x 6, value in S16 at x 64, 20 px pitch). An undetermined value
reads `unknown` or `-`.

| Tab | Rows (key) |
|---|---|
| Source | `ID` (VID:PID and vendor of the charger), `PD` (revision, `EPR` when advertised, maximum watts), `Fixed` (one row per current group, `5/9/15V 3A`), `PPS` / `AVS` (voltage range and current), `Used` (the PDO the sink requested), then `USB`, `Alt` and `Proto` when a link is known. A charger without PD caps shows `Type-C` (the Rp current) and `Legacy` (the detected fast-charge protocol). Waiting for caps shows one `PD` row |
| Cable | `ID` (e-marker VID:PID and vendor), `Speed`, `Current`, `Voltage`, `EPR`, `Type`, `Length` |
| Sink | `ID` (`unknown` until Identify answers), `Request` (PDO, volts, amps, `PPS`) with `State`, `Pwr` and `Data` (the match rows below), `USB`, `Alt`, `Proto`, `Caps` (the sink's own capabilities, whole entries, ` +` when more follow); when the K2 draws the power itself the tab reads `K2 (sink mode PLUS)` or `K2 (sink mode OK)` |

Match rows (Sink tab): `Pwr` reads `18W of 100W` (requested of offered) or `60W by cable` when the cable is the limit;
without a decoded PD contract it reads the Type-C default budget instead, `up to 2.5W` / `up to 15W` (5 V x the advertised
current, exact, `pdtabs::budget_line`), never a bare wattage that reads as a measurement. `Data` reads `5G of 10G` or
`5G by cable`; amber only when the cable limits the link, `?` when the cable is assumed
(no e-marker read, 3 A). Full wording: the Who's attached page section below.
Only real alt modes (DisplayPort, Thunderbolt) count as `Alt`: a vendor SVID such as 0x2717 is a charging protocol and shows as the
Proto row (`Xiaomi PD`) of the side that listed it. A VDM answer is filed by the sender's power role (header bit 8): the Source tab
reads the source's answers, the Sink tab the sink's (`pd::Engine::vdm` / `sink_vdm`), so a phone's ACK never overwrites the
charger's identity.

Colour: negotiated or used = green, advertised = amber (`Alt`), unknown or none = faint. `USB` is the data speed only
(`usblink::format_speed`); `Alt` is `usblink::format_alt`: entered modes green (`DP 2-lane`, `DP 4-lane`, `TB3/4`),
else advertised ones amber, `-` for none. Home keeps the combined `usblink::format_ui` in its data half.

### 4.1 Home: per-element rationale

**Layout variants.** `g_home_variant` (ui_pages.hpp, `home_layout()`) picks the geometry: 1 = compact, existing content (default); 2 = plus a one-line strip (contract, active CC, cable e-marker); 3 = two-line strip (contract, cable, CC1, CC2, protocol). The V/A/W digits are Spleen 12x24 scaled x2. Images: `tools/home_sheet.py`.
```
  0   34                178 194 216 224
0 [therm 32.8C][CC1][PD3.0 65W] pc ●○○○○]  chips: what is negotiated, at a glance
22 (V)   20.01        V                  badge + D48 + unit
60       ═══════════gauge══════════      gauge = V / contract V; width 186
66 (A)   2.990        A   ↑              one static physical-direction arrow
110(W)   59.83        W
154 [D+ 0.60 ][ 12.345 Wh      QC3 ]       left: lines card; right: totals card
176 [D- 0.60 ][ 0.617 Ah   1:23:45 ]
198 [Ω 6.69Ω ][ ~~~~ W 60 s ~~~~~~ ]
222 [✓ Power flows up  avg 1 s     ]
```
- **Badges, D48 and gauges.** The three values carry the page (visual hierarchy). Each badge repeats
  the quantity's colour. The gauge shows headroom against the PD contract (V,
  A and W of the active PDO). A full bar means "at the limit".
- **PD and PC chips.** The PD pill ends before the PC icon, including for three-digit wattages:
  e.g. `PD3.x240W` or `PD2 240W`. Unknown revisions use `PD?`. Values too large for the
  revision-qualified form use `PD` or `P` plus the complete wattage and `W`, never a clipped number;
  the PC icon keeps its meaning and position. In self-powered boot mode the `SELF-PWR` pill uses
  two-pixel side padding so its full label and border fit before the CC pill.
- **Direction has one dedicated arrow beside the current.** The A badge stays A; no USB-C
  port drawings or animated rail. The arrow follows the receiving physical port in all four
  orientations. Idle is a faint dash. Gauges use the freed width (186 pixels, formerly 160).
- **Lines card.** It uses the D+/D- tags. It holds load R (approved) with an Ω icon.
- **Totals card.** Wh (green) and Ah (blue, approved) are left-aligned in a stable column. The FC
  protocol pill is top right. Elapsed time is right-aligned. A 116 x 20 W sparkline (60 s) shows
  trends that a number cannot, such as a step after a PD renegotiation or thermal throttling.
- **Stale.**
  - Everything goes grey and the values are kept, because the last reading is the point.
  - The pill reads `LAST m:ss`.
  - The A badge stays "A"; the last direction arrow is frozen in grey.
  - The sparkline freezes.
  - Home alone freezes. The Detail page is a live instrument and never shows the LAST snapshot: unplugged it reads
    0.00 V, 0.00 A and the live CC and D+/D- levels (a waiting charger's Rp shows with VBUS dark), with no LAST pill.
- **Idle vs error.** Idle shows faint zeros (measured, and zero). Error shows dashes (nothing to measure
  with), with a red footer.

### 4.2 Other pages: key decisions
- **Who's attached.** The offers-versus-request comparison, the e-marker reader and Identify are tabs and actions of
  the Who's attached page (4.0.1, 5.1); there is no separate PD, Match or protocol-reader page. The page is passive:
  it only reads what the PD engine has decoded and transmits only from an action (Refresh, Read cable, Identify),
  never while it is merely open.
- **Diag.** Grouped cards with accent colours and status dots. Compact counter pairs use their own
  wide value column, while the other values remain aligned.
- **Lines.**
  - Each line has a tag and a bar against fixed 0.4 V and 2.0 V ticks, so the BC1.2/QC pattern is
    visible as shape.
  - The verdict card puts the decoded result in pills.
  - There is no NTC row (see open question 2).
- **Tools hub.** A kit `RowList` (see the Tools row in section 4): icon, name, a chevron when the tool is ready,
  otherwise the readiness tag, and the reason in the footer. Blocked tools still open, into their refused state.
  Hints name ports only as "top or bottom" or "either port".
- **Tool template (PD trigger refusal).** The refused PD trigger screen is the one place that keeps the flow
  diagram on the left, a card column on the right and a footer with the reason. Role pills come from what the K2
  knows (`K2` when it is the source) or from PD roles. A pill never says SRC because of the port's position. When
  nothing is known it shows `?`. The other tools are on the kit (RowList, KvTable, AlertBand).

### 4.3 Project-wide theme inventory

The inventory covers every entry in `Page` and `Menu`, plus the hold overlay. It is a source and
host-render audit, not a claim that every dynamic state was visually checked on a device.

| Screen or state | Theme treatment and audit outcome |
|---|---|
| Home; Who's attached | Preserved approved layouts, quantity colors and flow; the Who's attached tabs use the kit tab strip and RowList rows. |
| Diag; Lines | Retained status cards, measured-value colors, words and warnings; no selectable rows. Diag uses compact counter rows sized for full 32-bit values, including both members of a pair. |
| Tools; Settings | Retained the tool reference and matching cyan focus rail. |
| USB Link; Autorotate | Focus is separate from ON/OFF or ACTIVE; pending USB changes retain the replug warning. |
| Charger test detail | A failed step with a stored transition capture swaps its last two info lines (forced-revision note, PASS rule) for a 192 x 26 VBUS range trace at y 167 (Peak V + capture reason below, amber mark = first PD Accept, red = the failure trigger). No new buttons; steps without a capture keep the 7 text lines. Not verified on hardware. |
| Charger test | Every stage is on the kit: the top bar carries the title (12 px chrome, shortened) and the three-step rule, rows are `RowList` rows (radio marker for a pick, `ON` / `OFF` / `PART` tails on the family lists, a verdict marker on the Steps lead), the Results are a `ResultGrid`, Running and Waiting are a `HeroReadout` (VBUS) with a `PassBandGauge` / timeout bar. Text is fitted in pixels; a cut is a test failure. Warning and result colors remain semantic. |
| Charger test, result tones | One table (`result_layout::roll_up`, `Tone`) colours every Results chip, fixed, range and legacy alike: **every sampled step passed = green** (`kOkT`); **some passed, or the output capped = amber** (`kCautionT` tint, `kCaution` ink, and a 2 px bar under the label: never colour alone); **none passed = red** (solid `kError`); **all skipped = grey with the reason**. A pass with a skip and no fail is amber too (the row is only partly proven). A range chip keeps the advertised range text ("5-21V 3.2A"); how far it got is said where there is room: the Results footer for the focused row ("PPS: caps 19.4V of 21V", "max 13V of 21V", "reached 5-21V", "none reached", at most 27 cells), the Steps lead cell (amber for a Capped step) and the Detail page (band "CAPPED", the **Reached** row "3.3-19.4V of 21V" in place of Window, Reason "output capped"). A Capped step does not end its chain: the levels after it still run. Every one of these strings is width-tested (`test_ui_no_truncation`). Not verified on hardware. |
| FC trigger | Protocol picker now has the reference focus rail and neutral scroll thumb. Draft, review, live and cleanup retain their established measured/requested and warning layouts. |
| E-marker (Saved cables) | Kit RowList with the focus rail and fitted names; detail is a KvTable, delete confirmation and the empty state are AlertBands. |
| Cable R | Edge-to-edge table, verdict cell and flat scale segments; no cards, no flow diagram (the load port is a table row). |
| PD trigger | Preserved approved offer, edit, confirm and live screens; focused row and held contract remain distinct. Long ranges, currents and edit context use smaller native fonts before they can cross their columns. Live flow is in the measured heading, separate from the full measured value and unit. |
| Black Box | Capture/live-link picker already uses the focus rail; timeline and link screens are read-only. |
| Spoof | Two-row key/value header over the kit RowList (same identities as the wizard's Cable list; the solid square marks the pick, `ARMED` is the tail pill), focus rail and scroll cue. OK arms the row at once, OK on the armed row disarms, OK on Off selects no identity and disarms. |
| Report cards | List rows use the focus rail and an amber row for a charger with a failed group. The open card is a dense one-screen table: label column in dim, values in their measurement colour (V yellow, verdicts green/amber/red), light rules between PD fixed, PPS/AVS ranges, Handover and the legacy grid. |
| Reboot hold overlay | Retained warning frame, progress and release-to-cancel copy. |

Different screen purposes intentionally retain cards, pills, quantity colors and checklist/result
icons; the aim is consistent meaning, not identical layouts or removal of safety icons.
Host captures exercise representative idle, selected, running, refusal, result and rotated states;
they do not exhaust all long data, live transitions, or all rotations of every dynamic state.
Physical readability, controller offsets and real device behavior remain unverified in this pass.
Extreme decoded PDO values and 32-bit Diag counters have pixel-boundary and capture-parity host
checks in all four rotations. This does not cover every derived value or every legacy screen state.
The shared mini status readout uses `OVR` when a value cannot fit its slot, rather than overwriting
the adjacent value or flow symbol; the main measurement retains its value and units. Seven-page
dots start 3 px farther right to keep the adjacent flow glyph intact.

### 4.4 Report cards (Tools > Report cards)

Source: `k2/chargertest/report_card_hw.hpp` (`list` newest first, `remove`); page in `src/ui/ui_cards.cpp`. All text builders are host-tested (`tests/test_ui_cards.cpp`), every screen has a capture (`tests/test_ui_capture.cpp`, `cards_*`).

| Screen | Content |
|---|---|
| Empty | `No report cards yet`, three dim explanation lines, footer `0 cards saved`. OK is blank, long BACK does nothing. |
| List | Up to 8 rows (`ListView`, 20 px): title (name, else `VID:PID`, else `#fingerprint`), `b<boots>` (the card's boot counter; there is no RTC, so no date), `nP nF` passed/failed groups. Full title under the list, footer `i/N chargers`. |
| Card | Title and overall PASS/FAIL pill, id line `VID:PID  fp XXXXXXXX`, `PD fixed` verdict with highest passing V, one row per PPS/AVS range (`3.3-21V`, `max 20V`, `fail 13V`), `Handover` outcome and PD give-up time (`fell back 3.2s`), then the legacy groups in two columns (name coloured by verdict, max V, `fail` or `skip`; the name of a skipped row is dim and struck through like a skipped Results cell, the row's verdict glyph being a label-less dash). Footer: `run 3  boot 4  fw 0FF2C610`. The worst case (12 groups, 4 ranges, long name) fits above the footer line. |
| Delete confirm | `Delete this card?`, the charger name, what is lost; OK `Delete`, BACK `Cancel`. A note (`Card deleted` / `Delete failed`) shows in the footer on return. |

Buttons: the BACK slot reads `Back/Del` in the list and `List/Del` on a card (long press deletes; the full word does not fit the slot).

## 5 Navigation map
```
          long +  ─────────────────────────────────────►   (no wrap; the end -/+ label is dim)
  Home <-> Detail <-> Charger test <-> Lines <-> Tools <-> Settings (six dots)
  PD and Diag left the carousel: the PD view is the Who's attached page (Source tab), Diag is a Tools row.
  Charger test: a three-step wizard, Tests (presets, `Custom...` opens the family checklists) -> Spoof cable (the identity) -> Review, then Run/Results. Each session of the run reads "Detach charger" while the K2 holds the source dark, then "Initializing..." while it waits for the source (PD-silent for the D+/D- units); the run follows the company families: the vendor modes share one PD-silent session first (Huawei FCP/SCP, Samsung AFC, OPPO/OnePlus/vivo VOOC...), the PD unit follows (its Mi 6A probes read under Xiaomi), then QC/UFCS in a second PD-silent session, and the low PPS/AVS rows last.
  Entering Charger Test always starts at the Tests step (selection retained, confirmation cleared).
  Wizard pages (Tests, Custom, Group, Spoof cable, Review) are on the kit and share include/k2/ui/ui_setup_page.hpp for the side
  labels Up/Dn/OK and Back (Back on every step since the strip vocabulary, 6.10): the top bar reads the title with the
  three-step rule (`1/3` ... `3/3`), the footer states a fact (never the keys); Results, Running and the other stages carry
  no step count. The wizard advances on a pick and has no `Next` row (every remote `cursor` is a 0-based row). The Tests
  step rows are four presets (`All tests`, `PD only`, `Legacy only`, `Vendor modes`), `Custom...` and, after a run,
  `Last results` (six rows); the solid square marks the preset matching the mask, else `Custom...`; OK on a preset selects it
  and opens Spoof cable. `Custom...` opens the family list (rows 0..5 are the families in run order: Huawei / Honor, Samsung,
  OPPO / OnePlus / vivo, Xiaomi / Redmi (mirrors USB PD, not toggleable), USB PD, QC / UFCS (Class B 20 V included)); a
  short OK toggles the family, a long OK opens its Group page (a short OK there toggles a row).
  Back retreats one level (Group -> Custom -> Tests, Spoof cable -> Tests on the preset in force, Review -> Spoof cable on the identity
  in force).
  The preset rows carry a one-line note: `PD only` reads "PD fixed, PPS, Mi 6A (Xiaomi)", `Legacy only` reads
  "D+/D-: vendor modes + QC/UFCS" (the Mi 6A hunt rows belong to the PD set; every D+/D- family belongs to the legacy
  set), `Vendor modes` reads "Huawei, Samsung, OPPO, vivo" and `All tests` "Everything, QC Class B 20V too".
  **Step labels.** Every place that names a step uses one label from `step_label::label` (include/k2/chargertest/step_label.hpp, host
  golden vectors in tests/fixtures/step_labels.txt, mirrored by tools/step_label.py): `<protocol> <V> <A>`, e.g. `PD 9V 3A`,
  `PPS 20V 5A`, `PPS 11→8V` (a ramp row), `EPR 28V 5A`, `AVS 15V`, `Mi 20V 6A`, `QC3 12V`, `SCP 10V 2.25A`, `VOOC 5V`,
  `PD3.1 rev`. Amps appear only when the step has a meaningful one, never invented; the PDO position is not in the row
  text (it stays on the detail page). A Running/Steps row is the label (`data=true` text, shortened to fit), the measured
  voltage as the tail and the verdict marker. Gallery: `docs/ui_review_2026-11-06/labels/`.
  The Spoof cable step and Tools -> Spoof are the same list (`CableIdentityList`): one set of rows (`Off (real cable)` — no rating
  column, its note says what it means —, built-in chips, saved clones), one selection, one armed marker. A row is a short name with its rating in a second column; the solid square marks
  the pick, the amber `ARMED` pill the identity answering right now (the only tail pill), and a saved clone carries its
  library name in data colour with a dim `saved` tail. Captures: docs/ui_review_2026-11-06/wizard3/ (older rows).
  Results, step list and changes scroll like every other list: [+]/[-] move the
  highlight one row and the view scrolls only when the highlight would leave it; OK opens the detail of that row. The
  Results screen is the kit `ResultGrid`: counters (`16 pass` kOk, `4 fail` kError, `3 skip` kDim, each led by its
  shape, which is the legend; the word drops when it does not fit), one summary strip, then unit rows (26 px volt-only,
  40 px with a current line, a 2 px rule opens each family; a 60 px label strip, up to five verdict cells). A row is the
  protocol label and its cells, wrapped to display lines (a continuation line has no label and is selectable). Every
  cell is tinted by its verdict with a shape (check pass, cross fail, `-` skip), then the volt text, and the current under
  a bracket per run of equal current (`3A`). A **skipped** cell has no tint, so it carries its own shape: the label in dim
  ink with a 1 px `kDim` strike-through across it (on the x-height centre, the text width plus 1 px each side, clipped to the
  cell; only the volt label, never the bracket or the current). Pending stays faint with no strike, and "None" (a mode the
  source never offered) stays dim with no strike, so the three read apart: faint = not yet, struck = skipped, plain dim = not
  offered. The strike is the one pixel row painted over text (the cell's cost is its area plus the label width + 2 px,
  `kit::strike_px`, charged by the budgeted `draw_some` on both its paths). A label-less skip cell (the 12 px glyph of the
  report card) keeps the dash, so the card strikes the unit's name instead. Legacy D+/D- modes have volts only (the data model keeps
  no current for them, none is invented) and a Mi hunt shows the probed current. The footer explains the focused unit
  (`PPS: 2 pass 1 fail`), OK opens its steps. The strip holds the best passing mode of up to three protocol families by
  watts (`PD 20V 3A  Mi 20V 6A`), families that do not fit are dropped from the end. The grid has no section header
  rows. A protocol with nothing to show yet (EPR held back by the cable) folds into the "Skipped" row instead
  of leaving a bare label. The long [+]/[-]
  page switch is global, so scrolling cannot use a long press. Remote `cursor` is the setup row (0-based).
  USB PD has exactly two selectable entries: PD fixed (SPR/EPR fixed) and PPS (PPS/SPR AVS/EPR AVS).
  Subtests cover observed offers, not independently forced protocol revisions.
  Short -/+ selects rows; OK opens/toggles. All 13 visible entries start selected, including
  unavailable entries with explicit SKIP reasons. Cable identity (Off, built-in chips, saved clones) is the list shared
  with Tools -> Spoof; on the Cable step picking a row selects it and goes to Review, never arms spoof, START on Review arms it.
  Review has START as its only selectable row, focused on entry, with the setup summary as plain, tightly spaced, non-selectable lines below it (+/- do nothing). It requires two deliberate OK presses within five seconds; BACK/OK stops a live run.
  Resume run? (on a host-powered K2 the resume presents the Rd itself, like START, so a USB-C source turns VBUS on; the status says "Self-powered" only when the K2 is self-supplied now; texts in include/k2/chargertest/resume_text.hpp, every one fits its field; opens by itself after a reboot that cut a journaled run, remote stage "resume"): three rows (run N: d/t steps done, the interrupted phase "rebooted during request/handover", VBUS state), heading "Resume run?", footer "OK resumes; BACK keeps results". OK is refused ("Waiting for stable 5V") until VBUS has been 4.75-5.5 V for 500 ms; one OK continues from the next pending step (never re-detaches, never auto-requests above 5 V); BACK abandons, writes the finish record and shows the rebuilt results.
  Help uses the full body width, not the 24-character numeric-field limit. Instruction footers
  omit decorative icons; warnings retain theirs. Revision-only rows are absent. Results show the
  run's observed PD version from source RMDO, or PD2.0/PD3.x from the header when RMDO is absent.
  Review explains when an SPR PPS request reaches 21 V on a 20 V wire; the wire rating remains
  distinct from the allowable PPS request.
                BACK stops first; leaving also cancels, with verified 5V cleanup.
  Home: long OK resets energy.
  Tools: short -/+ selects; eight rows, no scroll, in this order: FC trigger, PD trigger, Spoof, Cable R,
         Who's attached, Black Box, Report cards, Diag. OK opens the row. BACK returns to Tools (selection kept).
         Spoof is a shortcut into Who's attached on the Cable tab's Spoof list (same list, same arming rules).
         Diag: long OK shares the log.
  Who's attached (5.1): three tabs Source / Cable / Sink; short +/- switch tabs, OK opens the Actions pop-up,
         BACK closes the pop-up, else leaves to Tools. Long OK = Share.
  Report cards: list; -/+ select, OK opens a card, long BACK asks to delete.
         Card: -/+ flip between chargers, BACK returns to the list, long BACK asks to delete.
         Delete confirm: OK deletes, BACK keeps. An empty store shows an explanation.
  Share (QR): long OK on a thing worth handing to a phone opens the share view: saved cable detail (Who's attached,
         Cable tab), Who's attached Source tab (source caps), Report cards detail, charger test Results (the run's card), Diag (the
         newest log entries as a LOG record; the label reads "Share*", short OK does nothing). The view
         shows the title, one QR code and "part i/n"; a record over one frame is several parts that cycle by
         themselves (about 1.5 s); long [+] / [-] page and stop the cycling, OK holds / resumes it, BACK returns.
         The screen repaints only when the part changes. Format and tools: docs/QR_SHARE.md.
  Settings: short -/+ selects; OK opens USB Link or Autorotate, toggles Rotate buttons,
            Screenshot and Wake sleeping port, cycles Smoothing (Off / Low / Medium / High).
            BACK returns to Settings (selection kept).
  Short BACK on a rotation page → Home, except an active test stops first.
  FC: short +/- picks a protocol; OK edits, then reviews. Review OK applies (twice above 5V).
      Live +/- opens a draft, never requests; BACK short/long or live OK stops first.
      After stopping, BACK exits while measured cleanup continues across pages.
  Hold BACK: HoldCard at 1 s, reboot at 5 s; release cancels reboot, not an already-issued stop.
  Remote `page` takes home detail chargertest lines tools settings or 0..5, the menu pages connection (16) and
         diag (17), and the old names as aliases: pd/source = Connection Source tab (18), emarker/cable = Cable
         tab (19), identify/sink = Sink tab (20). Other menus open from Tools/Settings.
  Open question: should the lightning + key open PD trigger from any page?
```

### 5.1 Who's attached page

One page for "what is plugged in": Tools -> Who's attached (menu page 16). Pure row model in
`include/k2/ui/ui_connection.hpp` (`conn::`), page in `src/ui/ui_connection_page.cpp`.

- **Tabs.** Source (the charger), Cable (the e-marker), Sink (a device inline). Short +/- switch tabs, a bottom
  status bar names the state. On open the default tab is Sink when a device is inline, else Source when a
  charger is seen, else Cable when a cable is present or read, else Source (empty).
- **Content.** Source and Sink: maker and VID:PID, PD revision and offers (Source) or the requested profile (Sink),
  USB version and speed, DP / Thunderbolt alt modes, and a "Protocol: Xiaomi PD" style line for known vendor
  SVIDs (names only, omitted when none; a name is a protocol, never a chip). Cable: e-marker speed, current,
  voltage, VID:PID, EPR capable. There is no seen/confidence line.
- **Layout (kit).** The tab strip is three cells at the top bar's progress-rule pitch (75 px, 74 px wide), label
  centred in S12, a 2 px underline in `kFocus` on the active tab and `kRule` on the others. Below it the rows are
  edge-to-edge with a hairline between them: key in S12 `kDim` at x 6, value in S16 at x 64, 20 px pitch, at most 9
  rows (later rows are dropped). Values use unit colours: V `kVolt`, A `kAmp`, W `kWatt`.
  Source PDOs: one "Fixed" row per current group and per 19-cell line (`5/9/15V 3A`, `20/28/36/48V 5A`, the
  current always at the end of its own line, nothing cut), then one PPS / AVS row each (`3.3-11V 5A`, range in
  `kVolt`, current in `kAmp`). The sink Caps row lists whole entries and ends with ` +` when more follow.
  The Actions pop-up is the shared kit `RowList` (`ctx_.rows`).
  **Every value fits the 19 cells whole** (host gate: `fit_text` fails on a cut, `test_worst_case_rows_fit`).
  Optional parts are added only when the whole part fits: the vendor name after VID:PID (a 10-character name
  leaves the ids alone), the ` x2` and ` TB` / ` USB4` tags after the cable speed, the RDO flag names after the
  State. A request row names the offer type first (`PDO3 9V 3A`, `PPS6 8.88V 2.05A`, `AVS8 20V 5A`, `BAT5 15V 45W`),
  the same text on the Source tab's Used row and the Sink tab's Request row. State reads `Requested`, `Accepted`,
  `Ready`, `Rejected`, then the flags most abnormal first (`Ready, Mismatch +`; ` +` = more flags did not fit).
  The cable speed drops its filler (`3.2 Gen2 10G`, `USB4 40G`, `TB 40G`, `2.0 480M`); VCONN is its own row.
  The Sink tab's two summary rows say who holds the number down (`pdtabs::power_line` / `data_line`):
  `Pwr 18W of 100W` (the sink asks for part of what is offered, neutral), `60W by cable` (the cable holds a bigger
  offer down, amber; `60W by cable?` neutral when no e-marker was read and 3 A is only assumed), `Data 5G of 10G`,
  `5G by cable` (amber), `80G cable` (only the cable is known), `+DP` when DisplayPort runs. Amber is for the cable
  limiting the link, never for a sink that asks for less than it is offered. On a link without a decoded PD contract
  (no Source_Capabilities seen) the `Pwr` row is the Type-C default budget instead, labelled as a budget and exact:
  `Pwr up to 2.5W` (500 mA Rp), `up to 7.5W` (1.5 A), `up to 15W` (3 A) -- 5 V x the advertised current, `pdtabs::budget_line`.
  **A live link repaints only the rows that changed.** The page keeps one hash per painted row (key, value, colour,
  cut, dim) and the kind, tab and row count of what the body shows. The whole body repaints only when the area was
  wiped or is stale (new tab, pop-up closed, empty state left, page entered, repaint pass) or the row count moved;
  a PPS phone that re-requests every second or two repaints its Request and Pwr rows and nothing else (host:
  `test_live_request_repaints_only_its_rows`; the log line `[CONN] body full=<0|1> rows=<painted>` counts it).
- **Actions pop-up** (OK; BACK closes it). Source: Refresh, Share (QR). Cable: Read cable, Save cable,
  Spoof as... (greyed "Live sink: unplug load" while a sink is inline; arming while a charger is attached shows
  "Armed, replug to apply"), Saved cables, Share (QR). Sink: Identify. A greyed row carries its reason. Long OK
  stays Share. The Tools Spoof row opens this page on the Cable tab's Spoof list.
- **Port-aware empty states.** An empty tab says why, using the topology the passive CC classifier already
  produces (`raguard::classify`): Rp (>= 2400 mV) on both CC lines = direct receptacle (a charger straight in the
  bottom port, no cable), on one line = single-CC cable, none = no source. It is Unknown unless the K2 is
  passive and VBUS is below 1000 mV; Unknown gives the generic hint.

  | Empty tab | No source | Direct receptacle | Single-CC cable |
  | :--- | :--- | :--- | :--- |
  | Source | NO CHARGER: plug a charger into the top plug or bottom port | CHARGER WAITING: charger waits for a sink, start a charger test | same |
  | Sink | NO DEVICE: plug a device into the bottom port | NO DEVICE: plug a device into the bottom port | NO DEVICE: charger is on the bottom port, no device side in this setup |
  | Cable | NO CABLE READ: plug a cable into the bottom port, then Read cable | NO CABLE: charger is plugged straight in | CABLE NOT READ: OK -> Read cable (unplug the charger first) |

  **Crossed CC.** With the K2 passive and VBUS dark for >= 1 s, `pd::Link::cc_cross()` can override the Source row
  above: `CC CROSSED` (hint "Flip one of the two cables (the CC wires meet no partner).") when a steady line (Rp, or
  Rp loaded by a cable's Ra) sits next to a toggling one, and `FLIP A CABLE?` (hint "A charger waits, nothing answers:
  try flipping a cable.") when only the loaded line is seen. The bottom bar reads `CC CROSSED - flip one cable` or
  `Flip a cable?`, and the Home Idle/Stale info row 2 value reads `CROSSED: flip one` (error colour) or
  `Flip a cable?` (caution colour); the value field is 17 characters wide. A charger or an inline sink wins over
  the verdict.

  The bottom status bar adds the topology word ("top plug" for a direct receptacle, "via cable" for a single-CC
  cable). The hint is text only: it reads the ADC the page already samples and never starts an active CC
  operation (host-tested per combination in `tests/test_ui_connection_page.cpp`, counters of Ra, Rd, scan,
  hard reset and capability requests stay at 0).
  **Limit:** which side of the CC analog switches the ADC taps is not hardware-proven
  (`docs/K2_PORTS_SPEC.md`, Q1), and the classification is passive, so "top plug" / "via cable" is the likely
  reading of the Rp pattern, not a measurement of the port.
- **Images:** `docs/ui_review_2026-11-06/connection/conn_gal_rot{0,3}_*.png` (three tabs filled and empty,
  pop-ups, Tools). Regenerate with `K2_DUMP` on `host-build/test_ui_capture`.

`charger-test status` / `charger-test results` read the existing RAM run without changing the
page, selection, confirmation or cleanup. Export is evidence retrieval only, not a remote START
button; entering the page and reading/capturing it remain passive.

## 6 Interaction and copy rules
1. **Stop before retreat.** A short BACK on an idle rotation page goes to Home. An active charger test
   stops first; active FC BACK stops before any retreat, including a 500 ms long BACK.
2. **Long-press actions are visible.** A `*` on a label means long press (`Rst*`). The footer spells
   it out where there is room. **Long OK = Share** wherever a screen shows one thing that can leave the
   device as a QR code (the label reads "Share"); a screen with nothing to share leaves the label blank.
3. **Pressed feedback.** The pressed slot flashes `focus` for 120 ms. The long-press slot stays lit
   until release.
4. **Hazardous applies are deliberate.** FC uses a Review screen and two released short OK presses
   above 5V; no navigation or hold sends voltage. FC has no on-device inline override. Existing
   The Who's attached Identify and Read cable actions ask for a second OK while a live sink is inline. Energy reset can be undone for 5 s.
5. **The reboot hold is visible** (HoldCard).
6. **Stale lifecycle.**
   - Unplug: the screen goes grey and shows the LAST pill.
   - Long OK on Home clears it.
   - Replug replaces it.
7. **Direction.** One static arrow changes only with flow state or orientation. Digits
   change at most 4 times a second, behind a display filter.
8. **Copy.**
   - The footer pattern is `[state]. [cause or action]`, at most 27 chars (the footer's 27 cells, item 9 below).
   - Ports are "top port", "bottom port", "either port". Never IN/OUT, input/output, forward/reverse,
     or source/sink by position.
   - OK verbs: Scan, Listen, Run, Force, Start, Stop, Send, Rescan, Clear, Open, Reset (the full
     list is `kOkVerbs` in `ui_labels.cpp`).
   - Units carry no space (`20.01V`). Upper case is for pills and overlines only.
   - The footer never repeats the strip: it states a fact about the screen, not which key to press
     (`footer_is_clean`). "Press again to confirm", not "OK again".
10. **Strip vocabulary.** One word per job on the four labels, checked by `label_violation`
   (`ui_labels.hpp`) for every page and menu in `test_every_label_fits_its_slot`:

   | slot | list page or menu | value page | rule |
   |---|---|---|---|
   | BACK | `Back` | `Back` | always `Back` (also wizard step 1); `Stop` only while a run is live |
   | [-] | `Up` (long label `Prev`) | `-` | `Up` / `Dn` move a highlight, `-` / `+` change a value |
   | [+] | `Dn` (long label `Next`) | `+` | never mixed on one page; `Tab-` / `Tab+` and `Mode` are the exceptions |
   | OK | a verb of the focused item | a verb | `Open`, `Send`, `Confirm`, `Zoom`...; never `Yes` / `Again` / `Span` |

   A second press that confirms reads `Confirm`; the footer says what is being confirmed
   ("Above 5V! Send again"), not which key does it.
9. **Footer tone (severity).** The footer has no icon: the band FILL states how the message should be
   read, never how the page feels. The text starts at x4 in font 16 (8 px advance), so the budget is
   **27 cells** (x4..219). `footer_style(FooterTone)` in `ui_widgets.hpp` is the table:

   | tone | band fill | text | use |
   |---|---|---|---|
   | Info | `bar` | `text` | a neutral note, a hint, a count, "USB link off", "Pick rows, then Next" |
   | Ok | `okT` | `ok` | a good or active state: "All subsystems up", "Spoof on", "Active this boot" |
   | Caution | `cautionT` | `caution` | the user should look: "Replug to apply", a partial result, a tool that needs a source |
   | Error | `errorT` | `error` | a failure or a blocked action |

   A green band on a plain instruction, an empty list or an in-progress state reads as "done" and is
   wrong: those are Info. `draw_footer_cached` repaints only when the tone, the text or the surface
   changes (`test_footer_tones` covers the four fills and the cache gate). In-progress states use the
   clock in their own quantity colour (Black Box filling).
11. **No truncation (gate).** Copy that does not fit its box is a bug in the copy: reword it, never cut it
    with "...". Two layers keep it so:
    - **Compile time.** `fits_px` / `all_fit` (`k2/ui/trunc.hpp`) take a constant string table, the
      font's advance and the box width in px; a `static_assert` beside each table (preset names and
      notes, family names, Tools entry names) makes a too-long entry a build error.
    - **Run time.** Host builds (`-DK2_TRUNC_LOG=1`) record every string `fit_text`, `Field::draw` or
      `gfx_text`'s surface clip had to shorten, with its box width and call site. Every UI host test
      fails through `test_report()` on a recorded cut, and `tests/test_ui_no_truncation.cpp` walks
      every page, submenu, Who's attached tab and the wizard in all four rotations over idle, SPR, EPR and
      loaded states. The device build keeps `fit_text` as a last-resort net with no logging.
    - **External data** (saved cable names, vendor strings, PDO lists read off the device) is cut by
      design: draw it with `fit_data` (counted, not failed). A value whose copy-part is longer than its
      box needs a layout change, not a data cut.
    - Where one vocabulary is mirrored outside the firmware (`charger_test::name(Reason)` in the card
      export and the web tools), the screen shortens at display time (`reason_text`) instead of
      changing the shared name.

### 6.1 Four-button input language: Pick / Edit / Review / Apply

Physical BACK and OK keep their meanings in every orientation. Directional keys retain their
physical meanings by default; the optional Rotate buttons setting swaps their actions and labels
at rotations 2 and 3. `-/Big*` and `+/Big*`
fit the tiny slots; the body states the exact fine/coarse units. `*` means a 500 ms long press,
**one change**, not repeat. The detector also emits a 5 s Hold; there is no repeat/double-click.

| state | BACK | MINUS / PLUS short | long MINUS / PLUS | OK short |
|---|---|---|---|---|
| Browse | parent, or Home on an idle main tab | previous / next row | previous / next main tab; no FC modal action | open/select; toggle explicit checkboxes |
| Numeric draft | previous step | decrease / increase draft | one coarse draft change | next/review, no request |
| Review | previous step | no adjustment; disarm | no action; disarm | apply ≤5V; arm then apply above5V |
| Live FC | stop immediately | open and nudge a retarget draft | no action | stop |
| Live FC edit/review | **stop**, short or long | draft adjustment / no adjustment | one coarse draft change / no action | review / deliberately apply |
| Scan/negotiation | stop, short or long | no action | no action | stop |
| FC cleanup | exit on short BACK; cleanup persists | no action | no action | no action |

FC is the first migration. PD retains its existing staged wizard and 20mV PPS/100mV AVS,
50mA current steps (long = 1V/0.5A once); PD live editing is **not added**. Settings retains
direct OK Apply/Toggle, stored USB choices and its existing wrap behavior. Charger test uses
grouped checklists, explicit Cable/Spoof setup and Review, retaining its five-second start
confirmation and runner. There are no user voltage ceilings; electrical guards remain.
Global nonwrapping long +/- paging, modal paging suppression, Home energy reset, BACK reboot
and the +/- autorotate chord remain. The chord disarms FC confirmation and never sends requests.

FC numeric units: QC3/QC3+ tap200mV, long1V once; SCP tap/long1V; QC2/AFC/FCP select offered
levels (long also selects one level); fixed5V entries skip editing. Bounds and monitor ceiling
clamp drafts to protocol units; a protocol with no level within the ceiling cannot apply.
QC3 supports3.6–20V for Class-B sources; QC3+ stays3.6–12V. The Tools hub marks an inline FC
load **blocked**, with `Load attached: unplug it`, not a confirmation/override suggestion.
The list has seven visible rows and a thumb. Each step fits the logical224×240 body.

Worked QC3 flow: pick QC3 → OK → draft9.00V → OK (Review) → OK (arm) → OK (send).
Live PLUS produces DRAFT9.20V while HELD remains9.00V and MEASURED stays independent.
OK reviews, then OK/OK confirms9.20V. Live MINUS drafts9.00V and follows the same confirmation.
BACK short/long stops immediately. A subsequent short BACK exits; cancellation never asks for OK.

Arms expire after3s and bind to protocol, exact millivolts and source session. Any non-apply key,
ceiling/session change, chord, refusal, stop or exit invalidates them. Changing the source during a
local running request cancels it. A failed retarget stops; there is no silent restart.
Each retarget rechecks the live load/PD guard; a newly attached load stops rather than bypasses it.
There is no session-wide permission to raise or lower HV. Existing remote explicit confirmation/
inline flags are unchanged, but remote requests obey the cleanup guard.

Release is a command, not proof. `Ui` retains an asynchronous FC release monitor after exit:
engine/scan idle, PD released, measured4.75–5.50V continuously500ms (sampling gaps>250ms reset
the dwell). Eight seconds without verification shows **Unsafe VBUS: unplug source**. Missing
measurement, unplugged0V or elevated VBUS never counts as verified5V. Other active tools,
PD/FC requests and charger-test starts remain blocked until verification; main-tab navigation
and read-only state/captures remain usable. A later safe measurement can clear the unsafe guard.
The charger runner keeps its existing release implementation; no generic wizard framework is added.

### Polish rules

- Footers and status titles fit or end in "..." (`fit_text`); menu titles keep x 20..94, so the V/A
  readout starts at x 100 on menus (x 92 on pages, which have page dots).
- Stale Home is grey throughout (digits, badges, units, PD chip without its wattage, D+/D- tags).
- Coral (`kWatt`) is the watt colour only; neutral groups such as Diag SYS use `kDim`.
- An instruction footer carries no success icon; a Dim-tone footer draws none.
- The Who's attached footer reads "Aborted: <reason>" for Identify only after a run that was cut short.
- Lists longer than their rows show a thin right-edge thumb (Tools hub).
- Counts carry their unit ("15 msg").

## 7 Open questions
1. **Lightning + key.** The label says the lightning (+) key "activates the PD screen". Should a
   long + on Home jump straight to PD trigger? That would conflict with long + = next page.
2. **NTC.** The label shows "NTC" next to USB HID. There may be an NTC (temperature probe) input on
   the PC port's spare pins. If an ADC channel reads it, Lines gets a real `NTC 25.3°C` row and Home
   gets a probe chip next to the MCU temperature. The pin is unknown; check the schematic or the
   `adc` channels.
3. **PD role display.** When the K2 sniffs a contract, which side is the source is known from the
   message direction. Confirm that `pd::` exposes it (for example via the port the Source_Caps came
   from) before showing a role pill on Home.
4. **Slot geometry.** The real button centres are unmeasured; the mockups use 66/54/54/66.
5. **Glyphs.** Adding `Ω ↑ ↓ °` to the 12 px font (`tools/gen_font.py`, about 300 B) would allow
   arrows in S12 status and footer text. Today the mini readout borrows the 16 px arrow.

Closed: **the direction of `kInaForwardSign`**. Positive current means bottom → top, so it draws ↑.
The user verified this with a charger on the bottom and a Pixel on the top.

