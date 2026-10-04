#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <atomic>
#include <optional>
#include <array>
#include <utility>
#include <concepts>

namespace lockfree {

/**
 * @brief Ultra-High Performance Multi-Producer Multi-Consumer (MPMC) Bounded Lock-Free Queue.
 *
 * Based on Dmitry Vyukov's algorithm.
 * Features:
 * 1. Bounded ring buffer with power-of-2 capacity.
 * 2. Wait-free / Lock-free atomic sequence numbers per cell guaranteeing FIFO slot claiming.
 * 3. Cache-line separation (alignas(64)) between enqueue and dequeue cursors to prevent false sharing.
 * 4. Fine-grained memory orderings:
 *    - CAS on positions uses relaxed ordering.
 *    - Cell sequence loads with acquire to synchronize data visibility.
 *    - Cell sequence stores with release to signal data availability / slot vacancy.
 *
 * @tparam T Type of elements stored in the queue.
 * @tparam Capacity Maximum capacity. Must be a power of 2 (>= 2).
 */
template <typename T, size_t Capacity>
requires (Capacity >= 2 && ((Capacity & (Capacity - 1)) == 0))
class MPMCQueue {
public:
    static constexpr size_t BUFFER_CAPACITY = Capacity;
    static constexpr size_t BUFFER_MASK = Capacity - 1;

    static_assert(std::is_nothrow_destructible_v<T>, "T must have a noexcept destructor for lock-free safety");

    MPMCQueue() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    ~MPMCQueue() noexcept {
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = buffer_[pos & BUFFER_MASK];
            const size_t seq = cell.sequence.load(std::memory_order_relaxed);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                reinterpret_cast<T*>(cell.storage)->~T();
                ++pos;
            } else {
                break;
            }
        }
    }

    // Non-copyable, non-movable
    MPMCQueue(const MPMCQueue&) = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&) = delete;
    MPMCQueue& operator=(MPMCQueue&&) = delete;

    /**
     * @brief Multi-Producer: Try to push an item (copy) into the queue.
     * @return true if successful, false if full.
     */
    [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(item);
    }

    /**
     * @brief Multi-Producer: Try to push an item (move) into the queue.
     * @return true if successful, false if full.
     */
    [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return try_emplace(std::move(item));
    }

    /**
     * @brief Multi-Producer: Emplace in-place with zero copying.
     * @return true if successful, false if full.
     */
    template <typename... Args>
    requires std::constructible_from<T, Args...>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        Cell* cell = nullptr;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & BUFFER_MASK];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0) {
                // Slot is empty and matches our pos. Attempt to advance enqueue_pos_.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    // Won the race for this slot
                    break;
                }
            } else if (diff < 0) {
                // The slot sequence is behind pos, meaning the queue is full.
                return false;
            } else {
                // Another thread moved enqueue_pos_ ahead; re-read current pos.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        // Construct object in claimed cell
        ::new (static_cast<void*>(cell->storage)) T(std::forward<Args>(args)...);

        // Store sequence = pos + 1 with release ordering so consumers see the constructed payload
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Multi-Consumer: Try to pop an item into the provided reference.
     * @return true if successful, false if empty.
     */
    [[nodiscard]] bool try_pop(T& value) noexcept(std::is_nothrow_move_assignable_v<T>) {
        Cell* cell = nullptr;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

        for (;;) {
            cell = &buffer_[pos & BUFFER_MASK];
            const size_t seq = cell->sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0) {
                // Slot has data and matches our pos + 1. Attempt to claim it.
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    // Won the race to read this slot
                    break;
                }
            } else if (diff < 0) {
                // The slot sequence has not yet been incremented by a producer, queue is empty.
                return false;
            } else {
                // Another consumer claimed pos; re-read pos.
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T* slot_ptr = reinterpret_cast<T*>(cell->storage);
        value = std::move(*slot_ptr);
        slot_ptr->~T();

        // Release slot back to producers: sequence = pos + BUFFER_MASK + 1
        cell->sequence.store(pos + BUFFER_MASK + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Multi-Consumer: Try to pop returning std::optional<T>.
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        T val;
        if (try_pop(val)) {
            return std::optional<T>(std::move(val));
        }
        return std::nullopt;
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
        const size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
        return (enq >= deq) ? (enq - deq) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return dequeue_pos_.load(std::memory_order_relaxed) >= enqueue_pos_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept {
        return Capacity;
    }

private:
    struct Cell {
        std::atomic<size_t> sequence;
        alignas(alignof(T)) std::byte storage[sizeof(T)];
    };

    static constexpr size_t CACHE_LINE_SIZE = 64;

    // --- Enqueue Position Cache Line ---
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_{0};
    char pad_enq_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];

    // --- Dequeue Position Cache Line ---
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_{0};
    char pad_deq_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];

    // --- Buffer Storage ---
    alignas(CACHE_LINE_SIZE) std::array<Cell, Capacity> buffer_;
};

} // namespace lockfree
