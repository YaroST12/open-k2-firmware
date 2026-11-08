<!-- SPDX-License-Identifier: GPL-3.0-or-later
     Copyright (C) 2026 Yaraslau Furman -->
# Provenance

How this project came to know what it knows about the WITRN K2 hardware, and what it does and does not
contain.

## Firmware code

All firmware code in this repository (`src/`, `include/`), the host tools (`tools/`, `webhid-flasher/`)
and the tests were written from scratch. No third-party firmware code, disassembly output or decompiled
source is included. Third-party components are listed, with their licences, in
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Hardware knowledge

What the firmware does with the K2's hardware (pin map, clock tree, which chip sits on which bus, how the
FUSB302 and the power monitor are driven, the boot and power-on behaviour) was learned from the
author's own unit: probing, measurements and logic captures on the running device. It is documented here
as facts about the hardware, in the author's own words, and then confirmed or corrected on hardware
(`docs/ROADMAP.md` says which features are verified). Claims in the docs carry a provenance tag such as
"reverse-engineered" (from a wire capture), "inferred" or "measured".

No analysis of any other firmware application for this device is included in this repository; the author's
reverse-engineering notes are private.

## Interoperability constants that are kept

A few values cannot be written independently because the device or the host side fixes them, so the
project keeps them as plain functional constants:

- the bootloader's frame protocol and its byte substitution table for the `.k2` container, in
  `tools/k2_flasher.py` and `webhid-flasher/k2boot.js` (without them the flasher cannot talk to the
  bootloader or build an image it accepts);
- the model GUID the bootloader checks in the container;
- the USB vendor and product ID of the K2's HID interface, kept so the host side sees the same device
  identity;
- cable and VDO identity constants (e-marker answers) captured from the author's own cables, used by the
  Spoof feature and the built-in cable identities.

## What is not here

- The bootloader stays on the device (`0x08000000..0x08003800`, never written by this firmware) and is
  **not distributed** with this project.
- No vendor firmware image, disassembly or data table is part of this repository or its history.

## Trademarks

WITRN is a trademark of its owner. This project is independent and is not affiliated with, endorsed by
or sponsored by WITRN. Product and company names are used only to say which hardware the firmware runs
on.
