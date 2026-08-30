Provenance for results/phase4-evaluation.txt.

summary.txt              scripts/phase4_summarize.py's output over the run; the
                         tables in results/phase4-evaluation.txt and
                         docs/phase4-eval.md are copied from here, not retyped.
environment.txt          host, kernel, compiler, RocksDB tag, commit, parameters
counter-availability.txt which PMU events resolved on the host, and which did not
counter-points.txt       the two operation counts each counter row is differenced
                         between
raw/perf.*.csv           the perf stat output every counter figure derives from,
                         two points per rep per benchmark (.hi and .lo)

The bulky per-run db_bench and memtablerep_bench logs are not committed -- they
are 7.7 MB of mostly progress output. ./scripts/run_phase4.sh regenerates them,
which is what the reproducibility exit criterion asks for.
