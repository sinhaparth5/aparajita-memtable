# Phase 1 results: SIMD node search

Status: complete. The AVX-512 path, which the previous revision of this document
could not execute, has now been measured.

Phase 1 asked whether a 64-byte node searched with vector compares beats a scalar
search, and whether the branch mispredictions the project is named after actually
disappear. Both hold. Two things that were not in the plan came out of it: the
return convention turned out to dominate the AVX2 kernel, and AVX-512 turned out
not to downclock at all for this instruction mix.

## Environments

Two hosts appear below, and they are not interchangeable.

| | primary | archived |
| --- | --- | --- |
| CPU | Intel Xeon Platinum 8581C @ 2.30 GHz (Emerald Rapids) | Intel i5-8400H @ 2.50 GHz (Coffee Lake) |
| AVX-512 | yes | no |
| Environment | ephemeral GCP `c4-standard-8`, guest PMU | WSL2 on a laptop |
| Cycle sd across kernels | 0.00 to 0.42 | 1.63 to 8.43 |
| Results | `results/` | `results/archive-i5-8400h/` |

The Xeon is the primary host for every number in this document. The laptop run is
kept because it is the only pre-AVX-512 data point, but its dispersion is an order
of magnitude worse and it should not be quoted as a measurement.

Reproduce with `./scripts/run_phase1.sh`. The counters need
`/proc/sys/kernel/perf_event_paranoid` at 2 or lower.

## Headline numbers

Single L1-resident node, 200k probes, 50% hit ratio, hits at uniformly random
slots. Median of 9 repetitions.

| kernel | cycles/probe | cyc sd | instructions | IPC | branches | branch-misses/probe |
| --- | --- | --- | --- | --- | --- | --- |
| scalar_linear | 27.65 | 0.09 | 34.5 | 1.25 | 15.24 | 0.525 (3.45%) |
| scalar_autovec | 27.75 | 0.05 | 34.5 | 1.24 | 15.24 | 0.530 (3.48%) |
| scalar_branchless | 16.53 | 0.03 | 90.0 | 5.45 | 3.00 | 0.0000 (0.00%) |
| scalar_binary | 70.24 | 0.42 | 51.4 | 0.73 | 13.16 | 1.910 (14.51%) |
| avx2 | 5.01 | 0.00 | 20.0 | 3.99 | 3.00 | 0.0000 (0.00%) |
| avx512 | 4.04 | 0.01 | 15.0 | 3.72 | 3.00 | 0.0000 (0.00%) |

AVX-512 runs the probe in 4.04 cycles against the branchy scalar loop's 27.65, a
6.8x reduction, with branch mispredictions at zero rather than merely low. AVX2
reaches 5.01. The gap between the two vector kernels is 20%, and it comes from
instruction count: one 512-bit compare and one mask move replace two 256-bit
compares, two movemasks, a shift and an or, which is 15 retired instructions
against 20.

## AVX-512 does not downclock here

This was Phase 1's one unmet exit criterion, and the answer is a clean negative.

The report now carries a `freq/nom` column, computed as
`PERF_COUNT_HW_CPU_CYCLES / PERF_COUNT_HW_REF_CPU_CYCLES`. Core cycles track the
actual clock; reference cycles tick at the invariant rate. Their ratio is
effective frequency as a multiple of nominal, so a part that clocks down under
512-bit work shows it as a fall in that ratio. This is the only frequency-visible
instrument in the harness, because every other counter here is deliberately
immune to frequency scaling.

Across the five hit-ratio runs:

| kernel | freq/nom, five runs |
| --- | --- |
| avx512 | 1.741, 1.741, 1.741, 1.741, 1.741 |
| avx2 | 1.740, 1.741, 1.740, 1.740, 1.741 |
| scalar_linear | 1.740, 1.716, 1.655, 1.740, 1.653 |

AVX-512 is not merely as stable as AVX2, it is the most stable kernel measured,
pinned at 1.741 in every run. The dispersion that exists belongs to the *scalar*
kernels, which is the opposite of what a downclocking story predicts.

Two isolated low readings did occur, 1.436 on `avx512` in `results/counters.txt`
and 1.436 on `scalar_branchless` at a 0% hit ratio. Because the same value lands
on a kernel with no vector instructions at all, it is host power management rather
than an instruction-mix effect, and the guest cannot see or pin the governor on a
cloud VM. They are reported rather than dropped, but they do not support a
downclock claim.

The likely reason for the null result is that this kernel's AVX-512 usage is
`vmovdqa64`, `vpcmpeqd` and a mask move: light integer work, the mildest class
Intel's frequency licensing distinguishes. The severe penalty documented on
Skylake-SP and Cascade Lake attaches to sustained 512-bit floating-point and FMA,
which this kernel never issues.

Two limits on the claim. It was measured on Emerald Rapids, the generation where
Intel had already largely removed the penalty, so it says nothing about Skylake-SP
or Cascade Lake. And a shared cloud VM is a poor frequency instrument by
construction. Confirming the negative on a downclock-prone part, on bare metal,
would close the question properly; until then the honest statement is that no
downclock is observable for this instruction mix on this generation.

## Hit ratio dependence

The scalar baseline's cost depends on the data; neither vector kernel's does.

| hit ratio | scalar_linear | avx2 | avx512 | avx512 speedup | scalar misses/probe |
| --- | --- | --- | --- | --- | --- |
| 0.00 | 10.02 | 5.01 | 4.01 | 2.5x | 0.000 |
| 0.25 | 19.15 | 5.01 | 4.01 | 4.8x | 0.260 |
| 0.50 | 27.72 | 5.01 | 4.02 | 6.9x | 0.524 |
| 0.75 | 35.36 | 5.01 | 4.01 | 8.8x | 0.804 |
| 1.00 | 40.05 | 5.01 | 4.01 | 10.0x | 1.008 |

Both vector kernels are flat to within 0.01 cycles across the entire range. The
scalar loop varies four-fold, and its misprediction count rises linearly with the
hit ratio: at 0% hits it runs all sixteen comparisons and exits identically every
time, so the predictor learns it perfectly, and every hit thereafter introduces an
early exit at a random slot that it cannot learn.

The framing consequence for the paper is unchanged from the earlier run and now
stronger. A memtable workload with a high hit ratio is where the SIMD advantage is
largest, and a benchmark probing mostly-absent keys understates it by a factor of
four.

## Working set sweep

Random node selection across an array, 8192 probes, throughput in millions of
probes per second.

| working set | scalar_linear | avx2 | avx512 | avx512 speedup |
| --- | --- | --- | --- | --- |
| 32 KiB (L1d) | 479.5 | 957.3 | 1199.4 | 2.50x |
| 128 KiB (L2) | 366.7 | 808.5 | 1173.8 | 3.20x |
| 4 MiB (L3) | 251.7 | 671.6 | 1028.7 | 4.09x |
| 64 MiB (DRAM) | 164.8 | 325.3 | 298.9 | 1.81x |

The advantage survives into DRAM but the AVX-512 lead over AVX2 does not: at 64 MiB
AVX-512 is marginally *slower* than AVX2, 298.9 against 325.3. That is the expected
shape. A node is exactly one cache line, so all three kernels pay an identical miss,
and once the workload is bound on line fills the width of the compare stops
mattering. The residual difference at that size is noise plus prefetch behavior, not
a property of the kernel.

This is the argument for the 64-byte node, and it is worth stating in the paper,
because a reviewer will expect the SIMD win to vanish entirely once the workload
turns memory-bound. It narrows; it does not vanish.

## The finding that was not in the plan

Carried forward from the i5-8400H run, where it was discovered, because it changed
the design rather than merely measuring it.

The first working version of the AVX2 kernel measured 25.4 cycles per probe with
0.51 branch misses. The vector compare was branchless, so the misses had to be
coming from somewhere else, and they were: the return statement.

Reporting "not found" as -1 forces `mask ? ctz(mask) : -1`, and that ternary is a
data-dependent branch. At a 50% hit ratio it is unpredictable by construction, so
it mispredicts about half the time. One branch, and it accounted for four fifths of
the kernel's cost.

Changing the convention so "not found" is `kNodeKeys` lets the miss fold into the
mask as bit 16, and `ctz` then produces it with no branch:

```cpp
return static_cast<int>(__builtin_ctz(mask | (1u << kNodeKeys)));
```

That single change took the kernel from 25.4 to 6.2 cycles per probe and its
mispredictions from 0.51 to 0.00. A four-fold difference produced by a return
convention. Any Phase 2 hot-path API reporting absence through a sentinel should
use the same trick.

## Secondary findings

**GCC does not auto-vectorize the search loop.** `scalar_autovec` and
`scalar_linear` retire the same 34.5 instructions and land within 0.1 cycles of
each other, so the `#pragma GCC novector` on the baseline changes nothing.
Early-exit loops are not vectorizable in general, so the compiler cannot reach the
AVX2 kernel on its own. The hand-written intrinsics are not redundant with `-O3`,
which is the question a maintainer will ask first.

**Branchlessness is not free, and its value depends on the hit ratio.** This
reverses on Emerald Rapids relative to the laptop. `scalar_branchless` is flat at
16.53 cycles at every hit ratio, because it always does the same work. That makes
it 1.7x *slower* than `scalar_linear` at a 0% hit ratio, where the branchy loop is
perfectly predicted, and 2.4x faster at a 100% hit ratio, where the branchy loop is
not. It retires 90 instructions to reach that, against 34.5. Removing a branch is
worth paying for only when the branch is actually unpredictable.

**Binary search is the worst option at this width**, at 70.2 cycles and 1.9
mispredictions, roughly 17x the AVX-512 kernel. Every step of a 16-element binary
search is a data-dependent branch on a random key. This is the relevant comparison
for the paper's motivation, because a skiplist node search has the same shape.

**The measurement environment matters more than expected.** Cycle standard
deviations fell from a range of 1.63-8.43 on the WSL2 laptop to 0.00-0.42 on the
Xeon, and several kernels now report 0.00 across nine repetitions. Conclusions that
were defensible before are now tight enough to quote.

## Exit criteria

| criterion | target | result |
| --- | --- | --- |
| Branch misses on the SIMD probe | below 0.5% of branches | met: 0.00% for both avx2 and avx512 |
| Cycles per lookup, SIMD versus scalar | measured ratio with dispersion | met: 5.5x (avx2) and 6.9x (avx512) at a 0.5 hit ratio, sd <= 0.09 |
| AVX-512 correct under CPUID gating | verified | met: executes and passes the kernel test suite |
| Downclocking recorded | recorded | met: none observed, freq/nom 1.741 in all five runs; see the caveats above |
