// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

// The ordered concurrent structure: a list of cache-line nodes with concurrent
// insert and lock-free reads.
//
// Layout follows the rule in ROADMAP.md that the bytes a probe compares sit
// contiguously in one line and everything else sits apart. NodeData opens with
// exactly 64 bytes of surrogates, so a search touches one line; the count, the
// link and the full keys live past it and are only reached once the SIMD step
// has already narrowed the answer to a candidate.
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
//
// ---------------------------------------------------------------------------
// Why this is a template
// ---------------------------------------------------------------------------
//
// Phase 2 proved the structure on plain keys owned by our own arena. Phase 3
// puts it under RocksDB, where three things change and none of them touch the
// algorithm above:
//
//   - an entry is a bare `const char*` into RocksDB's arena, not a 16-byte
//     string_view, because MemTableRep::Allocate hands key storage to the caller;
//   - order is decided by a RocksDB KeyComparator, which is a virtual call on a
//     length-prefixed internal key rather than a memcmp on a user key;
//   - memory comes from a rocksdb::Allocator rather than from aparajita::Arena.
//
// A Traits policy carries exactly those three differences, so the split, the
// copy-on-write publication and the tower — the parts that were hard to get
// right and are covered by tests/test_concurrent.cpp — exist once and are shared
// verbatim by both. A Traits provides:
//
//   using Entry;                                   stored, stable for the
//                                                  structure's lifetime
//   using Target;                                  a transient lookup key
//   char* allocate(size_t bytes, size_t align);
//   std::string_view order_bytes(Entry) const;     bytes feeding prefix+surrogate
//   std::string_view order_bytes(Target) const;    same, for a lookup key
//   int compare(Entry, Entry) const;               authoritative order
//   int compare(Entry, Target) const;              same, against a lookup key
//   static Entry null_entry();
//   bool hint_ordering() const;                    see below
//
// When Entry and Target are the same type the two overloads collapse into one,
// which is the case for both traits that exist today.
//
// hint_ordering() is the one member that is a promise rather than a mechanism.
// It asserts that comparing two keys' order bytes bytewise agrees in sign with
// compare() whenever those bytes differ in their first eight, which is what lets
// descend() answer a tower hop from an integer in the node header instead of a
// virtual call on the key. It is true for a memcmp order and for RocksDB's
// default bytewise user comparator, and false for anything else, including a
// reverse comparator. Getting it wrong is not a slow path, it is a wrong answer,
// so a traits that cannot establish the property must return false; the descent
// then falls back to the comparator at every hop and is merely slower.

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

template <class Traits>
struct BasicListNode;

// The compared surrogates occupy the first cache line exactly. Everything after
// that offset is cold: it is read only after a SIMD hit has produced a candidate.
template <class Traits>
struct alignas(kCacheLine) BasicNodeData {
    using Entry = typename Traits::Entry;

    std::uint32_t surrogates[kNodeKeys];

    std::uint32_t count;
    // Bytes shared by every key in this node, stripped before the surrogate is
    // taken. See surrogate_at() for why the lane would otherwise be useless on
    // real keys.
    std::uint32_t prefix_len;
    BasicListNode<Traits>* next;
    Entry keys[kNodeKeys];
};

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
//
// first_hint is the descent's whole point. A hop used to cost two dependent
// misses and a virtual call: load the successor's NodeData, reach past the
// surrogate line to keys[0], then hand a length-prefixed internal key to the
// comparator. The hint answers the same question from a 64-bit integer sitting in
// the node header the hop has already loaded, and only ties fall through to the
// old path. See descent_hint() for what it holds and BasicMemTable::descend() for
// why a tie is the only case that can be wrong.
//
// It is written before the node is published and then, for every node except the
// head, never again: descend() returns the last node whose first key is <= the
// key being inserted, so an insert into a non-head node always sorts at position
// 1 or later, and a split leaves the left half's first key alone. The head is the
// sole exception, because it is what descend() falls back to for a key below
// everything in the structure. That is harmless for a different reason: the head
// is where every descent starts and is never a node a hop moves *to*, so its hint
// is never read.
template <class Traits>
struct alignas(kCacheLine) BasicListNode {
    std::atomic<BasicNodeData<Traits>*> data{nullptr};
    std::atomic<std::uint64_t> first_hint{0};
    std::atomic_flag write_lock = ATOMIC_FLAG_INIT;
    std::uint8_t height{1};
    std::atomic<BasicListNode<Traits>*> tower[kMaxHeight] = {};
};

template <class Traits>
class BasicMemTable {
public:
    using Entry = typename Traits::Entry;
    using Target = typename Traits::Target;
    using NodeData = BasicNodeData<Traits>;
    using ListNode = BasicListNode<Traits>;

    static_assert(sizeof(std::uint32_t) * kNodeKeys == kCacheLine,
                  "the compared surrogates must fill exactly one cache line");
    static_assert(offsetof(NodeData, surrogates) == 0,
                  "surrogates must open the struct or they will not be line-aligned");
    static_assert(offsetof(NodeData, count) == kCacheLine,
                  "cold fields must begin on the second line, not share the compared one");
    static_assert(alignof(NodeData) == kCacheLine, "node data must be cache-line aligned");
    static_assert(alignof(ListNode) == kCacheLine, "nodes must not share a cache line");
    static_assert(offsetof(ListNode, first_hint) < kCacheLine,
                  "the descent hint must share the line a hop already loads, or it "
                  "trades a comparator call for the cache miss it was removing");

    explicit BasicMemTable(Traits traits)
        : traits_(traits), hints_(traits_.hint_ordering()) {
        head_ = create_node();
        head_->height = kMaxHeight;
        publish(head_, fresh_data());
    }

    BasicMemTable(const BasicMemTable&) = delete;
    BasicMemTable& operator=(const BasicMemTable&) = delete;

    const Traits& traits() const { return traits_; }

    // `entry` must already point at storage that outlives the structure. Safe to
    // call from many threads at once.
    void insert_entry(Entry entry) {
        const std::string_view bytes = traits_.order_bytes(entry);

        for (;;) {
            ListNode* cur = descend(entry, bytes);

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
            if (!belongs_here(d, entry)) {
                cur->write_lock.clear(std::memory_order_release);
                continue;
            }

            if (d->count < kNodeKeys) {
                insert_into(cur, d, entry, bytes);
            } else {
                split_and_insert(cur, d, entry, bytes);
            }

            cur->write_lock.clear(std::memory_order_release);
            return;
        }
    }

    // Lock-free. Reads only immutable payloads, so it never blocks a writer and a
    // writer never blocks it.
    bool contains_target(Target key) const {
        const std::string_view bytes = traits_.order_bytes(key);
        const ListNode* cur = descend(key, bytes);
        const NodeData* d = cur->data.load(std::memory_order_acquire);

        const int p = sorted_position(d, key, bytes);
        return p < static_cast<int>(d->count) && traits_.compare(d->keys[p], key) == 0;
    }

    // Ordered iteration. Yields keys in comparator order, which is the
    // requirement that ruled out the sort-at-flush design.
    //
    // Backward movement deserves a note. There are no back-pointers: level 0 lives
    // inside the immutable payload precisely so a split is one store, and adding a
    // reverse link would need a second store to the node being split away from and
    // reintroduce the torn state that design avoids. prev() therefore re-descends
    // to find the predecessor node, which is what RocksDB's own InlineSkipList
    // does for Prev(). Within a node it is a decrement, so the descent is paid
    // once per sixteen keys rather than once per key.
    class Iterator {
    public:
        Iterator() = default;
        // Constructed unpositioned and therefore invalid, matching
        // MemTableRep::Iterator, whose contract is that a fresh iterator is not
        // valid until a Seek. Use begin() for an iterator already on the first key.
        explicit Iterator(const BasicMemTable& t) : table_(&t) {}

        bool valid() const {
            return data_ != nullptr && pos_ >= 0 && pos_ < static_cast<int>(data_->count);
        }
        Entry key() const { return data_->keys[pos_]; }

        void next() {
            ++pos_;
            skip_forward();
        }

        void prev() {
            if (pos_ > 0) {
                --pos_;
                return;
            }
            skip_backward();
        }

        void seek_to_first() {
            node_ = table_->head_;
            data_ = node_->data.load(std::memory_order_acquire);
            pos_ = 0;
            skip_forward();
        }

        void seek_to_last() {
            node_ = table_->last_node();
            data_ = node_->data.load(std::memory_order_acquire);
            pos_ = static_cast<int>(data_->count) - 1;
            skip_backward_if_empty();
        }

        // First entry >= target.
        void seek(Target target) {
            const std::string_view bytes = table_->traits_.order_bytes(target);
            node_ = table_->descend(target, bytes);
            data_ = node_->data.load(std::memory_order_acquire);
            pos_ = table_->sorted_position(data_, target, bytes);
            skip_forward();
        }

        // Last entry <= target.
        void seek_for_prev(Target target) {
            seek(target);
            if (!valid()) {
                seek_to_last();
                return;
            }
            if (table_->traits_.compare(data_->keys[pos_], target) > 0) {
                prev();
            }
        }

    private:
        // A node can legitimately be empty (the head always is), so advancing may
        // have to cross several before it lands on a key.
        void skip_forward() {
            while (data_ != nullptr && pos_ >= static_cast<int>(data_->count)) {
                node_ = data_->next;
                data_ = node_ ? node_->data.load(std::memory_order_acquire) : nullptr;
                pos_ = 0;
            }
        }

        void skip_backward() {
            for (;;) {
                const ListNode* p = table_->pred_node(node_, data_);
                if (p == nullptr) {
                    data_ = nullptr;  // ran off the front
                    return;
                }
                node_ = p;
                data_ = p->data.load(std::memory_order_acquire);
                pos_ = static_cast<int>(data_->count) - 1;
                if (pos_ >= 0) {
                    return;
                }
            }
        }

        void skip_backward_if_empty() {
            if (pos_ < 0) {
                skip_backward();
            }
        }

        const BasicMemTable* table_{nullptr};
        const ListNode* node_{nullptr};
        const NodeData* data_{nullptr};
        int pos_{0};
    };

    Iterator begin() const {
        Iterator it(*this);
        it.seek_to_first();
        return it;
    }

    std::size_t size() const {
        std::size_t n = 0;
        for (auto it = begin(); it.valid(); it.next()) {
            ++n;
        }
        return n;
    }

private:
    friend class Iterator;

    static int lower_bound_surrogate(const NodeData* d, std::uint32_t s) {
        // The kernels take a Node, and NodeData opens with the identical 64-byte
        // surrogate array at offset 0, which the static_asserts above enforce.
        const ::aparajita::Node* line = reinterpret_cast<const ::aparajita::Node*>(d->surrogates);
        static const LowerBoundFn fn = lower_bound_dispatch();
        return fn(*line, s);
    }

    ListNode* create_node() {
        void* p = traits_.allocate(sizeof(ListNode), alignof(ListNode));
        return new (p) ListNode();
    }

    // Publishes a payload, hint first.
    //
    // The order is load-bearing in one direction. A node's first key never rises
    // (see BasicListNode), so a reader that catches the new hint beside the old
    // payload holds a hint no larger than either payload's first key, and a hint
    // that is too small only ever lets a hop enter a node it was already entitled
    // to enter. Storing the payload first would expose the opposite: the old,
    // larger hint beside the new payload, which would make a hop stop one node
    // short of the key it wanted.
    //
    // The comparison before the store is not an optimisation of the store itself,
    // which is to a line the caller is about to dirty anyway. It is there because
    // the hint is immutable for every node but the head, and writing a value that
    // never changes on every insert would make that invariant harder to trust
    // when reading the code.
    void publish(ListNode* n, NodeData* nd) const {
        if (nd->count > 0) {
            const std::uint64_t h = descent_hint(traits_.order_bytes(nd->keys[0]));
            if (n->first_hint.load(std::memory_order_relaxed) != h) {
                n->first_hint.store(h, std::memory_order_release);
            }
        }
        n->data.store(nd, std::memory_order_release);
    }

    NodeData* fresh_data() {
        NodeData* d = reinterpret_cast<NodeData*>(
            traits_.allocate(sizeof(NodeData), alignof(NodeData)));
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            d->surrogates[i] = kEmptyKey;
            d->keys[i] = Traits::null_entry();
        }
        d->count = 0;
        d->prefix_len = 0;
        d->next = nullptr;
        return d;
    }

    // True when `n` begins at or below `key`, and so may still be walked past.
    //
    // `hint` is descent_hint() of the key's order bytes, computed once per search
    // rather than once per hop. When the traits promise hint ordering and the two
    // hints differ, their order *is* the answer and neither the successor's
    // payload nor the comparator is touched. Only a tie is ambiguous, and a tie
    // falls through to exactly the code that ran before.
    //
    // The relaxed load is sufficient because reachability already orders it: a
    // reader only holds `n` at all by way of an acquire load on a tower pointer
    // or on a predecessor's payload, and publish() writes the hint before either
    // of those becomes visible.
    template <class K>
    bool starts_at_or_below(const ListNode* n, K key, std::uint64_t hint) const {
        if (hints_) {
            const std::uint64_t h = n->first_hint.load(std::memory_order_relaxed);
            if (h != hint) {
                return h < hint;
            }
        }
        const NodeData* d = n->data.load(std::memory_order_acquire);
        return d->count > 0 && traits_.compare(d->keys[0], key) <= 0;
    }

    // The last node whose first key is <= `key`. Nodes are ordered, so this is the
    // only node that may contain it. Descends the tower first, then finishes on
    // the authoritative level-0 links.
    template <class K>
    ListNode* descend(K key, std::string_view bytes) const {
        const std::uint64_t hint = hints_ ? descent_hint(bytes) : 0;
        ListNode* cur = head_;
        for (int lv = kMaxHeight - 1; lv >= 1; --lv) {
            for (;;) {
                ListNode* nx = cur->tower[lv].load(std::memory_order_acquire);
                if (nx != nullptr && starts_at_or_below(nx, key, hint)) {
                    cur = nx;
                    continue;
                }
                break;
            }
        }
        for (;;) {
            const NodeData* d = cur->data.load(std::memory_order_acquire);
            ListNode* nx = d->next;
            if (nx != nullptr && starts_at_or_below(nx, key, hint)) {
                cur = nx;
                continue;
            }
            return cur;
        }
    }

    // The node immediately before `n` on level 0, or null when `n` is the head.
    //
    // Found by descending on `n`'s own first key with a strict predicate: first
    // keys increase across nodes, so the last node starting strictly below `n`'s
    // first key is exactly `n`'s predecessor. An empty node has no first key to
    // descend on, and the only node that can be empty is the head, which has no
    // predecessor anyway.
    const ListNode* pred_node(const ListNode* n, const NodeData* d) const {
        if (n == head_ || d->count == 0) {
            return nullptr;
        }
        const Entry first = d->keys[0];
        const std::uint64_t hint =
            hints_ ? descent_hint(traits_.order_bytes(first)) : 0;
        const ListNode* cur = head_;
        for (int lv = kMaxHeight - 1; lv >= 1; --lv) {
            for (;;) {
                const ListNode* nx = cur->tower[lv].load(std::memory_order_acquire);
                if (nx != nullptr && nx != n && starts_strictly_below(nx, first, hint)) {
                    cur = nx;
                    continue;
                }
                break;
            }
        }
        for (;;) {
            const NodeData* cd = cur->data.load(std::memory_order_acquire);
            const ListNode* nx = cd->next;
            if (nx != nullptr && nx != n && starts_strictly_below(nx, first, hint)) {
                cur = nx;
                continue;
            }
            return cur;
        }
    }

    bool starts_strictly_below(const ListNode* n, Entry key, std::uint64_t hint) const {
        if (hints_) {
            const std::uint64_t h = n->first_hint.load(std::memory_order_relaxed);
            if (h != hint) {
                return h < hint;
            }
        }
        const NodeData* d = n->data.load(std::memory_order_acquire);
        return d->count > 0 && traits_.compare(d->keys[0], key) < 0;
    }

    // Last node on level 0, reached through the tower rather than by walking.
    const ListNode* last_node() const {
        const ListNode* cur = head_;
        for (int lv = kMaxHeight - 1; lv >= 1; --lv) {
            for (;;) {
                const ListNode* nx = cur->tower[lv].load(std::memory_order_acquire);
                if (nx == nullptr) {
                    break;
                }
                cur = nx;
            }
        }
        for (;;) {
            const NodeData* d = cur->data.load(std::memory_order_acquire);
            if (d->next == nullptr) {
                return cur;
            }
            cur = d->next;
        }
    }

    // The last node at or above level `lv` whose first key is <= `key`. Used to
    // find where a freshly split sibling belongs in each tower level.
    ListNode* pred_at_level(int lv, Entry key, std::uint64_t hint) const {
        ListNode* cur = head_;
        for (int l = kMaxHeight - 1; l >= lv; --l) {
            for (;;) {
                ListNode* nx = cur->tower[l].load(std::memory_order_acquire);
                if (nx != nullptr && starts_at_or_below(nx, key, hint)) {
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
    void link_tower(ListNode* node, Entry first_key) {
        const std::uint64_t hint =
            hints_ ? descent_hint(traits_.order_bytes(first_key)) : 0;
        for (int lv = 1; lv < node->height; ++lv) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                ListNode* pred = pred_at_level(lv, first_key, hint);
                if (pred == node) {
                    break;
                }
                ListNode* nx = pred->tower[lv].load(std::memory_order_acquire);
                if (nx != nullptr && starts_at_or_below(nx, first_key, hint)) {
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

    bool belongs_here(const NodeData* d, Entry key) const {
        if (d->next == nullptr) {
            return true;
        }
        const NodeData* nd = d->next->data.load(std::memory_order_acquire);
        return nd->count == 0 || traits_.compare(nd->keys[0], key) > 0;
    }

    bool shares_prefix(const NodeData* d, std::string_view bytes) const {
        if (d->count == 0 || d->prefix_len == 0) {
            return true;
        }
        if (bytes.size() < d->prefix_len) {
            return false;
        }
        const std::string_view first = traits_.order_bytes(d->keys[0]);
        return std::memcmp(bytes.data(), first.data(), d->prefix_len) == 0;
    }

    // Position at which `key` keeps the node sorted, and equally the first entry
    // >= `key` for a lookup.
    //
    // The SIMD answer is a candidate, not the answer. Two things can invalidate
    // it. A key that does not share the node's prefix has no meaningful surrogate
    // at this node's offset, so it is ordered against a different alphabet. And
    // the surrogate reproduces *bytewise* order on the order bytes, which is the
    // RocksDB default but not a guarantee: a column family with a custom
    // comparator, or two entries whose user keys tie and are separated by sequence
    // number, can order differently from their surrogates.
    //
    // So the candidate is confirmed against the comparator, which is the only
    // authority, and a failed confirmation falls back to a binary search. That
    // keeps the structure correct under any comparator while still paying two
    // comparisons instead of four in the case that actually happens.
    template <class K>
    int sorted_position(const NodeData* d, K key, std::string_view bytes) const {
        const int n = static_cast<int>(d->count);

        if (shares_prefix(d, bytes)) {
            const std::uint32_t s = surrogate_at(bytes, d->prefix_len);
            int p = lower_bound_surrogate(d, s);
            if (p > n) {
                p = n;
            }
            // Equal surrogates are contiguous because the node is sorted, so the
            // candidate run is a short scan from the SIMD answer. This is where a
            // shared four-byte prefix turns the search linear, which is what
            // collision_report measures.
            while (p < n && d->surrogates[p] == s && traits_.compare(d->keys[p], key) < 0) {
                ++p;
            }
            const bool left_ok = (p == 0) || traits_.compare(d->keys[p - 1], key) < 0;
            const bool right_ok = (p >= n) || traits_.compare(d->keys[p], key) >= 0;
            if (left_ok && right_ok) {
                return p;
            }
        }

        int lo = 0;
        int hi = n;
        while (lo < hi) {
            const int mid = lo + ((hi - lo) >> 1);
            if (traits_.compare(d->keys[mid], key) < 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // Fills a fresh payload from a sorted key run. The shared prefix is recomputed
    // from the first and last key every time rather than carried forward, because
    // an insert at either end can shorten it and a stale prefix would silently
    // mis-order every surrogate in the node. Recomputing is affordable precisely
    // because copy-on-write already rebuilds the node.
    void fill(NodeData* d, const Entry* keys, int n, ListNode* next) const {
        d->count = static_cast<std::uint32_t>(n);
        d->next = next;
        d->prefix_len = n > 1 ? static_cast<std::uint32_t>(common_prefix_len(
                                    traits_.order_bytes(keys[0]),
                                    traits_.order_bytes(keys[n - 1])))
                              : 0;
        for (int i = 0; i < n; ++i) {
            d->keys[i] = keys[i];
            d->surrogates[i] = surrogate_at(traits_.order_bytes(keys[i]), d->prefix_len);
        }
        for (int i = n; i < static_cast<int>(kNodeKeys); ++i) {
            d->keys[i] = Traits::null_entry();
            d->surrogates[i] = kEmptyKey;
        }
    }

    // Merges `key` into the node's sorted run, writing the result to `out`.
    int merge(const NodeData* d, Entry key, std::string_view bytes, Entry* out) const {
        const int p = sorted_position(d, key, bytes);
        for (int i = 0; i < p; ++i) {
            out[i] = d->keys[i];
        }
        out[p] = key;
        for (int i = p; i < static_cast<int>(d->count); ++i) {
            out[i + 1] = d->keys[i];
        }
        return static_cast<int>(d->count) + 1;
    }

    void insert_into(ListNode* cur, const NodeData* d, Entry key, std::string_view bytes) {
        Entry merged[kNodeKeys + 1];
        const int n = merge(d, key, bytes, merged);
        NodeData* nd = fresh_data();
        fill(nd, merged, n, d->next);
        publish(cur, nd);
    }

    // A full node becomes two. The right half is published as a brand new node
    // first, then a single release store swaps this node's payload to the left
    // half *and* its link to the new sibling at the same instant. A reader either
    // sees the old sixteen-key node or the new pair, never a torn state.
    void split_and_insert(ListNode* cur, const NodeData* d, Entry key, std::string_view bytes) {
        constexpr int kMid = static_cast<int>(kNodeKeys) / 2;
        Entry merged[kNodeKeys + 1];
        const int total = merge(d, key, bytes, merged);

        ListNode* sibling = create_node();
        NodeData* right = fresh_data();
        NodeData* left = fresh_data();

        fill(right, merged + kMid, total - kMid, d->next);
        fill(left, merged, kMid, sibling);

        // Everything about the sibling is settled before it becomes reachable,
        // height included. Writing a plain field after publication would be a
        // race even if no reader happens to look at it today.
        sibling->height = static_cast<std::uint8_t>(random_height());
        publish(sibling, right);

        publish(cur, left);

        // Only now, once the sibling is reachable on level 0, is it safe to index
        // it. A tower pointer to a node no one can reach on level 0 would let a
        // search jump to it and then walk forward off the real list.
        link_tower(sibling, right->keys[0]);
    }

    Traits traits_;
    // Cached because descend() consults it on every hop and the answer cannot
    // change: a comparator is fixed for the lifetime of a memtable.
    bool hints_;
    ListNode* head_;
};

// ---------------------------------------------------------------------------
// The standalone traits: our own arena, plain keys, memcmp order
// ---------------------------------------------------------------------------

class StringViewTraits {
public:
    using Entry = std::string_view;
    using Target = std::string_view;

    explicit StringViewTraits(Arena& arena) : arena_(&arena) {}

    char* allocate(std::size_t bytes, std::size_t align) const {
        return arena_->allocate(bytes, align);
    }

    static std::string_view order_bytes(std::string_view e) { return e; }
    static int compare(std::string_view a, std::string_view b) { return compare_keys(a, b); }
    static Entry null_entry() { return std::string_view(); }

    // compare() is compare_keys(), which is memcmp with a length tiebreak, so
    // bytewise order on the first eight bytes agrees with it by construction.
    static bool hint_ordering() { return true; }

    Arena& arena() const { return *arena_; }

private:
    Arena* arena_;
};

// The Phase 2 structure, unchanged in behaviour: it owns its keys, so insert
// copies into the arena before handing the entry to the shared core.
class MemTable : public BasicMemTable<StringViewTraits> {
public:
    explicit MemTable(Arena& arena) : BasicMemTable<StringViewTraits>(StringViewTraits(arena)) {}

    // Copies the key into the arena, so the caller's buffer need not outlive the
    // call. Safe to call from many threads at once.
    void insert(std::string_view key) {
        char* copy = traits().allocate(key.empty() ? 1 : key.size(), 1);
        if (!key.empty()) {
            std::memcpy(copy, key.data(), key.size());
        }
        insert_entry(std::string_view(copy, key.size()));
    }

    bool contains(std::string_view key) const { return contains_target(key); }
};

using NodeData = BasicNodeData<StringViewTraits>;
using ListNode = BasicListNode<StringViewTraits>;

} // namespace aparajita
