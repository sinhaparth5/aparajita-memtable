<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# Phase 4a: making the descent cheap

Phase 3 closed correct and slower: 17% behind the skiplist on `fillrandom`, 33%
behind on `readrandom`. Its closing argument was that the SIMD kernel accelerates
the last fraction of a lookup and leaves the rest as ordinary pointer chasing,
and that Phase 4's evaluation was therefore blocked on structural work.

This is the first piece of that work. It changes what a tower hop costs, and
nothing else: no kernel changed, no node layout changed, and the concurrency
argument is untouched.

## What a lookup was actually doing

The first thing to fix was the estimate. `docs/phase3.md` guessed "about eight
tower hops", reasoning from nodes being about half full. Both halves of that were
wrong, and the instrumented structure says so:

| | measured |
| --- | --- |
| nodes for 500,000 keys | 44,378 |
| average fill | 11.3 of 16 keys, not 8 |
| successors examined per lookup | **30.0** |
| of those, advances taken | 22.3 |
| of those, on level 0 | 4.0 |

Thirty, not eight. That is not a defect in the tower — the linkage is exactly what
a branching factor of four predicts, 4,374 nodes linked at level 1 out of 4,375
eligible — it is simply what a skiplist descent costs. The expected number of
comparisons for branching factor `p` is `(1/p)·log_{1/p} n`, which for 44,378
nodes at `p = 1/4` is about 30. Phase 3 counted levels and forgot that each level
is walked, not jumped.

So the SIMD kernel was replacing at most four comparisons out of thirty-four.

## What each of those thirty cost

`starts_at_or_below(nx, key)` asked whether a successor's first key is at or below
the target, and answering it walked a chain of dependent loads:

1. `nx->data` — free, it shares a line with the tower pointer already loaded;
2. `*d` at offset 64 — the surrogate array fills the first line, so `keys[0]` is
   on the node's *second* line: one miss;
3. the key bytes `keys[0]` points at, somewhere else in the arena: one miss;
4. a virtual call into the comparator, then a `memcmp`.

Three dependent random accesses per hop, thirty hops, on an arena far larger than
the last-level cache. At roughly 80-100 ns of memory latency each, that is the
whole 3 microseconds a standalone probe was measuring. The descent was not
comparison-bound. It was a pointer-chasing latency chain, and the comparator was
riding along on it.

## The change

Each node caches `first_hint`: the first eight bytes of its first key's order
bytes, big-endian and zero-padded, in the node header beside the tower pointers
the hop has already loaded. A hop compares two integers. Steps 2, 3 and 4 above
happen only when the two hints are equal.

```
if (hints_) {
    const std::uint64_t h = n->first_hint.load(std::memory_order_relaxed);
    if (h != hint) {
        return h < hint;      // decided, and nothing else was touched
    }
}
// unchanged: load the payload, call the comparator
```

Eight bytes rather than the surrogate's four, because the fallback here is not
cheap. A node-local surrogate that ties costs one extra comparison inside a node
already in cache; a descent hint that ties costs exactly the miss chain it exists
to avoid.

Three things make this safe.

**The hint is immutable.** `descend()` returns the last node whose first key is at
or below the key being inserted, so an insert into a non-head node always sorts at
position 1 or later, and a split leaves the left half's first key alone. A node's
first key is therefore fixed from the moment it is published. The head is the one
exception — it is where a key below everything in the structure lands — and it is
also the node every descent starts from and no hop ever moves *to*, so its hint is
never read.

**A tie decides nothing.** Equal hints fall through to the code that ran before.
Zero padding is what makes this correct rather than merely conservative: under
bytewise comparison a prefix sorts before every key extending it, and a real
`0x00` byte in that position compares equal to the padding, which is exactly the
"undecided in the first eight bytes" answer the fallback wants.

**Publication order is hint first, payload second.** A node's first key never
rises, so a reader that catches a new hint beside an old payload holds a hint no
larger than either payload's first key, and a too-small hint only ever lets a hop
enter a node it was already entitled to enter. The other order would expose the
old, larger hint beside the new payload, and a hop would stop one node short of
the key it wanted.

## The part that is a promise, not a mechanism

Comparing hints reproduces *bytewise* order. The comparator is the authority, and
the two agree only when the user comparator is the default bytewise one. Under a
reverse comparator the hint is confidently and exactly wrong, and unlike the
surrogate — which proposes a candidate the comparator then confirms — a wrong hop
is not corrected downstream. It does not cost time. It loses keys.

So the fast path is gated on `Traits::hint_ordering()`, and the plugin has to
answer that from the comparator RocksDB hands it. That turned out to be the
hardest part of the change, and the difficulty is in the interface.

`MemTableRep::KeyComparator` exposes ordering and nothing else. There is no
accessor to the `InternalKeyComparator` behind it, let alone to the user
comparator's name. The only route to it is the concrete type RocksDB actually
passes, `MemTable::KeyComparator`, and recovering that needs a `dynamic_cast` —
which needs RTTI, which RocksDB's Release build disables with `-fno-rtti` unless
asked otherwise.

The tempting alternative was to probe: order a handful of synthetic keys, check
the comparator agrees, and generalise. That was rejected. A comparator that agrees
on every probe and disagrees on the tenth key of a real workload would pass, and
the failure mode is a lost key rather than a slow lookup. `docs/phase3.md` already
records that the surrogate proposes and the comparator disposes; a hint that
guessed would be the same mistake one level up.

The plugin therefore enables the fast path only under `-DUSE_RTTI=ON`, which is a
supported RocksDB cmake option and not a source patch, and is correct without it.
`scripts/build_rocksdb_plugin.sh` sets it for the Release tree, which builds the
skiplist the same way, so the comparison below is not confounded by it.

## Proving the fallback still exists

The fast path is easy to test by accident and hard to test on purpose: every
existing test draws random keys, where an eight-byte tie is rare, so all of them
would still pass with the fallback deleted and ties resolved by the hint alone.
Deleting it and running them confirmed exactly that — a clean pass.

`tests/test_memtable.cpp` now carries a workload where the tie is universal: 3,000
keys behind a thirteen-byte `tenant:00042:` prefix, which is the shape
`bench/collision_report.cpp` finds in most realistic key distributions.
Every node in that structure has an identical hint, so nothing can be answered by
the fast path. With the fallback removed it fails immediately and loudly. A second,
smaller case covers keys that agree on exactly eight bytes and diverge afterwards,
including the prefix-versus-extension case the zero padding exists for.

On the plugin side, `HintOrderingFollowsTheUserComparator` pins the gate directly:
true for the default bytewise comparator, false for the reverse one. The
differential tests cannot see this — with the fast path wrongly disabled every one
of them still passes, only slower — which is precisely why it needed its own test.

## Results

The numbers are in [../results/phase4-descent.txt](../results/phase4-descent.txt),
and the shape of the experiment matters more than the figures. This host is the
development laptop, which `CLAUDE.md` records as a poor measurement machine, so
rather than compare the tree before against the tree after, both configurations
were built from identical sources differing in `-DUSE_RTTI` alone. That flag
decides whether the fast path compiles in at all, so it isolates exactly one
variable — and the skiplist rows, which are the same in both trees, are the
evidence that the flag does not move throughput by itself.

Everything resident in the memtable, 4 threads, 500,000 keys, WAL off, medians of
three runs:

| | hints off | hints on | change | against the skiplist |
| --- | --- | --- | --- | --- |
| `db_bench` `readrandom` | 4.841 us/op | 2.780 us/op | **-42.6%** | **5.5% faster** |
| `db_bench` `fillrandom` | 11.492 us/op | 8.044 us/op | **-30.0%** | 26.1% slower |
| `memtablerep_bench` read | 1.716 us/op | 1.167 us/op | **-32.0%** | **27.5% faster** |
| `memtablerep_bench` write | 1.640 us/op | 1.351 us/op | -17.6% | 46.4% slower |

The read path is the one this change was aimed at, and it has crossed over: in
RocksDB's own rep harness, which isolates the memtable from the block cache and
the version machinery, Aparajita is now 27.5% faster than the skiplist it replaces.
Phase 3 had it 13% slower on the same measurement. That is the first result in the
project where the design beats the thing it is arguing against on the thing it is
arguing about.

The write path improved too — a write descends before it inserts — but is still
well behind, and the gap is now the entire story.

One note on the benchmark configuration. Phase 3 used a 256 MiB write buffer,
which flushes twice over this workload, so roughly half of `readrandom`'s lookups
never reach the memtable and the memtable's contribution is diluted. The runs
above use 1 GiB and write no SSTs. Both are reported in the results file; the
resident one is the honest measurement of a memtable.

## What is left, and why it is next

Writes are behind because every insert rebuilds an entire node: a fresh 208-byte
payload, sixteen surrogates recomputed, and the shared prefix re-derived from the
first and last key. The skiplist links one node and returns.

The size dependence makes this visible. Going from a 256 MiB write buffer to
1 GiB barely moves the skiplist's `fillrandom` — 6.19 to 6.47 microseconds — while
Aparajita's goes from 5.97 to 8.04. A larger resident memtable means more nodes,
more arena, and a rebuild that keeps churning 208 bytes per insert through a
working set that no longer fits anywhere useful. Copy-on-write bought the atomic
split cheaply in Phase 2 and is charging for it on every write.

That is Phase 4b, and the candidate is an append-only node with a published sort
permutation: sixteen four-bit slot indices fit exactly in one 64-bit word, which is
enough to publish a new order in a single atomic store. An insert becomes a couple
of stores instead of a rebuild. The per-node spinlock is worth revisiting after
that and not before, since its cost only becomes visible once the rebuild stops
dominating it.
