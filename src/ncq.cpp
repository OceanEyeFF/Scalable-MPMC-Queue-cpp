#include <lscq/detail/ncq_impl.hpp>
#include <lscq/ncq.hpp>

#include <cstddef>
#include <cstdint>
#include <new>

namespace lscq {

namespace {

constexpr std::size_t CACHE_LINE_SIZE = 64;

constexpr std::size_t round_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + (alignment - 1)) / alignment * alignment;
}

}  // namespace

template <class T>
NCQ<T>::NCQ(std::size_t capacity) : entries_(nullptr), capacity_(capacity), head_(0), tail_(0) {
    static_assert(sizeof(Entry) == 16);
    static_assert(alignof(Entry) == 16);

    constexpr std::size_t entries_per_line = CACHE_LINE_SIZE / sizeof(Entry);  // 4

    if (capacity_ == 0) {
        capacity_ = 1;
    }
    if (capacity_ < entries_per_line) {
        capacity_ = entries_per_line;
    }
    capacity_ = round_up(capacity_, entries_per_line);

    Entry* raw = static_cast<Entry*>(
        ::operator new[](capacity_ * sizeof(Entry), std::align_val_t(CACHE_LINE_SIZE)));
    entries_.reset(raw);

    for (std::size_t i = 0; i < capacity_; ++i) {
        new (&entries_[i]) Entry{0, 0};
    }

    // Figure 5 initialization: Head = Tail = n (cycle 1) while all entries start with cycle 0.
    head_.store(static_cast<std::uint64_t>(capacity_), std::memory_order_relaxed);
    tail_.store(static_cast<std::uint64_t>(capacity_), std::memory_order_relaxed);
}

template <class T>
NCQ<T>::~NCQ() = default;

template <class T>
std::size_t NCQ<T>::cache_remap(std::size_t idx) const noexcept {
    // entries_per_line is 4 (64B line / 16B Entry). Use bit ops on the hot path.
    constexpr std::size_t entries_per_line = CACHE_LINE_SIZE / sizeof(Entry);  // 4
    static_assert(entries_per_line == 4, "Entry size must be 16B for cache_remap bit-ops");

    const std::size_t line = idx >> 2u;
    const std::size_t offset = idx & 3u;
    const std::size_t num_lines = capacity_ >> 2u;
    return offset * num_lines + line;
}

template <class T>
bool NCQ<T>::enqueue(T index) {
    if (index == kEmpty) {
        return false;
    }

    const std::uint64_t n = static_cast<std::uint64_t>(capacity_);
    while (true) {
        std::uint64_t t = tail_.load(std::memory_order_acquire);
        const std::uint64_t cycle_t = t / n;
        const std::size_t j = static_cast<std::size_t>(t % n);
        const std::size_t remapped_j = cache_remap(j);

        const Entry ent = detail::entry_load(&entries_[remapped_j]);
        const std::uint64_t cycle_e = ent.cycle_flags;

        if (cycle_e == cycle_t) {
            // Help to move tail.
            (void)tail_.compare_exchange_weak(t, t + 1, std::memory_order_release,
                                             std::memory_order_relaxed);
            continue;
        }

        if (cycle_e + 1 != cycle_t) {
            // Tail is already stale.
            continue;
        }

        Entry expected = ent;
        const Entry desired{cycle_t, static_cast<std::uint64_t>(index)};
        if (lscq::cas2(&entries_[remapped_j], expected, desired)) {
            // Try to move tail.
            (void)tail_.compare_exchange_weak(t, t + 1, std::memory_order_release,
                                             std::memory_order_relaxed);
            return true;
        }
    }
}

template <class T>
T NCQ<T>::dequeue() {
    const std::uint64_t n = static_cast<std::uint64_t>(capacity_);
    while (true) {
        std::uint64_t h = head_.load(std::memory_order_acquire);
        const std::uint64_t cycle_h = h / n;
        const std::size_t j = static_cast<std::size_t>(h % n);
        const std::size_t remapped_j = cache_remap(j);

        const Entry ent = detail::entry_load(&entries_[remapped_j]);
        const std::uint64_t cycle_e = ent.cycle_flags;

        if (cycle_e != cycle_h) {
            if (cycle_e + 1 == cycle_h) {
                return kEmpty;  // Empty queue.
            }
            continue;  // Head is already stale.
        }

        if (head_.compare_exchange_weak(h, h + 1, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
            return static_cast<T>(ent.index_or_ptr);
        }
    }
}

template <class T>
bool NCQ<T>::is_empty() const noexcept {
    return head_.load(std::memory_order_relaxed) >= tail_.load(std::memory_order_relaxed);
}

template std::size_t NCQ<std::uint64_t>::cache_remap(std::size_t) const noexcept;
template std::size_t NCQ<std::uint32_t>::cache_remap(std::size_t) const noexcept;

template class lscq::NCQ<std::uint64_t>;
template class lscq::NCQ<std::uint32_t>;

}  // namespace lscq
