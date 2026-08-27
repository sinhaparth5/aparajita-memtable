# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # add -DAPARAJITA_NATIVE=ON for -march=native
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure       # correctness gate for the search kernels
./build/counter_report                           # hardware counters: cycles and branch misses per probe
./build/bench_search                             # Google Benchmark timings
./scripts/run_phase1.sh                          # all of the above, writing results/
```

There is one test target, registered with CTest as `search_kernels`; run it directly as
`./build/test_search` for full output, or `ctest --test-dir build -R search_kernels`. It exits
non-zero and prints one `FAIL:` line per disagreement, so a partial failure is readable without
a test framework.

`counter_report` takes optional positional arguments: probe count, repetitions, hit ratio
(`./build/counter_report 200000 9 0.75`). Its `freq/nom` column is core cycles over reference
cycles, which is effective frequency as a multiple of nominal and the only frequency-visible
instrument in the harness; it exists to answer the AVX-512 downclocking question, since every other
counter here is deliberately immune to frequency scaling. It re-verifies every kernel against `search_scalar_linear`
before reporting any timing, so a wrong-but-fast kernel fails there rather than producing a number.
`bench_search` accepts the usual Google Benchmark flags, so `--benchmark_filter=probe/` skips the
slow working-set sweep. Google Benchmark is fetched at configure time; pass
`-DAPARAJITA_BENCHMARKS=OFF` to build offline without it.

Two measurement cautions on this class of host. Wall-clock timings from `bench_search` are noisy
under WSL2 or on a loaded machine, with coefficients of variation reaching 60%; the PMU cycle
counts from `counter_report` are the reliable instrument because they are immune to frequency
scaling. And `counter_report` needs `/proc/sys/kernel/perf_event_paranoid` at 2 or lower. It uses
`perf_event_open(2)` directly, so the `perf` binary does not need to be installed.

## Layout

`include/aparajita/` holds the public headers: `node.hpp` for the 64-byte node and its constants,
`search.hpp` for the scalar and SIMD kernels plus runtime dispatch, `workload.hpp` for the
branch-hostile probe generator shared by tests and benchmarks. `bench/` and `tests/` hold the
harnesses, `docs/phase1.md` the findings, `results/` the committed measurement output.

The library is header-only. CMake exposes it as the INTERFACE target `aparajita`, paired with a
second INTERFACE target `aparajita_flags` carrying `-O3 -g -fno-omit-frame-pointer -Wall -Wextra
-Wpedantic`. Every executable links both, so the baselines and the SIMD kernels are always compiled
identically; a comparison built any other way proves nothing.

## Working on the search kernels

There is no global `-mavx2`. The vector kernels carry per-function
`__attribute__((target("avx2")))` / `("avx512f")`, which is what keeps `search_dispatch()` a runtime
CPUID decision and lets one binary run on hosts without the ISA. Adding a global `-m` flag would let
the compiler emit those instructions outside the gated functions and fault on older hardware.

A new kernel has to satisfy the `SearchFn` signature and be registered in three places, none of
which reference each other:

- `tests/test_search.cpp`, in `runnable_kernels()`, with a `runnable` flag derived from `detect_isa()`
- `bench/counter_report.cpp`, in the `kernels[]` array, with the `Isa` it needs
- `bench/bench_search.cpp`, as a `BENCHMARK_TEMPLATE` registration

The ISA guards are not optional bookkeeping: an unguarded AVX-512 kernel executes and faults on a
host that lacks it, which is exactly the situation this repository is in.

Three conventions in the existing kernels are load-bearing and measured, not stylistic.

Search returns `kNotFound` (equal to `kNodeKeys`) rather than -1, because a -1 sentinel forces a
data-dependent ternary that cost 0.5 branch mispredictions per probe and four fifths of the AVX2
kernel's cycles; folding the miss into the compare mask as bit 16 removes it. Any hot-path API that
reports absence through a sentinel should follow the same rule.

`kEmptyKey` is `0xFFFFFFFF` so that it sorts above every real key, which keeps a partially filled
node sorted. Real keys must never take that value.

Use `_mm256_movemask_ps` on a cast rather than `_mm256_movemask_epi8`, which yields four bits per
lane and needs the trailing-zero count divided by 4.

Two smaller things that look like noise and are not. `search_scalar_linear` carries
`#pragma GCC novector`; without it GCC turns the honest branchy baseline into the AVX2 kernel and
the headline comparison measures nothing. And `kDestructiveInterference` in `node.hpp` is a literal
64 rather than `std::hardware_destructive_interference_size`, because GCC warns that the standard
constant varies with `-mtune` and compiler version, which would bake an unstable value into a header
RocksDB compiles against. The architecture document's argument for 128-byte separation applies to
Phase 2's concurrently written state, not to these read-mostly key arrays.

## Measurement discipline

`workload::make_probes` is the branch-hostile generator and its properties are easy to break by
accident. The hit/miss mix is randomized rather than blocked, hits land on a uniformly random slot,
and a miss is verified absent from every node. Blocking the mix, or always probing slot 0, makes the
branchy baseline look far better than it is and hides the mispredictions the project exists to
remove.

`results/` holds committed output from `./scripts/run_phase1.sh`, and `docs/phase1.md` quotes those
numbers in prose and tables. Changing a kernel means regenerating both together; a stale
`docs/phase1.md` is worse than no document. `results/archive-i5-8400h/` is a superseded run kept for
the cross-generation comparison; never quote it as current. `-march=native`
(`-DAPARAJITA_NATIVE=ON`) is for local measurement only, since it is not what ships and can silently
change which kernel the compiler emits, so anything destined for the paper needs the portable build
reported alongside it.

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

Phase 1 is complete, AVX-512 included: the kernel executes, passes the tests, and shows no
downclocking. There is no RocksDB integration yet; that is Phase 3. RocksDB is not yet a pinned
dependency, though `ROADMAP.md` wants it early because its `MemTableRep` header is what Phase 2's
node layout must satisfy.

The development laptop is an i5-11300H (Tiger Lake). It does have AVX-512, unlike the i5-8400H that
produced the archived results, but it runs under WSL2 where the governor is not readable and cycle
dispersion is an order of magnitude worse. It is a fine correctness host and a poor measurement one.

The project is dual-licensed under Apache 2.0 or MIT at the user's option, with the texts in
`LICENSE-APACHE` and `LICENSE-MIT`. New source files should carry an SPDX header of
`// SPDX-License-Identifier: Apache-2.0 OR MIT`.

## What the project is

Aparajita is a C++20 in-memory buffer (MemTable) plugin for RocksDB and LevelDB-style LSM-tree
engines. It replaces RocksDB's default concurrent skiplist, whose pointer-chasing across heap
allocations causes L1/L2 cache misses and branch mispredictions on every key comparison during
high-throughput multi-threaded writes.

The design rests on three things: 64-byte cache line alignment (`alignas(64)`) so no node straddles
a line, branchless SIMD search over vector comparison masks (`_mm256_cmpeq_epi32` plus a movemask)
instead of a conditional jump per comparison, and lock-free concurrency through C++20
acquire/release atomics.

It integrates by deriving from `rocksdb::MemTableRep` and `rocksdb::MemTableRepFactory`, selected
through `options.memtable_factory`.

## Constraints that shape the code

C++20, not later. The design uses concepts, the atomic memory model, and standard alignment
primitives.

Node memory comes from RocksDB's arena through the `Allocator*` the rep is handed, which is what
charges against `write_buffer_size`. Arena memory is bump-allocated and cannot be reallocated, so
growable storage has to be segmented into chunks rather than resized.

`IsInsertConcurrentlySupported()` must return true and `InsertKeyConcurrently` must be implemented.
RocksDB only routes concurrent writes to a rep that advertises support, and without it every
multi-threaded write benchmark serializes and the project's central claim cannot be demonstrated.

RocksDB keys are variable-length internal keys, a user key followed by a packed sequence number and
value type, not fixed-width integers. A 32-bit SIMD lane therefore compares a surrogate (a hash, or
the key's leading bytes) and every hit is a candidate needing full confirmation.

AVX2 is the working baseline with AVX-512 gated behind a CPUID check, since AVX-512 downclocks on
several Intel generations and is absent from most pre-Zen-4 AMD parts.

## Open design questions

Four questions in `ROADMAP.md` are unresolved and each changes code. The largest is whether the
structure keeps keys in comparator order or sorts at flush time the way `VectorRep` does, since
RocksDB iterators must yield keys in comparator order. Related to it: whether the SIMD lane holds a
hash of the user key or its leading bytes. Then the AVX-512 baseline question, and whether the
prefix-extractor and bloom paths are in scope for the first release.

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
