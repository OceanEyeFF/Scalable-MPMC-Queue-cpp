#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace lscq::detail {

template <class T>
struct ObjectPoolShard {
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<T>> objects;
    std::atomic<std::size_t> approx_size{0};
};

}  // namespace lscq::detail

