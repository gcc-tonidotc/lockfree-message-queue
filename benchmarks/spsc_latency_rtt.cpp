#include <iostream>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <numeric>
#include <thread>
#include <atomic>
#include <cstdint>
#include "lockfree/spsc_queue.hpp"
#include "lockfree/cpu_affinity.hpp"
#include "lockfree/tsc_clock.hpp"

struct LatencyToken {
    uint64_t id;
    uint64_t payload;
};

int main() {
    constexpr size_t CAPACITY = 4096;
    constexpr size_t WARMUP_COUNT = 100'000;
    constexpr size_t SAMPLE_COUNT = 500'000;

    std::cout << "===============================================================\n";
    std::cout << " SPSC PING-PONG ROUND-TRIP LATENCY (RTT) BENCHMARK\n";
    std::cout << "===============================================================\n";
    std::cout << "Measuring one-way queue transit latency (RTT / 2).\n";
    std::cout << "Warm-up Samples:  " << WARMUP_COUNT << "\n";
    std::cout << "Measured Samples: " << SAMPLE_COUNT << "\n";
    std::cout << "Core 0:           Thread A (Sender/Receiver)\n";
    std::cout << "Core 1:           Thread B (Echo Responder)\n";
    std::cout << "---------------------------------------------------------------\n";

    // Calibrate TSC
    const double cycles_per_ns = lockfree::TscClock::cycles_per_ns();
    std::cout << "Calibrated TSC:   " << std::fixed << std::setprecision(3) << cycles_per_ns << " GHz\n";

    auto q_a_to_b = std::make_unique<lockfree::SPSCQueue<LatencyToken, CAPACITY>>();
    auto q_b_to_a = std::make_unique<lockfree::SPSCQueue<LatencyToken, CAPACITY>>();

    std::atomic<bool> terminate_flag{false};
    std::atomic<bool> ready_flag{false};

    std::vector<double> latencies_ns;
    latencies_ns.reserve(SAMPLE_COUNT);

    // Thread B: Echo Responder
    std::thread thread_b([&]() {
        lockfree::pin_current_thread_to_core(1);
        ready_flag.store(true, std::memory_order_release);

        LatencyToken token;
        while (!terminate_flag.load(std::memory_order_relaxed)) {
            if (q_a_to_b->try_pop(token)) {
                while (!q_b_to_a->try_push(token)) {
                    lockfree::cpu_relax();
                }
            } else {
                lockfree::cpu_relax();
            }
        }
    });

    // Thread A: Sender & Measurer
    lockfree::pin_current_thread_to_core(0);
    while (!ready_flag.load(std::memory_order_acquire)) {
        lockfree::cpu_relax();
    }

    LatencyToken token{1, 42};
    LatencyToken reply;

    // Warm-up phase to warm up CPU branch predictors, instruction caches, and boost clocks
    std::cout << "Warming up caches & branch predictors...\n";
    for (size_t i = 0; i < WARMUP_COUNT; ++i) {
        token.id = i;
        while (!q_a_to_b->try_push(token)) { lockfree::cpu_relax(); }
        while (!q_b_to_a->try_pop(reply)) { lockfree::cpu_relax(); }
    }

    // Measurement phase
    std::cout << "Collecting " << SAMPLE_COUNT << " latency samples via RDTSC...\n";
    for (size_t i = 0; i < SAMPLE_COUNT; ++i) {
        token.id = i;

        const uint64_t t0 = lockfree::TscClock::rdtsc();
        while (!q_a_to_b->try_push(token)) { lockfree::cpu_relax(); }
        while (!q_b_to_a->try_pop(reply)) { lockfree::cpu_relax(); }
        const uint64_t t1 = lockfree::TscClock::rdtsc();

        // One-way latency is half of round-trip time
        const double rtt_ns = lockfree::TscClock::to_nanoseconds(t1 - t0);
        latencies_ns.push_back(rtt_ns / 2.0);
    }

    terminate_flag.store(true, std::memory_order_release);
    thread_b.join();

    // Sort to compute accurate percentiles
    std::sort(latencies_ns.begin(), latencies_ns.end());

    const double min_lat = latencies_ns.front();
    const double max_lat = latencies_ns.back();
    const double sum_lat = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0);
    const double mean_lat = sum_lat / static_cast<double>(latencies_ns.size());

    auto get_percentile = [&](double p) -> double {
        const size_t idx = static_cast<size_t>(p * static_cast<double>(latencies_ns.size() - 1));
        return latencies_ns[idx];
    };

    const double p50 = get_percentile(0.50);
    const double p90 = get_percentile(0.90);
    const double p99 = get_percentile(0.99);
    const double p999 = get_percentile(0.999);
    const double p9999 = get_percentile(0.9999);

    std::cout << "\n===============================================================\n";
    std::cout << " ONE-WAY LATENCY PERCENTILES (Nanoseconds)\n";
    std::cout << "===============================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Min Latency:       " << std::setw(8) << min_lat << " ns\n";
    std::cout << "  Mean Latency:      " << std::setw(8) << mean_lat << " ns\n";
    std::cout << "  50th % (Median):   " << std::setw(8) << p50 << " ns\n";
    std::cout << "  90th %:            " << std::setw(8) << p90 << " ns\n";
    std::cout << "  99th %:            " << std::setw(8) << p99 << " ns\n";
    std::cout << "  99.9th %:          " << std::setw(8) << p999 << " ns\n";
    std::cout << "  99.99th %:         " << std::setw(8) << p9999 << " ns\n";
    std::cout << "  Max (Tail):        " << std::setw(8) << max_lat << " ns\n";
    std::cout << "===============================================================\n";

    return 0;
}
