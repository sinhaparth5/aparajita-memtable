// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

namespace aparajita {

// One cache line holds sixteen 32-bit key surrogates. The width is deliberate:
// AVX2 covers it in two 256-bit compares, AVX-512 in one 512-bit compare, and a
// node never straddles two cache lines, so a probe costs exactly one line fill.
inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kNodeKeys = 16;

struct alignas(kCacheLine) Node {
    std::uint32_t keys[kNodeKeys];
};

static_assert(sizeof(Node) == kCacheLine, "node must occupy exactly one cache line");
static_assert(alignof(Node) == kCacheLine, "node must be cache-line aligned");

// Separation required between independently written variables to avoid false
// sharing. Defined as a literal rather than forwarded from
// std::hardware_destructive_interference_size on purpose: GCC warns that the
// standard constant varies with -mtune and compiler version, which would make
// it an unstable value baked into a public header that RocksDB compiles
// against. A static_assert below keeps us honest if the platform disagrees.
//
// The architecture document argues for 128-byte separation on Intel parts with
// adjacent-line prefetch. That applies to the concurrently written state in
// Phase 2, not to these read-mostly key arrays, so it is recorded here and
// applied there.
inline constexpr std::size_t kDestructiveInterference = 64;

#ifdef __cpp_lib_hardware_interference_size
// The warning this suppresses is the very reason kDestructiveInterference is a
// literal above. Reading the standard constant inside a static_assert couples
// nothing to the ABI, so it is safe here and nowhere else in this header.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
static_assert(std::hardware_destructive_interference_size >= kDestructiveInterference,
              "platform wants wider separation than 64 bytes; revisit kDestructiveInterference");
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

// Sentinel for "no key at this slot". The value is 0xFFFFFFFF rather than 0 so
// that it sorts above every real key: a partially filled node keeps its tail
// slots sentinel-valued and remains sorted, which any ordered search over the
// node depends on. Real keys must therefore never take this value.
inline constexpr std::uint32_t kEmptyKey = 0xFFFFFFFFu;

} // namespace aparajita
