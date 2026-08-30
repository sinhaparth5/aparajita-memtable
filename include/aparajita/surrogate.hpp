// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

// Turning a variable-length RocksDB key into the 32-bit value a SIMD lane holds.
//
// docs/phase2-design.md settles what goes in the lane: the first four bytes of
// the user key, not a hash. The reason is order. RocksDB iterators must yield
// keys in comparator order, and the ordered design rests on lower_bound over a
// sorted node, which only works if comparing two surrogates agrees with
// comparing the keys they came from. A hash destroys that; leading bytes keep it.

#include "aparajita/node.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace aparajita {

// Big-endian on purpose. RocksDB's default comparator is bytewise memcmp, so the
// first key byte must land in the most significant position for an unsigned 32-bit
// compare to reproduce that order. A little-endian load would order keys by their
// fourth byte first, which is not an ordering anyone asked for.
//
// Keys shorter than four bytes pad with 0x00 on the right, which is the correct
// direction: a prefix sorts before every key extending it, and zero padding
// reproduces that.
inline std::uint32_t surrogate(const char* key, std::size_t len) noexcept {
    const std::size_t n = len < 4 ? len : 4;
    std::uint32_t s = 0;
    for (std::size_t i = 0; i < n; ++i) {
        s = (s << 8) | static_cast<std::uint8_t>(key[i]);
    }
    return s << (8 * (4 - n));
}

inline std::uint32_t surrogate(std::string_view key) noexcept {
    return surrogate(key.data(), key.size());
}

// The same four bytes taken from `offset` rather than from the start.
//
// This exists because the whole-key surrogate is defeated by the key shapes
// RocksDB users actually write. A table prefix, a tenant id, or a big-endian
// timestamp puts identical bytes at the front of every key in the database, and
// a lane holding those bytes discriminates nothing. bench/collision_report.cpp
// measures it: six of seven realistic distributions collapse to a single
// surrogate value.
//
// The escape is that the lane never needed to be order-preserving globally. A
// search descends to the right node by comparing full keys, so the surrogate is
// only ever compared against others in the same node. Stripping the prefix those
// keys share costs nothing in ordering and restores all the discrimination the
// shared bytes were wasting. This is prefix truncation, the same trick B-trees
// use to fit more separators on a page.
inline std::uint32_t surrogate_at(std::string_view key, std::size_t offset) noexcept {
    if (offset >= key.size()) {
        return 0;
    }
    return surrogate(key.data() + offset, key.size() - offset);
}

// The 64-bit value a tower hop compares, taken from the front of the order bytes.
//
// This is the descent's counterpart to the node-local surrogate, and it answers a
// different question, so it is built differently. A surrogate is compared only
// against others inside one node, which is what lets it strip that node's shared
// prefix. A descent hint is compared across nodes, so it can strip nothing and
// must be taken from byte zero.
//
// Eight bytes rather than four because the hint has no confirmation step that is
// cheaper than the thing it replaces: a tie costs a full virtual comparator call
// on the key it was meant to avoid touching. Four bytes tie far too often on the
// shared table and tenant prefixes bench/collision_report.cpp measures.
//
// Big-endian and zero-padded for the same reason surrogate() is. Zero padding is
// order-preserving under bytewise comparison because a prefix sorts before every
// key extending it, and a real 0x00 byte at that position compares equal to the
// padding, which is exactly the "undecided in the first eight bytes" answer the
// caller falls back on.
inline std::uint64_t descent_hint(std::string_view key) noexcept {
    const std::size_t n = key.size() < 8 ? key.size() : 8;
    std::uint64_t h = 0;
    for (std::size_t i = 0; i < n; ++i) {
        h = (h << 8) | static_cast<std::uint8_t>(key[i]);
    }
    return h << (8 * (8 - n));
}

// Length of the longest common prefix. Over a sorted run this need only be
// computed for the first and last key: everything between shares at least that.
inline std::size_t common_prefix_len(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    std::size_t i = 0;
    while (i < n && a[i] == b[i]) {
        ++i;
    }
    return i;
}

// The surrogate is lossy, so a SIMD hit is a candidate rather than an answer and
// the full key still has to be compared. This is that comparison, and it is
// bytewise memcmp semantics rather than std::string_view's, because
// string_view::compare on char is implementation-signedness-dependent for bytes
// above 0x7F and RocksDB keys are arbitrary binary.
inline int compare_keys(std::string_view a, std::string_view b) noexcept {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    if (n != 0) {
        const int c = std::memcmp(a.data(), b.data(), n);
        if (c != 0) {
            return c;
        }
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

// A real key beginning with four 0xFF bytes produces a surrogate equal to
// kEmptyKey. That is not merely a false positive the full compare would filter:
// it is indistinguishable from an empty slot, so a node that inferred its
// occupancy from the sentinel would lose the key entirely. Nodes therefore carry
// an explicit count and never infer occupancy. This predicate exists so that
// intent is greppable and testable rather than living in a comment.
inline bool aliases_empty_sentinel(std::uint32_t s) noexcept {
    return s == kEmptyKey;
}

} // namespace aparajita
