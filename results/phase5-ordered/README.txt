Provenance for docs/phase5-ordered.md and the Phase 5 rows in paper/main.tex.

environment.txt  host, kernel, compiler, RocksDB tag, parameters
summary.txt      scripts/phase5_summarize.py's output over raw/; every table in
                 docs/phase5-ordered.md and paper/main.tex is copied from here
                 rather than retyped
raw/seek.*       one db_bench process per (seek_nexts, rep, threads, repetition),
                 fillrandom chained into seekrandom so the seeks run against the
                 memtable the fill just populated
raw/flush.*      the same fill against a 64 MiB write buffer with --statistics on,
                 which is where rocksdb.db.flush.micros and the L0 file count come
                 from

The raw files are committed here, unlike Phase 4's, because they are small once
db_bench's "... finished N ops" progress lines are stripped -- which is the only
edit made to them. Every header, option dump, result line, statistics block and
sst_files count is as db_bench wrote it, and phase5_summarize.py reparses these
files unchanged.
