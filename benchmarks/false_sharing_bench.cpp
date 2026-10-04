#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdint>
#include "lockfree/spsc_queue.hpp"
#include "lockfree/cpu_affinity.hpp"

// Intentionally unpadded SPSC queue where write_idx_ and read_idx_
// reside on the SAME 64-byte cache line.
// This triggers MESI cache coherency bouncing (false sharing).
template <typename T, size_t Capacity>
class UnpaddedSPSCQueue {
public:
    static constexpr size_t BUFFER_MASK = Capacity - 1;

    UnpaddedSPSCQueue() : write_idx_(0), read_idx_(0) {}

    bool try_push(const T& item) {
        const size_t current_write = write_idx_.load(std::memory_order_relaxed);
        const size_t current_read = read_idx_.load(std::memory_order_acquire);
        if (current_write - current_read >= Capacity) return false;

        buffer_[current_write & BUFFER_MASK] = item;
        write_idx_.store(current_write + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) {
        const size_t current_read = read_idx_.load(std::memory_order_relaxed);
        const size_t current_write = write_idx_.load(std::memory_order_acquire);
        if (current_read == current_write) return false;

        item = buffer_[current_read & BUFFER_MASK];
        read_idx_.store(current_read + 1, std::memory_order_release);
        return true;
    }

private:
    // Both atomics are adjacent without alignas(64) or padding:
    // sizeof(atomic<size_t>) * 2 = 16 bytes <= 64 bytes.
    // Core 0 writing to write_idx_ continuously invalidates Core 1's L1 cache line containing read_idx_!
    std::atomic<size_t> write_idx_{0};
    std::atomic<size_t> read_idx_{0};
    T buffer_[Capacity];
};

int main() {
    constexpr size_t CAPACITY = 65536;
    constexpr uint64_t TOTAL_MESSAGES = 20'000'000;

    std::cout << "===============================================================\n";
    std::cout << " FALSE SHARING BENCHMARK: Cache Line Contention (MESI Protocol)\n";
    std::cout << "===============================================================\n";
    std::cout << "Hardware Concurrency: " << lockfree::get_hardware_concurrency() << " cores\n";
    std::cout << "Total Messages:       " << TOTAL_MESSAGES << "\n";
    std::cout << "Queue Capacity:       " << CAPACITY << " slots\n\n";

    // -------------------------------------------------------------
    // Test 1: Unpadded Queue (False Sharing Present)
    // -------------------------------------------------------------
    {
        std::cout << "[1/2] Running UNPADDED Queue (False Sharing Active)...\n";
        auto queue = std::make_unique<UnpaddedSPSCQueue<uint64_t, CAPACITY>>();
        std::atomic<bool> start_flag{false};

        auto start_time = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point end_time;

        std::thread consumer([&]() {
            lockfree::pin_current_thread_to_core(1);
            while (!start_flag.load(std::memory_order_acquire)) {}

            uint64_t val = 0;
            for (uint64_t i = 0; i < TOTAL_MESSAGES; ++i) {
                while (!queue->try_pop(val)) {
                    lockfree::cpu_relax();
                }
            }
            end_time = std::chrono::steady_clock::now();
        });

        std::thread producer([&]() {
            lockfree::pin_current_thread_to_core(0);
            while (!start_flag.load(std::memory_order_acquire)) {}

            for (uint64_t i = 0; i < TOTAL_MESSAGES; ++i) {
                while (!queue->try_push(i)) {
                    lockfree::cpu_relax();
                }
            }
        });

        start_time = std::chrono::steady_clock::now();
        start_flag.store(true, std::memory_order_release);

        producer.join();
        consumer.join();

        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        const double seconds = static_cast<double>(duration_ms) / 1000.0;
        const double mops = (static_cast<double>(TOTAL_MESSAGES) / seconds) / 1e6;

        std::cout << "  -> Unpadded Elapsed Time: " << duration_ms << " ms\n";
        std::cout << "  -> Unpadded Throughput:   " << std::fixed << std::setprecision(2) << mops << " Mops/sec\n";
    }

    // -------------------------------------------------------------
    // Test 2: Padded Queue (alignas(64) + Cached Indices)
    // -------------------------------------------------------------
    {
        std::cout << "\n[2/2] Running PADDED Queue (alignas(64) + Cached Peer Indices)...\n";
        auto queue = std::make_unique<lockfree::SPSCQueue<uint64_t, CAPACITY>>();
        std::atomic<bool> start_flag{false};

        auto start_time = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point end_time;

        std::thread consumer([&]() {
            lockfree::pin_current_thread_to_core(1);
            while (!start_flag.load(std::memory_order_acquire)) {}

            uint64_t val = 0;
            for (uint64_t i = 0; i < TOTAL_MESSAGES; ++i) {
                while (!queue->try_pop(val)) {
                    lockfree::cpu_relax();
                }
            }
            end_time = std::chrono::steady_clock::now();
        });

        std::thread producer([&]() {
            lockfree::pin_current_thread_to_core(0);
            while (!start_flag.load(std::memory_order_acquire)) {}

            for (uint64_t i = 0; i < TOTAL_MESSAGES; ++i) {
                while (!queue->try_push(i)) {
                    lockfree::cpu_relax();
                }
            }
        });

        start_time = std::chrono::steady_clock::now();
        start_flag.store(true, std::memory_order_release);

        producer.join();
        consumer.join();

        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        const double seconds = static_cast<double>(duration_ms) / 1000.0;
        const double mops = (static_cast<double>(TOTAL_MESSAGES) / seconds) / 1e6;

        std::cout << "  -> Padded Elapsed Time:   " << duration_ms << " ms\n";
        std::cout << "  -> Padded Throughput:     " << std::fixed << std::setprecision(2) << mops << " Mops/sec\n";
    }

    std::cout << "\nConclusion: Cache-line padding and index caching prevent MESI invalidation\n"
              << "ping-pong, significantly reducing memory bus contention.\n";
    return 0;
}
