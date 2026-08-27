# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## State of the repository

There is no source code yet. The tree holds a research document, a roadmap, an Apache 2.0 LICENSE,
a one-line README, and a `.gitignore` configured for CMake and CLion. There are no build, lint, or
test commands to document; replace this section when the first ones land.

The project commits to dual Apache 2.0 / MIT licensing, but only the Apache text is present. A
`LICENSE-MIT` file is still needed.

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
