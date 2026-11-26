#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <thread>

#include "itch/synthetic.hpp"
#include "pipeline/pipeline.hpp"
#include "pipeline/spsc_queue.hpp"

using namespace pipeline;

TEST(SpscQueue, FullAndEmptySingleThread) {
    SpscQueue<std::uint64_t, 4, true, true, 1> q;
    std::uint64_t v = 0;
    EXPECT_FALSE(q.try_pop(v));
    for (std::uint64_t i = 1; i <= 4; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(5));
    EXPECT_EQ(q.size_approx(), 4u);
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1u);
    EXPECT_TRUE(q.try_push(5));  // wraps around the ring
    for (std::uint64_t want = 2; want <= 5; ++want) {
        EXPECT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, want);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, PaddingSeparatesProducerAndConsumerState) {
    // Padded: tail line + head line + slots, each on its own 128-byte line.
    EXPECT_TRUE(sizeof(SpscQueue<std::uint64_t, 16, true>) >= 2 * kCacheLine + 16 * 8);
    EXPECT_TRUE(sizeof(SpscQueue<std::uint64_t, 16, false>) < kCacheLine + 16 * 8);
}

namespace {
template <class Q>
void transfer(std::uint64_t n) {
    auto q = std::make_unique<Q>();
    std::uint64_t sum = 0, bad = 0, expect = 0;
    std::thread consumer([&] {
        std::uint64_t v;
        while (expect < n) {
            if (!q->try_pop(v)) continue;
            if (v != expect) ++bad;
            sum += v;
            ++expect;
        }
    });
    for (std::uint64_t i = 0; i < n; ++i)
        while (!q->try_push(i)) {
        }
    consumer.join();
    EXPECT_EQ(bad, 0u);
    EXPECT_EQ(sum, n * (n - 1) / 2);
}
}  // namespace

// Small rings force constant full/empty transitions across threads.
TEST(SpscQueue, TwoThreadsInOrderPadded) { transfer<SpscQueue<std::uint64_t, 8, true, true>>(5'000'000); }
TEST(SpscQueue, TwoThreadsInOrderUnpadded) { transfer<SpscQueue<std::uint64_t, 8, false, true>>(2'000'000); }
TEST(SpscQueue, TwoThreadsInOrderUncached) { transfer<SpscQueue<std::uint64_t, 8, false, false, 1>>(2'000'000); }
TEST(SpscQueue, TwoThreadsInOrderBatchedHead) { transfer<SpscQueue<std::uint64_t, 64, true, true, 32>>(5'000'000); }

// With head batching the producer may see the queue as full early, but never
// loses or reorders anything, and an empty-looking queue always publishes.
TEST(SpscQueue, BatchedHeadSingleThread) {
    SpscQueue<std::uint64_t, 8, true, true, 4> q;
    std::uint64_t v = 0;
    for (std::uint64_t i = 0; i < 8; ++i) EXPECT_TRUE(q.try_push(i));
    EXPECT_FALSE(q.try_push(8));
    EXPECT_TRUE(q.try_pop(v));                 // 1 pop: not yet published
    EXPECT_FALSE(q.try_push(8));               // producer still sees full
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(q.try_pop(v));  // 4 pops: published
    for (std::uint64_t i = 8; i < 12; ++i) EXPECT_TRUE(q.try_push(i));
    for (std::uint64_t want = 4; want < 12; ++want) {
        EXPECT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, want);
    }
    EXPECT_FALSE(q.try_pop(v));                // empty: head published
    EXPECT_EQ(q.size_approx(), 0u);
}

TEST(Pipeline, BuildsSameBooksAsSingleThread) {
    itch::SyntheticConfig cfg;
    cfg.messages = 500'000;
    cfg.stocks = 64;
    const auto buf = itch::SyntheticFeed::generate(cfg);
    const RunStats in = run_inline(buf.data(), buf.size());
    const RunStats p = run_pipeline<SpscQueue<Event, 1024>>(buf.data(), buf.size());
    const RunStats tiny = run_pipeline<SpscQueue<Event, 2, false, false, 1>>(buf.data(), buf.size());
    EXPECT_EQ(p.digest, in.digest);
    EXPECT_EQ(tiny.digest, in.digest);
    EXPECT_EQ(p.messages, in.messages);
    EXPECT_TRUE(p.events > cfg.messages * 9 / 10);  // nearly every message is an order event
}

TEST(Pipeline, DigestDetectsADifferentBook) {
    itch::SyntheticConfig a, b;
    a.messages = b.messages = 50'000;
    b.seed = a.seed + 1;
    const auto fa = itch::SyntheticFeed::generate(a), fb = itch::SyntheticFeed::generate(b);
    EXPECT_TRUE(run_inline(fa.data(), fa.size()).digest != run_inline(fb.data(), fb.size()).digest);
}

TEST(Pipeline, PacedRunRecordsLatency) {
    itch::SyntheticConfig cfg;
    cfg.messages = 100'000;
    const auto buf = itch::SyntheticFeed::generate(cfg);
    bench::Histogram h;
    const std::uint64_t interval = static_cast<std::uint64_t>(500.0 / bench::ns_per_tick());  // 2M msgs/s
    const RunStats s = run_pipeline<SpscQueue<Event, 1024>>(buf.data(), buf.size(), interval, &h);
    EXPECT_EQ(h.count(), s.events);
    EXPECT_EQ(s.digest, run_inline(buf.data(), buf.size()).digest);
    EXPECT_TRUE(s.seconds > 0.04);  // 100K messages at 2M/s take at least 50 ms
}
