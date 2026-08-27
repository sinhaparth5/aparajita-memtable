# aparajita-memtable

Aparajita: Eliminating CPU Cache Line Invalidations and Branch Mispredictions in LSM-Tree MemTables via SIMD Register Alignment

A hardware-optimized C++20 in-memory buffer (MemTable) plugin for RocksDB and LevelDB-style
LSM-tree storage engines, intended as a drop-in replacement for RocksDB's default concurrent
skiplist.

See [ROADMAP.md](ROADMAP.md) for the build plan.

## License

Dual-licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <http://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <http://opensource.org/licenses/MIT>)

at your option.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in
this work by you, as defined in the Apache-2.0 license, shall be dual licensed as above, without
any additional terms or conditions.
