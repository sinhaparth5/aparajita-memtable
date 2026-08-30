// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include "aparajita/node.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

namespace aparajita::workload {

// A node of sixteen distinct, sorted keys. Sorted because search_scalar_binary
// requires it; the SIMD kernels are order-agnostic and see the same data, so the
// comparison stays fair.
inline Node make_node(std::mt19937_64& rng) {
    std::uniform_int_distribution<std::uint32_t> dist(0, kEmptyKey - 1);
    std::vector<std::uint32_t> keys;
    keys.reserve(kNodeKeys);
    while (keys.size() < kNodeKeys) {
        const std::uint32_t k = dist(rng);
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
            keys.push_back(k);
        }
    }
    std::sort(keys.begin(), keys.end());

    // Indexed copy rather than std::copy: the compiler cannot prove a vector's
    // length matches the fixed array, and warns about a potential overrun.
    Node n{};
    for (std::size_t i = 0; i < kNodeKeys; ++i) {
        n.keys[i] = keys[i];
    }
    return n;
}

inline std::vector<Node> make_nodes(std::size_t count, std::mt19937_64& rng) {
    std::vector<Node> nodes(count);
    for (auto& n : nodes) {
        n = make_node(rng);
    }
    return nodes;
}

// The branch-hostile probe sequence. Two properties matter and both are easy to
// get wrong.
//
// First, the hit/miss mix is randomized rather than blocked. A run of hits
// followed by a run of misses is perfectly predictable and hides exactly the
// mispredictions this project claims to remove.
//
// Second, hits land on a uniformly random slot. Always probing slot 0 lets the
// branchy kernel exit after one comparison and makes the scalar baseline look
// far better than it is on real data.
inline std::vector<std::uint32_t> make_probes(const std::vector<Node>& nodes,
                                              std::size_t count,
                                              double hit_ratio,
                                              std::mt19937_64& rng) {
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::uniform_int_distribution<std::size_t> node_pick(0, nodes.size() - 1);
    std::uniform_int_distribution<std::size_t> slot_pick(0, kNodeKeys - 1);
    std::uniform_int_distribution<std::uint32_t> any_key(0, kEmptyKey - 1);

    std::vector<std::uint32_t> probes;
    probes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (coin(rng) < hit_ratio) {
            probes.push_back(nodes[node_pick(rng)].keys[slot_pick(rng)]);
        } else {
            // A miss must genuinely be absent everywhere, or the measured hit
            // ratio drifts away from the requested one.
            std::uint32_t k;
            bool present;
            do {
                k = any_key(rng);
                present = false;
                for (const auto& n : nodes) {
                    if (std::find(std::begin(n.keys), std::end(n.keys), k) != std::end(n.keys)) {
                        present = true;
                        break;
                    }
                }
            } while (present);
            probes.push_back(k);
        }
    }
    return probes;
}

// ---------------------------------------------------------------------------
// The append-only node shape
// ---------------------------------------------------------------------------
//
// make_node above produces a node whose slots are sorted, which is what the
// equality kernels and lower_bound_* want. It is not what BasicMemTable holds.
// Since Phase 4b a slot is written once in insertion order and the sorted order
// over the slots lives in a separate 64-bit word, so the kernel the structure
// actually dispatches to -- lower_bound_perm_* -- has to permute the lanes into
// rank order before it can compare them.
//
// Measuring the permuted kernels against a sorted node would therefore measure
// the wrong thing twice over: the permutation would be the identity, so the
// vpermd would be free, and the lanes would already be in the order the compare
// wants. This builds the real shape instead -- a random slot assignment, which
// is what keys arriving in random order produce.
struct AppendNode {
    Node node;
    std::uint64_t order;
};

inline AppendNode make_append_node(std::mt19937_64& rng) {
    // kNodeCapacity live keys, not kNodeKeys. Slot kPadSlot must stay at the
    // sentinel: an unused rank decodes to it, which is the free padding the
    // permuted kernels rely on instead of a separate count.
    const Node sorted = make_node(rng);

    std::vector<int> slot_of_rank(kNodeCapacity);
    for (std::size_t i = 0; i < kNodeCapacity; ++i) {
        slot_of_rank[i] = static_cast<int>(i);
    }
    std::shuffle(slot_of_rank.begin(), slot_of_rank.end(), rng);

    AppendNode a{};
    for (auto& k : a.node.keys) {
        k = kEmptyKey;
    }
    a.order = 0;
    for (std::size_t rank = 0; rank < kNodeCapacity; ++rank) {
        const int slot = slot_of_rank[rank];
        a.node.keys[slot] = sorted.keys[rank];
        a.order |= static_cast<std::uint64_t>(slot + 1) << (4 * rank);
    }
    return a;
}

inline std::vector<AppendNode> make_append_nodes(std::size_t count, std::mt19937_64& rng) {
    std::vector<AppendNode> nodes(count);
    for (auto& n : nodes) {
        n = make_append_node(rng);
    }
    return nodes;
}

// make_probes for the append-only shape. Same two properties -- randomized
// hit/miss mix, hits on a uniformly random rank -- but it draws hits from the
// live ranks only. Drawing from all sixteen slots would return kEmptyKey for one
// slot in sixteen and quietly report the sentinel as a hit.
inline std::vector<std::uint32_t> make_perm_probes(const std::vector<AppendNode>& nodes,
                                                   std::size_t count,
                                                   double hit_ratio,
                                                   std::mt19937_64& rng) {
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::uniform_int_distribution<std::size_t> node_pick(0, nodes.size() - 1);
    std::uniform_int_distribution<std::size_t> rank_pick(0, kNodeCapacity - 1);
    std::uniform_int_distribution<std::uint32_t> any_key(0, kEmptyKey - 1);

    const auto present_anywhere = [&nodes](std::uint32_t k) {
        for (const auto& a : nodes) {
            for (std::size_t rank = 0; rank < kNodeCapacity; ++rank) {
                if (a.node.keys[order_slot(a.order, static_cast<int>(rank))] == k) {
                    return true;
                }
            }
        }
        return false;
    };

    std::vector<std::uint32_t> probes;
    probes.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (coin(rng) < hit_ratio) {
            const auto& a = nodes[node_pick(rng)];
            probes.push_back(a.node.keys[order_slot(a.order, static_cast<int>(rank_pick(rng)))]);
        } else {
            std::uint32_t k;
            do {
                k = any_key(rng);
            } while (present_anywhere(k));
            probes.push_back(k);
        }
    }
    return probes;
}

} // namespace aparajita::workload
