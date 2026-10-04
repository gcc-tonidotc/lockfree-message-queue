# ⚡ Ultra-Low Latency Lock-Free Message Queue (C++20)

[![C++20](https://img.shields.io/badge/Language-C%2B%2B20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![Build Status](https://img.shields.io/badge/Build-Passing-brightgreen.svg)]()
[![Performance](https://img.shields.io/badge/SPSC_Throughput-235M+_msgs%2Fsec-orange.svg)]()
[![Latency](https://img.shields.io/badge/Median_Latency-157ns-purple.svg)]()

An ultra-optimized, lock-free, zero-allocation messaging engine implementing **Single-Producer Single-Consumer (SPSC)** and **Multi-Producer Multi-Consumer (MPMC)** circular queues in Modern C++20. Designed specifically for microsecond-to-nanosecond high-throughput event routing, electronic trading, and low-latency message broker architectures.

---

## 🎯 Why Solace Engineers & Low-Latency Systems Teams Care

**Solace PubSub+** event brokers process tens of millions of messages per second across global enterprise event meshes. Standard concurrency primitives (like `std::mutex` and `std::condition_variable`) invoke OS kernel transitions, cause thread descheduling, and introduce catastrophic tail latency spikes (often >10,000ns).

This project proves mastery of the core principles required in high-performance messaging infrastructure:
1. **Mechanical Sympathy & Cache Coherency (MESI Protocol)**: Cache-line padding (`alignas(64)`) eliminates false sharing, demonstrated empirically with a **>2.2x throughput increase** (235 Mops/sec vs 107 Mops/sec).
2. **Fine-Grained Memory Orderings**: Mathematical precision with `acquire` / `release` semantics and `relaxed` operations—eliminating pipeline stalls from unnecessary sequential consistency (`seq_cst`) full memory fences (`MFENCE`).
3. **Cached Peer Indices Pattern**: Amortizes cross-core L1/L2 cache invalidations by querying peer cursors only when the ring appears full or empty.
4. **Sub-Nanosecond Profiling via Hardware TSC (`RDTSC`)**: Microbenchmarking with serialized `__rdtscp` cycles rather than OS clock syscalls (`clock_gettime`).
5. **Core Affinity & Pipeline Relaxation**: Thread pinning via `pthread_setaffinity_np` and `_mm_pause()` spin-wait hints to prevent pipeline flushes and optimize CPU SMT execution.

---

## 🏗️ Architecture & Memory Layout

### 1. SPSC Cache-Line Separation (Eliminating False Sharing)

When two threads on separate CPU cores concurrently modify variables residing on the **same 64-byte cache line**, the CPU's MESI cache coherency protocol marks the line as `Invalid` in the peer core's L1 cache. This triggers constant bus traffic and cache bouncing:

```
[ UNPADDED QUEUE: False Sharing Disaster ]
+---------------------------------------------------------------+
|  write_idx_ (8B)  |  read_idx_ (8B)  |   Empty Padding (48B)  |  <-- Single 64-byte Cache Line
+---------------------------------------------------------------+
       ^                      ^
 Producer modifies      Consumer modifies
   (Invalidates)          (Invalidates)

[ PADDED QUEUE (Our Implementation): Zero False Sharing ]
+---------------------------------------------------------------+
|  write_idx_ (8B)  | cached_read_idx_ (8B) |  Pad to 64B       |  <-- Line 1 (Core 0 Exclusive)
+---------------------------------------------------------------+
|  read_idx_ (8B)   | cached_write_idx_ (8B)|  Pad to 64B       |  <-- Line 2 (Core 1 Exclusive)
+---------------------------------------------------------------+
|  Ring Buffer Slots [0 ... Capacity-1]                         |  <-- Line 3+ (Data Storage)
+---------------------------------------------------------------+
```

### 2. Cached Peer Indices Optimization

In standard ring buffers, the producer reads the consumer's `read_idx_` on *every* push, and the consumer reads `write_idx_` on *every* pop. This produces continuous cross-core cache invalidation traffic.

**Our Design**:
* **Producer** maintains a private `cached_read_idx_`. It only issues an atomic load (`memory_order_acquire`) against `read_idx_` when the buffer appears full.
* **Consumer** maintains a private `cached_write_idx_`. It only issues an atomic load against `write_idx_` when the buffer appears empty.
* **Result**: In typical sustained streaming, 99%+ of pushes and pops complete entirely within the core's local L1 cache with 0 bus traffic.

---

## ⚙️ Memory Ordering Rationale

| Operation | Atomic Variable | Memory Order | Rationale |
| :--- | :--- | :--- | :--- |
| **Producer Read Own Pos** | `write_idx_` | `relaxed` | Single-producer invariant: only the producer modifies `write_idx_`. |
| **Producer Read Consumer Pos** | `read_idx_` | `acquire` | Ensures consumer has freed the slot and memory writes are visible. |
| **Producer Publish Slot** | `write_idx_` | `release` | Guarantees slot payload store is globally visible *before* `write_idx_` increments. |
| **Consumer Read Own Pos** | `read_idx_` | `relaxed` | Single-consumer invariant: only the consumer modifies `read_idx_`. |
| **Consumer Read Producer Pos** | `write_idx_` | `acquire` | Guarantees new payload is fully written *before* consumer accesses the slot. |
| **Consumer Release Slot** | `read_idx_` | `release` | Signals slot vacancy back to producer. |

---

## 📊 Benchmark Results

*Hardware: Intel(R) Core(TM) Ultra 7 256V @ 3.05 GHz, 8 Physical Cores, Ubuntu 24.04 (WSL2), GCC 13.3 (-O3 -march=native).*

### 1. False Sharing Impact (Empirical Proof)
*Test: 20,000,000 messages routed across pinned CPU cores.*

| Implementation | Elapsed Time | Throughput | Speedup |
| :--- | :---: | :---: | :---: |
| **Unpadded SPSC** (Shared Cache Line) | 186 ms | 107.53 Mops/sec | Baseline |
| **Padded SPSC** (`alignas(64)` + Cached Indices) | **85 ms** | **235.29 Mops/sec** | **2.19x Faster** |

### 2. Sustained Throughput (Realistic 32-Byte Market Event)
*Test: 50,000,000 MarketData payloads (`symbol`, `price`, `volume`, `timestamp`).*

* **Total Messages**: 50,000,000
* **Total Time**: 2.634 seconds
* **Sustained Throughput**: **18.99 Million messages/sec**
* **Average Transit Latency**: **52.67 nanoseconds/msg**

### 3. Ping-Pong Round-Trip Latency (RTT) Percentiles
*Test: 500,000 ping-pong exchanges measured via Hardware Time-Stamp Counter (`RDTSC`). One-way latency = RTT / 2.*

| Metric / Percentile | Transit Latency (One-Way) |
| :--- | :---: |
| **Minimum** | **75.12 ns** |
| **Mean** | **202.19 ns** |
| **50th % (Median)** | **157.13 ns** |
| **90th %** | **241.11 ns** |
| **99th %** | **368.72 ns** |
| **99.9th %** | 5.87 µs |
| **99.99th %** | 51.90 µs |

### 4. Multi-Producer Multi-Consumer (MPMC) Scalability
*Test: Dmitry Vyukov bounded algorithm with 2 Producers, 2 Consumers under concurrent contention (10,000,000 messages).*

* **Total Consumed**: 10,000,000 / 10,000,000
* **Data Integrity**: 100% Checksum Verification Match (0 lost, 0 duplicates)
* **Contention Throughput**: **4.81 Million ops/sec**

---

## 🚀 Building & Running

### Requirements
* Modern C++20 compliant compiler (`g++-11+`, `clang++-14+`, or `MSVC 2019+`)
* Linux, WSL, or POSIX-compatible environment for `pthread_setaffinity_np`
* `make` or `cmake >= 3.20`

### Quick Start (Makefile)
```bash
# Build all targets with -O3 -march=native
make

# Run the complete Unit Test Suite (Lifecycle, FIFO, Leak-Free, Move-Only)
make test

# Run the interactive Live Demo
make run

# Run Individual Benchmarks
make bench_false_sharing   # Shows 2.2x speedup of cache padding
make bench_throughput       # 50M message throughput test
make bench_latency          # Sub-microsecond RDTSC ping-pong percentiles
make bench_mpmc             # Multi-producer multi-consumer contention test
```

### Quick Start (CMake)
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
ctest --output-on-failure
```

---

## 🔍 Key Code Samples

### In-Place Emplacement & Cached Read Index
```cpp
template <typename... Args>
[[nodiscard]] bool try_emplace(Args&&... args) {
    const size_t current_write = write_idx_.load(std::memory_order_relaxed);

    // Check against locally cached read index first
    if (current_write - cached_read_idx_ >= Capacity) {
        cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
        if (current_write - cached_read_idx_ >= Capacity) {
            return false; // Queue is full
        }
    }

    const size_t slot = current_write & BUFFER_MASK;
    ::new (static_cast<void*>(buffer_[slot].data)) T(std::forward<Args>(args)...);

    // Release payload to consumer
    write_idx_.store(current_write + 1, std::memory_order_release);
    return true;
}
```

### Zero-Copy Consumer Access (`front()` & `pop()`)
```cpp
// Inspect front element directly without copying
MarketMessage* msg = queue.front();
if (msg) {
    process(*msg);
    queue.pop(); // Advance read cursor
}
```

---

## 📚 References & Further Reading
* Dmitry Vyukov: *Bounded MPMC Queue* (1024cores.net)
* Martin Thompson: *Mechanical Sympathy & The LMAX Disruptor*
* Ulrich Drepper: *What Every Programmer Should Know About Memory*
* Paul E. McKenney: *Is Parallel Programming Hard, And, If So, What Can You Do About It?*
