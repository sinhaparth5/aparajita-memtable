#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Phase 4: the empirical evaluation, reproducible from a clean checkout.
#
# Runs db_bench against Aparajita, RocksDB's default skiplist and VectorRep across
# the thread counts ROADMAP.md names, then collects hardware counters for each rep.
# Everything lands in $OUT_DIR as raw tool output plus a parsed summary; nothing is
# rounded or transcribed by hand on the way.
#
# This is a measurement script and it is deliberately unhelpful about where it
# runs. CLAUDE.md's rule is that paper-grade numbers come from a rented machine,
# and the checks below enforce the parts of that which are checkable: an idle
# machine, a readable PMU, and a compiler new enough not to ignore the pragma on
# the scalar baseline.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT_DIR=${OUT_DIR:-$REPO_ROOT/results/phase4-eval}
ROCKSDB_SRC=${ROCKSDB_SRC:-$(dirname "$REPO_ROOT")/rocksdb-src}
BUILD_DIR=${BUILD_DIR:-$ROCKSDB_SRC/build}
DB_PATH=${DB_PATH:-/tmp/aparajita-phase4-db}

# Total operations is held constant across thread counts, so num is per thread.
# A scaling curve that also grows the work per point measures two things at once.
TOTAL_OPS=${TOTAL_OPS:-2000000}

# The keyspace, held fixed at every thread count, and this separation is the
# whole reason the curve means anything. db_bench's --num is per thread and sets
# BOTH the work each thread does and the range its keys are drawn from, so
# dividing it by the thread count to hold total work constant silently shrinks
# the keyspace with every step: at 1 thread the memtable ends up holding 1.26M
# distinct keys and at 16 threads 125k, each written sixteen times over. That is
# a working-set sweep wearing a thread-scaling curve's clothes. --num therefore
# stays constant and --writes/--reads carry the per-thread work.
KEYSPACE=${KEYSPACE:-2000000}
THREAD_COUNTS=${THREAD_COUNTS:-"1 4 16 64"}
REPETITIONS=${REPETITIONS:-5}

# Large enough that nothing flushes: the memtable is the object under test, and a
# flush partway through turns the measurement into a comparison of SST writers.
# Verified rather than assumed -- every run reports its SST count and the summary
# flags any that is not zero.
WRITE_BUFFER=${WRITE_BUFFER:-4294967296}

# VectorRep has no ordered index, and the consequence is larger than it sounds.
# VectorRep::Get builds an iterator, and for a *mutable* memtable the iterator
# copies the entire bucket and sorts the copy, because the sorted order cannot be
# cached on a structure that is still being appended to. That is O(n log n) per
# point lookup against a two-million-entry memtable, not O(log n). A full read
# pass would run for days.
#
# Its read benchmarks are therefore capped hard and reported as capped. The
# per-operation figure stays comparable because it is a mean over operations
# against an identically sized memtable -- the cap changes the sample size, not
# the thing being sampled.
READS_FULL=${READS_FULL:-200000}
READS_VECTOR=${READS_VECTOR:-20}
BENCH_TIMEOUT=${BENCH_TIMEOUT:-900}

# The counter pass runs its read benchmarks far longer than the timing pass does,
# so that the operations under test dominate the fill they sit behind and the
# two-point difference is a difference between two close numbers rather than
# between two large ones.
COUNTER_THREADS=${COUNTER_THREADS:-16}
COUNTER_READS=${COUNTER_READS:-20000000}
COUNTER_READS_VECTOR=${COUNTER_READS_VECTOR:-20}

mkdir -p "$OUT_DIR/raw"

# --------------------------------------------------------------------------
# Preflight
# --------------------------------------------------------------------------
fail() { echo "run_phase4: $*" >&2; exit 1; }

[[ -x "$BUILD_DIR/db_bench" ]] || fail "no db_bench at $BUILD_DIR; run scripts/build_rocksdb_plugin.sh first"

PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 99)
if [[ "$PARANOID" -gt 2 ]]; then
    echo "run_phase4: perf_event_paranoid is $PARANOID; counters will be skipped." >&2
    echo "run_phase4: sudo sysctl -w kernel.perf_event_paranoid=1 to collect them." >&2
fi

LOADAVG=$(cut -d' ' -f1 /proc/loadavg)
if (( $(echo "$LOADAVG > 1.0" | bc -l) )); then
    echo "run_phase4: load average is $LOADAVG before starting." >&2
    echo "run_phase4: a benchmark sharing the box with a build is not a measurement." >&2
    [[ -n "${ALLOW_LOADED_HOST:-}" ]] || fail "refusing to measure a busy host (set ALLOW_LOADED_HOST=1 to override)"
fi

# --------------------------------------------------------------------------
# Environment record
# --------------------------------------------------------------------------
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
    echo "governor:  $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo 'unavailable')"
    echo "turbo:     $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null | sed 's/^0$/on/;s/^1$/off/' || echo 'unavailable')"
    echo "paranoid:  $PARANOID"
    echo "gcc:       $(${CXX:-g++} --version 2>/dev/null | head -1 || echo unknown)"
    echo "rocksdb:   $(git -C "$ROCKSDB_SRC" describe --tags 2>/dev/null || echo unknown)"
    echo "aparajita: ${APARAJITA_REV:-$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout')}"
    echo "total_ops: $TOTAL_OPS"
    echo "keyspace:  $KEYSPACE"
    echo "wbuf:      $WRITE_BUFFER"
} | tee "$OUT_DIR/environment.txt"

# --------------------------------------------------------------------------
# db_bench invocation
# --------------------------------------------------------------------------
# One process per (rep, threads, benchmark-chain) so fillrandom leaves the
# memtable populated for the reads that follow it. Splitting them across processes
# would reopen the DB and read from SSTs instead, which measures the block cache.
run_db_bench() {
    local rep=$1 threads=$2 benchmarks=$3 tag=$4 wal=$5
    local writes=$(( TOTAL_OPS / threads ))
    local reads=$READS_FULL
    local extra=()

    if [[ "$rep" == vector ]]; then
        reads=$READS_VECTOR
        # VectorRep returns false from IsInsertConcurrentlySupported(), and
        # RocksDB refuses to open with concurrent writes enabled against a rep
        # that does not advertise them. This is not a handicap applied to make a
        # point; it is the reason VectorRep loses on write throughput, and it is
        # what the comparison is for.
        extra+=(--allow_concurrent_memtable_write=false --enable_write_thread_adaptive_yield=false)
    fi
    # reads is per thread, as num is, so the read pass stays constant work too.
    local per_thread_reads=$(( reads / threads ))
    (( per_thread_reads < 1 )) && per_thread_reads=1

    local log="$OUT_DIR/raw/${tag}.${rep}.t${threads}.txt"
    rm -rf "$DB_PATH"

    echo "-- $tag  rep=$rep threads=$threads writes=$writes reads=$per_thread_reads wal=$wal"
    if ! timeout "$BENCH_TIMEOUT" "$BUILD_DIR/db_bench" \
        --benchmarks="$benchmarks" \
        --memtablerep="$rep" \
        --num="$KEYSPACE" \
        --writes="$writes" \
        --reads="$per_thread_reads" \
        --threads="$threads" \
        --disable_wal="$wal" \
        --write_buffer_size="$WRITE_BUFFER" \
        --max_write_buffer_number=4 \
        --disable_auto_compactions=1 \
        --compression_type=none \
        --seed=42 \
        --benchmark_write_rate_limit=0 \
        "${extra[@]}" \
        --db="$DB_PATH" > "$log" 2>&1
    then
        echo "TIMED OUT or FAILED after ${BENCH_TIMEOUT}s" >> "$log"
        echo "   !! $tag $rep t=$threads did not complete"
    fi

    # SST count is the residency check, and the threshold is one rather than zero.
    # Closing the DB flushes whatever is still in the memtable, so exactly one SST
    # is the shutdown flush and says nothing about the run. Two or more means the
    # write buffer filled mid-benchmark and part of the workload was served by the
    # LSM tree instead of by the rep under test, which invalidates that row.
    echo "sst_files: $(find "$DB_PATH" -name '*.sst' 2>/dev/null | wc -l)" >> "$log"
    grep -E 'micros/op' "$log" | sed "s/^/   /" || true
}

# --------------------------------------------------------------------------
# 1. Thread scaling, WAL off
# --------------------------------------------------------------------------
if [[ -z "${SKIP_TIMING:-}" ]]; then
echo
echo "== thread scaling, WAL off, $REPETITIONS repetitions =="
for rep in aparajita skip_list vector; do
    for threads in $THREAD_COUNTS; do
        for r in $(seq 1 "$REPETITIONS"); do
            run_db_bench "$rep" "$threads" "fillrandom,readrandom" "scale-r${r}" 1
        done
    done
done

# readwhilewriting is its own pass: --threads there is the reader count and one
# writer runs alongside, so it is a different shape from the two above and mixing
# it into the same chain would leave the memtable in a different state.
echo
echo "== readwhilewriting, WAL off, $REPETITIONS repetitions =="
for rep in aparajita skip_list vector; do
    for threads in $THREAD_COUNTS; do
        for r in $(seq 1 "$REPETITIONS"); do
            run_db_bench "$rep" "$threads" "fillrandom,readwhilewriting" "rww-r${r}" 1
        done
    done
done

# --------------------------------------------------------------------------
# 2. One configuration with the WAL on
# --------------------------------------------------------------------------
# ROADMAP.md asks for WAL off in at least one configuration, and the reason is
# that fsync cost otherwise dominates. That claim is worth demonstrating rather
# than asserting, so the same 16-thread point is run both ways.
echo
echo "== WAL on, 16 threads, for contrast =="
for rep in aparajita skip_list; do
    for r in $(seq 1 3); do
        run_db_bench "$rep" 16 "fillrandom" "wal-r${r}" 0
    done
done
fi  # SKIP_TIMING

# --------------------------------------------------------------------------
# 3. Hardware counters
# --------------------------------------------------------------------------
# Three counters per operation, per rep. Branch misses and L1 misses are the two
# the project's central claim is about. The third, cache line invalidation, has no
# portable perf event: the Intel approximation is the HITM counter, which counts
# loads that hit a line another core holds modified. The event name is resolved
# against this host's perf rather than hardcoded, and if the PMU does not expose
# it the run says so instead of substituting something else.
# The name matters and it is not stable across microarchitectures. XSNP_HITM was
# retired at Ice Lake and its counterpart is XSNP_FWD: a retired load whose L3 hit
# was satisfied by forwarding from another core's modified copy of the line, which
# is the event this project's central claim is about. Calling that "HITM" in a
# paper without saying which counter produced it is the mistake ROADMAP.md warns
# against, so the resolved name is recorded and reported rather than assumed.
# Captured once into a variable rather than piped into grep per candidate, and
# that is a bug fix rather than a tidy-up. `perf list | grep -q` makes grep exit
# at the first match, perf list then dies of SIGPIPE with status 141, and under
# `set -o pipefail` the pipeline reports failure -- so every event this host has
# was reported absent, and the first counter run silently collected no HITM data
# at all while printing that the PMU did not support it.
PERF_EVENTS=$(perf list 2>/dev/null || true)

HITM_EVENT=""
for candidate in mem_load_l3_hit_retired.xsnp_hitm \
                 mem_load_uops_l3_hit_retired.xsnp_hitm \
                 mem_load_l3_hit_retired.xsnp_fwd; do
    if grep -qw -- "$candidate" <<< "$PERF_EVENTS"; then
        HITM_EVENT=$candidate
        break
    fi
done
# XSNP_NO_FWD (a clean cross-core L3 hit) is collected beside it. On its own the
# forwarding count says how often a reader hit a line someone had dirtied; next to
# the clean count it says what fraction of cross-core sharing was dirty sharing,
# which is the number that distinguishes a layout problem from a write pattern.
XSNP_CLEAN=""
for candidate in mem_load_l3_hit_retired.xsnp_no_fwd mem_load_l3_hit_retired.xsnp_hit; do
    if grep -qw -- "$candidate" <<< "$PERF_EVENTS"; then
        XSNP_CLEAN=$candidate
        break
    fi
done

# Enumeration is not availability. On a virtualized PMU the xsnp events are
# listed, program without error, and report 100% enabled time -- and then count
# exactly zero against a workload that retires a hundred billion instructions,
# because they are PEBS-backed and the hypervisor does not expose PEBS. A zero
# that means "this counter does not work here" is indistinguishable in the output
# from a zero that means "no cross-core dirty sharing occurred", and the second
# is a headline claim. So the event is calibrated against a workload that must
# produce cross-core traffic, and demoted if it stays at zero.
if [[ -n "$HITM_EVENT" && "$PARANOID" -le 2 ]]; then
    rm -rf "$DB_PATH"
    calib=$(perf stat -e "$HITM_EVENT" -x, \
                "$BUILD_DIR/db_bench" --benchmarks=fillrandom --memtablerep=skip_list \
                    --num=200000 --writes=25000 --threads=8 --disable_wal=1 \
                    --compression_type=none --db="$DB_PATH" 2>&1 \
            | awk -F, '/'"$HITM_EVENT"'/ {print $1}' | head -1)
    rm -rf "$DB_PATH"
    if [[ "$calib" == "0" || -z "$calib" || "$calib" == "<not"* ]]; then
        echo "run_phase4: $HITM_EVENT programs but counts $calib on a workload that" >&2
        echo "run_phase4: must generate cross-core sharing. Treating it as unavailable." >&2
        HITM_UNAVAILABLE="programmed but counted $calib during calibration"
        HITM_EVENT=""
        XSNP_CLEAN=""
    else
        echo "run_phase4: $HITM_EVENT calibrated, counted $calib"
    fi
fi

{
    echo "perf_event_paranoid: $PARANOID"
    echo "hitm event resolved:  ${HITM_EVENT:-NONE AVAILABLE on this PMU}"
    echo "clean xsnp resolved:  ${XSNP_CLEAN:-NONE AVAILABLE on this PMU}"
    [[ -n "${HITM_UNAVAILABLE:-}" ]] && echo "hitm demoted:         $HITM_UNAVAILABLE"
    echo "perf list entries:    $(grep -c . <<< "$PERF_EVENTS")"
} | tee "$OUT_DIR/counter-availability.txt"

if [[ "$PARANOID" -le 2 ]]; then
    EVENTS="cycles,instructions,branches,branch-misses,cache-references,cache-misses"
    if perf stat -e L1-dcache-loads -x, true >/dev/null 2>&1; then
        EVENTS="$EVENTS,L1-dcache-loads,L1-dcache-load-misses"
    else
        echo "run_phase4: L1-dcache events unavailable on this PMU; omitting." >&2
    fi
    [[ -n "$HITM_EVENT" ]] && EVENTS="$EVENTS,$HITM_EVENT"
    [[ -n "$XSNP_CLEAN" ]] && EVENTS="$EVENTS,$XSNP_CLEAN"

    echo
    echo "== hardware counters, $COUNTER_THREADS threads =="
    # Every figure here is a *difference between two runs*, and that is not
    # fastidiousness. perf wraps a whole process, so the counters for a read
    # benchmark include the fill that populated the memtable, the DB open, and the
    # flush on close. Subtracting a separate fill-only run to remove that failed
    # outright on the first attempt: the fill is twenty times the size of the read
    # pass, so the subtraction was two large noisy numbers differenced to a small
    # one and it produced negative instruction counts.
    #
    # Two points on the same benchmark fix it. Both runs do an identical fill at
    # an identical seed, so everything that is not the extra operations cancels
    # exactly rather than approximately -- setup, teardown, and the fill itself.
    # What remains is the marginal cost of (hi - lo) operations, which is the
    # per-operation number this phase is actually claiming.
    counter_run() {
        local rep=$1 bench=$2 point=$3 writes=$4 reads=$5
        local extra=()
        [[ "$rep" == vector ]] && extra+=(--allow_concurrent_memtable_write=false
                                          --enable_write_thread_adaptive_yield=false)
        local chain="fillrandom"
        [[ "$bench" != fillrandom ]] && chain="fillrandom,$bench"
        rm -rf "$DB_PATH"
        echo "-- counters $rep $bench $point (writes=$writes reads=$reads)"
        timeout "$BENCH_TIMEOUT" perf stat -e "$EVENTS" -x, \
            -o "$OUT_DIR/raw/perf.${rep}.${bench}.${point}.csv" \
            "$BUILD_DIR/db_bench" \
                --benchmarks="$chain" --memtablerep="$rep" \
                --num="$KEYSPACE" --writes="$writes" --reads="$reads" \
                --threads="$COUNTER_THREADS" \
                --disable_wal=1 --write_buffer_size="$WRITE_BUFFER" \
                --max_write_buffer_number=4 --disable_auto_compactions=1 \
                --compression_type=none --seed=42 "${extra[@]}" \
                --db="$DB_PATH" \
            > "$OUT_DIR/raw/perfrun.${rep}.${bench}.${point}.txt" 2>&1 \
            || echo "   !! counters $rep $bench $point did not complete"
    }

    W_HI=$(( TOTAL_OPS / COUNTER_THREADS ))
    W_LO=$(( W_HI / 10 ))
    for rep in aparajita skip_list vector; do
        if [[ "$rep" == vector ]]; then
            r_hi=$(( COUNTER_READS_VECTOR / COUNTER_THREADS )); (( r_hi < 1 )) && r_hi=1
            r_lo=$(( r_hi / 10 )); (( r_lo < 1 )) && r_lo=1
            (( r_lo == r_hi )) && r_lo=0
        else
            r_hi=$(( COUNTER_READS / COUNTER_THREADS ))
            r_lo=$(( r_hi / 10 ))
        fi
        # fillrandom differences on the write count. The residual still contains
        # the shutdown flush of the extra entries, which is SST-writing rather
        # than memtable work; it is the same code path for every rep, so it
        # dilutes the contrast slightly and cannot invent one.
        counter_run "$rep" fillrandom hi "$W_HI" 1
        counter_run "$rep" fillrandom lo "$W_LO" 1
        for bench in readrandom readwhilewriting; do
            counter_run "$rep" "$bench" hi "$W_HI" "$r_hi"
            counter_run "$rep" "$bench" lo "$W_HI" "$r_lo"
        done
    done
    {
        echo "counter_threads: $COUNTER_THREADS"
        echo "write_points:    hi=$(( W_HI * COUNTER_THREADS )) lo=$(( W_LO * COUNTER_THREADS )) total ops"
        echo "read_points:     hi=$COUNTER_READS lo=$(( COUNTER_READS / 10 )) total ops"
        echo "read_points_vec: hi=$COUNTER_READS_VECTOR lo=$(( COUNTER_READS_VECTOR / 10 )) total ops"
    } | tee "$OUT_DIR/counter-points.txt"

else
    echo "run_phase4: skipping counters, perf_event_paranoid=$PARANOID" | tee "$OUT_DIR/counters-skipped.txt"
fi

# --------------------------------------------------------------------------
# 4. memtablerep_bench: the rep with nothing else in the way
# --------------------------------------------------------------------------
# db_bench measures the whole engine, so a rep difference arrives diluted by the
# write path, the version machinery and the block cache. memtablerep_bench is the
# rep alone, and it is where the Phase 4b readwrite regression was visible.
if [[ -x "$BUILD_DIR/memtablerep_bench" && -z "${SKIP_TIMING:-}" ]]; then
    echo
    echo "== memtablerep_bench =="
    # VectorRep is deliberately absent from the read benchmarks here. Capping the
    # read count is available in db_bench and is used there; memtablerep_bench has
    # no equivalent knob, so including it would spend the whole budget timing
    # std::sort. Its fillrandom is run, because appending to a vector is the one
    # thing VectorRep is actually good at and the comparison should show it.
    for rep in aparajita skiplist vector; do
        benches=fillrandom,readrandom,readwrite
        [[ "$rep" == vector ]] && benches=fillrandom
        for threads in $THREAD_COUNTS; do
            for r in $(seq 1 "$REPETITIONS"); do
                log="$OUT_DIR/raw/mtrep-r${r}.${rep}.t${threads}.txt"
                echo "-- memtablerep_bench $rep t=$threads r=$r ($benches)"
                timeout "$BENCH_TIMEOUT" "$BUILD_DIR/memtablerep_bench" \
                    --memtablerep="$rep" \
                    --num_operations=$(( TOTAL_OPS / 10 )) \
                    --num_threads="$threads" \
                    --benchmarks="$benches" \
                    > "$log" 2>&1 || echo "TIMED OUT" >> "$log"
            done
        done
    done
fi

rm -rf "$DB_PATH"

# --------------------------------------------------------------------------
# 5. Summary
# --------------------------------------------------------------------------
python3 "$REPO_ROOT/scripts/phase4_summarize.py" "$OUT_DIR" | tee "$OUT_DIR/summary.txt"

echo
echo "results written to $OUT_DIR/"
