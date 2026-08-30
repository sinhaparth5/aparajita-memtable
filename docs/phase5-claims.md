# Phase 5: what the data supports, before writing it down

The paper draft is in [paper/main.tex](../paper/main.tex). This file is the audit
that came before it: every claim the paper wants to make, marked against the
measurement that backs it. It exists because the cheapest way to find out which
gaps matter is to try to write the claims down and see which ones have nothing
under them.

Three things came out of it that changed the draft.

## The title had to change

The roadmap's working title was "Aparajita: Eliminating CPU Cache Line
Invalidations and Branch Mispredictions in LSM-Tree MemTables via SIMD Alignment".

Branch mispredictions are measured cold. Every vector kernel and every branchless
scalar kernel records 0.0000 mispredictions per probe against 0.5253 for the
branchy scalar baseline, the vector kernels do not move with the hit ratio, and
per-operation branch misses inside RocksDB are 8.46 against the skiplist's 10.39.

Cache line invalidations are not measured at all. That was HITM's job and HITM
came back silently dead, for the reasons in
[docs/phase4-eval.md](phase4-eval.md). A title claiming elimination of a quantity
the paper never reports is the first thing a reviewer will find.

The draft is titled "Branchless SIMD Search and Append-Only Nodes for LSM-Tree
MemTables", which is what the evidence covers. If the HITM measurement is ever
taken on bare metal, the older title becomes available again.

## Two kernel runs disagree, and the roadmap quotes only one

| run | host | equality AVX2 | equality AVX-512 |
| --- | --- | --- | --- |
| `results/counters.txt` | c4-standard-8 | 5.01 | 4.04 |
| `results/phase4-ordered-kernels.txt` §1 | c4-standard-16 | 4.02 | 5.02 |

Which ISA is faster inverts between them, and the IPC column inverts to match
while `ins/probe` stays at 20 and 15 in both. The ordered families reproduce
across those same two runs to within 0.2 cycles, which is what `§3` relies on when
it compares permuted and unpermuted rows across instance sizes, so the equality
row in `§1` is the more likely anomaly.

`ROADMAP.md` and `CLAUDE.md` both say AVX-512 is 20% faster than AVX2, resting on
the first run alone. The paper quotes the ordered rows, which are stable, and makes
no claim about the relative speed of the two ISAs on equality search. Resolving
this properly needs a rerun of the equality family on both shapes; it is not worth
a rented host on its own, but it should ride along with the next trip.

## One result is more awkward than the documents admit

After the permutation was removed, the shipped AVX2 ordered kernel sits at 9.89
cycles per probe against `lb_scalar_brless` at 8.67. SIMD is 14% behind scalar on
the ISA that ships.

`results/phase4-ordered-kernels.txt` states this and then states the mitigation,
which is real: `lb_scalar_brless` reads a sorted node, no scalar branchless kernel
exists for the append-only shape, and the comparison the design actually faces is
9.89 against `perm_scalar`'s 49.49. But a reader who finds the 8.67 row before the
explanation has already formed a view. The paper puts the awkward number first and
the mitigation second.

## The audit table

Marked against `results/`. "Argued" means the reasoning is sound and no counter or
benchmark isolates it.

Six rows changed after the draft was reviewed and the gaps measured; see
[phase5-ordered.md](phase5-ordered.md).

| Claim | Status | Evidence |
| --- | --- | --- |
| Branch mispredictions removed from the probe | measured | 0.0000 miss/probe on every vector kernel, `counters.txt`, `phase4-ordered-kernels.txt` §3 |
| SIMD beats a branchy scalar search per probe | measured | 27.65 to 5.01/4.04 equality; 40.14 to 5.10/4.03 ordered |
| SIMD beats every scalar kernel on the shipped ISA | **not supported** | AVX2 ordered 9.89 against `lb_scalar_brless` 8.67; see above |
| Reads beat the skiplist inside RocksDB | measured | 22-39% at 1/4/16/64, ten runs per cell, non-overlapping |
| Read win is instruction and L1 driven | measured | 3271.5 vs 7349.9 instr/op; 62.18 vs 103.99 L1/op |
| Branch misses per read operation are lower | measured | 8.46 vs 10.39, with the rate inversion stated both ways |
| Cross-core line invalidation is reduced | **unsupported** | HITM not collectable; no proxy counter substitutes |
| Inserts beat the skiplist at the rep level | measured | `memtablerep_bench` 0.5435 vs 0.8040 us/op, holding 48-52% |
| Inserts beat the skiplist in `db_bench` | measured at 1 thread only | +18.8% at 1 thread; tie at 16 and 64 |
| Inserts scale with threads in `db_bench` | **not supported** | neither rep scales; 22,182 instr/op is the write group |
| Arena cost per insert is bounded | measured | 445.4 to 100.6 B/key standalone; about 1.4x skiplist under RocksDB |
| Descent cost is reduced | measured | 30.0 successors per lookup before hints; `phase4-descent.txt` |
| Reads are lock-free | argued, race-checked | one release store by construction; TSan clean at 1/4/16/64 |
| Writes are lock-free | **false as stated** | writers take a per-node spinlock; paper says so |
| Iteration matches the skiplist | measured | differential test, byte for byte, incl. reverse comparator |
| Seek beats the skiplist | measured | +15.1% to +28.6% at 1/4/16/64, `phase5-ordered/` |
| Iteration is *faster* than the skiplist | **false** | Seek + 10 Next is 3.0-4.3% behind at every thread count |
| Arena per key vs the skiplist | measured | 1.40x; 7 L0 files against 5 for the same 2M keys |
| Flushing more often costs flush time | **false** | 0.425 vs 0.441 us per key flushed; the cost is L0 files, not flush |
| Compaction cost of the extra L0 files | **unsupported** | every configuration here disables auto-compaction |
| Six of seven distributions defeat an absolute surrogate | **misstated** | four hold a single value, six are ineffective; corrected everywhere |
| Selectable without patching RocksDB | measured | `db_bench --memtablerep=aparajita` |
| AVX-512 does not downclock for this mix | measured, narrow | `freq/nom` 1.741 across five runs, Emerald Rapids only |
| AVX-512 is 20% faster than AVX2 | **contested** | inverts between runs; see above |
| Thread scaling curve | measured, capped | every point above 12 threads is contention |

## What this means for the hardware trip

Four things now want the same box, which is one rental rather than four:

- HITM, on bare metal or a provider exposing PEBS to guests.
- A scaling curve above 12 physical cores.
- The equality kernel rerun that settles the AVX2 and AVX-512 inversion.
- A `freq/nom` reading on a downclock-prone part, which Phase 1 left open and
  which the AVX-512 availability argument would be stronger for having.

The Phase 5 trip did **not** carry any of them. It was scoped to `db_bench` and ran
no counter passes, so all four are still open and still want one rental between
them. What it closed instead were two gaps this audit had not thought to ask about:
an ordered workload, and arena cost measured against the skiplist rather than
against our own earlier design.

None of them block the draft. All four are stated as limitations in it, and the
draft is written so that closing any of them is an edit rather than a rewrite.
