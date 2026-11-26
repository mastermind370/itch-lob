#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "engine/diff.hpp"
#include "engine/fast_engine.hpp"
#include "engine/ref_engine.hpp"

using namespace engine;

namespace {

// Runs the same scripted test on both engines.
template <class E>
std::unique_ptr<E> make() { return std::make_unique<E>(1000, 1024); }

template <class E>
void price_time_priority() {
    auto e = make<E>();
    VectorSink s;
    e->limit(1, Side::Sell, 101, 100, s);
    e->limit(2, Side::Sell, 100, 50, s);   // better price, later
    e->limit(3, Side::Sell, 100, 70, s);   // same price, later still
    e->limit(4, Side::Sell, 102, 10, s);
    const Result r = e->limit(9, Side::Buy, 101, 200, s);
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.filled, 200u);
    EXPECT_EQ(r.rested, 0u);
    // Best price first, then oldest first at the same price; trades at the resting price.
    ASSERT_EQ(s.fills.size(), 3u);
    EXPECT_TRUE(s.fills[0] == (Fill{9, 2, 100, 50}));
    EXPECT_TRUE(s.fills[1] == (Fill{9, 3, 100, 70}));
    EXPECT_TRUE(s.fills[2] == (Fill{9, 1, 101, 80}));
    EXPECT_TRUE(e->best_ask() == (Quote{101, 20}));
    EXPECT_EQ(e->best_bid().price, 0u);
}

template <class E>
void remainder_rests_at_back_of_queue() {
    auto e = make<E>();
    VectorSink s;
    e->limit(1, Side::Buy, 50, 10, s);
    e->limit(2, Side::Sell, 49, 30, s);  // fills 10 at 50, rests 20 at 49
    EXPECT_EQ(s.fills.size(), 1u);
    EXPECT_TRUE(e->best_ask() == (Quote{49, 20}));
    e->limit(3, Side::Sell, 49, 5, s);
    const auto snap = e->snapshot(Side::Sell);
    ASSERT_EQ(snap.size(), 1u);
    ASSERT_EQ(snap[0].queue.size(), 2u);
    EXPECT_EQ(snap[0].queue[0].first, 2u);
    EXPECT_EQ(snap[0].queue[1].first, 3u);
}

template <class E>
void market_sweeps_and_never_rests() {
    auto e = make<E>();
    VectorSink s;
    e->limit(1, Side::Buy, 10, 5, s);
    e->limit(2, Side::Buy, 9, 5, s);
    e->limit(3, Side::Buy, 8, 5, s);
    const Result r = e->market(4, Side::Sell, 12, s);
    EXPECT_EQ(r.filled, 12u);
    EXPECT_EQ(s.fills.size(), 3u);
    EXPECT_TRUE(e->best_bid() == (Quote{8, 3}));
    const Result r2 = e->market(5, Side::Sell, 100, s);  // only 3 left; the rest is cancelled
    EXPECT_EQ(r2.filled, 3u);
    EXPECT_EQ(r2.rested, 0u);
    EXPECT_EQ(e->best_bid().price, 0u);
    EXPECT_EQ(e->best_ask().price, 0u);
    EXPECT_EQ(e->market(6, Side::Buy, 10, s).filled, 0u);  // empty book
}

template <class E>
void cancel_from_middle_of_queue() {
    auto e = make<E>();
    VectorSink s;
    for (OrderId id = 1; id <= 4; ++id) e->limit(id, Side::Buy, 20, 10, s);
    EXPECT_EQ(e->cancel(2), Status::Ok);
    EXPECT_EQ(e->cancel(2), Status::UnknownId);
    EXPECT_EQ(e->cancel(4), Status::Ok);  // tail
    EXPECT_EQ(e->cancel(1), Status::Ok);  // head
    EXPECT_TRUE(e->best_bid() == (Quote{20, 10}));
    e->market(9, Side::Sell, 10, s);
    ASSERT_EQ(s.fills.size(), 1u);
    EXPECT_EQ(s.fills[0].maker, 3u);
    EXPECT_EQ(e->order_count(), 0u);
}

template <class E>
void rejects() {
    auto e = make<E>();
    VectorSink s;
    EXPECT_EQ(e->limit(1, Side::Buy, 0, 10, s).status, Status::BadPrice);
    EXPECT_EQ(e->limit(1, Side::Buy, 1000, 10, s).status, Status::BadPrice);  // band is [1, 1000)
    EXPECT_EQ(e->limit(1, Side::Buy, 10, 0, s).status, Status::BadQty);
    EXPECT_EQ(e->limit(1, Side::Buy, 10, 5, s).status, Status::Ok);
    EXPECT_EQ(e->limit(1, Side::Buy, 11, 5, s).status, Status::DuplicateId);
    EXPECT_EQ(e->market(1, Side::Sell, 5, s).status, Status::DuplicateId);
    EXPECT_EQ(e->cancel(77), Status::UnknownId);
    EXPECT_TRUE(s.fills.empty());
}

}  // namespace

TEST(Engine, PriceTimePriorityRef) { price_time_priority<RefEngine>(); }
TEST(Engine, PriceTimePriorityFast) { price_time_priority<FastEngine>(); }
TEST(Engine, RemainderRestsRef) { remainder_rests_at_back_of_queue<RefEngine>(); }
TEST(Engine, RemainderRestsFast) { remainder_rests_at_back_of_queue<FastEngine>(); }
TEST(Engine, MarketRef) { market_sweeps_and_never_rests<RefEngine>(); }
TEST(Engine, MarketFast) { market_sweeps_and_never_rests<FastEngine>(); }
TEST(Engine, CancelMiddleRef) { cancel_from_middle_of_queue<RefEngine>(); }
TEST(Engine, CancelMiddleFast) { cancel_from_middle_of_queue<FastEngine>(); }
TEST(Engine, RejectsRef) { rejects<RefEngine>(); }
TEST(Engine, RejectsFast) { rejects<FastEngine>(); }

TEST(Engine, FastPoolFullCancelsRemainder) {
    FastEngine e(100, 2);
    VectorSink s;
    EXPECT_EQ(e.limit(1, Side::Buy, 10, 1, s).status, Status::Ok);
    EXPECT_EQ(e.limit(2, Side::Buy, 10, 1, s).status, Status::Ok);
    const Result r = e.limit(3, Side::Buy, 10, 1, s);
    EXPECT_EQ(r.status, Status::Full);
    EXPECT_EQ(r.rested, 0u);
    EXPECT_EQ(e.cancel(1), Status::Ok);  // frees a node
    EXPECT_EQ(e.limit(3, Side::Buy, 10, 1, s).status, Status::Ok);
}

TEST(TwoLevelBits, NextPrevAcrossSummaryWords) {
    TwoLevelBits b(1u << 16);
    for (std::uint32_t i : {3u, 64u, 4095u, 4096u, 40000u, 65535u}) b.set(i);
    EXPECT_EQ(b.next_after(3), 64);
    EXPECT_EQ(b.next_after(64), 4095);
    EXPECT_EQ(b.next_after(4095), 4096);
    EXPECT_EQ(b.next_after(4096), 40000);
    EXPECT_EQ(b.next_after(40000), 65535);
    EXPECT_EQ(b.next_after(65535), -1);
    EXPECT_EQ(b.prev_before(65535), 40000);
    EXPECT_EQ(b.prev_before(40000), 4096);
    EXPECT_EQ(b.prev_before(4096), 4095);
    EXPECT_EQ(b.prev_before(4095), 64);
    EXPECT_EQ(b.prev_before(64), 3);
    EXPECT_EQ(b.prev_before(3), -1);
    b.clear(4095);
    b.clear(4096);
    EXPECT_EQ(b.next_after(64), 40000);
    EXPECT_EQ(b.prev_before(40000), 64);
}

// The headline test: 1M random orders, fast engine vs reference, checked after every order.
TEST(EngineDiff, OneMillionRandomOrders) {
    FlowConfig cfg;
    cfg.seed = 2026;
    const DiffReport r = run_diff(cfg, 1'000'000, 25'000);
    EXPECT_TRUE(r.ok());
    if (!r.ok()) std::printf("first mismatch at op %llu: %s\n", static_cast<unsigned long long>(r.first_bad_op), r.first_error.c_str());
    EXPECT_TRUE(r.fills > 100'000);  // the flow really does match
}

TEST(EngineDiff, OtherSeedsAndShapes) {
    for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
        FlowConfig cfg;
        cfg.seed = seed;
        cfg.target_open = seed * 50;  // thin books: levels empty and best prices jump often
        cfg.cross_ppt = 300;
        cfg.market_ppt = 100;
        const DiffReport r = run_diff(cfg, 200'000, 1'000);
        EXPECT_TRUE(r.ok());
        if (!r.ok()) std::printf("seed %llu: first mismatch at op %llu: %s\n", static_cast<unsigned long long>(seed),
                                 static_cast<unsigned long long>(r.first_bad_op), r.first_error.c_str());
    }
}
