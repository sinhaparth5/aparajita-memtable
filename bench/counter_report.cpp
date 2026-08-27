// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Phase 1 headline measurement: branch mispredictions and cycles per node
// lookup, for each search kernel, on a single L1-resident node.
//
// A single node is deliberate. It removes memory-hierarchy effects so what
// remains is the cost of the comparison strategy itself, which is the claim
// under test. The working-set sweep in bench_search.cpp covers the cache side.

#include "aparajita/search.hpp"
#include "aparajita/workload.hpp"
#include "counters.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aparajita;

namespace {

struct Kernel {
    const char* name;
    SearchFn fn;
    Isa needs;
};

volatile int g_sink = 0;

bench::Reading measure(SearchFn fn, const Node& node,
                       const std::vector<std::uint32_t>& probes,
                       const bench::CounterSet& c) {
    int acc = 0;

    // Warm the branch predictor and the line, so the measured window reflects
    // steady state rather than cold-start behavior.
    for (std::uint32_t p : probes) {
        acc += fn(node, p);
    }

    c.cycles.start();
    c.instructions.start();
    c.branches.start();
    c.branch_misses.start();

    for (std::uint32_t p : probes) {
        acc += fn(node, p);
    }

    bench::Reading r;
    r.branch_misses = c.branch_misses.stop();
    r.branches = c.branches.stop();
    r.instructions = c.instructions.stop();
    r.cycles = c.cycles.stop();

    g_sink = acc;
    return r;
}

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Reported alongside the median so a reader can tell a real gap from run noise.
// On a loaded or frequency-scaling host the spread matters more than the centre.
double stddev(const std::vector<double>& v) {
    double mean = 0.0;
    for (double x : v) mean += x;
    mean /= static_cast<double>(v.size());
    double acc = 0.0;
    for (double x : v) acc += (x - mean) * (x - mean);
    return std::sqrt(acc / static_cast<double>(v.size()));
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t probe_count = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 200000;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 9;
    const double hit_ratio = argc > 3 ? std::atof(argv[3]) : 0.5;

    const Isa host = detect_isa();
    std::printf("host ISA        : %s\n", isa_name(host));
    std::printf("probes per rep  : %zu\n", probe_count);
    std::printf("repetitions     : %d (median reported)\n", reps);
    std::printf("hit ratio       : %.2f\n\n", hit_ratio);

    bench::CounterSet counters;
    if (!counters.all_available()) {
        std::fprintf(stderr,
                     "hardware counters unavailable (errno=%d on branch-misses).\n"
                     "check /proc/sys/kernel/perf_event_paranoid is <= 2.\n",
                     counters.branch_misses.last_errno());
        return 1;
    }

    std::mt19937_64 rng(0xA9A12A11);
    const std::vector<Node> nodes = workload::make_nodes(1, rng);
    const Node& node = nodes[0];
    const auto probes = workload::make_probes(nodes, probe_count, hit_ratio, rng);

    const Kernel kernels[] = {
        {"scalar_linear",      &search_scalar_linear,      Isa::Scalar},
        {"scalar_autovec",     &search_scalar_autovec,     Isa::Scalar},
        {"scalar_branchless",  &search_scalar_branchless,  Isa::Scalar},
        {"scalar_binary",      &search_scalar_binary,      Isa::Scalar},
#if APARAJITA_X86
        {"avx2",               &search_avx2,               Isa::Avx2},
        {"avx512",             &search_avx512,             Isa::Avx512},
#endif
    };

    // Correctness gate. A faster kernel that returns the wrong slot is not a
    // result, so agreement is checked before any timing is reported.
    for (const auto& k : kernels) {
        if (k.needs == Isa::Avx2 && host == Isa::Scalar) continue;
        if (k.needs == Isa::Avx512 && host != Isa::Avx512) continue;
        for (std::uint32_t p : probes) {
            if (k.fn(node, p) != search_scalar_linear(node, p)) {
                std::fprintf(stderr, "kernel %s disagrees with scalar_linear on key %u\n",
                             k.name, p);
                return 2;
            }
        }
    }

    std::printf("%-18s %10s %8s %10s %6s %11s %10s %8s\n",
                "kernel", "cyc/probe", "cyc sd", "ins/probe", "IPC",
                "brnch/prb", "miss/prb", "miss%");
    std::printf("%s\n", std::string(90, '-').c_str());

    for (const auto& k : kernels) {
        if (k.needs == Isa::Avx2 && host == Isa::Scalar) {
            std::printf("%-18s %s\n", k.name, "skipped: host lacks avx2");
            continue;
        }
        if (k.needs == Isa::Avx512 && host != Isa::Avx512) {
            std::printf("%-18s %s\n", k.name, "skipped: host lacks avx512f (compiled, not executed)");
            continue;
        }

        std::vector<double> cyc, ins, brn, mis;
        for (int r = 0; r < reps; ++r) {
            const auto x = measure(k.fn, node, probes, counters);
            const double n = static_cast<double>(probe_count);
            cyc.push_back(static_cast<double>(x.cycles) / n);
            ins.push_back(static_cast<double>(x.instructions) / n);
            brn.push_back(static_cast<double>(x.branches) / n);
            mis.push_back(static_cast<double>(x.branch_misses) / n);
        }

        const double c = median(cyc), i = median(ins), b = median(brn), m = median(mis);
        std::printf("%-18s %10.2f %8.2f %10.2f %6.2f %11.2f %10.4f %7.2f%%\n",
                    k.name, c, stddev(cyc), i, i / c, b, m,
                    b > 0 ? 100.0 * m / b : 0.0);
    }

    return 0;
}
