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

`counter_report` takes optional positional arguments: probe count, repetitions, hit ratio
(`./build/counter_report 200000 9 0.75`). Google Benchmark is fetched at configure time; pass
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

## State of the repository

Phase 1 is complete except for the AVX-512 runtime check, which needs hardware this development
host lacks. There is no RocksDB integration yet; that is Phase 3.

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

Two conventions in the existing kernels are load-bearing and measured, not stylistic. Search
returns `kNotFound` (equal to `kNodeKeys`) rather than -1, because a -1 sentinel forces a
data-dependent ternary that cost 0.5 branch mispredictions per probe and four fifths of the AVX2
kernel's cycles; folding the miss into the compare mask as bit 16 removes it. And `kEmptyKey` is
`0xFFFFFFFF` so that it sorts above every real key, which keeps a partially filled node sorted.
Use `_mm256_movemask_ps` on a cast rather than `_mm256_movemask_epi8`, which yields four bits per
lane and needs the trailing-zero count divided by 4.

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
