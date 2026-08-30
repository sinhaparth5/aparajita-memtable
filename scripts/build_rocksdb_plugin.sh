#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0 OR MIT
#
# Builds RocksDB with the Aparajita memtable plugin linked in, then runs the
# differential test and a db_bench smoke check.
#
# The plugin is linked into the RocksDB tree rather than copied, so the RocksDB
# checkout stays a pristine tagged tree and the sources being built are always the
# ones in this repository. Nothing here patches RocksDB.
set -euo pipefail

# Pinned deliberately. The MemTableRep interface is not stable across major
# RocksDB versions -- UniqueRandomSample and the *AndValidate methods have all
# arrived or changed shape -- so the version this is known to build against is
# recorded here rather than tracking whatever HEAD happens to be.
ROCKSDB_TAG=${ROCKSDB_TAG:-v9.11.2}

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROCKSDB_SRC=${ROCKSDB_SRC:-$(dirname "$REPO_ROOT")/rocksdb-src}
BUILD_DIR=${BUILD_DIR:-$ROCKSDB_SRC/build}
JOBS=${JOBS:-$(nproc)}

if [[ ! -d "$ROCKSDB_SRC" ]]; then
    echo "== cloning RocksDB $ROCKSDB_TAG =="
    git clone --depth 1 --branch "$ROCKSDB_TAG" \
        https://github.com/facebook/rocksdb.git "$ROCKSDB_SRC"
fi

echo "== linking plugin =="
# A link, not a copy: the plugin's CMakeLists resolves this back to the repository
# to find include/aparajita, and a copy would silently build stale sources.
rm -rf "$ROCKSDB_SRC/plugin/aparajita"
ln -s "$REPO_ROOT/plugin/aparajita" "$ROCKSDB_SRC/plugin/aparajita"

# RocksDB 9.11.2 predates the GCC 13+ tightening that stopped <cstdint> being
# pulled in transitively, so several of its headers use uint64_t without
# including it. Forcing the include from the command line keeps the RocksDB
# checkout unpatched, which is the point: the exit criterion is that Aparajita
# is selectable without modifying RocksDB sources, and that has to stay true.
EXTRA_CXX_FLAGS="-include cstdint -include cstdio"

echo "== configuring =="
cmake -S "$ROCKSDB_SRC" -B "$BUILD_DIR" \
    -DROCKSDB_PLUGINS=aparajita \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_CXX_FLAGS="$EXTRA_CXX_FLAGS" \
    -DWITH_TESTS=ON -DWITH_BENCHMARK_TOOLS=ON -DWITH_GFLAGS=ON \
    -DWITH_SNAPPY=OFF -DWITH_LZ4=OFF -DWITH_ZLIB=OFF -DWITH_ZSTD=OFF -DWITH_BZ2=OFF \
    -DFAIL_ON_WARNINGS=OFF -DPORTABLE=1 \
    ${CMAKE_GENERATOR:+-G"$CMAKE_GENERATOR"}

echo "== building =="
cmake --build "$BUILD_DIR" -j"$JOBS" --target db_bench aparajita_memtable_test

echo
echo "== differential test against the skiplist rep =="
"$BUILD_DIR/aparajita_memtable_test"

echo
echo "== db_bench selects the rep by name =="
# The point of this run is the selection, not the number: if the plugin registry
# were not wired up, CreateFromString would fail here rather than falling back.
DB_PATH=${DB_PATH:-/tmp/aparajita_db_bench}
rm -rf "$DB_PATH"
"$BUILD_DIR/db_bench" \
    --benchmarks=fillrandom,readrandom \
    --memtablerep=aparajita \
    --num=200000 --threads=4 --disable_wal=1 \
    --db="$DB_PATH"
rm -rf "$DB_PATH"

echo
echo "plugin build and checks complete"
