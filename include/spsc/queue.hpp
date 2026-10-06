// spsc::Queue — a wait-free single-producer / single-consumer ring buffer.
//
// Built solo with heavy AI assistance (Claude Code). Every design decision below
// is explained because the point of this repository is that the reasoning is
// legible, not just the result.
//
// Three decisions carry the performance, and all three are worth understanding
// before using this:
//
//   1. Capacity is rounded up to a power of two so the wrap is a bitmask (`& mask_`)
//      rather than `%`. Integer division is ~20-40 cycles; the mask is one.
//
//   2. head_ and tail_ sit on separate cache lines. Without the padding the
//      producer's store to tail_ invalidates the consumer's cache line holding
//      head_ on every single push — false sharing, and it costs more than the
//      queue operation itself.
//
//   3. Each side caches the *other* side's index. This is the real win and the
//      part most implementations miss. The producer only re-reads the consumer's
//      head_ when its cached copy says the queue looks full. In the steady state
//      where the queue is not near-full, the producer never touches the
//      consumer's cache line at all.
//
// Memory ordering: release on publish, acquire on observe. Sequential
// consistency is not needed here — there is exactly one producer and one
// consumer, so there is no multi-variable ordering for a third thread to
// observe. The release/acquire pair is what guarantees that when the consumer
// sees the new tail_, the slot write that preceded it is visible.
//
// What this is NOT: not multi-producer, not multi-consumer, not resizable, and
// it never blocks. Using it from two producers is undefined behaviour and will
// corrupt silently.

#pragma once

#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace spsc {

// std::hardware_destructive_interference_size is the right answer but libc++
// does not ship it. 64 is correct for x86-64 and for Apple silicon's 128-byte
// pairs it is merely conservative, which is the safe direction to be wrong in.
inline constexpr std::size_t kCacheLine = 64;

// CacheIndices exists so the third optimisation above can be switched off and
// measured rather than asserted. Leave it true in real use; the benchmark
// instantiates a false version to report what the caching is actually worth.
template <typename T, bool CacheIndices = true>
class Queue {
    static_assert(std::is_nothrow_destructible_v<T>,
                  "T must be nothrow-destructible: the ring destroys elements during unwind");

public:
    // capacity is rounded UP to a power of two. One slot is reserved to keep
    // empty (head == tail) distinguishable from full, so usable space is
    // capacity() - 1.
    explicit Queue(std::size_t capacity)
        : slots_(static_cast<std::size_t>(std::bit_ceil(capacity < 2 ? 2 : capacity))),
          mask_(slots_ - 1),
          buffer_(static_cast<T*>(::operator new[](sizeof(T) * slots_, std::align_val_t{alignof(T)}))) {}

    ~Queue() {
        // Drain whatever is still queued so element destructors run.
        while (auto* p = front()) {
            p->~T();
            pop_slot();
        }
        ::operator delete[](buffer_, std::align_val_t{alignof(T)});
    }

    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    // ---- producer side ----------------------------------------------------

    // Returns false if the queue is full. Never blocks, never allocates.
    template <typename... Args>
    bool emplace(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t next = (tail + 1) & mask_;

        if constexpr (CacheIndices) {
            // Fast path: trust the cached head. Only if it says we are full do
            // we pay for a load of the consumer's real head_.
            if (next == cached_head_) {
                cached_head_ = head_.load(std::memory_order_acquire);
                if (next == cached_head_) return false;  // genuinely full
            }
        } else {
            // Uncached: load the consumer's line on every single push.
            if (next == head_.load(std::memory_order_acquire)) return false;
        }

        ::new (buffer_ + tail) T(std::forward<Args>(args)...);
        // Release: the slot construction above must be visible to the consumer
        // before it can observe this new tail.
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) { return emplace(v); }
    bool push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) { return emplace(std::move(v)); }

    // ---- consumer side ----------------------------------------------------

    // Pointer to the oldest element, or nullptr when empty. Valid until pop().
    T* front() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if constexpr (CacheIndices) {
            if (head == cached_tail_) {
                cached_tail_ = tail_.load(std::memory_order_acquire);
                if (head == cached_tail_) return nullptr;  // genuinely empty
            }
        } else {
            if (head == tail_.load(std::memory_order_acquire)) return nullptr;
        }
        return buffer_ + head;
    }

    // Destroys the front element. Undefined if the queue is empty.
    void pop() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // cached_tail_ is only meaningful when the caching is enabled; with it
        // off the authoritative tail_ is the only thing to check against.
        if constexpr (CacheIndices) {
            assert(head != cached_tail_ && "pop() on an empty queue");
        } else {
            assert(head != tail_.load(std::memory_order_acquire) && "pop() on an empty queue");
        }
        buffer_[head].~T();
        pop_slot();
    }

    bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
        T* p = front();
        if (!p) return false;
        out = std::move(*p);
        p->~T();
        pop_slot();
        return true;
    }

    // ---- observers --------------------------------------------------------
    // Both are approximate under concurrency: the other side may move the
    // moment you read it. Use them for metrics, never for control flow.

    std::size_t capacity() const noexcept { return slots_; }

    std::size_t size_approx() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return (t - h) & mask_;
    }

    bool empty_approx() const noexcept { return size_approx() == 0; }

private:
    void pop_slot() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        // Release: frees the slot for the producer. Pairs with its acquire load.
        head_.store((head + 1) & mask_, std::memory_order_release);
    }

    const std::size_t slots_;
    const std::size_t mask_;
    T* const buffer_;

    // The padding is the whole point: head_ and tail_ must never share a line.
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    // Consumer-private cache of tail_, parked on the consumer's line.
    alignas(kCacheLine) std::size_t cached_tail_{0};

    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    // Producer-private cache of head_, parked on the producer's line.
    alignas(kCacheLine) std::size_t cached_head_{0};

    // Trailing pad so a neighbouring object cannot land on tail_'s line.
    char pad_[kCacheLine]{};
};

}  // namespace spsc
