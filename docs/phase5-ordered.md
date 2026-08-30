# Phase 5: the two measurements the evaluation was missing

A peer review of the Phase 4 draft found two holes that were not arguable, only
measurable, and this is what closing them produced. Both cost the paper something,
which is the reason to record them rather than the reason not to.

`scripts/run_phase5.sh` produces everything here; `results/phase5-ordered/` holds
the raw output and the generated summary. One `c4-standard-24`, the same shape and
zone as the Phase 4 evaluation, deleted the moment the results came back.

## The hole: an ordered design never measured on an ordered workload

The paper justifies ordering against VectorRep on the grounds that a MemTable
iterator has to provide it. Phase 4 then measured `readrandom`, `readwhilewriting`
and `fillrandom` and nothing else. A reviewer asking "so what does a `Seek` cost?"
had nowhere to look.

`seekrandom` at two settings, both over a resident memtable, five repetitions:

| threads | Aparajita | skiplist | vs skiplist |
| --- | --- | --- | --- |
| **Seek alone** (`--seek_nexts=0`) | | | |
| 1 | 428.2 | 333.0 | +28.6% |
| 4 | 1744.3 | 1368.8 | +27.4% |
| 16 | 5688.7 | 4853.0 | +17.2% |
| 64 | 7639.7 | 6636.1 | +15.1% |
| **Seek then ten Next** (`--seek_nexts=10`) | | | |
| 1 | 191.7 | 197.6 | **-3.0%** |
| 4 | 744.1 | 769.6 | **-3.3%** |
| 16 | 2246.6 | 2348.0 | **-4.3%** |
| 64 | 3093.8 | 3217.5 | **-3.8%** |

Throughput in kops/s, median of five.

**A Seek wins and a Next loses, and both follow from the same design decision.**
A Seek is a descent plus a `lower_bound`, which is exactly what the hints and the
kernel were built to make cheap, and it tracks the point-lookup result: ahead
everywhere, narrowing as threads rise.

A `Next` is where the append-only node charges for itself. The skiplist's is one
pointer load along the level-0 chain. Ours decodes the next rank out of the order
word, indexes the slot that nibble names, and follows a key pointer into the arena.
That is three dependent steps against one, and it is not an accident or an
oversight -- it is the same fact that makes an insert one release store. The sorted
order is a computed permutation, not the physical layout, so walking it costs more
than following it would.

At ten Nexts per Seek the walk dominates and the design is 3-4% behind. The margin
is small and the ranges overlap at 1, 4 and 64 threads. At 16 they do not
(2187-2257 against 2289-2389), and the sign is the same at all four points, so this
is recorded as a loss and not a tie.

The obvious next move is left undone deliberately: the order word is already in a
line the iterator has loaded, so a `Next` that decoded several ranks at once would
amortize what is now paid per step. That is a phase, not a patch.

## The hole: arena overhead reported against ourselves, never against the skiplist

`docs/phase4b-append.md` reports 445.4 -> 100.6 bytes per key, which is Aparajita
against its own earlier self. The paper never said what the skiplist charges. Worse,
every Phase 4 configuration ran a 4 GiB write buffer with compaction disabled, so
the overhead could not cost anything even in principle.

Shrinking the write buffer to 64 MiB and letting the same two million keys flush:

|  | fill us/op | L0 files | keys/flush | flush us/key |
| --- | --- | --- | --- | --- |
| Aparajita | 1.031 | 7 | 285,714 | 0.425 |
| skiplist | 1.313 | 5 | 400,000 | 0.441 |

- **arena per key: 1.40x the skiplist.** Independently reproduces the 7-and-5 file
  counts `docs/phase4b-append.md` measured on a different host, which is what a
  property of the structure should do.
- **flush cost per key flushed: 0.96x.** Within noise of parity. The same two
  million keys get written either way, so flushing more often does not mean
  flushing more.
- **fill is 21% faster** at this buffer size, which is the 1-thread insert win
  showing up in the configuration where the insert path is not swamped.

So the arena overhead does not land on flush time. It lands on **L0 file count**:
40% more files for the same data, which is compaction work and write-stall pressure
that every configuration in this project disables. That is now stated as an open
cost in the paper rather than left for a reader to find.

## What this changes in the paper

Both results went in as measurements, and the Seek/Next split went into the
conclusion, because a paper that claims a read win and does not say which reads is
one bad question away from losing the claim.

The honest one-line summary of the design is now: **faster to look a key up,
slightly slower to scan from one, and 1.4x the memory per key.**

## What the review changed that was not a measurement

Four things in the draft were wrong or unsupported on their own evidence, and are
worth recording because three were self-inflicted:

- **"Six of seven distributions collapse to a single surrogate value"** was a
  misreading of `bench/collision_report.cpp`'s own table, repeated in five files.
  Four of the seven hold a literal single value; six are merely ineffective. See
  [phase2.md](phase2.md).
- **The evaluation keyspace was never in that table.** `db_bench_default` is now a
  distribution in `collision_report`, and it collapses: db_bench writes the key
  ordinal big-endian into the first eight bytes, so below 2^21 keys the leading five
  bytes are identical database-wide. Every RocksDB number this project reports is
  measured on a keyspace where an absolute lane would have been useless, which is
  the strongest case for the prefix correction and was being left unstated.
- **The first contribution compared across kernel families**, quoting the ordered
  SIMD rows against the *equality* scalar baseline at 27.65 rather than the ordered
  one at 40.14. This understated the win and was still a category error.
- **The read tables reported no dispersion** and claimed ten samples per read cell
  where the data has five. The ten applies to fill cells, which run twice.
