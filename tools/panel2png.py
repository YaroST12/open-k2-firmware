#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Convert the K2_DUMP host-test panel dumps (k2_<name>.panel, 240x240 RGB565 little endian) to PNG.

Usage: K2_DUMP=dir host-build/test_ui_capture; tools/panel2png.py dir outdir [--scale N] [name ...]
"""
import struct, sys, os
from PIL import Image

def main(argv):
    scale = 2
    if "--scale" in argv:
        i = argv.index("--scale"); scale = int(argv[i + 1]); del argv[i:i + 2]
    src, dst, names = argv[0], argv[1], argv[2:]
    os.makedirs(dst, exist_ok=True)
    for f in sorted(os.listdir(src)):
        if not (f.startswith("k2_") and f.endswith(".panel")):
            continue
        name = f[3:-6]
        if names and name not in names:
            continue
        raw = open(os.path.join(src, f), "rb").read()
        px = struct.unpack("<%dH" % (len(raw) // 2), raw)
        img = Image.new("RGB", (240, 240))
        img.putdata([(((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31) for v in px])
        if scale != 1:
            img = img.resize((240 * scale, 240 * scale), Image.NEAREST)
        img.save(os.path.join(dst, name + ".png"))

if __name__ == "__main__":
    main(sys.argv[1:])
