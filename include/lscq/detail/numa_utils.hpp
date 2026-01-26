#pragma once

#include <cstddef>
#include <new>

#if defined(__linux__) && defined(__has_include)
#if __has_include(<numa.h>)
#define LSCQ_HAS_NUMA 1
#include <numa.h>
#include <sched.h>
#else
#define LSCQ_HAS_NUMA 0
#endif
#else
#define LSCQ_HAS_NUMA 0
#endif

namespace lscq::detail::numa {

struct Allocation {
    void* ptr{nullptr};
    bool on_numa{false};
};

inline bool Available() noexcept {
#if LSCQ_HAS_NUMA
    return numa_available() != -1;
#else
    return false;
#endif
}

inline int CurrentNode() noexcept {
#if LSCQ_HAS_NUMA
    if (!Available()) {
        return -1;
    }
    int cpu = sched_getcpu();
    if (cpu < 0) {
        cpu = 0;
    }
    return numa_node_of_cpu(cpu);
#else
    return -1;
#endif
}

inline Allocation Allocate(std::size_t bytes) {
    if (bytes == 0) {
        return {};
    }
#if LSCQ_HAS_NUMA
    if (Available()) {
        const int node = CurrentNode();
        if (node >= 0) {
            void* ptr = numa_alloc_onnode(bytes, node);
            if (ptr != nullptr) {
                return {ptr, true};
            }
        }
    }
#endif
    void* ptr = ::operator new(bytes);
    return {ptr, false};
}

inline void Free(const Allocation& allocation, std::size_t bytes) noexcept {
    if (allocation.ptr == nullptr) {
        return;
    }
#if !LSCQ_HAS_NUMA
    (void)bytes;
#endif
#if LSCQ_HAS_NUMA
    if (allocation.on_numa) {
        numa_free(allocation.ptr, bytes);
        return;
    }
#endif
    ::operator delete(allocation.ptr);
}

}  // namespace lscq::detail::numa
