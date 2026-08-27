// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

// The Phase 2 standalone structure: an ordered list of cache-line nodes with
// concurrent insert and lock-free reads.
//
// Layout follows the rule in ROADMAP.md that the bytes a probe compares sit
// contiguously in one line and everything else sits apart. NodeData opens with
// exactly 64 bytes of surrogates, so a search touches one line; the count, the
// link and the full-key views live past it and are only reached once the SIMD
// step has already narrowed the answer to a candidate.
//
// Concurrency rests on one decision. NodeData is immutable once published, and
// the link to the next node lives *inside* it rather than beside it. That is what
// makes a split atomic: replacing a node's payload swaps its contents and its
// successor together in a single release store, so a reader sees either the old
// full node or the new half-node pointing at a fresh sibling, and never a state
// where a key is duplicated across both or missing from both. Putting `next` in a
// separate atomic would need two stores and would expose exactly those states.
//
// Readers are therefore lock-free and never write. Writers take a per-node
// spinlock, which is a deliberate narrowing of the "lock-free" goal in CLAUDE.md
// and is recorded in docs/phase2-design.md.

#include "aparajita/arena.hpp"
#include "aparajita/node.hpp"
#include "aparajita/search.hpp"
#include "aparajita/surrogate.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <string_view>
#include <thread>

namespace aparajita {

struct ListNode;  // the list node, distinct from the SIMD Node in node.hpp

// The compared surrogates occupy the first cache line exactly. Everything after
// that offset is cold: it is read only after a SIMD hit has produced a candidate.
struct alignas(kCacheLine) NodeData {
    std::uint32_t surrogates[kNodeKeys];

    std::uint32_t count;
    // Bytes shared by every key in this node, stripped before the surrogate is
    // taken. See surrogate_at() for why the lane would otherwise be useless on
    // real keys.
    std::uint32_t prefix_len;
    ListNode* next;
    std::string_view keys[kNodeKeys];
};

static_assert(sizeof(std::uint32_t) * kNodeKeys == kCacheLine,
              "the compared surrogates must fill exactly one cache line");
static_assert(offsetof(NodeData, surrogates) == 0,
              "surrogates must open the struct or they will not be line-aligned");
static_assert(offsetof(NodeData, count) == kCacheLine,
              "cold fields must begin on the second line, not share the compared one");
static_assert(alignof(NodeData) == kCacheLine, "node data must be cache-line aligned");

// Height of the index. Four is the branching factor, so twelve levels indexes
// roughly 4^12 nodes, which at sixteen keys each is far past any memtable RocksDB
// will flush.
inline constexpr int kMaxHeight = 12;

// Padded to a full line so two nodes never share one and writers to different
// nodes never invalidate each other's line.
//
// The forward pointers here are levels 1 and above only. Level 0 lives inside
// NodeData, because that is what lets a split swap contents and successor in one
// store. Splitting that link out into this struct to keep all levels together
// would cost exactly the atomicity the design depends on.
//
// Everything in this tower is therefore an accelerator and never the truth. A
// stale or missing tower pointer costs a longer walk, never a wrong answer: the
// search only follows a pointer to a node whose first key is <= the target, and
// since nodes are ordered and never removed, any node it skips cannot have held
// the target.
struct alignas(kCacheLine) ListNode {
    std::atomic<NodeData*> data{nullptr};
    std::atomic_flag write_lock = ATOMIC_FLAG_INIT;
    std::uint8_t height{1};
    std::atomic<ListNode*> tower[kMaxHeight] = {};
};

static_assert(alignof(ListNode) == kCacheLine, "nodes must not share a cache line");

class MemTable {
public:
    explicit MemTable(Arena& arena) : arena_(arena) {
        head_ = arena_.create<ListNode>();
        head_->height = kMaxHeight;
        NodeData* d = fresh_data();
        head_->data.store(d, std::memory_order_release);
    }

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    // Copies the key into the arena, so the caller's buffer need not outlive the
    // call. Safe to call from many threads at once.
    void insert(std::string_view key) {
        char* copy = arena_.allocate(key.size() == 0 ? 1 : key.size());
        if (!key.empty()) {
            std::memcpy(copy, key.data(), key.size());
        }
        const std::string_view stored(copy, key.size());

        for (;;) {
            ListNode* cur = descend(stored);

            // Contention is per node, so two writers only meet here when they
            // target the same sixteen keys. That makes a short spin the right
            // first move, but not the only one: RocksDB routinely runs more
            // writers than cores, and a pure spin there burns every other thread's
            // slice while the lock holder waits to be scheduled. Pause first, then
            // yield and let the holder finish.
            for (int spins = 0; cur->write_lock.test_and_set(std::memory_order_acquire); ++spins) {
                if (spins < 64) {
#if APARAJITA_X86
                    _mm_pause();
#endif
                } else {
                    std::this_thread::yield();
                }
            }

            NodeData* d = cur->data.load(std::memory_order_acquire);

            // The node may have split under us between the descent and the lock,
            // in which case this key now belongs to a sibling. Drop the lock and
            // walk again rather than inserting into the wrong node.
            if (!belongs_here(d, stored)) {
                cur->write_lock.clear(std::memory_order_release);
                continue;
            }

            if (d->count < kNodeKeys) {
                insert_into(cur, d, stored);
            } else {
                split_and_insert(cur, d, stored);
            }

            cur->write_lock.clear(std::memory_order_release);
            return;
        }
    }

    // Lock-free. Reads only immutable payloads, so it never blocks a writer and a
    // writer never blocks it.
    bool contains(std::string_view key) const {
        const ListNode* cur = descend(key);
        const NodeData* d = cur->data.load(std::memory_order_acquire);

        // Every key in the node shares its prefix, so a key that does not share
        // it cannot be here. That is a correctness requirement before it is an
        // optimisation: surrogates taken at this node's offset are only ordered
        // among keys that share the prefix.
        if (!shares_prefix(d, key)) {
            return false;
        }
        const std::uint32_t s = surrogate_at(key, d->prefix_len);

        int i = lower_bound_surrogate(d, s);
        // Equal surrogates are contiguous because the node is sorted, so the
        // candidate run is a short scan from the SIMD answer. This is where a
        // shared four-byte prefix turns the search linear, which is what
        // collision_report measures.
        for (; i < static_cast<int>(d->count) && d->surrogates[i] == s; ++i) {
            if (compare_keys(d->keys[i], key) == 0) {
                return true;
            }
        }
        return false;
    }

    // Ordered forward iteration. Yields keys in comparator order, which is the
    // requirement that ruled out the sort-at-flush design.
    class Iterator {
    public:
        explicit Iterator(const MemTable& t)
            : node_(t.head_), data_(t.head_->data.load(std::memory_order_acquire)), pos_(0) {
            skip_empty();
        }

        bool valid() const { return data_ != nullptr && pos_ < static_cast<int>(data_->count); }
        std::string_view key() const { return data_->keys[pos_]; }

        void next() {
            ++pos_;
            skip_empty();
        }

    private:
        void skip_empty() {
            while (data_ != nullptr && pos_ >= static_cast<int>(data_->count)) {
                node_ = data_->next;
                data_ = node_ ? node_->data.load(std::memory_order_acquire) : nullptr;
                pos_ = 0;
            }
        }

        const ListNode* node_;
        const NodeData* data_;
        int pos_;
    };

    Iterator begin() const { return Iterator(*this); }

    std::size_t size() const {
        std::size_t n = 0;
        for (auto it = begin(); it.valid(); it.next()) {
            ++n;
        }
        return n;
    }

private:
    static int lower_bound_surrogate(const NodeData* d, std::uint32_t s) {
        // The kernels take a ListNode, and NodeData opens with the identical 64-byte
        // surrogate array at offset 0, which the static_asserts above enforce.
        const ::aparajita::Node* line = reinterpret_cast<const ::aparajita::Node*>(d->surrogates);
        static const LowerBoundFn fn = lower_bound_dispatch();
        return fn(*line, s);
    }

    NodeData* fresh_data() {
        NodeData* d = reinterpret_cast<NodeData*>(
            arena_.allocate(sizeof(NodeData), alignof(NodeData)));
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            d->surrogates[i] = kEmptyKey;
            d->keys[i] = std::string_view();
        }
        d->count = 0;
        d->prefix_len = 0;
        d->next = nullptr;
        return d;
    }

    // True when `n` begins at or below `key`, and so may still be walked past.
    static bool starts_at_or_below(const ListNode* n, std::string_view key) {
        const NodeData* d = n->data.load(std::memory_order_acquire);
        return d->count > 0 && compare_keys(d->keys[0], key) <= 0;
    }

    // The last node whose first key is <= `key`. Nodes are ordered, so this is the
    // only node that may contain it. Descends the tower first, then finishes on
    // the authoritative level-0 links.
    ListNode* descend(std::string_view key) const {
        ListNode* cur = head_;
        for (int lv = kMaxHeight - 1; lv >= 1; --lv) {
            for (;;) {
                ListNode* nx = cur->tower[lv].load(std::memory_order_acquire);
                if (nx != nullptr && starts_at_or_below(nx, key)) {
                    cur = nx;
                    continue;
                }
                break;
            }
        }
        for (;;) {
            const NodeData* d = cur->data.load(std::memory_order_acquire);
            ListNode* nx = d->next;
            if (nx != nullptr && starts_at_or_below(nx, key)) {
                cur = nx;
                continue;
            }
            return cur;
        }
    }

    // The last node at or above level `lv` whose first key is <= `key`. Used to
    // find where a freshly split sibling belongs in each tower level.
    ListNode* pred_at_level(int lv, std::string_view key) const {
        ListNode* cur = head_;
        for (int l = kMaxHeight - 1; l >= lv; --l) {
            for (;;) {
                ListNode* nx = cur->tower[l].load(std::memory_order_acquire);
                if (nx != nullptr && starts_at_or_below(nx, key)) {
                    cur = nx;
                    continue;
                }
                break;
            }
        }
        return cur;
    }

    // Branching factor of four, matching RocksDB's skiplist.
    static int random_height() {
        thread_local std::mt19937 rng(std::random_device{}());
        int h = 1;
        while (h < kMaxHeight && (rng() & 3u) == 0u) {
            ++h;
        }
        return h;
    }

    // Best-effort. If a level cannot be linked the node is still reachable on
    // level 0, so this loop is allowed to give up without breaking anything.
    void link_tower(ListNode* node, std::string_view first_key) {
        for (int lv = 1; lv < node->height; ++lv) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                ListNode* pred = pred_at_level(lv, first_key);
                if (pred == node) {
                    break;
                }
                ListNode* nx = pred->tower[lv].load(std::memory_order_acquire);
                if (nx != nullptr && starts_at_or_below(nx, first_key)) {
                    continue;  // someone linked ahead of us; re-find
                }
                node->tower[lv].store(nx, std::memory_order_release);
                if (pred->tower[lv].compare_exchange_weak(nx, node,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
                    break;
                }
            }
        }
    }

    static bool belongs_here(const NodeData* d, std::string_view key) {
        if (d->next == nullptr) {
            return true;
        }
        const NodeData* nd = d->next->data.load(std::memory_order_acquire);
        return nd->count == 0 || compare_keys(nd->keys[0], key) > 0;
    }

    static bool shares_prefix(const NodeData* d, std::string_view key) {
        if (d->count == 0 || d->prefix_len == 0) {
            return true;
        }
        if (key.size() < d->prefix_len) {
            return false;
        }
        return std::memcmp(key.data(), d->keys[0].data(), d->prefix_len) == 0;
    }

    // Position at which `key` keeps the node sorted.
    //
    // Two paths, and the slow one is not an optimisation detail. A key that does
    // not share the node's prefix has no meaningful surrogate at this node's
    // offset, so the SIMD answer would be ordered against a different alphabet
    // and would place the key wrongly. The descent guarantees such a key still
    // belongs in this node, so it is placed by full comparison instead. It is
    // rare: it happens only at the boundary where a node's shared prefix ends.
    static int sorted_position(const NodeData* d, std::string_view key) {
        if (!shares_prefix(d, key)) {
            int p = 0;
            while (p < static_cast<int>(d->count) && compare_keys(d->keys[p], key) < 0) {
                ++p;
            }
            return p;
        }
        const std::uint32_t s = surrogate_at(key, d->prefix_len);
        int p = lower_bound_surrogate(d, s);
        while (p < static_cast<int>(d->count) && d->surrogates[p] == s &&
               compare_keys(d->keys[p], key) < 0) {
            ++p;
        }
        return p;
    }

    // Fills a fresh payload from a sorted key run. The shared prefix is recomputed
    // from the first and last key every time rather than carried forward, because
    // an insert at either end can shorten it and a stale prefix would silently
    // mis-order every surrogate in the node. Recomputing is affordable precisely
    // because copy-on-write already rebuilds the node.
    static void fill(NodeData* d, const std::string_view* keys, int n, ListNode* next) {
        d->count = static_cast<std::uint32_t>(n);
        d->next = next;
        d->prefix_len = n > 1 ? static_cast<std::uint32_t>(
                                    common_prefix_len(keys[0], keys[n - 1]))
                              : 0;
        for (int i = 0; i < n; ++i) {
            d->keys[i] = keys[i];
            d->surrogates[i] = surrogate_at(keys[i], d->prefix_len);
        }
        for (int i = n; i < static_cast<int>(kNodeKeys); ++i) {
            d->keys[i] = std::string_view();
            d->surrogates[i] = kEmptyKey;
        }
    }

    // Merges `key` into the node's sorted run, writing the result to `out`.
    static int merge(const NodeData* d, std::string_view key, std::string_view* out) {
        const int p = sorted_position(d, key);
        for (int i = 0; i < p; ++i) {
            out[i] = d->keys[i];
        }
        out[p] = key;
        for (int i = p; i < static_cast<int>(d->count); ++i) {
            out[i + 1] = d->keys[i];
        }
        return static_cast<int>(d->count) + 1;
    }

    void insert_into(ListNode* cur, const NodeData* d, std::string_view key) {
        std::string_view merged[kNodeKeys + 1];
        const int n = merge(d, key, merged);
        NodeData* nd = fresh_data();
        fill(nd, merged, n, d->next);
        cur->data.store(nd, std::memory_order_release);
    }

    // A full node becomes two. The right half is published as a brand new node
    // first, then a single release store swaps this node's payload to the left
    // half *and* its link to the new sibling at the same instant. A reader either
    // sees the old sixteen-key node or the new pair, never a torn state.
    void split_and_insert(ListNode* cur, const NodeData* d, std::string_view key) {
        constexpr int kMid = static_cast<int>(kNodeKeys) / 2;
        std::string_view merged[kNodeKeys + 1];
        const int total = merge(d, key, merged);

        ListNode* sibling = arena_.create<ListNode>();
        NodeData* right = fresh_data();
        NodeData* left = fresh_data();

        fill(right, merged + kMid, total - kMid, d->next);
        fill(left, merged, kMid, sibling);

        // Everything about the sibling is settled before it becomes reachable,
        // height included. Writing a plain field after publication would be a
        // race even if no reader happens to look at it today.
        sibling->height = static_cast<std::uint8_t>(random_height());
        sibling->data.store(right, std::memory_order_release);

        cur->data.store(left, std::memory_order_release);

        // Only now, once the sibling is reachable on level 0, is it safe to index
        // it. A tower pointer to a node no one can reach on level 0 would let a
        // search jump to it and then walk forward off the real list.
        link_tower(sibling, right->keys[0]);
    }

    Arena& arena_;
    ListNode* head_;
};

} // namespace aparajita
