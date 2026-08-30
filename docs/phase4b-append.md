<!-- SPDX-License-Identifier: Apache-2.0 OR MIT -->
# Phase 4b: an insert that allocates nothing

Phase 4a left one gap. Reads had crossed over and beaten the default skiplist; writes were still
27% behind, and the whole of that gap was one line of Phase 2's design: every insert rebuilt the
node it was inserting into.

This phase removes the rebuild. A node became append-only, and the sorted order over its slots
became a 64-bit word that an insert republishes in a single store. `fillrandom` crossed over with
it, so Aparajita is now ahead of the skiplist on both halves of the workload it was written to beat.

## What copy-on-write was charging

The Phase 2 structure kept a node sorted by never modifying one. An insert allocated a fresh
payload, merged the new key into the sorted run, recomputed the shared prefix and all sixteen
surrogates, and published the pointer. That made the publication a single release store, which is
what makes an atomic split possible, and it is why the design chose it.

The bill for it is easiest to read in the arena, because the arena is what RocksDB charges against
`write_buffer_size`:

```
500,000 keys of 16 bytes, standalone structure
   arena consumed          445.4 bytes per key
   of which key bytes       16.0
   of which structure      429.4      -- one 384-byte payload per insert, plus split overhead
   live structure          ~40        -- 44,378 nodes holding 11.3 keys each
```

Ten times more arena than the structure occupies. That is not only a memory number. The 44,378 live
nodes end up scattered through 222 MB of arena rather than packed into 20 MB, so every tower hop
lands on a cold page, and Phase 4a had just finished establishing that a lookup is thirty of those
hops and nothing else. Copy-on-write was paying for the split on every write and charging the read
path for it too.

## The change

A slot is written once and never again. Sorted rank `i` is held in nibble `i` of a 64-bit `order`
word, and republishing that word is the whole of an insert:

```cpp
d->keys[slot]       = key;                       // slot == count, never written before
d->surrogates[slot] = ...;
d->order.store(order_insert(order, rank, slot), std::memory_order_release);
```

Three stores into a node that stays where it is. No allocation, no copy, and nothing that was
already visible moves.

The publication is exactly as atomic as swapping a pointer was, and for the same reason: a reader
takes one acquire load of `order` and everything that word names is frozen. It either sees the new
word and every slot it references, or the old word and the node exactly as it was. That is why
`order` lives inside `NodeData` beside `next` rather than out in `ListNode` beside the tower — a
split changes both at once, and a reader must never see one without the other.

Splits are unchanged. A split does have to change a node's contents and its successor together, so
it still builds fresh payloads and swaps the pointer. It is now the only thing that allocates, and
it happens once per seven or so inserts instead of once per insert.

### The encoding costs one slot, and buys two things with it

A nibble holds `slot + 1`, not the slot, so 0 can mean "this rank is unused". Values 1..15 address
slots 0..14 and a node holds fifteen keys rather than sixteen.

The first thing that buys is a self-describing word. The count is the position of the highest
nonzero nibble, so nothing has to be published alongside the order. A separate count field would
need a second store, and a second store is precisely the torn state the single store exists to
prevent — the same argument that put `next` inside the payload in Phase 2.

The second is free padding. An unused rank decodes to slot 15, which no key ever occupies and which
every node holds at `kEmptyKey`, so a permuted node is sentinel-padded above its last key with no
count passed to the kernel and no tail blend. `lower_bound` keeps returning "above everything here"
by the same popcount that answers everything else.

### The lanes are no longer sorted in memory, so the kernel permutes them

Slot order is insertion order. The sixteen surrogates a search compares are therefore unsorted,
and the Phase 2 result that made ordering affordable — over a sorted node the compare mask is a
prefix of set bits, so its popcount *is* `lower_bound` — needs them sorted.

On AVX-512 that is one instruction. `vpermd` takes exactly the sixteen 32-bit indices the order word
packs, so the sorted line is materialised in a register and the popcount behind it is unchanged.
AVX2 has no 16-lane crossing permute, so each output half is gathered from both source halves and
blended on the index's high bit: four permutes and two blends where AVX-512 uses one instruction.
That is the widest the two ISAs have diverged anywhere in this project, and it is an argument for
AVX-512 that Phase 1 did not have — Phase 1 measured it 20% faster, which was never enough to
require it.

The load is masked to the live slots, and that is about concurrency rather than about the answer. A
writer appending to this node is storing into slot `count` while the kernel runs, and an unmasked
64-byte load would read the bytes it is writing. The permutation could not select that lane, so the
result would be right, but reading it is still a data race. Masking to slots below `count` keeps the
reader off it; the merge source is `kEmptyKey`, which is what those slots already hold, so the mask
cannot change an answer.

### The prefix is frozen, and does not need to be revised

Surrogates are taken past the bytes a node's keys share, which is what makes the lane discriminate
anything on real keyspaces. That prefix used to be recomputed on every insert, which an append-only
node cannot do: shortening it would invalidate every lane already written.

It does not need to be. A node's true shared prefix can only *shrink* as keys arrive — a key landing
inside the range leaves the first and last key alone, and a key extending the range can only shorten
their common prefix — so a frozen prefix is too long, never too short. And a key that does not share
it is not arbitrary: every key in the node agrees with the prefix at every position, so a key that
disagrees at position *j* differs there from all of them in the same direction, and a key shorter
than the prefix is a proper prefix of all of them. Either way it sorts below the node's entire range
or above it, never inside.

So such a key gets a saturated lane — 0 below, `kEmptyKey` above — which keeps the lane array sorted
and leaves the SIMD kernel usable for every other key in the node. Splits recompute the prefix from
scratch, so it sharpens as ranges narrow rather than decaying.

## What the tests can and cannot see

Mutation testing was worth more here than anywhere else in the project, because it found that most
of this change is invisible to every existing test.

`sorted_position` confirms every SIMD candidate against the comparator and falls back to binary
search. That is load-bearing for correctness under a custom comparator, and it also means a
`lower_bound` kernel returning pure noise still produces correct lookups, correct iteration and a
byte-for-byte match against the skiplist. Replacing the AVX-512 permute index with garbage passed
the entire differential suite. So did dropping the saturating lane, and so did deleting the prefix
recomputation at splits. All three are silent collapses back to the performance this project exists
to beat, and nothing in the repository failed.

Two things closed that:

- **The permuted kernels get their own gate.** `tests/test_search.cpp` now checks all three against a
  reference that counts rather than searches, over every occupancy from empty to full, plus the
  order word's own algebra. Four kernel mutations that survived the differential tests fail here.
- **`BasicMemTable::check_invariants` walks the structure** and returns the first invariant broken:
  lanes sorted in rank order, each lane matching the key beside it, the order word a permutation of
  the filled slots, the stored prefix never shorter than the keys' actual common prefix, the descent
  hint matching its node's first key, level 0 in comparator order. `tests/test_memtable.cpp` calls it
  after every workload and `tests/test_concurrent.cpp` after the writers join.

One mutation survives on purpose. Unmasking the load cannot change any result — it is a data race
and nothing else — so no deterministic test can catch it, and ThreadSanitizer cannot either: GCC
instruments the enclosing function but emits `vmovdqa32 (%rbx),%zmm1` with no `__tsan_read` in front
of it. The mask is justified by the memory model, not by a tool that can demonstrate it, and that is
recorded here rather than left as an unexplained line of code.

## Results

Measured on a rented host rather than the laptop, per the rule in CLAUDE.md: a GCP `c4-standard-16`,
Xeon Platinum 8581C (Emerald Rapids), 16 vCPU, Ubuntu 24.04, GCC 14. The instance was created for the
run and deleted immediately after.

It is a controlled comparison. One host, one RocksDB tree, one set of build flags, one uninterrupted
session with nothing else running; the only thing that changed between the two halves of every table
is the contents of `include/aparajita`. The `skip_list` rows are the check on that, and they agree to
within 0.5% on `db_bench`. The same comparison run on the development laptop under WSL2 agrees on
every direction and on most magnitudes, which is the second-host confirmation.

### The arena, which is the point

```
standalone, 500,000 keys          copy-on-write     append-only
   arena per key                     445.4 B           100.6 B      -77%
   insert                           1189.6 ns          717.2 ns     -40%
   lookup                            962.0 ns          770.7 ns     -20%

RocksDB, 2,000,000 keys at a 64 MiB write buffer
   SST files written                    15                 7
   keys resident per memtable      133,000           286,000        2.1x
   (skip_list, both halves)        400,000           400,000
```

The lookup row is worth pausing on: the read path was not touched in this phase. It improved because
the live nodes stopped being scattered through ten times the memory they occupy.

### db_bench, 5 repetitions, 4 threads, 2M ops, WAL off, 1 GiB write buffer

```
                        fillrandom us/op                 readrandom us/op
 copy-on-write
   skip_list     5.673 5.551 5.650 5.677 5.629    1.674 1.680 1.752 1.678 1.678
   aparajita     7.035 7.059 7.018 6.969 7.117    1.440 1.418 1.464 1.463 1.440
 append-only
   skip_list     5.669 5.625 5.659 5.641 5.596    1.733 1.704 1.688 1.750 1.693
   aparajita     5.520 5.468 5.540 5.531 5.534    1.259 1.248 1.251 1.240 1.253

medians              before    after    change     vs skip_list, before -> after
   fillrandom         7.035    5.531    -21.4%      24.5% slower -> 2.0% faster
   readrandom         1.440    1.251    -13.1%      14.2% faster -> 26.6% faster
```

`fillrandom` crossed over, which was the whole objective of the phase.

### memtablerep_bench, 7 repetitions, 200k ops, 4 threads

RocksDB's own rep harness: no SSTs, no block cache, no version machinery.

```
medians              before    after    change     vs skip_list, before -> after
   fillrandom         0.827    0.516    -37.7%       6.1% slower -> 33.2% faster
   readrandom         0.861    0.805     -6.5%      24.3% faster -> 27.5% faster
   readwrite write    0.170    0.194    +14.3%      40.2% faster -> 33.5% faster
   readwrite read     0.510    0.582    +14.3%      40.2% faster -> 33.5% faster
```

### The one thing that got worse

`readwrite` — one thread writing while three read — is 14% slower than it was, and that is the
predicted cost of the change rather than a surprise. Copy-on-write had a property this gives up: a
writer touched only memory no reader could reach, because the payload it was building was not
published yet. An append writes into the live node, so a reader searching that node now shares a
dirty line with the writer filling it.

It reproduces at the same magnitude on both hosts — +14.3% on the Xeon, +13.5% on the laptop — so it
is the change and not the machine. The structure is still a third faster than the skiplist on that
benchmark, so it is recorded rather than paid down. Quantifying it properly wants HITM counters,
which is Phase 4's job and not this one's.

## What is left

The per-node spinlock. ROADMAP.md deferred reconsidering it until the rebuild stopped dominating,
which was the right order: a writer used to hold that lock across an allocation, a merge and sixteen
surrogate computations, and now holds it across three stores. Whether a CAS on the order word can
replace it is a live question that this phase deliberately did not answer, because changing the
publication protocol and the locking discipline in one step would leave neither measured.

Node capacity is fifteen, so a split now yields two eight-key halves out of a sixteen-key merge.
Whether 4-bit ranks over sixteen slots can be recovered — the encoding needs seventeen values and
has sixteen — is a real question with a real answer available (count the zero nibbles), and it was
rejected here for costing more in explanation than one slot in sixteen is worth.
