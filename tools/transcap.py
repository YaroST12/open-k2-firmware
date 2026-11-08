#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Transition capture decoder and plotter (host side of include/k2/pd/transcap.hpp).

A capture is VBUS (mV) plus CC1 / CC2 (ADC counts, 12.9 mV each) sampled about every millisecond around a
PD voltage step, with PD messages and caller events as markers on the same microsecond timeline. The device
serves it as the delta stream of k2::transcap::encode() (include/k2/pd/transcap_calc.hpp, which this file mirrors):

  header   "K2TD" | n_samples u16 | n_markers u8 | reason u8 | trigger_us u32            (12 bytes)
  markers  n_markers x ( t_us u32 | code u8 | step u8 | arg u16 )
  samples  first raw ( t_us u32 | vbus_mv u16 | cc1 u8 | cc2 u8 ), then per sample a flag byte
           (bit0 dt changed, bit1 vbus, bit2 cc1, bit3 cc2) followed by the zigzag-varint deltas of the
           changed fields; flag 0 = a run of identical steps, followed by a varint of further repeats.

  decode_stream(bytes) -> Capture      plain Python, no dependencies
  to_csv(capture) -> str               t_ms relative to the trigger, vbus_mv, cc1_mv, cc2_mv, event
  plot_png(capture, path)              matplotlib when installed; returns False when it is not
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

MAGIC = b"K2TD"
HEADER_BYTES = 12
CC_MV_PER_COUNT = 3300.0 / 256.0   # the stored count is raw12 >> 4 (16 ADC counts), 3.3 V full scale: 12.9 mV
MARKS = {0: "none", 1: "arm", 2: "request", 3: "accept", 4: "ps_rdy", 5: "reject", 6: "wait", 7: "soft_reset",
         8: "hard_reset", 9: "attach", 10: "detach", 11: "trigger", 12: "rx", 13: "tx", 14: "cc"}
REASONS = {0: "none", 1: "manual", 2: "shutoff", 3: "no-psrdy", 4: "hardreset", 5: "detach", 6: "timeout",
           7: "voltage", 8: "other"}
STATES = {0: "off", 1: "armed", 2: "post", 3: "frozen"}
ARM_RESULTS = {0: "ok", 1: "busy", 2: "no power monitor"}
# Markers worth a label on the plot; the long tail of Rx / Tx messages is in the CSV only.
LABELLED = {"arm", "request", "accept", "ps_rdy", "reject", "wait", "soft_reset", "hard_reset", "attach", "detach", "trigger"}


class StreamError(ValueError):
    pass


class Capture:
    """samples: [(t_us, vbus_mv, cc1, cc2)], markers: [(t_us, code, step, arg)] (absolute device microseconds)."""

    def __init__(self, samples, markers, reason, trigger_us):
        self.samples, self.markers, self.reason, self.trigger_us = samples, markers, reason, trigger_us

    @property
    def reason_name(self):
        return REASONS.get(self.reason, str(self.reason))

    @property
    def span_us(self):
        return (self.samples[-1][0] - self.samples[0][0]) & 0xFFFFFFFF if len(self.samples) > 1 else 0

    def t0(self):
        """The time axis origin: the trigger when there was one, else the first sample."""
        if self.trigger_us:
            return self.trigger_us
        return self.samples[0][0] if self.samples else 0

    def rel_ms(self, t_us):
        return _s32(t_us - self.t0()) / 1000.0


def _s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def _unzz(v):
    return (v >> 1) ^ -(v & 1)


def decode_stream(data):
    data = bytes(data)
    if len(data) < HEADER_BYTES or data[:4] != MAGIC:
        raise StreamError("not a transition capture stream (bad magic)")
    n_samples, n_markers, reason = struct.unpack_from("<HBB", data, 4)
    trigger_us = struct.unpack_from("<I", data, 8)[0]
    o = HEADER_BYTES
    if len(data) < o + 8 * n_markers:
        raise StreamError("truncated marker table")
    markers = []
    for _ in range(n_markers):
        t, code, step, arg = struct.unpack_from("<IBBH", data, o)
        markers.append((t, code, step, arg))
        o += 8
    samples = []
    if n_samples:
        if len(data) < o + 8:
            raise StreamError("truncated first sample")
        t, v, c1, c2 = struct.unpack_from("<IHBB", data, o)
        o += 8
        samples.append((t, v, c1, c2))

        def var():
            nonlocal o
            v = 0
            for sh in range(0, 35, 7):
                if o >= len(data):
                    raise StreamError("truncated varint")
                b = data[o]
                o += 1
                v |= (b & 0x7F) << sh
                if not b & 0x80:
                    return v
            raise StreamError("varint too long")

        pdt = 0
        while len(samples) < n_samples:
            if o >= len(data):
                raise StreamError("truncated sample stream")
            flags = data[o]
            o += 1
            cnt, ddt, dv, d1, d2 = 1, 0, 0, 0, 0
            if flags == 0:
                cnt = var() + 1
            else:
                if flags & 1:
                    ddt = _unzz(var())
                if flags & 2:
                    dv = _unzz(var())
                if flags & 4:
                    d1 = _unzz(var())
                if flags & 8:
                    d2 = _unzz(var())
            for _ in range(cnt):
                if len(samples) >= n_samples:
                    raise StreamError("run past the sample count")
                dt = pdt + ddt
                t, v, c1, c2 = samples[-1]
                samples.append(((t + dt) & 0xFFFFFFFF, (v + dv) & 0xFFFF, (c1 + d1) & 0xFF, (c2 + d2) & 0xFF))
                pdt = dt
                ddt = dv = d1 = d2 = 0
    return Capture(samples, markers, reason, trigger_us)


def pd_message(header):
    """Short name of a PD message header (Rx / Tx markers carry the 16-bit header in `arg`)."""
    try:
        import pd_trace
        return pd_trace.message_name(header)
    except Exception:
        return f"hdr 0x{header:04X}"


def marker_text(m):
    """(label, detail) of one marker tuple."""
    t, code, step, arg = m
    name = MARKS.get(code, f"mark{code}")
    if name == "arm":
        return name, (f"PDO {step} {arg} mV" if step or arg else "")
    if name == "request":
        return name, f"PDO {step}"
    if name in ("rx", "tx"):
        return name, pd_message(arg)
    if name == "cc":
        return name, f"CC{arg}" if arg else ""
    return name, ""


def cc_mv(count):
    return int(round(count * CC_MV_PER_COUNT))


def to_csv(cap):
    """One row per sample and per marker, ordered by time. Columns: t_ms (relative to the trigger), vbus_mv,
    cc1_mv, cc2_mv, event (marker label, empty on sample rows)."""
    rows = [(s[0], f"{cap.rel_ms(s[0]):.3f},{s[1]},{cc_mv(s[2])},{cc_mv(s[3])},") for s in cap.samples]
    for m in cap.markers:
        label, detail = marker_text(m)
        text = (label + (" " + detail if detail else "")).replace(",", ";")
        rows.append((m[0], f"{cap.rel_ms(m[0]):.3f},,,,{text}"))
    rows.sort(key=lambda r: _s32(r[0] - cap.t0()))
    return "t_ms,vbus_mv,cc1_mv,cc2_mv,event\n" + "".join(r[1] + "\n" for r in rows)


def summary(cap):
    gaps = [(b[0] - a[0]) & 0xFFFFFFFF for a, b in zip(cap.samples, cap.samples[1:])]
    lo = min((s[1] for s in cap.samples), default=0)
    hi = max((s[1] for s in cap.samples), default=0)
    out = [f"capture: {len(cap.samples)} samples over {cap.span_us / 1000:.1f} ms, reason {cap.reason_name}, "
           f"{len(cap.markers)} markers; VBUS {lo}..{hi} mV"]
    if gaps:
        out.append(f"sample spacing: median {sorted(gaps)[len(gaps) // 2]} us, max {max(gaps)} us")
    for m in cap.markers:
        label, detail = marker_text(m)
        out.append(f"  {cap.rel_ms(m[0]):9.3f} ms  {label}{' ' + detail if detail else ''}")
    return "\n".join(out)


def plot_png(cap, path, title=None):
    """VBUS, CC1 and CC2 against time (ms, 0 = the trigger) with the PD messages as labelled markers.
    False when matplotlib is not installed (use to_csv)."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return False
    if not cap.samples:
        raise StreamError("no samples to plot")
    t = [cap.rel_ms(s[0]) for s in cap.samples]
    fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(12, 6.5), gridspec_kw={"height_ratios": [3, 2]})
    ax1.plot(t, [s[1] / 1000.0 for s in cap.samples], color="#1f77b4", lw=1.2, drawstyle="steps-post")
    ax1.set_ylabel("VBUS (V)")
    ax2.plot(t, [cc_mv(s[2]) for s in cap.samples], color="#d62728", lw=1.0, drawstyle="steps-post", label="CC1")
    ax2.plot(t, [cc_mv(s[3]) for s in cap.samples], color="#2ca02c", lw=1.0, drawstyle="steps-post", label="CC2")
    ax2.set_ylabel("CC (mV)")
    ax2.set_xlabel("time from the trigger (ms)")
    ax2.legend(loc="upper left", ncol=2, fontsize=8)
    ymax = ax1.get_ylim()[1]
    for k, m in enumerate(cap.markers):
        label, detail = marker_text(m)
        if label not in LABELLED and label not in ("rx", "tx"):
            continue
        x = cap.rel_ms(m[0])
        for ax in (ax1, ax2):
            ax.axvline(x, color="#777777", lw=0.6, ls=":")
        text = label if label in LABELLED else f"{label} {detail}"
        ax1.text(x, ymax * (0.98 - 0.07 * (k % 4)), " " + text, rotation=0, fontsize=7, va="top", color="#333333")
    ax1.set_title(title or f"Transition capture: {cap.reason_name}, {len(cap.samples)} samples over "
                           f"{cap.span_us / 1000:.0f} ms")
    ax1.grid(alpha=0.3)
    ax2.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return True
