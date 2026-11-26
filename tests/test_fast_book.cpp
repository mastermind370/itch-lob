#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "itch/parser.hpp"
#include "itch/synthetic.hpp"
#include "lob/book_builder.hpp"
#include "lob/checker.hpp"
#include "lob/fast_book.hpp"
#include "lob/map_book.hpp"
#include "lob/order_map.hpp"

using namespace itch;
using lob::FastBook;
using lob::FlatOrderMap;
using lob::MapBook;
using lob::RestingOrder;

TEST(FlatOrderMap, InsertFindErase) {
    FlatOrderMap m(1024);
    EXPECT_TRUE(m.insert(7, RestingOrder{1, Side::Buy, 100, 50}));
    EXPECT_FALSE(m.insert(7, RestingOrder{1, Side::Buy, 100, 50}));
    ASSERT_TRUE(m.find(7) != nullptr);
    EXPECT_EQ(m.find(7)->shares, 50u);
    m.find(7)->shares = 20;
    EXPECT_EQ(m.find(7)->shares, 20u);
    EXPECT_TRUE(m.find(8) == nullptr);
    m.erase(7);
    EXPECT_TRUE(m.find(7) == nullptr);
    EXPECT_EQ(m.size(), 0u);
    m.erase(12345);  // erasing a missing key is a no-op
}

// Random ops against std::unordered_map, in a small table so probe runs are long,
// wrap around the end, and deletions shift entries back across the wrap.
TEST(FlatOrderMap, MatchesUnorderedMapUnderChurn) {
    FlatOrderMap m(1024);
    std::unordered_map<std::uint64_t, std::uint32_t> ref;
    SplitMix64 rng(1);
    for (int i = 0; i < 400'000; ++i) {
        const std::uint64_t k = rng.below(1500);
        const std::uint64_t op = rng.below(3);
        if (op == 0) {
            const bool a = m.insert(k, RestingOrder{0, Side::Sell, 1, static_cast<std::uint32_t>(i)});
            const bool b = ref.emplace(k, static_cast<std::uint32_t>(i)).second;
            ASSERT_EQ(a, b);
        } else if (op == 1) {
            m.erase(k);
            ref.erase(k);
        } else {
            const RestingOrder* p = m.find(k);
            auto it = ref.find(k);
            ASSERT_EQ(p != nullptr, it != ref.end());
            if (p) ASSERT_EQ(p->shares, it->second);
        }
        ASSERT_EQ(m.size(), ref.size());
    }
    std::size_t seen = 0;
    m.for_each([&](std::uint64_t k, const RestingOrder& o) {
        ++seen;
        EXPECT_EQ(ref.at(k), o.shares);
    });
    EXPECT_EQ(seen, ref.size());
}

TEST(FlatOrderMap, GrowsPastCapacity) {
    FlatOrderMap m(1024);
    for (std::uint64_t k = 1; k <= 5000; ++k) ASSERT_TRUE(m.insert(k, RestingOrder{0, Side::Buy, 1, 1}));
    EXPECT_TRUE(m.grows() > 0);
    EXPECT_TRUE(m.capacity() >= 8192u);
    for (std::uint64_t k = 1; k <= 5000; ++k) ASSERT_TRUE(m.find(k) != nullptr);
}

TEST(FastBook, BestPricesAcrossWordBoundaries) {
    FastBook b;
    // $50.00 anchors the window; levels are 1 cent apart and span many 64-level words.
    b.add(Side::Buy, 500'000, 10);
    b.add(Side::Buy, 490'000, 20);   // 100 ticks lower
    b.add(Side::Buy, 436'100, 30);   // 639 ticks lower
    b.add(Side::Sell, 500'100, 5);
    b.add(Side::Sell, 512'800, 6);   // 127 ticks higher
    EXPECT_EQ(b.best_bid().price, 500'000u);
    EXPECT_EQ(b.best_ask().price, 500'100u);
    b.reduce(Side::Buy, 500'000, 10, true);
    EXPECT_EQ(b.best_bid().price, 490'000u);
    b.reduce(Side::Buy, 490'000, 20, true);
    EXPECT_EQ(b.best_bid().price, 436'100u);
    EXPECT_EQ(b.best_bid().shares, 30u);
    b.reduce(Side::Buy, 436'100, 30, true);
    EXPECT_EQ(b.best_bid().price, 0u);
    b.reduce(Side::Sell, 500'100, 5, true);
    EXPECT_EQ(b.best_ask().price, 512'800u);
    EXPECT_EQ(b.level_count(Side::Sell), 1u);
}

TEST(FastBook, OverflowOutsideWindowAndOffGrid) {
    FastBook b;
    b.add(Side::Buy, 1'000'000, 100);  // $100.00 anchors: window ~ $89.76 .. $110.23
    b.add(Side::Buy, 2'000'000, 1);    // far above the window -> overflow, and now the best bid
    b.add(Side::Buy, 999'950, 7);      // sub-penny -> overflow
    b.add(Side::Sell, 5'000'000, 3);   // overflow ask
    EXPECT_TRUE(b.overflow_adds() == 3u);
    EXPECT_EQ(b.best_bid().price, 2'000'000u);
    EXPECT_EQ(b.best_ask().price, 5'000'000u);
    EXPECT_EQ(b.level(Side::Buy, 999'950).shares, 7u);
    EXPECT_EQ(b.level_count(Side::Buy), 3u);
    b.reduce(Side::Buy, 2'000'000, 1, true);
    EXPECT_EQ(b.best_bid().price, 1'000'000u);
    b.add(Side::Sell, 1'000'100, 4);  // in-window ask beats the overflow ask
    EXPECT_EQ(b.best_ask().price, 1'000'100u);
}

TEST(FastBook, SubDollarStockUsesFinerTicks) {
    FastBook b;
    b.add(Side::Buy, 5'000, 10);  // $0.5000
    b.add(Side::Buy, 4'999, 10);  // $0.4999: on the grid for sub-dollar
    EXPECT_EQ(b.overflow_adds(), 0u);
    EXPECT_EQ(b.best_bid().price, 5'000u);
}

// Same random level operations on MapBook and FastBook; every query must agree.
TEST(FastBook, MatchesMapBookOnRandomOps) {
    MapBook ref;
    FastBook fast;
    SplitMix64 rng(3);
    struct O { Side s; Price p; std::uint32_t q; };
    std::vector<O> live;
    for (int i = 0; i < 300'000; ++i) {
        if (live.empty() || rng.below(2)) {
            const Side s = rng.below(2) ? Side::Buy : Side::Sell;
            // Mostly near $20 on the penny grid; some far away, some sub-penny.
            Price p = 200'000 + static_cast<Price>(rng.below(400)) * 100 - 20'000;
            if (rng.below(50) == 0) p = 100'000 + static_cast<Price>(rng.below(4'000'000));
            const std::uint32_t q = 1 + static_cast<std::uint32_t>(rng.below(500));
            ref.add(s, p, q);
            fast.add(s, p, q);
            live.push_back({s, p, q});
        } else {
            const std::size_t k = rng.below(live.size());
            O& o = live[k];
            const std::uint32_t q = 1 + static_cast<std::uint32_t>(rng.below(o.q));
            const bool gone = q == o.q;
            ref.reduce(o.s, o.p, q, gone);
            fast.reduce(o.s, o.p, q, gone);
            o.q -= q;
            if (gone) { live[k] = live.back(); live.pop_back(); }
        }
        ASSERT_EQ(fast.best_bid().price, ref.best_bid().price);
        ASSERT_EQ(fast.best_bid().shares, ref.best_bid().shares);
        ASSERT_EQ(fast.best_ask().price, ref.best_ask().price);
        ASSERT_EQ(fast.best_ask().shares, ref.best_ask().shares);
    }
    for (Side s : {Side::Buy, Side::Sell}) {
        EXPECT_EQ(fast.level_count(s), ref.level_count(s));
        std::size_t n = 0;
        fast.for_each_level(s, [&](Price p, const lob::Level& l) {
            ++n;
            EXPECT_EQ(l.shares, ref.level(s, p).shares);
            EXPECT_EQ(l.orders, ref.level(s, p).orders);
        });
        EXPECT_EQ(n, ref.level_count(s));
    }
}

TEST(Checker, FastBookPassesOnSyntheticFeed) {
    SyntheticConfig cfg;
    cfg.messages = 300'000;
    cfg.stocks = 32;
    const auto buf = SyntheticFeed::generate(cfg);
    auto b = std::make_unique<lob::BookBuilder<FastBook, FlatOrderMap>>(std::size_t{1} << 16);
    lob::CheckedReplay<FastBook, FlatOrderMap> chk(*b);
    ASSERT_EQ(parse_frames(buf.data(), buf.size(), chk).status, ParseStatus::Ok);
    chk.full_audit();
    EXPECT_TRUE(chk.ok());
    EXPECT_EQ(chk.stats().best_mismatches, 0u);
    EXPECT_TRUE(chk.stats().best_checks >= cfg.messages / 2);
    std::uint64_t overflow = 0;
    for (std::size_t i = 0; i < lob::kMaxLocates; ++i) overflow += b->book(static_cast<std::uint16_t>(i)).overflow_adds();
    EXPECT_EQ(overflow, 0u);  // synthetic prices are penny-aligned and within +/- $0.50 of the mid
}

// A fast book that forgets to update its best on removal must be caught by the best-quote check.
namespace {
struct StaleBestBook : FastBook {
    void reduce(Side s, Price p, std::uint32_t q, bool gone) {
        if (gone && s == Side::Buy && best_bid().price == p && level(s, p).orders == 1) {
            FastBook::reduce(s, p, q > 1 ? q - 1 : 0, false);  // leaves a phantom level behind
            return;
        }
        FastBook::reduce(s, p, q, gone);
    }
};
}  // namespace

TEST(Checker, CatchesStaleBestBid) {
    SyntheticConfig cfg;
    cfg.messages = 20'000;
    const auto buf = SyntheticFeed::generate(cfg);
    auto b = std::make_unique<lob::BookBuilder<StaleBestBook, FlatOrderMap>>(std::size_t{1} << 16);
    lob::CheckedReplay<StaleBestBook, FlatOrderMap> chk(*b);
    (void)parse_frames(buf.data(), buf.size(), chk);
    EXPECT_FALSE(chk.ok());
    EXPECT_TRUE(chk.stats().best_mismatches > 0);
}

// Pre-market: the first order is a $1.00 stub bid on a ~$50 stock. Without
// recentering, everything near the real touch would live in the overflow map.
TEST(FastBook, RecentersOnTouchAfterStubAnchor) {
    MapBook ref;
    FastBook fast;
    ref.add(Side::Buy, 10'000, 100);
    fast.add(Side::Buy, 10'000, 100);
    SplitMix64 rng(9);
    struct O { Side s; Price p; std::uint32_t q; };
    std::vector<O> live;
    for (int i = 0; i < 200'000; ++i) {
        if (live.size() < 500 || rng.below(2)) {
            const Side s = rng.below(2) ? Side::Buy : Side::Sell;
            const Price off = static_cast<Price>(100 * (1 + rng.below(30)));
            const Price p = s == Side::Buy ? 500'000 - off : 500'000 + off;
            ref.add(s, p, 100);
            fast.add(s, p, 100);
            live.push_back({s, p, 100});
        } else {
            const std::size_t k = rng.below(live.size());
            ref.reduce(live[k].s, live[k].p, 100, true);
            fast.reduce(live[k].s, live[k].p, 100, true);
            live[k] = live.back();
            live.pop_back();
        }
        ASSERT_EQ(fast.best_bid().price, ref.best_bid().price);
        ASSERT_EQ(fast.best_bid().shares, ref.best_bid().shares);
        ASSERT_EQ(fast.best_ask().price, ref.best_ask().price);
        ASSERT_EQ(fast.best_ask().shares, ref.best_ask().shares);
    }
    EXPECT_EQ(fast.recenters(), 1u);
    EXPECT_TRUE(fast.overflow_adds() <= FastBook::kRecenterAfter);  // only until the first recenter...
    EXPECT_EQ(fast.level(Side::Buy, 10'000).shares, 100u);                // ...and the stub is kept in overflow
    EXPECT_EQ(fast.level_count(Side::Buy), ref.level_count(Side::Buy));
    EXPECT_TRUE(fast.window_low() <= 500'000 && 500'000 <= fast.window_high());
}

// Deep orders far from a touch that is inside the window must not cause recentering.
TEST(FastBook, NoRecenterWhenTouchIsInWindow) {
    FastBook b;
    b.add(Side::Buy, 500'000, 100);
    b.add(Side::Sell, 500'100, 100);
    for (int i = 0; i < 5'000; ++i) b.add(Side::Sell, 9'000'000 + 100 * static_cast<Price>(i % 50), 1);
    EXPECT_EQ(b.recenters(), 0u);
    EXPECT_EQ(b.best_ask().price, 500'100u);
}
