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

} // namespace aparajita::workload
