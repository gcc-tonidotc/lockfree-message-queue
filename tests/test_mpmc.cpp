#include <iostream>
#include <cassert>
#include <vector>
#include <thread>
#include <atomic>
#include <numeric>
#include "lockfree/mpmc_queue.hpp"
#include "lockfree/cpu_affinity.hpp"

void test_basic_mpmc() {
    std::cout << "  [TEST] Basic MPMC push/pop ... ";
    lockfree::MPMCQueue<int, 4> q;
    assert(q.empty());

    assert(q.try_push(100));
    assert(q.try_push(200));
    assert(q.try_push(300));
    assert(q.try_push(400));
    // Full
    assert(!q.try_push(500));

    int val = 0;
    assert(q.try_pop(val) && val == 100);
    assert(q.try_push(500));

    assert(q.try_pop(val) && val == 200);
    assert(q.try_pop(val) && val == 300);
    assert(q.try_pop(val) && val == 400);
    assert(q.try_pop(val) && val == 500);
    assert(q.empty());
    assert(!q.try_pop(val));
    std::cout << "PASSED\n";
}

void test_concurrent_mpmc_checksum() {
    std::cout << "  [TEST] Concurrent MPMC Integrity (4 Producers, 4 Consumers, Checksum) ... ";
    constexpr size_t CAPACITY = 1024;
    constexpr size_t NUM_PROD = 4;
    constexpr size_t NUM_CONS = 4;
    constexpr uint64_t PER_PROD = 500'000;
    constexpr uint64_t TOTAL = NUM_PROD * PER_PROD;

    auto q = std::make_unique<lockfree::MPMCQueue<uint64_t, CAPACITY>>();
    std::atomic<bool> start{false};
    std::atomic<uint64_t> consumed_count{0};
    std::atomic<uint64_t> total_consumed_sum{0};

    // Theoretical expected sum: sum_{i=0}^{TOTAL-1} i = TOTAL * (TOTAL - 1) / 2
    uint64_t expected_sum = 0;
    for (uint64_t i = 0; i < TOTAL; ++i) {
        expected_sum += i;
    }

    std::vector<std::thread> consumers;
    for (size_t c = 0; c < NUM_CONS; ++c) {
        consumers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {}

            uint64_t local_sum = 0;
            uint64_t val = 0;
            while (consumed_count.load(std::memory_order_relaxed) < TOTAL) {
                if (q->try_pop(val)) {
                    local_sum += val;
                    consumed_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    lockfree::cpu_relax();
                }
            }
            total_consumed_sum.fetch_add(local_sum, std::memory_order_relaxed);
        });
    }

    std::vector<std::thread> producers;
    for (size_t p = 0; p < NUM_PROD; ++p) {
        producers.emplace_back([&, p]() {
            while (!start.load(std::memory_order_acquire)) {}

            const uint64_t start_val = p * PER_PROD;
            for (uint64_t i = 0; i < PER_PROD; ++i) {
                while (!q->try_push(start_val + i)) {
                    lockfree::cpu_relax();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    assert(consumed_count.load() == TOTAL);
    assert(total_consumed_sum.load() == expected_sum);
    assert(q->empty());
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " RUNNING MPMC LOCK-FREE QUEUE UNIT TESTS\n";
    std::cout << "===============================================================\n";

    test_basic_mpmc();
    test_concurrent_mpmc_checksum();

    std::cout << "\nALL MPMC TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "===============================================================\n";
    return 0;
}
