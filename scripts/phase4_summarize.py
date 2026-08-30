#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Parses the raw output run_phase4.sh leaves in results/phase4-eval/raw and prints
# the tables the phase reports. Nothing here rounds by hand or retypes a number:
# every figure in docs/phase4-eval.md should be traceable to a line this prints,
# and that line traceable to a file in raw/.

import csv
import os
import re
import statistics
import sys
from collections import defaultdict

OUT = sys.argv[1] if len(sys.argv) > 1 else "results/phase4-eval"
RAW = os.path.join(OUT, "raw")

# fillrandom   :       5.531 micros/op 180799 ops/sec 11.06 seconds 2000000 operations;
BENCH_RE = re.compile(
    r"^(\w+)\s*:\s*([\d.]+)\s+micros/op\s+(\d+)\s+ops/sec.*?([\d]+)\s+operations"
)
SST_RE = re.compile(r"^sst_files:\s*(\d+)")
NAME_RE = re.compile(r"^(?P<tag>[a-z]+)-r(?P<rep>\d+)\.(?P<mrep>\w+)\.t(?P<threads>\d+)\.txt$")
MTREP_RE = re.compile(r"^mtrep-r(?P<rep>\d+)\.(?P<mrep>\w+)\.t(?P<threads>\d+)\.txt$")

REP_ORDER = ["aparajita", "skip_list", "vector"]
MTREP_ORDER = ["aparajita", "skiplist", "vector"]


def med(xs):
    return statistics.median(xs) if xs else None


def fmt(x, width=9, prec=3):
    return "n/a".rjust(width) if x is None else f"{x:{width}.{prec}f}"


def load_db_bench():
    """(tag, memtablerep, threads, benchmark) -> {'us': [...], 'ops': [...], 'sst': [...]}"""
    data = defaultdict(lambda: defaultdict(list))
    incomplete = set()
    for fn in sorted(os.listdir(RAW)) if os.path.isdir(RAW) else []:
        m = NAME_RE.match(fn)
        if not m:
            continue
        path = os.path.join(RAW, fn)
        with open(path, errors="replace") as fh:
            body = fh.read()
        if "TIMED OUT" in body or "did not complete" in body:
            incomplete.add((m["tag"], m["mrep"], int(m["threads"])))
        for line in body.splitlines():
            b = BENCH_RE.match(line.strip())
            if b:
                key = (m["tag"], m["mrep"], int(m["threads"]), b.group(1))
                data[key]["us"].append(float(b.group(2)))
                data[key]["ops"].append(int(b.group(3)))
            s = SST_RE.match(line.strip())
            if s:
                data[(m["tag"], m["mrep"], int(m["threads"]), "_sst")]["sst"].append(int(s.group(1)))
    return data, incomplete


def collapse(data, tag, bench):
    """Merge the per-repetition tags (scale-r1..r5) into one series per rep/threads."""
    out = defaultdict(lambda: {"us": [], "ops": [], "sst": []})
    for (t, mrep, threads, b), vals in data.items():
        if not t.startswith(tag):
            continue
        if b == "_sst":
            out[(mrep, threads)]["sst"] += vals["sst"]
        elif b == bench:
            out[(mrep, threads)]["us"] += vals["us"]
            out[(mrep, threads)]["ops"] += vals["ops"]
    return out


def table(data, tag, bench, title, note=""):
    series = collapse(data, tag, bench)
    if not any(v["us"] for v in series.values()):
        return
    threads = sorted({t for (_, t) in series})
    print(f"\n== {title} ==")
    if note:
        print(note)
    print(f"{'threads':>9} " + " ".join(f"{r:>14}" for r in REP_ORDER) + "   aparajita vs skip_list")
    print(f"{'':>9} " + " ".join(f"{'kops/s':>14}" for _ in REP_ORDER))
    for t in threads:
        cells, ap, sk = [], None, None
        for r in REP_ORDER:
            v = med(series.get((r, t), {}).get("ops", []))
            cells.append("n/a".rjust(14) if v is None else f"{v/1000:14.1f}")
            if r == "aparajita":
                ap = v
            if r == "skip_list":
                sk = v
        ratio = f"{(ap/sk - 1)*100:+7.1f}%" if ap and sk else "      -"
        print(f"{t:>9} " + " ".join(cells) + f"   {ratio}")

    print(f"\n{'threads':>9} " + " ".join(f"{r:>14}" for r in REP_ORDER))
    print(f"{'':>9} " + " ".join(f"{'us/op':>14}" for _ in REP_ORDER))
    for t in threads:
        cells = []
        for r in REP_ORDER:
            v = med(series.get((r, t), {}).get("us", []))
            cells.append("n/a".rjust(14) if v is None else f"{v:14.3f}")
        print(f"{t:>9} " + " ".join(cells))

    # Residency. A row measured against a memtable that flushed is not a
    # measurement of the memtable, so this is printed beside the numbers rather
    # than checked once and forgotten.
    # Closing the DB flushes the memtable, so one SST is the shutdown flush and
    # carries no information. Two or more means the buffer filled mid-run.
    bad = [(r, t, max(v["sst"])) for (r, t), v in sorted(series.items())
           if v["sst"] and max(v["sst"]) > 1]
    if bad:
        print("\n  !! the write buffer filled mid-run here, so these rows were not")
        print("     served by the memtable alone and should not be quoted:")
        for r, t, n in bad:
            print(f"     {r} t={t}: {n} SST files (one is the flush on close)")
    else:
        print("\n  memtable-resident: no run produced more than the flush on close")


def scaling(data, tag, bench, title):
    """Throughput relative to the same rep's single-thread point."""
    series = collapse(data, tag, bench)
    threads = sorted({t for (_, t) in series})
    if not threads:
        return
    print(f"\n== {title}: scaling relative to 1 thread ==")
    print(f"{'threads':>9} " + " ".join(f"{r:>14}" for r in REP_ORDER))
    for t in threads:
        cells = []
        for r in REP_ORDER:
            v = med(series.get((r, t), {}).get("ops", []))
            base = med(series.get((r, 1), {}).get("ops", []))
            cells.append("n/a".rjust(14) if not (v and base) else f"{v/base:13.2f}x")
        print(f"{t:>9} " + " ".join(cells))


def read_perf(path):
    if not os.path.exists(path):
        return None
    counts = {}
    with open(path, errors="replace") as fh:
        for row in csv.reader(fh):
            if not row or row[0].startswith("#") or len(row) < 3:
                continue
            try:
                counts[row[2]] = float(row[0])
            except ValueError:
                continue  # <not counted> / <not supported>
    return counts or None


def counters(total_ops, counter_threads, reads_hi, reads_vec_hi):
    print(f"\n== hardware counters, {counter_threads} threads ==")
    print("Each row is the difference between two runs of the same benchmark that")
    print("differ only in how many operations they perform. Both do an identical")
    print("fill at an identical seed, so process setup, the fill, and the flush on")
    print("close cancel exactly rather than approximately. The figure is therefore")
    print("the marginal cost of one operation, not an average over the process.")
    print("Absolute counts for both points are in raw/.")

    for rep in REP_ORDER:
        print(f"\n  {rep}")
        print(f"    {'benchmark':>16} {'ops differenced':>16} {'instr/op':>10} "
              f"{'branch miss/op':>15} {'branch miss %':>14} {'L1 miss/op':>11} "
              f"{'HITM/op':>10} {'dirty %':>9}")
        # These reproduce run_phase4.sh's integer arithmetic exactly, including
        # its rounding. --reads and --writes are per thread, so a total that does
        # not divide by the thread count is not the total that ran, and dividing
        # a counter by an op count that is 12% wrong is a silent 12% error in
        # every figure on the row.
        ct = counter_threads
        w_hi = total_ops // ct
        w_lo = w_hi // 10
        if rep == "vector":
            r_hi = max(reads_vec_hi // ct, 1)
            r_lo = max(r_hi // 10, 1)
            if r_lo == r_hi:
                r_lo = 0
        else:
            r_hi = reads_hi // ct
            r_lo = r_hi // 10
        for bench, hi_ops, lo_ops in (
                ("fillrandom", w_hi * ct, w_lo * ct),
                ("readrandom", r_hi * ct, r_lo * ct),
                ("readwhilewriting", r_hi * ct, r_lo * ct)):
            hi = read_perf(os.path.join(RAW, f"perf.{rep}.{bench}.hi.csv"))
            lo = read_perf(os.path.join(RAW, f"perf.{rep}.{bench}.lo.csv"))
            n = hi_ops - lo_ops
            if not hi or not lo or n <= 0:
                print(f"    {bench:>16} {'no data':>16}")
                continue
            delta = {k: hi.get(k, 0) - lo.get(k, 0) for k in hi}

            def per(evt):
                v = delta.get(evt)
                return None if v is None else v / n

            bm, br = per("branch-misses"), per("branches")
            pct = 100.0 * bm / br if (bm is not None and br) else None
            l1 = per("L1-dcache-load-misses")
            # XSNP_FWD on Ice Lake and later is what XSNP_HITM was before it: a
            # load that hit a line another core held modified.
            # A counter that read zero at both points did not observe zero
            # events; it did not observe. Reporting 0.000/op for a cross-core
            # sharing counter that the hypervisor never implemented would put a
            # fabricated headline number into the paper.
            def dead(evt):
                return hi.get(evt, 0) == 0 and lo.get(evt, 0) == 0

            hitm_k = next((k for k in delta
                           if "hitm" in k.lower() or "xsnp_fwd" in k.lower()), None)
            clean_k = next((k for k in delta
                            if "xsnp_no_fwd" in k.lower()
                            or k.lower().endswith("xsnp_hit")), None)
            hitm = None if (hitm_k is None or dead(hitm_k)) else per(hitm_k)
            clean = None if (clean_k is None or dead(clean_k)) else per(clean_k)
            share = None
            if hitm is not None and clean is not None and (hitm + clean) > 0:
                share = 100.0 * hitm / (hitm + clean)
            # A negative marginal count means the two points did not differ by
            # what they were supposed to differ by. Say so rather than print it.
            flag = "  <- NEGATIVE, differencing failed" if (
                per("instructions") is not None and per("instructions") < 0) else ""
            print(f"    {bench:>16} {n:>16} {fmt(per('instructions'), 10, 1)} "
                  f"{fmt(bm, 15, 2)} {fmt(pct, 14, 3)} {fmt(l1, 11, 2)} {fmt(hitm, 10, 3)} "
                  f"{fmt(share, 9, 1)}{flag}")


def memtablerep():
    """memtablerep_bench prints a 'Running <bench>' header then 'write us/op: X'
    and/or 'read us/op: X'. Its fillrandom is hardcoded to one thread inside
    RocksDB regardless of --num_threads, so that row is a single-threaded insert
    cost at every column and is labelled as such rather than quietly repeated."""
    if not os.path.isdir(RAW):
        return
    running_re = re.compile(r"^Running\s+(\w+)")
    usop_re = re.compile(r"^(read|write)\s+us/op:\s*([\d.eE+-]+)")
    nthreads_re = re.compile(r"^Number of threads:\s*(\d+)")
    data = defaultdict(lambda: defaultdict(list))
    actual_threads = {}
    seen = False
    for fn in sorted(os.listdir(RAW)):
        m = MTREP_RE.match(fn)
        if not m:
            continue
        seen = True
        cur = None
        with open(os.path.join(RAW, fn), errors="replace") as fh:
            for line in fh:
                h = running_re.match(line)
                if h:
                    cur = h.group(1)
                    continue
                t = nthreads_re.match(line)
                if t and cur:
                    actual_threads[(cur, int(m["threads"]))] = int(t.group(1))
                    continue
                u = usop_re.match(line)
                if u and cur:
                    label = cur if cur != "readwrite" else f"readwrite {u.group(1)}"
                    data[(m["mrep"], int(m["threads"]))][label].append(float(u.group(2)))
    if not seen or not data:
        return
    print("\n== memtablerep_bench: the rep alone, us/op ==")
    print("No SSTs, no block cache, no version machinery. VectorRep runs fillrandom")
    print("only: memtablerep_bench has no read cap, and its point lookup on a")
    print("mutable vector sorts the whole bucket, so a read pass there would time")
    print("std::sort rather than a rep.")
    benches = sorted({b for v in data.values() for b in v})
    threads = sorted({t for (_, t) in data})
    for bench in benches:
        note = ""
        base = bench.split()[0]
        actual = {actual_threads.get((base, t)) for t in threads}
        actual.discard(None)
        if actual == {1}:
            note = "   (RocksDB runs this single-threaded whatever --num_threads says)"
        print(f"\n  {bench}{note}")
        print(f"{'threads':>9} " + " ".join(f"{r:>14}" for r in MTREP_ORDER))
        for t in threads:
            cells = []
            for r in MTREP_ORDER:
                v = med(data.get((r, t), {}).get(bench, []))
                cells.append("n/a".rjust(14) if v is None else f"{v:14.4f}")
            ap = med(data.get(("aparajita", t), {}).get(bench, []))
            sk = med(data.get(("skiplist", t), {}).get(bench, []))
            ratio = f"   {(sk/ap - 1)*100:+6.1f}% vs skiplist" if (ap and sk) else ""
            print(f"{t:>9} " + " ".join(cells) + ratio)


def main():
    total_ops = int(os.environ.get("TOTAL_OPS", 2000000))
    counter_threads = int(os.environ.get("COUNTER_THREADS", 16))
    counter_reads = int(os.environ.get("COUNTER_READS", 20000000))
    counter_reads_vec = int(os.environ.get("COUNTER_READS_VECTOR", 20))

    data, incomplete = load_db_bench()
    if not data:
        print(f"no db_bench output found under {RAW}")
        return 1

    print("Phase 4: empirical evaluation")
    env = os.path.join(OUT, "environment.txt")
    if os.path.exists(env):
        print()
        with open(env) as fh:
            print(fh.read().rstrip())

    if incomplete:
        print("\n!! runs that did not complete within the timeout:")
        for tag, rep, t in sorted(incomplete):
            print(f"   {tag} {rep} t={t}")

    table(data, "scale", "fillrandom", "fillrandom, WAL off",
          "Total operations held constant; num is per thread.")
    scaling(data, "scale", "fillrandom", "fillrandom")
    table(data, "scale", "readrandom", "readrandom over a resident memtable, WAL off",
          "VectorRep's read count is capped; see run_phase4.sh for why.")
    table(data, "rww", "readwhilewriting", "readwhilewriting, WAL off",
          "--threads is the reader count; one writer runs alongside.")
    scaling(data, "rww", "readwhilewriting", "readwhilewriting")
    table(data, "wal", "fillrandom", "fillrandom with the WAL on, 16 threads",
          "The same point as the first table, WAL enabled, to show what it hides.")

    counters(total_ops, counter_threads, counter_reads, counter_reads_vec)
    memtablerep()
    return 0


if __name__ == "__main__":
    sys.exit(main())
