# Archived: i5-8400H run, 2026-08-27

The original Phase 1 measurements, taken on the development laptop before it was
replaced. Kept because `docs/phase1.md` cites them for the cross-generation
comparison, and because they are the only run of the pre-`freq/nom` harness.

Two reasons not to read these as current. The host was an i5-8400H (Coffee Lake)
with no AVX-512 at all, so the `avx512` row is a skip rather than a measurement.
And it ran under WSL2 on a machine that was not quiet: cycle standard deviations
run from 1.63 to 8.43, against 0.00 to 0.42 for the same kernels on the Xeon.

`counters.txt` here predates the `freq/nom` column, so it has eight columns where
the current report has nine.
