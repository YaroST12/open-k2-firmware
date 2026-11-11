# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The page names must agree in the firmware enums, k2_remote.py and k2proto.js (and the dashboard copy)."""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent))
from webgate import WEB, web_only  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import k2_remote as kr  # noqa: E402


def enum_names(text, enum):
    body = re.search(r"enum class %s\s*:\s*uint8_t\s*\{([^}]*)\}" % enum, text).group(1)
    return [n.strip().lower() for n in body.split(",") if n.strip() and n.strip() != "Count"]


def js_array(text, var):
    m = re.search(r"var %s = \[([^\]]*)\];" % var, text)
    return re.findall(r'"([^"]+)"', m.group(1))


class PageNames(unittest.TestCase):
    fw = (ROOT / "include" / "k2" / "ui" / "ui_types.hpp").read_text()
    pages = enum_names(fw, "Page")
    menus = enum_names(fw, "Menu")

    def test_firmware_rotation_is_what_the_tools_expect(self):
        self.assertEqual(self.pages[:3], ["home", "detail", "chargertest"])
        self.assertEqual(len(self.pages), kr.ROTATION_PAGES)

    def test_k2_remote(self):
        self.assertEqual(kr.PAGES, self.pages + self.menus)

    @web_only
    def test_k2proto_js(self):
        js = (ROOT / "tools" / "web" / "k2proto.js").read_text()
        self.assertEqual(js_array(js, "PAGES"), self.pages)
        self.assertEqual(js_array(js, "MENUS"), self.menus)

    @web_only
    def test_dashboard_copy(self):
        html = (ROOT / "tools" / "web" / "k2.html").read_text()
        self.assertEqual(js_array(html, "PAGES"), self.pages)
        self.assertEqual(js_array(html, "MENUS"), self.menus)

    def test_menu_page_name(self):
        n = kr.ROTATION_PAGES
        self.assertEqual(kr.page_name(0), "home")
        self.assertEqual(kr.page_name(n), "tools/" + self.menus[0])
        self.assertEqual(kr.page_name(n + 3), "tools/" + self.menus[3])

    def test_menus_are_the_tail_of_k2_remote_pages(self):
        self.assertEqual(kr.PAGES[kr.ROTATION_PAGES:], self.menus)

    def test_every_menu_has_a_page_name_in_all_three_copies(self):
        base = kr.ROTATION_PAGES
        js = (ROOT / "tools" / "web" / "k2proto.js").read_text()
        html = (ROOT / "tools" / "web" / "k2.html").read_text()
        for k, m in enumerate(self.menus):
            parent = "settings/" if m in ("usblink", "autorotate") else "tools/"
            self.assertEqual(kr.page_name(base + k), parent + m)
        for text in ((js, html) if WEB else ()):
            self.assertEqual(js_array(text, "MENUS"), self.menus)
            self.assertIn('"settings/"', text)
            self.assertIn('"tools/"', text)

    def test_connection_tab_aliases_follow_the_firmware(self):
        hpp = (ROOT / "include" / "k2" / "ui" / "ui_types.hpp").read_text()
        m = re.search(r"kConnTabAlias\s*=\s*([^;]+);", hpp)
        self.assertIsNotNone(m, "kConnTabAlias not found in include/k2/ui/ui_types.hpp")
        self.assertEqual(kr.CONN_ALIAS_BASE, kr.ROTATION_PAGES + len(self.menus))
        self.assertIn("kMenuPageBase", m.group(1))
        # old names land on the matching tab, new names are accepted too
        self.assertEqual(kr.page_index("pd"), kr.CONN_ALIAS_BASE)
        self.assertEqual(kr.page_index("source"), kr.CONN_ALIAS_BASE)
        self.assertEqual(kr.page_index("emarker"), kr.CONN_ALIAS_BASE + 1)
        self.assertEqual(kr.page_index("cable"), kr.CONN_ALIAS_BASE + 1)
        self.assertEqual(kr.page_index("identify"), kr.CONN_ALIAS_BASE + 2)
        self.assertEqual(kr.page_index("sink"), kr.CONN_ALIAS_BASE + 2)
        self.assertEqual(kr.page_index("connection"), kr.ROTATION_PAGES + self.menus.index("connection"))
        self.assertEqual(kr.page_index("diag"), kr.ROTATION_PAGES + self.menus.index("diag"))
        self.assertEqual(kr.page_name(kr.CONN_ALIAS_BASE + 2), "tools/connection/sink")

    def test_page_index_past_the_last_alias_is_numeric(self):
        n = kr.CONN_ALIAS_BASE + 3
        self.assertEqual(kr.page_name(n), str(n))


if __name__ == "__main__":
    unittest.main()
