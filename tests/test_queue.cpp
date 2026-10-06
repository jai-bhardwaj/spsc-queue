// Tests for spsc::Queue.
//
// The single-threaded tests pin down the edges (wrap, full, empty, lifetime).
// The threaded soak test is the one that matters: it is written to be run under
// ThreadSanitizer, where a missing acquire/release would be reported as a data
// race even if the test otherwise passed.

#include "spsc/queue.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

void test_capacity_rounds_up_to_power_of_two() {
    CHECK(spsc::Queue<int>(5).capacity() == 8);
    CHECK(spsc::Queue<int>(8).capacity() == 8);
    CHECK(spsc::Queue<int>(9).capacity() == 16);
    CHECK(spsc::Queue<int>(1).capacity() == 2);  // clamped to the minimum
    CHECK(spsc::Queue<int>(0).capacity() == 2);
}

void test_push_pop_fifo_order() {
    spsc::Queue<int> q(8);
    for (int i = 0; i < 7; ++i) CHECK(q.push(i));
    int out = -1;
    for (int i = 0; i < 7; ++i) {
        CHECK(q.try_pop(out));
        CHECK(out == i);
    }
    CHECK(!q.try_pop(out));
}

void test_full_reserves_one_slot() {
    spsc::Queue<int> q(4);  // 4 slots, 3 usable
    CHECK(q.push(1));
    CHECK(q.push(2));
    CHECK(q.push(3));
    CHECK(!q.push(4));  // full: one slot is reserved to separate full from empty
    CHECK(q.size_approx() == 3);
}

void test_empty_on_construction() {
    spsc::Queue<int> q(8);
    int out = 0;
    CHECK(q.empty_approx());
    CHECK(q.front() == nullptr);
    CHECK(!q.try_pop(out));
}

// The interesting edge: drive the ring well past its capacity so the indices
// wrap repeatedly. An off-by-one in the mask shows up here and nowhere else.
void test_wraparound_many_times() {
    spsc::Queue<int> q(4);
    int out = -1;
    for (int i = 0; i < 1000; ++i) {
        CHECK(q.push(i));
        CHECK(q.try_pop(out));
        CHECK(out == i);
    }
    CHECK(q.empty_approx());
}

void test_alternating_partial_fill() {
    spsc::Queue<int> q(8);
    int out = -1;
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 5; ++i) CHECK(q.push(round * 10 + i));
        for (int i = 0; i < 5; ++i) {
            CHECK(q.try_pop(out));
            CHECK(out == round * 10 + i);
        }
    }
}

// Lifetime: every constructed element must be destroyed exactly once, including
// the ones still queued when the Queue itself is destroyed.
struct Counted {
    static inline int live = 0;
    int v;
    explicit Counted(int x = 0) : v(x) { ++live; }
    Counted(const Counted& o) : v(o.v) { ++live; }
    Counted(Counted&& o) noexcept : v(o.v) { ++live; }
    Counted& operator=(const Counted&) = default;
    Counted& operator=(Counted&&) noexcept = default;
    ~Counted() { --live; }
};

void test_destroys_every_element() {
    Counted::live = 0;
    {
        spsc::Queue<Counted> q(8);
        for (int i = 0; i < 5; ++i) CHECK(q.emplace(i));
        Counted out;
        CHECK(q.try_pop(out));
        CHECK(out.v == 0);
        // 4 elements deliberately left in the queue at destruction.
    }
    CHECK(Counted::live == 0);
}

void test_non_trivial_type() {
    spsc::Queue<std::string> q(8);
    CHECK(q.emplace("hello"));
    CHECK(q.emplace(std::string("world")));
    std::string out;
    CHECK(q.try_pop(out));
    CHECK(out == "hello");
    CHECK(q.try_pop(out));
    CHECK(out == "world");
}

void test_front_then_pop() {
    spsc::Queue<int> q(8);
    CHECK(q.push(42));
    int* p = q.front();
    CHECK(p != nullptr);
    CHECK(*p == 42);
    q.pop();
    CHECK(q.front() == nullptr);
}

// The soak test. Run under TSan this is what proves the memory ordering: the
// consumer must observe every value exactly once, in order, with no torn reads.
void test_threaded_soak() {
    constexpr int kItems = 500000;
    spsc::Queue<int> q(1024);
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (int i = 0; i < kItems; ++i) {
            while (!q.push(i)) {
                // Spin: the consumer will drain. A real system would back off.
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    long long received = 0;
    int expected = 0;
    bool order_ok = true;
    while (received < kItems) {
        int v = 0;
        if (q.try_pop(v)) {
            if (v != expected) order_ok = false;
            ++expected;
            ++received;
        } else if (producer_done.load(std::memory_order_acquire) && q.empty_approx()) {
            break;
        }
    }
    producer.join();

    CHECK(received == kItems);
    CHECK(order_ok);
    CHECK(q.empty_approx());
}

struct Test {
    const char* name;
    void (*fn)();
};

}  // namespace

int main() {
    const Test tests[] = {
        {"capacity rounds up to power of two", test_capacity_rounds_up_to_power_of_two},
        {"push/pop preserves FIFO order", test_push_pop_fifo_order},
        {"full reserves one slot", test_full_reserves_one_slot},
        {"empty on construction", test_empty_on_construction},
        {"wraps around many times", test_wraparound_many_times},
        {"alternating partial fill", test_alternating_partial_fill},
        {"destroys every element", test_destroys_every_element},
        {"works with non-trivial types", test_non_trivial_type},
        {"front() then pop()", test_front_then_pop},
        {"threaded soak, 500k items", test_threaded_soak},
    };

    for (const auto& t : tests) {
        const int before = g_failures;
        t.fn();
        std::printf("%-40s %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }

    if (g_failures == 0) {
        std::printf("\nAll %zu tests passed.\n", sizeof(tests) / sizeof(tests[0]));
        return 0;
    }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
