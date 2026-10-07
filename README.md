# spsc-queue

A wait-free single-producer / single-consumer ring buffer in C++20. Header-only, no dependencies,
~190 lines.

There are a lot of SPSC queues on GitHub. Most are either unbenchmarked, or benchmarked without the
method stated, or quietly wrong about memory ordering. This one ships the evidence: a
ThreadSanitizer run in CI that would fail if the acquire/release pairing were wrong, a benchmark
harness you can rerun yourself, and the reasoning for every design decision written down in the
header.

Built solo with heavy AI assistance (Claude Code). The design decisions, the benchmark methodology
and the bugs found along the way are mine, and the commit history shows the work.

## Use it

```cpp
#include "spsc/queue.hpp"

spsc::Queue<int> q(1024);   // rounded up to a power of two

// producer thread
if (!q.push(42)) { /* full — this never blocks */ }

// consumer thread
int v;
if (q.try_pop(v)) { /* got it */ }
```

Header-only, so copying `include/spsc/queue.hpp` into your project is a valid install. With CMake:

```cmake
add_subdirectory(spsc-queue)
target_link_libraries(your_target PRIVATE spsc_queue)
```

## Benchmarks

Apple M1, 8 hardware threads, Apple clang 16, `-O2`, 1024-slot queue. Median of 9 runs after a
discarded warm-up; min and max given so you can see the noise rather than take the median on trust.

| Benchmark | median ns/op | min | max |
| --- | ---: | ---: | ---: |
| push + pop, single thread, uncontended | **1.4** | 1.3 | 2.4 |
| throughput, 1 producer + 1 consumer | **21.8** | 17.5 | 29.5 |
| throughput, index caching disabled | 24.7 | 24.1 | 25.2 |
| throughput, `std::mutex` + `std::deque` | 72.3 | 66.0 | 93.3 |
| round-trip latency (ping-pong, two queues) | 164.2 | 161.6 | 171.1 |

About **46 M items/sec** on the threaded throughput benchmark.

**On the mutex comparison, there is no number I am willing to publish.** The earlier version of
this section quoted **2.4× to 3.3×**, which was already a retreat from a wrong 4.3×. Re-measured
across thirteen runs on the same laptop, the mutex baseline alone ranged from **51 ns to 125 ns**
— a 2.4× swing in the denominator — and the resulting multiplier ranged from 1.8× to 5.2×. So the
stated range was not just too narrow, it did not contain most of what the machine actually does.

The lock-free side is the stable one: 19–24 ns per item across the same runs. The mutex side is
not stable, and any multiplier is therefore a property of the scheduler rather than of either
queue. The defensible statement is the two distributions, which is what the table above gives.

**Index caching is worth about 1.13×** (24.7 → 21.8 ns). Settled runs put it between 1.10× and
1.28×. Under load it has measured as low as **0.61×**, i.e. the optimisation appearing actively
harmful — which it is not, and which is a property of the measurement, not the code. That one
reading is why `bench` now refuses to print any ratio when the load average exceeds 35% of core
count, or when the relative spread of any benchmark exceeds 50%. The medians still print so you
can see the state of the machine; the multipliers are withheld and the binary exits non-zero.

That guard matters more than the numbers it withholds. Everything wrong with the first version of
this benchmark was a confident figure sitting on an unexamined baseline, and the harness had no
way to say so.

Reproduce:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/spsc_bench
```

Threads are not pinned to cores. On a machine with performance and efficiency clusters that adds
real variance, which is why the spread is published alongside the median. Numbers are comparable
within a run, not across machines — **and not on a busy one.** Run it on an idle machine or the
ratios will be withheld, by design.

## What the first benchmark got wrong

The first version of this README claimed **4.3× the mutex baseline** and said index caching
"roughly halves throughput". Both were wrong, and they were wrong in the flattering direction, which
is the direction to be suspicious of.

Three mistakes, all in the harness rather than the queue:

1. **No warm-up.** The first timed run paid for cold caches, cold branch predictors and a scheduler
   that had not yet settled the threads onto cores. It reported the mutex baseline at 89 ns/item,
   and I treated a later 55–72 ns reading as the steady state. Thirteen further runs show there is
   no steady state to find: that baseline spans 51–125 ns. The warm-up was a real bug, but it was
   not the reason the number moved.
2. **Too few repetitions.** Five runs with one bad outlier moves the median. Nine runs plus a
   discarded warm-up does not.
3. **An unmeasured claim stated as fact.** "Roughly halves throughput" was my intuition about what
   the index caching was doing, written into the README without ever being measured. When I added
   the `CacheIndices` template parameter so the claim could actually be tested, it came out at
   1.13×. The intuition was directionally right and quantitatively nonsense.

The third one is the one worth sitting with. The first two produce noisy numbers; the third produces
a confident number with nothing behind it, and no amount of rerunning the benchmark would have
caught it. The fix was structural — make the thing switchable so it can be measured — not more
careful timing.

This is also why the queue carries a `CacheIndices` template parameter that you should never set to
`false` in real use. It exists so the optimisation can be falsified.

## Why it is fast

Three decisions carry it, and the third is the one most implementations miss.

**Power-of-two capacity.** The wrap is `& mask_`, not `%`. Integer division costs 20–40 cycles; the
mask costs one. The constructor rounds capacity up with `std::bit_ceil`.

**Cache-line padding.** `head_` and `tail_` sit on separate cache lines. Without the padding, the
producer's store to `tail_` invalidates the consumer's line holding `head_` on *every* push. That
false sharing costs more than the queue operation it is part of.

**Each side caches the other's index.** The producer keeps a private copy of the consumer's `head_`
and only re-reads the real one when its cached copy suggests the queue is full. In the steady state
— queue not near-full, not near-empty — the producer never touches the consumer's cache line at all,
and vice versa. Measured, this is worth **1.13×** (24.7 → 21.8 ns/item); build with
`spsc::Queue<T, false>` to check that for yourself. I expected it to be worth far more, and was
wrong — see "What the first benchmark got wrong".

## Memory ordering

Release on publish, acquire on observe.

Sequential consistency is not required here. There is exactly one producer and one consumer, so
there is no multi-variable ordering for a third thread to disagree about — `seq_cst` would only buy
a full barrier nobody reads. The release/acquire pair is what guarantees that when the consumer
observes the new `tail_`, the slot write that happened before it is visible.

This is the kind of claim that is easy to make and hard to trust, so CI runs the 500,000-item
two-thread soak test under ThreadSanitizer on every push. A missing or weakened ordering is reported
there even though the single-threaded tests would still pass.

## What this is not

- **Not multi-producer or multi-consumer.** Two producers is undefined behaviour and will corrupt
  silently. If you need MPMC, you want a different data structure, not this one with a lock around it.
- **Not resizable.** Capacity is fixed at construction.
- **Never blocks.** `push` returns `false` when full and `try_pop` returns `false` when empty.
  Back-off is the caller's problem, deliberately — blocking policy does not belong in the queue.
- **One slot is reserved** so that empty and full stay distinguishable, so usable capacity is
  `capacity() - 1`.

## Tests

Ten tests covering capacity rounding, FIFO order, the full and empty edges, repeated wrap-around,
element lifetime (every constructed element destroyed exactly once, including those still queued at
destruction), non-trivial types, and the threaded soak.

```sh
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure

# and the one that matters
cmake -B build-tsan -DSPSC_TSAN=ON -DSPSC_BUILD_BENCH=OFF && cmake --build build-tsan && ./build-tsan/spsc_tests
```

## Licence

MIT.
