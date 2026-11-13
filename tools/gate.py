#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""`make gate`: the one command every agent runs before committing. Fails fast with a clear message.

  1 build   docker build as the host uid, -Werror on our sources only
  2 invariants  section addresses and the four unmangled symbols the bootloader needs
  2b stack  link layout + worst call depth (tools/stack_depth.py: main loop incl. the deepest page path, ISRs,
            the [FAULT] handler) must fit the PSP / MSP stacks with margin; no interrupt priorities set
  3 map     no malloc / operator new / __cxa_ / 64-bit divmod linked
  4 ids     remote command ids unique across remote_proto.hpp, k2_remote.py, k2proto.js
  4b 64bit  no unallowlisted 64-bit integer types in src/ or include/k2/ (tools/check_64bit.py)
  5 tests   make test (C++, Python, node)
  6 size    report against tools/size_baseline.txt; over +5 % text is a WARNING, not a failure
  6b net    the K2_NET=1 variant (off by default) in build-net/: -Werror build, layout invariants, stack budget, map;
            only with K2_WEB=1 (env or `make K2_WEB=1 gate`), which also puts the web tests back into step 5

  7 static  (opt-in, --static) tools/static_check.sh sanitize: every host test under ASan+UBSan, zero
            src/ reports and zero failed tests

`python3 tools/gate.py --update-baseline` rewrites the baseline from the current build."""
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from stack_depth import EXC_FRAME, MAIN_ROOT, Graph, analyze, ci_files, load_text  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
IMAGE = "open-k2-builder:latest"
ELF = "build/open_k2.elf"
BASELINE = ROOT / "tools/size_baseline.txt"
SIZE_WARN_PCT = 5.0
RAM_TOP = 0x20018000          # 96 KB mapped (never touch SRAM above it)
MIN_STACK_BYTES = 8192        # coarse floor: .bss end to RAM top (guard + PSP + MSP); the depth check below is the real one
STACK_MARGIN = 1024           # PSP bytes left over after the worst main-loop path plus one exception frame
MSP_MARGIN = 64               # MSP bytes left over after the deepest ISR or the [FAULT] handler

REQUIRED_SECTIONS = {".isr_vector": 0x08003800, ".meta_version": 0x08004000, ".meta_model": 0x08004010,
                     ".meta_commit": 0x08004400, ".text": 0x08004404}
REQUIRED_SYMBOLS = ("Reset_Handler", "USB_LP_CAN1_RX0_IRQHandler", "g_pfnVectors", "k2_model_signature")
FORBIDDEN_MAP = (("malloc", re.compile(r"(?<![A-Za-z0-9_])malloc\b|_malloc_r\b|a-mallocr")), ("operator new", re.compile(r"_Zn[aw][jm]|operator new")),
                 ("__cxa_", re.compile(r"__cxa_")),
                 ("_udivmoddi4", re.compile(r"\b_?_udivmoddi4\b")),
                 ("__aeabi_ldivmod", re.compile(r"\b__aeabi_ldivmod\b|_aeabi_ldivmod\.o")),
                 ("__aeabi_uldivmod", re.compile(r"\b__aeabi_uldivmod\b|_aeabi_uldivmod\.o")))


class GateError(Exception):
    pass


# ---- pure checks (unit-tested in tests/test_gate.py) -------------------------------------------

def check_invariants(objdump_h, nm):
    errs, vma = [], {}
    for line in objdump_h.splitlines():
        f = line.split()
        if len(f) >= 4 and f[0].isdigit() and f[1].startswith("."):
            vma[f[1]] = int(f[3], 16)
    for name, addr in REQUIRED_SECTIONS.items():
        if name not in vma:
            errs.append(f"section {name} missing (must be at 0x{addr:08X})")
        elif vma[name] != addr:
            errs.append(f"section {name} at 0x{vma[name]:08X}, must be 0x{addr:08X}")
    symbols = {line.split()[-1] for line in nm.splitlines() if line.split()}
    for s in REQUIRED_SYMBOLS:
        if s not in symbols:
            errs.append(f"symbol {s} missing or mangled")
    return errs


def parse_symbols(nm):
    """{name: address} from `nm` output (symbols without an address are skipped)."""
    out = {}
    for line in nm.splitlines():
        f = line.split()
        if len(f) == 3:
            try:
                out[f[2]] = int(f[0], 16)
            except ValueError:
                pass
    return out


STACK_SYMBOLS = ("_ebss", "_sguard", "_eguard", "_psp_top", "_estack")


def check_stack(sym, main_depth, isr_depth, fault_depth):
    """The stack budget, from the link layout (`nm` symbols) and the worst call depths of
    tools/stack_depth.py (main_depth: main loop incl. the deepest page path, without the exception frame;
    isr_depth: deepest ISR incl. its frame; fault_depth: the [FAULT] handler).

    Layout: .bss | MPU guard (_sguard.._eguard, no-access) | PSP stack (_eguard.._psp_top, thread mode) |
    MSP stack (_psp_top.._estack, exceptions and the fault handler). Errors:
      - the layout symbols are missing, the guard is not a valid MPU region, the pieces are out of order
      - fewer than MIN_STACK_BYTES between the end of .bss and the top of RAM (coarse floor)
      - PSP stack < main_depth + exception frame + STACK_MARGIN
      - MSP stack < max(isr_depth, fault_depth) + MSP_MARGIN"""
    missing = [s for s in STACK_SYMBOLS if s not in sym]
    if missing:
        return [f"stack layout symbols missing from the link: {', '.join(missing)}"]
    errs = []
    guard = sym["_eguard"] - sym["_sguard"]
    if guard < 32 or guard & (guard - 1) or sym["_sguard"] % guard:
        errs.append(f"stack guard 0x{sym['_sguard']:08X}..0x{sym['_eguard']:08X} is not a power-of-two sized, "
                    f"size-aligned MPU region (>= 32 B)")
    if not sym["_ebss"] <= sym["_sguard"] < sym["_eguard"] <= sym["_psp_top"] <= sym["_estack"]:
        errs.append("stack layout out of order, need _ebss <= _sguard < _eguard <= _psp_top <= _estack")
        return errs
    if sym["_estack"] != RAM_TOP:
        errs.append(f"_estack 0x{sym['_estack']:08X} is not the top of the mapped RAM 0x{RAM_TOP:08X}")
    total = RAM_TOP - sym["_ebss"]
    if total < MIN_STACK_BYTES:
        errs.append(f"only {total} bytes between .bss and the top of RAM, need {MIN_STACK_BYTES}: "
                    f"shrink .bss (nm --size-sort build/open_k2.elf)")
    psp, msp = sym["_psp_top"] - sym["_eguard"], sym["_estack"] - sym["_psp_top"]
    need = main_depth + EXC_FRAME + STACK_MARGIN
    if psp < need:
        errs.append(f"PSP stack {psp} B < worst main-loop depth {main_depth} + exception frame {EXC_FRAME} + "
                    f"margin {STACK_MARGIN} = {need} B (short by {need - psp}): shrink the frames on the path "
                    f"printed by `python3 tools/stack_depth.py`, or .bss")
    need = max(isr_depth, fault_depth) + MSP_MARGIN
    if msp < need:
        errs.append(f"MSP stack {msp} B < worst ISR {isr_depth} / [FAULT] handler {fault_depth} + margin "
                    f"{MSP_MARGIN} = {need} B: grow _msp_size in link.ld or shrink the handler path")
    return errs


# Everything runs at the reset priority, so no interrupt nests and the MSP only has to hold one ISR. A source
# that sets priorities invalidates the depth analysis: fail so the analysis (and this check) get updated.
PRIORITY_WRITE = re.compile(r"NVIC_SetPriority|NVIC->IP|\bSHPR\s*\[[^]]*\]\s*(?:=|\|=)|\bIPR?\s*\[[^]]*\]\s*=")


def check_no_priorities(files):
    """files: {path: text}. The lines that write an interrupt priority."""
    return [f"{p}:{n}: {line.strip()}" for p, text in sorted(files.items())
            for n, line in enumerate(text.splitlines(), 1) if PRIORITY_WRITE.search(line)]


def check_map(text):
    head = text.split("Discarded input sections", 1)
    live = head[0] + (head[1].split("Linker script and memory map", 1)[1] if len(head) > 1
                      and "Linker script and memory map" in head[1] else "")
    return [f"map links {label}: {m.group(0)!r}" for label, rx in FORBIDDEN_MAP
            for m in [rx.search(live)] if m]


def parse_size(text):
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 3 and all(x.isdigit() for x in f[:3]):
            return {"text": int(f[0]), "data": int(f[1]), "bss": int(f[2])}
    raise GateError("cannot parse arm-none-eabi-size output")


def parse_baseline(text):
    d = dict(re.findall(r"(text|data|bss)=(\d+)", text))
    if set(d) != {"text", "data", "bss"}:
        raise GateError("tools/size_baseline.txt must contain text=N data=N bss=N")
    return {k: int(v) for k, v in d.items()}


def size_report(now, base):
    lines, warn = [], False
    for k in ("text", "data", "bss"):
        d = now[k] - base[k]
        pct = 100.0 * d / base[k] if base[k] else 0.0
        lines.append(f"  {k:5} {now[k]:8} (baseline {base[k]:8}, {d:+d}, {pct:+.1f} %)")
        if k == "text" and pct > SIZE_WARN_PCT:
            warn = True
    return lines, warn


# ---- steps ---------------------------------------------------------------------------------------

def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True, **kw)


def docker_tool(*args):
    uid = run(["id", "-u"]).stdout.strip(), run(["id", "-g"]).stdout.strip()
    r = run(["docker", "run", "--rm", "--user", f"{uid[0]}:{uid[1]}", "-v", f"{ROOT}:/workspace",
             "-w", "/workspace", IMAGE, *args])
    if r.returncode:
        raise GateError(f"{' '.join(args[:2])} failed: {r.stderr.strip()}")
    return r.stdout


def foreign_owned(build, uid):
    """Entries under `build` (itself included) not owned by `uid`; [] when it does not exist."""
    out = []
    if not build.exists():
        return out
    for dirpath, dirs, files in os.walk(build):
        for p in [Path(dirpath)] + [Path(dirpath) / n for n in dirs + files]:
            try:
                if p.lstat().st_uid != uid:
                    out.append(p)
            except OSError:
                pass
    return out


def _docker_chown(build):
    r = run(["docker", "run", "--rm", "-v", f"{ROOT}:/workspace", "-w", "/workspace", IMAGE,
             "chown", "-R", f"{os.getuid()}:{os.getgid()}", f"/workspace/{build.relative_to(ROOT)}"])
    if r.returncode:
        raise GateError(f"could not take ownership of build/ ({r.stderr.strip()}); run: "
                        f"sudo chown -R {os.getuid()}:{os.getgid()} {build}")


def ensure_build_owned(build, uid, gid, owned=foreign_owned, chown=_docker_chown, say=print):
    """A build/ left behind by an older root docker run breaks the host-uid build: hand it back first."""
    bad = owned(build, uid)
    if bad:
        say(f"build/ has {len(bad)} entries not owned by uid {uid} (e.g. {bad[0]}), left by a root docker run; "
            "fixing with a docker chown")
        chown(build)


def step_build(build="build", *make_args):
    ensure_build_owned(ROOT / build, os.getuid(), os.getgid())
    log = ROOT / build
    log.mkdir(exist_ok=True)
    r = run(["make", "docker-build", "K2_WERROR=1", f"BUILD_DIR={build}", *make_args])
    (log / "gate_build.log").write_text(r.stdout + r.stderr)
    if r.returncode:
        text = r.stdout + r.stderr
        errs = [l for l in text.splitlines() if re.search(r"error|warning", l)][:15]
        raise GateError(f"build failed (warnings are errors in our sources; full log {build}/gate_build.log):\n  "
                        + "\n  ".join(errs or text.splitlines()[-15:]))


def step_invariants(build="build"):
    elf = f"{build}/open_k2.elf"
    errs = check_invariants(docker_tool("arm-none-eabi-objdump", "-h", elf), docker_tool("arm-none-eabi-nm", elf))
    if errs:
        raise GateError("bootloader layout invariants broken (a wrong layout leaves the K2 in DFU):\n  "
                        + "\n  ".join(errs))


FAULT_ROOT = "k2_fault_main"


def stack_depths(root=ROOT, build="build"):
    """(main depth, ISR depth, fault depth, unresolved functions on the worst main path) from the .ci call
    graphs of the last build (tools/stack_depth.py: page virtuals and callbacks are followed)."""
    files = ci_files([root / build / "cg"], root / build)
    if not files:
        raise GateError(f"no .ci call-graph files under {build}/: build with the -fcallgraph-info flags of the Makefile")
    g = Graph(*load_text(p.read_text(errors="replace") for p in files))
    for r in (MAIN_ROOT, FAULT_ROOT):
        if r not in g.frame:
            raise GateError(f"{r} is missing from the call graph, so the depth analysis cannot run")
    res = analyze(g)
    main, chain = res["main"]
    isr = max((d for d, _ in res["isr"].values()), default=0)
    return main, isr, g.worst(FAULT_ROOT)[0], [g.dm.get(n, n) for n in chain if n in g.unresolved]


def step_stack(build="build"):
    sym = parse_symbols(docker_tool("arm-none-eabi-nm", f"{build}/open_k2.elf"))
    main, isr, fault, lower = stack_depths(ROOT, build)
    if sym.get("_ebss"):
        print(f"  free RAM above .bss {RAM_TOP - sym['_ebss']} B (floor {MIN_STACK_BYTES} B)")
    print(f"  main loop {main} B, deepest ISR {isr} B, [FAULT] handler {fault} B "
          f"(python3 tools/stack_depth.py shows the path)")
    errs = check_stack(sym, main, isr, fault)
    if lower:
        errs.append("the worst main-loop path has indirect calls the analysis cannot resolve, so its depth is only "
                    "a lower bound: add a RESOLVE rule in tools/stack_depth.py for " + "; ".join(lower))
    srcs = {str(p.relative_to(ROOT)): p.read_text(errors="replace")
            for d in ("src", "include") for p in sorted((ROOT / d).rglob("*")) if p.suffix in (".cpp", ".hpp", ".h", ".c")
            and "third_party" not in p.parts and p.name != "at32f403a_407.h"}
    errs += [f"interrupt priority written (the ISR depth analysis assumes no nesting): {s}"
             for s in check_no_priorities(srcs)]
    if errs:
        raise GateError("stack budget:\n  " + "\n  ".join(errs))
    if sym.get("_psp_top") and sym.get("_eguard"):
        psp = sym["_psp_top"] - sym["_eguard"]
        print(f"  PSP stack {psp} B, {psp - main - EXC_FRAME} B free after the worst path")


def step_map(build="build"):
    errs = check_map((ROOT / build / "open_k2.map").read_text())
    if errs:
        raise GateError("heap or C++ runtime linked into the firmware:\n  " + "\n  ".join(errs))


def step_ids():
    r = run([sys.executable, "tools/check_cmd_ids.py"])
    if r.returncode:
        raise GateError("remote command ids collide:\n  " + r.stderr.strip().replace("\n", "\n  "))


def step_64bit():
    r = run([sys.executable, "tools/check_64bit.py", "--mode", "block"])
    if r.returncode:
        raise GateError("unallowlisted 64-bit integer types:\n  " + (r.stderr or r.stdout).strip().replace("\n", "\n  "))


def step_tests():
    r = run(["make", f"-j{os.cpu_count() or 4}", "-Otarget", "test"])
    if r.returncode:
        raise GateError("make test failed:\n" + "\n".join((r.stdout + r.stderr).splitlines()[-25:]))


def step_size():
    now = parse_size(docker_tool("arm-none-eabi-size", ELF))
    if "--update-baseline" in sys.argv:
        BASELINE.write_text(f"# arm-none-eabi-size of build/open_k2.elf (one image, all USB modes); update with "
                            f"`python3 tools/gate.py --update-baseline`\n"
                            f"text={now['text']} data={now['data']} bss={now['bss']}\n")
        print("  baseline updated")
        return
    lines, warn = size_report(now, parse_baseline(BASELINE.read_text()))
    print("\n".join(lines))
    if warn:
        print(f"  WARNING: text grew more than {SIZE_WARN_PCT:g} % over the baseline. Explain the growth "
              "in the commit body (Key Changes), or update the baseline if it is intended.")


def sanitize_errors(log):
    """Sanitizer reports outside third_party, plus failed test targets, from build/static/sanitize.log."""
    lines = log.splitlines()
    return ([l for l in lines if re.search(r"runtime error|AddressSanitizer", l) and "third_party" not in l]
            + [l for l in lines if l.startswith("make: ***")])


def step_static():
    r = run(["bash", "tools/static_check.sh", "sanitize"])
    if r.returncode:
        raise GateError("tools/static_check.sh sanitize did not run:\n  " + (r.stdout + r.stderr).strip())
    errs = sanitize_errors((ROOT / "build/static/sanitize.log").read_text())
    if errs:
        raise GateError("ASan/UBSan host tests (full log build/static/sanitize.log):\n  " + "\n  ".join(errs[:15]))


NET_BUILD = "build-net"       # the K2_NET=1 variant's own build directory (the default build is K2_NET=0)


def step_net_variant():
    """The K2_NET=1 build (lwIP, NCM, the web dashboard) is off by default, so nothing else compiles it: build it in
    its own directory (incremental after the first run) and hold it to the same -Werror, layout, stack and map
    checks. Its size is not compared against the baseline (that is the default image's)."""
    step_build(NET_BUILD, "K2_NET=1")
    step_invariants(NET_BUILD)
    step_stack(NET_BUILD)
    step_map(NET_BUILD)
    print(f"  K2_NET=1 variant ({NET_BUILD}/open_k2.elf): " + parse_size_line(docker_tool("arm-none-eabi-size", f"{NET_BUILD}/open_k2.elf")))


def parse_size_line(text):
    s = parse_size(text)
    return f"text={s['text']} data={s['data']} bss={s['bss']}"


WEB = os.environ.get("K2_WEB") == "1"   # the web parts (Makefile K2_WEB): off by default

STEPS = [("build (-Werror, docker as host uid)", step_build), ("bootloader invariants", step_invariants),
         ("stack budget", step_stack), ("map: no malloc / operator new / __cxa_ / 64-bit divmod", step_map),
         ("remote command ids", step_ids), ("64-bit integer guard", step_64bit),
         ("tests (C++, Python" + (", node)" if WEB else "; web off)"), step_tests), ("size report", step_size)] + (
         [("K2_NET=1 variant (-Werror, layout, stack, map)", step_net_variant)] if WEB else [])


def main():
    steps = STEPS + ([("static: ASan/UBSan host tests", step_static)] if "--static" in sys.argv else [])
    for i, (name, fn) in enumerate(steps, 1):
        print(f"== gate {i}/{len(steps)}: {name}", flush=True)
        try:
            fn()
        except GateError as e:
            print(f"\nGATE FAILED at step {i} ({name}):\n{e}", file=sys.stderr)
            return 1
    print("\nGATE PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
