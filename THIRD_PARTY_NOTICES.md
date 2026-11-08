<!-- SPDX-License-Identifier: GPL-3.0-or-later
     Copyright (C) 2026 Yaraslau Furman -->
# Third-party notices

Open-K2 Firmware itself is GPL-3.0-or-later (see `LICENSE`). The components below live under
`third_party/` and keep their own licences; none of their files carry our SPDX header. Every licence
named here is compatible with GPL-3.0-or-later, so the combined firmware image can be distributed under
the GPL.

Each entry was checked against the licence file in the tree, not against upstream documentation.

| Component | Where | Version / pin | Licence | Licence file |
|---|---|---|---|---|
| TinyUSB | `third_party/tinyusb` (git submodule, `.gitmodules`) | commit `dae3f9a366bf` | MIT | `third_party/tinyusb/LICENSE` |
| TinyUSB `dhserver` (DHCP server, `lib/networking/dhserver.c`) | inside the TinyUSB submodule | same commit | MIT, (c) 2015 Sergey Fetisov | notice in the head of `dhserver.c` / `dhserver.h` |
| lwIP | `third_party/lwip` (git submodule) | commit `77dcd25a7250`, 2.2.1 line | BSD-3-Clause (Swedish Institute of Computer Science) | `third_party/lwip/COPYING` |
| littlefs | `third_party/littlefs` (vendored, unmodified) | v2.11.0 (`PINNED.txt`) | BSD-3-Clause (the littlefs authors, Arm Limited) | `third_party/littlefs/LICENSE.md` |
| QR Code generator library (C) | `third_party/qrcodegen` (vendored, two small local changes listed in `PINNED.txt`) | see `PINNED.txt` | MIT, (c) Project Nayuki | notice at the top of `third_party/qrcodegen/qrcodegen.c` (no separate file) |
| Spleen bitmap fonts | `third_party/fonts/spleen` | 2.2.0 | BSD-2-Clause, (c) 2018-2026 Frederic Cambus | `third_party/fonts/spleen/LICENSE` |
| Atkinson Hyperlegible Mono | `third_party/fonts/atkinson-hyperlegible-mono` | 2.001 | SIL OFL-1.1 | `third_party/fonts/atkinson-hyperlegible-mono/OFL.txt` |
| JetBrains Mono | `third_party/fonts/jetbrains-mono` | 2.304 | SIL OFL-1.1 | `third_party/fonts/jetbrains-mono/OFL.txt` |

## Notes

- **TinyUSB is MIT, not Apache-2.0.** The top-level licence and the headers of the files we build
  (`src/tusb.c`, `device/`, `class/{cdc,hid,msc,net}`, `portable/st/stm32_fsdev`) are MIT. Other parts of
  the TinyUSB repository (board and vendor-SDK code under `hw/` and `lib/`) carry their own licences, but
  none of that is compiled here: the `Makefile` builds only the files listed under `TUSB_SRCS`, plus
  `dhserver.c` in the network build.
- **lwIP is compiled only in the `K2_NET=1` build.** The default image does not contain it.
- **Fonts.** The font files are build-time inputs: `tools/gen_font.py` rasterises them into
  `src/ui/fonts/*.cpp` and `include/k2/ui/font_aa.hpp`, and the firmware never reads a font file. Those
  generated glyph bitmaps are derived from Atkinson Hyperlegible Mono (the screen font) and, for four
  arrows (U+2190..U+2193), from JetBrains Mono, and remain under the SIL OFL-1.1 terms of those fonts;
  the copyright lines are `Copyright 2020-2024 The Atkinson Hyperlegible Mono Project Authors` and
  `Copyright 2020 The JetBrains Mono Project Authors`. Neither OFL file declares a Reserved Font Name.
  Spleen is kept as a 1-bit fallback source (`tools/gen_font.py --source spleen`); its BDF files are not
  linked into the default image. JetBrains Mono is also used by `docs/ui_mockups/render_mockups*.py` to
  draw mockups.
  Provenance and checksums: `third_party/fonts/PINNED.txt`.
- **Not in the tree.** The Python tools use ordinary PyPI packages (for example Pillow,
  fontTools, freetype-py, matplotlib, pytest, pyzbar) that are installed by the user and not redistributed here; the build runs inside a Debian
  image with the GNU Arm Embedded toolchain, also not redistributed.
