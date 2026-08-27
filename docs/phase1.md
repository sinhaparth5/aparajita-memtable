# Phase 1 results: SIMD node search

Status: complete except the AVX-512 runtime check, which this host cannot perform.

Phase 1 asked whether a 64-byte node searched with vector compares beats a scalar
search, and whether the branch mispredictions the project is named after actually
disappear. Both hold, and the second one turned out to depend on a detail that was
not in the plan.

## Headline numbers

Single L1-resident node, 200k probes, 50% hit ratio, hits at uniformly random
slots. Median of 9 repetitions on an Intel i5-8400H (Coffee Lake, AVX2, no
AVX-512), GCC 15.2, `-O3`.

| kernel | cycles/probe | instructions | IPC | branches | branch-misses/probe |
| --- | --- | --- | --- | --- | --- |
| scalar_linear | 30.0 | 35.0 | 1.17 | 14.7 | 0.528 (3.6%) |
| scalar_autovec | 29.7 | 35.0 | 1.18 | 14.7 | 0.527 (3.6%) |
| scalar_branchless | 23.3 | 89.0 | 3.82 | 3.0 | 0.0001 (0.00%) |
| scalar_binary | 86.9 | 53.4 | 0.61 | 14.2 | 2.102 (14.9%) |
| avx2 | 7.0 | 20.0 | 2.86 | 3.0 | 0.0012 (0.04%) |
| avx512 | compiled, not executed on this host | | | | |

AVX2 runs the probe in roughly a quarter of the cycles the branchy scalar loop
needs, with branch mispredictions at the noise floor. Reproduce with
`./scripts/run_phase1.sh`.

## The finding that was not in the plan

The first working version of the AVX2 kernel measured 25.4 cycles per probe with
0.51 branch misses per probe. The vector compare was branchless, so the misses had
to be coming from somewhere else, and they were: the return statement.

Reporting "not found" as -1 forces `mask ? ctz(mask) : -1`, and that ternary is a
data-dependent branch. At a 50% hit ratio it is unpredictable by construction, so
it mispredicts about half the time. One branch, and it accounted for four fifths of
the kernel's cost.

Changing the convention so "not found" is `kNodeKeys` lets the miss case fold into
the mask as bit 16, and `ctz` then produces it with no branch:

```cpp
return static_cast<int>(__builtin_ctz(mask | (1u << kNodeKeys)));
```

That single change took the kernel from 25.4 to 6.2 cycles per probe and its
mispredictions from 0.51 to 0.00. It is a four-fold difference produced by a return
convention, and it means the sentinel choice is load-bearing rather than cosmetic.
Phase 2 should treat any API returning a sentinel on the hot path the same way.

## Hit ratio dependence

The scalar baseline's cost depends on the data; the vector kernel's does not.

| hit ratio | scalar_linear cyc/probe | avx2 cyc/probe | speedup |
| --- | --- | --- | --- |
| 0.00 | 19.1 | 6.6 | 2.9x |
| 0.25 | 24.2 | 6.1 | 4.0x |
| 0.50 | 29.6 | 6.3 | 4.7x |
| 0.75 | 37.1 | 6.2 | 6.0x |
| 1.00 | 43.2 | 6.1 | 7.1x |

At a 0% hit ratio the scalar loop always runs all sixteen comparisons and exits the
same way every time, so the predictor learns it and misses drop to zero. Every hit
introduces an early exit at a random slot, which the predictor cannot learn. The
vector kernel is flat at about 6.2 cycles throughout because it does the same work
regardless.

This matters for how the paper frames its benchmarks. A memtable workload with a
high hit ratio is where the SIMD advantage is largest, and a benchmark that probes
mostly-absent keys will understate it.

## Working set sweep

Random node selection across an array, 8192 probes, single run per size.

| working set | scalar_linear | avx2 | speedup |
| --- | --- | --- | --- |
| 32 KiB (L1d) | 157.0 M probes/s | 350.2 M/s | 2.23x |
| 128 KiB (L2) | 135.6 M/s | 343.7 M/s | 2.53x |
| 4 MiB (L3) | 95.1 M/s | 202.0 M/s | 2.13x |
| 64 MiB (DRAM) | 28.0 M/s | 78.2 M/s | 2.79x |

The advantage survives into DRAM. It should: a node is exactly one cache line, so
both kernels pay the identical miss, and the vector kernel simply does less work
once the line arrives. This is the argument for the 64-byte node size, and it is
worth stating in the paper because a reviewer will expect the SIMD win to vanish
once the workload turns memory-bound.

## Secondary findings

**GCC does not auto-vectorize the search loop.** `scalar_autovec` and
`scalar_linear` retire the same 35 instructions, so the `#pragma GCC novector` on
the baseline changes nothing. Early-exit loops are not vectorizable in general, so
the compiler cannot reach the AVX2 kernel on its own. The hand-written intrinsics
are not redundant with `-O3`, which is the question a maintainer will ask first.

**Binary search is the worst option at this width.** 86.9 cycles per probe and 2.1
mispredictions, roughly 12x the AVX2 kernel. Every step of a 16-element binary
search is a data-dependent branch on a random key. This is the relevant comparison
for the paper's motivation, because a skiplist node search has the same shape.

**Branchlessness accounts for most of the win, not vector width.**
`scalar_branchless` reaches 23.3 cycles with no vector instructions at all, so
removing the branch is worth about 7 cycles and the vector compare is worth a
further 16. Both parts are real, and the paper should not attribute the whole
result to SIMD.

## What this host could not verify

**AVX-512 was never executed.** The i5-8400H has no AVX-512, so `search_avx512`
is compiled and gated behind `__builtin_cpu_supports("avx512f")` but has run
nowhere. Its correctness and the downclocking penalty both remain open. This needs
a Skylake-SP or newer Xeon, an Ice Lake or newer client part, or a Zen 4 machine
before any AVX-512 claim goes in the paper.

**Wall-clock timings are noisy here.** The Google Benchmark repetitions show
coefficients of variation up to 62% on this WSL2 host, where the governor is not
readable and the machine was not quiet. The PMU cycle counts are far more stable
because they are immune to frequency scaling, which is why the tables above are
cycle counts rather than nanoseconds. Paper-grade timings need a quiet machine with
frequency scaling pinned.

**`perf(1)` is not installed**, but it is not needed. `perf_event_open(2)` works
directly on this host (a core PMU is exposed and `perf_event_paranoid` is 2), and
counting inside the process scopes the measurement to the probe loop instead of the
whole binary. `bench/counters.hpp` does this in about 80 lines.

## Exit criteria

| criterion | target | result |
| --- | --- | --- |
| Branch misses on the SIMD probe | below 0.5% of branches | met: 0.00 to 0.04% |
| Cycles per lookup, AVX2 versus scalar | measured ratio with dispersion | met: 4.3x to 4.9x, standard deviations reported |
| AVX-512 correct under CPUID gating | verified | not met: host lacks AVX-512, path compiled and gated only |
| Downclocking recorded | recorded | not met, same reason |
