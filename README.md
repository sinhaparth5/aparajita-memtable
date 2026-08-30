<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/logo-dark.svg">
  <source media="(prefers-color-scheme: light)" srcset="assets/logo-light.svg">
  <img alt="Aparajita" src="assets/logo-light.svg" width="440">
</picture>

**Eliminating CPU cache line invalidations and branch mispredictions in LSM-tree memtables via SIMD register alignment**

[![License](https://img.shields.io/badge/license-Apache--2.0%20OR%20MIT-blue.svg)](#license)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)](CMakeLists.txt)
[![RocksDB](https://img.shields.io/badge/RocksDB-v9.11.2-a4243b.svg)](plugin/aparajita)
[![Paper](https://img.shields.io/badge/paper-PDF-informational.svg)](paper/main.pdf)
[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.22180368.svg)](https://doi.org/10.5281/zenodo.22180368)

</div>

---

Aparajita is a C++20 in-memory buffer (`MemTableRep`) for RocksDB and LevelDB-style LSM-tree
engines, and a drop-in replacement for RocksDB's default concurrent skip list. It is selected by
name, without patching RocksDB sources:

```sh
db_bench --memtablerep=aparajita ...
```

A skip list chases pointers across heap allocations, so every key comparison risks an L1 or L2
miss and a branch misprediction. Aparajita packs fifteen 32-bit order-preserving key surrogates
and a sentinel into one 64-byte cache line and searches them with a single branchless vector
compare. The population count of the resulting mask *is* the lower bound, so an ordered search
costs one compare, one movemask and one `popcnt` — no data-dependent branch anywhere on the read
path.

## Results

Measured on a rented Intel Xeon Platinum 8581C (Emerald Rapids), against RocksDB v9.11.2's default
skip list on the same host and the same build. Full method and raw data in
[`docs/phase4-eval.md`](docs/phase4-eval.md) and [`results/`](results).

| `db_bench` workload | 1 thread | 4 | 16 | 64 |
| --- | ---: | ---: | ---: | ---: |
| `readrandom` | **+38.6%** | **+37.3%** | **+22.2%** | **+25.8%** |
| `readwhilewriting` | **+35.8%** | **+38.1%** | **+29.0%** | **+22.8%** |
| `seekrandom` (seek alone) | **+28.6%** | **+27.4%** | **+17.2%** | **+15.1%** |
| `seekrandom` (seek + 10 × `Next`) | −3.0% | −3.3% | −4.3% | −3.8% |
| `fillrandom` | **+18.8%** | +2.6% | −3.0% | +0.6% |

Reads win uniformly and the samples do not overlap. Two results go the other way and are reported
here for the same reason they are reported in the paper:

- **Iteration is 3–4% slower.** A skip list's `Next` is one pointer load; ours decodes a rank from
  the node's order word, indexes the slot that nibble names, then follows a key pointer. That the
  sorted order is a computed permutation is exactly what makes an insert a single release store, so
  this is the design's price rather than a defect.
- **Multi-threaded writes are a tie, not a win.** At 16 threads a `fillrandom` operation retires
  22,182 instructions, which no memtable insert costs — that is RocksDB's write-thread group,
  charged identically to every representation. At the representation level, in `memtablerep_bench`,
  the skip list is 48% behind on insert at every thread count.

The kernel underneath, in cycles per probe over a 16-lane node
([`results/phase4-ordered-kernels.txt`](results/phase4-ordered-kernels.txt)):

| kernel | cycles/probe | branch misses/probe |
| --- | ---: | ---: |
| scalar `lower_bound`, branchy | 40.14 | 1.04 |
| scalar `lower_bound`, branchless | 8.67 | 0.00 |
| AVX2 | 9.89 | 0.00 |
| AVX-512 | 6.04 | 0.00 |

Aparajita charges about 1.4× the skip list's arena per key. That overhead lands on L0 file count
(7 files against 5 for the same 2M keys), not on flush time: per-key flush cost is 0.425 against
0.441 µs. The compaction cost of 40% more L0 files is not measured, and is the most likely place
this design still loses overall.

## Build

The header-only core and its harnesses need nothing but CMake and a C++20 compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build -E tsan --output-on-failure
```

The RocksDB plugin has to be compiled inside a RocksDB checkout, because `MemTableRep` needs
internal headers that `librocksdb-dev` does not install. The script clones and pins v9.11.2:

```sh
./scripts/build_rocksdb_plugin.sh
```

`CLAUDE.md` documents the six non-obvious things about that build, and
[`docs/`](docs) carries the findings per phase.

## How it works

Four decisions interlock, and each is argued in [`docs/`](docs) and in the paper:

- **The structure is ordered, not sorted at flush time.** RocksDB iterators must yield keys in
  comparator order, so an unordered buffer only defers the cost. A *relational* compare over a
  sorted node produces a mask whose set bits form a prefix, so its population count is the lower
  bound directly.
- **A lane holds leading key bytes, not a hash**, because a hash destroys the order the lower bound
  depends on. Bytes are loaded big-endian so an unsigned 32-bit compare reproduces RocksDB's
  bytewise order.
- **Surrogates are taken after the node's shared prefix.** An absolute four-byte lane takes a single
  value across the whole keyspace in five of eight realistic key distributions — a table prefix, a
  tenant id or a big-endian timestamp puts identical bytes at the front of every key. Measuring the
  lane per node instead restores every distribution to an effective one.
- **A node is append-only and its sorted order is a 64-bit word.** A slot is written once, before
  the order word that names it, so an insert is two stores into a free slot and one release store of
  the new order. Readers are lock-free and never write.

## Paper

[`paper/main.pdf`](paper/main.pdf) is the write-up, built with `make -C paper`.

## Citation

Archived on Zenodo with a DOI. Cite the concept DOI [`10.5281/zenodo.22180368`](https://doi.org/10.5281/zenodo.22180368) to point at
whatever the latest version is, or [`10.5281/zenodo.22180369`](https://doi.org/10.5281/zenodo.22180369) to pin v0.5.0 specifically.

```bibtex
@software{aparajita,
  author    = {Sinha, Parth Kumar},
  title     = {Aparajita: A {SIMD} {MemTable} for {LSM}-Tree Key-Value Stores},
  version   = {0.5.0},
  doi       = {10.5281/zenodo.22180369},
  publisher = {Zenodo},
  year      = {2026}
}
```

Machine-readable metadata is in [`CITATION.cff`](CITATION.cff), and GitHub renders a formatted
citation from the sidebar.

## License

Dual-licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <http://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <http://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in
this work by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without
any additional terms or conditions.
