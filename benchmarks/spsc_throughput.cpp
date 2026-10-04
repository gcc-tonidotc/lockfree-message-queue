#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstring>
#include <cstdint>
#include "lockfree/spsc_queue.hpp"
#include "lockfree/cpu_affinity.hpp"

// Realistic financial / event mesh market data message (32 bytes)
struct MarketMessage {
    uint64_t seq_id;
    uint64_t send_time_ns;
    char symbol[8];
    float price;
    uint32_t volume;
};

int main() {
    constexpr size_t CAPACITY = 65536;
    constexpr uint64_t TOTAL_MESSAGES = 50'000'000;

    std::cout << "===============================================================\n";
    std::cout << " SPSC QUEUE THROUGHPUT BENCHMARK (Sustained Message Routing)\n";
    std::cout << "===============================================================\n";
    std::cout << "Message Type:     MarketMessage (" << sizeof(MarketMessage) << " bytes)\n";
    std::cout << "Total Messages:   " << TOTAL_MESSAGES << "\n";
    std::cout << "Queue Capacity:   " << CAPACITY << " slots\n";
    std::cout << "CPU Core 0:       Producer Thread (Pinned)\n";
    std::cout << "CPU Core 1:       Consumer Thread (Pinned)\n";
    std::cout << "---------------------------------------------------------------\n";

    auto queue = std::make_unique<lockfree::SPSCQueue<MarketMessage, CAPACITY>>();
    std::atomic<bool> start_signal{false};

    std::chrono::steady_clock::time_point start_tp;
    std::chrono::steady_clock::time_point end_tp;

    std::thread consumer([&]() {
        lockfree::pin_current_thread_to_core(1);
        while (!start_signal.load(std::memory_order_acquire)) {
            lockfree::cpu_relax();
        }

        MarketMessage msg;
        uint64_t checksum = 0;

        for (uint64_t i = 0; i < TOTAL_MESSAGES; ++i) {
            while (!queue->try_pop(msg)) {
                lockfree::cpu_relax();
            }
            checksum += msg.seq_id;
        }

        end_tp = std::chrono::steady_clock::now();
        // Prevent compiler dead-code elimination of checksum
        if (checksum == 0) std::cout << "Zero checksum\n";
    });

    std::thread producer([&]() {
        lockfree::pin_current_thread_to_core(0);
        while (!start_signal.load(std::memory_order_acquire)) {
            lockfree::cpu_relax();
        }

        MarketMessage msg;
        std::memcpy(msg.symbol, "SOLACE01", 8);
        msg.price = 142.50f;
        msg.volume = 500;

        for (uint64_t i = 0; i < TOTAL_MESSAGES; ++i) {
            msg.seq_id = i;
            msg.send_time_ns = i;
            while (!queue->try_push(msg)) {
                lockfree::cpu_relax();
            }
        }
    });

    start_tp = std::chrono::steady_clock::now();
    start_signal.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    const auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_tp - start_tp).count();
    const double seconds = static_cast<double>(duration_ns) / 1e9;
    const double mops = (static_cast<double>(TOTAL_MESSAGES) / seconds) / 1e6;
    const double ns_per_msg = static_cast<double>(duration_ns) / static_cast<double>(TOTAL_MESSAGES);

    std::cout << "\nResults:\n";
    std::cout << "  Elapsed Time:     " << std::fixed << std::setprecision(3) << seconds << " seconds\n";
    std::cout << "  Throughput:       " << std::fixed << std::setprecision(2) << mops << " Million msgs/sec (Mops)\n";
    std::cout << "  Avg Latency/Msg:  " << std::fixed << std::setprecision(2) << ns_per_msg << " nanoseconds\n";
    std::cout << "===============================================================\n";

    return 0;
}
