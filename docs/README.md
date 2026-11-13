# Documentation index

Index of the user and developer documentation. The project overview, build, flash and recovery steps are in the root
[`README.md`](../README.md).

## Using the K2

| Document | What it covers |
|---|---|
| [`USER_GUIDE.md`](USER_GUIDE.md) | Getting started and a reference of every page, tool and setting |
| [`QR_SHARE.md`](QR_SHARE.md) | The QR frame format, the record schemas and the report-card format versions |
| [`EMARKER_JSON.md`](EMARKER_JSON.md) | The saved-cable JSON format |
| [`site/r/`](site/r/) | Source of the charger-test results page that a report-card QR code opens |

## How the firmware works

| Document | What it covers |
|---|---|
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Code layout, boot, flash map, USB modes, drivers, the PD engine, data flows |
| [`UI_DESIGN.md`](UI_DESIGN.md) | Screen design: geometry, palette, components, navigation, per-page layout |
| [`K2_PORTS_SPEC.md`](K2_PORTS_SPEC.md) | What each of the K2's two USB-C ports can do, with the evidence |
| [`SOURCE_SINK_DETECTION.md`](SOURCE_SINK_DETECTION.md) | How the firmware tells what is connected to each port |
| [`CONNECTION_MATRIX.md`](CONNECTION_MATRIX.md) | Every way to hook up the K2 (power, partners, cables, orientation), what it must do, and the evidence per row |
| [`CHARGER_TEST_FLOW.md`](CHARGER_TEST_FLOW.md) | The charger test: units, sessions, order |
| [`DEBUG_STREAMING.md`](DEBUG_STREAMING.md) | The sources the device holds and the live stream that records a hardware run |
| [`PROTOCOL_LAB.md`](PROTOCOL_LAB.md) | The CDC debug-shell commands that drive every charging protocol by hand |

## Charging protocols and chargers

| Document | What it covers |
|---|---|
| [`FASTCHARGE_PROTOCOLS.md`](FASTCHARGE_PROTOCOLS.md) | The D+/D- and UFCS protocols as the firmware speaks them, with captures |
| [`CHARGING_PROTOCOL_MAP.md`](CHARGING_PROTOCOL_MAP.md) | Fast-charge families by owner, transport and power tier |
| [`CHARGER_QUIRKS.md`](CHARGER_QUIRKS.md) | Evidence log of charger behaviour patterns and what the firmware does about each |
| [`captures/`](captures/) | Raw captures referenced by the quirks log |

## Project

| Document | What it covers |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) | Status of every feature and what is verified on hardware |
| [`HW_TEST_PROCEDURES.md`](HW_TEST_PROCEDURES.md) | Step-by-step on-device test procedures, with a results log |
| [`MATH_SELFTEST.md`](MATH_SELFTEST.md) | On-device `selftest` shell command, 7 DWT cycle-timed kernels, and A/B benchmark workflow |
| [`CODING_STANDARDS.md`](CODING_STANDARDS.md) | Rules for code and commits |
| [`TESTING.md`](TESTING.md) | The host world replay: running it, making a scenario from a recording, the timing model and the reboot world |

## Mock-ups and images

| Path | What it holds |
|---|---|
| [`ui_mockups/`](ui_mockups/) | The generated UI mock-ups (`render_mockups.py`) and their pixel tables; `v1/` is the superseded first design |
| [`ui_review_2026-11-06/`](ui_review_2026-11-06/) | Rendered screens used as the gallery in `UI_DESIGN.md` |
| [`guide/img/`](guide/img/) | Screenshots used by the user guide |

## Agent material

[`agents/`](agents/README.md) holds handoffs, dated reviews, plans and research scratch written for coding agents. It
is working material and may be out of date.
