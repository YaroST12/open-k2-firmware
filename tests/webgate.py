# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The web parts (tools/web, webhid-flasher, the K2_NET dashboard) are tested only with K2_WEB=1 (Makefile K2_WEB)."""
import os
import unittest

WEB = os.environ.get("K2_WEB") == "1"
web_only = unittest.skipUnless(WEB, "web parts off (K2_WEB=1 runs them)")
