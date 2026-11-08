#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Render a K2_DUMP .panel (240x240 RGB565 LE) as ASCII art for fast in-text UI iteration.

  tools/panel2ascii.py host-build/k2_mock_home.panel [--cols 120] [--rows 60]

Each character cell covers a block of pixels; the glyph is picked from the block's average
luminance and whether any pixel is a "text/accent" color (non-background, non-dim).
"""
import struct
import sys

RAMP = " .:-=+*#%@"


def main(argv):
    path = argv[0]
    cols = 120
    rows = 60
    if "--cols" in argv:
        i = argv.index("--cols"); cols = int(argv[i + 1])
    if "--rows" in argv:
        i = argv.index("--rows"); rows = int(argv[i + 1])
    raw = open(path, "rb").read()
    px = struct.unpack("<%dH" % (len(raw) // 2), raw)
    W = H = 240
    bw, bh = W / cols, H / rows

    def lum(v):
        r, g, b = ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31
        return (r * 299 + g * 587 + b * 114) // 1000

    # background = most common color
    from collections import Counter
    bg = Counter(px).most_common(1)[0][0]
    bg_l = lum(bg)

    out = []
    for ry in range(rows):
        line = []
        for rx in range(cols):
            x0, x1 = int(rx * bw), max(int(rx * bw) + 1, int((rx + 1) * bw))
            y0, y1 = int(ry * bh), max(int(ry * bh) + 1, int((ry + 1) * bh))
            tot = n = 0
            for y in range(y0, y1):
                base = y * W
                for x in range(x0, x1):
                    tot += lum(px[base + x]); n += 1
            avg = tot / max(n, 1)
            d = avg - bg_l
            if abs(d) < 12:
                line.append(" ")
            else:
                # brighter than bg -> denser glyph
                idx = int((d / 255 + 1) / 2 * (len(RAMP) - 1))
                line.append(RAMP[max(0, min(len(RAMP) - 1, idx))])
        out.append("".join(line))
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1:])
