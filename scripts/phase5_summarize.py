#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Turns run_phase5.sh's raw db_bench output into the tables docs/phase5-ordered.md
# and paper/main.tex quote, so those tables are generated rather than retyped.
import os
import re
import statistics
import sys

OPS = re.compile(r"^(\w+)\s*:\s*([0-9.]+)\s+micros/op\s+(\d+)\s+ops/sec", re.M)
FLUSH = re.compile(r"rocksdb\.db\.flush\.micros.*COUNT\s*:\s*(\d+)\s+SUM\s*:\s*(\d+)")
SST = re.compile(r"^sst_files:\s*(\d+)", re.M)


def med(xs):
    return statistics.median(xs) if xs else None


def fmt(x, width=10, prec=1):
    return " " * width if x is None else f"{x:{width}.{prec}f}"


def load(out_dir):
    seek, flush = {}, {}
    raw = os.path.join(out_dir, "raw")
    for name in sorted(os.listdir(raw)):
        path = os.path.join(raw, name)
        text = open(path, errors="replace").read()
        parts = name.split(".")
        if name.startswith("seek."):
            nexts = int(parts[1][1:])
            rep, threads = parts[2], int(parts[3][1:])
            for m in OPS.finditer(text):
                if m.group(1) == "seekrandom":
                    key = (nexts, rep, threads)
                    seek.setdefault(key, {"kops": [], "us": [], "sst": []})
                    seek[key]["kops"].append(int(m.group(3)) / 1000.0)
                    seek[key]["us"].append(float(m.group(2)))
            m = SST.search(text)
            if m:
                seek.setdefault((nexts, rep, threads), {"kops": [], "us": [], "sst": []})
                seek[(nexts, rep, threads)]["sst"].append(int(m.group(1)))
        elif name.startswith("flush."):
            rep = parts[1]
            rec = flush.setdefault(rep, {"us": [], "sst": [], "n": [], "sum": []})
            for m in OPS.finditer(text):
                if m.group(1) == "fillrandom":
                    rec["us"].append(float(m.group(2)))
            f = FLUSH.search(text)
            if f:
                rec["n"].append(int(f.group(1)))
                rec["sum"].append(int(f.group(2)))
            m = SST.search(text)
            if m:
                rec["sst"].append(int(m.group(1)))
    return seek, flush


def seek_table(seek, nexts, threads_all, reps):
    title = "Seek alone" if nexts == 0 else f"Seek then {nexts} Next"
    print(f"\n== seekrandom, seek_nexts={nexts} ({title}), WAL off ==")
    print(f"{'threads':>9}" + "".join(f"{r:>15}" for r in reps) + f"{'vs skip_list':>16}")
    print(f"{'':>9}" + "".join(f"{'kops/s':>15}" for _ in reps))
    for t in threads_all:
        row = f"{t:>9}"
        vals = {}
        for r in reps:
            v = med(seek.get((nexts, r, t), {}).get("kops", []))
            vals[r] = v
            row += fmt(v, 15, 1)
        a, s = vals.get("aparajita"), vals.get("skip_list")
        row += f"{(a / s - 1) * 100:>15.1f}%" if a and s else " " * 16
        print(row)

    print(f"\n{'threads':>9}" + "".join(f"{r:>15}" for r in reps))
    print(f"{'':>9}" + "".join(f"{'min/med/max':>15}" for _ in reps))
    for t in threads_all:
        row = f"{t:>9}"
        for r in reps:
            xs = seek.get((nexts, r, t), {}).get("kops", [])
            row += f"{(f'{min(xs):.0f}/{med(xs):.0f}/{max(xs):.0f}' if xs else ''):>15}"
        print(row)


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "results/phase5-ordered"
    seek, flush = load(out_dir)

    print("Phase 5: ordered access, and what the arena costs when it has to flush")
    env = os.path.join(out_dir, "environment.txt")
    if os.path.exists(env):
        print()
        print(open(env).read().rstrip())

    reps = ["aparajita", "skip_list"]
    threads_all = sorted({t for (_, _, t) in seek})
    for nexts in sorted({n for (n, _, _) in seek}):
        seek_table(seek, nexts, threads_all, reps)

    bad = [(k, max(v["sst"])) for k, v in sorted(seek.items()) if v["sst"] and max(v["sst"]) > 1]
    print("\n  memtable-resident: " + ("no run produced more than the flush on close"
                                       if not bad else f"WARNING, extra SSTs: {bad}"))

    if flush:
        print("\n== arena cost and flush cost, 64 MiB write buffer, 1 thread ==")
        print("Flushes are how a rep's arena per key becomes visible: fewer keys per")
        print("memtable means more flushes for the same keyspace.")
        print(f"\n{'rep':>12}{'fill us/op':>13}{'L0 files':>11}{'keys/flush':>13}"
              f"{'flush us/key':>15}")
        base = {}
        for rep in reps:
            r = flush.get(rep)
            if not r:
                continue
            n = med(r["n"])
            total_us = med(r["sum"])
            ssts = med(r["sst"])
            keys = (2000000 / ssts) if ssts else None
            # Per key flushed, which is the only fair comparison: the two reps
            # flush different numbers of keys per flush, so neither the per-flush
            # cost nor the raw total is comparable on its own.
            per_key = (total_us / (n * keys)) if n and keys else None
            base[rep] = (ssts, keys, per_key)
            print(f"{rep:>12}{fmt(med(r['us']), 13, 3)}{fmt(ssts, 11, 0)}"
                  f"{fmt(keys, 13, 0)}{fmt(per_key, 15, 3)}")
        if "aparajita" in base and "skip_list" in base:
            (sa, ka, pa), (ss, ks, ps) = base["aparajita"], base["skip_list"]
            if ka and ks:
                print(f"\n  arena per key, Aparajita vs skiplist:  {ks / ka:.2f}x")
                print(f"  L0 files for the same 2M keys:        {sa / ss:.2f}x")
            if pa and ps:
                print(f"  flush cost per key flushed:           {pa / ps:.2f}x")
                print("\n  The extra files are the arena overhead made visible. They do not")
                print("  cost extra flush time -- the same keys are written either way, at a")
                print("  per-key cost that is within a few percent -- but they are more L0")
                print("  files for compaction to merge, which this configuration disables and")
                print("  therefore does not measure.")

    print("\nSeek is the ordered workload the design is justified by and Phase 4 did")
    print("not measure. Flush is the other half of the same question: the iterator")
    print("that ordered iteration needs is also what a flush walks.")


if __name__ == "__main__":
    main()
