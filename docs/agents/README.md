# Working material for coding agents

This folder holds notes written for coding agents that pick up work on this repository: handoffs, dated review
snapshots, implementation plans and research scratch. It is working material, not documentation. Each file is a
snapshot from the date it names (or from the session that wrote it), the code has moved on since, and some of it
may be out of date or wrong. Where a file disagrees with the code, the code is right; where it disagrees with a
document in [`docs/`](../README.md), the document in `docs/` is right.

Users and contributors should start from the [documentation index](../README.md) and the root
[`README.md`](../../README.md).

## Handoffs

| File | What it is |
|---|---|
| [`HANDOFF.md`](HANDOFF.md) | Entry point for an agent: hardware summary, how to build, gate, flash and debug, the first-contact backup procedure, the open-issues table |
| [`HANDOFF_PD.md`](HANDOFF_PD.md) | Continuation notes for USB-PD sink, trigger and charger-test work (2026-11-05) |
| [`HANDOFF_UI.md`](HANDOFF_UI.md) | Continuation notes for the 240x240 UI rework (2026-11-05) |
| [`HANDOFF_2026-11-06.md`](HANDOFF_2026-11-06.md) | End-of-session state of 2026-11-06: what was merged, the e-marker spoof result, open items |

## Plans, reviews and research

| File | What it is |
|---|---|
| [`DETECTION_IMPLEMENTATION_PLAN.md`](DETECTION_IMPLEMENTATION_PLAN.md) | Review of the trigger and detection research notes against the code, a ranked gap list and a step plan (2026-11-07). Reserves report-card format v7 |
| [`UI_REVIEW_2026-11-06.md`](UI_REVIEW_2026-11-06.md) | UI review snapshot with before/after mock-ups; the images are in [`../ui_review_2026-11-06/`](../ui_review_2026-11-06/) |
| [`FEATURE_PROTOTYPES.md`](FEATURE_PROTOTYPES.md) | Ideas for features that are not built (limiter, eFuse, bench supply, profiler) and the actuators they would use |
| [`TRIGGER_RESEARCH_NOTES.md`](TRIGGER_RESEARCH_NOTES.md) | Public-source research on fast-charge trigger sequences; reviewed against the code, the plan above wins on conflicts |
| [`QC_RESEARCH.md`](QC_RESEARCH.md) | Reverse-engineered Quick Charge sink behaviour compared with the implementation |
| [`LINUX_TYPEC_SECOND_SOURCE.md`](LINUX_TYPEC_SECOND_SOURCE.md) | Whether a Linux laptop's Type-C sysfs can serve as a second reference for PDOs and e-markers |
| [`research/xiaomi-10v5a.md`](research/xiaomi-10v5a.md) | How a phone reaches 10 V / 5 A over PD, from public kernel source |

## Rules for agents

| File | What it is |
|---|---|
| [`GEMINI.md`](GEMINI.md) | Commit message format (header, Summary / Rationale / Details, co-author trailer) |
| [`../../.github/agents/`](../../.github/agents/) | Custom agent definitions (firmware development, UI design, UI QA). They stay under `.github/` because GitHub reads them there |

Coding rules for everyone are in [`../CODING_STANDARDS.md`](../CODING_STANDARDS.md).
