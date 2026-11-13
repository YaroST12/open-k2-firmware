#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
# Static analysis over the firmware sources (src/, include/; never third_party). Report only:
# it always exits 0 unless a tool itself fails to start, so it can run as a non-blocking step.
#
#   tools/static_check.sh [tidy|gcc|cppcheck|sanitize|all]     (default: all = tidy, gcc, cppcheck)
#
# Results land in build/static/: tidy.log, gcc.log (ARM g++ -fanalyzer + extra warnings, run in
# the open-k2-builder image), cppcheck.log (cppcheck installed into a throwaway container), and summary.txt with counts
# per tool. Needs build/gen (run `make docker-build` once first). Checks and suppressions: .clang-tidy, tools/cppcheck.supp.
#
# `sanitize` is the one mode that is also a default step of `make gate` (tools/gate.py step_sanitize): every host test
# built and run under sanitizers in the open-k2-builder image (as the host uid), once with g++ and once with clang, each
# in its own build directory (build/san-gcc, build/san-clang; incremental, so a warm run only rebuilds what changed) with
# its own log (build/static/sanitize-gcc.log, sanitize-clang.log). The sets:
#   g++    address, undefined, float-divide-by-zero, float-cast-overflow (leak detection is on)
#   clang  the same plus local-bounds and implicit-integer-sign-change (a negative value reaching an unsigned type).
#          Left out on purpose: unsigned-integer-overflow (the firmware wraps millisecond counters and hashes on
#          purpose: thousands of reports) and implicit-conversion's truncation checks (every narrow ++ and masked store
#          reports). third_party and system headers are exempt from sign-change through tools/sanitize_ignorelist.txt.
# Every check is fatal (-fno-sanitize-recover=all): a report aborts its test, which fails the gate.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/static"
mkdir -p "$out"
what=${1:-all}
img=open-k2-builder:latest
inc=(-Iinclude -isystem third_party/tinyusb/src -DCFG_TUSB_MCU=OPT_MCU_AT32F403A_407
     -Ithird_party/lwip/src/include -Ithird_party/tinyusb/lib/networking -Ithird_party/littlefs
     -Ibuild/gen)

run_tidy() {
    python3 "$root/tools/gen_compdb.py" >/dev/null
    (cd "$root" && run-clang-tidy -p build -j "$(nproc)" -quiet 'src/') >"$out/tidy.log" 2>&1
}

run_gcc() {
    local flags
    flags="-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -Os -std=c++23"
    flags+=" -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -Wno-volatile"
    flags+=" -Wall -Wextra -Wshadow -Wconversion -Wnull-dereference -Wduplicated-cond -Wlogical-op"
    flags+=" -Wcast-align -fanalyzer ${inc[*]}"
    docker run --rm -v "$root":/workspace -w /workspace "$img" sh -c \
        "find src -name '*.cpp' | sort | xargs -P $(nproc) -I{} sh -c 'arm-none-eabi-g++ $flags -c {} -o /dev/null 2>&1'" \
        >"$out/gcc.log" 2>&1
}

run_cppcheck() {
    docker run --rm -v "$root":/workspace -w /workspace "$img" sh -c \
        "(apt-get update -qq && apt-get install -y -qq cppcheck) >/dev/null 2>&1;
         cppcheck --enable=all --std=c++20 --language=c++ -j $(nproc) --inline-suppr \
           --suppressions-list=tools/cppcheck.supp --template='{file}:{line}: {severity}: {message} [{id}]' \
           -Iinclude -D__GNUC__ -DCFG_TUSB_MCU=OPT_MCU_AT32F403A_407 src include/k2 2>&1" \
        >"$out/cppcheck.log" 2>&1
}

# One sanitizer pass: $1 = name (build dir and log), $2 = C++ compiler command, $3 = C compiler command, $4 = flags.
sanitize_pass() {
    local flags="$4 -fno-sanitize-recover=all -fno-omit-frame-pointer"
    # make does not see a change of flags or of compiler: stamp both in the build directory and start clean on a change.
    local dir="$root/build/san-$1" stamp
    stamp="$2 $3 $flags $(docker image inspect -f '{{.Id}}' "$img" 2>/dev/null)"
    [ "$(cat "$dir/.stamp" 2>/dev/null)" = "$stamp" ] || rm -rf "$dir"
    mkdir -p "$dir" && printf '%s' "$stamp" >"$dir/.stamp"
    docker run --rm --user "$(id -u):$(id -g)" -e UBSAN_OPTIONS=print_stacktrace=1 -v "$root":/workspace -w /workspace "$img" \
        make -k -j"$(nproc)" test-run HOST_DIR="build/san-$1" HOST_CXX="$2 $flags" HOST_CC="$3 $flags" \
        >"$out/sanitize-$1.log" 2>&1
    local rc=$?
    # The closing line the gate reads: an incremental run re-runs only the changed tests, so the log alone cannot say that
    # the whole suite passed; the per-test stamps (one per passed binary) can.
    echo "[sanitize] $1: docker exit $rc, $(find "$dir/run" -name '*.ok' 2>/dev/null | wc -l) tests passed" >>"$out/sanitize-$1.log"
}

run_sanitize() {
    local base="-fsanitize=address,undefined,float-divide-by-zero,float-cast-overflow"
    sanitize_pass gcc g++ gcc "$base" &
    sanitize_pass clang clang++ clang \
        "$base,local-bounds,implicit-integer-sign-change -fsanitize-ignorelist=tools/sanitize_ignorelist.txt" &
    wait
}

case "$what" in
    tidy) run_tidy ;;
    gcc) run_gcc ;;
    cppcheck) run_cppcheck ;;
    sanitize) run_sanitize ;;
    all) run_tidy & run_gcc & run_cppcheck & wait ;;
    *) echo "usage: $0 [tidy|gcc|cppcheck|sanitize|all]" >&2; exit 2 ;;
esac

{
    for f in tidy gcc cppcheck; do
        [ -f "$out/$f.log" ] || continue
        printf '%-9s %5d warnings\n' "$f" "$(grep -cE ': (warning|error|style|performance|portability):' "$out/$f.log")"
        grep -oE '\[[-A-Za-z0-9.=,]+\]$' "$out/$f.log" | sort | uniq -c | sort -rn | head -15 | sed 's/^/          /'
    done
    for f in gcc clang; do
        [ -f "$out/sanitize-$f.log" ] || continue
        printf '%-9s %5d reports outside third_party, %d failed tests\n' "sanitize-$f" \
            "$(grep -E 'runtime error|AddressSanitizer' "$out/sanitize-$f.log" | grep -vc third_party)" \
            "$(grep -c '^make: \*\*\*' "$out/sanitize-$f.log")"
    done
} | tee "$out/summary.txt"
exit 0
