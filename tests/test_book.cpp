#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "itch/encoder.hpp"
#include "itch/parser.hpp"
#include "itch/synthetic.hpp"
#include "lob/book_builder.hpp"
#include "lob/checker.hpp"
#include "lob/map_book.hpp"

using namespace itch;
using lob::BookBuilder;
using lob::MapBook;

namespace {

constexpr std::uint16_t L = 5;

std::unique_ptr<BookBuilder<MapBook>> replay(const std::vector<std::uint8_t>& buf) {
    auto b = std::make_unique<BookBuilder<MapBook>>();
    const auto r = parse_frames(buf.data(), buf.size(), *b);
    EXPECT_EQ(r.status, ParseStatus::Ok);
    return b;
}

}  // namespace

TEST(MapBook, BestPricesAndLevels) {
    MapBook b;
    EXPECT_EQ(b.best_bid().price, 0u);
    b.add(Side::Buy, 100, 10);
    b.add(Side::Buy, 101, 5);
    b.add(Side::Buy, 101, 7);
    b.add(Side::Sell, 103, 4);
    b.add(Side::Sell, 102, 9);
    EXPECT_EQ(b.best_bid().price, 101u);
    EXPECT_EQ(b.best_bid().shares, 12u);
    EXPECT_EQ(b.best_ask().price, 102u);
    EXPECT_EQ(b.level(Side::Buy, 101).orders, 2u);
    EXPECT_EQ(b.level_count(Side::Buy), 2u);

    b.reduce(Side::Buy, 101, 5, true);
    EXPECT_EQ(b.best_bid().shares, 7u);
    b.reduce(Side::Buy, 101, 7, true);  // level empties and disappears
    EXPECT_EQ(b.best_bid().price, 100u);
    EXPECT_EQ(b.level_count(Side::Buy), 1u);
}

TEST(BookBuilder, AddExecuteCancelDelete) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.stock_directory(L, 1, "AAPL");
    e.add_order(L, 2, 1, Side::Buy, 300, "AAPL", 1'500'000);
    e.add_order(L, 3, 2, Side::Buy, 200, "AAPL", 1'500'000);
    e.add_order(L, 4, 3, Side::Sell, 100, "AAPL", 1'501'000);
    e.executed(L, 5, 1, 100, 1);  // order 1: 300 -> 200
    e.cancel(L, 6, 2, 50);        // order 2: 200 -> 150
    auto b = replay(buf);
    const auto& bk = b->book(L);
    EXPECT_EQ(bk.best_bid().price, 1'500'000u);
    EXPECT_EQ(bk.best_bid().shares, 350u);
    EXPECT_EQ(bk.level(Side::Buy, 1'500'000).orders, 2u);
    EXPECT_EQ(bk.best_ask().shares, 100u);
    EXPECT_EQ(b->symbol(L), "AAPL");
    EXPECT_EQ(b->locate_of("AAPL"), L);

    buf.clear();
    e.stock_directory(L, 1, "AAPL");
    e.add_order(L, 2, 1, Side::Buy, 300, "AAPL", 1'500'000);
    e.executed(L, 3, 1, 300, 1);  // full fill removes the order
    e.add_order(L, 4, 2, Side::Sell, 100, "AAPL", 1'501'000);
    e.del(L, 5, 2);
    b = replay(buf);
    EXPECT_EQ(b->book(L).best_bid().price, 0u);
    EXPECT_EQ(b->book(L).best_ask().price, 0u);
    EXPECT_TRUE(b->orders().empty());
    EXPECT_EQ(b->anomalies().total(), 0u);
}

TEST(BookBuilder, ReplaceMovesOrderAndKeepsSide) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.add_order(L, 1, 10, Side::Sell, 500, "MSFT", 2'000'000);
    e.replace(L, 2, 10, 11, 300, 1'990'000);
    auto b = replay(buf);
    const auto& bk = b->book(L);
    EXPECT_EQ(bk.level(Side::Sell, 2'000'000).shares, 0u);
    EXPECT_EQ(bk.best_ask().price, 1'990'000u);
    EXPECT_EQ(bk.best_ask().shares, 300u);
    EXPECT_EQ(b->orders().count(10), 0u);
    EXPECT_EQ(b->orders().count(11), 1u);
    EXPECT_EQ(b->touched_count(), 2u);
}

TEST(BookBuilder, StocksAreIndependent) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.add_order(1, 1, 1, Side::Buy, 100, "A", 10'000);
    e.add_order(2, 2, 2, Side::Buy, 200, "B", 20'000);
    auto b = replay(buf);
    EXPECT_EQ(b->book(1).best_bid().shares, 100u);
    EXPECT_EQ(b->book(2).best_bid().shares, 200u);
    EXPECT_EQ(b->book(3).best_bid().price, 0u);
}

TEST(BookBuilder, AnomaliesAreCountedNotFatal) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.del(L, 1, 999);                                      // unknown ref
    e.add_order(L, 2, 1, Side::Buy, 100, "X", 10'000);
    e.add_order(L, 3, 1, Side::Buy, 100, "X", 10'000);     // duplicate ref
    e.executed(L, 4, 1, 150, 1);                           // more than remaining
    auto b = replay(buf);
    EXPECT_EQ(b->anomalies().unknown_ref, 1u);
    EXPECT_EQ(b->anomalies().duplicate_add, 1u);
    EXPECT_EQ(b->anomalies().overfill, 1u);
    EXPECT_EQ(b->book(L).best_bid().price, 0u);  // overfill clamped: order fully removed
}

TEST(Checker, PassesOnSyntheticFeed) {
    SyntheticConfig cfg;
    cfg.messages = 300'000;
    cfg.stocks = 32;
    const auto buf = SyntheticFeed::generate(cfg);
    BookBuilder<MapBook> b;
    lob::CheckedReplay<MapBook> chk(b);
    ASSERT_EQ(parse_frames(buf.data(), buf.size(), chk).status, ParseStatus::Ok);
    chk.full_audit();
    EXPECT_TRUE(chk.ok());
    EXPECT_TRUE(chk.stats().level_checks >= cfg.messages / 2);
    EXPECT_EQ(chk.stats().level_mismatches, 0u);
    EXPECT_EQ(chk.stats().audit_mismatches, 0u);
    EXPECT_TRUE(chk.stats().audit_levels > 0);
    EXPECT_EQ(chk.stats().crossed_checks, 0u);  // generator never crosses
}

// A book that silently drops a share on every add must be caught.
namespace {
struct LeakyBook : MapBook {
    void add(Side s, Price p, std::uint32_t q) { MapBook::add(s, p, q > 1 ? q - 1 : q); }
};
}  // namespace

TEST(Checker, CatchesABrokenBook) {
    SyntheticConfig cfg;
    cfg.messages = 20'000;
    const auto buf = SyntheticFeed::generate(cfg);
    BookBuilder<LeakyBook> b;
    lob::CheckedReplay<LeakyBook> chk(b);
    (void)parse_frames(buf.data(), buf.size(), chk);
    chk.full_audit();
    EXPECT_FALSE(chk.ok());
    EXPECT_TRUE(chk.stats().level_mismatches > 0);
}
