#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
# Reproduce the 2026-11-09 flaky-gate row (docs/ROADMAP.md "Flaky host tests"), or re-validate its fix:
# run the two named host tests under a CPU load of 2x nproc and report every run's verdict.
#
#   tools/flaky_repro.sh [tries]        default tries = 20
#
# Exit status: 0 = every run passed (green), 1 = at least one run failed (red).
# The host test binary is built on demand (make test builds it anyway).
set -u
cd "$(dirname "$0")/.."
tries=${1:-20}
mkdir -p runs

if [ ! -x host-build/test_dbg_stream ]; then
  make -s -Otarget host-build/test_dbg_stream >&2 || make -s host-build/test_dbg_stream >&2
fi

hogs=${FLAKY_HOGS:-$((4 * $(nproc)))}   # 4x nproc: the node watchdog needs a harsher load than the C++ one
for _ in $(seq "$hogs"); do (while :; do :; done) & done
trap 'kill $(jobs -p) 2>/dev/null' EXIT
sleep 0.2                      # let the load settle

red=0
for i in $(seq "$tries"); do
  if ./host-build/test_dbg_stream > runs/flaky_repro_dbg.$$ 2>&1; then
    echo "run $i: test_dbg_stream PASS"
  else
    echo "run $i: test_dbg_stream FAIL"; sed 's/^/    /' runs/flaky_repro_dbg.$$; red=1
  fi
  rm -f runs/flaky_repro_dbg.$$
  if node --test --test-name-pattern='screenshot: chunked strips' tests/test_k2proto.mjs > runs/flaky_repro_mjs.$$ 2>&1; then
    echo "run $i: test_k2proto.mjs screenshot PASS"
  else
    echo "run $i: test_k2proto.mjs screenshot FAIL"; grep -E 'fail|not ok|Error|error' runs/flaky_repro_mjs.$$ | head -8 | sed 's/^/    /'; red=1
  fi
  rm -f runs/flaky_repro_mjs.$$
done

if [ "$red" = 0 ]; then echo "GREEN: $tries/$tries runs passed under a ${hogs}-hog load"; else echo "RED: at least one run failed under a ${hogs}-hog load"; fi
exit "$red"
