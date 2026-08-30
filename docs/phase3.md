# Phase 3: RocksDB plugin integration

Weeks 5-6. Aparajita now builds as a RocksDB plugin, is selectable by name, and
produces byte-identical results to the default skiplist rep. It is also slower
than the skiplist, which is the finding that matters most in this phase and is
the subject of the last section.

Reproduce with `./scripts/build_rocksdb_plugin.sh`. Measurements in
[../results/phase3-rocksdb.txt](../results/phase3-rocksdb.txt).

## What was built

`plugin/aparajita/` holds the RocksDB face of the project: `AparajitaMemTableRep`
deriving from `MemTableRep`, `AparajitaMemTableFactory` from
`MemTableRepFactory`, and the registration hook that makes
`--memtablerep=aparajita` resolve. It is laid out as a RocksDB plugin, so the
supported build links the directory into a RocksDB checkout and configures with
`-DROCKSDB_PLUGINS=aparajita`. No RocksDB source is patched.

The structure itself did not need rewriting. `include/aparajita/memtable.hpp`
became a template over a Traits policy carrying the three things that actually
differ under RocksDB, and the split, the copy-on-write publication and the tower
are shared verbatim with the Phase 2 tests that already cover them:

| | standalone | under RocksDB |
| --- | --- | --- |
| entry | `std::string_view`, 16 bytes, key owned by our arena | `const char*`, 8 bytes, key owned by RocksDB |
| order | `memcmp` on the user key | `MemTableRep::KeyComparator`, a virtual call on the internal key |
| memory | `aparajita::Arena` | the `rocksdb::Allocator` the rep is handed |

`MemTableRep::Allocate` hands key storage to the caller, so the rep never copies
a key. That removed the key copy from the Phase 2 write amplification for free.

## Three things the interface forced

**The surrogate cannot be trusted, only proposed.** The surrogate reproduces
bytewise order over the user key. RocksDB's comparator is the only authority on
order, and it can disagree in two ordinary situations: a column family with a
custom comparator, and two entries with the same user key, which share a
surrogate exactly and are separated by a descending sequence number. So
`sorted_position` treats the SIMD answer as a candidate and confirms it against
the comparator, falling back to a binary search when confirmation fails. This is
sound under any comparator and costs two comparator calls rather than four.

It is also load-bearing rather than defensive. Removing the confirmation and
trusting the SIMD answer makes the structure hang under the reverse-comparator
test: the sort invariant breaks, and `descend` walks a list that is no longer
ordered. The test that catches this is
`MatchesSkipListUnderReverseComparator`.

**Backward iteration with no back-pointers.** `MemTableRep::Iterator` requires
`Prev()` and `SeekForPrev()`. Level 0 lives inside the immutable payload
precisely so a split is a single store, and adding a reverse link would need a
second store and reintroduce the torn state that design exists to avoid. `prev()`
therefore re-descends to find the predecessor node, which is what RocksDB's own
`InlineSkipList::Prev` does. Within a node it is a decrement, so the descent is
paid once per sixteen keys.

**`UniqueRandomSample` is on the flush path.** The base class asserts rather than
providing a default, so leaving it alone aborts a debug build during flush. It is
implemented as reservoir sampling over one forward pass.

## Exit criteria

| Exit criterion | Target | Result |
| --- | --- | --- |
| RocksDB memtable test suite | passes against Aparajita | partial: see below |
| `IsInsertConcurrentlySupported()` | returns true, concurrent path exercised | met |
| Iterator ordering | matches the skiplist rep byte for byte | met |
| Selection | `db_bench --memtablerep=aparajita` with no RocksDB source patch | met |

The first criterion is partial and the reason is structural rather than a
shortfall in the work. RocksDB's own memtable gtests build their `Options`
through `CurrentOptions()` and never consult `--memtablerep`, and `db_stress`
resolves the flag through a hardcoded enum (`StringToRepFactory`) that does not
go through the object registry at all. Running either against Aparajita would
require patching RocksDB, which the fourth criterion explicitly forbids.

What was run instead:

- `memtablerep_bench`, RocksDB's own rep-level harness, which *does* resolve the
  rep through `CreateFromString` and therefore accepts the plugin by name.
- `plugin/aparajita/aparajita_memtable_test.cc`, a differential gtest built inside
  the RocksDB tree that runs an identical workload under Aparajita and under
  `skip_list` and requires the two to agree exactly on forward iteration,
  backward iteration, every `Get`, every `Seek` and every `SeekForPrev`. It covers
  single-threaded and 8-thread concurrent writes, a flush, and a reverse
  comparator. Six tests, all passing, in a Debug build with RocksDB's internal
  assertions enabled.

The differential test is the stronger evidence of the two, because "matches the
skiplist byte for byte" is the criterion that a hand-written expectation could
only approximate.

## The result that matters: it is slower

Median of three runs, 4 threads, 500k keys, WAL off, everything resident in the
memtable. The development laptop is a poor measurement host, but the run-to-run
spread here is under 2% and the gap is far larger than that, so the direction is
not in doubt even if the exact figures are.

| | skiplist | Aparajita | |
| --- | --- | --- | --- |
| `fillrandom` | 6.27 us/op | 7.34 us/op | 17% slower |
| `readrandom` | 3.99 us/op | 5.32 us/op | 33% slower |

Phase 1 measured the intra-node SIMD kernel at 4.04 cycles per probe against
27.65 for the branchy scalar baseline, and that measurement stands. The problem is
that the kernel is a smaller share of a lookup than the project has been assuming.

A 500k-key memtable holds roughly 60k nodes, since copy-on-write splits leave
nodes about half full. The tower descent to reach the right node is therefore
about eight hops, and every hop calls the comparator virtually on a full
variable-length internal key.

> **Correction, Phase 4a.** Both numbers in that sentence are wrong, and
> instrumenting the structure rather than reasoning about it says so: 500,000
> keys make 44,378 nodes at an average fill of 11.3 of 16, and a lookup examines
> **30 successors**, not eight. The error was counting levels instead of hops —
> each level of a skiplist is walked, not jumped, so the cost is
> `(1/p)·log_{1/p} n`, about 30 at branching factor four. The conclusion below
> survives and is in fact understated; see [phase4-descent.md](phase4-descent.md). The SIMD kernel then replaces what would have been
at most four comparisons inside one sixteen-key node. **The design accelerates the
last eighth of the search and leaves the other seven eighths as ordinary
pointer-chasing with virtual comparator calls.** Against `InlineSkipList`, which
inlines its comparator and prefetches, that is not a winning trade at this scale.

Writes lose for a separate and more tractable reason. Every insert rebuilds an
entire node: a fresh 208-byte payload, sixteen surrogates recomputed, and the
shared prefix re-derived from the first and last key. The skiplist links one node
and returns. Copy-on-write bought the atomic split cheaply in Phase 2 and is now
charging for it on the hot path.

Two smaller costs, named so they are not rediscovered: writers contending for the
same node serialise on its spinlock while `InlineSkipList` is lock-free per
writer, and node allocation wastes up to 63 bytes rounding a 208-byte payload up
to 64-byte alignment, because RocksDB's allocators guarantee only pointer
alignment.

## What this means for Phase 4

Phase 4 as written assumes the comparison is worth running. On these numbers it
is not yet, so the honest sequence is to fix the structure first and benchmark
after. The three candidates, in the order their measured cost suggests:

1. **Make the descent cheap, not just the leaf.** This is the big one and it
   questions a premise. Options are widening nodes so the tree is shallower,
   holding surrogates in the *index* nodes so the descent itself is a SIMD
   compare, or caching the comparator's decision so a hop does not always cost a
   virtual call.
2. **Stop copying a whole node per insert.** An append-only node with a small
   published sort permutation would make an insert a couple of stores instead of a
   208-byte rebuild. Sixteen four-bit slot indices fit exactly in one atomic
   64-bit word, which is enough to publish a new order atomically.
3. **Reconsider the per-node spinlock** once the first two are done, since its
   cost is only visible after they stop dominating.

None of this invalidates Phase 1 or Phase 2. The kernels are correct and fast,
the structure is correct and concurrent, and the integration is correct and
selectable. What Phase 3 establishes is that a fast leaf search is not by itself
a fast memtable, and that is worth knowing before a paper claims otherwise.
