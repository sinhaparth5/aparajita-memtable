// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Single-threaded correctness for the Phase 2 structure. The concurrency tests
// live in test_concurrent.cpp; everything here would still have to hold if the
// structure were never touched by a second thread.

#include "aparajita/arena.hpp"
#include "aparajita/memtable.hpp"
#include "aparajita/surrogate.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace aparajita;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::string random_key(std::mt19937_64& rng, std::size_t max_len = 12) {
    std::uniform_int_distribution<std::size_t> len(1, max_len);
    std::uniform_int_distribution<int> byte(0, 255);
    std::string s;
    const std::size_t n = len(rng);
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(static_cast<char>(byte(rng)));
    }
    return s;
}

} // namespace

int main() {
    // -----------------------------------------------------------------------
    // surrogate extraction
    // -----------------------------------------------------------------------
    check(surrogate("") == 0u, "empty key surrogates to 0");
    check(surrogate("a") == 0x61000000u, "one-byte key sits in the high byte");
    check(surrogate("ab") == 0x61620000u, "two-byte key pads right with zeros");
    check(surrogate("abcd") == 0x61626364u, "four bytes fill the lane big-endian");
    check(surrogate("abcde") == 0x61626364u, "bytes past the fourth are ignored");
    check(surrogate("abcd") == surrogate("abcdZZZ"), "surrogate is a four-byte prefix");

    // A prefix must sort below any key extending it, which is what the zero
    // padding is for.
    check(surrogate("ab") < surrogate("abc"), "prefix sorts below its extension");

    // -----------------------------------------------------------------------
    // descent hints
    // -----------------------------------------------------------------------
    check(descent_hint("") == 0u, "empty key hints to 0");
    check(descent_hint("a") == 0x6100000000000000ull, "one byte sits in the top byte");
    check(descent_hint("abcdefgh") == 0x6162636465666768ull, "eight bytes fill the word");
    check(descent_hint("abcdefgh") == descent_hint("abcdefghZZZ"),
          "the hint is an eight-byte prefix and ignores the rest");
    check(descent_hint("ab") < descent_hint("abc"), "hint keeps prefix order");

    // The tie the descent must never resolve on its own. These two keys have the
    // same hint and are not equal, and compare_keys puts the shorter one first;
    // a hop that trusted the hint here would order them arbitrarily.
    check(descent_hint(std::string("abc")) == descent_hint(std::string("abc\0", 4)),
          "zero padding is indistinguishable from a trailing zero byte");
    check(compare_keys(std::string_view("abc"), std::string_view("abc\0", 4)) < 0,
          "and the comparator, unlike the hint, still separates them");

    // The alias that forced an explicit occupancy count.
    {
        const char ff[] = {'\xFF', '\xFF', '\xFF', '\xFF'};
        check(aliases_empty_sentinel(surrogate(ff, 4)),
              "four 0xFF bytes alias the empty-slot sentinel");
        check(!aliases_empty_sentinel(surrogate("abcd")), "an ordinary key does not alias");
    }

    // The load-bearing invariant of the whole ordered design: surrogate order
    // must never contradict key order. It may tie where keys differ past the
    // fourth byte, but it must never invert.
    {
        std::mt19937_64 rng(4242);
        for (int i = 0; i < 200000; ++i) {
            const std::string a = random_key(rng);
            const std::string b = random_key(rng);
            const int kc = compare_keys(a, b);
            const std::uint32_t sa = surrogate(a), sb = surrogate(b);
            if (kc < 0 && !(sa <= sb)) {
                check(false, "surrogate inverted a key ordering");
                break;
            }
            if (sa < sb && !(kc < 0)) {
                check(false, "surrogate ordered two keys the comparator did not");
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // memtable: insert, lookup, ordered iteration
    // -----------------------------------------------------------------------
    {
        Arena arena;
        MemTable t(arena);
        check(!t.contains("anything"), "empty table contains nothing");
        check(t.size() == 0, "empty table has size 0");
    }

    // Enough keys to force many splits, inserted in a shuffled order so the
    // structure cannot get away with an append-only path.
    {
        Arena arena;
        MemTable t(arena);
        std::mt19937_64 rng(7);
        std::vector<std::string> keys;
        std::set<std::string> unique;
        while (unique.size() < 5000) {
            std::string k = random_key(rng);
            if (unique.insert(k).second) {
                keys.push_back(k);
            }
        }
        std::shuffle(keys.begin(), keys.end(), rng);
        for (const auto& k : keys) {
            t.insert(k);
        }

        check(t.size() == keys.size(), "size matches the number of inserts");

        for (const auto& k : keys) {
            if (!t.contains(k)) {
                check(false, "inserted key not found");
                break;
            }
        }

        // Iteration must be sorted, which is the requirement that ruled out
        // sort-at-flush.
        {
            std::vector<std::string> seen;
            for (auto it = t.begin(); it.valid(); it.next()) {
                seen.emplace_back(it.key());
            }
            check(seen.size() == keys.size(), "iteration yields every key");
            bool sorted = true;
            for (std::size_t i = 1; i < seen.size(); ++i) {
                if (compare_keys(seen[i - 1], seen[i]) > 0) {
                    sorted = false;
                    break;
                }
            }
            check(sorted, "iteration yields keys in comparator order");

            std::vector<std::string> expect(unique.begin(), unique.end());
            check(seen == expect, "iteration matches a sorted reference exactly");
        }

        // Absent keys must miss even when they share a surrogate with a present
        // one, which is the case a lossy lane gets wrong.
        {
            std::mt19937_64 r2(99);
            int checked = 0;
            for (int i = 0; i < 20000 && checked < 2000; ++i) {
                const std::string k = random_key(r2);
                if (unique.count(k) == 0) {
                    ++checked;
                    if (t.contains(k)) {
                        check(false, "absent key reported present");
                        break;
                    }
                }
            }
        }
    }

    // Keys sharing a four-byte prefix all collapse to one surrogate, so the
    // candidate run inside a node is maximal. This is the degenerate case the
    // design doc calls out, and it must stay correct even when it stops being fast.
    {
        Arena arena;
        MemTable t(arena);
        std::vector<std::string> keys;
        for (int i = 0; i < 500; ++i) {
            keys.push_back("PFX:" + std::to_string(i));
        }
        for (const auto& k : keys) {
            t.insert(k);
        }
        for (const auto& k : keys) {
            if (!t.contains(k)) {
                check(false, "shared-prefix key not found");
                break;
            }
        }
        check(t.size() == keys.size(), "shared-prefix keys all stored");
        check(!t.contains("PFX:99999"), "absent shared-prefix key misses");
    }

    // Keys shorter than the lane, including the empty key.
    {
        Arena arena;
        MemTable t(arena);
        const char* shorts[] = {"", "a", "ab", "abc", "abcd", "b", "\xFF", "\xFF\xFF\xFF\xFF"};
        for (const char* s : shorts) {
            t.insert(s);
        }
        for (const char* s : shorts) {
            if (!t.contains(s)) {
                check(false, std::string("short key not found: len ") +
                                 std::to_string(std::string(s).size()));
            }
        }
        check(t.size() == std::size(shorts), "all short keys stored");
    }

    // Iteration in both directions, and the two seeks.
    //
    // These are the operations RocksDB's MemTableRep::Iterator requires and the
    // ones the Phase 2 structure never exercised: it only ever walked forward
    // from the head. prev() in particular has no back-pointer to follow and
    // re-descends instead, so a node boundary is the interesting case and a
    // 5000-key table crosses several hundred of them.
    {
        Arena arena;
        MemTable t(arena);
        std::mt19937_64 rng(20260828);
        std::set<std::string> ref;
        while (ref.size() < 5000) {
            ref.insert(random_key(rng));
        }
        for (const auto& k : ref) {
            t.insert(k);
        }
        const std::vector<std::string> sorted(ref.begin(), ref.end());

        // Forward from the front.
        {
            std::size_t i = 0;
            auto it = t.begin();
            for (; it.valid(); it.next(), ++i) {
                if (i >= sorted.size() || it.key() != sorted[i]) {
                    check(false, "forward iteration diverged at " + std::to_string(i));
                    break;
                }
            }
            check(i == sorted.size(), "forward iteration visited every key");
        }

        // Backward from the end must reproduce the same order reversed.
        {
            auto it = t.begin();
            it.seek_to_last();
            check(it.valid() && it.key() == sorted.back(), "seek_to_last lands on the last key");
            std::size_t i = sorted.size();
            for (; it.valid(); it.prev()) {
                if (i == 0 || it.key() != sorted[i - 1]) {
                    check(false, "backward iteration diverged at " + std::to_string(i));
                    break;
                }
                --i;
            }
            check(i == 0, "backward iteration visited every key");
        }

        // next() then prev() must return to where it started, across boundaries.
        {
            auto it = t.begin();
            for (std::size_t i = 0; i + 1 < sorted.size(); ++i) {
                it.next();
                it.prev();
                if (it.key() != sorted[i]) {
                    check(false, "next/prev round trip failed at " + std::to_string(i));
                    break;
                }
                it.next();
            }
        }

        // seek() lands on the first key >= target, seek_for_prev() on the last
        // key <= target. Probed with keys that are present and keys that are not.
        {
            std::mt19937_64 probe(99);
            for (int trial = 0; trial < 2000; ++trial) {
                const bool use_present = (trial % 2) == 0;
                std::string target = use_present
                                         ? sorted[probe() % sorted.size()]
                                         : random_key(probe);

                const auto lb = std::lower_bound(sorted.begin(), sorted.end(), target);
                auto it = t.begin();
                it.seek(target);
                if (lb == sorted.end()) {
                    check(!it.valid(), "seek past the last key is invalid");
                } else {
                    check(it.valid() && it.key() == *lb, "seek lands on lower_bound");
                }

                auto it2 = t.begin();
                it2.seek_for_prev(target);
                if (lb != sorted.end() && *lb == target) {
                    check(it2.valid() && it2.key() == target, "seek_for_prev finds an exact key");
                } else if (lb == sorted.begin()) {
                    check(!it2.valid(), "seek_for_prev before the first key is invalid");
                } else {
                    check(it2.valid() && it2.key() == *(lb - 1), "seek_for_prev lands below");
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Keys the descent hint cannot decide
    // -----------------------------------------------------------------------
    //
    // descend() answers a tower hop from an eight-byte hint and only falls back
    // to the comparator when two hints tie. Every test above draws random keys,
    // where a tie is rare, so they would all still pass if the fallback were
    // deleted and ties were resolved by the hint alone.
    //
    // These keys make the tie universal. A shared table-or-tenant prefix longer
    // than eight bytes is the shape bench/collision_report.cpp found in six of
    // seven realistic distributions, and it gives every node in the structure an
    // identical hint. Nothing here can be answered by the fast path, so this is
    // the workload that decides whether the slow path is still correct.
    {
        Arena arena;
        MemTable t(arena);

        const std::string prefix = "tenant:00042:";  // thirteen shared bytes
        std::mt19937_64 rng(20260830);
        std::set<std::string> ref;
        while (ref.size() < 3000) {
            std::string k = prefix + random_key(rng, 8);
            ref.insert(k);
        }
        std::vector<std::string> sorted(ref.begin(), ref.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::string& a, const std::string& b) {
                      return compare_keys(a, b) < 0;
                  });

        std::vector<std::string> shuffled = sorted;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        for (const std::string& k : shuffled) {
            t.insert(k);
        }

        check(t.size() == sorted.size(), "shared-prefix keys all stored");
        for (const std::string& k : sorted) {
            check(t.contains(k), "shared-prefix key found again");
        }
        check(!t.contains(prefix), "the bare shared prefix was never inserted");

        std::size_t i = 0;
        bool ordered = true;
        for (auto it = t.begin(); it.valid(); it.next(), ++i) {
            if (i >= sorted.size() || it.key() != sorted[i]) {
                ordered = false;
                break;
            }
        }
        check(ordered && i == sorted.size(),
              "shared-prefix keys iterate in comparator order");

        // Seeks over the same keys, which reach the descent through a different
        // entry point than contains() does.
        for (std::size_t probe = 0; probe < 500; ++probe) {
            const std::string target = (probe & 1u)
                                           ? sorted[rng() % sorted.size()]
                                           : prefix + random_key(rng, 8);
            auto lb = std::lower_bound(sorted.begin(), sorted.end(), target,
                                       [](const std::string& a, const std::string& b) {
                                           return compare_keys(a, b) < 0;
                                       });
            auto it = t.begin();
            it.seek(target);
            if (lb == sorted.end()) {
                check(!it.valid(), "shared-prefix seek past the end is invalid");
            } else {
                check(it.valid() && it.key() == *lb, "shared-prefix seek lands on lower_bound");
            }
        }
    }

    // The other tie the hint cannot see: keys that agree on their first eight
    // bytes and diverge only afterwards, including the zero-padding case where
    // one key is a prefix of the other.
    {
        Arena arena;
        MemTable t(arena);
        const std::vector<std::string> keys = {
            std::string("abcdefgh"),
            std::string("abcdefgh ", 9),
            std::string("abcdefgh  ", 10),
            std::string("abcdefghi"),
            std::string("abcdefghÿ", 9),
        };
        std::vector<std::string> sorted = keys;
        std::sort(sorted.begin(), sorted.end(),
                  [](const std::string& a, const std::string& b) {
                      return compare_keys(a, b) < 0;
                  });
        for (const std::string& k : keys) {
            t.insert(k);
        }
        check(t.size() == keys.size(), "keys tying in eight bytes are all distinct");
        for (const std::string& k : keys) {
            check(t.contains(k), "eight-byte tie found again");
        }
        std::size_t i = 0;
        bool ordered = true;
        for (auto it = t.begin(); it.valid(); it.next(), ++i) {
            if (i >= sorted.size() || it.key() != sorted[i]) {
                ordered = false;
                break;
            }
        }
        check(ordered && i == sorted.size(), "eight-byte ties iterate in comparator order");
    }

    if (g_failures == 0) {
        std::printf("all memtable tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
