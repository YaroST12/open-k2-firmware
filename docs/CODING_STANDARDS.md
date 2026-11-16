# Coding standards

Short rules for everyone who writes code in this repository. Design and module map: `docs/ARCHITECTURE.md`. Feature
status: `docs/ROADMAP.md`.

1. **Self-contained drivers.** One header plus one .cpp per driver, in its own `k2::` namespace. A driver owns its pins,
   registers, clocks, DMA and IRQ setup and exposes a small init/poll/read API. Other code uses only that API; reaching into
   another driver's registers or globals is a bug. Protocol and calculation logic is pure code over buffers and values.
2. **C++23 bare metal.** Flags `-std=c++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit`. No
   `new`/`delete`, no heap, no `<vector>`, `<string>`, `<iostream>`, `<format>`, `std::function`. Registers through
   `constexpr` addresses. ISRs, the vector table and meta objects keep `extern "C"` plus their `section`/`used` attributes.
   `make gate` fails on `malloc`, `operator new` or `__cxa_` in the map. Never touch SRAM above the mapped 96 KB.
3. **Unique command ids.** Remote command ids live in `include/k2/core/remote_proto.hpp`, `tools/k2_remote.py` and
   `tools/web/k2proto.js`. Take the next free id inside the subsystem range declared in `remote_proto.hpp` (core 0x01-0x1F, debug 0x20, system 0x30, pd 0x40, tools 0x50, storage 0x60, black box 0x70) and mirror it in all three. `make gate`
   (`tools/check_cmd_ids.py`) fails on a collision, a name/id mismatch or an id outside every range.
4. **Additive edits to shared files.** `src/ui/ui_pages.cpp`, the remote dispatcher, `k2_remote.py`, `k2proto.js`, `Makefile`:
   add new entries, do not reorder, rename or reformat existing ones. Keeps parallel branches mergeable.
5. **Tests first for pure logic.** Decoders, parsers, formatters and state machines get a host test under `tests/` before
   the change (`make test`: C++, Python, node). Hardware-facing code is verified on the device.
6. **`make gate` before every commit, after the last commit.** Its 11 steps: build with `-Werror` on our sources, section
   layout and four symbols, stack budget (worst call depth against the PSP/MSP, no interrupt priorities), map (no
   `malloc` / `operator new` / `__cxa_` / 64-bit divmod), hot block inside the zero-wait flash window, command ids,
   64-bit integer guard, the host tests, the ASan/UBSan host tests (g++ and clang), the size delta against
   `tools/size_baseline.txt`, and the perf check (device selftest; SKIPPED without a matching K2). Text growth over 5 %
   is a WARNING: explain it in the commit body. `K2_WEB=1` adds the node tests and the `K2_NET=1` / `K2_WEBUSB=1`
   variant builds. Commit format: `type(scope): summary` plus Summary / Rationale / Key Changes sections
   ([agents/COMMITS.md](agents/COMMITS.md)).
7. **Hardware validation only via `make smoke`.** It is gate plus `tools/hw_smoke.py` (auto-DFU flash, ping and git hash,
   log capture, state check, every page screenshot, harmless remote exercise). Do not hand-roll flash/probe loops overnight.
8. **One owner of the K2.** Every host tool takes an exclusive lock (`tools/k2_lock.py`, flock on `/tmp/k2-device.lock`,
   wait `K2_LOCK_WAIT` s, default 120) in the shared `LinuxHidTransport` open path; `flash --auto-dfu` and
   `hw_smoke.py` hold it across the whole flash and re-enumeration. Never open the HID device by another route.
9. **Hardware safety.** Unattended runs never send a trigger (fc / pd request), Identify, DFU-failure path or fault test,
   and never do anything that changes VBUS. Smoke is read-only plus page navigation. Triggers, HV tests and flash writes
   outside the app region (e-marker library) need a human at the device and an explicit request.
