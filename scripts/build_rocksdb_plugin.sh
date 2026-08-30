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
DEBUG_BUILD_DIR=${DEBUG_BUILD_DIR:-$ROCKSDB_SRC/build-debug}
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

# Two trees, and the reason is not tidiness. RocksDB wraps WITH_TESTS in a
# CMAKE_DEPENDENT_OPTION that forces it off unless CMAKE_BUILD_TYPE is exactly
# Debug, so -DWITH_TESTS=ON on a Release build silently generates no test targets
# at all and the build fails on an unknown target rather than on a missing flag.
# Release is what db_bench should be measured in; Debug is the only place the
# gtest exists, and it turns on RocksDB's internal asserts as a bonus.
COMMON_CMAKE_ARGS=(
    -DROCKSDB_PLUGINS=aparajita
    -DCMAKE_CXX_STANDARD=20
    -DCMAKE_CXX_FLAGS="$EXTRA_CXX_FLAGS"
    -DWITH_GFLAGS=ON
    -DWITH_SNAPPY=OFF -DWITH_LZ4=OFF -DWITH_ZLIB=OFF -DWITH_ZSTD=OFF -DWITH_BZ2=OFF
    -DFAIL_ON_WARNINGS=OFF -DPORTABLE=1
)

echo "== configuring release tree =="
# USE_RTTI is not incidental. The descent's hint fast path is only enabled when
# the plugin can confirm the user comparator is the default bytewise one, and
# MemTableRep::KeyComparator exposes no route to it other than a dynamic_cast --
# see AparajitaHintOrdering(). RocksDB's Release build passes -fno-rtti by
# default, which turns the fast path off and costs the descent roughly a third of
# its speed. This is a supported cmake option, not a patch, and it applies to the
# whole tree, so the skiplist this is benchmarked against is built the same way.
cmake -S "$ROCKSDB_SRC" -B "$BUILD_DIR" \
    "${COMMON_CMAKE_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_RTTI=ON \
    -DWITH_BENCHMARK_TOOLS=ON \
    ${CMAKE_GENERATOR:+-G"$CMAKE_GENERATOR"}

echo "== configuring debug tree =="
cmake -S "$ROCKSDB_SRC" -B "$DEBUG_BUILD_DIR" \
    "${COMMON_CMAKE_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_TESTS=ON \
    ${CMAKE_GENERATOR:+-G"$CMAKE_GENERATOR"}

echo "== building =="
cmake --build "$BUILD_DIR" -j"$JOBS" --target db_bench
cmake --build "$DEBUG_BUILD_DIR" -j"$JOBS" --target aparajita_memtable_test

echo
echo "== differential test against the skiplist rep =="
"$DEBUG_BUILD_DIR/aparajita_memtable_test"

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
    --compression_type=none \
    --db="$DB_PATH"
rm -rf "$DB_PATH"

echo
echo "plugin build and checks complete"
