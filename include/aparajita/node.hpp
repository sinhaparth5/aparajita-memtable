// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>

namespace aparajita {

// One cache line holds sixteen 32-bit key surrogates. The width is deliberate:
// AVX2 covers it in two 256-bit compares, AVX-512 in one 512-bit compare, and a
// node never straddles two cache lines, so a probe costs exactly one line fill.
inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kNodeKeys = 16;

struct alignas(kCacheLine) Node {
    std::uint32_t keys[kNodeKeys];
};

static_assert(sizeof(Node) == kCacheLine, "node must occupy exactly one cache line");
static_assert(alignof(Node) == kCacheLine, "node must be cache-line aligned");

// Separation required between independently written variables to avoid false
// sharing. Defined as a literal rather than forwarded from
// std::hardware_destructive_interference_size on purpose: GCC warns that the
// standard constant varies with -mtune and compiler version, which would make
// it an unstable value baked into a public header that RocksDB compiles
// against. A static_assert below keeps us honest if the platform disagrees.
//
// The architecture document argues for 128-byte separation on Intel parts with
// adjacent-line prefetch. That applies to the concurrently written state in
// Phase 2, not to these read-mostly key arrays, so it is recorded here and
// applied there.
inline constexpr std::size_t kDestructiveInterference = 64;

#ifdef __cpp_lib_hardware_interference_size
// The warning this suppresses is the very reason kDestructiveInterference is a
// literal above. Reading the standard constant inside a static_assert couples
// nothing to the ABI, so it is safe here and nowhere else in this header.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
static_assert(std::hardware_destructive_interference_size >= kDestructiveInterference,
              "platform wants wider separation than 64 bytes; revisit kDestructiveInterference");
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

// Sentinel for "no key at this slot". The value is 0xFFFFFFFF rather than 0 so
// that it sorts above every real key: a partially filled node keeps its tail
// slots sentinel-valued and remains sorted, which any ordered search over the
// node depends on. Real keys must therefore never take this value.
inline constexpr std::uint32_t kEmptyKey = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------
// The order word: sixteen four-bit slot indices in one 64-bit integer
// ---------------------------------------------------------------------------
//
// Phase 2 kept a node sorted by rebuilding it. Every insert allocated a fresh
// payload, merged the new key into the sorted run and published the pointer,
// which made the publication a single store but charged a whole node -- 384
// bytes standalone, 208 under RocksDB -- against the arena for every key
// written. docs/phase4b-append.md has the measurement.
//
// The way out is to stop moving keys. A slot is written once and never again,
// and the *order* over those slots is what gets republished. Sorted rank i is
// held in nibble i of a 64-bit word, so a new order is one aligned 64-bit store
// and the publication stays exactly as atomic as swapping a pointer was.
//
// A nibble holds `slot + 1`, not the slot, and 0 means "this rank is unused".
// That costs one of the sixteen slots -- values 1..15 address slots 0..14 -- and
// buys two things worth more than the slot.
//
// The word becomes self-describing. The count is the number of used ranks, and
// used ranks are dense from rank 0, so it is the position of the highest nonzero
// nibble. Nothing else has to be published alongside the order, which matters
// because a second field would need a second store and would reopen exactly the
// torn states the single store exists to prevent.
//
// And the slot the encoding gives up becomes the padding lane. An unused rank
// decodes to slot 15, which no key ever occupies and which every node keeps at
// kEmptyKey, so a permuted node is sentinel-padded above its last key for free:
// the ordered kernels need no count and no separate blend to establish it. See
// lower_bound_perm_* in search.hpp.
inline constexpr std::size_t kNodeCapacity = kNodeKeys - 1;

// The slot an unused rank decodes to. Never holds a key; always holds kEmptyKey.
inline constexpr int kPadSlot = static_cast<int>(kNodeKeys) - 1;

static_assert(kNodeKeys == 16, "the order word packs exactly sixteen four-bit ranks");
static_assert(kNodeCapacity < kNodeKeys, "one slot must stay free to be the padding lane");

// Slot holding sorted rank `rank`, or kPadSlot when that rank is unused.
inline int order_slot(std::uint64_t order, int rank) noexcept {
    // Subtracting before masking is deliberate: a zero nibble borrows and lands
    // on 15, which is kPadSlot, so "unused" needs no branch of its own.
    return static_cast<int>(((order >> (4 * rank)) - 1) & 0xFu);
}

// Number of used ranks.
//
// Used ranks are dense from rank 0 and every used nibble is nonzero, so the
// highest set bit sits inside the last used nibble. countl_zero is defined at
// zero, unlike __builtin_clzll, which is why an empty node needs no special case.
inline int order_count(std::uint64_t order) noexcept {
    return (67 - std::countl_zero(order)) >> 2;
}

// The order over slots 0..n-1 in slot order, for a node built sorted in one go.
inline std::uint64_t order_identity(int n) noexcept {
    std::uint64_t o = 0;
    for (int i = 0; i < n; ++i) {
        o |= static_cast<std::uint64_t>(i + 1) << (4 * i);
    }
    return o;
}

// `order` with `slot` spliced in at sorted rank `rank`, everything above it
// shifted one nibble up. This is the whole of an insert's bookkeeping.
inline std::uint64_t order_insert(std::uint64_t order, int rank, int slot) noexcept {
    const int bit = 4 * rank;
    // rank <= count <= kNodeCapacity - 1 = 14 on this path, so bit + 4 <= 60 and
    // neither shift below can reach 64. A full node splits instead of appending.
    const std::uint64_t below = bit == 0 ? 0ull : (order & ((1ull << bit) - 1));
    const std::uint64_t above = (order >> bit) << (bit + 4);
    return below | above | (static_cast<std::uint64_t>(slot + 1) << bit);
}

} // namespace aparajita
