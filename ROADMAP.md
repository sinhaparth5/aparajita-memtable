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

The tree holds the architecture document, this roadmap, a `CLAUDE.md`, a README, both license
texts, and a CMake/CLion `.gitignore`. No source, no build system, no tests.

Licensing is settled: dual Apache 2.0 or MIT at the user's option, following the convention of
paired `LICENSE-APACHE` and `LICENSE-MIT` files with the election stated in the README. Source
files should carry `// SPDX-License-Identifier: Apache-2.0 OR MIT`, and the CMake package metadata
should declare the same once Phase 1 creates it.

One setup item remains for week 1 alongside Phase 1. RocksDB needs to be a pinned dependency from
the start rather than at Phase 3, because its `MemTableRep` header is what Phase 2's node layout
has to satisfy.

## Phase 1: environment and micro-benchmarking primitives

Weeks 1-2. Complete, apart from the AVX-512 runtime check. Results and analysis in
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
| Branch misses on the SIMD probe | below 0.5% of branches | met: 0.00 to 0.04% |
| Cycles per lookup, AVX2 versus scalar | measured ratio with dispersion | met: 4.3x to 4.9x (7.0 versus 30.0 cycles/probe) |
| AVX-512 path | correct under CPUID gating, downclocking recorded | blocked: the development host is an i5-8400H with no AVX-512, so the path is compiled and CPUID-gated but has never executed |

The AVX-512 gap carries into Phase 4, since a paper claiming an AVX-512 result needs a machine
that has it: Skylake-SP or newer Xeon, Ice Lake or newer client, or Zen 4.

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

| Exit criterion | Target |
| --- | --- |
| ThreadSanitizer over concurrent insert and read | clean, at 1, 4, 16, and 64 threads |
| Surrogate collision rate | measured on at least one realistic key distribution, not synthetic sequential keys |
| Ordering decision | ordered structure or sort-on-flush, decided and written down |
| Node layout | verified 64-byte aligned and 64 bytes in size by static_assert |

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

| Exit criterion | Target |
| --- | --- |
| RocksDB memtable test suite | passes against Aparajita |
| `IsInsertConcurrentlySupported()` | returns true, with the concurrent path exercised |
| Iterator ordering | matches the skiplist rep's output on the same key set, byte for byte |
| Selection | `db_bench --memtablerep=aparajita` runs without a RocksDB source patch |

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

| Exit criterion | Target |
| --- | --- |
| Thread scaling curve | Aparajita versus skiplist versus VectorRep at 1, 4, 16, 64 threads |
| Regression check | no configuration where Aparajita loses to the skiplist without a stated reason |
| Counter data | branch misses, L1 misses, and HITM per operation for each rep |
| Reproducibility | a script that reruns the full suite from a clean checkout |

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
AVX-512 downclocks on several Intel generations and is absent from most AMD parts before Zen 4,
which argues for AVX2 by default.

Whether to support the `MemTableRep` prefix-extractor and bloom paths, or to declare them out of
scope in the first release.
