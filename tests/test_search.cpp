// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Correctness gate for the search kernels. Every kernel must agree with the
// branchy scalar reference on every input, including the cases the SIMD paths
// are most likely to get wrong: a hit in the upper 256-bit half, a hit in the
// last slot, and the empty-slot sentinel.

#include "aparajita/search.hpp"
#include "aparajita/workload.hpp"

#include <cstdio>
#include <iterator>
#include <random>
#include <vector>

using namespace aparajita;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Kernel {
    const char* name;
    SearchFn fn;
    bool runnable;
};

std::vector<Kernel> runnable_kernels() {
    const Isa host = detect_isa();
    std::vector<Kernel> k = {
        {"scalar_linear", &search_scalar_linear, true},
        {"scalar_autovec", &search_scalar_autovec, true},
        {"scalar_branchless", &search_scalar_branchless, true},
        {"scalar_binary", &search_scalar_binary, true},
    };
#if APARAJITA_X86
    k.push_back({"avx2", &search_avx2, host == Isa::Avx2 || host == Isa::Avx512});
    k.push_back({"avx512", &search_avx512, host == Isa::Avx512});
#endif
    return k;
}

} // namespace

int main() {
    static_assert(sizeof(Node) == 64);
    static_assert(alignof(Node) == 64);

    const auto kernels = runnable_kernels();
    std::printf("host ISA: %s\n", isa_name(detect_isa()));
    for (const auto& k : kernels) {
        std::printf("  %-18s %s\n", k.name, k.runnable ? "testing" : "skipped (unsupported host)");
    }

    // Every slot, so an off-by-one in the upper-half mask shift cannot hide.
    {
        Node n{};
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            n.keys[i] = static_cast<std::uint32_t>(100 + i * 7);
        }
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            for (const auto& k : kernels) {
                if (!k.runnable) continue;
                char msg[128];
                std::snprintf(msg, sizeof(msg), "%s: slot %zu", k.name, i);
                check(k.fn(n, n.keys[i]) == static_cast<int>(i), msg);
            }
        }
        for (const auto& k : kernels) {
            if (!k.runnable) continue;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s: absent key returns kNotFound", k.name);
            check(k.fn(n, 99u) == kNotFound, msg);
        }
    }

    // A partially filled node. Sentinels occupy the tail rather than being
    // scattered, which is why kEmptyKey is 0xFFFFFFFF: it sorts above every real
    // key, so a partially filled node stays sorted and search_scalar_binary's
    // precondition still holds. The SIMD kernels have no such precondition,
    // which is a point in favour of an unordered node layout in Phase 2.
    {
        Node n{};
        const std::uint32_t real[] = {100u, 200u, 300u, 400u, 500u, 4242u};
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            n.keys[i] = i < std::size(real) ? real[i] : kEmptyKey;
        }
        for (const auto& k : kernels) {
            if (!k.runnable) continue;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s: partially filled node finds last real key", k.name);
            check(k.fn(n, 4242u) == 5, msg);
            std::snprintf(msg, sizeof(msg), "%s: partially filled node misses absent key", k.name);
            check(k.fn(n, 7u) == kNotFound, msg);
        }
    }

    // Randomized agreement against the reference.
    {
        std::mt19937_64 rng(12345);
        const auto nodes = workload::make_nodes(64, rng);
        const auto probes = workload::make_probes(nodes, 20000, 0.5, rng);
        for (const auto& node : nodes) {
            for (std::uint32_t p : probes) {
                const int expect = search_scalar_linear(node, p);
                for (const auto& k : kernels) {
                    if (!k.runnable) continue;
                    if (k.fn(node, p) != expect) {
                        char msg[160];
                        std::snprintf(msg, sizeof(msg), "%s: disagrees on key %u", k.name, p);
                        check(false, msg);
                        return 1;
                    }
                }
            }
        }
    }

    if (g_failures == 0) {
        std::printf("\nall kernel tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d failure(s)\n", g_failures);
    return 1;
}
