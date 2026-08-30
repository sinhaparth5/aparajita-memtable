#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Phase 5: the two measurements the paper's evaluation was missing.
#
# Review of the Phase 4 draft found that the paper justifies an ordered design
# against VectorRep on the grounds that "ordering is what a MemTable iterator has
# to provide", and then never measures an ordered workload. It also reports arena
# per key only against Aparajita's own earlier self, never against the skiplist,
# on a configuration whose write buffer is large enough that the difference costs
# nothing.
#
# Part A closes the first: seekrandom at seek_nexts 0 and 10, which is a Seek
# alone and a Seek followed by a short scan, over the same resident memtable the
# Phase 4 read numbers use.
#
# Part B closes the second: the same fill against a bounded write buffer, so the
# rep's arena cost per key shows up as flushes, with rocksdb.db.flush.micros for
# what each of those flushes costs. Iterating a structure is what a flush does, so
# a rep with a cheap iterator can be behind on arena and still even on flush time.
#
# The same discipline as scripts/run_phase4.sh: one host, one build, an idle
# machine, nothing rounded by hand on the way out.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT_DIR=${OUT_DIR:-$REPO_ROOT/results/phase5-ordered}
ROCKSDB_SRC=${ROCKSDB_SRC:-$(dirname "$REPO_ROOT")/rocksdb-src}
BUILD_DIR=${BUILD_DIR:-$ROCKSDB_SRC/build}
DB_PATH=${DB_PATH:-/tmp/aparajita-phase5-db}

KEYSPACE=${KEYSPACE:-2000000}
TOTAL_OPS=${TOTAL_OPS:-2000000}
THREAD_COUNTS=${THREAD_COUNTS:-"1 4 16 64"}
REPETITIONS=${REPETITIONS:-5}
REPS=${REPS:-"aparajita skip_list"}

# Part A holds the memtable resident, exactly as Phase 4 does, so the seek numbers
# sit alongside the point-lookup ones rather than measuring a different structure.
WRITE_BUFFER=${WRITE_BUFFER:-4294967296}
SEEKS_FULL=${SEEKS_FULL:-100000}
SEEK_NEXTS=${SEEK_NEXTS:-"0 10"}

# Part B wants flushes, so the buffer is small enough that the fill produces
# several. 64 MiB is RocksDB's own default and is the figure the phase 4b
# occupancy measurement used, which makes the two directly comparable.
FLUSH_BUFFER=${FLUSH_BUFFER:-67108864}
FLUSH_REPETITIONS=${FLUSH_REPETITIONS:-3}

BENCH_TIMEOUT=${BENCH_TIMEOUT:-1800}

mkdir -p "$OUT_DIR/raw"

fail() { echo "run_phase5: $*" >&2; exit 1; }

[[ -x "$BUILD_DIR/db_bench" ]] || fail "no db_bench at $BUILD_DIR; run scripts/build_rocksdb_plugin.sh first"

LOADAVG=$(cut -d' ' -f1 /proc/loadavg)
if (( $(echo "$LOADAVG > 1.0" | bc -l) )); then
    echo "run_phase5: load average is $LOADAVG before starting." >&2
    [[ -n "${ALLOW_LOADED_HOST:-}" ]] || fail "refusing to measure a busy host (set ALLOW_LOADED_HOST=1 to override)"
fi

{
    echo "date:      $(date -Iseconds)"
    echo "host:      $(hostname)"
    echo "kernel:    $(uname -sr)"
    echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    echo "vcpu:      $(nproc) logical"
    echo "cores:     $(lscpu | sed -n 's/^Core(s) per socket: *//p') per socket, $(lscpu | sed -n 's/^Socket(s): *//p') socket(s)"
    echo "smt:       $(cat /sys/devices/system/cpu/smt/active 2>/dev/null || echo unknown)"
    echo "memory:    $(awk '/MemTotal/ {printf "%.0f GiB", $2/1048576}' /proc/meminfo)"
    echo "isa:       $(grep -m1 -o 'avx512f' /proc/cpuinfo || echo 'no avx512')"
    echo "gcc:       $(${CXX:-g++} --version 2>/dev/null | head -1 || echo unknown)"
    echo "rocksdb:   $(git -C "$ROCKSDB_SRC" describe --tags 2>/dev/null || echo unknown)"
    echo "aparajita: ${APARAJITA_REV:-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout')}"
    echo "keyspace:  $KEYSPACE"
    echo "wbuf_A:    $WRITE_BUFFER"
    echo "wbuf_B:    $FLUSH_BUFFER"
} | tee "$OUT_DIR/environment.txt"

# --------------------------------------------------------------------------
# Part A: ordered access
# --------------------------------------------------------------------------
# fillrandom and seekrandom chain inside one process, so the seeks run against the
# memtable the fill just populated rather than against reopened SSTs.
run_seek() {
    local rep=$1 threads=$2 nexts=$3 iter=$4
    local writes=$(( TOTAL_OPS / threads ))
    local seeks=$(( SEEKS_FULL / threads ))
    (( seeks > 0 )) || seeks=1
    local log="$OUT_DIR/raw/seek.n${nexts}.${rep}.t${threads}.i${iter}.txt"
    rm -rf "$DB_PATH"

    echo "-- seek nexts=$nexts rep=$rep threads=$threads iter=$iter"
    if ! timeout "$BENCH_TIMEOUT" "$BUILD_DIR/db_bench" \
        --benchmarks=fillrandom,seekrandom \
        --memtablerep="$rep" \
        --num="$KEYSPACE" \
        --writes="$writes" \
        --reads="$seeks" \
        --seek_nexts="$nexts" \
        --threads="$threads" \
        --disable_wal=1 \
        --write_buffer_size="$WRITE_BUFFER" \
        --max_write_buffer_number=4 \
        --disable_auto_compactions=1 \
        --compression_type=none \
        --seed=42 \
        --db="$DB_PATH" > "$log" 2>&1
    then
        echo "   FAILED (see $log)" >&2
        return 1
    fi
    echo "sst_files: $(find "$DB_PATH" -name '*.sst' 2>/dev/null | wc -l)" >> "$log"
    grep -E 'micros/op' "$log" | sed 's/^/   /' || true
}

for nexts in $SEEK_NEXTS; do
    for rep in $REPS; do
        for t in $THREAD_COUNTS; do
            for i in $(seq 1 "$REPETITIONS"); do
                run_seek "$rep" "$t" "$nexts" "$i" || true
            done
        done
    done
done

# --------------------------------------------------------------------------
# Part B: arena cost and flush cost, at a write buffer small enough to flush
# --------------------------------------------------------------------------
run_flush() {
    local rep=$1 iter=$2
    local log="$OUT_DIR/raw/flush.${rep}.i${iter}.txt"
    rm -rf "$DB_PATH"

    echo "-- flush rep=$rep iter=$iter"
    if ! timeout "$BENCH_TIMEOUT" "$BUILD_DIR/db_bench" \
        --benchmarks=fillrandom \
        --memtablerep="$rep" \
        --num="$KEYSPACE" \
        --threads=1 \
        --disable_wal=1 \
        --write_buffer_size="$FLUSH_BUFFER" \
        --max_write_buffer_number=4 \
        --disable_auto_compactions=1 \
        --compression_type=none \
        --statistics \
        --stats_interval_seconds=0 \
        --seed=42 \
        --db="$DB_PATH" > "$log" 2>&1
    then
        echo "   FAILED (see $log)" >&2
        return 1
    fi
    echo "sst_files: $(find "$DB_PATH" -name '*.sst' 2>/dev/null | wc -l)" >> "$log"
    grep -E 'micros/op|rocksdb.db.flush.micros' "$log" | sed 's/^/   /' || true
}

for rep in $REPS; do
    for i in $(seq 1 "$FLUSH_REPETITIONS"); do
        run_flush "$rep" "$i" || true
    done
done

rm -rf "$DB_PATH"
python3 "$REPO_ROOT/scripts/phase5_summarize.py" "$OUT_DIR" | tee "$OUT_DIR/summary.txt"
echo "run_phase5: wrote $OUT_DIR"
