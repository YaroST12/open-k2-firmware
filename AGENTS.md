# AGENTS.md

## Context Control (High Priority)

Overrides built-in parallel pre-reading and whole-file reading defaults. Keep working context lean and preserve prompt cache.

- **Narrow search, focused reading (no blanket pre-reading)**: To locate code, first use `rg -l` / `rg -n` to pin down exact files and line numbers, then read only the files to be modified or their direct call chain. No speculative batch parallel `read_file` calls (`<= 3` uncertain files read in parallel per turn), and never bulk-read a directory or `docs/agents/` up front.
- **Read large files in sections**: For files over 200 lines, never call `read_file` without `StartLine` and `EndLine`. First locate target lines with `rg -n`, then read only the needed range (`<= 200` lines per read). Only read top-of-file includes/declarations when edits require them.
- **Limit `run_command` output**:
  - Scope searches to relevant trees (`src/ include/ tests/ tools/`) and skip noisy paths (`third_party/`, `build*/`, `host-build*/`, `runs/`, e.g. `-g '!third_party/**'`).
  - When running `rg`, `find`, `git diff`, `git log`, or `ls`, default to `-l`, `--stat`, `-m 20`, or `| head -n 60` so output stays under the 10 KB truncation limit and never triggers a follow-up read of background logs.
  - Filter verbose build/test output (e.g. `| tail -n 40` or error/summary lines) instead of dumping full logs.
- **Use subagents selectively (protect prompt cache)**: Subagents start with a cold context and duplicate reads between main and subagent sessions. Prefer narrow `rg` + sectioned `read_file` in the main session for routine lookups, localized bugs, and small edits; reserve subagents/swarms for genuinely broad multi-module investigations or parallel isolated worktree implementations.

## Project Orientation (Read on Demand)

Open only the doc section relevant to the current task:

- Feature status, known bugs, and next-up priorities: `docs/ROADMAP.md` (1,400+ lines — search with `rg -n` and read only relevant sections)
- Build, gate, debug, and hardware summary: `docs/agents/HANDOFF.md`
- Coding standards: `docs/CODING_STANDARDS.md`
- Commit message format: `docs/agents/COMMITS.md`
- UI layout & design spec: `docs/UI_DESIGN.md`
- Hardware test procedures: `docs/HW_TEST_PROCEDURES.md`

## Roadmap Tracking (`docs/ROADMAP.md`)

Every agent working on this repository must reference and maintain `docs/ROADMAP.md`:

- **Before starting work**: Search `docs/ROADMAP.md` (`rg -n`) for existing entries under `## Current bugs`, `## Specified work (not started)`, `## Not verified on hardware`, or feature tables so work aligns with prior specs, root-cause notes, and priorities.
- **After completing work or finding bugs**: Update `docs/ROADMAP.md` with your progress — mark completed/partial items with the commit hash, add newly discovered bugs or follow-ups under `## Current bugs`, and move items between sections as appropriate.
- **Hardware verification honesty**: Only mark an item `verified` / move it to `## Verified on hardware` (with date, hardware setup, and evidence) after observing it on a real K2; passing host tests alone stays `not verified`.

## Critical Workflow & Hardware Guardrails

- **Host tests**: `make K2_WERROR=1 host-build/<test_name>` and run `./host-build/<test_name>` (e.g. `test_pd_link`, `test_ui_connection_page`).
- **Firmware build & gate**: `make docker-build K2_WERROR=1` (no host ARM toolchain); `make gate` runs full checks.
- **Physical K2 hardware (strict)**:
  - **Always ask the user first** before flashing, rebooting, or running `tools/k2_remote.py`, `tools/k2_flasher.py`, or `make smoke` against a connected K2 — the device may be in active use.
  - **Never pass `--no-backup`** to `tools/k2_flasher.py`.
- **Git hygiene**: Never `git push`, create tags, or stage `.claude/`. Always use explicit `git add <paths>`.
