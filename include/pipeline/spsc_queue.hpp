// Lock-free single-producer single-consumer ring buffer.
//
// Fixed power-of-two capacity, monotonically increasing 64-bit indices (no
// wrap-around ambiguity), acquire/release ordering only, no CAS.
//
// Padded = true puts the producer's state (tail + its cached copy of head), the
// consumer's state (head + cached tail) and the slots on separate cache lines,
// so the two cores don't invalidate each other's line on every operation
// (false sharing). CacheIndices = true lets each side keep a private copy of
// the other side's index and re-read the shared atomic only when the queue
// looks full (producer) or empty (consumer). Both are on by default; turning
// them off is for the ablation in pipeline_bench.
//
// HeadBatch > 1 makes the consumer publish its read position only every
// HeadBatch pops (or whenever it finds the queue empty). Without it, a full
// queue makes the producer re-read `head` after every single pop, bouncing
// that cache line between the cores once per message. The producer just sees
// slightly less free space than there really is; latency is unaffected because
// the producer still publishes every push immediately.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pipeline {

// Apple Silicon uses 128-byte lines; Intel's adjacent-line prefetcher also pulls
// pairs of 64-byte lines, so 128 is the safe padding on both.
inline constexpr std::size_t kCacheLine = 128;

template <class T, std::size_t N, bool Padded = true, bool CacheIndices = true, std::size_t HeadBatch = 32>
class SpscQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(HeadBatch >= 1 && HeadBatch <= N / 2);
    static constexpr std::size_t kAlign = Padded ? kCacheLine : alignof(std::atomic<std::uint64_t>);

public:
    static constexpr std::size_t capacity = N;

    // Producer only.
    bool try_push(const T& v) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if constexpr (CacheIndices) {
            if (t - head_cache_ >= N) {
                head_cache_ = head_.load(std::memory_order_acquire);
                if (t - head_cache_ >= N) return false;
            }
        } else {
            if (t - head_.load(std::memory_order_acquire) >= N) return false;
        }
        slots_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer only.
    bool try_pop(T& out) noexcept {
        const std::uint64_t h = head_local_;
        if constexpr (CacheIndices) {
            if (h == tail_cache_) {
                publish_head(h);  // about to wait: let the producer see all freed slots
                tail_cache_ = tail_.load(std::memory_order_acquire);
                if (h == tail_cache_) return false;
            }
        } else {
            if (h == tail_.load(std::memory_order_acquire)) {
                publish_head(h);
                return false;
            }
        }
        out = slots_[h & (N - 1)];
        head_local_ = h + 1;
        if (head_local_ - head_published_ >= HeadBatch) publish_head(head_local_);
        return true;
    }

    // Approximate; for monitoring only.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return static_cast<std::size_t>(tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire));
    }

private:
    void publish_head(std::uint64_t h) noexcept {
        if (h != head_published_) {
            head_.store(h, std::memory_order_release);
            head_published_ = h;
        }
    }

    // Producer-owned line.
    alignas(kAlign) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t head_cache_ = 0;
    // Consumer-owned line.
    alignas(kAlign) std::atomic<std::uint64_t> head_{0};
    std::uint64_t tail_cache_ = 0;
    std::uint64_t head_local_ = 0;      // next slot to read
    std::uint64_t head_published_ = 0;  // last value stored to head_
    // Slots start on their own line.
    alignas(kAlign) T slots_[N]{};
};

}  // namespace pipeline
