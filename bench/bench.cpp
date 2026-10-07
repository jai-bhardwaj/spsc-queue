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
#include <cstdlib>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using ns = std::chrono::nanoseconds;

// 9 reps, and the first is discarded as a warm-up. This is not pedantry: an
// earlier version used 5 reps with no warm-up and reported the mutex baseline
// at 89 ns/item, making the lock-free queue look 4.3x faster. The real figure
// is ~2.5x. The cold run — cold caches, cold branch predictors, the OS not yet
// settled on a core — was dragging the median. See README, "What the first
// benchmark got wrong".
constexpr int kReps = 9;
// Above this relative spread a median is noise, not a measurement.
constexpr double kSpreadLimit = 0.50;
constexpr int kThroughputItems = 2'000'000;
constexpr int kRoundTrips = 200'000;
constexpr std::size_t kCapacity = 1024;

// ---- lock-free throughput --------------------------------------------------

// Templated on the index-caching flag so the same code path measures both
// variants. The README claims the caching is worth something; this is what
// makes that claim checkable instead of asserted.
template <bool CacheIndices>
double throughput_ns_per_item() {
    spsc::Queue<int, CacheIndices> q(kCapacity);
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

double throughput_spsc_ns_per_item() { return throughput_ns_per_item<true>(); }
double throughput_uncached_ns_per_item() { return throughput_ns_per_item<false>(); }

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

// Reports the spread alongside the median, so a reader can see for themselves
// how noisy the machine was rather than trusting a single number.
template <typename F>
void measure(const char* label, F f, double& out_median, double& worst_spread) {
    f();  // warm-up
    std::vector<double> runs;
    runs.reserve(kReps);
    for (int i = 0; i < kReps; ++i) runs.push_back(f());
    std::sort(runs.begin(), runs.end());
    out_median = runs[runs.size() / 2];

    // Relative spread is the honest noise signal. A median is only worth
    // quoting when the runs behind it agree; publishing one without this is
    // how the first version of this benchmark reached a confident wrong answer.
    const double spread = (runs.back() - runs.front()) / out_median;
    if (spread > worst_spread) worst_spread = spread;

    std::printf("| %-40s | %6.1f | %5.1f | %5.1f | %5.0f%% |%s\n", label, out_median,
                runs.front(), runs.back(), spread * 100.0,
                spread > kSpreadLimit ? "  <-- NOISY" : "");
}

}  // namespace

int main() {
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    std::printf("spsc::Queue benchmarks\n");
    std::printf("capacity %zu slots, median of %d runs, %d hardware threads\n",
                kCapacity, kReps, cores);

    // A busy machine does not just add noise, it inverts conclusions: measured
    // under load average 16 on an 8-core box, this benchmark reported index
    // caching at 0.61x - the optimisation looking actively harmful. Refuse to
    // publish derived ratios from a machine in that state.
    double la[3] = {0, 0, 0};
    const bool have_load = getloadavg(la, 3) == 3;
    const double load_ratio = cores > 0 ? la[0] / cores : 0.0;
    const bool machine_busy = have_load && load_ratio > 0.35;
    if (have_load) {
        std::printf("load average %.2f over %d cores (%.0f%% busy)%s\n", la[0], cores,
                    load_ratio * 100.0,
                    machine_busy ? "  <-- TOO BUSY TO TRUST" : "");
    }
    std::printf("\n");

    double st = 0, tp = 0, uc = 0, mx = 0, rt = 0;
    double worst_spread = 0.0;

    std::printf("| %-40s | %6s | %5s | %5s | %5s |\n", "Benchmark (ns/op)", "median", "min",
                "max", "sprd");
    std::printf("| %-40s | %6s | %5s | %5s | %5s |\n", "----------------------------------------",
                "------", "-----", "-----", "-----");
    measure("push+pop, single thread, uncontended", uncontended_push_pop_ns, st, worst_spread);
    measure("throughput, 1 producer + 1 consumer", throughput_spsc_ns_per_item, tp, worst_spread);
    measure("throughput, index caching DISABLED", throughput_uncached_ns_per_item, uc, worst_spread);
    measure("throughput, std::mutex + std::deque", throughput_mutex_ns_per_item, mx, worst_spread);
    measure("round-trip latency (ping-pong, 2 queues)", roundtrip_spsc_ns, rt, worst_spread);

    std::printf("\nthroughput: %.0f M items/sec\n", 1e3 / tp);

    // A ratio divides one noisy number by another, so it is the first thing to
    // go wrong and the last thing a reader can check. Print it only when the
    // runs behind it held still.
    const bool trustworthy = !machine_busy && worst_spread <= kSpreadLimit;
    if (trustworthy) {
        std::printf("lock-free is %.2fx the mutex baseline\n", mx / tp);
        std::printf("index caching is worth %.2fx (%.1f ns -> %.1f ns per item)\n", uc / tp, uc,
                    tp);
    } else {
        std::printf("\nRATIOS WITHHELD. ");
        if (machine_busy) {
            std::printf("Load average %.2f on %d cores is too high. ", la[0], cores);
        }
        if (worst_spread > kSpreadLimit) {
            std::printf("Worst relative spread was %.0f%%, over the %.0f%% limit. ", worst_spread * 100.0,
                        kSpreadLimit * 100.0);
        }
        std::printf("\nThe per-benchmark medians above are still printed so you can see the\n"
                    "state of the machine, but any multiplier derived from them would be a\n"
                    "property of the scheduler rather than of the queue. Re-run on an idle\n"
                    "machine. Measured under load, this benchmark has reported index caching\n"
                    "at 0.61x, i.e. the optimisation looking harmful, which it is not.\n");
        return 1;
    }
    return 0;
}
