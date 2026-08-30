# Aparajita roadmap

Aparajita is a hardware-optimized C++20 in-memory buffer (MemTable) plugin for RocksDB and
LevelDB-style LSM-tree engines. It replaces RocksDB's default pointer-heavy concurrent skiplist,
whose random pointer-chasing across heap allocations stalls modern multi-core CPUs on L1/L2 misses
and mispredicts branches on every key comparison.

Three ideas carry the design. Nodes align to 64-byte cache lines so no node straddles a line split.
Search evaluates several keys per instruction through vector comparison masks
(`_mm256_cmpeq_epi32` then a movemask) rather than a conditional jump per comparison. Concurrent
access uses C++20 acquire/release atomics instead of locks.

## Scope change from the previous version of this file

The earlier roadmap was derived from `High Performance C++ System Architecture.md` before the
project scope was known. Most of that document no longer applies. A `MemTableRep` runs inside
RocksDB's own thread and I/O model, so its thread-per-core execution model, its io_uring and SQPOLL
work, and its write-ahead-log phase are all RocksDB's concern rather than this plugin's. What
survives from it is the microarchitectural material: cache line alignment and false sharing,
Struct of Arrays layout, static dispatch over virtual calls, and the `perf` methodology.

## Schedule

Weeks are as specified. Dates assume a start on Monday 2026-08-31; shift them together if that
moves.

| Phase | Weeks | Dates | Focus |
| --- | --- | --- | --- |
| 1 | 1-2 | 2026-08-31 to 2026-09-13 | Benchmark harness and SIMD search primitives |
| 2 | 3-4 | 2026-09-14 to 2026-09-27 | Node layout, search kernels, lock-free insert |
| 3 | 5-6 | 2026-09-28 to 2026-10-11 | RocksDB plugin integration |
| 4 | 7-8 | 2026-10-12 to 2026-10-25 | db_bench evaluation |
| 5 | 9+ | 2026-10-26 onward | Paper, arXiv preprint, upstream pitch |

## Where the repo stands

*This section described the tree before Phase 1 and is kept for the record; `CLAUDE.md` has the
current state.* The tree held the architecture document, this roadmap, a `CLAUDE.md`, a README, both
license texts, and a CMake/CLion `.gitignore`. No source, no build system, no tests.

Licensing is settled: dual Apache 2.0 or MIT at the user's option, following the convention of
paired `LICENSE-APACHE` and `LICENSE-MIT` files with the election stated in the README. Source
files should carry `// SPDX-License-Identifier: Apache-2.0 OR MIT`, and the CMake package metadata
should declare the same once Phase 1 creates it.

One setup item remains for week 1 alongside Phase 1. RocksDB needs to be a pinned dependency from
the start rather than at Phase 3, because its `MemTableRep` header is what Phase 2's node layout
has to satisfy.

## Phase 1: environment and micro-benchmarking primitives

Weeks 1-2. Complete, including the AVX-512 runtime check. Results and analysis in
[docs/phase1.md](docs/phase1.md); reproduce with `./scripts/run_phase1.sh`.

Goal: prove the SIMD search wins on a standalone kernel before any of it touches RocksDB.

Stand up CMake with C++20, Google Benchmark, and the `perf` invocations that will be used for the
rest of the project. Write a standalone micro-benchmark over a 64-byte node: scalar linear search,
scalar binary search, AVX2 mask search, and an AVX-512 variant behind a runtime CPUID check. Feed it
the branch-hostile case, which is randomly ordered lookup keys with a mix of hits and misses, since
a predictable access pattern will hide exactly the mispredictions this project claims to remove.

Two measurement notes. The scalar baseline has to be a fair one, compiled at the same optimization
level and not accidentally auto-vectorized, or the comparison proves nothing. And `-march=native`
builds will not be what ships, so record results for both native and a portable baseline such as
`-mavx2`.

One implementation detail to get right early: `_mm256_movemask_epi8` on the result of
`_mm256_cmpeq_epi32` yields 32 bits, four per lane, so a trailing-zero count must be divided by 4.
`_mm256_movemask_ps` on a cast of the same vector gives 8 bits directly and is usually the cleaner
choice.

| Exit criterion | Target | Result |
| --- | --- | --- |
| Branch misses on the SIMD probe | below 0.5% of branches | met: 0.00% for both avx2 and avx512 |
| Cycles per lookup, SIMD versus scalar | measured ratio with dispersion | met: 5.5x (avx2) and 6.9x (avx512) at a 0.5 hit ratio, 5.01 and 4.04 versus 27.65 cycles/probe |
| AVX-512 path | correct under CPUID gating, downclocking recorded | met: executes and passes the kernel tests; no downclock observed, freq/nom 1.741 in all five runs |

The AVX-512 criterion was closed on a rented Xeon Platinum 8581C (Emerald Rapids) rather than on
the development laptop, which is an i5-11300H. Two gaps remain deliberately open. The null
downclock result is from the generation where Intel had already removed most of the penalty, so it
does not speak for Skylake-SP or Cascade Lake, and it was taken on a shared cloud VM where the
guest cannot pin the governor. Confirming the negative on a downclock-prone part on bare metal is
the remaining work, and it belongs with the Phase 4 hardware rather than here.

One result changed the design rather than just measuring it. Returning -1 for "not found" costs
about 0.5 branch mispredictions per probe, because the ternary that produces it is data-dependent
and unpredictable at a realistic hit ratio. Folding the miss into the compare mask as bit 16 and
returning `kNodeKeys` instead took the AVX2 kernel from 25.4 to 6.2 cycles per probe. Any hot-path
API in Phase 2 that reports absence through a sentinel should use the same convention.

## Phase 2: node layout and concurrency

Weeks 3-4. Goal: a working standalone data structure, still outside RocksDB.

Design the `alignas(64)` node so the key material a probe compares sits contiguously in the line
and the pointers or offsets sit apart from it. Implement the fixed-width search kernel first, then
the prefix comparison path for variable-length keys. Then lock-free insertion over
`std::atomic` with acquire/release ordering, with the failure and retry paths tested under
ThreadSanitizer rather than by inspection.

The hard problem in this phase is that RocksDB keys are not fixed-width integers. A `MemTableRep`
receives a length-prefixed internal key, which is a variable-length user key followed by a packed
sequence number and value type. A 32-bit lane compare therefore operates on a surrogate, either a
hash of the key or its first four bytes, and every SIMD hit is a candidate that needs a full
comparison to confirm. Two consequences to settle here rather than discover in Phase 3. The
surrogate's collision rate sets how often the fast path falls back, so measure it on realistic key
distributions and not on sequential integers. And whether the structure keeps keys in comparator
order or sorts them at flush time is a fork in the design: RocksDB iterators must yield keys in
comparator order, and `VectorRep` gets away with an unordered vector only by sorting when the
memtable becomes immutable.

Memory has a constraint too. RocksDB hands the rep an `Allocator*` backed by its arena, and
allocations through it are what charge against `write_buffer_size`. Arena memory is bump-allocated
and cannot be reallocated, so a growable array has to be segmented into chunks rather than resized.

Results and analysis in [docs/phase2.md](docs/phase2.md), decisions in
[docs/phase2-design.md](docs/phase2-design.md).

| Exit criterion | Target | Result |
| --- | --- | --- |
| ThreadSanitizer over concurrent insert and read | clean, at 1, 4, 16, and 64 threads | met: clean at all four, on every run. The 64-thread case did not complete for two phases; the start barrier was one real cause and fixing it made the case terminate, but it was not the whole cause and the cost is heavy-tailed — 9 s to 524 s for identical work on one idle host. `docs/phase4-eval.md` has the corrected account |
| Surrogate collision rate | measured on at least one realistic key distribution, not synthetic sequential keys | met, on seven; the result invalidated the leading-bytes surrogate and forced prefix truncation |
| Ordering decision | ordered structure or sort-on-flush, decided and written down | met: ordered |
| Node layout | verified 64-byte aligned and 64 bytes in size by static_assert | met |

Two findings carry into Phase 3. Copy-on-write costs 448 bytes per key against the
skiplist's rough 50, which no probe-side speed compensates for and which Phase 3
must fix before any `db_bench` comparison is meaningful. And the surrogate is only
useful after the node's shared prefix is stripped, which is now a property the
RocksDB integration has to preserve when internal keys replace plain user keys.

## Phase 3: RocksDB plugin integration

Weeks 5-6. Goal: drop-in replacement through `options.memtable_factory`.

Implement `AparajitaMemTableRep` deriving from `rocksdb::MemTableRep` and `AparajitaMemTableFactory`
from `rocksdb::MemTableRepFactory`. The interface surface is larger than `Insert` and `Contains`
alone: `Allocate`, `InsertKey` and its `InsertKeyConcurrently` variants, `Get` for point lookups,
`GetIterator` for the ordered scan path, `ApproximateMemoryUsage`, and `MarkReadOnly` when the
memtable becomes immutable.

`InsertKeyConcurrently` matters more than the rest. RocksDB only routes concurrent writes to a rep
that returns true from `IsInsertConcurrentlySupported()`, and if Aparajita does not implement it,
every multi-threaded write benchmark in Phase 4 serializes and the headline claim collapses. The
default skiplist supports it; `VectorRep` does not, which is part of why `VectorRep` loses on write
throughput despite its friendlier memory layout.

Build the plugin against RocksDB's `plugin/` mechanism so `db_bench` can select it by name, rather
than patching RocksDB sources. Correctness work in this phase is the RocksDB memtable test suite
run against the new rep, not custom tests alone.

Results and analysis in [docs/phase3.md](docs/phase3.md); reproduce with
`./scripts/build_rocksdb_plugin.sh`. RocksDB is pinned at v9.11.2 in that script.

| Exit criterion | Target | Result |
| --- | --- | --- |
| RocksDB memtable test suite | passes against Aparajita | partial: RocksDB's memtable gtests and `db_stress` cannot select a plugin rep without patching RocksDB, so a differential gtest against `skip_list` and `memtablerep_bench` were run instead |
| `IsInsertConcurrentlySupported()` | returns true, with the concurrent path exercised | met |
| Iterator ordering | matches the skiplist rep's output on the same key set, byte for byte | met: forward, backward, every Get, Seek and SeekForPrev agree, under concurrent writes, across a flush, and under a reverse comparator |
| Selection | `db_bench --memtablerep=aparajita` runs without a RocksDB source patch | met |

The phase closes correct and slower. `fillrandom` is 17% behind the skiplist and
`readrandom` 33% behind, consistently and well outside run-to-run spread. The
cause is structural: a lookup spends about eight tower hops reaching the right
node, each one a virtual comparator call on a full internal key, and the SIMD
kernel only replaces the handful of comparisons inside the final sixteen-key
node. Inserts additionally rebuild a whole node under copy-on-write where the
skiplist links one and returns.

That reorders Phase 4. Running the full evaluation against a rep that loses by
17-33% would only document the loss, so the structural work named at the end of
docs/phase3.md comes first: make the descent cheap rather than only the leaf,
and stop copying a node per insert.

## Phase 4a: making the descent cheap

Goal: stop a lookup paying thirty virtual comparator calls to reach a node the SIMD kernel then
searches in four.

The premise Phase 3 left behind needed measuring rather than estimating, and the measurement moved
it: a lookup over 500,000 keys examines thirty successors, not the eight `docs/phase3.md` guessed,
and each one was a chain of three dependent random loads ending in a virtual call. Each node now
caches the first eight bytes of its first key in its header, so a hop compares two integers in a
cache line it has already loaded, and only a tie falls through to the comparator.

The interesting constraint is that the hint reproduces bytewise order and the comparator is the
authority, so the fast path has to be switched off under any other comparator. Establishing that
from inside a `MemTableRep` needs RTTI, which RocksDB's Release build disables by default. Probing
the comparator and generalising was rejected: a wrong hop is not a slow lookup, it is a lost key.

Findings in [docs/phase4-descent.md](docs/phase4-descent.md), numbers in
[results/phase4-descent.txt](results/phase4-descent.txt).

| Exit criterion | Target | Result |
| --- | --- | --- |
| Descent cost | measured, not estimated, before and after | met: 30.0 successors examined per lookup at 500k keys, at 11.3/16 average fill |
| Correctness under a non-bytewise comparator | the fast path off, and proven off | met: `HintOrderingFollowsTheUserComparator`, plus the reverse-comparator differential test |
| Fallback coverage | a test that fails if ties stop falling back to the comparator | met: the shared-prefix workload in `tests/test_memtable.cpp`, verified by deleting the fallback |
| Read throughput | ahead of the skiplist on a memtable-resident workload | met |
| Write throughput | ahead of the skiplist | not met: still behind, and the cause is Phase 4b's |

## Phase 4b: stop copying a node per insert

Goal: close the write gap, which was the whole gap.

Every insert rebuilt an entire node: a fresh payload, sixteen surrogates recomputed, and the shared
prefix re-derived from the first and last key. The skiplist links one node and returns.
Copy-on-write bought the atomic split cheaply in Phase 2 and was charging for it on every write, at
445 bytes of arena per inserted key against a live structure of 40.

A node is now append-only. A slot is written once, before the order word that names it, and never
again; the sorted order over the slots is fifteen four-bit indices in one 64-bit word, and
republishing that word is the whole of an insert. The word is self-describing -- the count is the
position of its highest nonzero nibble -- which is what keeps the publication a single store, and
the slot the `slot + 1` encoding gives up becomes the padding lane the ordered kernels need. Slot
order is not sorted order, and the ordered kernel turned out not to care: `lower_bound` counts the
live keys below the target, and a count does not depend on the arrangement. The permutation Phase 4b
introduced was removed once the counters made its cost visible.

Findings in [docs/phase4b-append.md](docs/phase4b-append.md), numbers in
[results/phase4b-append.txt](results/phase4b-append.txt).

| Exit criterion | Target | Result |
| --- | --- | --- |
| Arena per insert | an insert that does not allocate | met: allocation only at splits; 445.4 -> 100.6 B/key standalone, and 2.1x more keys resident per memtable under RocksDB |
| Write throughput | ahead of the skiplist | met: `fillrandom` 7.035 -> 5.531 us/op, from 24.5% behind to 2.0% ahead |
| Read throughput | no worse | met: `readrandom` 1.440 -> 1.251 us/op, 26.6% ahead, from the locality the smaller arena buys |
| Publication atomicity | a reader never sees a torn node | met by construction: one release store, and nothing it names is ever revised |
| Ordered kernels | checked against a reference, not inferred from the structure | met: `lower_bound_perm_*` registered in `tests/test_search.cpp`; four kernel mutations that pass the differential suite fail there |
| Cost of the permutation | measured against the sorted kernel, not argued | met, after the fact, and the answer retired the permutation: 2.0x on AVX-512 and 4.1x on AVX2 bought nothing, because a popcount is blind to order. Removed, and re-measured at 21.03 -> 9.89 cycles per probe on AVX2 and 8.12 -> 6.04 on AVX-512 (`results/phase4-ordered-kernels.txt`) |
| Invariants no query can observe | a test that fails when they break | met: `BasicMemTable::check_invariants`, called from both structure tests |
| Concurrent read/write | not regressed | not met: `readwrite` is 14.3% slower than copy-on-write, the cost of a writer dirtying a line readers hold. Still 33.5% ahead of the skiplist; quantifying it wants HITM counters and belongs to Phase 4 |

One thing this phase did not know it was buying, and it was buying nothing. The counter harness
covered the equality kernels only, so the price of permuting the lanes went unmeasured until the
Phase 4 prerequisites were closed: 4.04 -> 8.12 cycles per probe on AVX-512, and 5.10 -> 21.03 on
AVX2, where the missing 16-lane crossing permute costs four permutes and two blends. Asking what
that bought produced the answer that it bought nothing — `lower_bound` returns a count of the live
keys below the target, and a count is blind to how they are arranged, so the permuted vector and the
unpermuted one have the same popcount. The kernels no longer permute, and the re-measure gives 9.89
cycles per probe on AVX2 against 21.03, and 6.04 on AVX-512 against 8.12. What remains over the
sorted-node kernels — 1.94x and 1.50x — is the live-lane mask that keeps a reader off the slot a
writer is filling, which is not removable. `docs/phase4b-append.md` keeps the original argument with
the correction beside it. The db_bench win was never at stake either way: it came from arena
locality rather than from the probe.

Two things this phase deliberately did not do. The per-node spinlock still guards an insert, even
though it now covers three stores rather than an allocation and a merge, because changing the
publication protocol and the locking discipline in one step would leave neither measured. And node
capacity is fifteen rather than sixteen: recovering the slot is possible but costs more in
explanation than it is worth, and the reasoning is in the design document.

## Phase 4: empirical evaluation

Weeks 7-8. Goal: the numbers the paper reports.

Run `db_bench` against Aparajita, the default skiplist, and `VectorRep` at 1, 4, 16, and 64 write
threads. `fillrandom` and `readwhilewriting` are the two that matter most; `readrandom` over a
populated memtable isolates the probe path. Run with `--disable_wal=1` in at least one
configuration, because WAL fsync cost otherwise dominates and hides the memtable difference
entirely.

Collect throughput and p99 from `db_bench` itself, and the microarchitectural counters from `perf`.
One caveat on the third metric: cache line invalidation has no portable `perf` event. The usual
approximation on Intel is the HITM counter, `mem_load_l3_hit_retired.xsnp_hitm`, which counts loads
that hit a modified line in another core's cache. Confirm the event name against `perf list` on the
test host, and say in the paper which counter was used rather than reporting an abstract
"invalidation rate".

Report the environment: CPU model, core and socket count, whether SMT and turbo were on, kernel
version, and RocksDB commit. Pin `db_bench` threads and disable frequency scaling for the runs that
go in the paper.

Results and analysis in [docs/phase4-eval.md](docs/phase4-eval.md), numbers in
[results/phase4-evaluation.txt](results/phase4-evaluation.txt); reproduce with
`./scripts/build_rocksdb_plugin.sh && ./scripts/run_phase4.sh`.

| Exit criterion | Target | Result |
| --- | --- | --- |
| Thread scaling curve | Aparajita versus skiplist versus VectorRep at 1, 4, 16, 64 threads | met, with a stated limit: quota capped the host at 12 physical cores, so 16 and 64 threads are contention rather than headroom |
| Regression check | no configuration where Aparajita loses to the skiplist without a stated reason | met: no loss outside run-to-run spread. `fillrandom` at 16 and 64 threads is a tie, and the reason is that both reps are bounded by RocksDB's write-thread group there |
| Counter data | branch misses, L1 misses, and HITM per operation for each rep | partial: branch misses and L1 misses collected for all three reps. HITM was not collectable — GCP's API rejects the `enhanced` vPMU tier, and under `standard` the event programs and silently counts zero because the hypervisor does not implement PEBS |
| Reproducibility | a script that reruns the full suite from a clean checkout | met: `scripts/run_phase4.sh` and `scripts/phase4_summarize.py` |

The read result is the headline and it is uniform: ahead of the skiplist at every
thread count on both read benchmarks, 22-39% on `readrandom` and 23-38% on
`readwhilewriting`, with non-overlapping samples across ten runs per cell. The
counters back it at 55% fewer retired instructions and 40% fewer L1 misses per
lookup.

The write result needed restating rather than reporting. `fillrandom` is +18.8% at
one thread and a tie at 16 and 64 — but *neither* rep scales there, and a
`fillrandom` operation at 16 threads retires 22,182 instructions, which no memtable
insert costs. Multi-threaded `db_bench fillrandom` on this core count is measuring
RocksDB's write-thread group with the memtable as a rounding error.
`memtablerep_bench`, which has no write group, puts the skiplist 48% behind on
insert and holds that across all four thread counts. The claim the project can
support is therefore "the rep inserts about half again as fast, visible at one
thread and in `memtablerep_bench`, and swamped by RocksDB's write path in
multi-threaded `db_bench`" — not a multi-threaded `db_bench` write win.

Two things this phase corrected rather than measured. The first run divided
`--num` by the thread count to hold total work constant, which also divides the
keyspace, so the memtable held 1.26M distinct keys at 1 thread and 125k at 16 and
the read curve was partly a working-set sweep. And the ThreadSanitizer cost
recorded at the end of the previous phase was a draw from a heavy tail, not a
result; see below.

## Phase 5: paper and upstream pitch

Week 9 onward. Goal: preprint out, maintainers engaged.

Write the paper in IEEEtran, titled "Aparajita: Eliminating CPU Cache Line Invalidations and Branch
Mispredictions in LSM-Tree MemTables via SIMD Alignment", and submit to arXiv under cs.DB with
cs.PF as a cross-list. Related work has to cover the existing RocksDB reps (`VectorRep`,
`HashSkipListRep`, `HashLinkListRep`) and the SIMD-search literature the design draws on, since a
reviewer familiar with RocksDB will ask why this is not `VectorRep` with vector instructions.

For the upstream pitch, a GitHub Discussion or RFC on `facebook/rocksdb` with the benchmark data
inline is more likely to land than a cold pull request. Maintainers will ask about the cases where
the design loses, so name them: the surrogate collision rate on adversarial keys, the flush-time
sort cost if the structure ends up unordered, and behavior on non-AVX2 hardware.

## Open technical questions

These have no answer yet and each one changes code.

Ordered structure or unordered with a sort at flush time. This is the largest fork in the design
and Phase 2 must close it.

What the 32-bit SIMD lane holds: a hash of the user key, or its leading bytes. Leading bytes
preserve order and allow a range scan to use the same kernel; a hash distributes better but forces
the unordered design above.

AVX2 as the shipped baseline with AVX-512 as an opportunistic path, or AVX-512 as a requirement.
This briefly looked decisive and then stopped being. With the permutation in place AVX-512 was 2.6x
faster than AVX2 on the ordered kernel — 8.12 against 21.03 cycles per probe — because AVX2 had to
emulate a 16-lane crossing permute it does not have. Removing the permutation removed the emulation
and the gap fell to 1.64x (6.04 against 9.89), which is the 20% equality gap plus the extra work AVX2
does to mask the live lanes: the same regime, not a different one. Phase 1 weakened half the case for
caution: the AVX-512 kernel is 20% faster than AVX2 (4.04
against 5.01 cycles per probe, 15 retired instructions against 20) and showed no downclocking at
all on Emerald Rapids, which is expected for light integer 512-bit work rather than the sustained
FP and FMA that Intel's frequency licensing actually penalizes. Availability remains the real
argument for an AVX2 default: AVX-512 is absent from most AMD parts before Zen 4, and Intel fused
it off in client parts after Rocket Lake. The measured speed gap is small enough that AVX2 as the
shipped baseline costs little.

Whether to support the `MemTableRep` prefix-extractor and bloom paths, or to declare them out of
scope in the first release.
