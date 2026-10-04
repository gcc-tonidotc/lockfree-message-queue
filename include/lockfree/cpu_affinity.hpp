#pragma once

#include <thread>
#include <iostream>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define LOCKFREE_HAS_MM_PAUSE 1
#endif

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace lockfree {

/**
 * @brief Emit a CPU pause instruction to hint the hardware pipeline during spin-waiting.
 * 
 * On x86/x64, executes the `PAUSE` instruction:
 * 1. Prevents pipeline memory order violations when exiting spin loops.
 * 2. Reduces power consumption and core temperature.
 * 3. Frees internal execution units for other SMT / HyperThreads sharing the core.
 */
inline void cpu_relax() noexcept {
#if defined(LOCKFREE_HAS_MM_PAUSE)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

/**
 * @brief Pin the calling thread to a specific CPU core.
 * Essential in ultra-low latency systems to avoid OS scheduler context switches
 * and L1/L2 cache invalidations.
 *
 * @param core_id Zero-indexed logical CPU core ID.
 * @return true if successfully pinned, false otherwise.
 */
inline bool pin_current_thread_to_core(int core_id) noexcept {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    return (rc == 0);
#else
    (void)core_id;
    return false; // Platform-specific pinning not implemented for this target
#endif
}

/**
 * @brief Pin a std::thread to a specific CPU core.
 */
inline bool pin_thread_to_core(std::thread& th, int core_id) noexcept {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    int rc = pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
    return (rc == 0);
#else
    (void)th;
    (void)core_id;
    return false;
#endif
}

/**
 * @brief Returns total logical hardware concurrency.
 */
inline unsigned int get_hardware_concurrency() noexcept {
    return std::max(1u, std::thread::hardware_concurrency());
}

} // namespace lockfree
