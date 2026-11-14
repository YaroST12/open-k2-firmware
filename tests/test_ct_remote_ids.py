# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The remote charger-test START / CANCEL ids, the unit tables and the stop names must agree in the firmware, k2_remote.py and
k2proto.js (and the dashboard's embedded copy). Command ids stay append-only; these are ops inside CHARGER_TEST 0x0B."""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from webgate import web_only  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import k2_remote as kr  # noqa: E402

PROTO = (ROOT / "include" / "k2" / "core" / "remote_proto.hpp").read_text()
CT_HPP = (ROOT / "include" / "k2" / "chargertest" / "charger_test.hpp").read_text()
CT_CPP = (ROOT / "src" / "chargertest" / "charger_test.cpp").read_text()
PAGES_HPP = (ROOT / "include" / "k2" / "ui" / "ui_pages.hpp").read_text()
GUARD_HPP = (ROOT / "include" / "k2" / "pd" / "ra_guard.hpp").read_text()


def constexpr_u8(name):
    m = re.search(r"\b%s\s*=\s*([^,;]+)[,;]" % re.escape(name), PROTO)
    expr = m.group(1).strip()
    shift = re.fullmatch(r"1u\s*<<\s*(\d+)", expr)
    return 1 << int(shift.group(1)) if shift else int(expr, 0)


def enum_body(text, enum):
    return re.search(r"enum class %s\s*:\s*uint8_t\s*\{([^}]*)\}" % enum, text).group(1)


def enum_names(text, enum):
    return [n.strip() for n in enum_body(text, enum).split(",") if n.strip() and n.strip() != "Count"]


def cpp_strings(text, start):
    i = text.index(start)
    return re.findall(r'"([^"]*)"', text[i:text.index("};", i)])


def guard_text():
    m = re.search(r"kRefusalText\s*=\s*((?:\s*\"[^\"]*\")+)\s*;", GUARD_HPP)
    return "".join(re.findall(r'"([^"]*)"', m.group(1)))


def js_var(text, var):
    return re.search(r"var %s = ([^;]+);" % var, text, re.S).group(1)


def js_strings(text, var):
    return re.findall(r'"([^"]*)"', js_var(text, var))


class Firmware(unittest.TestCase):
    def test_ops_follow_the_view_request(self):
        self.assertEqual(constexpr_u8("kChargerTestStartReq"), 4)
        self.assertEqual(constexpr_u8("kChargerTestCancelReq"), 5)

    def test_stop_names_are_the_session_enum(self):
        names = enum_names(PAGES_HPP, "Stopped")
        self.assertEqual([n.lower() for n in names], ["nothing", "run", "wait", "confirmation", "refusal", "resume"])
        for i, n in enumerate(["Nothing", "Run", "Wait", "Confirm", "Refusal", "Resume"]):
            self.assertEqual(constexpr_u8("kCtStop" + n), i)


class K2Remote(unittest.TestCase):
    def test_start_cancel_ids_and_flags(self):
        self.assertEqual((kr.CT_START_REQ, kr.CT_CANCEL_REQ), (constexpr_u8("kChargerTestStartReq"), constexpr_u8("kChargerTestCancelReq")))
        self.assertEqual(kr.CT_START_CONFIRM, constexpr_u8("kCtStartConfirm"))
        self.assertEqual(kr.CT_START_ALL_TESTS, constexpr_u8("kCtStartAllTests"))
        self.assertEqual(kr.CT_STOPPED, [n.lower() for n in enum_names(PAGES_HPP, "Stopped")])

    def test_states_cover_the_firmware_enum(self):
        self.assertEqual(len(kr.TEST_STATES), len(enum_names(CT_HPP, "State")))

    def test_guard_text(self):
        self.assertEqual(kr.CT_GUARD_REFUSAL, guard_text())


@web_only
class K2Proto(unittest.TestCase):
    copies = {"k2proto.js": (ROOT / "tools" / "web" / "k2proto.js").read_text(),
              "k2.html": (ROOT / "tools" / "web" / "k2.html").read_text()}

    def each(self, check):
        for name, text in self.copies.items():
            with self.subTest(copy=name):
                check(text)

    def test_start_cancel_ids_and_flags(self):
        def check(js):
            m = re.search(r"var CT_START_REQ = (\d+), CT_CANCEL_REQ = (\d+), CT_START_CONFIRM = (\d+), CT_START_ALL_TESTS = (\d+);", js)
            self.assertEqual([int(g) for g in m.groups()],
                             [kr.CT_START_REQ, kr.CT_CANCEL_REQ, kr.CT_START_CONFIRM, kr.CT_START_ALL_TESTS])
            self.assertEqual(js_strings(js, "CT_STOPPED"), kr.CT_STOPPED)
            self.assertEqual(js_strings(js, "CT_CABLE_KINDS"), kr.CABLE_KINDS)
            self.assertEqual(re.search(r'var CT_GUARD_REFUSAL = "([^"]*)";', js).group(1), kr.CT_GUARD_REFUSAL)
        self.each(check)

    def test_units_follow_the_firmware(self):
        unit_enum = enum_names(CT_HPP, "Unit")
        default = re.search(r"kDefaultUnits\[\]\s*=\s*\{([^}]*)\}", CT_CPP).group(1)
        default = [u.strip().removeprefix("Unit::") for u in default.split(",") if u.strip()]
        def check(js):
            units = js_strings(js, "TEST_UNITS")
            self.assertEqual(units, kr.TEST_UNITS)
            self.assertEqual(len(units), len(unit_enum))
            self.assertEqual(js_strings(js, "TEST_UNIT_LABELS"), cpp_strings(CT_CPP, "const char* name(Unit u)"))
            self.assertEqual(js_strings(js, "TEST_DEFAULT_UNITS"), [units[unit_enum.index(u)] for u in default])
        self.each(check)

    def test_states_cover_the_firmware_enum(self):
        self.each(lambda js: self.assertEqual(len(js_strings(js, "TEST_STATES")), len(enum_names(CT_HPP, "State"))))

    def test_builtin_cables_are_the_dialogs(self):
        self.each(lambda js: self.assertEqual(
            tuple(int(i) for i in re.findall(r"id: (\d+)", js_var(js, "CT_BUILTINS"))), kr.DIALOG_BUILTINS))


if __name__ == "__main__":
    unittest.main()
