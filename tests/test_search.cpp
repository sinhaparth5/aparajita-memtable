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
    // precondition still holds. Phase 2 settled on an ordered node, so that
    // precondition is now the structure's invariant rather than one kernel's
    // quirk, and the lower_bound kernels below depend on it too.
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

    // -----------------------------------------------------------------------
    // lower_bound kernels: the ordered path
    // -----------------------------------------------------------------------

    struct LbKernel {
        const char* name;
        LowerBoundFn fn;
        bool runnable;
    };
    std::vector<LbKernel> lbs = {
        {"lb_scalar", &lower_bound_scalar, true},
        {"lb_scalar_branchless", &lower_bound_scalar_branchless, true},
    };
#if APARAJITA_X86
    {
        const Isa h = detect_isa();
        lbs.push_back({"lb_avx2", &lower_bound_avx2, h == Isa::Avx2 || h == Isa::Avx512});
        lbs.push_back({"lb_avx512", &lower_bound_avx512, h == Isa::Avx512});
    }
#endif
    std::printf("\nlower_bound kernels:\n");
    for (const auto& k : lbs) {
        std::printf("  %-22s %s\n", k.name, k.runnable ? "testing" : "skipped (unsupported host)");
    }

    // Exact hits, gaps, and both ends. A key present at slot i must return i; a
    // key falling between slots must return the slot above it.
    {
        Node n{};
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            n.keys[i] = static_cast<std::uint32_t>(100 + i * 7);
        }
        for (const auto& k : lbs) {
            if (!k.runnable) continue;
            char msg[160];
            for (std::size_t i = 0; i < kNodeKeys; ++i) {
                std::snprintf(msg, sizeof(msg), "%s: exact hit at slot %zu", k.name, i);
                check(k.fn(n, n.keys[i]) == static_cast<int>(i), msg);
                std::snprintf(msg, sizeof(msg), "%s: gap below slot %zu", k.name, i);
                check(k.fn(n, n.keys[i] - 1) == static_cast<int>(i), msg);
            }
            std::snprintf(msg, sizeof(msg), "%s: below every key returns 0", k.name);
            check(k.fn(n, 0u) == 0, msg);
            std::snprintf(msg, sizeof(msg), "%s: above every key returns kNodeKeys", k.name);
            check(k.fn(n, 100000u) == static_cast<int>(kNodeKeys), msg);
        }
    }

    // The signed-compare trap. _mm256_cmpgt_epi32 is signed, so a kernel that
    // forgets to bias reads 0x80000000 and above as negative and orders them
    // below every small key. These keys straddle that boundary deliberately, and
    // this is the case that fails loudly when the bias is missing.
    {
        Node n{};
        const std::uint32_t spread[] = {1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xFFFFFFFEu};
        for (std::size_t i = 0; i < kNodeKeys; ++i) {
            n.keys[i] = i < std::size(spread) ? spread[i] : kEmptyKey;
        }
        for (const auto& k : lbs) {
            if (!k.runnable) continue;
            char msg[160];
            std::snprintf(msg, sizeof(msg), "%s: high-bit key orders above small keys", k.name);
            check(k.fn(n, 0x80000000u) == 3, msg);
            std::snprintf(msg, sizeof(msg), "%s: key just under the signed boundary", k.name);
            check(k.fn(n, 0x7FFFFFFFu) == 2, msg);
            std::snprintf(msg, sizeof(msg), "%s: largest real key", k.name);
            check(k.fn(n, 0xFFFFFFFEu) == 5, msg);
            // kEmptyKey padding sorts above every real key, so a needle above all
            // six reals lands at 6 rather than at kNodeKeys.
            std::snprintf(msg, sizeof(msg), "%s: sentinel tail bounds the search", k.name);
            check(k.fn(n, 0xFFFFFFFFu) == 6, msg);
        }
    }

    // Randomized agreement against the branchy reference, over sorted nodes.
    {
        std::mt19937_64 rng(999);
        const auto nodes = workload::make_nodes(64, rng);
        const auto probes = workload::make_probes(nodes, 20000, 0.5, rng);
        for (const auto& node : nodes) {
            for (std::uint32_t p : probes) {
                const int expect = lower_bound_scalar(node, p);
                for (const auto& k : lbs) {
                    if (!k.runnable) continue;
                    if (k.fn(node, p) != expect) {
                        char msg[160];
                        std::snprintf(msg, sizeof(msg),
                                      "%s: disagrees on key %u (got %d, want %d)",
                                      k.name, p, k.fn(node, p), expect);
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
