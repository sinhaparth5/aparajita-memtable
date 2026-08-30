// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

// The RocksDB face of Aparajita: a MemTableRepFactory that RocksDB can select
// through options.memtable_factory, or by name once the plugin is linked in.
//
// The structure itself lives in include/aparajita/memtable.hpp and knows nothing
// about RocksDB. This header and its .cc are the adapter.

#include <memory>

#include "rocksdb/memtablerep.h"

namespace ROCKSDB_NAMESPACE {

// Whether Aparajita's descent may answer a tower hop from an eight-byte hint
// rather than a virtual comparator call, under `cmp`.
//
// Exposed here only so a test can pin it down. The property it reports is a
// promise about the comparator, and a promise nothing checks is one that quietly
// stops being true: without this, a change that made the answer false for the
// default comparator would cost every descent its fast path and no test would
// notice, because a disabled fast path is still correct.
bool AparajitaHintOrdering(const MemTableRep::KeyComparator& cmp);

class AparajitaMemTableFactory : public MemTableRepFactory {
 public:
  AparajitaMemTableFactory() = default;
  ~AparajitaMemTableFactory() override = default;

  static const char* kClassName() { return "AparajitaMemTableFactory"; }
  static const char* kNickName() { return "aparajita"; }
  const char* Name() const override { return kClassName(); }
  const char* NickName() const override { return kNickName(); }

  using MemTableRepFactory::CreateMemTableRep;
  MemTableRep* CreateMemTableRep(const MemTableRep::KeyComparator& compare,
                                 Allocator* allocator,
                                 const SliceTransform* transform,
                                 Logger* logger) override;

  // RocksDB only routes concurrent writes to a rep that advertises support. If
  // this returned false every multi-threaded write benchmark would serialise on
  // the memtable switch and the project's central claim could not be shown.
  bool IsInsertConcurrentlySupported() const override { return true; }
};

}  // namespace ROCKSDB_NAMESPACE

// The plugin registration hook, register_AparajitaMemTable, is deliberately not
// declared here. RocksDB generates its declaration into build_version.cc inside
// an `extern "C"` block, and a second declaration in this header has to match it
// exactly -- including having ObjectLibrary complete at that point -- for no
// benefit. The definition lives in aparajita_memtable.cc.
