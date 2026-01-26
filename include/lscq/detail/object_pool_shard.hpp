#pragma once

#include <algorithm>
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

    using pointer = T*;

    std::size_t GetBatch(pointer* out, std::size_t max_count) {
        if (out == nullptr || max_count == 0) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(mutex);
        const std::size_t available = objects.size();
        const std::size_t count = std::min(max_count, available);
        for (std::size_t i = 0; i < count; ++i) {
            std::unique_ptr<T> obj = std::move(objects.back());
            objects.pop_back();
            out[i] = obj.release();
        }
        if (count > 0) {
            approx_size.fetch_sub(count, std::memory_order_relaxed);
        }
        return count;
    }

    std::size_t PutBatch(pointer* items, std::size_t count) {
        if (items == nullptr || count == 0) {
            return 0;
        }
        std::size_t stored = 0;
        std::lock_guard<std::mutex> lock(mutex);
        objects.reserve(objects.size() + count);
        for (std::size_t i = 0; i < count; ++i) {
            pointer obj = items[i];
            if (obj == nullptr) {
                continue;
            }
            objects.emplace_back(obj);
            ++stored;
        }
        if (stored > 0) {
            approx_size.fetch_add(stored, std::memory_order_relaxed);
        }
        return stored;
    }

    std::size_t TryStealBatch(pointer* out, std::size_t max_count) {
        if (out == nullptr || max_count == 0) {
            return 0;
        }
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || objects.empty()) {
            return 0;
        }
        const std::size_t available = objects.size();
        const std::size_t count = std::min(max_count, available);
        for (std::size_t i = 0; i < count; ++i) {
            std::unique_ptr<T> obj = std::move(objects.back());
            objects.pop_back();
            out[i] = obj.release();
        }
        if (count > 0) {
            approx_size.fetch_sub(count, std::memory_order_relaxed);
        }
        return count;
    }
};

}  // namespace lscq::detail
