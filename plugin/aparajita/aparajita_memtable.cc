// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Adapter between RocksDB's MemTableRep interface and the Aparajita structure.
//
// Everything RocksDB-specific is here. Three differences from the standalone
// structure are carried by AparajitaTraits and nothing else changes:
//
//   - an entry is a bare `const char*` into RocksDB's arena. MemTableRep::Allocate
//     hands key storage to the caller, so the rep never copies a key and an entry
//     costs eight bytes rather than a sixteen-byte string_view.
//   - order comes from MemTableRep::KeyComparator, a virtual call over a
//     length-prefixed internal key, and it is the only authority. The surrogate
//     proposes; the comparator disposes.
//   - memory comes from the rocksdb::Allocator the rep is handed, which is what
//     charges against write_buffer_size.

#include "aparajita_memtable.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

#include "aparajita/memtable.hpp"
#include "db/lookup_key.h"
#include "db/memtable.h"
#include "memory/allocator.h"
#include "memory/arena.h"
#include "rocksdb/comparator.h"
#include "rocksdb/slice.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/utilities/object_registry.h"

namespace ROCKSDB_NAMESPACE {

// Whether descend() may answer a tower hop from an eight-byte integer instead of
// a virtual comparator call. The promise is that bytewise order on the order
// bytes agrees with the comparator whenever those bytes differ, and only the
// default bytewise user comparator makes it.
//
// Establishing that turns out to be the hardest part of the optimisation, and
// the difficulty is in RocksDB's interface rather than in the idea.
// MemTableRep::KeyComparator exposes ordering and nothing else: no accessor
// reaches the InternalKeyComparator behind it, let alone the user comparator's
// name. The only route is the concrete type RocksDB actually passes, and
// recovering that needs RTTI.
//
// RocksDB compiles Release with -fno-rtti unless asked otherwise, so the answer
// has to be "no" there rather than a guess. The alternative -- probing the
// comparator with a handful of synthetic keys and generalising -- was rejected:
// a comparator that agrees on every probe and disagrees on the tenth key of a
// real workload would pass, and the failure would not be a slow descent but a
// lost key. docs/phase3.md already records that the surrogate proposes and the
// comparator disposes; a hint that guesses would be the same mistake one level
// up.
//
// So the fast path is available exactly when RocksDB is built with
// -DUSE_RTTI=ON, which is a supported cmake option and not a source patch, and
// the plugin is correct either way. Anything that is not a MemTable::KeyComparator
// over the default bytewise user comparator -- a reverse comparator, a comparator
// carrying a timestamp, or a caller that subclasses MemTableRep::KeyComparator
// itself -- fails one of the checks below and takes the slow descent.
bool AparajitaHintOrdering(const MemTableRep::KeyComparator& cmp) {
#ifdef ROCKSDB_USE_RTTI
  const auto* known = dynamic_cast<const MemTable::KeyComparator*>(&cmp);
  if (known == nullptr) {
    return false;
  }
  const Comparator* user = known->comparator.user_comparator();
  return user != nullptr &&
         std::strcmp(user->Name(), BytewiseComparator()->Name()) == 0;
#else
  (void)cmp;
  return false;
#endif
}

namespace {

// The tag appended to every user key inside an internal key: a 56-bit sequence
// number packed with an 8-bit value type.
constexpr size_t kInternalKeyTagLen = 8;

struct AparajitaTraits {
  // A length-prefixed internal key, exactly the pointer RocksDB handed back from
  // Allocate. Lookup keys have the same shape (LookupKey::memtable_key), so a
  // stored entry and a probe are the same type and the comparator handles both.
  using Entry = const char*;
  using Target = const char*;

  Allocator* alloc;
  const MemTableRep::KeyComparator* cmp;

  // RocksDB's allocators guarantee only pointer alignment, and ConcurrentArena
  // rounds to sizeof(void*). A node that is merely pointer-aligned can straddle
  // two cache lines, which would cost every probe a second line fill and give up
  // the premise the whole design rests on. So the alignment is taken by hand:
  // over-allocate by align-1 and round the address up.
  //
  // The padding is real overhead, up to 63 bytes on a 208-byte node. It buys the
  // guarantee the static_asserts in memtable.hpp make and the counters in Phase 1
  // measured, so it is paid deliberately rather than avoided.
  char* allocate(size_t bytes, size_t align) const {
    char* raw = alloc->AllocateAligned(bytes + align - 1);
    const uintptr_t addr = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t aligned = (addr + align - 1) & ~static_cast<uintptr_t>(align - 1);
    return reinterpret_cast<char*>(aligned);
  }

  // The bytes the prefix and the surrogate are taken from: the user key, without
  // the sequence-and-type tag.
  //
  // The tag is excluded on purpose. It is the low eight bytes of every internal
  // key and it *descends* with sequence number while the user key ascends, so
  // including it would put an anti-sorted field in the middle of the lane and
  // break the ordering the surrogate exists to reproduce. Two entries sharing a
  // user key therefore share a surrogate and are separated by the comparator,
  // which is exactly the candidate-confirmation path the design already has.
  static std::string_view order_bytes(const char* e) {
    const Slice internal_key = GetLengthPrefixedSlice(e);
    assert(internal_key.size() >= kInternalKeyTagLen);
    return std::string_view(internal_key.data(),
                            internal_key.size() - kInternalKeyTagLen);
  }

  int compare(const char* a, const char* b) const { return (*cmp)(a, b); }

  static Entry null_entry() { return nullptr; }

  bool hint_ordering() const { return AparajitaHintOrdering(*cmp); }
};

using Table = aparajita::BasicMemTable<AparajitaTraits>;

class AparajitaRep : public MemTableRep {
 public:
  AparajitaRep(const MemTableRep::KeyComparator& compare, Allocator* allocator,
               const SliceTransform* transform)
      : MemTableRep(allocator),
        table_(AparajitaTraits{allocator, &compare}),
        cmp_(compare),
        transform_(transform) {}

  ~AparajitaRep() override = default;

  void Insert(KeyHandle handle) override {
    table_.insert_entry(static_cast<const char*>(handle));
  }

  // The same call. The structure takes a per-node lock for the sixteen keys it
  // is rewriting and needs no separate serial path, so there is nothing for the
  // single-threaded variant to do differently.
  void InsertConcurrently(KeyHandle handle) override {
    table_.insert_entry(static_cast<const char*>(handle));
  }

  bool Contains(const char* key) const override { return table_.contains_target(key); }

  // Every byte comes from the allocator RocksDB gave us, so there is nothing to
  // report here. This is the same answer SkipListRep gives.
  size_t ApproximateMemoryUsage() override { return 0; }

  void Get(const LookupKey& k, void* callback_args,
           bool (*callback_func)(void* arg, const char* entry)) override {
    Table::Iterator iter(table_);
    for (iter.seek(k.memtable_key().data());
         iter.valid() && callback_func(callback_args, iter.key()); iter.next()) {
    }
  }

  // On the flush path, where it estimates how much of the memtable is garbage.
  // The base class asserts rather than defining a default, so leaving it alone
  // would abort a debug build during flush.
  //
  // Reservoir sampling over one forward pass. Flush already reads every entry, so
  // a linear pass costs nothing extra, and unlike repeated random seeks it cannot
  // return fewer samples than asked for when unlucky.
  void UniqueRandomSample(const uint64_t num_entries,
                          const uint64_t target_sample_size,
                          std::unordered_set<const char*>* entries) override {
    entries->clear();
    if (target_sample_size == 0 || num_entries == 0) {
      return;
    }
    std::vector<const char*> reservoir;
    reservoir.reserve(static_cast<size_t>(target_sample_size));

    std::mt19937_64 rng(num_entries * 1000003ull + target_sample_size);
    uint64_t seen = 0;
    for (Table::Iterator it = table_.begin(); it.valid(); it.next(), ++seen) {
      if (reservoir.size() < target_sample_size) {
        reservoir.push_back(it.key());
      } else {
        const uint64_t j = rng() % (seen + 1);
        if (j < target_sample_size) {
          reservoir[static_cast<size_t>(j)] = it.key();
        }
      }
    }
    entries->insert(reservoir.begin(), reservoir.end());
  }

  MemTableRep::Iterator* GetIterator(Arena* arena) override {
    if (arena == nullptr) {
      return new AparajitaIterator(&table_);
    }
    void* mem = arena->AllocateAligned(sizeof(AparajitaIterator));
    return new (mem) AparajitaIterator(&table_);
  }

 private:
  class AparajitaIterator : public MemTableRep::Iterator {
   public:
    explicit AparajitaIterator(const Table* table) : iter_(*table) {}
    ~AparajitaIterator() override = default;

    bool Valid() const override { return iter_.valid(); }

    const char* key() const override {
      assert(Valid());
      return iter_.key();
    }

    void Next() override {
      assert(Valid());
      iter_.next();
    }

    void Prev() override {
      assert(Valid());
      iter_.prev();
    }

    // RocksDB passes either a ready-made memtable key or an internal key that
    // still needs the length prefix. Encoding into tmp_ is what SkipListRep does
    // and keeps the comparator seeing one shape.
    void Seek(const Slice& internal_key, const char* memtable_key) override {
      iter_.seek(memtable_key != nullptr ? memtable_key
                                         : EncodeKey(&tmp_, internal_key));
    }

    void SeekForPrev(const Slice& internal_key, const char* memtable_key) override {
      iter_.seek_for_prev(memtable_key != nullptr ? memtable_key
                                                  : EncodeKey(&tmp_, internal_key));
    }

    void SeekToFirst() override { iter_.seek_to_first(); }
    void SeekToLast() override { iter_.seek_to_last(); }

   private:
    Table::Iterator iter_;
    std::string tmp_;  // for passing to EncodeKey
  };

  Table table_;
  const MemTableRep::KeyComparator& cmp_;
  const SliceTransform* transform_;
};

}  // namespace

MemTableRep* AparajitaMemTableFactory::CreateMemTableRep(
    const MemTableRep::KeyComparator& compare, Allocator* allocator,
    const SliceTransform* transform, Logger* /*logger*/) {
  return new AparajitaRep(compare, allocator, transform);
}

}  // namespace ROCKSDB_NAMESPACE

// Called by RocksDB's generated plugin registry, wired up by the `_FUNC` line in
// aparajita.mk. This is what makes `--memtablerep=aparajita` resolve without
// patching any RocksDB source.
//
// Global namespace and extern "C" are both required. RocksDB generates
// build_version.cc with this exact declaration inside an `extern "C"` block and
// takes its address unqualified, so a definition tucked inside ROCKSDB_NAMESPACE
// or left with C++ linkage compiles fine and then fails to link.
extern "C" int register_AparajitaMemTable(ROCKSDB_NAMESPACE::ObjectLibrary& library,
                                          const std::string& /*arg*/) {
  using ROCKSDB_NAMESPACE::AparajitaMemTableFactory;
  using ROCKSDB_NAMESPACE::MemTableRepFactory;
  using ROCKSDB_NAMESPACE::ObjectLibrary;

  library.AddFactory<MemTableRepFactory>(
      ObjectLibrary::PatternEntry(AparajitaMemTableFactory::kClassName(), true)
          .AnotherName(AparajitaMemTableFactory::kNickName()),
      [](const std::string& /*uri*/, std::unique_ptr<MemTableRepFactory>* guard,
         std::string* /*errmsg*/) {
        guard->reset(new AparajitaMemTableFactory());
        return guard->get();
      });
  size_t num_types;
  return static_cast<int>(library.GetFactoryCount(&num_types));
}
