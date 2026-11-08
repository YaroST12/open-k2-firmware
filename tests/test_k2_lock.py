# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Device lock (tools/k2_lock.py): two real processes, plus the transport open path."""
import os
import subprocess
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))
import k2_lock

HOLDER = (
    "import sys, time; sys.path.insert(0, %r); import k2_lock\n"
    "with k2_lock.hold(wait_s=5):\n"
    "    print('held', flush=True); time.sleep(float(sys.argv[1]))\n" % str(TOOLS)
)


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.env = dict(os.environ, K2_LOCK_FILE=str(Path(self.dir.name) / "k2.lock"))
        self.env.pop("K2_LOCK_OWNER_PID", None)
        self._saved = {k: os.environ.get(k) for k in ("K2_LOCK_FILE", "K2_LOCK_OWNER_PID", "K2_LOCK_WAIT")}
        os.environ["K2_LOCK_FILE"] = self.env["K2_LOCK_FILE"]
        os.environ.pop("K2_LOCK_OWNER_PID", None)

    def tearDown(self):
        for k, v in self._saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
        self.dir.cleanup()

    def start_holder(self, secs):
        p = subprocess.Popen([sys.executable, "-c", HOLDER, str(secs)], env=self.env,
                             stdout=subprocess.PIPE, text=True)
        self.assertEqual(p.stdout.readline().strip(), "held")
        return p


class LockCase(Base):
    def test_second_process_times_out_and_names_holder(self):
        p = self.start_holder(3)
        try:
            with self.assertRaises(k2_lock.DeviceLockTimeout) as cm:
                with k2_lock.hold(wait_s=0.5, report=lambda m: None):
                    pass
            self.assertIn(f"pid {p.pid}", str(cm.exception))
            self.assertIn("-c", str(cm.exception))      # the holder's command line
        finally:
            p.kill(); p.wait()

    def test_waits_then_proceeds_when_holder_exits(self):
        p = self.start_holder(1.0)
        msgs = []
        t0 = time.monotonic()
        with k2_lock.hold(wait_s=10, report=msgs.append):
            pass
        self.assertGreater(time.monotonic() - t0, 0.5)
        self.assertTrue(any("waiting for the K2" in m and f"pid {p.pid}" in m for m in msgs))
        p.wait()

    def test_reentrant_in_one_process(self):
        with k2_lock.hold(wait_s=0.2):
            with k2_lock.hold(wait_s=0.2):
                pass
            # still held after the inner release: another process must not get it
            p = subprocess.run([sys.executable, "-c",
                "import sys; sys.path.insert(0, %r); import k2_lock\n"
                "try:\n    k2_lock.hold(wait_s=0.3, report=lambda m: None).__enter__()\n"
                "except k2_lock.DeviceLockTimeout: sys.exit(7)\n" % str(TOOLS)],
                env=self.env)
            self.assertEqual(p.returncode, 7)

    def test_released_after_exit(self):
        with k2_lock.hold(wait_s=0.2):
            pass
        p = subprocess.run([sys.executable, "-c",
            "import sys; sys.path.insert(0, %r); import k2_lock\n"
            "with k2_lock.hold(wait_s=0.3): pass\n" % str(TOOLS)], env=self.env)
        self.assertEqual(p.returncode, 0)

    def test_child_inherits_the_parents_lock(self):
        with k2_lock.hold(wait_s=0.2):
            env = k2_lock.child_env(self.env)
            p = subprocess.run([sys.executable, "-c",
                "import sys; sys.path.insert(0, %r); import k2_lock\n"
                "with k2_lock.hold(wait_s=0.3): pass\n" % str(TOOLS)], env=env)
            self.assertEqual(p.returncode, 0)

    def test_stale_owner_variable_does_not_bypass(self):
        # env names a pid that does not hold the lock (the lock file is empty): must not bypass
        p = self.start_holder(3)
        try:
            os.environ["K2_LOCK_OWNER_PID"] = str(os.getpid())
            with self.assertRaises(k2_lock.DeviceLockTimeout):
                with k2_lock.hold(wait_s=0.4, report=lambda m: None):
                    pass
        finally:
            p.kill(); p.wait()

    def test_wait_default_and_env(self):
        os.environ.pop("K2_LOCK_WAIT", None)
        self.assertEqual(k2_lock.default_wait(), 120.0)
        os.environ["K2_LOCK_WAIT"] = "7"
        self.assertEqual(k2_lock.default_wait(), 7.0)


class TransportPath(Base):
    def test_transport_open_takes_the_lock(self):
        class Dev:
            def open_path(self, p): pass
            def set_nonblocking(self, n): pass
            def close(self): pass
        fake = types.SimpleNamespace(
            enumerate=lambda v, p: [{"path": b"/dev/x", "product_string": "Open-K2 Logger"}],
            device=Dev)
        sys.modules["hid"] = fake
        os.environ["K2_LOCK_WAIT"] = "0.4"
        import k2_flasher
        p = self.start_holder(3)
        try:
            with self.assertRaises(k2_lock.DeviceLockTimeout):
                k2_flasher.LinuxHidTransport()
        finally:
            p.kill(); p.wait()
        t = k2_flasher.LinuxHidTransport()       # free now: opens
        t.close()
        t2 = k2_flasher.LinuxHidTransport()      # and close released it
        t2.close()


class ProbeMessage(unittest.TestCase):
    def test_app_running_message(self):
        import k2_flasher
        app = [{"product_string": "Open-K2 Logger", "serial_number": "1"}]
        boot = [{"product_string": "WITRN.K2", "serial_number": "20230727"}]
        for cmd in ("probe", "backup"):
            m = k2_flasher.app_running_message(app, cmd, False)
            self.assertIn("the app is running", m)
            self.assertIn("--auto-dfu", m)
            self.assertIn("k2_remote.py dfu --confirm", m)
        self.assertIn("the app is running", k2_flasher.app_running_message(app, "flash", False))
        self.assertIsNone(k2_flasher.app_running_message(app, "flash", True))
        self.assertIsNone(k2_flasher.app_running_message(app, "monitor", False))
        self.assertIsNone(k2_flasher.app_running_message(boot, "probe", False))
        self.assertIsNone(k2_flasher.app_running_message([], "probe", False))


if __name__ == "__main__":
    unittest.main()
