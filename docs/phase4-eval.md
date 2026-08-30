# Phase 4: the empirical evaluation

Numbers in [results/phase4-evaluation.txt](../results/phase4-evaluation.txt),
reproduce with `./scripts/build_rocksdb_plugin.sh && ./scripts/run_phase4.sh`.

Phases 4a and 4b fixed the two structural problems Phase 3 exposed and each was
measured against the thing it changed. This phase asks the question the paper has
to answer instead: across the workloads and thread counts RocksDB users care
about, where does Aparajita win, where does it lose, and what do the hardware
counters say about why.

The short version is that the read win is large, uniform and counter-backed; the
write win is real but is invisible in multi-threaded `db_bench` for a reason that
has nothing to do with the memtable; and one of the three exit-criterion counters
could not be collected on rented hardware at all.

## The setup, and the one thing it cannot do

A `c4-standard-24` in `us-central1-b`: Xeon Platinum 8581C (Emerald Rapids), 12
physical cores and 24 logical, SMT on, 88 GiB, Ubuntu 24.04, GCC 14.2, RocksDB
v9.11.2 built Release with `USE_RTTI=ON`. Same part as Phases 4a and 4b, so the
figures are comparable across phases.

Quota capped the shape at 24 vCPUs. The 1- and 4-thread points sit below physical
core count, 16 sits between physical and logical, and 64 is 2.7x oversubscribed.
The top of every curve is therefore contention, not headroom, and is labelled as
such throughout. It is not a useless point — RocksDB routinely runs more writers
than cores, which is why the per-node backoff yields instead of spinning flat —
but it cannot be read as scaling.

## The measurement bug that had to be fixed first

The first run of this phase produced a read scaling curve that looked
substantially better than the one reported here, and it was wrong.

`db_bench`'s `--num` is per thread, and it sets two different things at once: how
much work each thread does, and the range its random keys are drawn from. The
obvious way to hold total work constant across thread counts is
`--num=$((TOTAL/threads))`, and that quietly shrinks the keyspace at every step.
At 1 thread the memtable ended up holding 1.26 million distinct keys; at 16
threads it held 125,000, each written sixteen times over. The measured read hit
rate drifted from 63% to 100% across the curve as a direct consequence.

That is a working-set sweep wearing a thread-scaling curve's clothes. Two
independent variables moved together and the smaller working set at high thread
counts flattered the reads. `--num` is now pinned at 2,000,000 and `--writes` and
`--reads` carry the per-thread work, which holds the hit rate at 63% everywhere.

The lesson generalises past this flag: any benchmark knob that sets both the work
and the data has to be split before a curve over it means anything.

## Reads: the win, and the counters behind it

Aparajita is ahead at every thread count on both read benchmarks — 22% to 39% on
`readrandom`, 23% to 38% on `readwhilewriting` — and the samples do not overlap.
Across ten runs per cell, Aparajita's slowest run beats the skiplist's fastest at
every point but one.

The counters say why, as marginal per-operation costs:

| `readrandom`, 16 threads | Aparajita | skip_list |
| --- | --- | --- |
| instructions/op | 3,271.5 | 7,349.9 |
| branch misses/op | 8.46 | 10.39 |
| L1 misses/op | 62.18 | 103.99 |

Less than half the instructions and 40% fewer L1 misses per lookup. That is Phase
4a's descent — a tower hop comparing two integers in a line already loaded instead
of making a virtual comparator call — plus Phase 4b's arena locality, showing up
exactly where they were supposed to.

**One inversion is worth stating rather than burying.** Aparajita's branch miss
*rate* on reads is worse than the skiplist's, 1.24% against 0.73%, while its
branch misses *per operation* are better, 8.46 against 10.39. Both are true. The
design executes less than half as many branches per lookup, so the same absolute
count of mispredictions is a larger share of a much smaller denominator. Per-op
cost is what the workload actually pays, but a paper that quoted the rate alone
would be quoting the single framing in which this design looks worse than the
thing it replaces.

## Writes: the win is real and `db_bench` cannot see it

`fillrandom` is +18.8% at 1 thread, +2.6% at 4, and a tie at 16 and 64. The
4-thread figure reproduces Phase 4b's +2.0% on a different host, which is
reassuring about both measurements.

The scaling column is the tell. Neither rep scales: Aparajita goes 1.00x, 2.05x,
0.75x across 4, 16 and 64 threads, and the skiplist goes 1.16x, 2.52x, 0.88x. Two
structures with entirely different insert paths do not accidentally share a
scaling curve.

The counters name the culprit. A `fillrandom` operation at 16 threads retires
22,182 instructions in Aparajita and 24,177 in the skiplist. A memtable insert is
not twenty-two thousand instructions — an append into a node under a spinlock is a
few hundred. The rest is RocksDB's write-thread group: writers spinning in
`enable_write_thread_adaptive_yield` while the group leader commits the batch,
charged identically to every rep. At 16 threads on 12 cores, `db_bench fillrandom`
measures RocksDB's write path with the memtable as a rounding error.

`memtablerep_bench` has no write group, and the difference reappears intact:
0.5435 µs/op against the skiplist's 0.8040, the skiplist 48% slower, holding at
48–52% across all four columns.

So the honest claim about writes is: **the rep inserts roughly half again as fast
as the skiplist; that is visible undiluted at one thread and in
`memtablerep_bench` at every thread count; and it is swamped by RocksDB's
write-group machinery in multi-threaded `db_bench`.** Claiming a multi-threaded
`db_bench` write win from this data would be claiming something the data does not
support.

A caveat on `memtablerep_bench`'s own numbers: RocksDB hardcodes its `fillrandom`
to a single thread regardless of `--num_threads`, so those four columns are four
samples of one single-threaded measurement rather than a curve. They agree to
within 3%, which is the useful thing they say.

## VectorRep behaves as the design predicts, not as its layout suggests

VectorRep has the friendliest memory layout of the three and loses anyway, which
is the comparison the roadmap wanted.

It is by far the fastest single-threaded fill — 2,222.9 kops/s against Aparajita's
678.9, because appending to a vector is the cheapest insert available — and it is
the only rep whose fill throughput *falls* as threads are added (0.54x at 4, 0.41x
at 16), because `IsInsertConcurrentlySupported()` returns false and every write
serialises.

Its reads are not slow but disqualifying: 0.9 to 3.3 seconds per lookup, 6.4
billion retired instructions per lookup. `VectorRep::Get` builds an iterator, and
on a *mutable* memtable that iterator copies the entire bucket and sorts the copy,
because a sorted order cannot be cached on a structure still being appended to.
Its read benchmarks are capped at 20 operations for this reason and reported as
capped; uncapped they would have run for days.

## The WAL control

The roadmap asks for `--disable_wal=1` in at least one configuration because fsync
cost otherwise hides the memtable difference. That is worth demonstrating rather
than asserting, so the 16-thread point was run both ways. WAL off: 1394.8 and
1437.7 kops/s. WAL on: 1043.0 and 1048.1. The WAL costs about 27% and closes the
already-small gap to 0.5%.

## HITM could not be measured, and the failure mode is silent

Cross-core loads that hit a line another core holds modified are the counter this
project's central claim is *about*, and they are not in the tables.

GCP exposes three vPMU tiers. The API accepted `standard` for this shape and
rejected `enhanced` — the tier whose documentation covers LLC events — on both v1
and beta. Under `standard`, `mem_load_l3_hit_retired.xsnp_fwd` enumerates in `perf
list`, programs without error, and reports 100% enabled time, and then counts
**exactly zero** against a run retiring 108 billion instructions across 16 threads.
`cache-references` and `cache-misses` come back `<not supported>` outright. These
are PEBS-backed events, the hypervisor does not implement PEBS, and so the counter
reads zero rather than refusing.

That is more dangerous than an unsupported event. "0.000 HITM per operation" is a
publishable-looking number and it would have been false. `run_phase4.sh` now
calibrates the event against a workload that must produce cross-core sharing and
demotes it to unavailable if it stays at zero, and `phase4_summarize.py` prints
`n/a` rather than `0` when a counter read zero at both differencing points.

Two smaller traps on the way there. The event is not called `XSNP_HITM` on this
part — Intel retired that name at Ice Lake and the counterpart is `XSNP_FWD` — so
resolving the name against the host's own `perf list` rather than hardcoding it is
the difference between measuring the right thing and measuring nothing. And the
first attempt to resolve it failed for an unrelated reason: `perf list | grep -q`
makes `grep` exit at the first match, `perf list` then dies of SIGPIPE with status
141, and under `set -o pipefail` the pipeline reports failure. Every event the host
had was reported absent, and the run printed "NONE AVAILABLE on this PMU" while the
events sat there.

HITM stays open and needs bare metal or a provider that exposes PEBS to guests.

## Why every counter figure is a difference between two runs

`perf` wraps a whole process, so a read benchmark's counters include the fill that
populated the memtable, the DB open, and the flush on close.

The obvious correction — subtract a separate fill-only run — failed outright. The
fill is twenty times the size of the read pass, so the subtraction was two large
noisy numbers differenced to a small one, and it produced *negative* instruction
counts for Aparajita's reads.

Two points on the same benchmark fix it. Both runs do an identical fill at an
identical seed and differ only in operation count, so everything that is not the
extra operations cancels exactly rather than approximately. Reads difference
20,000,000 against 2,000,000; writes difference 2,000,000 against 200,000. The
residual on the write row still contains the shutdown flush of the extra entries,
which is SST-writing rather than memtable work; it is the same code path for every
rep, so it dilutes the contrast slightly and cannot invent one.

## The ThreadSanitizer claim was wrong and is corrected

`CLAUDE.md` and `results/phase4-ordered-kernels.txt` stated that after the
start-barrier fix the 64-thread instrumented case "finishes in 6.9 ms of race
time" and that "the whole instrumented run takes about 90 ms". That is not
representative. Five runs of the same binary on this host:

| run | total |
| --- | --- |
| under `ctest` | 186 s |
| direct, run 1 | 524 s |
| direct, run 2 (idle box) | 9.4 s |
| direct, run 3 (idle box) | 298 s |
| direct, run 4 (idle box) | 373 s |

Per phase, from the run that printed them:

```
 1 threads x 640 keys  [spawn  1.1 ms, race      3.3 ms, verify 1.7 ms]
 4 threads x 160 keys  [spawn  1.4 ms, race      2.9 ms, verify 2.1 ms]
16 threads x  40 keys  [spawn  3.0 ms, race    526.2 ms, verify 2.1 ms]
64 threads x  10 keys  [spawn 11.7 ms, race 523,880.3 ms, verify 2.2 ms]
```

The barrier fix itself is not in question: `spawn` is 11.7 ms where it used to
dominate, and the case now terminates every time, which it did not before. What
was wrong was concluding the barrier was the *whole* cause. The distribution is
heavy-tailed — 9 s to 524 s for identical work on an idle machine — and 6.9 ms was
a draw from its fast tail reported as the outcome.

What remains is the per-node spinlock's backoff under instrumentation. 640 total
inserts take under three milliseconds of race at 4 threads and up to nine minutes
at 64, and the work is identical; what grows is the number of threads spinning in
an instrumented `test_and_set` loop, where TSan's per-thread vector clocks make
each of those atomics cost O(threads). Sixteen times the threads for a hundred
thousand times the race time is that shape.

The exit criterion is unaffected — TSan reports no races at 1, 4, 16 and 64 on
every run here. What changes is what the project may claim about the cost, and it
is a live argument for the CAS-on-the-order-word that Phase 4b deferred: that
would remove the spin loop the instrumentation is choking on.

Three standing explanations in this project have now failed on contact with
measurement — eight tower hops that were thirty, lock contention that was a
harness livelock, and a permutation that was never needed. This is the fourth, and
it is one of mine from earlier the same day. The pattern is consistent enough to
be a working rule: an explanation in this repository that has not been measured
since it was written is a hypothesis, whoever wrote it.

## What this phase leaves open

- **HITM**, above. Needs hardware this project has not rented yet.
- **A thread-scaling curve with headroom.** Every point above 12 threads here is
  contention. The shape of the curve on a machine with 64 real cores is unknown.
- **The write-group ceiling.** Since multi-threaded `db_bench fillrandom` measures
  RocksDB's write path rather than the rep, a memtable win has to be argued from
  `memtablerep_bench` and from the single-threaded point. Whether that is
  persuasive to a RocksDB maintainer is a question for the upstream pitch, and it
  is a better question to have found now than in review.
