#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Reproduces the Phase 1 measurements from a clean checkout.
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-build}
OUT_DIR=${OUT_DIR:-results}

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$BUILD_DIR" -j"$(nproc)"

mkdir -p "$OUT_DIR"

echo "== environment =="
{
    echo "date:    $(date -Iseconds)"
    echo "kernel:  $(uname -sr)"
    echo "cpu:     $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "cores:   $(nproc) logical"
    # The compiler CMake actually used, not whatever "g++" resolves to. These
    # differ whenever CXX is set, and environment.txt is the provenance record a
    # reader trusts, so reporting the wrong one silently misattributes results.
    echo "gcc:     $("$(cmake -LA -N "$BUILD_DIR" 2>/dev/null | sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p')" --version 2>/dev/null | head -1)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'unavailable')"
    echo "paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
} | tee "$OUT_DIR/environment.txt"

echo
echo "== correctness =="
ctest --test-dir "$BUILD_DIR" --output-on-failure

echo
echo "== hardware counters =="
"$BUILD_DIR/counter_report" | tee "$OUT_DIR/counters.txt"

if [[ -x "$BUILD_DIR/bench_search" ]]; then
    echo
    echo "== timing =="
    "$BUILD_DIR/bench_search" \
        --benchmark_repetitions=5 \
        --benchmark_report_aggregates_only=true \
        --benchmark_out="$OUT_DIR/timing.json" \
        --benchmark_out_format=json
fi

echo
echo "results written to $OUT_DIR/"
