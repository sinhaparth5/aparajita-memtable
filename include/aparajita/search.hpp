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
// Ordered search: lower_bound over a sorted node
// ---------------------------------------------------------------------------

// The kernels above answer "is this key present", which is all a point lookup
// needs. RocksDB needs more than that: MemTableRep iterators must yield keys in
// comparator order, and Seek(target) must land on the first key >= target. This
// is the primitive that supplies both, and it is why Phase 2 chose an ordered
// structure over VectorRep's sort-at-flush.
//
// The trick is that over a *sorted* node the compare mask is a prefix of set
// bits, so its popcount is exactly the number of keys below the needle, which is
// exactly lower_bound. No trailing-zero count, no sentinel bit, and no branch.
// It also falls out that "greater than everything in this node" returns
// kNodeKeys, matching the kNotFound convention the equality kernels established.
//
// Every kernel here requires n.keys sorted ascending. kEmptyKey padding in the
// tail satisfies that by construction, since it sorts above every real key.
using LowerBoundFn = int (*)(const Node&, std::uint32_t) noexcept;

// Branchy reference. Vectorization disabled for the same reason as
// search_scalar_linear: left alone the compiler reaches for the vector unit and
// the baseline stops being a baseline.
inline int lower_bound_scalar(const Node& n, std::uint32_t key) noexcept {
    int i = 0;
#if defined(__clang__)
#pragma clang loop vectorize(disable)
#elif defined(__GNUC__)
#pragma GCC novector
#endif
    while (i < static_cast<int>(kNodeKeys) && n.keys[i] < key) {
        ++i;
    }
    return i;
}

// Counts rather than early-exits, so the loop has one predictable backedge.
inline int lower_bound_scalar_branchless(const Node& n, std::uint32_t key) noexcept {
    int count = 0;
    for (int i = 0; i < static_cast<int>(kNodeKeys); ++i) {
        count += static_cast<int>(n.keys[i] < key);
    }
    return count;
}

#if APARAJITA_X86

// _mm256_cmpgt_epi32 is a SIGNED compare and these keys are unsigned, which is a
// trap rather than a detail: kEmptyKey is 0xFFFFFFFF, which reads as -1 signed
// and would sort below every real key instead of above it, inverting the padding.
// XOR-ing both sides by 0x80000000 maps the unsigned order onto the signed one.
// kEmptyKey biases to INT32_MAX, so the sentinel keeps sorting above everything,
// exactly as it does unbiased.
__attribute__((target("avx2")))
inline int lower_bound_avx2(const Node& n, std::uint32_t key) noexcept {
    const __m256i bias = _mm256_set1_epi32(INT32_MIN);
    const __m256i needle = _mm256_xor_si256(_mm256_set1_epi32(static_cast<int>(key)), bias);

    const __m256i lo = _mm256_load_si256(reinterpret_cast<const __m256i*>(n.keys));
    const __m256i hi = _mm256_load_si256(reinterpret_cast<const __m256i*>(n.keys) + 1);

    // keys[i] < key, expressed as needle > keys[i] with the identical bias applied
    // to both sides so the relation is preserved.
    const __m256i lt_lo = _mm256_cmpgt_epi32(needle, _mm256_xor_si256(lo, bias));
    const __m256i lt_hi = _mm256_cmpgt_epi32(needle, _mm256_xor_si256(hi, bias));

    const unsigned m_lo = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(lt_lo)));
    const unsigned m_hi = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(lt_hi)));

    return __builtin_popcount(m_lo | (m_hi << 8));
}

// AVX-512 has a native unsigned compare, so the bias the AVX2 path needs
// disappears entirely: four instructions become two, and the sign-extension trap
// above cannot arise.
__attribute__((target("avx512f")))
inline int lower_bound_avx512(const Node& n, std::uint32_t key) noexcept {
    const __m512i needle = _mm512_set1_epi32(static_cast<int>(key));
    const __m512i line = _mm512_load_si512(reinterpret_cast<const void*>(n.keys));
    const __mmask16 mask = _mm512_cmplt_epu32_mask(line, needle);
    return __builtin_popcount(static_cast<unsigned>(mask));
}

#endif // APARAJITA_X86

// ---------------------------------------------------------------------------
// Ordered search over an append-only node
// ---------------------------------------------------------------------------
//
// The kernels above require the surrogates to be sorted in memory, which was
// true while every insert rebuilt the node. Phase 4b stops rebuilding: a slot is
// written once and the sorted order over the slots is republished as a 64-bit
// order word (see node.hpp). The lane array is therefore in slot order, which is
// insertion order, which is not sorted.
//
// Permuting it back is one instruction on AVX-512. `vpermd` takes exactly the
// sixteen 32-bit indices the order word packs, so the sorted line is materialised
// in a register and the prefix-mask popcount that made lower_bound branchless in
// Phase 2 is unchanged behind it. AVX2 has no 16-lane crossing permute, so each
// output half is gathered from both source halves and blended on the index's
// high bit -- four permutes and two blends instead of one instruction, which is
// the widest the two ISAs have diverged anywhere in this project.
//
// Two details do real work.
//
// Unused ranks decode to slot 15, and slot 15 is the padding lane no key ever
// occupies, so the tail of the permuted line is kEmptyKey without a count, a
// mask or a blend. That is what the order word's `slot + 1` encoding bought.
//
// The load is masked to the live slots even so, and that is about concurrency
// rather than about the result. A writer appending to this node is storing into
// slot `count` while these kernels run, and an unmasked 64-byte load would read
// the bytes it is writing -- a data race, even though the permutation could not
// select the lane. Masking to slots below `count` keeps the reader off it. The
// merge source is kEmptyKey, which is what those slots already hold, so the mask
// changes no answer this kernel can return.
using LowerBoundPermFn = int (*)(const Node&, std::uint32_t, std::uint64_t) noexcept;

// The reference the vector kernels are checked against. Branchy and unvectorized
// for the same reason lower_bound_scalar is: a baseline the compiler has turned
// into a vector kernel is not a baseline.
inline int lower_bound_perm_scalar(const Node& n, std::uint32_t key,
                                   std::uint64_t order) noexcept {
    const int count = order_count(order);
    int rank = 0;
#if defined(__clang__)
#pragma clang loop vectorize(disable)
#elif defined(__GNUC__)
#pragma GCC novector
#endif
    while (rank < count && n.keys[order_slot(order, rank)] < key) {
        ++rank;
    }
    return rank;
}

#if APARAJITA_X86

// Spreading sixteen nibbles into sixteen dwords is the only part that is not
// obvious. Broadcasting the order word gives dwords that alternate low half and
// high half; one permute reshapes that into "low half for ranks 0-7, high half
// for ranks 8-15", after which a per-lane variable shift and a mask leave each
// lane holding its own nibble. Subtracting one turns `slot + 1` into the slot and
// carries an unused rank's zero to 15, the padding lane.
__attribute__((target("avx512f")))
inline int lower_bound_perm_avx512(const Node& n, std::uint32_t key,
                                   std::uint64_t order) noexcept {
    const __m512i ones = _mm512_set1_epi32(1);
    const __m512i nibble = _mm512_set1_epi32(0xF);

    const __m512i halves = _mm512_set1_epi64(static_cast<long long>(order));
    const __m512i pick = _mm512_set_epi32(1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0);
    const __m512i shifts = _mm512_set_epi32(28, 24, 20, 16, 12, 8, 4, 0,
                                            28, 24, 20, 16, 12, 8, 4, 0);
    const __m512i lanes = _mm512_permutexvar_epi32(pick, halves);
    const __m512i nib = _mm512_and_si512(_mm512_srlv_epi32(lanes, shifts), nibble);
    const __m512i idx = _mm512_and_si512(_mm512_sub_epi32(nib, ones), nibble);

    const __mmask16 live = static_cast<__mmask16>((1u << order_count(order)) - 1u);
    const __m512i line = _mm512_mask_load_epi32(_mm512_set1_epi32(static_cast<int>(kEmptyKey)),
                                                live, reinterpret_cast<const void*>(n.keys));
    const __m512i sorted = _mm512_permutexvar_epi32(idx, line);

    const __mmask16 lt =
        _mm512_cmplt_epu32_mask(sorted, _mm512_set1_epi32(static_cast<int>(key)));
    return __builtin_popcount(static_cast<unsigned>(lt));
}

// _mm256_permutevar8x32_epi32 reads only the low three bits of each index, so
// feeding it a 0-15 index twice -- once against each source half -- yields the
// two candidate lanes, and the index's bit 3 says which to keep. That is the
// whole of the 16-lane crossing permute AVX2 does not have.
//
// The unsigned-versus-signed trap in lower_bound_avx2 applies here unchanged:
// _mm256_cmpgt_epi32 is signed, so both sides are biased by 0x80000000 or
// kEmptyKey would read as -1 and sort below every real key.
__attribute__((target("avx2")))
inline int lower_bound_perm_avx2(const Node& n, std::uint32_t key,
                                 std::uint64_t order) noexcept {
    const __m256i ones = _mm256_set1_epi32(1);
    const __m256i nibble = _mm256_set1_epi32(0xF);
    const __m256i seven = _mm256_set1_epi32(7);
    const __m256i shifts = _mm256_set_epi32(28, 24, 20, 16, 12, 8, 4, 0);
    const __m256i pad = _mm256_set1_epi32(static_cast<int>(kEmptyKey));

    const __m256i count = _mm256_set1_epi32(order_count(order));
    const __m256i live_lo = _mm256_cmpgt_epi32(count, _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
    const __m256i live_hi =
        _mm256_cmpgt_epi32(count, _mm256_setr_epi32(8, 9, 10, 11, 12, 13, 14, 15));
    const int* base = reinterpret_cast<const int*>(n.keys);
    const __m256i src_lo =
        _mm256_blendv_epi8(pad, _mm256_maskload_epi32(base, live_lo), live_lo);
    const __m256i src_hi =
        _mm256_blendv_epi8(pad, _mm256_maskload_epi32(base + 8, live_hi), live_hi);

    const __m256i word_lo = _mm256_set1_epi32(static_cast<int>(static_cast<std::uint32_t>(order)));
    const __m256i word_hi =
        _mm256_set1_epi32(static_cast<int>(static_cast<std::uint32_t>(order >> 32)));
    const __m256i idx_lo = _mm256_and_si256(
        _mm256_sub_epi32(_mm256_and_si256(_mm256_srlv_epi32(word_lo, shifts), nibble), ones),
        nibble);
    const __m256i idx_hi = _mm256_and_si256(
        _mm256_sub_epi32(_mm256_and_si256(_mm256_srlv_epi32(word_hi, shifts), nibble), ones),
        nibble);

    const __m256i sorted_lo =
        _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(src_lo, idx_lo),
                           _mm256_permutevar8x32_epi32(src_hi, idx_lo),
                           _mm256_cmpgt_epi32(idx_lo, seven));
    const __m256i sorted_hi =
        _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(src_lo, idx_hi),
                           _mm256_permutevar8x32_epi32(src_hi, idx_hi),
                           _mm256_cmpgt_epi32(idx_hi, seven));

    const __m256i bias = _mm256_set1_epi32(INT32_MIN);
    const __m256i needle = _mm256_xor_si256(_mm256_set1_epi32(static_cast<int>(key)), bias);
    const __m256i lt_lo = _mm256_cmpgt_epi32(needle, _mm256_xor_si256(sorted_lo, bias));
    const __m256i lt_hi = _mm256_cmpgt_epi32(needle, _mm256_xor_si256(sorted_hi, bias));

    const unsigned m_lo = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(lt_lo)));
    const unsigned m_hi = static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(lt_hi)));
    return __builtin_popcount(m_lo | (m_hi << 8));
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
// Same ISA decision as search_dispatch, for the ordered kernels.
inline LowerBoundFn lower_bound_dispatch() noexcept {
    switch (detect_isa()) {
#if APARAJITA_X86
        case Isa::Avx512: return &lower_bound_avx512;
        case Isa::Avx2:   return &lower_bound_avx2;
#endif
        default:          return &lower_bound_scalar_branchless;
    }
}

// Same ISA decision again, for the permuted ordered kernels.
inline LowerBoundPermFn lower_bound_perm_dispatch() noexcept {
    switch (detect_isa()) {
#if APARAJITA_X86
        case Isa::Avx512: return &lower_bound_perm_avx512;
        case Isa::Avx2:   return &lower_bound_perm_avx2;
#endif
        default:          return &lower_bound_perm_scalar;
    }
}

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
