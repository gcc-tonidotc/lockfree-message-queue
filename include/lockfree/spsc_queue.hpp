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
#include <algorithm>

namespace lockfree {

// Typical x86_64 L1 cache line size is 64 bytes.
// Aligning producer and consumer indices to separate cache lines prevents
// "false sharing", where cache coherency protocols (MESI/MOESI) invalidate
// the entire 64-byte line across CPU cores even when accessing different variables.
inline constexpr size_t CACHE_LINE_SIZE = 64;

/**
 * @brief Ultra-Low Latency Lock-Free Single-Producer Single-Consumer (SPSC) Circular Queue.
 * 
 * Key Performance Features:
 * 1. Power-of-2 capacity enables fast bitwise masking instead of slow modulo arithmetic.
 * 2. Strict cache-line separation (alignas(64)) between Producer State, Consumer State, and Buffer.
 * 3. Cached peer indices (cached_read_idx_ & cached_write_idx_) reduce cross-core MESI
 *    invalidation traffic by orders of magnitude: the peer's atomic index is only read
 *    with acquire semantics when the queue appears full or empty.
 * 4. Placement new / manual destruction in uninitialized slot storage allows non-default-constructible
 *    and move-only types (e.g. std::unique_ptr) with zero unnecessary constructor overhead.
 * 5. C++20 concepts and noexcept guarantees for maximum compiler optimization and inlining.
 *
 * @tparam T Value type stored in the queue.
 * @tparam Capacity Maximum number of elements. Must be a power of 2 (>= 2).
 */
template <typename T, size_t Capacity>
requires (Capacity >= 2 && ((Capacity & (Capacity - 1)) == 0))
class SPSCQueue {
public:
    static constexpr size_t BUFFER_CAPACITY = Capacity;
    static constexpr size_t BUFFER_MASK = Capacity - 1;

    static_assert(std::is_nothrow_destructible_v<T>, "T must have a noexcept destructor for lock-free safety");

    SPSCQueue() noexcept {
        // Monotonically increasing 64-bit indices start at 0
        write_idx_.store(0, std::memory_order_relaxed);
        read_idx_.store(0, std::memory_order_relaxed);
    }

    ~SPSCQueue() noexcept {
        size_t current_read = read_idx_.load(std::memory_order_relaxed);
        const size_t current_write = write_idx_.load(std::memory_order_relaxed);
        while (current_read != current_write) {
            const size_t slot = current_read & BUFFER_MASK;
            reinterpret_cast<T*>(buffer_[slot].data)->~T();
            ++current_read;
        }
    }

    // Non-copyable, non-movable: identity pinned in memory to protect cache alignment & thread synchronization
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;
    SPSCQueue(SPSCQueue&&) = delete;
    SPSCQueue& operator=(SPSCQueue&&) = delete;

    /**
     * @brief Producer: Push a copy of an item into the queue.
     * @return true if successfully pushed, false if queue is full.
     */
    [[nodiscard]] bool try_push(const T& item) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return try_emplace(item);
    }

    /**
     * @brief Producer: Move an item into the queue.
     * @return true if successfully pushed, false if queue is full.
     */
    [[nodiscard]] bool try_push(T&& item) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return try_emplace(std::move(item));
    }

    /**
     * @brief Producer: Emplace an item in-place in the queue slot with zero copy.
     * @return true if successfully emplaced, false if queue is full.
     */
    template <typename... Args>
    requires std::constructible_from<T, Args...>
    [[nodiscard]] bool try_emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const size_t current_write = write_idx_.load(std::memory_order_relaxed);

        // Check if queue appears full against our cached read index.
        // This avoids querying the atomic read_idx_ over the cache bus on every single push!
        if (current_write - cached_read_idx_ >= Capacity) {
            // Re-read consumer's actual read index with acquire semantics
            cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
            if (current_write - cached_read_idx_ >= Capacity) {
                return false; // Queue is genuinely full
            }
        }

        // Slot calculation using bitwise mask (single CPU cycle)
        const size_t slot = current_write & BUFFER_MASK;
        ::new (static_cast<void*>(buffer_[slot].data)) T(std::forward<Args>(args)...);

        // Release order ensures the slot payload write is visible to the consumer
        // BEFORE the updated write_idx_ becomes visible.
        write_idx_.store(current_write + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Consumer: Pop an item from the queue into the provided destination.
     * @return true if an item was popped, false if queue is empty.
     */
    [[nodiscard]] bool try_pop(T& value) noexcept(std::is_nothrow_move_assignable_v<T>) {
        const size_t current_read = read_idx_.load(std::memory_order_relaxed);

        // Check if queue appears empty against our cached write index.
        // Avoids reading the atomic write_idx_ across CPU cache lines on every pop.
        if (current_read == cached_write_idx_) {
            // Re-read producer's actual write index with acquire semantics
            cached_write_idx_ = write_idx_.load(std::memory_order_acquire);
            if (current_read == cached_write_idx_) {
                return false; // Queue is genuinely empty
            }
        }

        const size_t slot = current_read & BUFFER_MASK;
        T* slot_ptr = reinterpret_cast<T*>(buffer_[slot].data);
        value = std::move(*slot_ptr);
        slot_ptr->~T();

        // Release order ensures the consumer has finished reading the slot
        // BEFORE the updated read_idx_ becomes visible to the producer.
        read_idx_.store(current_read + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Consumer: Pop an item returning std::optional<T>.
     */
    [[nodiscard]] std::optional<T> try_pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
        const size_t current_read = read_idx_.load(std::memory_order_relaxed);

        if (current_read == cached_write_idx_) {
            cached_write_idx_ = write_idx_.load(std::memory_order_acquire);
            if (current_read == cached_write_idx_) {
                return std::nullopt;
            }
        }

        const size_t slot = current_read & BUFFER_MASK;
        T* slot_ptr = reinterpret_cast<T*>(buffer_[slot].data);
        std::optional<T> opt(std::move(*slot_ptr));
        slot_ptr->~T();

        read_idx_.store(current_read + 1, std::memory_order_release);
        return opt;
    }

    /**
     * @brief Consumer Zero-Copy Peek: Returns pointer to current front element without popping it.
     * Returns nullptr if empty.
     */
    [[nodiscard]] T* front() noexcept {
        const size_t current_read = read_idx_.load(std::memory_order_relaxed);
        if (current_read == cached_write_idx_) {
            cached_write_idx_ = write_idx_.load(std::memory_order_acquire);
            if (current_read == cached_write_idx_) {
                return nullptr;
            }
        }
        const size_t slot = current_read & BUFFER_MASK;
        return reinterpret_cast<T*>(buffer_[slot].data);
    }

    /**
     * @brief Consumer: Discard the front element (used after front() zero-copy consumption).
     */
    void pop() noexcept {
        const size_t current_read = read_idx_.load(std::memory_order_relaxed);
        const size_t slot = current_read & BUFFER_MASK;
        T* slot_ptr = reinterpret_cast<T*>(buffer_[slot].data);
        slot_ptr->~T();
        read_idx_.store(current_read + 1, std::memory_order_release);
    }

    /**
     * @brief Approximate number of elements currently in the queue.
     */
    [[nodiscard]] size_t size() const noexcept {
        const size_t wr = write_idx_.load(std::memory_order_relaxed);
        const size_t rd = read_idx_.load(std::memory_order_relaxed);
        return (wr >= rd) ? (wr - rd) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return read_idx_.load(std::memory_order_relaxed) == write_idx_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept {
        return Capacity;
    }

private:
    // Slot uninitialized byte storage with proper alignment for type T
    struct Slot {
        alignas(alignof(T)) std::byte data[sizeof(T)];
    };

    // -------------------------------------------------------------
    // CACHE-LINE LAYOUT TO ELIMINATE FALSE SHARING
    // -------------------------------------------------------------
    
    // --- Cache Line 1: Producer State ---
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> write_idx_{0};
    size_t cached_read_idx_{0}; // Producer's private copy of read_idx_
    // Padding to ensure Consumer State starts on a fresh 64-byte cache line
    char pad_producer_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - sizeof(size_t)];

    // --- Cache Line 2: Consumer State ---
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> read_idx_{0};
    size_t cached_write_idx_{0}; // Consumer's private copy of write_idx_
    // Padding to ensure Buffer starts on its own cache line
    char pad_consumer_[CACHE_LINE_SIZE - sizeof(std::atomic<size_t>) - sizeof(size_t)];

    // --- Cache Line 3+: Buffer Storage ---
    alignas(CACHE_LINE_SIZE) std::array<Slot, Capacity> buffer_;
};

} // namespace lockfree
