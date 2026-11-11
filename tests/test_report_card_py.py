# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import report_card  # noqa: E402

FIX = ROOT / "tests" / "fixtures"
PAYLOAD = bytes.fromhex((FIX / "report_card_sample.hex").read_text().strip())


def test_sample_matches_the_firmware_json_fixture():
    # The same fixture tests/test_report_card_json.cpp renders from the C++ side.
    assert report_card.dumps(report_card.decode(PAYLOAD)) == (FIX / "report_card_sample.json").read_text().strip()


def test_qc_label_follows_the_firmware_rule():
    # The sample has PPS ranges: a Qualcomm VID makes it QC4+, 100 W or more QC5; any other VID has no label.
    rec = bytearray(PAYLOAD)
    assert "qc" not in report_card.decode(bytes(rec))
    rec[4:6] = (0x05C6).to_bytes(2, "little")
    rec[33] = 2   # the sample is format 1; format 2 carries pdp_w in byte 54
    rec[54] = 45
    c = report_card.decode(bytes(rec))
    assert c["qc"] == "QC4+" and list(c)[list(c).index("qc") - 1] in ("pdp_w", "name_auto")
    rec[54] = 140
    assert report_card.decode(bytes(rec))["qc"] == "QC5"
    rec[4:6] = (0x2717).to_bytes(2, "little")
    assert "qc" not in report_card.decode(bytes(rec))
    assert report_card.qc_label(0x05C6, 140, []) == ""   # Qualcomm VID without a PPS range


def test_moto_and_samsung_labels_follow_the_firmware_rule():
    # Mirrors report_card::qc_label(): Moto TurboPower (Motorola / Lenovo VID + a PPS APDO above 5 A); the Samsung SFC 3.0 / 2.0
    # labels are a brand claim and need the brand's own VID (0x04E8) - vid 0 or another vendor's VID gets none, however SFC-shaped
    # the capability is; Qualcomm keeps its own label.
    def pps(lo, hi, ma):
        return [{"alg": "PPS (PD3.x)", "lo_mv": lo, "hi_mv": hi, "max_mv": hi, "fail_mv": 0, "max_ma": ma}]
    assert report_card.qc_label(0x22B8, 0, pps(5000, 20000, 6250)) == "Moto TurboPower"
    assert report_card.qc_label(0x17EF, 0, pps(5000, 20000, 6250)) == "Moto TurboPower"
    assert report_card.qc_label(0x22B8, 0, pps(5000, 20000, 2000)) == ""
    assert report_card.qc_label(0x2717, 0, pps(5000, 20000, 6250)) == ""   # Xiaomi VID is not the brand's: no SFC label
    assert report_card.qc_label(0x04E8, 0, pps(3300, 21000, 3000)) == "Samsung SFC 3.0"
    assert report_card.qc_label(0, 0, pps(3300, 21000, 3000)) == ""        # vid 0: no brand claim
    assert report_card.qc_label(0x04E8, 0, pps(3300, 21000, 2950)) == ""
    assert report_card.qc_label(0x04E8, 0, pps(5000, 15000, 3000)) == "Samsung SFC 2.0"
    assert report_card.qc_label(0x04E8, 0, pps(3300, 11000, 4050), cable_ma=5000) == "Samsung SFC 2.0"
    assert report_card.qc_label(0x04E8, 0, pps(3300, 11000, 4050), cable_ma=3000) == ""
    assert report_card.qc_label(0x04E8, 0, pps(5000, 11000, 4050), cable_ma=5000) == ""   # does not cover 3.3 V
    assert report_card.qc_label(0x05C6, 45, pps(3300, 21000, 3000)) == "QC4+"
    assert report_card.qc_label(0, 0, [{"alg": "PD", "lo_mv": 0, "hi_mv": 0}]) == ""


def test_v0_card_still_decodes():
    # A card stored before format 1 (byte 33 = 0): u16 mV max and a plain detail byte, no failing target.
    v0 = bytes.fromhex((FIX / "report_card_sample_v0.hex").read_text().strip())
    assert v0[33] == 0 and PAYLOAD[33] == 4  # PAYLOAD is now v4
    assert report_card.dumps(report_card.decode(v0)) == (FIX / "report_card_sample_v0.json").read_text().strip()


def test_format_2_carries_pdp_and_auto_name_flag():
    rec = bytearray(PAYLOAD)
    rec[33] = 2; rec[54] = 65; rec[55] = 1
    c = report_card.decode(bytes(rec))
    assert (c["brand"], c["pdp_w"], c["name_auto"]) == ("Apple", 65, True)
    assert list(c)[:8] == ["no", "name", "fp", "vid", "pid", "brand", "pdp_w", "name_auto"]
    old = bytearray(PAYLOAD)
    old[33] = 1; old[54] = 65; old[55] = 1            # format 1: those bytes are name padding, not data
    assert "pdp_w" not in report_card.decode(bytes(old)) and "name_auto" not in report_card.decode(bytes(old))


def test_qc_label_needs_the_samsung_vid():
    # Row B item 5: the SFC labels are a brand claim, so they need the brand's own VID (report_card.hpp
    # kSamsungVid); a probe that read vid 0, or another vendor's VID, gets no Samsung label. A Qualcomm
    # VID keeps the QC label.
    pps = [{"alg": "PPS (PD3.x)", "lo_mv": 3300, "hi_mv": 21000, "max_ma": 3000}]
    assert report_card.qc_label(0x04E8, 100, pps) == "Samsung SFC 3.0"
    assert report_card.qc_label(0, 100, pps) == ""            # vid 0: no brand claim
    assert report_card.qc_label(0x2717, 65, pps) == ""        # another vendor's VID: still no Samsung label
    assert report_card.qc_label(0x05C6, 65, pps) == "QC4+"    # Qualcomm keeps the QC label
    assert report_card.qc_label(0x05C6, 100, pps) == "QC5"


def test_v9_spread_decodes_per_level():
    # ROADMAP "PPS coverage" item 3: format 9 adds the setpoint error in mV per level of each APDO (block at 198:
    # 4 ranges x 4 levels, int8, -128 = none). A v8-length payload of the same card decodes with no spread.
    assert report_card.PAYLOAD == 198 and report_card.FORMAT == 10  # v9 added the spread block at 198; v10 is the current format
    rec = bytearray(214)
    rec[31] = 1                                   # one range
    rec[33] = 9                                   # format v9
    q = 104                                       # range 0: PPS (alg 4), pos 6, 5-11 V
    rec[q] = 4; rec[q + 1] = 6
    rec[q + 2:q + 4] = (50 | (100 << 9)).to_bytes(2, "little")
    rec[q + 4:q + 6] = (110).to_bytes(2, "little")
    rec[q + 6:q + 8] = (110).to_bytes(2, "little")
    for k, e in enumerate((88, 30, -17, -24)):   # the spread block at 198: errors low to high
        rec[198 + k] = e & 0xFF
    c = report_card.decode(bytes(rec))
    assert c["ranges"][0]["err_mv"] == [88, 30, -17, -24]
    assert len(report_card.REASONS) == 47 and report_card.REASONS[45:] == ["floored above target", "step resolution"]
    rec[33] = 8                                   # the same bytes as a v8 payload (198): no spread is read
    c8 = report_card.decode(bytes(rec[:198]))
    assert "err_mv" not in c8["ranges"][0]
    assert report_card.PAYLOAD_V9 == 214

def test_brand_table_matches_the_firmware():
    hdr = (ROOT / "include" / "k2" / "chargertest" / "report_card.hpp").read_text()
    consts = dict(re.findall(r'inline constexpr uint16_t (k\w+) = 0x([0-9A-Fa-f]{4});', hdr))   # case kSamsungVid: ...
    fw = {int(consts.get(v, v[2:] if v.startswith("0x") else v), 16): n
          for v, n in re.findall(r'case (0x[0-9A-F]{4}|k\w+): return "([^"]+)"', hdr)}
    assert fw == report_card.BRANDS


def test_group_record_packs_voltages_and_detail():
    rec = bytearray(PAYLOAD)
    q = 56 + 4 * 1                                    # QC2
    rec[q] = 2 << 6 | 11                              # fail, request failed
    v = 511 | 200 << 9 | (0x20 | 3) << 18             # max 51.1 V, fail 20 V, pd::Err 3 (stored as 128 + 3)
    rec[q + 1:q + 4] = v.to_bytes(3, "little")
    g = report_card.decode(bytes(rec))["groups"]["QC2"]
    assert (g["max_mv"], g["fail_mv"], g["detail"]) == (51100, 20000, 131)


def test_bad_payloads_are_refused():
    with pytest.raises(report_card.BadCard):
        report_card.decode(PAYLOAD[:-1])
    bad = bytearray(PAYLOAD)
    bad[29] = 99
    with pytest.raises(report_card.BadCard):
        report_card.decode(bytes(bad))
    bad = bytearray(PAYLOAD)
    bad[104] = 99
    with pytest.raises(report_card.BadCard):
        report_card.decode(bytes(bad))


def test_in_contract_handover_record_decodes():
    # Handover outcome 5 (HandoverOutcome::InContract) is what card 1 on the device carries after a T21 run.
    rec = bytearray(PAYLOAD)
    rec[28] = 5
    assert report_card.decode(bytes(rec))["handover"] == "in PD contract"


def test_handover_table_covers_the_enum():
    hdr = (ROOT / "include" / "k2" / "chargertest" / "charger_test.hpp").read_text()
    enum = re.search(r"enum class HandoverOutcome[^{]*\{(.*?)\}", hdr, re.S).group(1)
    assert len(report_card.HANDOVER) == len(enum.split(","))
    src = (ROOT / "src" / "chargertest" / "report_card_json.cpp").read_text()
    row = re.search(r"kHandover\[\]\s*=\s*\{(.*?)\};", src, re.S).group(1)
    assert re.findall(r'"([^"]*)"', row) == report_card.HANDOVER


def _cpp_table(fn_marker):
    src = (ROOT / "src" / "chargertest" / "charger_test.cpp").read_text()
    body = src[src.index(fn_marker):]
    m = re.search(r"names\[\]\s*=\s*\{(.*?)\};", body, re.S)
    return re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))


@pytest.mark.parametrize("marker,table", [
    ("const char* name(Algorithm a)", report_card.ALGORITHMS),
    ("const char* name(Group g)", report_card.GROUPS),
    ("const char* name(Reason r)", report_card.REASONS),
    ("const char* name(State s)", report_card.STATES),
])
def test_name_tables_match_charger_test_cpp(marker, table):
    assert _cpp_table(marker) == table


def _enum_names(header, name):
    body = re.search(rf"enum class {name}\b[^{{]*\{{(.*?)\}}", (ROOT / "include" / "k2" / header).read_text(), re.S).group(1)
    body = re.sub(r"//[^\n]*", "", body)
    return [x.strip().split("=")[0].strip() for x in body.split(",") if x.strip()]


def _switch_names(path, fn):
    src = (ROOT / "src" / path).read_text()
    body = src[src.index(fn):]
    body = body[:body.index("return \"?\";")]
    return re.findall(r'return "((?:[^"\\]|\\.)*)";', body)


@pytest.mark.parametrize("header,enum,cpp,fn,table", [
    ("fastcharge/fc_engine.hpp", "Fail", "fastcharge/fc_engine.cpp", "const char* name(Fail f)", report_card.FC_FAILS),
    ("pd/pd_sink.hpp", "Err", "pd/pd_sink.cpp", "const char* err_name(Err e)", report_card.PD_ERRS),
    ("pd/pd_sink.hpp", "SinkResult", "pd/pd_sink.cpp", "const char* result_name(SinkResult r)", report_card.SINK_RESULTS),
])
def test_detail_tables_match_the_engines(header, enum, cpp, fn, table):
    # The card names a group's cause from the engine's own enum; these tables are what the decoder prints.
    assert len(_enum_names(header, enum)) == len(table)
    assert _switch_names(cpp, fn) == table


def test_group_why_prefers_the_engine_code_and_falls_back_to_the_reason():
    f = report_card.group_why
    assert f("QC2", 8, "request failed") == "voltage not reached"
    assert f("PD", 128 + 3, "request failed") == report_card.PD_ERRS[3]
    assert f("PD", 2, "request failed") == report_card.SINK_RESULTS[2]
    assert f("QC3", 0, "timeout") == "timeout"
    assert f("QC3", 99, "timeout") == "timeout"       # out of range: the reason names it
    assert f("PD", 144, "over voltage cap") == "EPR: above K2's 36V monitor"
    assert f("PD", 139, "EPR entry refused") == "EPR: refused (RDO)"
    assert f("QC3", 144, "timeout") == "timeout"      # only the PD group carries EPR details


def test_epr_detail_names_match_the_firmware_and_the_tools():
    src = (ROOT / "src" / "chargertest" / "charger_test.cpp").read_text()
    body = src[src.index("const char* epr_detail_name"):]
    body = body[:body.index("default:")]
    base = {"kDetailEprFail": 136, "kDetailMonitor36": 144, "kDetailMonitor48": 145, "kDetailMonitorOther": 146,
            "kDetailEprOffered": 147, "kDetailEprWithheld": 148,
            "kDetailEprOfferedRefused": 149, "kDetailNoChain": 155, "kDetailChainUnfinished": 156, "kDetailCableCap": 157,
            "kDetailVendorRefused": 158, "kDetailSourceReset": 159}
    cpp = {base[m.group(1)] + int(m.group(2) or 0): m.group(3)
           for m in re.finditer(r'case (kDetail\w+?)(?: \+ (\d+))?: return "([^"]*)";', body)}
    assert cpp == report_card.EPR_DETAILS
    import k2_remote
    assert cpp == k2_remote.EPR_DETAILS
    # no value collides with a pd::Err (128 + err) or a sink result
    assert min(cpp) >= 128 + len(report_card.PD_ERRS)


_WEB = __import__("os").environ.get("K2_WEB") == "1"   # the web parts are off by default (Makefile K2_WEB)


def _js_fc_fails(path):
    src = (ROOT / "tools" / "web" / path).read_text()
    return re.findall(r'"([^"]*)"', re.search(r"var FC_FAILS = \[(.*?)\];", src, re.S).group(1))


@pytest.mark.parametrize("table", [
    pytest.param(lambda: __import__("k2_remote").FC_FAILS, id="k2_remote.py"),
    pytest.param(lambda: _js_fc_fails("k2proto.js"), id="k2proto.js", marks=pytest.mark.skipif(not _WEB, reason="K2_WEB=1")),
    pytest.param(lambda: _js_fc_fails("k2.html"), id="k2.html", marks=pytest.mark.skipif(not _WEB, reason="K2_WEB=1")),
])
def test_every_fc_fail_table_names_every_enum_value(table):
    # fc::Fail grew NeedsLoad/NoVoltage without names once: the decoders printed "unknown (13)".
    assert _switch_names("fastcharge/fc_engine.cpp", "const char* name(Fail f)") == table()


def test_full_20_char_name_does_not_pick_up_the_pdp_byte():
    rec = bytearray(PAYLOAD)
    rec[33] = 2
    rec[34:54] = b"Anker Prime100 USB-C"
    rec[54] = 100                                     # PDP watts = 'd', right after the name field
    assert report_card.decode(bytes(rec))["name"] == "Anker Prime100 USB-C"


def test_format_3_carries_advertised_currents():
    rec = bytearray(PAYLOAD)
    rec[33] = 3; rec[55] = 60 << 1          # highest fixed PDO 3 A
    lo = int.from_bytes(rec[104 + 2:104 + 4], "little") & 0x1FF
    rec[104 + 2:104 + 4] = (lo | 60 << 9).to_bytes(2, "little")   # first range 3 A
    c = report_card.decode(bytes(rec))
    assert c["ranges"][0]["max_ma"] == 3000 and c["ranges"][0]["lo_mv"] == lo * 100
    assert c["groups"]["PD"]["max_ma"] == 3000


def test_v4_card_with_empty_epr_levels():
    # v4 cards (format byte >= 4) always have epr_levels in the output
    c = report_card.decode(PAYLOAD)
    assert "epr_levels" in c
    assert c["epr_levels"] == {}  # sample card has no EPR entries


def test_v3_payload_has_no_epr_levels():
    # v3 cards (format byte = 3) do not have epr_levels
    v0 = bytes.fromhex((FIX / "report_card_sample_v0.hex").read_text().strip())
    assert v0[33] == 0  # v0 format
    c = report_card.decode(v0)
    assert "epr_levels" not in c


def test_v4_payload_decodes_epr_levels():
    # Create a v4 payload with non-empty epr_levels
    rec = bytearray(PAYLOAD)
    rec[33] = 4  # v4 format
    # Add EPR entries at offset 144 (after ranges)
    # EPR entry 0: 280 dv, Pass, max_mv=28200 (rounded from 28280)
    epr_offset = 144
    rec[epr_offset:epr_offset + 2] = (280).to_bytes(2, "little")
    rec[epr_offset + 2] = int(report_card.VERDICT.index("pass")) << 6
    rec[epr_offset + 3:epr_offset + 6] = (283).to_bytes(3, "little")  # dv_round(28280) = 283
    c = report_card.decode(bytes(rec))
    assert "epr_levels" in c
    assert len(c["epr_levels"]) == 1
    assert "28000V" in c["epr_levels"] or "28V" in c["epr_levels"]
    assert "max_ma" not in report_card.decode(PAYLOAD)["ranges"][0]


def test_v5_cable_guard_flags_decode_as_the_firmware_renders_them():
    # flags byte of the cable block (offset 162 + 13): bits 3..5 = guard reading + 1, bit 6 = started anyway.
    rec = bytearray(PAYLOAD)
    rec[33] = 5
    rec.extend(b"\0" * (178 - len(rec)))
    assert "guard" not in report_card.decode(bytes(rec))["cable"]          # a card before the field: keys absent
    for topo, word in enumerate(("unknown", "none", "cable", "direct")):
        rec[162 + 13] = 3 | ((topo + 1) << 3)                              # observed + spoofed + the reading
        c = report_card.decode(bytes(rec))["cable"]
        assert c["guard"] == word and "guard_bypassed" not in c
    rec[162 + 13] |= 0x40
    c = report_card.decode(bytes(rec))["cable"]
    assert c["guard"] == "direct" and c["guard_bypassed"] == 1


def test_v5_oppo_svid_flag_decodes_as_the_firmware_renders_it():
    # src_flags (offset 162 + 15) bit 0: the source listed OPPO's SVID; the key appears only then.
    rec = bytearray(PAYLOAD)
    rec[33] = 5
    rec.extend(b"\0" * (178 - len(rec)))
    assert "oppo_svid" not in report_card.decode(bytes(rec))
    rec[162 + 15] = 1
    assert report_card.decode(bytes(rec))["oppo_svid"] == 1


def test_v5_scp_capability_decodes_as_the_firmware_renders_it():
    # src_flags bits 1..7: the adapter's SCP current limit in 50 mA units; the key appears only when it was read.
    rec = bytearray(PAYLOAD)
    rec[33] = 5
    rec.extend(b"\0" * (178 - len(rec)))
    assert "scp_ma" not in report_card.decode(bytes(rec))
    rec[162 + 15] = 120 << 1
    card = report_card.decode(bytes(rec))
    assert card["scp_ma"] == 6000 and "oppo_svid" not in card
    rec[162 + 15] = (45 << 1) | 1
    card = report_card.decode(bytes(rec))
    assert card["scp_ma"] == 2250 and card["oppo_svid"] == 1


def _ufcs_mode(lo_dv, hi_dv, units):
    return (lo_dv | hi_dv << 8 | units << 17).to_bytes(3, "little")


def test_v6_ufcs_modes_decode_as_the_firmware_renders_them():
    # offset 178: four u24 modes (lo 100 mV in 8 bits, hi 100 mV in 9 bits, current in 50 mA in 7 bits); hi 0 = unused.
    rec = bytearray(PAYLOAD)
    rec[33] = 6
    rec.extend(b"\0" * (report_card.PAYLOAD_V6 - len(rec)))
    assert len(rec) == 190
    assert "ufcs" not in report_card.decode(bytes(rec))                    # no modes stored: the key is absent
    rec[178:184] = _ufcs_mode(33, 55, 100) + _ufcs_mode(50, 200, 60)
    assert report_card.decode(bytes(rec))["ufcs"] == [{"lo_mv": 3300, "hi_mv": 5500, "max_ma": 5000},
                                                     {"lo_mv": 5000, "hi_mv": 20000, "max_ma": 3000}]
    rec[184:187] = _ufcs_mode(50, 110, 0)                                  # a mode with no current known: max_ma omitted
    assert report_card.decode(bytes(rec))["ufcs"][2] == {"lo_mv": 5000, "hi_mv": 11000}
    rec[187:190] = _ufcs_mode(255, 511, 127)                               # the saturated values
    modes = report_card.decode(bytes(rec))["ufcs"]
    assert len(modes) == 4 and modes[3] == {"lo_mv": 25500, "hi_mv": 51100, "max_ma": 6350}


def test_v7_repeat_block_decodes_only_for_v7():
    # offset 190: times u8, last_boots u16, last_uptime_s u24; keys appear only when times > 1 (as in the firmware's JSON).
    rec = bytearray(PAYLOAD)
    rec[33] = 7
    rec.extend(b"\0" * (report_card.PAYLOAD_V7 - len(rec)))
    assert len(rec) == 196
    assert "times" not in report_card.decode(bytes(rec))                   # zero count: once
    rec[190] = 1
    assert "times" not in report_card.decode(bytes(rec))                   # one run: no repeat keys
    rec[190:196] = bytes([3, 0x34, 0x12, 0x56, 0x34, 0x12])
    c = report_card.decode(bytes(rec))
    assert (c["times"], c["last_boots"], c["last_uptime_s"]) == (3, 0x1234, 0x123456)
    assert list(c).index("times") == list(c).index("uptime_s") + 1         # the key order of the renderer
    # A v6 payload (190 bytes, or zero padded to 196 by the store, with the old CRC bytes there) has no repeat block.
    rec[33] = 6
    assert "times" not in report_card.decode(bytes(rec))
    assert "times" not in report_card.decode(bytes(rec[:190]))


def test_v8_rebalanced_decodes_only_for_v8():
    # offset 196: rebalanced u8 (+ a spare byte); the key appears only when the count is non-zero (as in the firmware's JSON).
    rec = bytearray(PAYLOAD)
    rec.extend(b"\0" * (report_card.PAYLOAD - len(rec)))
    assert len(rec) == 198
    rec[33] = 8
    assert "rebalanced" not in report_card.decode(bytes(rec))             # zero count: no key
    rec[196] = 2
    assert report_card.decode(bytes(rec))["rebalanced"] == 2
    rec[33] = 7                                                             # a v7 payload ends at 196: no v8 byte there
    assert "rebalanced" not in report_card.decode(bytes(rec[:196]))


def test_v5_payloads_have_no_ufcs_modes():
    # A v5 card is 178 bytes (or zero padded to 190 by the store, with the old CRC bytes where the modes would be).
    rec = bytearray(PAYLOAD)
    rec[33] = 5
    rec.extend(b"\0" * (178 - len(rec)))
    assert "ufcs" not in report_card.decode(bytes(rec))
    rec.extend(b"\x12\x34\x56\0\0\0\0\0\0\0\0\0")
    assert "ufcs" not in report_card.decode(bytes(rec))
