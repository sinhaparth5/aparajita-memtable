# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

The header-only core and its own harnesses:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DAPARAJITA_NATIVE=ON for -march=native
cmake --build build -j$(nproc)
ctest --test-dir build -E tsan --output-on-failure   # see the caution below before dropping -E
./build/counter_report                           # hardware counters: cycles and branch misses per probe
./build/collision_report                         # surrogate quality on seven key distributions
./build/bench_search                             # Google Benchmark timings
./scripts/run_phase1.sh                          # Phase 1 only, writing results/
```

Four CTest targets, each also runnable directly for full output:

| CTest name | binary | covers |
| --- | --- | --- |
| `search_kernels` | `test_search` | the equality and `lower_bound` kernels against a scalar reference |
| `memtable` | `test_memtable` | single-threaded structure correctness |
| `concurrent_insert` | `test_concurrent` | insert and read at 1, 4, 16, 64 threads |
| `concurrent_insert_tsan` | `test_concurrent_tsan` | the same, under ThreadSanitizer |

Every test binary is framework-free. It prints one `FAIL:` line per disagreement and exits
non-zero, so a partial failure is readable without parsing a report.

**A plain `ctest` does not terminate on an 8-core host.** `concurrent_insert_tsan` runs the same
thread counts as the uninstrumented target, and the 64-thread case does not complete under
instrumentation on this laptop; it prints 1, 4 and 16 clean and then hangs. That is the documented
Phase 2 gap, not a regression, and `docs/phase2.md` explains why (all 64 threads walk the key space
at the same rate, so they contend on one node's lock, and every `test_and_set` in the backoff loop
is a TSan event). Use `-E tsan` for routine work, run `./build/test_concurrent_tsan` on its own when
touching concurrency, and expect to kill it after the 16-thread line. Configuring with
`-DAPARAJITA_TSAN=OFF` drops the target entirely.

`counter_report` takes optional positional arguments: probe count, repetitions, hit ratio
(`./build/counter_report 200000 9 0.75`). Its `freq/nom` column is core cycles over reference
cycles, which is effective frequency as a multiple of nominal and the only frequency-visible
instrument in the harness; it exists to answer the AVX-512 downclocking question, since every other
counter here is deliberately immune to frequency scaling. It re-verifies every kernel against
`search_scalar_linear` before reporting any timing, so a wrong-but-fast kernel fails there rather
than producing a number. `collision_report` takes a key count (default 200000) and reports expected
full-key comparisons per lookup, where 1.0 is a perfect lane. `bench_search` accepts the usual
Google Benchmark flags, so `--benchmark_filter=probe/` skips the slow working-set sweep. Google
Benchmark is fetched at configure time; pass `-DAPARAJITA_BENCHMARKS=OFF` to build offline without
it.

Two measurement cautions on this class of host. Wall-clock timings from `bench_search` are noisy
under WSL2 or on a loaded machine, with coefficients of variation reaching 60%; the PMU cycle
counts from `counter_report` are the reliable instrument because they are immune to frequency
scaling. And `counter_report` needs `/proc/sys/kernel/perf_event_paranoid` at 2 or lower. It uses
`perf_event_open(2)` directly, so the `perf` binary does not need to be installed.

## The RocksDB plugin

`plugin/aparajita/` is the RocksDB face of the project and is not built by this repository's
CMakeLists: it has to be compiled inside a RocksDB checkout, because `MemTableRep` needs internal
headers (`memory/allocator.h`, `db/lookup_key.h`) that `librocksdb-dev` does not install.

```sh
./scripts/build_rocksdb_plugin.sh    # clones RocksDB v9.11.2 if absent, builds, runs the checks
```

RocksDB is pinned at v9.11.2 in that script. The `MemTableRep` interface is not stable across major
versions, so bumping it means rereading `rocksdb/memtablerep.h` rather than assuming.

Six things about that build cost real time to work out and none are obvious from the plugin README.

Sources are declared in `aparajita.mk` only. RocksDB's CMake appends both the sources parsed out of
the `.mk` and any `${plugin}_SOURCES` set by `CMakeLists.txt`, so declaring them in both compiles the
file twice and fails the link on duplicate symbols. Tests are the other way round: `${plugin}_TESTS`
is read from `CMakeLists.txt` only.

Anything `CMakeLists.txt` sets for RocksDB to read back needs `PARENT_SCOPE`, since RocksDB pulls it
in with `add_subdirectory`. Without it the include path is set in a scope nobody reads.

The registration hook named by `aparajita_FUNC` must be `extern "C"` in the *global* namespace.
RocksDB generates its declaration into `build_version.cc` inside an `extern "C"` block and takes its
address unqualified, so a definition inside `ROCKSDB_NAMESPACE` compiles and then fails to link.

Tests only exist in a Debug build. RocksDB wraps `WITH_TESTS` in a `CMAKE_DEPENDENT_OPTION` that
forces it off unless `CMAKE_BUILD_TYPE` is exactly `Debug`, so `-DWITH_TESTS=ON` on a Release build
silently produces no test targets at all. The script keeps a Release tree for `db_bench` and a
separate Debug tree for `aparajita_memtable_test`, which also turns on RocksDB's internal asserts.

The Release tree is built with `-DUSE_RTTI=ON`, and that is not incidental. The descent's hint fast
path is enabled only when the plugin can confirm the user comparator is the default bytewise one,
and `MemTableRep::KeyComparator` exposes no route to it other than a `dynamic_cast` to
`MemTable::KeyComparator`. RocksDB compiles Release with `-fno-rtti` by default, which turns the
fast path off and costs reads roughly 40%. `USE_RTTI` is a supported cmake option, not a patch, and
it applies to the whole tree, so a comparison against `skip_list` in that tree is still fair. It is
also the control the Phase 4a measurements use: two trees differing in that flag alone, with the
skiplist rows as the evidence that the flag by itself moves nothing.

RocksDB 9.11.2 does not compile with GCC 13 or newer without help: several headers use `uint64_t`
without including `<cstdint>`. The build passes `-include cstdint` rather than patching RocksDB,
because "selectable without patching RocksDB sources" is an exit criterion that has to stay true.

Two limits worth knowing before promising a test run. RocksDB's own memtable gtests build their
options through `CurrentOptions()` and never consult `--memtablerep`, and `db_stress` resolves that
flag through a hardcoded enum rather than the object registry. Neither can be pointed at a plugin rep
without patching RocksDB. `memtablerep_bench` can, because it goes through `CreateFromString`.

## Layout

`include/aparajita/` holds the public headers, and the library is header-only:

- `node.hpp` for the 64-byte SIMD `Node`, the constants (`kCacheLine`, `kNodeKeys`,
  `kNodeCapacity`, `kEmptyKey`) and the order-word encoding a node publishes an insert with
- `search.hpp` for the two kernel families and their runtime dispatch
- `surrogate.hpp` for turning a variable-length key into the 32-bit value a lane holds
- `arena.hpp` for the bump allocator standing in for `rocksdb::Allocator`
- `memtable.hpp` for the ordered concurrent structure built on all of the above
- `workload.hpp` for the branch-hostile probe generator shared by tests and benchmarks

`bench/` and `tests/` hold the harnesses, `docs/` the findings per phase, `results/` the committed
measurement output.

CMake exposes the library as the INTERFACE target `aparajita`, paired with a second INTERFACE
target `aparajita_flags` carrying `-O3 -g -fno-omit-frame-pointer -Wall -Wextra -Wpedantic`. Every
executable links both, so the baselines and the SIMD kernels are always compiled identically; a
comparison built any other way proves nothing. `test_concurrent_tsan` is the one exception, built
at `-O1` with `-fsanitize=thread` as a separate target rather than through a global flag, because
TSan instruments everything it links and the timing harnesses have to stay uninstrumented.

## How the structure works

Reading `memtable.hpp` alone will not explain why it is shaped this way. Four decisions interlock,
and each is argued in `docs/phase2-design.md` or `docs/phase2.md`.

**It is ordered, not sort-at-flush.** RocksDB iterators must yield keys in comparator order, so an
unordered buffer only defers the cost. The thing that made ordering affordable is that a
*relational* compare over a sorted node produces a mask that is a prefix of set bits, so its
popcount is `lower_bound` directly: one compare, one movemask, one `popcnt`, no branch and no
trailing-zero count. That is `lower_bound_avx2` / `lower_bound_avx512`, and `Seek` and ordered
iteration are built on it.

**The lane holds leading key bytes, not a hash**, because a hash destroys the order `lower_bound`
needs. Bytes are loaded big-endian so an unsigned 32-bit compare of two surrogates reproduces
RocksDB's bytewise `memcmp` order, and short keys pad with `0x00` because a prefix sorts before any
key extending it.

**Surrogates are taken after the node's shared prefix, not from the start of the key.** This is the
correction that measurement forced. A lane holding the first four bytes discriminates nothing on
real keyspaces: six of seven realistic distributions in `collision_report` collapse to a single
surrogate value across 200,000 keys, because a table prefix, a tenant id or a big-endian timestamp
puts identical bytes at the front of every key in the database. The lane never needed to be
order-preserving globally, only within a node, so each `NodeData` stores `prefix_len` and takes
surrogates past it. That restores every distribution to an effective lane. Two consequences live in
the code: `fill()` recomputes the prefix from the first and last key at every split, since an
insert at either end can shorten it and a stale prefix silently mis-orders every surrogate; and a
key that does not share the node's prefix is placed by full comparison instead, which is
correctness rather than a fallback optimisation.

**A node is append-only, and the sorted order over its slots is a 64-bit word.** This is Phase 4b
and it is what took writes past the skiplist; `docs/phase4b-append.md` argues it. A slot is written
once, before the order word that names it, and never written again, so an insert is two stores into
a free slot and one release store of the new order. Nothing already visible moves, and a reader that
takes one acquire load of the order word holds a consistent node: everything that word names is
frozen. Until Phase 4b every insert allocated and rebuilt a whole payload, which cost 445 bytes of
arena per key against a live structure of 40.

A nibble holds `slot + 1`, not the slot, so 0 means "unused rank". That costs one of the sixteen
slots and buys the two things that make the scheme work. The word is self-describing -- the count is
the position of its highest nonzero nibble -- so nothing needs a second store beside it. And an
unused rank decodes to slot 15, the lane no key occupies and every node keeps at `kEmptyKey`, so a
permuted node is sentinel-padded for free. Do not "recover" the sixteenth slot without reading the
last section of the design document first.

Slot order is insertion order, so the lanes are not sorted in memory and `lower_bound` permutes them
into rank order before comparing: `lower_bound_perm_avx512` does it with one `vpermd`, the AVX2 kernel
with four permutes and two blends. The load is masked to the live slots, and that mask is about
concurrency, not about the answer -- a writer is storing into slot `count` while the kernel runs, and
an unmasked 64-byte load would race with it. No test can catch its removal and neither can TSan,
which does not instrument the vector load; the mask is justified by the memory model alone.

**`NodeData`'s level-0 link and its order word both live inside the payload.** That is what makes a
split atomic. A split changes a node's contents and its successor at once, so it publishes the right
half first and then swaps the left half's payload pointer in a single release store: a reader sees
either the old full node or the new pair, never a state where a key is duplicated across both or
missing from both. Moving either `next` or `order` out into `ListNode` beside the tower would need
two stores and would expose exactly those states. Do not move them.

Above level 0 is a skiplist tower with branching factor four, and everything in it is an
accelerator and never the truth: a stale or missing tower pointer costs a longer walk, never a
wrong answer, because the search only follows a pointer to a node whose first key is at or below
the target and nodes are ordered and never removed. It is not decoration. Before the tower existed
the 64-thread test did not finish in two minutes; with it the set takes 0.7 seconds.

**A tower hop compares an eight-byte hint in the node header, not the node's first key.** This is
Phase 4a and it is what took reads past the skiplist; `docs/phase4-descent.md` argues it. Three
invariants hold it up and each is easy to break by accident.

The hint is immutable. `descend()` returns the last node whose first key is at or below the key
being inserted, so an insert into a non-head node always sorts at position 1 or later and a split
leaves the left half's first key alone. The head is the sole exception and is also the only node no
hop ever moves *to*, so its hint is never read. Any change that lets a non-head node's first key
decrease invalidates the whole scheme.

A tie decides nothing. Equal hints fall through to the comparator, which is why the hint is eight
bytes and the surrogate is four: a surrogate tie costs one comparison inside a node already in
cache, a hint tie costs the miss chain the hint exists to avoid.

`publish()` stores the hint before the payload, and the order is not stylistic. A first key never
rises, so a new hint beside an old payload is a hint that is too small, and too small only ever lets
a hop enter a node it was entitled to enter. The other order exposes a hint that is too large, and a
hop then stops one node short of the key it wanted.

The fast path is off unless `Traits::hint_ordering()` says the comparator agrees with bytewise order,
because a wrong hop is not a slow lookup, it is a lost key. `tests/test_memtable.cpp` carries a
shared-prefix workload where every hint ties, which is the only test in the tree that fails if the
fallback is removed; random keys tie too rarely to notice.

Readers are lock-free and never write. Writers take a per-node spinlock that pauses 64 times and
then yields. This narrows the "lock-free concurrency" pillar and is recorded rather than hidden: a
fully lock-free ordered insert has to shift keys inside a sorted node, which is not one atomic
operation, so the design publishes through a single release store with a retry plus a lock to stop
writers livelocking against each other. Phase 4b shrank what that lock covers from an allocation and
a fifteen-key merge to three stores, which is what makes replacing it with a CAS on the order word a
live question rather than an idle one; it was deliberately left for its own phase. The backoff yields because RocksDB routinely runs more writers than cores, where
a flat spin burns every other thread's slice while the lock holder waits to be scheduled.

The layout that all of this protects is enforced by `static_assert`, not by comment: surrogates at
offset 0, cold fields (`order`, `prefix_len`, `next`, the keys) beginning at offset 64,
`NodeData` and `ListNode` both 64-byte aligned, and `first_hint` within `ListNode`'s first line.
`lower_bound_surrogate` reinterprets the surrogate array as a `Node` and depends on those assertions
holding.

## Working on the search kernels

There is no global `-mavx2`. The vector kernels carry per-function
`__attribute__((target("avx2")))` / `("avx512f")`, which is what keeps dispatch a runtime CPUID
decision and lets one binary run on hosts without the ISA. Adding a global `-m` flag would let the
compiler emit those instructions outside the gated functions and fault on older hardware.

Three kernel families, with different registration duties:

- `SearchFn` (equality, `search_*`) is registered in `tests/test_search.cpp` in `runnable_kernels()`,
  in `bench/counter_report.cpp` in the `kernels[]` array, and in `bench/bench_search.cpp` as a
  `BENCHMARK_TEMPLATE`. None of the three reference each other.
- `LowerBoundFn` (ordered over a sorted node, `lower_bound_*`) is registered in
  `tests/test_search.cpp` only. Nothing in the structure calls it any more; it is the reference the
  permuted kernels are reasoned against and the fair comparison for what permuting costs.
- `LowerBoundPermFn` (ordered over an append-only node, `lower_bound_perm_*`) is what
  `BasicMemTable` actually dispatches to, and is registered in `tests/test_search.cpp` only.

Registering a permuted kernel there is not a formality, and this is the one place in the repository
where skipping a test would go unnoticed indefinitely. `sorted_position` confirms every SIMD
candidate against the comparator and falls back to binary search, so an ordered kernel returning
noise still yields correct lookups, correct iteration, and a byte-for-byte match against RocksDB's
skiplist. Four deliberate kernel bugs passed the entire differential suite and fail only in
`test_search`. Anything that touches these kernels must be checked there.

The counter and timing harnesses still cover the equality family only, which is the remaining
measurement gap before Phase 4 quotes an ordered number.

All three carry a `runnable` flag derived from `detect_isa()`. The ISA guards are not optional
bookkeeping: an unguarded AVX-512 kernel executes and faults on a host that lacks it.

Four conventions in the existing kernels are load-bearing and measured, not stylistic.

Search returns `kNotFound` (equal to `kNodeKeys`) rather than -1, because a -1 sentinel forces a
data-dependent ternary that cost 0.5 branch mispredictions per probe and four fifths of the AVX2
kernel's cycles; folding the miss into the compare mask as bit 16 removes it. `lower_bound` returns
`kNodeKeys` for "above everything here" by the same convention, which falls out of the popcount for
free. Any hot-path API that reports absence through a sentinel should follow the same rule.

`kEmptyKey` is `0xFFFFFFFF` so that it sorts above every real key, which keeps a partially filled
node sorted. Two places depend on this in ways that are easy to break. `lower_bound_avx2` uses
`_mm256_cmpgt_epi32`, which is a *signed* compare, so both sides are XOR-ed by `0x80000000` to map
unsigned order onto signed; without the bias `kEmptyKey` reads as -1 and sorts below every real key,
inverting the padding. AVX-512 has `_mm512_cmplt_epu32_mask` natively and needs no bias. And a real
key beginning with four `0xFF` bytes produces a surrogate equal to `kEmptyKey`, which is why nodes
carry an explicit `count` and never infer occupancy from the sentinel; `aliases_empty_sentinel()`
exists so that intent is greppable.

Use `_mm256_movemask_ps` on a cast rather than `_mm256_movemask_epi8`, which yields four bits per
lane and needs the trailing-zero count divided by 4.

Two smaller things that look like noise and are not. `search_scalar_linear` and
`lower_bound_scalar` carry `#pragma GCC novector`; without it GCC turns the honest branchy baseline
into the vector kernel and the headline comparison measures nothing. And `kDestructiveInterference`
in `node.hpp` is a literal 64 rather than `std::hardware_destructive_interference_size`, because GCC
warns that the standard constant varies with `-mtune` and compiler version, which would bake an
unstable value into a header RocksDB compiles against.

## Measurement discipline

`workload::make_probes` is the branch-hostile generator and its properties are easy to break by
accident. The hit/miss mix is randomized rather than blocked, hits land on a uniformly random slot,
and a miss is verified absent from every node. Blocking the mix, or always probing slot 0, makes the
branchy baseline look far better than it is and hides the mispredictions the project exists to
remove. `tests/test_concurrent.cpp` interleaves keys across threads for the same reason: blocked
ranges would put writers on different nodes almost always and would never exercise a writer
arriving after a split.

`docs/phase1.md`, `docs/phase2.md`, `docs/phase4-descent.md` and `docs/phase4b-append.md` quote
committed numbers in prose and tables. Changing a kernel or the structure means regenerating the
results and the document together; a stale document is worse than no document. `results/` holds output from `./scripts/run_phase1.sh`, which covers Phase 1 only;
the Phase 2 numbers in `docs/phase2.md` came from ad-hoc runs of `collision_report` and
`test_concurrent` and have no script yet. `results/archive-i5-8400h/` is a superseded run kept for
the cross-generation comparison; never quote it as current. `-march=native`
(`-DAPARAJITA_NATIVE=ON`) is for local measurement only, since it is not what ships and can silently
change which kernel the compiler emits, so anything destined for the paper needs the portable build
reported alongside it.

Concurrent insert throughput measured against `arena.hpp` is bounded by its single allocation mutex
and should not be quoted. RocksDB's `ConcurrentArena` hands each writer a per-thread block precisely
to avoid that, and Phase 3 inherits it.

Paper-grade numbers come from a rented machine, not from the laptop. The working recipe is an
ephemeral GCP instance, and two details cost real time to rediscover. PMU access needs
`--performance-monitoring-unit=architectural`, which the API rejects on every `c3`, `c3d` and `n2`
shape tried; `c4-standard-8` accepts it, and `c4` also requires `--boot-disk-type=hyperdisk-balanced`
rather than `pd-balanced`. Set `kernel.perf_event_paranoid` to 1 on the instance and build with
`CXX=g++-14` or newer, since `#pragma GCC novector` on the scalar baseline is silently ignored by
GCC 13 and older.

Delete the instance the moment the results are copied back, and confirm with
`gcloud compute instances list` and `gcloud compute disks list` that neither the instance nor an
orphaned boot disk survives.

## State of the repository

Phases 1 through 3 are complete. The SIMD kernels are correct and fast, the standalone structure is
correct and concurrent, and Aparajita builds as a RocksDB plugin that `db_bench` selects by name and
that matches the default skiplist byte for byte.

Phases 4a and 4b are also complete, and between them Aparajita is now ahead of the default skiplist
on both halves of the workload it was written to beat. Phase 3 closed 17% behind on `fillrandom` and
33% behind on `readrandom`. Two structural problems accounted for all of it, and neither was in the
SIMD kernels.

Phase 4a fixed the descent (`docs/phase4-descent.md`). A lookup spent about thirty tower hops
reaching the right node -- not eight, as `docs/phase3.md` first estimated -- each a virtual
comparator call on a full internal key. Each node now caches the first eight bytes of its first key,
so a hop compares two integers in a line it has already loaded and only a tie falls back to the
comparator. `results/phase4-descent.txt` has the numbers and the RTTI caveat that goes with them.

Phase 4b fixed the insert (`docs/phase4b-append.md`). Copy-on-write rebuilt a whole node per insert,
which cost 445 bytes of arena per key against a live structure of 40 and scattered the nodes a
lookup walks across ten times the memory they occupy. A node is now append-only and publishes an
insert as one store of a 64-bit order word. `fillrandom` crossed over with it and `readrandom`
improved by a sixth for free. `results/phase4b-append.txt` has the numbers.

What Phase 3 answered, and what it left open:

- **Write amplification is gone on the RocksDB side.** `MemTableRep::Allocate` hands key storage to
  the caller, so the rep never copies a key and an entry is a bare 8-byte pointer into RocksDB's
  arena rather than a 16-byte `std::string_view`. Phase 4b then removed the per-insert node rebuild,
  so the rep allocates only when a node splits. Where the rep used to charge roughly three times the
  skiplist's arena per key it now charges about 1.4 times.
- **The surrogate is a candidate, never an answer.** `Traits::order_bytes` strips the 8-byte packed
  tag so the prefix and surrogate are computed on the user key, but bytewise order on the user key
  is not RocksDB's order: a custom comparator can disagree, and equal user keys are separated by a
  *descending* sequence number. `sorted_position` therefore confirms every SIMD candidate against
  the comparator and falls back to binary search. That confirmation is load-bearing, not defensive.
- `arena.hpp` was a stand-in written against arena semantics deliberately, and swapping in
  `rocksdb::Allocator` behind `Traits::allocate` was the substitution it was meant to be. Its single
  mutex is gone on the RocksDB path; the standalone harnesses still use it.
- ThreadSanitizer coverage at 64 threads is still the one open Phase 2 exit criterion, and closing
  it still needs a host with more cores rather than a change to the structure.

The development laptop is an i5-11300H (Tiger Lake) with 8 logical cores. It has AVX-512, unlike the
i5-8400H that produced the archived results, but it runs under WSL2 where the governor is not
readable and cycle dispersion is an order of magnitude worse. It is a fine correctness host and a
poor measurement one, and it is the host on which the TSan target hangs.

Phase 4b's numbers in `results/phase4b-append.txt` came from a rented `c4-standard-16` instead, and
that is the pattern to follow for anything quoted onward. Two details cost time there. TSan aborts at
startup on Ubuntu 24.04 with "unexpected memory mapping" until `sysctl vm.mmap_rnd_bits=28` is set,
which is the kernel's ASLR entropy and not a fault in the target. And a benchmark sharing the box
with a build or a TSan run is not a measurement: the 64-thread TSan case alone drives load average
past 60 on sixteen cores, so it has to be run before or after the benchmarks and never beside them.

The project is dual-licensed under Apache 2.0 or MIT at the user's option, with the texts in
`LICENSE-APACHE` and `LICENSE-MIT`. New source files should carry an SPDX header of
`// SPDX-License-Identifier: Apache-2.0 OR MIT`.

## What the project is

Aparajita is a C++20 in-memory buffer (MemTable) plugin for RocksDB and LevelDB-style LSM-tree
engines. It replaces RocksDB's default concurrent skiplist, whose pointer-chasing across heap
allocations causes L1/L2 cache misses and branch mispredictions on every key comparison during
high-throughput multi-threaded writes.

The design rests on three things: 64-byte cache line alignment (`alignas(64)`) so no node straddles
a line, branchless SIMD search over vector comparison masks instead of a conditional jump per
comparison, and C++20 acquire/release atomics instead of locks on the read path.

It integrates by deriving from `rocksdb::MemTableRep` and `rocksdb::MemTableRepFactory`, selected
through `options.memtable_factory`. The Phase 3 surface is larger than `Insert` and `Contains`:
`Allocate`, `InsertKey` and its concurrent variants, `Get`, `GetIterator`, `ApproximateMemoryUsage`,
and `MarkReadOnly`. Build against RocksDB's `plugin/` mechanism so `db_bench --memtablerep=aparajita`
selects it without patching RocksDB sources.

## Constraints that shape the code

C++20, not later. The design uses concepts, the atomic memory model, and standard alignment
primitives.

Node memory comes from RocksDB's arena through the `Allocator*` the rep is handed, which is what
charges against `write_buffer_size`. Arena memory is bump-allocated and cannot be reallocated, so
growable storage has to be segmented into chunks rather than resized. Alignment is per allocation,
not global: nodes need 64 bytes, and rounding a 13-byte key copy up to 64 cost roughly a tenth of
the arena before it was fixed.

`IsInsertConcurrentlySupported()` must return true and `InsertKeyConcurrently` must be implemented.
RocksDB only routes concurrent writes to a rep that advertises support, and without it every
multi-threaded write benchmark serializes and the project's central claim cannot be demonstrated.
`VectorRep` not supporting it is part of why it loses on write throughput despite a friendlier
memory layout.

AVX2 is the shipped baseline with AVX-512 gated behind a CPUID check. Phase 1 measured AVX-512 at
20% faster with no downclocking on Emerald Rapids, so the argument for an AVX2 default is
availability rather than speed: AVX-512 is absent from most AMD parts before Zen 4, and Intel fused
it off in client parts after Rocket Lake.

## Open design questions

Two of the four in `ROADMAP.md` are settled, in `docs/phase2-design.md`: the structure is ordered,
and the lane holds leading key bytes rather than a hash. Both are load-bearing now, so reopening
either means rewriting `lower_bound` and the whole iteration path.

Two remain. Whether AVX-512 is opportunistic or required, which now rests on availability rather
than measured speed. And whether the `MemTableRep` prefix-extractor and bloom paths are in scope for
the first release: Phase 3 shipped without them, so the rep inherits the base class's
`GetDynamicPrefixIterator`, which is the full iterator. That is correct but gives up the skiplist's
prefix-bloom short-circuit, and it stays a deliberate omission rather than a settled answer.

## The architecture document

`High Performance C++ System Architecture.md` predates the project scope and mostly does not apply.
A `MemTableRep` runs inside RocksDB's thread and I/O model, so the document's thread-per-core
execution model, io_uring and SQPOLL work, and write-ahead-log phase belong to RocksDB rather than
to this plugin. What remains useful is the microarchitectural material: cache line alignment and
false sharing, Struct of Arrays layout, static dispatch over virtual calls, and the `perf`
methodology.

Two quirks when reading it. Its numeric targets are broken image references (`![][image20]` through
`![][image27]`), so those numbers are unrecoverable from the file; the sole surviving figure is a
98.5% L1 hit rate. Its trailing digits on sentences (`...cache affinity1.`) are citation markers
pointing at the "Works cited" list at the end.

## Other agent configs

A Codex config exists at `~/.codex/config.toml`. To bring its MCP servers, commands, or
instructions into Claude Code, reply `/import` to see what is importable, then
`/import --yes=<digest>` using the digest the scan prints. If `/import` is unavailable on this
surface, run `claude import` from a terminal.
