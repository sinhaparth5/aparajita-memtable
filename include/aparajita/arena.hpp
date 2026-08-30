// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

// A stand-in for the rocksdb::Allocator the rep is handed in Phase 3.
//
// It exists now so the Phase 2 structure is written against arena semantics from
// the start rather than being retrofitted. Two of those semantics shape the code
// above it. Memory is bump-allocated and never individually freed, so a node
// published by a split costs an allocation and nothing else -- and, since nothing
// is reclaimed, so did every insert until Phase 4b stopped rebuilding nodes on
// the way past. And an arena cannot
// reallocate, so growable storage has to be segmented into chunks rather than
// resized, which is why this hands out fresh chunks instead of growing one.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace aparajita {

class Arena {
public:
    static constexpr std::size_t kChunkSize = 64 * 1024;
    static constexpr std::size_t kAlign = 64;

    Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Alignment is per call, not global. Nodes must be 64-byte aligned so their
    // compared bytes cannot straddle a cache line, but rounding *every*
    // allocation up to 64 bytes charges a 13-byte key copy 64 bytes, and key
    // copies are the most frequent allocation there is. Measured on the 64-thread
    // insert test, that single mistake accounted for roughly a tenth of the
    // arena.
    char* allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t)) {
        const std::size_t need = (bytes + align - 1) & ~(align - 1);
        std::lock_guard<std::mutex> g(mu_);
        const std::size_t pad = (align - (offset_ & (align - 1))) & (align - 1);
        if (offset_ + pad + need > limit_) {
            new_chunk(need > kChunkSize ? need : kChunkSize);
        } else {
            offset_ += pad;
        }
        char* p = cur_ + offset_;
        offset_ += need;
        used_ += need;
        return p;
    }

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        static_assert(alignof(T) <= kAlign, "type wants stronger alignment than the arena provides");
        return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    // One mutex for all allocation, which is a known bottleneck rather than an
    // oversight: RocksDB's ConcurrentArena hands each writer a per-thread block
    // precisely to avoid this. Phase 3 inherits that allocator and the problem
    // goes away with it. Until then, concurrent insert throughput measured here
    // is bounded by this lock and should not be quoted.

    // What RocksDB would charge against write_buffer_size.
    std::size_t memory_usage() const {
        std::lock_guard<std::mutex> g(mu_);
        return used_;
    }

private:
    // Paired with the aligned ::operator new below. A plain unique_ptr<char[]>
    // would call delete[] on memory that came from the aligned overload, which is
    // undefined rather than merely untidy.
    struct AlignedDelete {
        void operator()(char* p) const noexcept {
            ::operator delete(p, std::align_val_t{kAlign});
        }
    };
    using Chunk = std::unique_ptr<char[], AlignedDelete>;

    void new_chunk(std::size_t size) {
        chunks_.push_back(Chunk(
            static_cast<char*>(::operator new(size, std::align_val_t{kAlign}))));
        cur_ = chunks_.back().get();
        offset_ = 0;
        limit_ = size;
    }

    mutable std::mutex mu_;
    std::vector<Chunk> chunks_;
    char* cur_{nullptr};
    std::size_t offset_{0};
    std::size_t limit_{0};
    std::size_t used_{0};
};

} // namespace aparajita
