#include <algorithm>
#include <lscq/ebr.hpp>

// @deprecated LSCQ now uses ObjectPool-based node recycling instead of EBR. This implementation is
// retained for comparison and potential rollback.

namespace lscq {

thread_local EBRManager::ThreadState* EBRManager::tls_state_ = nullptr;
thread_local bool EBRManager::tls_initialized_ = false;

EBRManager::EBRManager() { global_epoch_.store(0, std::memory_order_relaxed); }

EBRManager::~EBRManager() {
    // Reclaim all pending retired nodes
    for (std::size_t i = 0; i < kNumGenerations; ++i) {
        for (auto& node : retired_[i]) {
            if (node.ptr != nullptr && node.deleter) {
                node.deleter(node.ptr);
            }
        }
        retired_[i].clear();
    }

    // Clean up thread states
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (auto* state : thread_states_) {
        delete state;
    }
    thread_states_.clear();

    // Reset thread-local state to prevent use-after-free
    // This is safe because we're in the destructor and no other threads
    // should be using this EBRManager anymore
    tls_state_ = nullptr;
    tls_initialized_ = false;
}

EBRManager::ThreadState* EBRManager::get_or_create_state() {
    // Check if existing state is valid (belongs to this manager)
    if (tls_initialized_ && tls_state_ != nullptr && tls_state_->owner == this) {
        return tls_state_;
    }

    // Need to create new state for this manager
    tls_state_ = new ThreadState();
    tls_state_->owner = this;
    tls_initialized_ = true;
    register_thread(tls_state_);
    return tls_state_;
}

void EBRManager::register_thread(ThreadState* state) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    thread_states_.push_back(state);
}

void EBRManager::enter_critical() {
    ThreadState* state = get_or_create_state();
    state->epoch = global_epoch_.load(std::memory_order_acquire);
    state->active = true;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void EBRManager::exit_critical() {
    ThreadState* state = get_or_create_state();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    state->active = false;
}

void EBRManager::retire(void* ptr, std::function<void(void*)> deleter) {
    if (ptr == nullptr) {
        return;
    }

    const std::uint64_t epoch = global_epoch_.load(std::memory_order_acquire);
    const std::size_t gen_idx = epoch % kNumGenerations;

    RetiredNode node;
    node.ptr = ptr;
    node.deleter = std::move(deleter);
    node.epoch = epoch;

    {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        retired_[gen_idx].push_back(std::move(node));
    }
}

bool EBRManager::can_advance_epoch() const {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    const std::uint64_t current = global_epoch_.load(std::memory_order_acquire);

    for (const auto* state : thread_states_) {
        if (state->active && state->epoch < current) {
            return false;
        }
    }
    return true;
}

std::uint64_t EBRManager::min_active_epoch() const {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    const std::uint64_t current = global_epoch_.load(std::memory_order_acquire);
    std::uint64_t min_epoch = current;

    for (const auto* state : thread_states_) {
        if (state->active && state->epoch < min_epoch) {
            min_epoch = state->epoch;
        }
    }
    return min_epoch;
}

void EBRManager::reclaim_generation(std::size_t gen_idx) {
    std::vector<RetiredNode> to_delete;

    {
        std::lock_guard<std::mutex> lock(retired_mutex_);
        std::swap(to_delete, retired_[gen_idx]);
    }

    for (auto& node : to_delete) {
        if (node.ptr != nullptr && node.deleter) {
            node.deleter(node.ptr);
        }
    }
}

std::size_t EBRManager::try_reclaim() {
    std::size_t reclaimed = 0;

    // Try to advance epoch if all threads have caught up
    if (can_advance_epoch()) {
        global_epoch_.fetch_add(1, std::memory_order_release);
    }

    const std::uint64_t current_epoch = global_epoch_.load(std::memory_order_acquire);

    // Reclaim nodes from generations that are safe to delete
    // A node is safe to delete if its epoch < current_epoch - 2
    // With 3 generations, this means we can reclaim the oldest generation
    // when we have advanced at least 2 epochs since the node was retired
    if (current_epoch >= 2) {
        const std::uint64_t safe_epoch = current_epoch - 2;

        for (std::size_t i = 0; i < kNumGenerations; ++i) {
            std::vector<RetiredNode> remaining;
            std::vector<RetiredNode> to_delete;

            {
                std::lock_guard<std::mutex> lock(retired_mutex_);
                for (auto& node : retired_[i]) {
                    if (node.epoch <= safe_epoch) {
                        to_delete.push_back(std::move(node));
                    } else {
                        remaining.push_back(std::move(node));
                    }
                }
                retired_[i] = std::move(remaining);
            }

            for (auto& node : to_delete) {
                if (node.ptr != nullptr && node.deleter) {
                    node.deleter(node.ptr);
                    ++reclaimed;
                }
            }
        }
    }

    return reclaimed;
}

std::uint64_t EBRManager::current_epoch() const noexcept {
    return global_epoch_.load(std::memory_order_acquire);
}

bool EBRManager::has_pending() const noexcept {
    std::lock_guard<std::mutex> lock(retired_mutex_);
    for (std::size_t i = 0; i < kNumGenerations; ++i) {
        if (!retired_[i].empty()) {
            return true;
        }
    }
    return false;
}

std::size_t EBRManager::pending_count() const noexcept {
    std::lock_guard<std::mutex> lock(retired_mutex_);
    std::size_t count = 0;
    for (std::size_t i = 0; i < kNumGenerations; ++i) {
        count += retired_[i].size();
    }
    return count;
}

}  // namespace lscq
