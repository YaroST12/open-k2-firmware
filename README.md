# Open-K2 Firmware

Open-source bare-metal C++23 firmware for the **WITRN K2 USB-PD Analyzer / Power Meter** (Artery AT32F403A). It is flashed through the K2's own bootloader, which it never touches.

Not affiliated: WITRN is a trademark of its owner, and this project is independent of it. See
[`PROVENANCE.md`](PROVENANCE.md) for where the hardware knowledge comes from and what is (and is not)
in this repository, and [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the bundled third-party
components and their licences.

## What it does

- **Power meter**: volts, amps, watts and energy, trend graphs, D+/D-/CC1/CC2 line levels.
- **USB-PD analyzer**: passive by default; decodes the charger's offers (Fixed, PPS, AVS, EPR), reads the partner's identity, and can request a PDO or a PPS/AVS voltage (**PD trigger**).
- **Fast-charge triggers** on D+/D-: QC, AFC, FCP/SCP, VOOC and a one-click auto detector.
- **Charger test**: guided no-load voltage sweeps with per-step results, saved as report cards you can share as a QR code.
- **Cables**: e-marker read and save (library of up to 140), cable-identity Spoof, cable resistance.
- **Black Box**: a PD trace recorder with trigger-frozen captures that survive power-off.
- **USB**: remote control (`tools/k2_remote.py`), a browser dashboard, a read-only drive, and a WebHID flasher.

How to use all of it: **[`docs/USER_GUIDE.md`](docs/USER_GUIDE.md)**, with a short Getting started part and a full reference of every page, tool and setting.
Feature status, with what is verified on hardware and what is not, is in `docs/ROADMAP.md`.
Step-by-step hardware test procedures for every protocol and feature, with a results log: `docs/HW_TEST_PROCEDURES.md`.

## Hardware

- **MCU**: AT32F403AVGT7 (Cortex-M4F, 240 MHz from the 8 MHz crystal, 1 MB flash, 96 KB SRAM mapped by default).
- **Display**: ST7789V 240x240 on SPI3 (PA15 CS, PB3 SCK, PB4 DC, PB5 MOSI, PB6 RESET, PB7 backlight as plain GPIO).
- **Buttons**: PB14 BACK, PA14 MINUS, PA9 OK, PA13 PLUS. The pin map is `include/k2/drivers/pins.hpp`.
- **Buses**: FUSB302 on PB10/PB11, power monitor (INA226 on this unit) on PB8/PB9, both bit-banged I2C.
- **Debugging**: no debugger (SWJ is off, its pins are buttons and LCD lines). The channels are USB log lines (`[TAG] ...`), the Black Box and the screen.
- **Flash**: `0x08000000..0x08003800` is the bootloader and is never written. The app is linked at `0x08003800`; the layout rules and the writable regions are in `docs/ARCHITECTURE.md` (code: `include/k2/drivers/flash_map.hpp`).

---

## Download / flash

- **From the browser** (Chrome or Edge, WebHID): open <https://yarost12.github.io/open-k2-firmware/>, press **Connect**, pick the K2, then **Flash release vX.Y**. The page offers the newest tagged release only; it downloads the image from the site itself and checks its SHA-256 before flashing. The K2 needs USB HID enabled (Settings > USB Link) to be sent into the bootloader without the button, or hold **MINUS** while plugging in; see "Flashing" below.
- **A release asset**: every release on the [Releases page](https://github.com/YaroST12/open-k2-firmware/releases) carries `open_k2.k2` (the container to flash), `open_k2.bin`, `open_k2.elf` and `SHA256SUMS`. Flash the `.k2` with the page above (file picker) or `python3 tools/k2_flasher.py flash open_k2.k2 --auto-dfu`.
- **CI builds** of `main` and of pull requests are workflow artifacts (Actions > ci), not releases; the flasher page never offers them.

Cut a release (maintainers): `git tag vX.Y && git push origin vX.Y`. `release.yml` runs the full gate with the version string derived from the tag by `tools/fw_version.py` (`vX.Y` gives `X.Y.0-0-<hash>`, `vX.Y.Z` gives `X.Y.Z-0-<hash>`; builds after the tag read `X.Y.0-<commits since>-<hash>`; major and minor 0 to 15, the container stores them in one byte), creates the GitHub Release with the assets and notes, and redeploys the Pages site so the flasher follows. One-time repo setup: Settings > Pages > Source: GitHub Actions.

## Build

The host has no ARM toolchain; everything builds in the `open-k2-builder` Docker image.

```bash
make docker-image     # once
make docker-build     # build/open_k2.elf, .bin, .k2 (the container to flash)
make test             # host unit tests (C++, Python, node)
make gate             # build with -Werror, invariants, map check, command ids, all tests, size
```

`make gate` runs the whole test suite: a cold run takes about 10 minutes in CI, and a local incremental run is much faster. A normal `make docker-build` is quick.

**`third_party/` must be populated.** TinyUSB and lwIP are git submodules (`third_party/tinyusb`, `third_party/lwip`), and a fresh clone or a new git worktree has them empty, so the build fails. Run `git submodule update --init third_party/tinyusb third_party/lwip`, or in a new worktree copy them from a populated checkout: `cp -a /path/to/open-k2-firmware/third_party/{tinyusb,lwip} third_party/`.

All firmware is C++23 under `src/` and `include/` (only TinyUSB and lwIP are C). One image holds every USB mode except the network one: the web interface (lwIP, CDC-NCM, HTTP dashboard) is compiled out by default to free RAM and flash and comes back with `make docker-build K2_NET=1` (see "USB modes").
Build identity stamps are non-intermediate: a changed git hash recompiles the version object
even when the host-test object cache uses `.SECONDARY`.

### Code navigation (clangd, IDEs)

Every build writes `compile_commands.json` at the repo root (gitignored): each compile recipe records its exact command in `<object>.cmd`, and `tools/compdb.py` merges them (`make docker-build` for `build/`, `make test` for the host tests in `host-build/`, `make compdb` to merge what exists). Firmware entries use the ARM compiler, files only the tests build (`tests/*.cpp`) use the host `g++`; a file built by both is listed once, as firmware. `build-net/` and other `BUILD_DIR`s never touch the file, and an unchanged build leaves it untouched (no reindex).

The ARM compiler is not on the host, so the first `make docker-build` also copies its system headers (newlib, libstdc++, 24 MB) out of the image into `.cache/arm-none-eabi/` and `compdb.py` adds them to the firmware entries; `make nav-headers` redoes the copy after a toolchain image change. The committed `.clangd` drops the GCC-only flags clangd does not know. Open the repo with clangd from the repo root; `clangd --check=src/ui/ui_home.cpp` is the quick test (its "errors" count includes harmless "tweak: ... FAIL" probes; real parse errors are the lines without `tweak:`). A user-level `~/.config/clangd/config.yaml` that removes `-m*`/`-f*` flags makes clangd parse the firmware as armv4t instead of Thumb-2; drop that rule for exact parsing.

## USB link modes

**USB is off by default.** After a flash the K2 does not enumerate on the HID port until you enable it on the device: Settings > USB Link. Choices, applied after a replug (kept in flash):

| Mode | What the host sees |
| :--- | :--- |
| Off (default) | nothing; the remote, the log and the smoke test have no channel |
| HID | the logger and remote channel (VID 0x0716 / PID 0x5060), used by `k2_remote.py`, `pd_trace.py`, WebHID |
| HID + Drive | HID plus the read-only `K2 DRIVE` (status, log, saved cables as JSON) |
| HID + Network (**`K2_NET=1` builds only**) | HID plus a CDC-NCM network link; the K2 serves the dashboard at `http://192.168.7.1/` (the host gets 192.168.7.2 by DHCP; Linux, macOS and Windows 11 need no driver) |

The network mode is not in the default build (`K2_NET=0`: no lwIP, no NCM, no HTTP server, no embedded dashboard; Settings > USB Link shows only HID and Drive, and a stored "network" setting runs as HID). To get it back, build with `make docker-build K2_NET=1` and flash that image. `make gate` also builds that variant in `build-net/`, so it keeps compiling. The WebHID dashboard in `tools/web` works in every build.

Drive and Network cannot be combined (USB packet memory). **The HID port needs a USB C-to-A cable**: a C-to-C cable does not enumerate on the left port. Enable HID in Settings > USB Link before you use any `tools/k2_remote.py` command or `make smoke`.

**Orientation:** open Settings > Autorotate, select Auto or
Manual 0/90/180/270 deg with short PLUS/MINUS, then press OK to apply. Auto follows gravity;
Manual stays fixed. Orientation choices last for the current boot.

---

## Flashing

1. Get the K2 into DFU mode: with the app running and USB HID on, `--auto-dfu` does it for you (below). Otherwise hold **MINUS** while plugging in the PC USB cable; this always works, also for a K2 that runs nothing useful.
2. Check the connection (DFU mode only): `python3 tools/k2_flasher.py probe`
3. Flash:
   ```bash
   python3 tools/k2_flasher.py flash build/open_k2.k2              # K2 already in DFU
   python3 tools/k2_flasher.py flash build/open_k2.k2 --auto-dfu   # app running, no button; what `make flash` does
   python3 tools/k2_flasher.py flash build/open_k2.k2 --monitor --timeout 20   # then show the boot log
   python3 tools/k2_remote.py dfu --confirm                         # only enter DFU, flash later
   make smoke                                                       # gate, flash, exercise the device, write build/smoke/<hash>/
   ```
   Every `flash` first makes a full backup into `runs/k2-backup-<UTC time>` (`--backup-dir DIR` to choose,
   `--no-backup` to skip; `make flash FLASH_ARGS=--no-backup` passes it on). `make smoke` flashes with `--no-backup`.

The K2 is a shared device: do not run ad-hoc flash loops, and nothing that changes VBUS runs unattended.

### How DFU entry works and how to recover

`DFU_ENTER` (remote command 0x30, confirm payload `DFU!`, also the "Enter DFU..." button of the
WebHID dashboard) makes the app program the "K200" model signature word at 0x08004010 to 0 (a
pure 1 -> 0 bit-clear, no erase, from RAM with interrupts off), verify it and reset. The
bootloader no longer recognises the app and stays in DFU. Flashing erases and rewrites that page,
so the signature comes back with the next firmware.

- The bootloader region 0x08000000-0x080037FF is never written; the write path accepts only the two
  half-words of the marker word, and only the value 0.
- Neither state (not the app, not the bootloader): hold **MINUS** while plugging in USB, then flash.
- If the bit-clear does not force DFU the app simply boots again (only one marker word changed). The
  flasher then reports "the app is still answering"; next try is the commit marker 0x08004400, one
  constant (`kDfuMarkerAddr` in `include/k2/drivers/flash.hpp`).

---

## USB drive

The K2 enumerates as a composite device: the HID logger (unchanged) plus a read-only mass-storage drive named `K2 DRIVE` (when Settings > USB Link has the Drive on; HID-only is a runtime mode of the same firmware image, and Drive cannot be combined with the network mode). In the network mode (a `make docker-build K2_NET=1` image only; Settings > USB Link > Network, replug) the K2 serves the dashboard itself: open http://192.168.7.1/ (the host gets 192.168.7.2 by DHCP; Linux, macOS and Windows 11 need no driver). Files:

- `README.TXT`, `STATUS.TXT` (live V/A/W, source caps, cable), `LOG.TXT` (recent log lines)
- `CABLES/<name>.JSN`: one JSON file per saved cable (see `docs/EMARKER_JSON.md`); manage them with `python3 tools/k2_remote.py emarker list | save | rename <id> "<name>" | delete <id> --confirm | spoof <id>|builtin:<id|name>|off`
- `NATIVE/` (only when the unit has a native e-marker library): `LIBRARY.BIN` (the raw 2 KB page at 0x080E3800) and `LIBRARY.TXT` (header, checksum, one line per record). Read-only.
- `SAVEDLOG.TXT` after `store save-log`, plus `RIPPLE.CSV` and `BLACKBOX/` placeholders

Save the cable currently read by the K2 with `python3 tools/k2_remote.py store save-emarker` (same as `emarker save`), list with `store list`. The native pages at 0x080E3000/0x080E3800 are never written. Flash is written only by these commands. After a save, unplug and re-plug (or re-mount) the drive to see the new files. Details: `docs/ARCHITECTURE.md`, section "USB drive (MSC), flash store and FAT synthesis".

---

## Browser dashboard (prototype)

`tools/web/k2.html` is a card dashboard over WebHID (Chrome or Edge): power with a V/A/W chart, USB-PD, cable e-marker, lines and ADC, ripple capture, device and DFU controls, and the log. Sections for identity (with Identify device), PD trigger and fast-charge trigger appear only when the firmware answers their commands.

```bash
make web      # serves tools/web on http://localhost:8765/k2.html
```

Click **Connect** and pick the K2. On Linux the hidraw node needs the udev rule with `MODE="0666"` for VID 0716, and no other tool may hold the device. Details: `docs/ARCHITECTURE.md`, section "WebHID dashboard".

### WebHID flasher (GitHub Pages)

`webhid-flasher/index.html` is a standalone firmware flasher over WebHID — no Python, no DFU button dance: pick a `.k2`/`.bin` file, press **Flash firmware**. If the app is running it is sent into the bootloader via `DFU_ENTER` first (press **Connect** again once the K2 re-enumerates as `WITRN.K2`); a K2 already in DFU (MINUS held while plugging in) is flashed directly. Same flow as `k2_flasher.py flash`: erase, write, read-back verify, commit marker, reboot. The page refuses any image whose erase span would reach the device-storage area (nothing above 0x080E0000 is ever erased or written); an image that reaches the littlefs volume (0x080B3000 and up) is flashed with a note, and the K2 formats a new volume at the next boot. A flash that stops half way leaves the commit marker clear, so the K2 stays in the bootloader and the next attempt starts over.

**Backup and restore.** The backup is full by default: the whole 1 MB (0x08000000..0x08100000), read twice and compared. The box **Back up this K2 first (full backup)** is ticked every time the page loads; with it ticked, **Flash** (file or release) makes the backup first, downloads it, then flashes, in one click. Unticking it shows a warning: the flash erases the firmware that is on the K2 now, and without a backup it cannot be brought back. **Back up the K2** makes a backup without flashing (the K2 goes into the bootloader the same way as for a flash). The download is one ZIP, `K2_BACKUP_<version>_<UTC time>.zip`, with `K2_RECOVERY_<version>.k2` (the file to keep), `flash.bin`, `app.bin`, `bootloader.bin`, `native_nvram.bin` and `manifest.json`; the same files are also offered one by one under "Individual files". To restore, pick the ZIP (or the `.k2` inside it) as the firmware file, or press **Restore this backup** right after the backup. The recovery image stops before the littlefs volume (0x080B3000), so restoring puts the firmware back and leaves the volume untouched. The native data area (0x080E0000, settings and e-marker records) is saved in `native_nvram.bin` but is never restored by the page. `manifest.json` has `backup_kind` (`full`, `app-only` or `partial`) and the SHA-256 of `native_nvram.bin`. If the bootloader refuses reads above 0x080E3000 the backup is marked `partial` and the page says that the device-storage part is missing. The advanced option "application area only" reads 0x08000000..0x080E3000 and says so in the result. The Python flasher does the same: `k2_flasher.py backup <folder>` is full and writes the ZIP next to the folder (`--app-only` for the application area only; `--full` is accepted and changes nothing), and `k2_flasher.py flash` makes a full backup into `runs/k2-backup-<UTC time>` (or `--backup-dir`) first unless `--no-backup` is given.

```bash
make webhid   # stages the page with tools/web/k2proto.js under build/webhid/ and serves it on http://localhost:8765/
```

The page is published to GitHub Pages by `.github/workflows/pages.yml` on pushes to `main` and on manual runs; `release.yml` starts it on `main` after it creates a release (a deploy from a tag ref does not go live, so a release made by hand in the GitHub UI needs a manual "Run workflow" on `main`) (one-time setup: Settings → Pages → Source: GitHub Actions; the workflow stages `tools/web/k2proto.js` next to the page, so nothing is tracked twice), reachable at `https://<owner>.github.io/<repo>/`. Pages never builds firmware: it adds the newest release's `open_k2.k2` as `fw/open_k2.k2` plus `fw/latest.json` (`tools/stage_fw.py`, checked against the release's `SHA256SUMS`), and the page's release card reads them (GitHub release assets carry no CORS headers, so the page cannot fetch them from GitHub directly). Without a release there is no `fw/` and the card stays hidden; `make site` stages a local build the same way, labelled `local-<git>`. Protocol code: `webhid-flasher/k2boot.js` (bootloader protocol + `.k2` container, byte-tested against `tools/k2_flasher.py` in `tests/test_k2boot.mjs`).

### Site layout and the results page

The Pages site is the flasher at the root plus everything under `docs/site/`, which the workflow copies over the root
(`cp -r docs/site/. _site/`), so the flasher keeps its address and other pages can sit beside it.

| Path on the site | Source | What |
|---|---|---|
| `/` | `webhid-flasher/` | the WebHID firmware flasher |
| `/fw/` | newest GitHub release | `open_k2.k2` + `latest.json`, staged by the workflow (absent until a release exists) |
| `/r/` | `docs/site/r/` | charger-test results: the page a K2 report-card QR code opens |

The K2 shows a report card as a link code (long OK on the charger test Results page or in Tools, Report cards); a phone
camera opens `https://yarost12.github.io/open-k2-firmware/r/#<card>` and the page shows one badge per capability the charger
achieved, with the full results below. The card travels after the `#`, so it never reaches the server; the page is static
(`card.js` decoder and badge model, `view.js` drawing, `index.html`), loads nothing from the network and works offline once
opened. Format and golden tests: `docs/QR_SHARE.md`, `tests/test_cardpage.mjs`. `make site` stages the whole tree under
`build/site/` the way the workflow does.

---

## Recovery

### First contact with a new K2 (do this before flashing anything)

The area 0x080E0000..0x080FFEFF holds data that was on the unit before this firmware: settings, e-marker
records, assets. This firmware never erases or programs it (the volume ends at 0x080E0000; the only
erasable page there is an old raw store of ours, proven by its own signature), but copy it out first so it
can always be restored:

1. Put the K2 in DFU (hold MINUS while plugging in USB), then `python3 tools/k2_flasher.py probe`.
2. `python3 tools/k2_flasher.py backup /path/outside/the/repo/k2-dump` (full by default). It reads
   0x08000000..0x08100000 twice and compares. Exit 3 means the bootloader refused above some address; the
   readable part is saved and `manifest.json` names `last_readable_address`.
3. Only if the dump is partial: `python3 tools/k2_flasher.py flash build/open_k2.k2` (safe: no wipe, no
   format of the device-storage area), enable USB HID in Settings > USB Link, replug (left port needs a C-to-A cable),
   then `python3 tools/k2_remote.py flash-dump /path/outside/the/repo/k2-dump`.
4. Store the dump outside the repo, then flash and use this firmware normally.

Whether the bootloader reads above 0x080E3000 is unverified.

### Erase this firmware's data and leave the app

`factory-wipe` removes everything this firmware keeps in flash (the littlefs volume at
0x080B3000..0x080E0000 and, on units that ran older builds, legacy stores above 0x080E4000) and leaves the K2
in the bootloader, ready for another `.k2` container:

1. Flash this firmware (`make flash`) and enable USB HID in Settings > USB Link, so the remote works.
2. `python3 tools/k2_remote.py factory-wipe --confirm`. The K2 unmounts the volume, erases its 90 sectors and
   the legacy pages that carry our own old signature (the rest of 0x080E0000.., the bootloader and
   the info page are never touched), reads every erased one back
   as 0xFF, clears its own backup registers (DR22..DR37) and enters DFU without booting the app
   again. The command prints the sector counts and exits 0; on any erase or verify failure it exits 6, the
   K2 stays in the app and does not enter DFU.
3. While the K2 is in DFU, flash the next container with `python3 tools/k2_flasher.py flash <file.k2>` (no
   `--auto-dfu`). Do not replug the K2 before this step: the app marker is already cleared, so it stays in
   DFU either way.

## License

Open-K2 Firmware is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
License for more details.

You should have received a copy of the GNU General Public License along with this program (the file
[`LICENSE`](LICENSE)). If not, see <https://www.gnu.org/licenses/>.

Copyright (C) 2026 Yaraslau Furman. Every first-party source file carries
`SPDX-License-Identifier: GPL-3.0-or-later`. Code under `third_party/` keeps its own licence, listed in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

Not affiliated with WITRN. WITRN is a trademark of its owner.
