#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch/encoder.hpp"
#include "itch/parser.hpp"
#include "itch/synthetic.hpp"

using namespace itch;

namespace {

// Records the last message of each kind plus the order they arrived in.
struct Recorder : NullHandler {
    std::string order;
    SystemEvent sys{};
    StockDirectory dir{};
    AddOrder add{};
    OrderExecuted exe{};
    OrderExecutedWithPrice exep{};
    OrderCancel cxl{};
    OrderDelete del{};
    OrderReplace rep{};
    Trade trd{};
    char other_type = 0;
    std::size_t other_len = 0;

    void on_system_event(const SystemEvent& m) { order += m.hdr.type; sys = m; }
    void on_stock_directory(const StockDirectory& m) { order += m.hdr.type; dir = m; }
    void on_add(const AddOrder& m) { order += m.hdr.type; add = m; }
    void on_executed(const OrderExecuted& m) { order += m.hdr.type; exe = m; }
    void on_executed_with_price(const OrderExecutedWithPrice& m) { order += m.hdr.type; exep = m; }
    void on_cancel(const OrderCancel& m) { order += m.hdr.type; cxl = m; }
    void on_delete(const OrderDelete& m) { order += m.hdr.type; del = m; }
    void on_replace(const OrderReplace& m) { order += m.hdr.type; rep = m; }
    void on_trade(const Trade& m) { order += m.hdr.type; trd = m; }
    void on_other(char t, const std::uint8_t*, std::size_t len) { order += t; other_type = t; other_len = len; }
};

constexpr std::uint64_t kTs = 0x0000123456789ABCull;  // uses all 6 timestamp bytes

}  // namespace

TEST(Endian, LoadsAreBigEndian) {
    const std::uint8_t b[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    EXPECT_EQ(load_be16(b), 0x0102u);
    EXPECT_EQ(load_be32(b), 0x01020304u);
    EXPECT_EQ(load_be48(b), 0x010203040506ull);
    EXPECT_EQ(load_be64(b), 0x0102030405060708ull);
}

TEST(Endian, StoreLoadRoundTrip) {
    std::uint8_t b[8] = {};
    store_be48(b, kTs);
    EXPECT_EQ(load_be48(b), kTs);
    store_be64(b, 0xDEADBEEFCAFEF00Dull);
    EXPECT_EQ(load_be64(b), 0xDEADBEEFCAFEF00Dull);
}

TEST(Messages, LengthTableMatchesSpec) {
    EXPECT_EQ(kMessageLength['A'], 36);
    EXPECT_EQ(kMessageLength['F'], 40);
    EXPECT_EQ(kMessageLength['E'], 31);
    EXPECT_EQ(kMessageLength['C'], 36);
    EXPECT_EQ(kMessageLength['X'], 23);
    EXPECT_EQ(kMessageLength['D'], 19);
    EXPECT_EQ(kMessageLength['U'], 35);
    EXPECT_EQ(kMessageLength['P'], 44);
    EXPECT_EQ(kMessageLength['R'], 39);
    EXPECT_EQ(kMessageLength['S'], 12);
    EXPECT_EQ(kMessageLength['Z'], 0);
}

TEST(Messages, PriceHasFourDecimals) {
    EXPECT_EQ(price_to_double(1'234'500), 123.45);
    EXPECT_EQ(trim_symbol("AAPL    "), "AAPL");
}

TEST(Parser, AddOrder) {
    std::vector<std::uint8_t> buf;
    Encoder(buf).add_order(7, kTs, 1234567890123ull, Side::Buy, 300, "AAPL", 1'503'200);
    Recorder r;
    const auto res = parse_frames(buf.data(), buf.size(), r);
    EXPECT_EQ(res.status, ParseStatus::Ok);
    EXPECT_EQ(res.messages, 1u);
    EXPECT_EQ(res.consumed, buf.size());
    EXPECT_EQ(r.add.hdr.type, 'A');
    EXPECT_EQ(r.add.hdr.stock_locate, 7);
    EXPECT_EQ(r.add.hdr.timestamp, kTs);
    EXPECT_EQ(r.add.order_ref, 1234567890123ull);
    EXPECT_EQ(r.add.side, Side::Buy);
    EXPECT_EQ(r.add.shares, 300u);
    EXPECT_EQ(trim_symbol(r.add.stock), "AAPL");
    EXPECT_EQ(r.add.price, 1'503'200u);
    EXPECT_TRUE(r.add.attribution.empty());
}

TEST(Parser, AddOrderWithMpid) {
    std::vector<std::uint8_t> buf;
    Encoder(buf).add_order(1, kTs, 99, Side::Sell, 50, "MSFT", 4'000'000, "GSCO");
    Recorder r;
    EXPECT_EQ(parse_frames(buf.data(), buf.size(), r).status, ParseStatus::Ok);
    EXPECT_EQ(r.add.hdr.type, 'F');
    EXPECT_EQ(r.add.side, Side::Sell);
    EXPECT_EQ(r.add.attribution, "GSCO");
}

TEST(Parser, StringFieldsAreZeroCopy) {
    std::vector<std::uint8_t> buf;
    Encoder(buf).add_order(1, kTs, 1, Side::Buy, 1, "ZVZZT", 1);
    Recorder r;
    (void)parse_frames(buf.data(), buf.size(), r);
    const auto* p = reinterpret_cast<const std::uint8_t*>(r.add.stock.data());
    EXPECT_TRUE(p >= buf.data() && p + 8 <= buf.data() + buf.size());
    EXPECT_EQ(p, buf.data() + 2 + 24);  // frame length + field offset
}

TEST(Parser, ExecuteCancelDeleteReplace) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.executed(3, kTs, 11, 100, 555);
    e.executed_with_price(3, kTs + 1, 12, 40, 556, 'N', 999'900);
    e.cancel(3, kTs + 2, 13, 25);
    e.del(3, kTs + 3, 14);
    e.replace(3, kTs + 4, 15, 16, 700, 123'400);

    Recorder r;
    const auto res = parse_frames(buf.data(), buf.size(), r);
    ASSERT_EQ(res.status, ParseStatus::Ok);
    EXPECT_EQ(res.messages, 5u);
    EXPECT_EQ(r.order, "ECXDU");

    EXPECT_EQ(r.exe.order_ref, 11u);
    EXPECT_EQ(r.exe.executed_shares, 100u);
    EXPECT_EQ(r.exe.match_number, 555u);

    EXPECT_EQ(r.exep.order_ref, 12u);
    EXPECT_EQ(r.exep.executed_shares, 40u);
    EXPECT_EQ(r.exep.match_number, 556u);
    EXPECT_EQ(r.exep.printable, 'N');
    EXPECT_EQ(r.exep.execution_price, 999'900u);

    EXPECT_EQ(r.cxl.order_ref, 13u);
    EXPECT_EQ(r.cxl.cancelled_shares, 25u);

    EXPECT_EQ(r.del.order_ref, 14u);
    EXPECT_EQ(r.del.hdr.timestamp, kTs + 3);

    EXPECT_EQ(r.rep.original_order_ref, 15u);
    EXPECT_EQ(r.rep.new_order_ref, 16u);
    EXPECT_EQ(r.rep.shares, 700u);
    EXPECT_EQ(r.rep.price, 123'400u);
}

TEST(Parser, SystemEventDirectoryAndTrade) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.system_event(0, kTs, 'O');
    e.stock_directory(42, kTs, "SPY", 100);
    e.trade(42, kTs, 0, Side::Buy, 200, "SPY", 4'500'000, 77);
    Recorder r;
    ASSERT_EQ(parse_frames(buf.data(), buf.size(), r).status, ParseStatus::Ok);
    EXPECT_EQ(r.order, "SRP");
    EXPECT_EQ(r.sys.event_code, 'O');
    EXPECT_EQ(r.dir.hdr.stock_locate, 42);
    EXPECT_EQ(trim_symbol(r.dir.stock), "SPY");
    EXPECT_EQ(r.dir.round_lot_size, 100u);
    EXPECT_EQ(r.trd.shares, 200u);
    EXPECT_EQ(r.trd.price, 4'500'000u);
    EXPECT_EQ(r.trd.match_number, 77u);
}

TEST(Parser, OtherKnownTypeGoesToOnOther) {
    std::vector<std::uint8_t> msg(kMessageLength['H'], 0);
    msg[0] = 'H';
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.raw(msg);
    e.del(1, kTs, 5);
    Recorder r;
    const auto res = parse_frames(buf.data(), buf.size(), r);
    EXPECT_EQ(res.status, ParseStatus::Ok);
    EXPECT_EQ(r.order, "HD");
    EXPECT_EQ(r.other_len, 25u);
}

TEST(Parser, UnknownTypeIsSkipped) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.raw({'z', 1, 2, 3});
    e.del(1, kTs, 5);
    Recorder r;
    const auto res = parse_frames(buf.data(), buf.size(), r);
    EXPECT_EQ(res.status, ParseStatus::Ok);
    EXPECT_EQ(res.messages, 2u);
    EXPECT_EQ(r.other_type, 'z');
    EXPECT_EQ(r.other_len, 4u);
    EXPECT_EQ(r.del.order_ref, 5u);
}

TEST(Parser, EmptyBuffer) {
    Recorder r;
    const auto res = parse_frames(nullptr, 0, r);
    EXPECT_EQ(res.status, ParseStatus::Ok);
    EXPECT_EQ(res.messages, 0u);
}

TEST(Parser, TruncatedFrameStopsAtBoundary) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.del(1, kTs, 1);
    const std::size_t first = buf.size();
    e.add_order(1, kTs, 2, Side::Buy, 100, "AAPL", 1);
    for (std::size_t cut = first + 1; cut < buf.size(); ++cut) {
        Recorder r;
        const auto res = parse_frames(buf.data(), cut, r);
        EXPECT_EQ(res.status, ParseStatus::Truncated);
        EXPECT_EQ(res.messages, 1u);
        EXPECT_EQ(res.consumed, first);
    }
}

TEST(Parser, ResumeAfterTruncation) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    for (std::uint64_t i = 1; i <= 100; ++i) e.del(1, kTs, i);
    // Feed in uneven chunks the way a socket would deliver them.
    struct Count : NullHandler {
        std::uint64_t n = 0, sum = 0;
        void on_delete(const OrderDelete& m) { ++n; sum += m.order_ref; }
    } c;
    std::vector<std::uint8_t> pending;
    for (std::size_t off = 0; off < buf.size(); off += 37) {
        const std::size_t end = std::min(buf.size(), off + 37);
        pending.insert(pending.end(), buf.begin() + off, buf.begin() + end);
        const auto res = parse_frames(pending.data(), pending.size(), c);
        ASSERT_TRUE(res.status != ParseStatus::BadLength);
        pending.erase(pending.begin(), pending.begin() + res.consumed);
    }
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(c.n, 100u);
    EXPECT_EQ(c.sum, 5050u);
}

TEST(Parser, WrongLengthForKnownTypeIsRejected) {
    std::vector<std::uint8_t> buf;
    Encoder e(buf);
    e.del(1, kTs, 1);
    const std::size_t good = buf.size();
    e.raw(std::vector<std::uint8_t>{'A', 0, 0, 0, 0});  // 'A' must be 36 bytes
    Recorder r;
    const auto res = parse_frames(buf.data(), buf.size(), r);
    EXPECT_EQ(res.status, ParseStatus::BadLength);
    EXPECT_EQ(res.consumed, good);
    EXPECT_EQ(res.messages, 1u);
}

TEST(Parser, ZeroLengthFrameIsRejected) {
    const std::uint8_t buf[] = {0, 0};
    Recorder r;
    EXPECT_EQ(parse_frames(buf, sizeof buf, r).status, ParseStatus::BadLength);
}

// The synthetic feed must be valid input for book building in later stages.
TEST(Synthetic, EveryReferenceIsLiveAndSharesNeverGoNegative) {
    SyntheticConfig cfg;
    cfg.messages = 200'000;
    cfg.stocks = 16;
    const auto buf = SyntheticFeed::generate(cfg);

    struct Checker : NullHandler {
        std::unordered_map<std::uint64_t, std::uint32_t> live;
        std::uint64_t errors = 0, adds = 0, removes = 0, order_msgs = 0;
        void on_add(const AddOrder& m) {
            ++order_msgs; ++adds;
            if (!live.emplace(m.order_ref, m.shares).second) ++errors;
        }
        void reduce(std::uint64_t ref, std::uint32_t q, bool full_ok) {
            ++order_msgs;
            auto it = live.find(ref);
            if (it == live.end() || q > it->second || (!full_ok && q == it->second)) { ++errors; return; }
            it->second -= q;
            if (it->second == 0) { live.erase(it); ++removes; }
        }
        void on_executed(const OrderExecuted& m) { reduce(m.order_ref, m.executed_shares, true); }
        void on_executed_with_price(const OrderExecutedWithPrice& m) { reduce(m.order_ref, m.executed_shares, true); }
        void on_cancel(const OrderCancel& m) { reduce(m.order_ref, m.cancelled_shares, false); }
        void on_delete(const OrderDelete& m) {
            ++order_msgs;
            if (live.erase(m.order_ref) != 1) ++errors; else ++removes;
        }
        void on_replace(const OrderReplace& m) {
            ++order_msgs;
            if (live.erase(m.original_order_ref) != 1) ++errors;
            if (!live.emplace(m.new_order_ref, m.shares).second) ++errors;
        }
        void on_trade(const Trade&) { ++order_msgs; }
    } chk;

    const auto res = parse_frames(buf.data(), buf.size(), chk);
    ASSERT_EQ(res.status, ParseStatus::Ok);
    EXPECT_EQ(chk.errors, 0u);
    EXPECT_EQ(chk.order_msgs, cfg.messages);
    EXPECT_EQ(chk.adds - chk.removes, chk.live.size());
}

TEST(Synthetic, SameSeedSameBytes) {
    SyntheticConfig cfg;
    cfg.messages = 10'000;
    EXPECT_TRUE(SyntheticFeed::generate(cfg) == SyntheticFeed::generate(cfg));
    SyntheticConfig other = cfg;
    other.seed = 7;
    EXPECT_FALSE(SyntheticFeed::generate(cfg) == SyntheticFeed::generate(other));
}
