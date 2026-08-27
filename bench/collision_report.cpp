// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Phase 2 exit criterion: the surrogate collision rate, measured on realistic key
// distributions rather than on sequential integers.
//
// The distinction matters more than it sounds. A 32-bit lane holds the first four
// bytes of the user key, so every key sharing a four-byte prefix collapses to one
// surrogate and the SIMD hit degenerates into a linear scan of candidates, each
// needing a full comparison. Sequential integers would hide this completely or
// expose it completely depending only on whether the varying digits fall inside
// the first four bytes, which is why measuring on them proves nothing either way.
//
// The number that matters is not the fraction of keys that collide. It is the
// expected number of full-key comparisons a lookup performs, which for a key drawn
// uniformly from the set is sum(m_i^2) / N over surrogate multiplicities m_i. A
// distribution can have a high collision rate and a low expected scan if the
// collisions are spread thin, and the reverse.

#include "aparajita/node.hpp"
#include "aparajita/surrogate.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace aparajita;

namespace {

struct Stats {
    std::size_t keys;
    std::size_t distinct;
    std::size_t largest_group;
    double expected_scan;
    double collision_rate;
};

Stats analyze(const std::vector<std::string>& keys) {
    std::map<std::uint32_t, std::size_t> hist;
    for (const auto& k : keys) {
        ++hist[surrogate(k)];
    }
    Stats s{};
    s.keys = keys.size();
    s.distinct = hist.size();
    s.largest_group = 0;
    double sum_sq = 0.0;
    std::size_t colliding = 0;
    for (const auto& [sur, m] : hist) {
        (void)sur;
        s.largest_group = std::max(s.largest_group, m);
        sum_sq += static_cast<double>(m) * static_cast<double>(m);
        if (m > 1) {
            colliding += m;
        }
    }
    s.expected_scan = sum_sq / static_cast<double>(keys.size());
    s.collision_rate = static_cast<double>(colliding) / static_cast<double>(keys.size());
    return s;
}

std::string hex_of(std::uint64_t v, int digits) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (int i = digits - 1; i >= 0; --i) {
        s.push_back(d[(v >> (4 * i)) & 0xF]);
    }
    return s;
}

// The same measurement against the surrogate the structure actually uses: taken
// after the prefix shared by the sixteen keys of one node, not from the start of
// the key. Keys are sorted and dealt into nodes of sixteen, which is how the
// memtable holds them, and each node strips its own prefix.
//
// The comparison between this and the global column is the whole point of the
// report. A lane that is useless on a table-prefixed keyspace becomes useful
// again once it stops spending its four bytes on bytes every key shares.
Stats analyze_paged(std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    Stats s{};
    s.keys = keys.size();
    s.distinct = 0;
    s.largest_group = 0;
    double sum_sq = 0.0;
    std::size_t colliding = 0;

    for (std::size_t i = 0; i < keys.size(); i += kNodeKeys) {
        const std::size_t n = std::min<std::size_t>(kNodeKeys, keys.size() - i);
        const std::size_t plen =
            n > 1 ? common_prefix_len(keys[i], keys[i + n - 1]) : 0;
        std::map<std::uint32_t, std::size_t> hist;
        for (std::size_t j = 0; j < n; ++j) {
            ++hist[surrogate_at(keys[i + j], plen)];
        }
        s.distinct += hist.size();
        for (const auto& [sur, m] : hist) {
            (void)sur;
            s.largest_group = std::max(s.largest_group, m);
            sum_sq += static_cast<double>(m) * static_cast<double>(m);
            if (m > 1) {
                colliding += m;
            }
        }
    }
    s.expected_scan = sum_sq / static_cast<double>(keys.size());
    s.collision_rate = static_cast<double>(colliding) / static_cast<double>(keys.size());
    return s;
}

using Gen = std::function<std::vector<std::string>(std::size_t, std::mt19937_64&)>;

std::vector<std::string> gen_random_binary(std::size_t n, std::mt19937_64& rng) {
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<std::string> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::string s(16, '\0');
        for (auto& c : s) c = static_cast<char>(byte(rng));
        v.push_back(std::move(s));
    }
    return v;
}

std::vector<std::string> gen_sequential(std::size_t n, std::mt19937_64&) {
    std::vector<std::string> v;
    v.reserve(n);
    char buf[32];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "user_%010zu", i);
        v.emplace_back(buf);
    }
    return v;
}

std::vector<std::string> gen_sequential_bigendian(std::size_t n, std::mt19937_64&) {
    // The same sequence with the varying bytes first, which is what a key encoded
    // as a big-endian integer looks like.
    std::vector<std::string> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::string s(8, '\0');
        for (int b = 0; b < 8; ++b) {
            s[static_cast<std::size_t>(b)] =
                static_cast<char>((i >> (8 * (7 - b))) & 0xFF);
        }
        v.push_back(std::move(s));
    }
    return v;
}

std::vector<std::string> gen_tenant_prefixed(std::size_t n, std::mt19937_64& rng) {
    // The shape the design doc warns about: a small set of prefixes shared across
    // an entire keyspace, which is how multi-tenant and per-table keys are built.
    std::uniform_int_distribution<int> tenant(0, 999);
    std::vector<std::string> v;
    v.reserve(n);
    char buf[64];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "t%03d:%s", tenant(rng), hex_of(rng(), 12).c_str());
        v.emplace_back(buf);
    }
    return v;
}

std::vector<std::string> gen_single_prefix(std::size_t n, std::mt19937_64& rng) {
    // The worst realistic case: one table prefix on every key.
    std::vector<std::string> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        v.push_back("rows:" + hex_of(rng(), 16));
    }
    return v;
}

std::vector<std::string> gen_uuid_hex(std::size_t n, std::mt19937_64& rng) {
    std::vector<std::string> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        v.push_back(hex_of(rng(), 16) + "-" + hex_of(rng(), 16));
    }
    return v;
}

std::vector<std::string> gen_timestamp(std::size_t n, std::mt19937_64& rng) {
    // Time-series keys: a big-endian millisecond timestamp then a series id. Real
    // timestamps advance slowly relative to the key count, so the leading bytes
    // barely move.
    std::uint64_t base = 1'750'000'000'000ull;
    std::vector<std::string> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t ts = base + (i / 10);
        std::string s(8, '\0');
        for (int b = 0; b < 8; ++b) {
            s[static_cast<std::size_t>(b)] = static_cast<char>((ts >> (8 * (7 - b))) & 0xFF);
        }
        s += hex_of(rng(), 4);
        v.push_back(std::move(s));
    }
    return v;
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 200000;

    const std::pair<const char*, Gen> gens[] = {
        {"random_binary", gen_random_binary},
        {"uuid_hex", gen_uuid_hex},
        {"sequential_bigendian", gen_sequential_bigendian},
        {"tenant_prefixed", gen_tenant_prefixed},
        {"timestamp_series", gen_timestamp},
        {"sequential_decimal", gen_sequential},
        {"single_prefix", gen_single_prefix},
    };

    std::printf("surrogate collision report: first 4 key bytes, big-endian\n");
    std::printf("keys per distribution: %zu\n\n", n);
    std::printf("%-22s %12s %12s   %s\n", "distribution", "global", "per-node", "verdict");
    std::printf("%-22s %12s %12s\n", "", "exp scan", "exp scan");
    std::printf("%s\n", std::string(72, '-').c_str());

    for (const auto& [name, gen] : gens) {
        std::mt19937_64 rng(0xC0FFEE);
        const auto keys = gen(n, rng);
        const Stats g = analyze(keys);
        const Stats pn = analyze_paged(keys);
        // A node holds sixteen keys, so an expected scan at or above that means
        // the SIMD step has stopped narrowing anything within a node.
        auto verdict = [](double e) {
            return e < 1.5 ? "SIMD effective" : e < 16.0 ? "degraded" : "SIMD defeated";
        };
        std::printf("%-22s %12.1f %12.1f   %s\n", name, g.expected_scan, pn.expected_scan,
                    verdict(pn.expected_scan));
    }

    std::printf("\nexp scan is the expected number of full-key comparisons per lookup,\n");
    std::printf("sum(m^2)/N over surrogate multiplicities. 1.0 is a perfect lane, 16\n");
    std::printf("means the lane discriminates nothing within a node.\n\n");
    std::printf("global   = surrogate taken from the start of the key\n");
    std::printf("per-node = taken after the prefix shared by one node's sixteen keys,\n");
    std::printf("           which is what memtable.hpp actually does\n");
    return 0;
}
