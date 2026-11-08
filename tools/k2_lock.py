# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Exclusive, re-entrant lock for the K2 USB device, shared by every host tool.

flock(2) on /tmp/k2-device.lock (override: K2_LOCK_FILE). The holder writes its pid and command line
into the file so a waiting process can say who is in the way. Re-entrant inside one process (depth
counter). A child process of a holder inherits the lock through K2_LOCK_OWNER_PID (see child_env);
the variable is honoured only while the lock file still names that live pid.
Wait: default 120 s, env K2_LOCK_WAIT.
"""
import contextlib
import fcntl
import os
import sys
import time

DEFAULT_WAIT_S = 120.0
_fd = None
_depth = 0


class DeviceLockTimeout(RuntimeError):
    pass


def lock_path():
    return os.environ.get("K2_LOCK_FILE", "/tmp/k2-device.lock")


def default_wait():
    try:
        return float(os.environ.get("K2_LOCK_WAIT", DEFAULT_WAIT_S))
    except ValueError:
        return DEFAULT_WAIT_S


def _cmdline(pid):
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return f.read().replace(b"\0", b" ").decode("utf-8", "replace").strip()
    except OSError:
        return ""


def _holder(path):
    """(pid, cmdline) recorded in the lock file, or (None, "")."""
    try:
        with open(path) as f:
            first, _, rest = f.read().partition("\n")
        return int(first), rest.strip()
    except (OSError, ValueError):
        return None, ""


def _inherited(path):
    owner = os.environ.get("K2_LOCK_OWNER_PID")
    if not owner or not owner.isdigit():
        return False
    pid, _ = _holder(path)
    if pid != int(owner):
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def child_env(env=None):
    """Environment for a subprocess that may use the K2 while this process holds the lock."""
    env = dict(os.environ if env is None else env)
    env["K2_LOCK_OWNER_PID"] = str(os.getpid())
    return env


def acquire(wait_s=None, report=None):
    global _fd, _depth
    if _depth:
        _depth += 1
        return
    path = lock_path()
    if _inherited(path):
        _depth = 1
        return              # _fd stays None: nothing of ours to unlock
    wait_s = default_wait() if wait_s is None else wait_s
    report = report or (lambda m: print(m, file=sys.stderr, flush=True))
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    try:
        os.chmod(path, 0o666)
    except OSError:
        pass
    deadline = time.monotonic() + wait_s
    told = False
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except OSError:
            pid, cmd = _holder(path)
            who = f"held by pid {pid}: {cmd}" if pid else "held by another process"
            if time.monotonic() >= deadline:
                os.close(fd)
                raise DeviceLockTimeout(f"gave up after {wait_s:g} s waiting for the K2 ({who})")
            if not told:
                report(f"waiting for the K2 ({who})")
                told = True
            time.sleep(0.1)
    os.ftruncate(fd, 0)
    os.write(fd, f"{os.getpid()}\n{_cmdline(os.getpid())}\n".encode())
    _fd, _depth = fd, 1


def release():
    global _fd, _depth
    if _depth == 0:
        return
    _depth -= 1
    if _depth == 0 and _fd is not None:
        try:
            os.ftruncate(_fd, 0)
        finally:
            os.close(_fd)      # closing drops the flock
            _fd = None


@contextlib.contextmanager
def hold(wait_s=None, report=None):
    acquire(wait_s, report)
    try:
        yield
    finally:
        release()
