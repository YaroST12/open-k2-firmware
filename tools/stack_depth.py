#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Worst-case stack depth from GCC callgraph-info files (-fcallgraph-info=su,da). Used by `make gate`.

usage: stack_depth.py [--cg DIR] [--build DIR] [--root NAME] [--isr NAME,...] [--top N] [--no-resolve]
                      [--scenario charger-test] [--page REGEX] [--no-events] [--via REGEX]

Every build writes the .ci files (Makefile: CG_DIR = build/cg for the LTO partitions, the non-LTO lwIP / Net
objects write theirs next to their .o). With no option the tool reads build/ (the gate's stack-depth step and
`make stack-report` call it).

GCC records an indirect call as an edge to "__indirect_call". RESOLVE maps the callers we know (page
virtuals, list row callbacks, littlefs block callbacks, the systick hooks, the charger-test and fast-charge
hardware seams, the LCD sink and idle hook, TinyUSB class drivers, lwIP callbacks) to the functions they can reach.
INTERFACES does the same by name for an abstract interface: the implementer's methods that carry the
interface's virtual names, read from the header (charger_test::Env -> ChargerTestSession). Resolving
a caller to *every* function that matches would chain unrelated pages through each other (a page's update
calling Ui::redraw calling the next page's update ...), so each rule counts its dispatches along a chain
and a dispatch past the rule's limit is not followed: that is the model, a call chain holds at most `limit`
nested dispatches of that kind. The limits are the real nesting depths of this firmware:

  page virtual   2   Ui::tick -> page.on_event -> Ui::set_page -> page.draw_static (never three)
  list row       1   ListView::draw -> row callback (a row does not draw another list)
  charger seam   2   Runner -> Env (ChargerTestSession) -> a session helper that calls the Runner back
  idle hook      1   LCD stream -> run_round -> mainloop::idle_hook (the pacer never nests a round)
  ...

A direct-call cycle (recursion) is cut where it closes and listed. A caller with an indirect call that no
rule covers, sitting on the reported chain, is flagged: the depth is then a lower bound. So is a caller a rule
matches whose callee pattern finds no function (a stale rule after a rename: listed as such).

The whole-firmware number composes the worst of everything. --scenario charger-test models a measured
situation (the charger-test page on screen for a whole run, nobody pressing: only that page's virtuals, no
on_event); --via REGEX keeps only chains through a function, to name the deepest one under a given step.

Functions without a frame record (libc, assembly) count as 0 bytes. The exception entry frame is added
for every ISR (EXC_FRAME), nested ISRs are summed (the priority order is not read, so this is pessimistic).
"""
import argparse
import pathlib
import re
import subprocess
import sys

NODE = re.compile(r'node: \{ title: "([^"]+)" label: "([^"]*)"')
EDGE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
STACK = re.compile(r'\\n(\d+) bytes \((static|dynamic|dynamic,bounded)\)')

EXC_FRAME = 104        # 8 core words + 17 FP words (S0-S15, FPSCR) + the reserved word and alignment: the worst case
MAIN_ROOT = "k2_reset_main"   # Reset_Handler is a naked asm trampoline that switches to PSP and calls this
# Handlers that run on the MSP (everything the vector table points at that does work).
ISR_ROOTS = ("SysTick_Handler", "USB_LP_CAN1_RX0_IRQHandler", "USB_HP_CAN1_TX_IRQHandler",
             "EXTI2_IRQHandler", "EXTI9_5_IRQHandler", "EXTI15_10_IRQHandler")

# Dispatch classes and how many of one kind may be nested on a chain.
LIMITS = {
    "page": 2,       # all PageIf virtuals together
    "update": 1, "on_event": 1, "draw_static": 1, "enter": 1, "leave": 1, "sample": 1, "labels": 1, "invalidate": 1,
    "row": 1,        # list row draw / signature callbacks
    "env": 2,        # the charger-test Runner's hardware / UI seams, fast-charge Engine's HwEnv
    "hook": 1,       # systick tick hook, boottrace poll, log tap
    "lfs": 1,        # littlefs block-device callbacks (they sit at the bottom: no recursion)
    "flash": 1,      # the flash BlockDevice virtual behind the lfs callbacks
    "usb": 1,        # TinyUSB class drivers behind tud_task / invoke_class_control
    "drive": 1,      # FAT image providers
    "lwip": 1,       # lwIP callbacks
    "lcd": 1,        # the LCD capture / pixel sinks
    "list": 1,       # fs::list callbacks
    "shell": 1,      # shell hardware seams
    "idle": 1,       # the LCD stream's idle hook (the pacer never nests a service round)
    "addref": 1,     # the charger-test planner's step sink
    "listview": 1,   # ListView -> RowList row/sig adapter (which then dispatches once via "row")
}

PAGE_VIRTUALS = ("update", "on_event", "draw_static", "enter", "leave", "sample", "labels", "invalidate")
PAGE_VIRTUAL = re.compile(r"k2::\w*(Page|Menu|Wizard)\w*::(%s)\(" % "|".join(PAGE_VIRTUALS))


def pv(*names):
    """Callee regex for these page virtuals (the last group is the virtual's name)."""
    return r"k2::\w*(Page|Menu|Wizard)\w*::(%s)\(" % "|".join(names)

# (caller regex, callee regex, class keys) over demangled names: where an indirect call in the caller can land.
# Two keys are special: "page" (the callee must be a different page than the ones already on the chain: a
# page never switches to itself) and "row" (the callee must belong to the page that owns the list being drawn).
RESOLVE = [
    # Which Ui function calls which virtual (read off src/ui/ui.cpp): a redraw never feeds an event, a panel draw
    # only asks for the labels. Without this a remote command's set_page would chain into any virtual.
    (r"k2::Ui::(draw_slot|slot_sig|draw_panel|sync_panel)", pv("labels"), ("page", "*virt")),
    (r"k2::Ui::redraw", pv("invalidate", "sample", "draw_static", "update"), ("page", "*virt")),
    (r"k2::Ui::(set_page|push_menu|pop_menu)", pv("enter", "leave"), ("page", "*virt")),
    (r"k2::Ui::(dispatch_button|on_button|tick|run_action)|k2::PageBase::",
     pv("on_event", "update", "invalidate", "draw_static", "sample", "enter", "leave"), ("page", "*virt")),
    (r"k2::Ui::(?!draw_slot|slot_sig|draw_panel|sync_panel|redraw|set_page|push_menu|pop_menu|dispatch_button"
     r"|on_button|tick|run_action)|k2::capture_begin",
     pv("update", "invalidate", "draw_static", "sample"), ("page", "*virt")),
    # The remote command handler feeds the current page an event (Ui::current()->on_event(...)).
    (r"k2::remote::handle_request|k2::Remote::", pv("on_event", "update", "invalidate", "draw_static", "sample", "enter", "leave"),
     ("page", "*virt")),
    (r"k2::http_server::.*::start\(", r"^k2::capture_begin\(", ("hook",)),     # Shot::begin is main's capture lambda
    (r"^(Reset_Handler|k2_reset_main)$", r"^_GLOBAL__sub_I_", ("init", "*empty")),  # the static constructors (may be none)
    (r"k2::ListView::", r"^k2::kit::RowList::(draw_row_cb|row_sig_cb)\(", ("listview",)),
    # ResultGrid's row-spec callback fn_ (grid_.init(..., &Page::result_row_fn, this)); the call site shows up
    # in the page's paint methods too when anchor_bottom/draw_some are inlined into them.
    (r"k2::kit::ResultGrid::|k2::\w+Page::", r"k2::\w+Page::result_row_fn\(", ("row",)),
    (r"^lfs_", r"k2::fs::.*cb_(read|prog|erase|sync)", ("lfs",)),
    # The charger-test session's poll pumps the journal (and saves the card when the run ends) into littlefs;
    # GCC inlines the lfs commit path into poll's clone, so cfg->prog shows up under poll. cb_prog's own call is
    # the flash BlockDevice's virtual.
    (r"k2::ChargerTestSession::poll", r"k2::fs::.*cb_(read|prog|erase|sync)\(", ("lfs",)),
    (r"k2::fs::.*cb_(read|prog|erase|sync)\(", r"k2::fs::BlockDevice::(read|prog|erase)\(", ("flash",)),
    (r"k2::systick::tick\(", r"k2::load::.*tick_hook", ("hook",)),
    # The loop's Context hooks (main.cpp fills them): reboot (the OK + Back chord from Ui::tick, REBOOT) and DFU.
    (r"k2::mainloop::", r"(^|::)(reboot_now|dfu_now)\(", ("hook",)),
    (r"k2::systick::delay_ms\(", r"k2::boottrace\w*::.*poll|^k2::mainloop::.*wait_hook\(", ("hook",)),
    # The LCD stream's service round (lcd::set_idle_hook): every long paint calls the main loop's idle hook (PD service,
    # UFCS/HS service, VBUS scan, transition capture) through the pacer, which never nests a round.
    (r"k2::lcd::.*run_round\(", r"^k2::mainloop::idle_hook\(", ("idle",)),
    # The FUSB302 driver's SOP'/SOP'' frame hook (fusb302::set_sop_prime_hook): the e-marker spoof's answer.
    (r"k2::fusb302::.*handle_frame\(", r"^k2::emk_spoof::.*on_sop_prime\(", ("hook",)),
    # The debug stream's frame sink: shell_hw::poll hands dbg_hw::service its CDC write lambda.
    (r"k2::dbg::Mux::service", r"^k2::shell_hw::poll\(\)::\{lambda", ("hook",)),
    # The paint budget's clock (Ui::set_paint_clock(systick::micros)).
    (r"k2::PaintBudget::", r"^k2::systick::micros\(", ("hook",)),
    # The kit RowList's row callback (RowList::init(..., &Page::row_fn, this)): the page that owns the list.
    (r"k2::kit::RowList::", r"^k2::\w+::\w+\(void\*, int, bool, k2::kit::RowSpec&\)", ("row",)),
    # The charger-test planner's type-erased step sink (AddRef): the Runner's plan lambdas.
    (r"k2::charger_test::.*AddRef::operator\(\)", r"^k2::charger_test::\(anonymous namespace\)::AddRef::AddRef<",
     ("addref",)),
    (r"k2::usb::log\(", r"k2::shell_hw::.*tap", ("hook",)),
    (r"k2::(fc|fctrig)::", r"k2::fctrig::.*HwEnv::", ("env",)),
    (r"k2::shell(_hw)?::", r"k2::shell_hw::.*Hw::", ("shell",)),
    (r"k2::lcd::(?!.*run_round\()", r"k2::.*sink_(begin|pixels|fill)", ("lcd",)),
    # The vfat image asks its provider for a file's bytes (fill) or a directory entry's long name (lfn)
    (r"k2::drive::Drive::read\(|k2::vfat::Image::read\(", r"k2::drive::Drive::(fill|lfn)\(", ("drive",)),
    (r"k2::vfat::Image::dir_", r"k2::drive::Drive::lfn\(", ("drive",)),
    (r"k2::drive::Drive::(render|rebuild)", r"k2::drive::.*_FUN", ("drive",)),
    # Fs::list_raw's callback is a storage/journal-layer Fs::list<F> thunk (never a UI page lambda)
    (r"k2::fs::Fs::list_raw", r"^k2::fs::Fs::list<(?!.*Page::).*_FUN\(", ("list",)),
    (r"^(tud_task_ext|invoke_class_control|configuration_reset)",
     r"^(hidd|mscd|cdcd|netd|tud_\w+_cb|tud_hid\w*|tud_msc\w*)", ("usb",)),
]


# Interfaces whose overrides an indirect call in the caller reaches, matched by name: the implementer's methods that
# carry the interface's virtual names (read from the header, so a new Env method is followed without a rule edit).
# (caller regex, header, interface, implementer prefix regex, keys)
INTERFACES = [
    # The charger-test Runner's hardware / journal seam: ChargerTestSession implements charger_test::Env (privately).
    # LTO inlines Runner::start / cancel into the session's begin_run / cancel, so the seam calls show up there too.
    (r"k2::charger_test::Runner::|k2::ChargerTestSession::(begin_run|cancel)\(", "include/k2/chargertest/charger_test.hpp",
     "Env", r"k2::ChargerTestSession::", ("env",)),
]

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Measured situations (--scenario): (what it models, page regex, on_event dispatched)
SCENARIOS = {
    "charger-test": ("a charger-test run on its page, nobody pressing (heartbeat stack_free of a Deep run)",
                     "ChargerTestPage", False),
}


# The read-only Fs entry points never reach littlefs' commit path: lfs_file_opencfg_ runs
# lfs_fs_forceconsistency only for a write-open (third_party/littlefs/lfs.c), lfs_file_close_ -> lfs_file_sync_
# returns early for a clean file, and lfs_stat / lfs_dir_* only fetch. GCC's call graph still has those edges,
# so once a chain enters a read-only entry point the commit functions are not followed.
READ_ONLY_ENTRY = re.compile(r"^k2::fs::Fs::(read_file|read_at|size_of|exists|list_raw)\(")
LFS_COMMIT = re.compile(r"^lfs_(fs_forceconsistency|dir_commit|file_sync|file_flush|file_write)")


def short(title):
    return title.rsplit(":", 1)[-1] if title.startswith("/") else title


def demangle(names):
    names = list(names)
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True,
                             check=True).stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        out = names
    return dict(zip(names, out)) if len(out) == len(names) else {n: n for n in names}


def load_text(texts):
    """(frame bytes by function, frame kind, direct edges) from the text of .ci files."""
    frame, kind, edges = {}, {}, {}
    for text in texts:
        for line in text.splitlines():
            m = NODE.search(line)
            if m:
                name = short(m.group(1))
                s = STACK.search(m.group(2))
                if s:
                    frame[name] = int(s.group(1))
                    kind[name] = s.group(2)
                else:
                    frame.setdefault(name, None)
                continue
            m = EDGE.search(line)
            if m:
                edges.setdefault(short(m.group(1)), set()).add(short(m.group(2)))
    return frame, kind, edges


def ci_files(cg_dirs, build_dir):
    files = []
    for d in cg_dirs:
        files += sorted(pathlib.Path(d).glob("*.ci"))
    b = pathlib.Path(build_dir)
    skip = {str(pathlib.Path(d).resolve()) for d in cg_dirs}
    for p in sorted(b.rglob("*.ci")):
        if str(p.parent.resolve()) in skip or p.parent.name == "cg":
            continue
        files.append(p)
    return files


def interface_methods(text, name):
    """The virtual methods declared in `struct|class name { ... }` of a header's text, in order (comments ignored)."""
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    m = re.search(r"\b(?:struct|class)\s+%s\b[^;{]*\{" % re.escape(name), text)
    if not m:
        return []
    depth, i = 1, m.end()
    while i < len(text) and depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    names = []
    for v in re.finditer(r"\bvirtual\s+[^;{(]*?\b(\w+)\s*\(", text[m.end():i]):
        if v.group(1) not in names and not v.group(1).startswith("~"):
            names.append(v.group(1))
    return names


def interface_rules(entries):
    """RESOLVE rules from interfaces: (caller regex, header text, interface name, implementer prefix regex, keys) -> the
    caller's indirect calls land on the implementer's methods that carry the interface's virtual names. An interface
    that is not found yields a rule that matches nothing, so its callers show up as stale (unresolved)."""
    out = []
    for cre, text, iface, impl, ks in entries:
        names = interface_methods(text, iface)
        tre = (r"^(?:%s)(%s)\(" % (impl, "|".join(names))) if names else r"(?!)"
        out.append((cre, tre, ks))
    return out


def file_interfaces(root=ROOT):
    """INTERFACES with each header read from the tree (a missing header reads as empty: the rule goes stale)."""
    out = []
    for cre, path, iface, impl, ks in INTERFACES:
        p = root / path
        out.append((cre, p.read_text(errors="replace") if p.exists() else "", iface, impl, ks))
    return out


class Graph:
    def __init__(self, frame, kind, edges, resolve=True, rules=None, page=None, events=True, via=None):
        """rules: the RESOLVE list (default: RESOLVE plus the INTERFACES read from the tree). page: a regex, only that
        page's virtuals are dispatched (the page on screen for a whole run); events=False drops on_event dispatches
        (no button press, no remote key); via: worst() only counts chains through a function matching this regex."""
        self.frame, self.kind, self.direct = frame, kind, edges
        self.rules = rules if rules is not None else RESOLVE + interface_rules(file_interfaces())
        self.page = re.compile(page) if page else None
        self.events = events
        self.via = re.compile(via) if via else None
        self.dm = demangle(set(frame) | set(edges))
        self.keys = sorted({k for _, _, ks in self.rules for k in ks if not k.startswith("*")} | set(LIMITS))
        self.kid = {k: i for i, k in enumerate(self.keys)}
        self.limit = [LIMITS.get(k, 1) for k in self.keys]
        self.indirect = {}     # caller -> [(callee, key ids)]
        self.unresolved = set()
        self.stale = set()     # callers a rule matched that resolved to nothing (a renamed seam): unresolved too
        self.cut = set()       # direct-call cycles cut
        self.memo = {}
        if resolve:
            self._resolve()

    def _resolve(self):
        names = set(self.frame)
        for src, ts in self.direct.items():
            if "__indirect_call" not in ts:
                continue
            hit = empty_ok = False
            dsrc = self.dm.get(src, src)
            out = []
            for cre, tre, ks in self.rules:
                if not re.search(cre, dsrc):
                    continue
                hit = True
                empty_ok = empty_ok or "*empty" in ks
                for n in sorted(names):
                    if n == src:
                        continue
                    dn = self.dm.get(n, n)
                    m = re.search(tre, dn)
                    if not m:
                        continue
                    kids = [self.kid[k] for k in ks if not k.startswith("*") and k not in ("page", "row")]
                    if "*virt" in ks:       # the page virtual's own name: one nested dispatch of each
                        v = m.group(m.lastindex) if m.lastindex else None
                        if v in self.kid:
                            kids.append(self.kid[v])
                    special = "page" if "page" in ks else "row" if "row" in ks else ""
                    out.append((n, tuple(kids), special))
            if out:
                self.indirect[src] = out
            elif hit and not empty_ok:
                self.stale.add(src)
            if not hit or (not out and not empty_ok):
                self.unresolved.add(src)

    def owner(self, node):
        """The class a function belongs to (the page whose virtual or row callback it is)."""
        m = re.match(r"k2::(\w+)::", self.dm.get(node, node))
        return m.group(1) if m else ""

    def worst(self, root):
        """(bytes, chain) of the deepest chain from root; None when `via` is set and no chain passes through it."""
        self.memo = {}
        counts = (0,) * len(self.keys)
        return self._worst(root, counts, (), False, frozenset(), self.via is None)

    def _page_ok(self, node):
        """False for a page virtual the scenario (page / events) leaves out."""
        m = PAGE_VIRTUAL.match(self.dm.get(node, node))
        if not m:
            return True
        if self.page and not self.page.fullmatch(self.owner(node)):
            return False
        return self.events or m.group(2) != "on_event"

    def _worst(self, node, counts, owners, ro, onstack, found):
        dn = self.dm.get(node, node)
        found = found or bool(self.via.search(dn))
        # A page virtual reached any way (a devirtualised direct call too) makes its page an owner of the chain.
        if PAGE_VIRTUAL.match(dn) and self.owner(node) not in owners:
            owners = owners + (self.owner(node),)
        ro = ro or bool(READ_ONLY_ENTRY.match(dn))
        key = (node, counts, owners, ro, found)
        if key in self.memo:
            return self.memo[key]
        onstack = onstack | {node}
        best = None
        for t in sorted(self.direct.get(node, ())):
            if t == "__indirect_call":
                continue
            if t in onstack:
                self.cut.add((node, t))
                continue
            if ro and LFS_COMMIT.match(t):
                continue
            if (self.page or not self.events) and not self._page_ok(t):
                continue
            r = self._worst(t, counts, owners, ro, onstack, found)
            if r is not None and (best is None or r[0] > best[0]):
                best = r
        for t, kids, special in self.indirect.get(node, ()):
            if t in onstack:
                continue
            if special == "page" and self.owner(t) in owners:
                continue                      # a page never dispatches to a page already on the chain
            if special == "row" and not (owners and self.owner(t) == owners[-1]):
                continue                      # a row callback belongs to the page that owns the list
            if special == "page" and not self._page_ok(t):
                continue                      # the scenario's page only (and no on_event without presses)
            nc = list(counts)
            ok = True
            for k in kids:
                nc[k] += 1
                if nc[k] > self.limit[k]:
                    ok = False
            if not ok:
                continue
            r = self._worst(t, tuple(nc), owners, ro, onstack, found)
            if r is not None and (best is None or r[0] > best[0]):
                best = r
        own = self.frame.get(node) or 0
        if best is None:
            res = (own, (node,)) if found else None
        else:
            res = (own + best[0], (node,) + best[1])
        self.memo[key] = res
        return res

    def describe(self, chain, top=60):
        lines = []
        for n in chain[:top]:
            f = self.frame.get(n)
            tag = f"  [{self.kind[n]}]" if self.kind.get(n, "static") != "static" else ""
            flag = "  <- indirect call not resolved" if n in self.unresolved else ""
            lines.append(f"    {('?' if f is None else f):>5}  {self.dm.get(n, n)}{tag}{flag}")
        return lines


def analyze(g, main_root=MAIN_ROOT, isrs=ISR_ROOTS):
    """The numbers the gate checks: main-loop depth, per-ISR depth (each with the exception frame)."""
    out = {"main": g.worst(main_root), "isr": {}}
    for r in isrs:
        if r in g.frame or r in g.direct:
            d, chain = g.worst(r)
            out["isr"][r] = (d + EXC_FRAME, chain)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cg", action="append", default=[], help="directory of LTO .ci files (default build/cg)")
    ap.add_argument("--build", default="build", help="build dir; its non-LTO .ci files are read too")
    ap.add_argument("--root", default=MAIN_ROOT)
    ap.add_argument("--isr", default=",".join(ISR_ROOTS))
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--no-resolve", action="store_true")
    ap.add_argument("--via", help="only chains through a function matching this regex (demangled name)")
    ap.add_argument("--page", help="only this page's virtuals are dispatched (regex on the class, e.g. ChargerTestPage)")
    ap.add_argument("--no-events", action="store_true", help="no on_event dispatch (no button press, no remote key)")
    ap.add_argument("--scenario", choices=sorted(SCENARIOS),
                    help="a measured situation: " + "; ".join(f"{k}: {v[0]}" for k, v in sorted(SCENARIOS.items())))
    a = ap.parse_args()
    if a.scenario:
        _, page, events = SCENARIOS[a.scenario]
        a.page, a.no_events = a.page or page, a.no_events or not events
    cg = a.cg or [str(pathlib.Path(a.build) / "cg")]
    files = ci_files(cg, a.build)
    if not files:
        print("no .ci files: build first (make docker-build)", file=sys.stderr)
        return 2
    g = Graph(*load_text(p.read_text(errors="replace") for p in files), resolve=not a.no_resolve,
              page=a.page, events=not a.no_events, via=a.via)
    if a.via:
        r = g.worst(a.root)
        if r is None:
            print(f"{a.root}: no chain through /{a.via}/")
            return 1
        res = {"main": r, "isr": {}}
        g.via = None
        res["isr"] = analyze(g, a.root, [x for x in a.isr.split(",") if x])["isr"]
    else:
        res = analyze(g, a.root, [r for r in a.isr.split(",") if r])
    d, chain = res["main"]
    what = ", ".join(x for x in (a.scenario and f"scenario {a.scenario}", a.page and f"page {a.page}",
                                 a.no_events and "no on_event", a.via and f"via /{a.via}/") if x)
    print(f"{a.root}: {d} bytes (main loop{', ' + what if what else ''})")
    print("\n".join(g.describe(chain, a.top)))
    tot = 0
    for r, (dd, ch) in sorted(res["isr"].items()):
        tot += dd
        print(f"{r}: {dd} bytes with the exception frame")
        if dd - EXC_FRAME > 64:
            print("\n".join(g.describe(ch, a.top)))
    print(f"ISR sum (nested, pessimistic): {tot} bytes")
    dyn = sorted(g.dm.get(n, n) for n, k in g.kind.items() if k != "static")
    if dyn:
        print("dynamic frames:", "; ".join(dyn))
    big = sorted(((f, n) for n, f in g.frame.items() if f), reverse=True)[:10]
    print("largest frames:", "; ".join(f"{g.dm.get(n, n)} {f}" for f, n in big))
    on_path = [g.dm.get(n, n) for n in chain if n in g.unresolved]
    if on_path:
        print("LOWER BOUND: unresolved indirect calls on the worst path:", "; ".join(on_path))
    if g.cut:
        print(f"{len(g.cut)} direct-call cycles cut (recursion):",
              "; ".join(sorted(f"{g.dm.get(s, s)} -> {g.dm.get(t, t)}" for s, t in g.cut)[:8]))
    if g.stale:
        print(f"{len(g.stale)} callers a RESOLVE rule matched but resolved to nothing (a renamed seam?):",
              "; ".join(sorted(g.dm.get(n, n) for n in g.stale)))
    if g.unresolved:
        print(f"{len(g.unresolved)} callers with unresolved indirect calls:",
              "; ".join(sorted(g.dm.get(n, n) for n in g.unresolved)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
