#include <iostream>
#include <cassert>
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include "lockfree/spsc_queue.hpp"
#include "lockfree/cpu_affinity.hpp"

// Object lifecycle tracker to catch memory leaks and double destruction
struct InstanceTracker {
    static inline std::atomic<int> alive_count{0};
    static inline std::atomic<int> construct_count{0};
    static inline std::atomic<int> destruct_count{0};

    int id;

    explicit InstanceTracker(int val) : id(val) {
        alive_count.fetch_add(1, std::memory_order_relaxed);
        construct_count.fetch_add(1, std::memory_order_relaxed);
    }

    InstanceTracker(const InstanceTracker& other) : id(other.id) {
        alive_count.fetch_add(1, std::memory_order_relaxed);
        construct_count.fetch_add(1, std::memory_order_relaxed);
    }

    InstanceTracker(InstanceTracker&& other) noexcept : id(other.id) {
        alive_count.fetch_add(1, std::memory_order_relaxed);
        construct_count.fetch_add(1, std::memory_order_relaxed);
        other.id = -1;
    }

    InstanceTracker& operator=(const InstanceTracker& other) {
        id = other.id;
        return *this;
    }

    InstanceTracker& operator=(InstanceTracker&& other) noexcept {
        id = other.id;
        other.id = -1;
        return *this;
    }

    ~InstanceTracker() noexcept {
        alive_count.fetch_sub(1, std::memory_order_relaxed);
        destruct_count.fetch_add(1, std::memory_order_relaxed);
    }

    static void reset() {
        alive_count.store(0);
        construct_count.store(0);
        destruct_count.store(0);
    }
};

void test_basic_push_pop() {
    std::cout << "  [TEST] Basic push/pop ... ";
    lockfree::SPSCQueue<int, 4> q;
    assert(q.empty());
    assert(q.size() == 0);

    assert(q.try_push(10));
    assert(q.try_push(20));
    assert(q.size() == 2);
    assert(!q.empty());

    int val = 0;
    assert(q.try_pop(val));
    assert(val == 10);
    assert(q.try_pop(val));
    assert(val == 20);
    assert(q.empty());
    assert(!q.try_pop(val));
    std::cout << "PASSED\n";
}

void test_capacity_boundary() {
    std::cout << "  [TEST] Capacity boundaries ... ";
    lockfree::SPSCQueue<int, 4> q;
    assert(q.try_push(1));
    assert(q.try_push(2));
    assert(q.try_push(3));
    assert(q.try_push(4));

    // Queue is now full (capacity 4)
    assert(!q.try_push(5));

    int val = 0;
    assert(q.try_pop(val) && val == 1);
    // Can push again now
    assert(q.try_push(5));
    assert(!q.try_push(6));

    assert(q.try_pop(val) && val == 2);
    assert(q.try_pop(val) && val == 3);
    assert(q.try_pop(val) && val == 4);
    assert(q.try_pop(val) && val == 5);
    assert(q.empty());
    std::cout << "PASSED\n";
}

void test_fifo_ordering() {
    std::cout << "  [TEST] Strict FIFO ordering ... ";
    constexpr size_t N = 1000;
    lockfree::SPSCQueue<size_t, 1024> q;

    for (size_t i = 0; i < N; ++i) {
        assert(q.try_push(i));
    }

    for (size_t i = 0; i < N; ++i) {
        size_t val = 0;
        assert(q.try_pop(val));
        assert(val == i);
    }
    assert(q.empty());
    std::cout << "PASSED\n";
}

void test_move_only_types() {
    std::cout << "  [TEST] Move-only types (std::unique_ptr) ... ";
    lockfree::SPSCQueue<std::unique_ptr<int>, 8> q;

    auto p1 = std::make_unique<int>(100);
    auto p2 = std::make_unique<int>(200);

    assert(q.try_push(std::move(p1)));
    assert(q.try_emplace(std::make_unique<int>(200)));

    std::unique_ptr<int> out1;
    assert(q.try_pop(out1));
    assert(out1 != nullptr && *out1 == 100);

    auto out2 = q.try_pop();
    assert(out2.has_value() && out2.value() != nullptr && *out2.value() == 200);
    std::cout << "PASSED\n";
}

void test_zero_copy_front_pop() {
    std::cout << "  [TEST] Zero-copy front() / pop() ... ";
    lockfree::SPSCQueue<std::string, 8> q;
    assert(q.try_emplace("Solace PubSub+"));

    std::string* front_ptr = q.front();
    assert(front_ptr != nullptr);
    assert(*front_ptr == "Solace PubSub+");

    // Modify in place without copy
    front_ptr->append(" Ultra-Low Latency");
    assert(*q.front() == "Solace PubSub+ Ultra-Low Latency");

    q.pop();
    assert(q.empty());
    assert(q.front() == nullptr);
    std::cout << "PASSED\n";
}

void test_lifecycle_and_leak_free() {
    std::cout << "  [TEST] Object lifecycle & leak prevention ... ";
    InstanceTracker::reset();

    {
        lockfree::SPSCQueue<InstanceTracker, 8> q;
        assert(q.try_emplace(1));
        assert(q.try_emplace(2));
        assert(q.try_emplace(3));

        InstanceTracker popped(-1);
        assert(q.try_pop(popped));
        assert(popped.id == 1);
        // Queue currently has items 2 and 3. Destructor of queue should clean them up!
    }

    // When queue went out of scope, remaining items 2 and 3 should be destructed
    assert(InstanceTracker::alive_count.load() == 0);
    assert(InstanceTracker::construct_count.load() == InstanceTracker::destruct_count.load());
    std::cout << "PASSED\n";
}

void test_multithreaded_stress() {
    std::cout << "  [TEST] Multi-threaded SPSC stress (5,000,000 items) ... ";
    constexpr size_t CAPACITY = 4096;
    constexpr uint64_t TOTAL = 5'000'000;

    auto q = std::make_unique<lockfree::SPSCQueue<uint64_t, CAPACITY>>();
    std::atomic<bool> start{false};

    std::thread consumer([&]() {
        lockfree::pin_current_thread_to_core(1);
        while (!start.load(std::memory_order_acquire)) {}

        uint64_t expected = 0;
        uint64_t val = 0;
        while (expected < TOTAL) {
            if (q->try_pop(val)) {
                assert(val == expected);
                ++expected;
            } else {
                lockfree::cpu_relax();
            }
        }
    });

    std::thread producer([&]() {
        lockfree::pin_current_thread_to_core(0);
        while (!start.load(std::memory_order_acquire)) {}

        for (uint64_t i = 0; i < TOTAL; ++i) {
            while (!q->try_push(i)) {
                lockfree::cpu_relax();
            }
        }
    });

    start.store(true, std::memory_order_release);
    producer.join();
    consumer.join();

    assert(q->empty());
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "===============================================================\n";
    std::cout << " RUNNING SPSC LOCK-FREE QUEUE UNIT TESTS\n";
    std::cout << "===============================================================\n";

    test_basic_push_pop();
    test_capacity_boundary();
    test_fifo_ordering();
    test_move_only_types();
    test_zero_copy_front_pop();
    test_lifecycle_and_leak_free();
    test_multithreaded_stress();

    std::cout << "\nALL SPSC TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "===============================================================\n";
    return 0;
}
