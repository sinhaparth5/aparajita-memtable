// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Differential tests: Aparajita against the default skiplist rep.
//
// The exit criterion for this phase is that iteration matches the skiplist's
// output on the same key set byte for byte, so that is what these tests assert,
// rather than checking Aparajita against a hand-written expectation. Anything the
// two reps disagree about is a bug in the new one, and the comparison covers
// ordering, Get, Seek, SeekForPrev and reverse iteration at once.
//
// The tests run at the DB level on purpose. A rep-level harness would have to
// hand-encode length-prefixed internal keys and would drift from whatever
// MemTable::Add actually does; going through the DB exercises the rep exactly as
// RocksDB drives it, including the concurrent insert path.

#include "aparajita_memtable.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "port/stack_trace.h"
#include "rocksdb/comparator.h"
#include "rocksdb/convenience.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "test_util/testharness.h"
#include "test_util/testutil.h"

namespace ROCKSDB_NAMESPACE {

namespace {

// Everything the two reps must agree on, flattened into one comparable blob.
struct Snapshot {
  std::vector<std::string> forward;
  std::vector<std::string> backward;
  std::vector<std::string> gets;
  std::vector<std::string> seeks;
  std::vector<std::string> seeks_for_prev;
};

std::string MakeKey(int i) {
  char buf[64];
  snprintf(buf, sizeof(buf), "user:%08d:suffix", i);
  return std::string(buf);
}

std::string MakeValue(int i) {
  return "value-" + std::to_string(i * 7919);
}

class AparajitaMemTableTest : public testing::Test {
 protected:
  // Applies an identical workload under the given rep and records everything
  // observable about the result.
  Snapshot Run(std::shared_ptr<MemTableRepFactory> factory,
               const Comparator* comparator, bool flush, int num_keys,
               int write_threads) {
    Options options;
    options.create_if_missing = true;
    options.memtable_factory = factory;
    options.comparator = comparator;
    // Keep everything in the memtable unless the case explicitly asks for a
    // flush; otherwise the SST reader would answer the queries and the memtable
    // rep under test would not be exercised at all.
    options.write_buffer_size = 256 << 20;
    options.disable_auto_compactions = true;
    options.allow_concurrent_memtable_write = true;
    options.enable_write_thread_adaptive_yield = false;

    std::string dbname = test::PerThreadDBPath("aparajita_diff_test");
    EXPECT_OK(DestroyDB(dbname, options));

    DB* db = nullptr;
    EXPECT_OK(DB::Open(options, dbname, &db));
    std::unique_ptr<DB> guard(db);

    WriteOptions wo;
    wo.disableWAL = true;

    if (write_threads <= 1) {
      for (int i = 0; i < num_keys; ++i) {
        EXPECT_OK(db->Put(wo, MakeKey(i), MakeValue(i)));
      }
    } else {
      // The concurrent path is only reached when the rep advertises support for
      // it, so this doubles as a check that IsInsertConcurrentlySupported() is
      // actually honoured rather than silently serialised.
      std::vector<std::thread> threads;
      for (int t = 0; t < write_threads; ++t) {
        threads.emplace_back([&, t] {
          for (int i = t; i < num_keys; i += write_threads) {
            ASSERT_OK(db->Put(wo, MakeKey(i), MakeValue(i)));
          }
        });
      }
      for (auto& th : threads) {
        th.join();
      }
    }

    // Overwrite and delete a slice of the space so the memtable holds several
    // versions of one user key, which is where sequence-number ordering inside a
    // node matters and where a surrogate cannot discriminate at all.
    for (int i = 0; i < num_keys; i += 7) {
      EXPECT_OK(db->Put(wo, MakeKey(i), MakeValue(i) + "-v2"));
    }
    for (int i = 0; i < num_keys; i += 11) {
      EXPECT_OK(db->Delete(wo, MakeKey(i)));
    }

    if (flush) {
      EXPECT_OK(db->Flush(FlushOptions()));
    }

    Snapshot snap;
    ReadOptions ro;

    {
      std::unique_ptr<Iterator> it(db->NewIterator(ro));
      for (it->SeekToFirst(); it->Valid(); it->Next()) {
        snap.forward.push_back(it->key().ToString() + "=" + it->value().ToString());
      }
      EXPECT_OK(it->status());
    }

    {
      std::unique_ptr<Iterator> it(db->NewIterator(ro));
      for (it->SeekToLast(); it->Valid(); it->Prev()) {
        snap.backward.push_back(it->key().ToString() + "=" + it->value().ToString());
      }
      EXPECT_OK(it->status());
    }

    for (int i = 0; i < num_keys; ++i) {
      std::string value;
      Status s = db->Get(ro, MakeKey(i), &value);
      snap.gets.push_back(s.ok() ? value : s.ToString());
    }

    // Probe between and beyond the stored keys, not only on them, so a seek that
    // lands one slot off is caught.
    {
      std::unique_ptr<Iterator> it(db->NewIterator(ro));
      for (int i = -3; i < num_keys + 3; i += 3) {
        const std::string target = MakeKey(i);
        it->Seek(target);
        snap.seeks.push_back(it->Valid() ? it->key().ToString() : "<end>");
        it->SeekForPrev(target);
        snap.seeks_for_prev.push_back(it->Valid() ? it->key().ToString() : "<begin>");
      }
      EXPECT_OK(it->status());
    }

    guard.reset();
    EXPECT_OK(DestroyDB(dbname, options));
    return snap;
  }

  void ExpectIdentical(const Snapshot& a, const Snapshot& b) {
    EXPECT_EQ(a.forward, b.forward);
    EXPECT_EQ(a.backward, b.backward);
    EXPECT_EQ(a.gets, b.gets);
    EXPECT_EQ(a.seeks, b.seeks);
    EXPECT_EQ(a.seeks_for_prev, b.seeks_for_prev);
    // A snapshot where everything is empty would satisfy every check above, so
    // assert the workload actually produced something to compare.
    EXPECT_GT(a.forward.size(), 0u);
    EXPECT_EQ(a.forward.size(), a.backward.size());
  }

  std::shared_ptr<MemTableRepFactory> Aparajita() {
    return std::make_shared<AparajitaMemTableFactory>();
  }
  std::shared_ptr<MemTableRepFactory> SkipList() {
    return std::make_shared<SkipListFactory>();
  }
};

TEST_F(AparajitaMemTableTest, MatchesSkipListSingleThreaded) {
  auto expected = Run(SkipList(), BytewiseComparator(), false, 5000, 1);
  auto actual = Run(Aparajita(), BytewiseComparator(), false, 5000, 1);
  ExpectIdentical(expected, actual);
}

TEST_F(AparajitaMemTableTest, MatchesSkipListConcurrentWrites) {
  auto expected = Run(SkipList(), BytewiseComparator(), false, 5000, 8);
  auto actual = Run(Aparajita(), BytewiseComparator(), false, 5000, 8);
  ExpectIdentical(expected, actual);
}

TEST_F(AparajitaMemTableTest, MatchesSkipListAcrossFlush) {
  auto expected = Run(SkipList(), BytewiseComparator(), true, 5000, 4);
  auto actual = Run(Aparajita(), BytewiseComparator(), true, 5000, 4);
  ExpectIdentical(expected, actual);
}

// The surrogate reproduces bytewise order on the user key. Under a comparator
// that orders the other way it reproduces exactly the wrong order, so this is the
// case that proves the SIMD answer is treated as a candidate and confirmed
// against the comparator rather than trusted. Without that confirmation the
// structure silently mis-sorts and this test fails.
TEST_F(AparajitaMemTableTest, MatchesSkipListUnderReverseComparator) {
  auto expected = Run(SkipList(), ReverseBytewiseComparator(), false, 5000, 1);
  auto actual = Run(Aparajita(), ReverseBytewiseComparator(), false, 5000, 1);
  ExpectIdentical(expected, actual);
}

TEST_F(AparajitaMemTableTest, AdvertisesConcurrentInsertSupport) {
  AparajitaMemTableFactory factory;
  EXPECT_TRUE(factory.IsInsertConcurrentlySupported());
  EXPECT_STREQ(factory.Name(), "AparajitaMemTableFactory");
  EXPECT_STREQ(factory.NickName(), "aparajita");
}

// The descent's hint fast path is a promise about the comparator, and the
// differential tests above cannot see it: with the fast path wrongly disabled
// every one of them still passes, only slower. So pin the answer directly.
//
// The reverse case is the one that must never drift. It is not a missed
// optimisation but a wrong descent, and it is why MatchesSkipListUnderReverse-
// Comparator above is the test that would fail if this returned true.
TEST_F(AparajitaMemTableTest, HintOrderingFollowsTheUserComparator) {
  const InternalKeyComparator bytewise(BytewiseComparator());
  const InternalKeyComparator reverse(ReverseBytewiseComparator());
  MemTable::KeyComparator bytewise_key_cmp(bytewise);
  MemTable::KeyComparator reverse_key_cmp(reverse);

#ifdef ROCKSDB_USE_RTTI
  EXPECT_TRUE(AparajitaHintOrdering(bytewise_key_cmp));
#else
  // Without RTTI the comparator cannot be identified and the fast path is off.
  // Tests only build in Debug, where RocksDB defines ROCKSDB_USE_RTTI, so this
  // branch exists to state the contract rather than because it is exercised.
  EXPECT_FALSE(AparajitaHintOrdering(bytewise_key_cmp));
#endif
  EXPECT_FALSE(AparajitaHintOrdering(reverse_key_cmp));
}

// The plugin registry generated from aparajita.mk is what makes
// --memtablerep=aparajita work in db_bench without patching RocksDB.
TEST_F(AparajitaMemTableTest, ResolvesByName) {
  ConfigOptions config_options;
  config_options.ignore_unsupported_options = false;
  std::shared_ptr<MemTableRepFactory> factory;
  ASSERT_OK(MemTableRepFactory::CreateFromString(config_options, "aparajita",
                                                 &factory));
  ASSERT_NE(factory, nullptr);
  EXPECT_STREQ(factory->Name(), "AparajitaMemTableFactory");
  EXPECT_TRUE(factory->IsInsertConcurrentlySupported());
}

}  // namespace
}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
