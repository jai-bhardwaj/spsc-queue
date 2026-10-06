# spsc-queue

A wait-free single-producer / single-consumer ring buffer in C++20. Header-only, no dependencies,
~170 lines.

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

Apple M1, 8 hardware threads, Apple clang 16, `-O2`, 1024-slot queue, median of 5 runs.

| Benchmark | ns/op | items/sec |
| --- | ---: | ---: |
| push + pop, single thread, uncontended | **2.1** | 479,754,366 |
| throughput, 1 producer + 1 consumer | **20.6** | 48,647,203 |
| throughput, `std::mutex` + `std::deque` | 89.1 | 11,221,796 |
| round-trip latency (ping-pong, two queues) | 168.0 | — |

**4.3× the mutex baseline on threaded throughput.**

Reproduce:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/spsc_bench
```

Threads are not pinned to cores, which on a machine with performance and efficiency clusters adds
real variance — that is why the median is reported rather than the minimum. Numbers are comparable
within a run, not across machines.

## Why it is fast

Three decisions carry it, and the third is the one most implementations miss.

**Power-of-two capacity.** The wrap is `& mask_`, not `%`. Integer division costs 20–40 cycles; the
mask costs one. The constructor rounds capacity up with `std::bit_ceil`.

**Cache-line padding.** `head_` and `tail_` sit on separate cache lines. Without the padding, the
producer's store to `tail_` invalidates the consumer's line holding `head_` on *every* push. That
false sharing costs more than the queue operation it is part of.

**Each side caches the other's index.** This is the real win. The producer keeps a private copy of
the consumer's `head_` and only re-reads the real one when its cached copy suggests the queue is
full. In the steady state — queue not near-full, not near-empty — the producer never touches the
consumer's cache line at all, and vice versa. Removing this one optimisation roughly halves
throughput.

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
