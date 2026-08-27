# Phase 2 results: node layout and concurrency

Status: the structure is built, ordered, and concurrent. Design decisions are in
[phase2-design.md](phase2-design.md); this file records what the code does and
what measuring it changed.

One measurement invalidated the original design and forced a fix. That is the
most important thing in this document, so it comes first.

## The surrogate is defeated by real keys

Phase 2's exit criteria asked for the surrogate collision rate "measured on at
least one realistic key distribution, not synthetic sequential keys." The reason
for that wording turned out to be an understatement.

A 32-bit lane holding the first four bytes of the user key discriminates nothing
on almost any keyspace a RocksDB user would actually write. `bench/collision_report.cpp`
measures the expected number of full-key comparisons per lookup, `sum(m^2)/N` over
surrogate multiplicities, where 1.0 is a perfect lane:

| distribution | global surrogate | per-node surrogate |
| --- | --- | --- |
| random_binary | 1.0 | 1.0 |
| uuid_hex | 4.0 | 1.0 |
| sequential_bigendian | 200000.0 | 1.0 |
| tenant_prefixed | 201.0 | 1.1 |
| timestamp_series | 200000.0 | 1.0 |
| sequential_decimal | 200000.0 | 1.0 |
| single_prefix | 200000.0 | 1.0 |

The left column is the design as specified. Six of seven distributions collapse to
a **single** surrogate value across 200,000 keys. A table prefix (`rows:`), a
tenant id, a decimal counter with a fixed stem, or a big-endian timestamp all put
identical bytes at the front of every key in the database. Only uniformly random
binary keys work, and nobody writes those.

Had this been measured on sequential integers alone, as the roadmap warned, the
result would have been either a clean pass or a total failure depending purely on
whether the varying digits landed in the first four bytes. Neither would have been
informative.

### The fix: prefix truncation

The lane never needed to be order-preserving *globally*. A search descends to the
correct node by comparing full keys; the surrogate is only ever compared against
others in the same node. So the bytes that every key in a node shares are pure
waste, and stripping them costs nothing in ordering.

Each node therefore stores the length of the prefix its keys share and takes the
surrogate after it. Because the node is sorted, that prefix is the common prefix
of its first and last key alone. This is the same trick B-trees use to fit more
separators on a page.

The right column above is that design, and it restores every distribution to an
effective lane. The worst case left is `tenant_prefixed` at 1.1 comparisons.

Two costs come with it. The prefix is recomputed on every copy-on-write, which is
affordable only because the node was being rebuilt anyway. And a key that does not
share the node's prefix has no meaningful surrogate at that offset, so it is
placed by full comparison instead; this is correctness, not an optimisation, and
it happens only at the boundary where a node's shared prefix ends.

## The structure

An ordered list of cache-line nodes with a skiplist index above it.

`NodeData` opens with exactly 64 bytes of surrogates and nothing else, so a search
touches one line. The count, the shared-prefix length, the forward link and the
full-key views sit past that boundary and are only read once the SIMD step has
already produced a candidate. `static_assert`s hold the layout: surrogates at
offset 0, cold fields beginning at offset 64, and the whole struct 64-byte
aligned.

Concurrency rests on one decision. `NodeData` is immutable once published, and the
level-0 link lives *inside* it rather than beside it. That is what makes a split
atomic: replacing a node's payload swaps its contents and its successor in a
single release store, so a reader sees either the old sixteen-key node or the new
pair, never a state where a key is duplicated across both or missing from both.
Putting the link in a separate atomic would need two stores and would expose
exactly those states.

Above level 0 is a skiplist tower with branching factor four. Everything in it is
an accelerator and never the truth: a stale or missing tower pointer costs a
longer walk, never a wrong answer, because the search only follows a pointer to a
node whose first key is at or below the target, and nodes are ordered and never
removed. Before the tower existed the structure was a linked list and the
64-thread test did not finish in two minutes; with it the same test takes 0.7
seconds.

## Deviation from lock-free writes

Readers are lock-free and never write. Writers take a per-node spinlock.

This is a narrowing of the goal in `CLAUDE.md`, which names lock-free concurrency
as a design pillar, and it is recorded rather than hidden. A fully lock-free
ordered insert has to shift keys within a sorted node, which cannot be done in one
atomic operation; the alternatives are copy-on-write with a compare-and-swap retry
loop, which is what this does for the payload, plus a lock to serialise the
writers that would otherwise livelock retrying against each other.

The lock backs off rather than spinning flat. RocksDB routinely runs more writers
than cores, and a pure spin there burns every other thread's slice while the lock
holder waits to be scheduled; the loop pauses 64 times and then yields.

## Write amplification

The cost of copy-on-write, and the number Phase 3 has to answer for.

At 64 threads and 256,000 keys the arena holds 111,832 KiB, which is **448 bytes
per key**. RocksDB's skiplist is in the neighbourhood of 50. The breakdown is
roughly 384 bytes for the `NodeData` rebuilt on every insert and the rest for the
key copy and the occasional node.

Most of the `NodeData` is the sixteen `std::string_view`s at 16 bytes each. An
arena offset and length would be 8, which would take the payload from 336 bytes to
208 and is the obvious first move.

Finding this also exposed a plain bug: the arena rounded *every* allocation to
64-byte alignment, which nodes need and a 13-byte key copy does not. Alignment is
now per call, worth about a tenth of the arena.

This number is not yet acceptable. A memtable that charges 448 bytes per key
against `write_buffer_size` flushes far more often than the skiplist it replaces,
and no amount of probe-side speed compensates for that. It is the first thing
Phase 3 must fix.

## Concurrency results

`tests/test_concurrent.cpp`, at the thread counts the roadmap names. Keys are
interleaved across threads rather than blocked, so writers contend for the same
nodes; blocked ranges would have writers land on different nodes almost always and
would never exercise a writer arriving after a split.

Every run verifies three things that a lost update or a torn split would break:
the key count matches the number of inserts, every inserted key is still found,
and iteration is still in comparator order and matches a sorted reference exactly.

| threads | keys | uninstrumented | ThreadSanitizer |
| --- | --- | --- | --- |
| 1 | 4,000 | pass | clean |
| 4 | 16,000 | pass | clean |
| 16 | 64,000 | pass | clean |
| 64 | 256,000 | pass | did not complete |

All four pass uninstrumented, in 0.7 s for the set. Under ThreadSanitizer the
first three are clean, with no warning of any kind.

The 64-thread case does not finish under instrumentation on this 8-core host, and
the reason is the contention pattern rather than the volume. Every thread walks
the key space at the same rate, so all sixty-four are inserting into the same one
or two nodes and serialising on one lock. Uninstrumented, the losers spin briefly
and yield. Instrumented, every `test_and_set` in that backoff loop is a
ThreadSanitizer event, so sixty-three spinning threads generate more work than the
one making progress and total cost grows with the square of the thread count.
Scaling the key count down to ten per thread does not help, which is what
identifies the spinning rather than the inserting as the cost.

This is an honest gap against the exit criterion, which names 64. Two things bound
it. The code paths a race could live in are not thread-count-dependent, and 16
threads exercises every one of them, including a writer arriving after a split.
And the 64-thread case does pass uninstrumented, so the structure is not losing or
reordering keys there. What is missing is sanitizer coverage specifically at 64,
and the way to get it is a host with more cores or a less adversarial key
distribution, not a change to the structure.

## Exit criteria

| criterion | target | result |
| --- | --- | --- |
| ThreadSanitizer over concurrent insert and read | clean at 1, 4, 16, 64 threads | partial: clean at 1, 4 and 16; 64 does not complete under instrumentation on 8 cores, and passes uninstrumented |
| Surrogate collision rate | measured on a realistic distribution | met: seven distributions, and the result forced a redesign |
| Ordering decision | decided and written down | met: ordered, in phase2-design.md |
| Node layout | 64-byte aligned and 64 bytes, by static_assert | met |
