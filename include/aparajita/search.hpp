// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include "aparajita/node.hpp"

#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#define APARAJITA_X86 1
#include <immintrin.h>
#else
#define APARAJITA_X86 0
#endif

namespace aparajita {

// Every kernel returns the index of the first slot equal to `key`, or kNotFound.
// Signature is identical across kernels so the benchmark can template over them.
using SearchFn = int (*)(const Node&, std::uint32_t) noexcept;

// "Not found" is kNodeKeys, one past the last valid slot, rather than -1.
//
// This is not cosmetic. A -1 convention forces `mask ? ctz(mask) : -1`, and at a
// realistic hit ratio that ternary is a data-dependent branch the predictor
// cannot learn, costing roughly 0.5 mispredictions per probe. It measured as the
// single largest remaining source of misprediction in the vector kernels, which
// is exactly what this project exists to remove. Folding the sentinel into the
// mask as bit 16 makes ctz produce kNodeKeys on a miss with no branch at all.
inline constexpr int kNotFound = static_cast<int>(kNodeKeys);

// ---------------------------------------------------------------------------
// Scalar baselines
// ---------------------------------------------------------------------------

// The honest branchy baseline: one conditional jump per key, early exit on hit.
// Vectorization is disabled explicitly. Without that, GCC turns this into the
// AVX2 kernel on its own and the comparison measures nothing.
inline int search_scalar_linear(const Node& n, std::uint32_t key) noexcept {
#if defined(__clang__)
#pragma clang loop vectorize(disable)
#elif defined(__GNUC__)
#pragma GCC novector
#endif
    for (int i = 0; i < static_cast<int>(kNodeKeys); ++i) {
        if (n.keys[i] == key) {
            return i;
        }
    }
    return kNotFound;
}

// Same loop with the compiler left alone. This is the fair "what you get for
// free" data point, and the gap between it and the hand-written kernel is the
// number that says whether the intrinsics are worth maintaining.
inline int search_scalar_autovec(const Node& n, std::uint32_t key) noexcept {
    for (int i = 0; i < static_cast<int>(kNodeKeys); ++i) {
        if (n.keys[i] == key) {
            return i;
        }
    }
    return kNotFound;
}

// Branch-free scalar: no early exit, so the loop has one predictable backedge
// instead of sixteen data-dependent jumps. Isolates how much of the SIMD win is
// vector width and how much is simply removing the branch.
inline int search_scalar_branchless(const Node& n, std::uint32_t key) noexcept {
    std::uint32_t mask = 0;
    for (int i = 0; i < static_cast<int>(kNodeKeys); ++i) {
        mask |= static_cast<std::uint32_t>(n.keys[i] == key) << i;
    }
    return __builtin_ctz(mask | (1u << kNodeKeys));
}

// Binary search over a sorted node. Included because it is the comparison a
// reviewer will ask for: a skiplist node search is closer to this than to a
// linear scan. Requires n.keys sorted ascending.
inline int search_scalar_binary(const Node& n, std::uint32_t key) noexcept {
    int lo = 0;
    int hi = static_cast<int>(kNodeKeys) - 1;
    while (lo <= hi) {
        const int mid = lo + ((hi - lo) >> 1);
        const std::uint32_t v = n.keys[mid];
        if (v == key) {
            return mid;
        }
        if (v < key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return kNotFound;
}

#if APARAJITA_X86

// ---------------------------------------------------------------------------
// AVX2: two 256-bit compares over the line
// ---------------------------------------------------------------------------

// Note on the movemask choice. _mm256_movemask_epi8 over a _mm256_cmpeq_epi32
// result yields 32 bits, four per lane, so a trailing-zero count needs dividing
// by 4. _mm256_movemask_ps on a cast of the same vector yields 8 bits, one per
// 32-bit lane, which is what we want and one instruction cheaper to consume.
__attribute__((target("avx2")))
inline int search_avx2(const Node& n, std::uint32_t key) noexcept {
    const __m256i needle = _mm256_set1_epi32(static_cast<int>(key));

    const __m256i lo = _mm256_load_si256(reinterpret_cast<const __m256i*>(n.keys));
    const __m256i hi = _mm256_load_si256(reinterpret_cast<const __m256i*>(n.keys) + 1);

    const __m256i eq_lo = _mm256_cmpeq_epi32(lo, needle);
    const __m256i eq_hi = _mm256_cmpeq_epi32(hi, needle);

    const unsigned m_lo = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(eq_lo)));
    const unsigned m_hi = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(eq_hi)));

    // Bit 16 is the miss sentinel, so ctz yields kNotFound with no branch.
    const unsigned mask = m_lo | (m_hi << 8);
    return static_cast<int>(__builtin_ctz(mask | (1u << kNodeKeys)));
}

// ---------------------------------------------------------------------------
// AVX-512: one 512-bit compare, mask register straight out
// ---------------------------------------------------------------------------

// Compiled but not executed on any pre-Skylake-SP Intel part or most AMD parts
// before Zen 4. Reached only through the CPUID gate in search_dispatch().
__attribute__((target("avx512f")))
inline int search_avx512(const Node& n, std::uint32_t key) noexcept {
    const __m512i needle = _mm512_set1_epi32(static_cast<int>(key));
    const __m512i line = _mm512_load_si512(reinterpret_cast<const void*>(n.keys));
    const __mmask16 mask = _mm512_cmpeq_epi32_mask(line, needle);
    return static_cast<int>(__builtin_ctz(static_cast<unsigned>(mask) | (1u << kNodeKeys)));
}

#endif // APARAJITA_X86

// ---------------------------------------------------------------------------
// Runtime dispatch
// ---------------------------------------------------------------------------

enum class Isa { Scalar, Avx2, Avx512 };

inline Isa detect_isa() noexcept {
#if APARAJITA_X86
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f")) {
        return Isa::Avx512;
    }
    if (__builtin_cpu_supports("avx2")) {
        return Isa::Avx2;
    }
#endif
    return Isa::Scalar;
}

inline const char* isa_name(Isa isa) noexcept {
    switch (isa) {
        case Isa::Avx512: return "avx512f";
        case Isa::Avx2:   return "avx2";
        case Isa::Scalar: return "scalar";
    }
    return "unknown";
}

// Resolved once, not per call. A function pointer costs an indirect jump, which
// is exactly what the project is trying to remove from the hot path, so Phase 2
// should hoist this decision to a template parameter instead.
inline SearchFn search_dispatch() noexcept {
    switch (detect_isa()) {
#if APARAJITA_X86
        case Isa::Avx512: return &search_avx512;
        case Isa::Avx2:   return &search_avx2;
#endif
        default:          return &search_scalar_branchless;
    }
}

} // namespace aparajita
