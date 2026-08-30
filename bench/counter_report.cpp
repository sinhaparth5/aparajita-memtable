// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Phase 1 headline measurement: branch mispredictions, cycles, and effective
// frequency per node lookup, for each search kernel, on a single L1-resident node.
//
// A single node is deliberate. It removes memory-hierarchy effects so what
// remains is the cost of the comparison strategy itself, which is the claim
// under test. The working-set sweep in bench_search.cpp covers the cache side.
//
// Three kernel families are reported, and the third is the one the structure
// runs. Phase 1 measured equality only, which was the whole surface at the time.
// Since Phase 4b, BasicMemTable dispatches every lookup to lower_bound_perm_*,
// so quoting an equality number as if it were the structure's probe cost would
// be quoting a kernel nothing calls. The ordered families are here so a Phase 4
// number can be taken from the kernel that actually runs, and so the price of
// the append-only layout -- lower_bound_perm_* against lower_bound_* on the same
// keys -- is a measured quantity rather than an argued one.

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

// Ordered over a node whose slots are sorted. Nothing in the structure calls
// these any more; they are the reference the permuted kernels are measured
// against, which is what makes "what permuting costs" a number.
struct LbKernel {
    const char* name;
    LowerBoundFn fn;
    Isa needs;
};

// Ordered over an append-only node: slots in insertion order, sorted order in a
// separate word. This is what BasicMemTable dispatches to.
struct PermKernel {
    const char* name;
    LowerBoundPermFn fn;
    Isa needs;
};

volatile int g_sink = 0;

void start_all(const bench::CounterSet& c) {
    c.cycles.start();
    c.ref_cycles.start();
    c.instructions.start();
    c.branches.start();
    c.branch_misses.start();
}

bench::Reading stop_all(const bench::CounterSet& c) {
    bench::Reading r;
    r.branch_misses = c.branch_misses.stop();
    r.branches = c.branches.stop();
    r.instructions = c.instructions.stop();
    r.ref_cycles = c.ref_cycles.stop();
    r.cycles = c.cycles.stop();
    return r;
}

// Three near-identical measurement loops rather than one templated one. The loop
// body is the thing being measured, so it is written out at each call signature
// and left as tight as it can be; wrapping the three in a generic callable would
// put the harness inside the window it is measuring.
bench::Reading measure(SearchFn fn, const Node& node,
                       const std::vector<std::uint32_t>& probes,
                       const bench::CounterSet& c) {
    int acc = 0;

    // Warm the branch predictor and the line, so the measured window reflects
    // steady state rather than cold-start behavior.
    for (std::uint32_t p : probes) {
        acc += fn(node, p);
    }

    start_all(c);
    for (std::uint32_t p : probes) {
        acc += fn(node, p);
    }
    const bench::Reading r = stop_all(c);

    g_sink = acc;
    return r;
}

bench::Reading measure_lb(LowerBoundFn fn, const Node& node,
                          const std::vector<std::uint32_t>& probes,
                          const bench::CounterSet& c) {
    int acc = 0;
    for (std::uint32_t p : probes) {
        acc += fn(node, p);
    }

    start_all(c);
    for (std::uint32_t p : probes) {
        acc += fn(node, p);
    }
    const bench::Reading r = stop_all(c);

    g_sink = acc;
    return r;
}

// The order word is a loop invariant here, which is honest: a real lookup takes
// one acquire load of it per node and then probes that node once. Hoisting it
// would not be, if the loop swept many nodes -- see the working-set sweep.
bench::Reading measure_perm(LowerBoundPermFn fn, const workload::AppendNode& a,
                            const std::vector<std::uint32_t>& probes,
                            const bench::CounterSet& c) {
    int acc = 0;
    for (std::uint32_t p : probes) {
        acc += fn(a.node, p, a.order);
    }

    start_all(c);
    for (std::uint32_t p : probes) {
        acc += fn(a.node, p, a.order);
    }
    const bench::Reading r = stop_all(c);

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

bool runnable(Isa needs, Isa host) {
    if (needs == Isa::Avx2) return host == Isa::Avx2 || host == Isa::Avx512;
    if (needs == Isa::Avx512) return host == Isa::Avx512;
    return true;
}

const char* skip_reason(Isa needs) {
    return needs == Isa::Avx512 ? "skipped: host lacks avx512f (compiled, not executed)"
                                : "skipped: host lacks avx2";
}

void print_header() {
    std::printf("%-18s %10s %8s %10s %6s %11s %10s %8s %8s\n",
                "kernel", "cyc/probe", "cyc sd", "ins/probe", "IPC",
                "brnch/prb", "miss/prb", "miss%", "freq/nom");
    std::printf("%s\n", std::string(99, '-').c_str());
}

// Runs `take` once per repetition and prints the median row. The lambda is
// invoked outside the counter window by measure_*, so nothing here is measured.
template <typename Take>
void report(const char* name, int reps, std::size_t probe_count, Take&& take) {
    std::vector<double> cyc, ins, brn, mis, freq;
    for (int r = 0; r < reps; ++r) {
        const bench::Reading x = take();
        const double n = static_cast<double>(probe_count);
        cyc.push_back(static_cast<double>(x.cycles) / n);
        ins.push_back(static_cast<double>(x.instructions) / n);
        brn.push_back(static_cast<double>(x.branches) / n);
        mis.push_back(static_cast<double>(x.branch_misses) / n);
        // Effective frequency as a multiple of nominal. Guarded because
        // ref-cycles reads zero on a host that silently refused the event,
        // and a division by it would print an infinity that looks like data.
        freq.push_back(x.ref_cycles > 0
                           ? static_cast<double>(x.cycles) / static_cast<double>(x.ref_cycles)
                           : 0.0);
    }

    const double c = median(cyc), i = median(ins), b = median(brn), m = median(mis);
    std::printf("%-18s %10.2f %8.2f %10.2f %6.2f %11.2f %10.4f %7.2f%% %8.3f\n",
                name, c, stddev(cyc), i, i / c, b, m,
                b > 0 ? 100.0 * m / b : 0.0, median(freq));
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

    // A separate node for the permuted family, in the shape the structure holds.
    // Its keys differ from `node`'s, so the two ordered tables are not compared
    // against each other key for key -- they are compared as distributions over
    // the same generator, which is what the sorted/permuted question needs.
    const std::vector<workload::AppendNode> append_nodes = workload::make_append_nodes(1, rng);
    const workload::AppendNode& anode = append_nodes[0];
    const auto perm_probes = workload::make_perm_probes(append_nodes, probe_count, hit_ratio, rng);

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

    const LbKernel lb_kernels[] = {
        {"lb_scalar",          &lower_bound_scalar,            Isa::Scalar},
        {"lb_scalar_brless",   &lower_bound_scalar_branchless, Isa::Scalar},
#if APARAJITA_X86
        {"lb_avx2",            &lower_bound_avx2,              Isa::Avx2},
        {"lb_avx512",          &lower_bound_avx512,            Isa::Avx512},
#endif
    };

    const PermKernel perm_kernels[] = {
        {"perm_scalar",        &lower_bound_perm_scalar,       Isa::Scalar},
#if APARAJITA_X86
        {"perm_avx2",          &lower_bound_perm_avx2,         Isa::Avx2},
        {"perm_avx512",        &lower_bound_perm_avx512,       Isa::Avx512},
#endif
    };

    // Correctness gate. A faster kernel that returns the wrong slot is not a
    // result, so agreement is checked before any timing is reported. Each family
    // is checked against its own scalar reference: the three answer different
    // questions and none of them is a drop-in for another.
    for (const auto& k : kernels) {
        if (!runnable(k.needs, host)) continue;
        for (std::uint32_t p : probes) {
            if (k.fn(node, p) != search_scalar_linear(node, p)) {
                std::fprintf(stderr, "kernel %s disagrees with scalar_linear on key %u\n",
                             k.name, p);
                return 2;
            }
        }
    }
    for (const auto& k : lb_kernels) {
        if (!runnable(k.needs, host)) continue;
        for (std::uint32_t p : probes) {
            if (k.fn(node, p) != lower_bound_scalar(node, p)) {
                std::fprintf(stderr, "kernel %s disagrees with lower_bound_scalar on key %u\n",
                             k.name, p);
                return 2;
            }
        }
    }
    for (const auto& k : perm_kernels) {
        if (!runnable(k.needs, host)) continue;
        for (std::uint32_t p : perm_probes) {
            if (k.fn(anode.node, p, anode.order) !=
                lower_bound_perm_scalar(anode.node, p, anode.order)) {
                std::fprintf(stderr,
                             "kernel %s disagrees with lower_bound_perm_scalar on key %u\n",
                             k.name, p);
                return 2;
            }
        }
    }

    std::printf("== equality: search_* over a sorted node ==\n");
    print_header();
    for (const auto& k : kernels) {
        if (!runnable(k.needs, host)) {
            std::printf("%-18s %s\n", k.name, skip_reason(k.needs));
            continue;
        }
        report(k.name, reps, probe_count,
               [&] { return measure(k.fn, node, probes, counters); });
    }

    std::printf("\n== ordered: lower_bound_* over a sorted node (the reference) ==\n");
    print_header();
    for (const auto& k : lb_kernels) {
        if (!runnable(k.needs, host)) {
            std::printf("%-18s %s\n", k.name, skip_reason(k.needs));
            continue;
        }
        report(k.name, reps, probe_count,
               [&] { return measure_lb(k.fn, node, probes, counters); });
    }

    std::printf("\n== ordered: lower_bound_perm_* over an append-only node (what runs) ==\n");
    print_header();
    for (const auto& k : perm_kernels) {
        if (!runnable(k.needs, host)) {
            std::printf("%-18s %s\n", k.name, skip_reason(k.needs));
            continue;
        }
        report(k.name, reps, probe_count,
               [&] { return measure_perm(k.fn, anode, perm_probes, counters); });
    }

    return 0;
}
