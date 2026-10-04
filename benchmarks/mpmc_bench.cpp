#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <numeric>
#include "lockfree/mpmc_queue.hpp"
#include "lockfree/cpu_affinity.hpp"

int main() {
    constexpr size_t CAPACITY = 65536;
    constexpr size_t NUM_PRODUCERS = 2;
    constexpr size_t NUM_CONSUMERS = 2;
    constexpr uint64_t MSGS_PER_PRODUCER = 5'000'000;
    constexpr uint64_t TOTAL_MESSAGES = NUM_PRODUCERS * MSGS_PER_PRODUCER;

    std::cout << "===============================================================\n";
    std::cout << " MPMC QUEUE MULTI-THREADED CONTENTION BENCHMARK\n";
    std::cout << "===============================================================\n";
    std::cout << "Producers:        " << NUM_PRODUCERS << "\n";
    std::cout << "Consumers:        " << NUM_CONSUMERS << "\n";
    std::cout << "Total Messages:   " << TOTAL_MESSAGES << "\n";
    std::cout << "Queue Capacity:   " << CAPACITY << " slots\n";
    std::cout << "---------------------------------------------------------------\n";

    auto queue = std::make_unique<lockfree::MPMCQueue<uint64_t, CAPACITY>>();
    std::atomic<bool> start_flag{false};
    std::atomic<uint64_t> total_consumed{0};
    std::atomic<uint64_t> consumer_checksum{0};

    std::vector<std::thread> consumers;
    std::vector<std::thread> producers;

    for (size_t c = 0; c < NUM_CONSUMERS; ++c) {
        consumers.emplace_back([&, c]() {
            lockfree::pin_current_thread_to_core(static_cast<int>((c + NUM_PRODUCERS) % lockfree::get_hardware_concurrency()));
            while (!start_flag.load(std::memory_order_acquire)) {
                lockfree::cpu_relax();
            }

            uint64_t local_sum = 0;
            uint64_t val = 0;

            while (total_consumed.load(std::memory_order_relaxed) < TOTAL_MESSAGES) {
                if (queue->try_pop(val)) {
                    local_sum += val;
                    total_consumed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    lockfree::cpu_relax();
                }
            }
            consumer_checksum.fetch_add(local_sum, std::memory_order_relaxed);
        });
    }

    for (size_t p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&, p]() {
            lockfree::pin_current_thread_to_core(static_cast<int>(p % lockfree::get_hardware_concurrency()));
            while (!start_flag.load(std::memory_order_acquire)) {
                lockfree::cpu_relax();
            }

            const uint64_t start_val = p * MSGS_PER_PRODUCER;
            for (uint64_t i = 0; i < MSGS_PER_PRODUCER; ++i) {
                while (!queue->try_push(start_val + i)) {
                    lockfree::cpu_relax();
                }
            }
        });
    }

    const auto start_time = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();
    const auto end_time = std::chrono::steady_clock::now();

    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    const double seconds = static_cast<double>(duration_ms) / 1000.0;
    const double mops = (static_cast<double>(TOTAL_MESSAGES) / seconds) / 1e6;

    std::cout << "\nResults:\n";
    std::cout << "  Consumed Total:   " << total_consumed.load() << " / " << TOTAL_MESSAGES << "\n";
    std::cout << "  Elapsed Time:     " << duration_ms << " ms\n";
    std::cout << "  MPMC Throughput:  " << std::fixed << std::setprecision(2) << mops << " Million ops/sec\n";
    std::cout << "===============================================================\n";

    return 0;
}
