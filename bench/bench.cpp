// Benchmarks for spsc::Queue.
//
// Method, stated so the numbers mean something:
//
//   Throughput  — one producer thread and one consumer thread move N items
//                 through the queue as fast as they can. Reported as items/sec
//                 and ns/item. This is the number that matters for a pipeline.
//
//   Round-trip  — two queues, ping-pong. The producer sends a value and waits
//                 for it to come back. Measures latency rather than bandwidth,
//                 and it is the harder number to make look good.
//
//   Baseline    — the same workload through std::mutex + std::deque, which is
//                 what most code actually does. If the lock-free version is not
//                 meaningfully faster than this, it is not worth its complexity.
//
// Every figure is the median of kReps runs, so one unlucky scheduling event
// cannot flatter or ruin the result. Build with -O2 or the numbers are fiction.
//
// Pinning: threads are NOT pinned to cores. On a laptop with heterogeneous
// cores (Apple P/E clusters) that adds variance, which is why the median is
// reported rather than the minimum. Numbers are comparable within a run, not
// across machines.

#include "spsc/queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

constexpr int kReps = 5;
constexpr int kThroughputItems = 2'000'000;
constexpr int kRoundTrips = 200'000;
constexpr std::size_t kCapacity = 1024;

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ---- lock-free throughput --------------------------------------------------

double throughput_spsc_ns_per_item() {
    spsc::Queue<int> q(kCapacity);
    const auto t0 = Clock::now();

    std::thread producer([&] {
        for (int i = 0; i < kThroughputItems; ++i)
            while (!q.push(i)) {}
    });

    int sink = 0;
    for (int i = 0; i < kThroughputItems; ++i) {
        int v;
        while (!q.try_pop(v)) {}
        sink += v & 1;
    }
    producer.join();

    const auto elapsed = std::chrono::duration_cast<ns>(Clock::now() - t0).count();
    // Keep the optimiser from deleting the consumer loop.
    if (sink == -1) std::printf("unreachable\n");
    return static_cast<double>(elapsed) / kThroughputItems;
}

// ---- mutex baseline --------------------------------------------------------

class MutexQueue {
public:
    void push(int v) {
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(v);
    }
    bool try_pop(int& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop_front();
        return true;
    }

private:
    std::mutex m_;
    std::deque<int> q_;
};

double throughput_mutex_ns_per_item() {
    MutexQueue q;
    const auto t0 = Clock::now();

    std::thread producer([&] {
        for (int i = 0; i < kThroughputItems; ++i) q.push(i);
    });

    int sink = 0;
    for (int i = 0; i < kThroughputItems; ++i) {
        int v;
        while (!q.try_pop(v)) {}
        sink += v & 1;
    }
    producer.join();

    const auto elapsed = std::chrono::duration_cast<ns>(Clock::now() - t0).count();
    if (sink == -1) std::printf("unreachable\n");
    return static_cast<double>(elapsed) / kThroughputItems;
}

// ---- round-trip latency ----------------------------------------------------

double roundtrip_spsc_ns() {
    spsc::Queue<int> to(kCapacity), back(kCapacity);
    std::thread echo([&] {
        for (int i = 0; i < kRoundTrips; ++i) {
            int v;
            while (!to.try_pop(v)) {}
            while (!back.push(v)) {}
        }
    });

    const auto t0 = Clock::now();
    for (int i = 0; i < kRoundTrips; ++i) {
        while (!to.push(i)) {}
        int v;
        while (!back.try_pop(v)) {}
    }
    const auto elapsed = std::chrono::duration_cast<ns>(Clock::now() - t0).count();
    echo.join();
    return static_cast<double>(elapsed) / kRoundTrips;
}

// ---- single-threaded op cost ----------------------------------------------
// No contention at all: the pure cost of the push/pop instructions.

double uncontended_push_pop_ns() {
    spsc::Queue<int> q(kCapacity);
    constexpr int kOps = 5'000'000;
    const auto t0 = Clock::now();
    int sink = 0;
    for (int i = 0; i < kOps; ++i) {
        q.push(i);
        int v;
        q.try_pop(v);
        sink += v & 1;
    }
    const auto elapsed = std::chrono::duration_cast<ns>(Clock::now() - t0).count();
    if (sink == -1) std::printf("unreachable\n");
    return static_cast<double>(elapsed) / kOps;
}

template <typename F>
double best_of(F f) {
    std::vector<double> runs;
    runs.reserve(kReps);
    for (int i = 0; i < kReps; ++i) runs.push_back(f());
    return median(runs);
}

}  // namespace

int main() {
    std::printf("spsc::Queue benchmarks\n");
    std::printf("capacity %zu slots, median of %d runs, %d hardware threads\n\n",
                kCapacity, kReps, static_cast<int>(std::thread::hardware_concurrency()));

    const double st = best_of(uncontended_push_pop_ns);
    const double tp = best_of(throughput_spsc_ns_per_item);
    const double mx = best_of(throughput_mutex_ns_per_item);
    const double rt = best_of(roundtrip_spsc_ns);

    std::printf("| Benchmark                                  | ns/op  | items/sec   |\n");
    std::printf("| ------------------------------------------ | ------ | ----------- |\n");
    std::printf("| push+pop, single thread, uncontended       | %6.1f | %11.0f |\n", st, 1e9 / st);
    std::printf("| throughput, 1 producer + 1 consumer        | %6.1f | %11.0f |\n", tp, 1e9 / tp);
    std::printf("| throughput, std::mutex + std::deque        | %6.1f | %11.0f |\n", mx, 1e9 / mx);
    std::printf("| round-trip latency (ping-pong, 2 queues)   | %6.1f | %11s |\n", rt, "-");
    std::printf("\nlock-free is %.1fx the mutex baseline on throughput\n", mx / tp);
    return 0;
}
