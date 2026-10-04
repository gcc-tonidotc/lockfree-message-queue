#pragma once

#include <cstdint>
#include <chrono>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <x86intrin.h>
#define LOCKFREE_HAS_RDTSC 1
#endif

namespace lockfree {

/**
 * @brief High-precision Hardware Time-Stamp Counter (TSC) profiler.
 *
 * RDTSC is an x86 instruction that returns the number of processor cycles since reset.
 * It has drastically lower overhead (~15-25 cycles) compared to OS clock calls (clock_gettime),
 * making it the gold standard for sub-microsecond and nanosecond benchmarking in low-latency trading & messaging.
 */
class TscClock {
public:
    /**
     * @brief Read the current CPU cycle counter using serialized RDTSCP / RDTSC.
     */
    [[nodiscard]] static inline uint64_t rdtsc() noexcept {
#if defined(LOCKFREE_HAS_RDTSC)
        unsigned int aux;
        return __rdtscp(&aux);
#else
        return static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()
        );
#endif
    }

    /**
     * @brief Calibrate cycles-per-nanosecond against std::chrono::steady_clock.
     */
    static double cycles_per_ns() noexcept {
        static const double rate = compute_rate();
        return rate;
    }

    /**
     * @brief Convert cycle count difference to nanoseconds.
     */
    static double to_nanoseconds(uint64_t cycles) noexcept {
        return static_cast<double>(cycles) / cycles_per_ns();
    }

private:
    static double compute_rate() noexcept {
#if defined(LOCKFREE_HAS_RDTSC)
        // Sleep for 50ms and measure elapsed cycles to calculate clock frequency
        const auto start_time = std::chrono::steady_clock::now();
        const uint64_t start_cycles = rdtsc();

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        const auto end_time = std::chrono::steady_clock::now();
        const uint64_t end_cycles = rdtsc();

        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        if (elapsed_ns == 0) return 1.0;
        return static_cast<double>(end_cycles - start_cycles) / static_cast<double>(elapsed_ns);
#else
        return 1.0;
#endif
    }
};

} // namespace lockfree
