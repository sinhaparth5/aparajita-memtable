// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

// The ordered concurrent structure: a list of cache-line nodes with concurrent
// insert and lock-free reads.
//
// Layout follows the rule in ROADMAP.md that the bytes a probe compares sit
// contiguously in one line and everything else sits apart. NodeData opens with
// exactly 64 bytes of surrogates, so a search touches one line; the order word,
// the link and the full keys live past it and are only reached once the SIMD step
// has already narrowed the answer to a candidate.
//
// Concurrency rests on one decision, which two phases have now used for different
// things: a reader is only ever shown a fact through a single release store, and
// nothing it can reach from that fact is ever revised.
//
// A split takes the form of publishing a whole new payload, because it changes a
// node's contents and its successor at once. `next` therefore lives *inside*
// NodeData: swapping the pointer swaps both together, so a reader sees either the
// old full node or the new half-node pointing at a fresh sibling, and never a
// state where a key is duplicated across both or missing from both.
//
// An ordinary insert does not need any of that, and Phase 4b stopped paying for
// it. A node is append-only -- a slot is written once, before the order word that
// names it, and never written again -- so an insert writes into a free slot and
// republishes the sorted order as one 64-bit store. Nothing already visible
// moves. A reader either sees the new order word and every slot it names, or the
// old one and the node exactly as it was. See BasicNodeData, and node.hpp for the
// encoding.
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
// append-and-republish path and the tower — the parts that were hard to get
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
//
// The arrays are in slot order, which is insertion order, and not in sorted
// order. Phase 4b made a node append-only: a slot is written once, before any
// order word names it, and never written again. `order` carries the sorted
// permutation over the slots and republishing it is the whole of an insert.
// docs/phase4b-append.md has why, and node.hpp has the encoding.
//
// What is mutable after publication and what is not is the entire concurrency
// argument, so it is worth stating flatly:
//
//   surrogates[s], keys[s]   written once, before the order word naming slot s
//   order                    republished by every insert into this node
//   prefix_len, next         written once, before this payload is published
//
// `next` lives in here because a split has to swap a node's contents and its
// successor at the same instant, and publishing a fresh payload is how it does
// that. `order` lives in here for exactly the same reason: a split changes both,
// and a reader must never see one without the other. Moving either out beside
// the tower in ListNode would cost a second store and reopen precisely the torn
// states this arrangement exists to prevent.
template <class Traits>
struct alignas(kCacheLine) BasicNodeData {
    using Entry = typename Traits::Entry;

    std::uint32_t surrogates[kNodeKeys];

    // Sorted rank -> slot. Also the node's occupancy: see order_count().
    std::atomic<std::uint64_t> order;
    // Bytes shared by every key in this node, stripped before the surrogate is
    // taken. See surrogate_at() for why the lane would otherwise be useless on
    // real keys.
    //
    // Fixed when the payload is built and never revised, which an append-only
    // node has no choice about: shortening it would invalidate every surrogate
    // already written. It does not have to be revised, because a node's shared
    // prefix can only shrink as keys arrive and a key that does not share it
    // sorts outside the node's whole range -- see append(), which gives such a
    // key a saturating surrogate and keeps the lane array sorted regardless.
    // Splits recompute it from scratch, so the prefix sharpens as nodes divide.
    std::uint32_t prefix_len;
    BasicListNode<Traits>* next;
    Entry keys[kNodeKeys];
};

// Height of the index. Four is the branching factor, so twelve levels indexes
// roughly 4^12 nodes, which at fifteen keys each is far past any memtable RocksDB
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
    static_assert(offsetof(NodeData, order) == kCacheLine,
                  "cold fields must begin on the second line, not share the compared one");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "publishing an insert is one store, which a locked order word would not be");
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
            // target the same fifteen keys. That makes a short spin the right
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

            // Relaxed: only the lock holder ever stores this, and we hold it.
            const std::uint64_t order = d->order.load(std::memory_order_relaxed);
            const int n = order_count(order);
            if (n < static_cast<int>(kNodeCapacity)) {
                append(cur, d, order, n, entry, bytes);
            } else {
                split_and_insert(cur, d, order, n, entry, bytes);
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
        const std::uint64_t order = snapshot(d);
        const int n = order_count(order);

        const int p = sorted_position(d, order, n, key, bytes);
        return p < n && traits_.compare(key_at(d, order, p), key) == 0;
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
    // once per node rather than once per key.
    class Iterator {
    public:
        Iterator() = default;
        // Constructed unpositioned and therefore invalid, matching
        // MemTableRep::Iterator, whose contract is that a fresh iterator is not
        // valid until a Seek. Use begin() for an iterator already on the first key.
        explicit Iterator(const BasicMemTable& t) : table_(&t) {}

        bool valid() const { return data_ != nullptr && pos_ >= 0 && pos_ < count_; }
        Entry key() const { return BasicMemTable::key_at(data_, order_, pos_); }

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
            adopt(table_->head_);
            pos_ = 0;
            skip_forward();
        }

        void seek_to_last() {
            adopt(table_->last_node());
            pos_ = count_ - 1;
            skip_backward_if_empty();
        }

        // First entry >= target.
        void seek(Target target) {
            const std::string_view bytes = table_->traits_.order_bytes(target);
            adopt(table_->descend(target, bytes));
            pos_ = table_->sorted_position(data_, order_, count_, target, bytes);
            skip_forward();
        }

        // Last entry <= target.
        void seek_for_prev(Target target) {
            seek(target);
            if (!valid()) {
                seek_to_last();
                return;
            }
            if (table_->traits_.compare(key(), target) > 0) {
                prev();
            }
        }

    private:
        // Moves onto a node and takes one snapshot of its order word, which every
        // position this iterator reports is then read through. Taking it once is
        // what makes the iterator coherent over an append-only node: the ranks the
        // snapshot covers name slots that are already frozen, so a writer adding a
        // key to this node cannot move a key out from under a walk in progress.
        // The walk simply does not see the new key, which is the same freedom a
        // reader had against copy-on-write.
        void adopt(const ListNode* n) {
            node_ = n;
            data_ = n != nullptr ? n->data.load(std::memory_order_acquire) : nullptr;
            order_ = data_ != nullptr ? BasicMemTable::snapshot(data_) : 0;
            count_ = order_count(order_);
        }

        // A node can legitimately be empty (the head always is), so advancing may
        // have to cross several before it lands on a key.
        void skip_forward() {
            while (data_ != nullptr && pos_ >= count_) {
                adopt(data_->next);
                pos_ = 0;
            }
        }

        void skip_backward() {
            for (;;) {
                const ListNode* p = table_->pred_node(node_, data_, order_);
                if (p == nullptr) {
                    data_ = nullptr;  // ran off the front
                    return;
                }
                adopt(p);
                pos_ = count_ - 1;
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
        std::uint64_t order_{0};
        int count_{0};
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

    // A full walk checking the invariants that hold by construction. Returns the
    // first one violated, or nullptr.
    //
    // This exists because the invariants that matter most here are invisible to
    // every other test in the repository. sorted_position() confirms each SIMD
    // candidate against the comparator and falls back to binary search, so a node
    // whose surrogate lanes have stopped being sorted still answers every lookup
    // correctly and every iteration in the right order -- it just stops using the
    // vector unit. A descent hint that disagrees with its node's first key is the
    // same shape of problem one level up. Both are silent collapses back to the
    // performance the project exists to beat, and a differential test against the
    // skiplist agrees with them completely. This turns them into a failing test.
    //
    // Linear in the structure, and not safe against concurrent writers: it reads
    // a node and then reasons about it, which a writer appending to that node
    // invalidates. Call it on a quiesced structure.
    const char* check_invariants() const {
        const NodeData* prev = nullptr;
        std::uint64_t prev_order = 0;

        for (const ListNode* n = head_; n != nullptr;) {
            const NodeData* d = n->data.load(std::memory_order_acquire);
            const std::uint64_t o = snapshot(d);
            const int count = order_count(o);
            if (count > static_cast<int>(kNodeCapacity)) {
                return "a node holds more keys than kNodeCapacity";
            }

            // The order word is a permutation of the used slots. Slots are
            // consumed in order, so "used" is exactly [0, count).
            bool seen[kNodeKeys] = {};
            for (int r = 0; r < count; ++r) {
                const int slot = order_slot(o, r);
                if (slot >= count) {
                    return "the order word names a slot that was never filled";
                }
                if (seen[slot]) {
                    return "the order word names one slot at two ranks";
                }
                seen[slot] = true;
            }

            for (int r = 1; r < count; ++r) {
                if (traits_.compare(key_at(d, o, r - 1), key_at(d, o, r)) > 0) {
                    return "keys within a node are out of comparator order";
                }
                if (surrogate_at_rank(d, o, r - 1) > surrogate_at_rank(d, o, r)) {
                    return "surrogate lanes within a node are not sorted";
                }
            }

            // Every lane is either the surrogate its key actually has at this
            // node's prefix, or -- for a key that does not share that prefix -- a
            // saturated lane in the direction the key sorts. Anything else means
            // append() computed a lane on the wrong alphabet.
            for (int r = 0; r < count; ++r) {
                const std::string_view bytes = traits_.order_bytes(key_at(d, o, r));
                const std::uint32_t lane = surrogate_at_rank(d, o, r);
                if (shares_prefix(d, o, bytes)) {
                    if (lane != surrogate_at(bytes, d->prefix_len)) {
                        return "a lane disagrees with the key stored beside it";
                    }
                } else if (lane != 0u && lane != kEmptyKey) {
                    return "a key outside the node's prefix has an unsaturated lane";
                }
            }

            // The stored prefix is never shorter than the keys' actual common
            // prefix. fill() sets it to exactly that, and every later append can
            // only widen the node's range, which shortens the true prefix while
            // this one stays put. So `too long, never too short` is the property,
            // and it is what makes a frozen prefix safe: a key that no longer
            // shares it is handled, a key whose lane was taken past a prefix it
            // does not have would not be.
            //
            // The head before its first split is the one exception, and it is the
            // only node that ever holds a payload fill() did not build: it starts
            // empty, with no sorted run to derive a prefix from, and grows by
            // appending at prefix zero until it splits.
            const bool never_filled = (n == head_ && d->next == nullptr);
            if (count > 1 && !never_filled) {
                const std::size_t actual =
                    common_prefix_len(traits_.order_bytes(key_at(d, o, 0)),
                                      traits_.order_bytes(key_at(d, o, count - 1)));
                if (d->prefix_len < actual) {
                    return "a node's stored prefix is shorter than its keys' common prefix";
                }
            }

            if (count > 0 &&
                n->first_hint.load(std::memory_order_relaxed) !=
                    descent_hint(traits_.order_bytes(key_at(d, o, 0)))) {
                return "a node's descent hint does not match its first key";
            }

            if (prev != nullptr && count > 0) {
                const int pc = order_count(prev_order);
                if (pc > 0 &&
                    traits_.compare(key_at(prev, prev_order, pc - 1), key_at(d, o, 0)) > 0) {
                    return "level 0 runs out of comparator order across nodes";
                }
            }

            prev = d;
            prev_order = o;
            n = d->next;
        }
        return nullptr;
    }

private:
    friend class Iterator;

    static int lower_bound_surrogate(const NodeData* d, std::uint64_t order, std::uint32_t s) {
        // The kernels take a Node, and NodeData opens with the identical 64-byte
        // surrogate array at offset 0, which the static_asserts above enforce.
        const ::aparajita::Node* line = reinterpret_cast<const ::aparajita::Node*>(d->surrogates);
        static const LowerBoundPermFn fn = lower_bound_perm_dispatch();
        return fn(*line, s, order);
    }

    // The order word, read once. Every slot the word names is frozen, so a
    // snapshot is a consistent view of the node even while a writer appends to
    // it: ranks the snapshot does not cover are exactly the ones still in flight.
    // This acquire is what orders the writer's slot stores before the reader's
    // use of them, so nothing below reads a slot through a stale word.
    static std::uint64_t snapshot(const NodeData* d) {
        return d->order.load(std::memory_order_acquire);
    }

    static Entry key_at(const NodeData* d, std::uint64_t order, int rank) {
        return d->keys[order_slot(order, rank)];
    }

    static std::uint32_t surrogate_at_rank(const NodeData* d, std::uint64_t order, int rank) {
        return d->surrogates[order_slot(order, rank)];
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
        const std::uint64_t o = nd->order.load(std::memory_order_relaxed);
        if (order_count(o) > 0) {
            publish_hint(n, key_at(nd, o, 0));
        }
        n->data.store(nd, std::memory_order_release);
    }

    // The hint half of publication, on its own, because an append publishes a new
    // first key without publishing a new payload.
    void publish_hint(ListNode* n, Entry first) const {
        const std::uint64_t h = descent_hint(traits_.order_bytes(first));
        if (n->first_hint.load(std::memory_order_relaxed) != h) {
            n->first_hint.store(h, std::memory_order_release);
        }
    }

    // Placement-new rather than a cast over raw arena bytes: `order` is a
    // std::atomic and beginning its lifetime is not optional. The zeroing that
    // comes with value-initialisation is affordable now that a fresh payload
    // means a split rather than every single insert.
    NodeData* fresh_data() {
        NodeData* d = new (traits_.allocate(sizeof(NodeData), alignof(NodeData))) NodeData();
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            d->surrogates[i] = kEmptyKey;
            d->keys[i] = Traits::null_entry();
        }
        d->order.store(0, std::memory_order_relaxed);
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
        const std::uint64_t o = snapshot(d);
        return order_count(o) > 0 && traits_.compare(key_at(d, o, 0), key) <= 0;
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
    const ListNode* pred_node(const ListNode* n, const NodeData* d, std::uint64_t order) const {
        if (n == head_ || order_count(order) == 0) {
            return nullptr;
        }
        const Entry first = key_at(d, order, 0);
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
        const std::uint64_t o = snapshot(d);
        return order_count(o) > 0 && traits_.compare(key_at(d, o, 0), key) < 0;
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
        const std::uint64_t o = snapshot(nd);
        return order_count(o) == 0 || traits_.compare(key_at(nd, o, 0), key) > 0;
    }

    // Whether `bytes` agrees with this node's stripped prefix, and so has a
    // surrogate on the same alphabet as the lanes already stored here.
    //
    // A false answer says more than "no fast path", and append() relies on the
    // stronger reading. Every key in the node agrees with the prefix at every one
    // of its positions, so a key that disagrees at position j differs there from
    // all of them in the same direction, and a key shorter than the prefix is a
    // proper prefix of all of them. Either way it sorts below the node's whole
    // range or above it, never inside.
    bool shares_prefix(const NodeData* d, std::uint64_t order, std::string_view bytes) const {
        if (order_count(order) == 0 || d->prefix_len == 0) {
            return true;
        }
        if (bytes.size() < d->prefix_len) {
            return false;
        }
        const std::string_view first = traits_.order_bytes(key_at(d, order, 0));
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
    int sorted_position(const NodeData* d, std::uint64_t order, int n, K key,
                        std::string_view bytes) const {
        if (shares_prefix(d, order, bytes)) {
            const std::uint32_t s = surrogate_at(bytes, d->prefix_len);
            int p = lower_bound_surrogate(d, order, s);
            if (p > n) {
                p = n;
            }
            // Equal surrogates are contiguous because the node is sorted, so the
            // candidate run is a short scan from the SIMD answer. This is where a
            // shared four-byte prefix turns the search linear, which is what
            // collision_report measures.
            while (p < n && surrogate_at_rank(d, order, p) == s &&
                   traits_.compare(key_at(d, order, p), key) < 0) {
                ++p;
            }
            const bool left_ok = (p == 0) || traits_.compare(key_at(d, order, p - 1), key) < 0;
            const bool right_ok = (p >= n) || traits_.compare(key_at(d, order, p), key) >= 0;
            if (left_ok && right_ok) {
                return p;
            }
        }

        int lo = 0;
        int hi = n;
        while (lo < hi) {
            const int mid = lo + ((hi - lo) >> 1);
            if (traits_.compare(key_at(d, order, mid), key) < 0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

    // Fills a fresh payload from a sorted key run: slot order and sorted order
    // coincide, so the published permutation is the identity.
    //
    // This is the only place the shared prefix is computed, and a split is the
    // only thing that calls it. That is a change of role rather than of code. It
    // used to run on every insert because copy-on-write rebuilt the node anyway;
    // now a node's prefix is set when it is born and holds for its life, and the
    // recomputation here is what makes the prefix *sharpen* as nodes divide and a
    // key range narrows. append() handles the keys that arrive not sharing it.
    void fill(NodeData* d, const Entry* keys, int n, ListNode* next) const {
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
        d->order.store(order_identity(n), std::memory_order_relaxed);
    }

    // Merges `key` into the node's sorted run, writing the result to `out`.
    int merge(const NodeData* d, std::uint64_t order, int n, Entry key,
              std::string_view bytes, Entry* out) const {
        const int p = sorted_position(d, order, n, key, bytes);
        for (int i = 0; i < p; ++i) {
            out[i] = key_at(d, order, i);
        }
        out[p] = key;
        for (int i = p; i < n; ++i) {
            out[i + 1] = key_at(d, order, i);
        }
        return n + 1;
    }

    // An insert into a node with room: two stores into a free slot and one store
    // to republish the order. No allocation, no copy, and the node stays where it
    // is. This is the whole of Phase 4b.
    //
    // The surrogate is the only subtle part. It has to be taken at this node's
    // frozen prefix, and a key that does not share that prefix has no meaningful
    // surrogate there. Such a key is not arbitrary, though: shares_prefix()
    // explains why it must sort below the node's entire range or above it, which
    // is to say its rank is 0 or n and never between. Saturating its lane in that
    // direction -- 0 below, kEmptyKey above -- keeps the lane array sorted, so the
    // SIMD kernel stays usable for every other key in the node. Ties against a
    // real key that happens to reach the same value are resolved where every
    // surrogate tie is resolved, by the comparator.
    //
    // Publication order matches publish(): hint first, then the order word, so a
    // reader that catches the new hint beside the old order holds a hint no larger
    // than either version's first key.
    void append(ListNode* cur, NodeData* d, std::uint64_t order, int n, Entry key,
                std::string_view bytes) {
        const int rank = sorted_position(d, order, n, key, bytes);
        const int slot = n;  // slots are consumed in order, so the free one is n

        d->keys[slot] = key;
        d->surrogates[slot] = shares_prefix(d, order, bytes)
                                  ? surrogate_at(bytes, d->prefix_len)
                                  : (rank == 0 ? 0u : kEmptyKey);

        if (rank == 0) {
            publish_hint(cur, key);
        }
        d->order.store(order_insert(order, rank, slot), std::memory_order_release);
    }

    // A full node becomes two. The right half is published as a brand new node
    // first, then a single release store swaps this node's payload to the left
    // half *and* its link to the new sibling at the same instant. A reader either
    // sees the old full node or the new pair, never a torn state.
    void split_and_insert(ListNode* cur, const NodeData* d, std::uint64_t order, int n,
                          Entry key, std::string_view bytes) {
        constexpr int kMid = static_cast<int>(kNodeCapacity + 1) / 2;
        Entry merged[kNodeCapacity + 1];
        const int total = merge(d, order, n, key, bytes, merged);

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
        link_tower(sibling, right->keys[0]);  // fill() published the identity order
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
