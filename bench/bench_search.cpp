// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Timing side of Phase 1. Two families:
//   probe/<kernel>      single L1-resident node, isolates comparison cost
//   working_set/<size>  node array swept across L1, L2, L3, DRAM

#include "aparajita/search.hpp"
#include "aparajita/workload.hpp"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <vector>

using namespace aparajita;

namespace {

constexpr std::size_t kProbes = 8192;
constexpr double kHitRatio = 0.5;

struct SingleNodeFixture {
    std::vector<Node> nodes;
    std::vector<std::uint32_t> probes;

    SingleNodeFixture() {
        std::mt19937_64 rng(0xA9A12A11);
        nodes = workload::make_nodes(1, rng);
        probes = workload::make_probes(nodes, kProbes, kHitRatio, rng);
    }
};

const SingleNodeFixture& single_node() {
    static const SingleNodeFixture f;
    return f;
}

// Registration is compile-time but ISA support is not, so a kernel the host
// cannot run has to opt out from inside the body. The test and counter_report
// both filter their kernel lists before dispatching; Google Benchmark has no
// equivalent hook, and an unguarded avx512 registration executes and faults on
// every pre-Ice-Lake host. SkipWithError reports it as a skip rather than a
// crash, which is what running one binary across mixed hardware requires.
bool isa_available(Isa needs) noexcept {
    const Isa host = detect_isa();
    switch (needs) {
        case Isa::Avx512: return host == Isa::Avx512;
        case Isa::Avx2:   return host == Isa::Avx2 || host == Isa::Avx512;
        case Isa::Scalar: return true;
    }
    return false;
}

template <SearchFn Fn, Isa Needs = Isa::Scalar>
void BM_Probe(benchmark::State& state) {
    if (!isa_available(Needs)) {
        state.SkipWithError("host lacks the required ISA");
        return;
    }
    const auto& f = single_node();
    const Node& node = f.nodes[0];
    for (auto _ : state) {
        for (std::uint32_t p : f.probes) {
            benchmark::DoNotOptimize(Fn(node, p));
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * kProbes);
}

// Node count is the benchmark argument. Random node selection defeats the
// hardware prefetcher, so the larger sizes actually pay for the cache miss
// rather than streaming through the array.
template <SearchFn Fn, Isa Needs = Isa::Scalar>
void BM_WorkingSet(benchmark::State& state) {
    if (!isa_available(Needs)) {
        state.SkipWithError("host lacks the required ISA");
        return;
    }
    const std::size_t node_count = static_cast<std::size_t>(state.range(0));

    std::mt19937_64 rng(0xC0FFEE);
    const auto nodes = workload::make_nodes(node_count, rng);

    std::vector<std::uint32_t> idx(kProbes);
    std::vector<std::uint32_t> keys(kProbes);
    std::uniform_int_distribution<std::size_t> node_pick(0, node_count - 1);
    std::uniform_int_distribution<std::size_t> slot_pick(0, kNodeKeys - 1);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    std::uniform_int_distribution<std::uint32_t> any_key(0, kEmptyKey - 1);

    for (std::size_t i = 0; i < kProbes; ++i) {
        const std::size_t n = node_pick(rng);
        idx[i] = static_cast<std::uint32_t>(n);
        keys[i] = coin(rng) < kHitRatio ? nodes[n].keys[slot_pick(rng)] : any_key(rng);
    }

    for (auto _ : state) {
        for (std::size_t i = 0; i < kProbes; ++i) {
            benchmark::DoNotOptimize(Fn(nodes[idx[i]], keys[i]));
        }
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * kProbes);
    state.SetLabel(std::to_string(node_count * sizeof(Node) / 1024) + " KiB");
}

} // namespace

BENCHMARK_TEMPLATE(BM_Probe, &search_scalar_linear)->Name("probe/scalar_linear");
BENCHMARK_TEMPLATE(BM_Probe, &search_scalar_autovec)->Name("probe/scalar_autovec");
BENCHMARK_TEMPLATE(BM_Probe, &search_scalar_branchless)->Name("probe/scalar_branchless");
BENCHMARK_TEMPLATE(BM_Probe, &search_scalar_binary)->Name("probe/scalar_binary");
#if APARAJITA_X86
BENCHMARK_TEMPLATE(BM_Probe, &search_avx2, Isa::Avx2)->Name("probe/avx2");
BENCHMARK_TEMPLATE(BM_Probe, &search_avx512, Isa::Avx512)->Name("probe/avx512");
#endif

// Sized to this host: 512 nodes = 32 KiB (L1d), 2048 = 128 KiB (L2, 256 KiB),
// 65536 = 4 MiB (L3, 8 MiB), 1048576 = 64 MiB (DRAM).
BENCHMARK_TEMPLATE(BM_WorkingSet, &search_scalar_linear)
    ->Name("working_set/scalar_linear")
    ->Arg(512)->Arg(2048)->Arg(65536)->Arg(1048576);
#if APARAJITA_X86
BENCHMARK_TEMPLATE(BM_WorkingSet, &search_avx2, Isa::Avx2)
    ->Name("working_set/avx2")
    ->Arg(512)->Arg(2048)->Arg(65536)->Arg(1048576);
BENCHMARK_TEMPLATE(BM_WorkingSet, &search_avx512, Isa::Avx512)
    ->Name("working_set/avx512")
    ->Arg(512)->Arg(2048)->Arg(65536)->Arg(1048576);
#endif

BENCHMARK_MAIN();
