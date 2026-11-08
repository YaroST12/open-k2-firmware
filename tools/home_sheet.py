#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Build the Big / Detail comparison images from the panels that tests/test_ui_home.cpp dumps.

  mkdir -p build/size/home2/panels && K2_DUMP=build/size/home2/panels host-build/test_ui_home
  python3 tools/panel2png.py build/size/home2/panels build/size/home2/png --scale 2
  python3 tools/home_sheet.py build/size/home2
"""
import sys
from PIL import Image

out = sys.argv[1] if len(sys.argv) > 1 else "build/size/home2"
states = ["idle", "forward", "reverse", "novbus", "legacy", "noemarker"]
GAP, BG = 20, (40, 40, 40)


def load(name):
    return Image.open(f"{out}/png/home2_{name}.png").convert("RGB")


def strip(names, path):
    ims = [load(n) for n in names]
    w, h = ims[0].size
    sheet = Image.new("RGB", (len(ims) * w + (len(ims) - 1) * GAP, h), BG)
    for i, im in enumerate(ims):
        sheet.paste(im, (i * (w + GAP), 0))
    sheet.save(path)


def grid(path):
    w, h = load("big_idle").size
    sheet = Image.new("RGB", (len(states) * w + (len(states) - 1) * GAP, 2 * h + GAP), BG)
    for r, page in enumerate(("big", "detail")):
        for i, s in enumerate(states):
            sheet.paste(load(f"{page}_{s}"), (i * (w + GAP), r * (h + GAP)))
    sheet.save(path)


load("big_forward").save(f"{out}/big.png")
load("detail_forward").save(f"{out}/detail.png")
strip(["big_forward", "detail_forward"], f"{out}/sheet.png")
grid(f"{out}/states.png")
strip(["big_anim0", "big_anim1", "big_anim2"], f"{out}/animation.png")

# zoomed crop of the two 14 px icons (charger at x3,y20 and plug at x108,y20) on the Big page, 4x again
big = load("big_forward")
s = big.size[0] // 240
icons = Image.new("RGB", (2 * 60 * s * 4 + GAP, 22 * s * 4), BG)
for i, x in enumerate((0, 104)):
    crop = big.crop((x * s, 17 * s, (x + 60) * s, 39 * s)).resize((60 * s * 4, 22 * s * 4), Image.NEAREST)
    icons.paste(crop, (i * (60 * s * 4 + GAP), 0))
icons.save(f"{out}/icons_zoom.png")
