#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include "lockfree/spsc_queue.hpp"
#include "lockfree/mpmc_queue.hpp"
#include "lockfree/cpu_affinity.hpp"
#include "lockfree/tsc_clock.hpp"

void print_banner() {
    std::cout << 
"================================================================================\n"
"   _   _ _ _                   _                    _            _     \n"
"  | | | | | |_ _ __ __ _      | |    ___   ___ | | _| |_ __ ___  ___| |__  \n"
"  | | | | | __| '__/ _` |_____| |   / _ \\ / __| |/ /|  _/ '__/ _ \\/ _ \\ '_ \\ \n"
"  | |_| | | |_| | | (_| |_____| |__| (_) | (__|   < | || | |  __/  __/ | | |\n"
"   \\___/|_|\\__|_|  \\__,_|     |_____\\___/ \\___|_|\\_\\\\__|_|  \\___|\\___|_| |_|\n"
"            Lock-Free Ultra-Low Latency Messaging Engine (C++20)\n"
"================================================================================\n\n";
}

void print_hardware_summary() {
    std::cout << "Hardware & Architecture Diagnostics:\n";
    std::cout << "  Logical CPU Cores:        " << lockfree::get_hardware_concurrency() << '\n';
    std::cout << "  L1 Cache Line Size:       " << lockfree::CACHE_LINE_SIZE << " bytes\n";
    std::cout << "  Hardware TSC Rate:        " << std::fixed << std::setprecision(3) 
              << lockfree::TscClock::cycles_per_ns() << " GHz\n";
#if defined(__x86_64__) || defined(_M_X64)
    std::cout << "  CPU Architecture:         x86_64 (Intel/AMD SSE/AVX)\n";
    std::cout << "  Pipeline Relax Hint:      _mm_pause (Hardware SMT pause)\n";
#endif
    std::cout << "--------------------------------------------------------------------------------\n\n";
}

void run_live_spsc_demo() {
    std::cout << ">>> Running Live SPSC Pipeline Demonstration (10,000,000 messages) <<<\n";
    constexpr size_t CAPACITY = 65536;
    constexpr uint64_t MSG_COUNT = 10'000'000;

    struct MarketEvent {
        uint64_t id;
        uint64_t timestamp_tsc;
        char symbol[8];
        double price;
        uint32_t volume;
    };

    auto queue = std::make_unique<lockfree::SPSCQueue<MarketEvent, CAPACITY>>();
    std::atomic<bool> start_flag{false};

    std::chrono::steady_clock::time_point t_start;
    std::chrono::steady_clock::time_point t_end;

    std::thread consumer([&]() {
        lockfree::pin_current_thread_to_core(1);
        while (!start_flag.load(std::memory_order_acquire)) { lockfree::cpu_relax(); }

        MarketEvent ev;
        uint64_t received = 0;
        while (received < MSG_COUNT) {
            if (queue->try_pop(ev)) {
                ++received;
            } else {
                lockfree::cpu_relax();
            }
        }
        t_end = std::chrono::steady_clock::now();
    });

    std::thread producer([&]() {
        lockfree::pin_current_thread_to_core(0);
        while (!start_flag.load(std::memory_order_acquire)) { lockfree::cpu_relax(); }

        MarketEvent ev;
        std::memcpy(ev.symbol, "SOLACE", 7);
        ev.price = 145.25;
        ev.volume = 100;

        for (uint64_t i = 0; i < MSG_COUNT; ++i) {
            ev.id = i;
            ev.timestamp_tsc = lockfree::TscClock::rdtsc();
            while (!queue->try_push(ev)) {
                lockfree::cpu_relax();
            }
        }
    });

    t_start = std::chrono::steady_clock::now();
    start_flag.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    const auto dur_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
    const double sec = static_cast<double>(dur_ns) / 1e9;
    const double mops = (static_cast<double>(MSG_COUNT) / sec) / 1e6;
    const double ns_per_op = static_cast<double>(dur_ns) / static_cast<double>(MSG_COUNT);

    std::cout << "  Processed:        " << MSG_COUNT << " msgs in " << std::fixed << std::setprecision(3) << sec << " s\n";
    std::cout << "  Throughput:       " << std::fixed << std::setprecision(2) << mops << " Million msgs/sec\n";
    std::cout << "  Average Transit:  " << std::fixed << std::setprecision(2) << ns_per_op << " ns/msg\n\n";
}

int main(int argc, char** argv) {
    print_banner();
    print_hardware_summary();

    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: ./message_queue_demo\n"
                  << "Run unit tests: make test\n"
                  << "Run false-sharing benchmark: make bench_false_sharing\n"
                  << "Run throughput benchmark:    make bench_throughput\n"
                  << "Run latency RTT benchmark:   make bench_latency\n"
                  << "Run MPMC benchmark:          make bench_mpmc\n";
        return 0;
    }

    run_live_spsc_demo();

    std::cout << "Run 'make bench' to execute the comprehensive benchmark suite!\n";
    return 0;
}
