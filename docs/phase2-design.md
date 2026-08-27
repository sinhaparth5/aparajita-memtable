# Phase 2 design decisions

Two of the four open questions in `ROADMAP.md` are settled here, because the
Phase 2 code cannot be written without them. They resolve together: the second
follows from the first.

## Decision 1: ordered structure, not sort-at-flush

**Aparajita keeps keys in comparator order at all times.** It does not follow
`VectorRep` in accepting an unordered buffer and sorting when the memtable becomes
immutable.

The argument that made this look hard was that a SIMD node search is an equality
compare, and equality has no notion of order. That framing was wrong, and the
Phase 1 kernels are what made it look true: `_mm256_cmpeq_epi32` answers only "is
this key present."

Replace the equality compare with a *relational* one and the order falls out for
free. Over a sorted node, the mask from a less-than compare is a prefix of set
bits, so its popcount is the number of keys below the needle, which is exactly
`lower_bound`. That is one compare, one movemask and one `popcnt`: the same shape
and roughly the same cost as the equality kernel, with no branch and no
trailing-zero count. `lower_bound_avx2` and `lower_bound_avx512` in `search.hpp`
implement it, and it is what `Seek` and ordered iteration are built on.

With `lower_bound` available at that price the case for sort-at-flush collapses:

- RocksDB iterators must yield keys in comparator order regardless, so an
  unordered structure only defers the cost, it does not remove it.
- `VectorRep` pays that deferred cost badly. It re-sorts to serve an iterator,
  which is why it loses on `readwhilewriting` — the exact benchmark this project
  needs to win, since a mixed read/write workload is where a memtable's layout
  actually matters.
- A flush-time sort is a latency spike on the write path at the worst moment,
  when the memtable is already being handed off.
- `VectorRep` also does not support concurrent insert, and Phase 3 requires
  `IsInsertConcurrentlySupported()` to return true. Copying its design would mean
  re-deriving the concurrency story anyway.

The cost accepted in exchange is that insertion must place a key at its sorted
position rather than appending, which is the harder half of the lock-free work
and is the main risk in this phase.

## Decision 2: the SIMD lane holds leading key bytes, not a hash

This follows from Decision 1 rather than being an independent choice. A hash
destroys order, so a hashed lane can only ever answer equality, which would put
`lower_bound` out of reach and force the sort-at-flush design just rejected.

The lane therefore holds the first four bytes of the user key, loaded
**big-endian** so that an unsigned 32-bit comparison of two surrogates agrees with
the bytewise `memcmp` order RocksDB's default comparator uses. Keys shorter than
four bytes pad with `0x00`, which is correct because a prefix sorts before any key
extending it.

Three consequences to carry into the implementation:

- The surrogate is lossy, so every SIMD hit is a *candidate*. Confirming it needs
  a full comparison against the stored key. This was already true for a hash; the
  difference is that a comparison result is still useful when it is not an exact
  match, whereas a hash mismatch tells you nothing.
- Collision behavior is workload-dependent in a way a hash's is not. Keys sharing
  a four-byte prefix — a common shape, since RocksDB users routinely prefix keys
  with a tenant or table id — all collapse to one surrogate, and the node
  degenerates toward a linear scan of candidates. Measuring this on a realistic
  key distribution rather than on sequential integers is a Phase 2 exit criterion
  for exactly this reason.
- `kEmptyKey` is `0xFFFFFFFF`, so a real key beginning with four `0xFF` bytes
  produces a surrogate indistinguishable from an empty slot. That is not merely a
  false positive to be filtered by full comparison: it breaks the padding
  invariant that the sorted-tail layout depends on. The node needs an explicit
  occupancy count rather than inferring occupancy from the sentinel.

## What is still open

The two remaining questions in `ROADMAP.md` are untouched here. The AVX-512
baseline question now has Phase 1 evidence behind it (no downclocking, 20% faster
than AVX2) but the decision rests on availability rather than speed. Whether the
prefix-extractor and bloom paths are in scope is a Phase 3 concern.
