#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Copy tools/web/k2proto.js into the <script id="k2proto"> block of tools/web/k2.html.

k2.html must stay one self-contained file, so the protocol layer is embedded; tests/test_k2proto.mjs
fails when the two drift. Run via `make web-sync`.
"""
import re
import sys
from pathlib import Path

web = Path(__file__).resolve().parent
html, js = web / "k2.html", (web / "k2proto.js").read_text()
text = html.read_text()
new, n = re.subn(r'(<script id="k2proto">\n).*?(</script>)', lambda m: m.group(1) + js + m.group(2), text, count=1, flags=re.S)
if n != 1:
    sys.exit("k2.html has no <script id=\"k2proto\"> block")
if new != text:
    html.write_text(new)
    print("k2.html: protocol block updated")
