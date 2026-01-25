/**
 * @file ebr.hpp
 * @brief Epoch-Based Reclamation (EBR) memory reclamation utilities.
 * @author lscq contributors
 * @version 0.1.0
 *
 * Epoch-Based Reclamation is used to safely reclaim nodes removed from lock-free data structures.
 * Threads enter/exit critical sections to announce liveness; reclamation happens once all active
 * threads have advanced beyond the retirement epoch.
 *
 * @deprecated LSCQ now uses ObjectPool-based node recycling instead of EBR. This implementation is
 * retained for comparison and potential rollback.
 */

#ifndef LSCQ_EBR_HPP_
#define LSCQ_EBR_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lscq {

/**
 * @class EBRManager
 * @brief Epoch-Based Reclamation (EBR) manager for safe memory reclamation.
 *
 * @deprecated Use ObjectPool-based node recycling instead.
 *
 * EBR is a memory reclamation technique that allows lock-free data structures
 * to safely reclaim memory. It works by tracking epochs (global time periods)
 * and only reclaiming memory when all threads have moved past the epoch in
 * which the memory was retired.
 *
 * This implementation uses a 3-generation retired list strategy:
 * - Nodes retired in epoch E are stored in retired_[E % 3]
 * - Nodes can be safely reclaimed when their epoch < global_epoch - 2
 *
 * Thread-safety: All public methods are thread-safe. Each thread that accesses an EBR-protected
 * data structure should bracket the access with @ref enter_critical and @ref exit_critical (or use
 * @ref EpochGuard).
 *
 * Complexity:
 * - @ref enter_critical / @ref exit_critical - O(1)
 * - @ref retire - amortized O(1) (may allocate)
 * - @ref try_reclaim - O(R) where R is the number of retired nodes examined/reclaimed
 *
 * Example:
 * @code
 * lscq::EBRManager ebr;
 *
 * // Enter/exit manually:
 * ebr.enter_critical();
 * // ... access EBR-protected structure ...
 * ebr.exit_critical();
 *
 * // Or use RAII:
 * {
 *     lscq::EpochGuard g(ebr);
 *     // ... access EBR-protected structure ...
 * }
 * @endcode
 */
class EBRManager {
   public:
    /**
     * @brief Construct an EBR manager
     */
    EBRManager();

    /**
     * @brief Destroy the EBR manager, reclaiming all remaining nodes
     */
    ~EBRManager();

    EBRManager(const EBRManager&) = delete;
    EBRManager& operator=(const EBRManager&) = delete;
    EBRManager(EBRManager&&) = delete;
    EBRManager& operator=(EBRManager&&) = delete;

    /**
     * @brief Enter a critical section (epoch guard)
     *
     * Must be called before accessing any shared data structure protected by EBR.
     * Pairs with exit_critical().
     *
     * @note Thread-local state is maintained internally.
     *
     * @throws std::bad_alloc If thread-local registration allocates and fails.
     */
    void enter_critical();

    /**
     * @brief Exit a critical section (epoch guard)
     *
     * Must be called after finishing access to shared data structures.
     * Pairs with enter_critical().
     */
    void exit_critical();

    /**
     * @brief Retire a node for later reclamation
     *
     * The node will be reclaimed after all threads have moved past the current epoch.
     *
     * @param ptr Pointer to retire (will be deleted when safe)
     * @param deleter Custom deleter function to call instead of delete
     *
     * @throws std::bad_alloc If internal bookkeeping storage grows and allocation fails.
     */
    void retire(void* ptr, std::function<void(void*)> deleter);

    /**
     * @brief Retire a node using default delete
     *
     * @tparam T Type of the pointer
     * @param ptr Pointer to retire
     *
     * @throws std::bad_alloc If internal bookkeeping storage grows and allocation fails.
     */
    template <typename T>
    void retire(T* ptr) {
        retire(static_cast<void*>(ptr), [](void* p) { delete static_cast<T*>(p); });
    }

    /**
     * @brief Try to reclaim retired nodes
     *
     * Attempts to reclaim nodes that are safe to delete (epoch < global_epoch - 2).
     * This may advance the global epoch if conditions are met.
     *
     * @return Number of nodes reclaimed
     */
    std::size_t try_reclaim();

    /**
     * @brief Get the current global epoch
     *
     * @return Current epoch value
     */
    std::uint64_t current_epoch() const noexcept;

    /**
     * @brief Check if there are any pending retired nodes
     *
     * @return true if there are nodes waiting to be reclaimed
     */
    bool has_pending() const noexcept;

    /**
     * @brief Get the number of pending retired nodes
     *
     * @return Count of nodes waiting to be reclaimed
     */
    std::size_t pending_count() const noexcept;

   private:
    struct RetiredNode {
        void* ptr;
        std::function<void(void*)> deleter;
        std::uint64_t epoch;
    };

    struct ThreadState {
        std::uint64_t epoch{0};
        bool active{false};
        EBRManager* owner{nullptr};  // Pointer to owning manager for validation
    };

    static constexpr std::size_t kNumGenerations = 3;

    alignas(64) std::atomic<std::uint64_t> global_epoch_{0};

    mutable std::mutex retired_mutex_;
    std::vector<RetiredNode> retired_[kNumGenerations];

    mutable std::mutex threads_mutex_;
    std::vector<ThreadState*> thread_states_;

    static thread_local ThreadState* tls_state_;
    static thread_local bool tls_initialized_;

    ThreadState* get_or_create_state();
    void register_thread(ThreadState* state);
    bool can_advance_epoch() const;
    void reclaim_generation(std::size_t gen_idx);
    std::uint64_t min_active_epoch() const;
};

/**
 * @class EpochGuard
 * @brief RAII guard for EBR critical sections.
 *
 * @deprecated Use ObjectPool-based node recycling instead.
 *
 * Automatically calls enter_critical() on construction and exit_critical() on destruction.
 *
 * Usage:
 * @code
 * EBRManager& ebr = ...;
 * {
 *     EpochGuard guard(ebr);
 *     // Access shared data structure safely
 * } // exit_critical() called automatically
 * @endcode
 */
class EpochGuard {
   public:
    /**
     * @brief Construct an EpochGuard and enter critical section
     *
     * @param ebr Reference to the EBR manager
     */
    explicit EpochGuard(EBRManager& ebr) noexcept : ebr_(ebr) { ebr_.enter_critical(); }

    /**
     * @brief Destroy the EpochGuard and exit critical section
     */
    ~EpochGuard() noexcept { ebr_.exit_critical(); }

    EpochGuard(const EpochGuard&) = delete;
    EpochGuard& operator=(const EpochGuard&) = delete;
    EpochGuard(EpochGuard&&) = delete;
    EpochGuard& operator=(EpochGuard&&) = delete;

   private:
    EBRManager& ebr_;
};

}  // namespace lscq

#endif  // LSCQ_EBR_HPP_
