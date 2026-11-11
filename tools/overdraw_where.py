#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Which code paints pixels twice. Resolves the call stacks tests/test_ui_overdraw prints when K2_OVERDRAW_TRACE=1.

  K2_OVERDRAW_TRACE=1 host-build/test_ui_overdraw > /tmp/trace.txt
  python3 tools/overdraw_where.py host-build/test_ui_overdraw /tmp/trace.txt [--page Home] [--top 12] [--depth 2]

Each TRACE line is one call stack (as offsets into the test binary) with the pixels sent under it and how many of those
landed on a pixel already written since the start of the paint. The report groups by the first `--depth` frames above the
drawing primitives (gfx / text / lcd), so a line reads "component  <- caller": rewritten px, sent px, per page."""
import argparse
import collections
import re
import subprocess
import sys

PRIMITIVES = ("gfx.cpp", "text.cpp", "fake_lcd.cpp", "ui_viewport.cpp", "pixel_sink", "font_aa", "lcd.hpp", "lcd_timing", "<span>",
              "/usr/include", "bits/", "ui_widgets.cpp:", "stl_")


def resolve(binary, offsets):
    """{offset: (function, 'file:line')} through one addr2line call."""
    uniq = sorted(set(offsets))
    out = subprocess.run(["addr2line", "-f", "-C", "-e", binary] + [hex(o) for o in uniq], capture_output=True, text=True,
                         check=True).stdout.splitlines()
    res = {}
    for i, o in enumerate(uniq):
        func = out[2 * i]
        loc = out[2 * i + 1].rsplit("/", 1)[-1]
        res[o] = (re.sub(r"\(.*", "", func), loc)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("trace")
    ap.add_argument("--page")
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--depth", type=int, default=2)
    a = ap.parse_args()
    rows = []
    for line in open(a.trace):
        m = re.match(r"TRACE (\S+) sent=(\d+) rewritten=(\d+)((?: [0-9a-f]+)*)", line)
        if m and (not a.page or m.group(1) == a.page):
            rows.append((m.group(1), int(m.group(2)), int(m.group(3)), [int(x, 16) for x in m.group(4).split()]))
    if not rows:
        sys.exit("no TRACE lines (run the test with K2_OVERDRAW_TRACE=1)")
    names = resolve(a.binary, [o for r in rows for o in r[3]])
    agg = collections.defaultdict(lambda: [0, 0])
    for page, sent, rew, offs in rows:
        frames = [names[o] for o in offs]
        keep = [f for f in frames if not any(p in f[1] or p in f[0] for p in PRIMITIVES)]
        keep = [f for f in keep if not f[0].startswith(("main", "walk", "_start", "__libc", "(anonymous"))]
        key = " <- ".join(f"{f[0]} ({f[1]})" for f in keep[:a.depth])
        agg[(page, key)][0] += rew
        agg[(page, key)][1] += sent
    by_page = collections.defaultdict(list)
    for (page, key), (rew, sent) in agg.items():
        by_page[page].append((rew, sent, key))
    for page, items in by_page.items():
        items.sort(reverse=True)
        print(f"== {page}: {sum(i[0] for i in items)} px rewritten under {len(items)} call sites")
        for rew, sent, key in items[:a.top]:
            print(f"  {rew:7d} rewritten of {sent:7d} sent  {key}")


if __name__ == "__main__":
    main()
