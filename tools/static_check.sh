#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
# Static analysis over the firmware sources (src/, include/; never third_party). Report only:
# it always exits 0 unless a tool itself fails to start, so it can run as a non-blocking step.
#
#   tools/static_check.sh [tidy|gcc|cppcheck|sanitize|all]     (default: all)
#
# Results land in build/static/: tidy.log, gcc.log (ARM g++ -fanalyzer + extra warnings, run in
# the open-k2-builder image), cppcheck.log (cppcheck installed into a throwaway container), and a
# sanitize.log (every host test built with
# -fsanitize=address,undefined into host-asan/), and summary.txt with counts per tool. Needs build/gen (run `make docker-build` once first).
# Checks and suppressions: .clang-tidy, tools/cppcheck.supp.
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

run_sanitize() {
    # lwIP's misaligned-pbuf reports are host-only (MEM_ALIGNMENT 4 with 8-byte pointers): ignored.
    (cd "$root" && make -k -j "$(nproc)" test-run HOST_DIR=host-asan \
        HOST_CXX="g++ -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" \
        HOST_CC="gcc -fsanitize=address,undefined -fno-omit-frame-pointer") >"$out/sanitize.log" 2>&1
}

case "$what" in
    tidy) run_tidy ;;
    gcc) run_gcc ;;
    cppcheck) run_cppcheck ;;
    sanitize) run_sanitize ;;
    all) run_tidy & run_gcc & run_cppcheck & run_sanitize & wait ;;
    *) echo "usage: $0 [tidy|gcc|cppcheck|sanitize|all]" >&2; exit 2 ;;
esac

{
    for f in tidy gcc cppcheck; do
        [ -f "$out/$f.log" ] || continue
        printf '%-9s %5d warnings\n' "$f" "$(grep -cE ': (warning|error|style|performance|portability):' "$out/$f.log")"
        grep -oE '\[[-A-Za-z0-9.=,]+\]$' "$out/$f.log" | sort | uniq -c | sort -rn | head -15 | sed 's/^/          /'
    done
    if [ -f "$out/sanitize.log" ]; then
        printf '%-9s %5d src/ reports, %d failed tests\n' sanitize \
            "$(grep -E 'runtime error|AddressSanitizer' "$out/sanitize.log" | grep -vc third_party)" \
            "$(grep -c '^make: \*\*\*' "$out/sanitize.log")"
    fi
} | tee "$out/summary.txt"
exit 0
