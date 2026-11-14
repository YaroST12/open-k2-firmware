#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Copy the shared web sources into their embedded blocks of tools/web/k2.html.

k2.html must stay one self-contained file, so these are embedded verbatim:
  k2proto.js  -> <script id="k2proto">   the protocol layer
  k2kit.js    -> <script id="k2kit">     the K2 UI kit's renderers
  ctgrid.js   -> <script id="ctgrid">    the charger-test grid layout
  k2kit.css   -> <style id="k2kit">      the kit's tokens and component styles
tests/test_k2proto.mjs and tests/test_ctgrid.mjs fail when an embed drifts. Run via `make web-sync`.
"""
import re
import sys
from pathlib import Path

web = Path(__file__).resolve().parent
html = web / "k2.html"
EMBEDS = (("script", "k2proto", "k2proto.js"), ("script", "k2kit", "k2kit.js"),
          ("script", "ctgrid", "ctgrid.js"), ("style", "k2kit", "k2kit.css"))
text = new = html.read_text()
for tag, ident, name in EMBEDS:
    src = (web / name).read_text()
    if "</" + tag in src:
        sys.exit(f"{name} contains a literal </{tag}: it cannot be embedded")
    new, n = re.subn(rf'(<{tag} id="{ident}">\n).*?(</{tag}>)', lambda m: m.group(1) + src + m.group(2), new, count=1, flags=re.S)
    if n != 1:
        sys.exit(f'k2.html has no <{tag} id="{ident}"> block')
if new != text:
    html.write_text(new)
    print("k2.html: embedded blocks updated")
