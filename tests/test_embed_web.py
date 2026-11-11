# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/embed_web.py turns the dashboard into a deterministic gzip blob in a C++ source."""
import gzip
import pathlib
import re
import shutil
import sys
import unittest

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from webgate import WEB, web_only  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import embed_web  # noqa: E402


def decode(cpp):
    body = re.search(r"kData\[\] = \{([^}]*)\}", cpp).group(1)
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-f]{2})", body))


@web_only
class EmbedWeb(unittest.TestCase):
    def test_roundtrip(self):
        page = b"<html>" + b"k2 " * 500 + b"</html>"
        cpp = embed_web.render(page)
        self.assertEqual(gzip.decompress(decode(cpp)), page)
        self.assertIn("true}", cpp.replace(" ", ""))  # WebAsset{..., gzip = true}

    def test_deterministic(self):
        page = b"abc" * 100
        self.assertEqual(embed_web.render(page), embed_web.render(page))

    def test_gzip_fallback_roundtrips(self):
        data = b"k2 dashboard " * 200
        self.assertEqual(gzip.decompress(embed_web.compress(data, use_zopfli=False)), data)

    @unittest.skipUnless(shutil.which("zopfli"), "zopfli not installed (the builder image has it)")
    def test_zopfli_is_deterministic_and_beats_gzip(self):
        html = embed_web.minify((ROOT / "tools" / "web" / "k2.html").read_bytes())
        z = embed_web.compress(html)
        self.assertEqual(z, embed_web.compress(html))
        self.assertEqual(gzip.decompress(z), html)
        self.assertLess(len(z), len(embed_web.compress(html, use_zopfli=False)))

    def test_real_page_is_smaller_and_intact(self):
        html = (ROOT / "tools" / "web" / "k2.html").read_bytes()
        blob = decode(embed_web.render(html))
        self.assertLess(len(blob), len(html) // 2)
        self.assertEqual(gzip.decompress(blob), embed_web.minify(html))

    def test_minify_drops_indent_and_script_comment_lines(self):
        page = (b"<style>\n  body { margin: 0 }\n</style>\n"
                b"<script>\n  // a comment line\n    const a = 1; // kept: trails code\n"
                b"  const u = 'http://x';\n</script>\n")
        self.assertEqual(embed_web.minify(page),
                         b"<style>\nbody { margin: 0 }\n</style>\n"
                         b"<script>\nconst a = 1; // kept: trails code\n"
                         b"const u = 'http://x';\n</script>\n")

    def test_minify_keeps_comment_like_text_outside_scripts(self):
        page = b"<p>\n  // shown on the page\n</p>\n"
        self.assertEqual(embed_web.minify(page), b"<p>\n// shown on the page\n</p>\n")

    def test_minify_leaves_template_literals_alone(self):
        page = b"<script>\nconst t = `\n  line one\n  // not a comment\n`;\n</script>\n"
        self.assertEqual(embed_web.minify(page), page)

    def test_minify_saves_on_real_page(self):
        html = (ROOT / "tools" / "web" / "k2.html").read_bytes()
        self.assertLess(len(embed_web.minify(html)), len(html) * 9 // 10)

    def test_page_embeds_current_protocol(self):
        html = (ROOT / "tools" / "web" / "k2.html").read_text()
        js = (ROOT / "tools" / "web" / "k2proto.js").read_text()
        self.assertIn(js, html)  # make web-sync was run


if __name__ == "__main__":
    unittest.main()
