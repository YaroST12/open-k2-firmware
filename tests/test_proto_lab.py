# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import proto_lab  # noqa: E402


class FakePort:
    """Answers each written line from a table; unknown lines get `ok`."""

    def __init__(self, replies=None):
        self.replies = replies or {}
        self.sent = []
        self.out = b""
        self.closed = False

    def write(self, data):
        for line in data.decode().splitlines():
            self.sent.append(line)
            key = line.replace(" --json", "")
            rep = self.replies.get(key)
            if rep is None:
                rep = ['{"cmd":"vbus","mv":5000,"ma":0}'] if key == "vbus" else []
                rep = rep + ["ok"]
            self.out += ("\r\n".join(rep) + "\r\n").encode()

    def read(self, timeout):
        d, self.out = self.out, b""
        return d

    def close(self):
        self.closed = True


class TickClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        self.t += 0.01
        return self.t


def make(tmp_path, replies=None):
    p = FakePort(replies)
    return p, proto_lab.Lab(p, tmp_path / "x.log", clock=TickClock())


def test_cmd_adds_json_and_parses(tmp_path):
    p, lab = make(tmp_path, {"accp read 0x80": ['{"cmd":"accp.read","reg":128,"ack":true,"val":7}', "ok"]})
    r = lab.cmd("accp read 0x80")
    assert r[0]["val"] == 7
    assert p.sent[0] == "accp read 0x80 --json"
    assert p.sent[1].startswith("vbus")          # V/I trace sample after the command


def test_err_raises_and_try_cmd_continues(tmp_path):
    p, lab = make(tmp_path, {"qc2 9000": ["err: this can raise VBUS"]})
    try:
        lab.cmd("qc2 9000")
        assert False
    except proto_lab.LabError as e:
        assert "raise VBUS" in str(e)
    assert "error" in lab.try_cmd("qc2 9000")[0]


def test_timeout(tmp_path):
    p = FakePort()
    p.write = lambda d: None
    lab = proto_lab.Lab(p, tmp_path / "x.log", clock=TickClock())
    try:
        lab.cmd("accp ping", timeout=0.5)
        assert False
    except proto_lab.LabError as e:
        assert "timeout" in str(e)


def test_close_resets_to_safe_state(tmp_path):
    p, lab = make(tmp_path)
    lab.close()
    assert p.sent[-3:] == ["fc reset", "dp hiz", "dm hiz"]
    assert p.closed
    lab.close()                                   # idempotent
    assert p.sent.count("fc reset") == 1


def test_log_file_has_commands_and_replies(tmp_path):
    p, lab = make(tmp_path)
    lab.cmd("vbus")
    lab.close()
    text = (tmp_path / "x.log").read_text()
    assert " TX vbus --json" in text and " RX ok" in text


def test_script_accp_dump_and_main(tmp_path, monkeypatch):
    monkeypatch.setattr(proto_lab, "LOG_DIR", tmp_path)
    p = FakePort({"accp dump 0x00 0xFF": ['{"cmd":"accp.dump","reg":0,"ack":true}', "ok"]})
    rc = proto_lab.main(["script", "accp-dump"], port=p)
    assert rc == 0
    assert any(s.startswith("accp ping") for s in p.sent)
    assert p.sent[-1] == "dm hiz"
    assert list(tmp_path.glob("accp-dump-*.log"))


def test_run_mode_error_still_resets(tmp_path, monkeypatch):
    monkeypatch.setattr(proto_lab, "LOG_DIR", tmp_path)
    p = FakePort({"scp 9000 3000": ["err: busy"]})
    rc = proto_lab.main(["run", "scp 9000 3000"], port=p)
    assert rc == 1
    assert "fc reset" in p.sent


def test_all_scripts_run_against_fake(tmp_path, monkeypatch):
    monkeypatch.setattr(proto_lab, "LOG_DIR", tmp_path)
    for n in proto_lab.SCRIPTS:
        p = FakePort()
        assert proto_lab.main(["--no-trace", "script", n], port=p) == 0, n
