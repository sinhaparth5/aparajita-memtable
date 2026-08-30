// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Concurrent insert and read, at the thread counts ROADMAP.md names as the Phase 2
// exit criterion: 1, 4, 16 and 64.
//
// Built twice. The plain target runs the same logic at full speed and checks the
// results; the ThreadSanitizer target runs it instrumented and checks that no
// access pattern is a race in the first place. Passing one without the other
// proves little: TSan alone would accept a structure that loses keys, and the
// uninstrumented run alone would miss a race that happens not to have fired.

#include "aparajita/arena.hpp"
#include "aparajita/memtable.hpp"
#include "aparajita/surrogate.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <thread>
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

// TSan runs roughly an order of magnitude slower and its shadow memory grows with
// the footprint, so the instrumented build does far less work. The point of that
// run is whether a race exists, not how many keys it survived.
//
// It also scales work *down* as threads go up, which is not the usual shape and is
// worth explaining. Every thread walks the key space in the same direction at the
// same rate, so at any instant all of them are inserting into the same one or two
// nodes and serialising on that node's lock. Under TSan the losers of that race
// are not idle: every test_and_set in the backoff loop is instrumented, so
// sixty-three spinning threads burn more measured CPU than the one making
// progress, and total work grows with the square of the thread count. Holding the
// key count fixed means the 64-thread case never finishes. Racing on fewer keys
// still executes every path a race could live in, which is what this build is for;
// the uninstrumented build carries the volume.
#if defined(__SANITIZE_THREAD__) || (defined(__has_feature) && __has_feature(thread_sanitizer))
constexpr bool kSanitized = true;
constexpr int kReaderCap = 2;
constexpr const char* kMode = "thread-sanitized";
#else
constexpr bool kSanitized = false;
constexpr int kReaderCap = 32;
constexpr const char* kMode = "uninstrumented";
#endif

// The start barrier. A bare `while (!start.load()) {}` is what this replaces, and
// it was not free: sixty-six threads are created in a loop, so the ones already
// up spin at full tilt on every core while main is still creating the rest, and
// under TSan each of those loads is an instrumented event. The threads that have
// no work yet were competing with the thread whose only job was to release them.
// Yielding costs nothing at low thread counts and stops the barrier being a
// contention benchmark of its own at high ones.
void await(const std::atomic<bool>& start) {
    for (int spins = 0; !start.load(std::memory_order_acquire); ++spins) {
        if (spins >= 64) {
            std::this_thread::yield();
        }
    }
}

std::string make_key(int thread_id, int i) {
    // Interleaved rather than blocked. If each thread owned a contiguous key
    // range, writers would almost always land on different nodes and the
    // interesting case, two writers contending for one node's spinlock and one
    // of them arriving after a split, would essentially never be exercised.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "k%08d.t%02d", i * 97 + thread_id, thread_id);
    return std::string(buf);
}

int keys_for(int threads) {
    if (!kSanitized) {
        return 4000;
    }
    const int scaled = 640 / threads;
    return scaled < 10 ? 10 : scaled;
}

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

void run_at(int threads) {
    const int kKeysPerThread = keys_for(threads);
    const Clock::time_point t_begin = Clock::now();
    Arena arena;
    MemTable table(arena);

    std::vector<std::string> expect_all;
    for (int t = 0; t < threads; ++t) {
        for (int i = 0; i < kKeysPerThread; ++i) {
            expect_all.push_back(make_key(t, i));
        }
    }

    std::atomic<bool> start{false};
    std::atomic<int> reader_hits{0};
    std::vector<std::thread> pool;

    // Writers.
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            await(start);
            for (int i = 0; i < kKeysPerThread; ++i) {
                table.insert(make_key(t, i));
            }
        });
    }

    // Readers, running concurrently with the writers. They cannot assert on what
    // they find, because a key may legitimately not be inserted yet; what they
    // are here for is to put lock-free reads and concurrent writes on the same
    // memory at the same time so TSan has something to judge.
    // Capped under TSan. Ninety-six instrumented threads on eight cores spend
    // their time being descheduled rather than racing, so the cap buys coverage
    // of the same interleavings at a fraction of the runtime.
    const int readers = std::min(threads < 4 ? 1 : threads / 2, kReaderCap);
    for (int r = 0; r < readers; ++r) {
        pool.emplace_back([&, r] {
            await(start);
            int hits = 0;
            std::mt19937_64 rng(static_cast<std::uint64_t>(r) + 1);
            for (int i = 0; i < kKeysPerThread; ++i) {
                const int t = static_cast<int>(rng() % static_cast<std::uint64_t>(threads));
                const int k = static_cast<int>(rng() % static_cast<std::uint64_t>(kKeysPerThread));
                if (table.contains(make_key(t, k))) {
                    ++hits;
                }
                // Also exercise the ordered path against a moving structure.
                if ((i & 63) == 0) {
                    int n = 0;
                    for (auto it = table.begin(); it.valid() && n < 64; it.next()) {
                        ++n;
                    }
                }
            }
            reader_hits.fetch_add(hits, std::memory_order_relaxed);
        });
    }

    // Phase timing, printed on every run. The 64-thread case under TSan was
    // recorded as "does not complete" for two phases without anyone knowing which
    // part of it was slow, and a total runtime cannot distinguish a structure that
    // contends from a harness that does. Three numbers separate them: spawn is the
    // harness, race is the structure, verify is single-threaded and should track
    // key count and nothing else.
    const double spawn_ms = ms_since(t_begin);
    const Clock::time_point t_race = Clock::now();

    start.store(true, std::memory_order_release);
    for (auto& th : pool) {
        th.join();
    }
    const double race_ms = ms_since(t_race);
    const Clock::time_point t_verify = Clock::now();

    // Every key inserted must be present exactly once, and the table must still
    // be sorted. A lost update or a torn split shows up in one of these three.
    check(table.size() == expect_all.size(),
          std::to_string(threads) + " threads: size matches inserts (" +
              std::to_string(table.size()) + " vs " + std::to_string(expect_all.size()) + ")");

    for (const auto& k : expect_all) {
        if (!table.contains(k)) {
            check(false, std::to_string(threads) + " threads: key lost");
            break;
        }
    }

    std::vector<std::string> seen;
    seen.reserve(expect_all.size());
    for (auto it = table.begin(); it.valid(); it.next()) {
        seen.emplace_back(it.key());
    }
    bool sorted = true;
    for (std::size_t i = 1; i < seen.size(); ++i) {
        if (compare_keys(seen[i - 1], seen[i]) > 0) {
            sorted = false;
            break;
        }
    }
    check(sorted, std::to_string(threads) + " threads: iteration still ordered");

    std::set<std::string> expect_sorted(expect_all.begin(), expect_all.end());
    std::vector<std::string> expect_vec(expect_sorted.begin(), expect_sorted.end());
    check(seen == expect_vec, std::to_string(threads) + " threads: contents match exactly");

    // The writers have joined, so the structure is quiesced and the invariants
    // that no query can observe are safe to walk. This is the only place they get
    // checked after concurrent appends, which is where an interleaving that
    // published an order word before the slot it names would show up.
    if (const char* broken = table.check_invariants()) {
        check(false, std::to_string(threads) + " threads: " + broken);
    }

    std::printf("  %2d threads x %4d keys: %zu stored, %d reader hits, arena %zu KiB"
                "  [spawn %.1f ms, race %.1f ms, verify %.1f ms]\n",
                threads, kKeysPerThread, seen.size(), reader_hits.load(),
                arena.memory_usage() / 1024, spawn_ms, race_ms, ms_since(t_verify));
    // Flushed per case. Under ThreadSanitizer the last case takes long enough
    // that buffered output looks indistinguishable from a hang, which cost real
    // time to diagnose once already.
    std::fflush(stdout);
}

} // namespace

int main() {
    std::printf("concurrent insert (%s)\n", kMode);
    std::fflush(stdout);
    for (int threads : {1, 4, 16, 64}) {
        run_at(threads);
    }

    if (g_failures == 0) {
        std::printf("all concurrency tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
}
