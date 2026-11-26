// Deterministic synthetic ITCH 5.0 feed.
//
// Produces a stream that is valid for book reconstruction: every execute,
// cancel, delete and replace refers to a live order, and executed/cancelled
// shares never exceed what is left on the order. The default message mix
// is roughly that of a real NASDAQ day (dominated by adds and deletes).
//
// The visible book never crosses (best bid < best ask for every stock).
//
// Uses its own RNG and integer math only, so the same seed produces the
// same bytes on every platform and standard library.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "itch/encoder.hpp"

namespace itch {

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // Uniform in [0, n). Lemire's multiply-shift; bias is negligible for our n.
    std::uint64_t below(std::uint64_t n) noexcept {
        __extension__ using u128 = unsigned __int128;
        return static_cast<std::uint64_t>((static_cast<u128>(next()) * n) >> 64);
    }

private:
    std::uint64_t s_;
};

struct SyntheticConfig {
    std::uint64_t messages = 1'000'000;  // order messages (excludes S/R preamble)
    std::uint32_t stocks = 64;
    std::uint64_t seed = 42;
    // Mix in parts per 1000 (remainder goes to adds).
    std::uint32_t delete_ppt = 400;
    std::uint32_t replace_ppt = 80;
    std::uint32_t execute_ppt = 30;
    std::uint32_t execute_price_ppt = 3;
    std::uint32_t cancel_ppt = 15;
    std::uint32_t trade_ppt = 7;
    std::size_t target_live_per_stock = 400;  // book depth the generator steers toward
};

class SyntheticFeed {
public:
    explicit SyntheticFeed(SyntheticConfig cfg) : cfg_(cfg), rng_(cfg.seed) {
        stocks_.reserve(cfg_.stocks);
        for (std::uint32_t i = 0; i < cfg_.stocks; ++i) {
            Stock s;
            s.locate = static_cast<std::uint16_t>(i + 1);
            s.symbol = make_symbol(i);
            // $10 .. $210, on the penny grid like real NASDAQ quotes above $1.
            s.mid = (100'000 + static_cast<Price>(rng_.below(2'000'000))) / 100 * 100;
            stocks_.push_back(std::move(s));
        }
    }

    // Writes the preamble: start-of-messages event + one directory entry per stock.
    void write_preamble(std::vector<std::uint8_t>& out) {
        Encoder e(out);
        e.system_event(0, tick(), 'O');
        for (const auto& s : stocks_) e.stock_directory(s.locate, tick(), s.symbol);
        e.system_event(0, tick(), 'Q');
    }

    // Appends up to `n` order messages; returns how many were written.
    std::uint64_t write_messages(std::vector<std::uint8_t>& out, std::uint64_t n) {
        Encoder e(out);
        std::uint64_t i = 0;
        for (; i < n && emitted_ < cfg_.messages; ++i, ++emitted_) step(e);
        return i;
    }

    void write_epilogue(std::vector<std::uint8_t>& out) {
        Encoder e(out);
        e.system_event(0, tick(), 'M');
        e.system_event(0, tick(), 'E');
        e.system_event(0, tick(), 'C');
    }

    [[nodiscard]] bool done() const noexcept { return emitted_ >= cfg_.messages; }
    [[nodiscard]] const SyntheticConfig& config() const noexcept { return cfg_; }

    // Convenience: whole feed in memory.
    static std::vector<std::uint8_t> generate(const SyntheticConfig& cfg) {
        std::vector<std::uint8_t> out;
        out.reserve(cfg.messages * 34 + 4096);
        SyntheticFeed f(cfg);
        f.write_preamble(out);
        f.write_messages(out, cfg.messages);
        f.write_epilogue(out);
        return out;
    }

private:
    struct Live {
        std::uint64_t ref;
        Side side;
        std::uint32_t shares;
        Price price;
    };
    struct Stock {
        std::uint16_t locate = 0;
        std::string symbol;
        Price mid = 0;
        std::vector<Live> live;
    };

    static std::string make_symbol(std::uint32_t i) {
        std::string s = "S";
        do {
            s.push_back(static_cast<char>('A' + i % 26));
            i /= 26;
        } while (i && s.size() < 8);
        return s;
    }

    std::uint64_t tick() noexcept {
        ts_ += 1 + rng_.below(2'000);  // ~1 us average gap
        return ts_;
    }

    Price quote_price(Stock& s, Side side) noexcept {
        // Each stock's mid is fixed; orders rest 1..50 cents ($0.01 = 100) away
        // on their own side. Every bid is below every ask, so the book never
        // crosses (a drifting mid would leave stale bids above new asks).
        // Offsets are skewed toward the touch, like real depth.
        const std::uint64_t a = rng_.below(50), b = rng_.below(50);
        const Price off = static_cast<Price>(100 * (1 + (a < b ? a : b)));
        if (side == Side::Buy) return s.mid > off + 100 ? s.mid - off : 100;
        return s.mid + off;
    }

    std::uint32_t lot() noexcept {
        static constexpr std::uint32_t kLots[] = {100, 100, 100, 200, 300, 500, 1000, 1, 37, 250};
        return kLots[rng_.below(std::size(kLots))];
    }

    void add(Encoder& e, Stock& s) {
        const Side side = rng_.below(2) ? Side::Buy : Side::Sell;
        const Live o{next_ref_++, side, lot(), quote_price(s, side)};
        const bool mpid = rng_.below(20) == 0;
        e.add_order(s.locate, tick(), o.ref, o.side, o.shares, s.symbol, o.price, mpid ? "MPID" : "");
        s.live.push_back(o);
    }

    void step(Encoder& e) {
        Stock& s = stocks_[rng_.below(stocks_.size())];
        // Steer depth: below target, favour adds; above, favour removals.
        if (s.live.empty() || (s.live.size() < cfg_.target_live_per_stock / 2 && rng_.below(2))) {
            add(e, s);
            return;
        }
        std::uint64_t r = rng_.below(1000);
        if (s.live.size() > 2 * cfg_.target_live_per_stock && r >= cfg_.delete_ppt) r = 0;  // force a delete

        const std::size_t idx = rng_.below(s.live.size());
        Live& o = s.live[idx];
        auto remove = [&] {
            s.live[idx] = s.live.back();
            s.live.pop_back();
        };

        std::uint32_t acc = cfg_.delete_ppt;
        if (r < acc) {
            e.del(s.locate, tick(), o.ref);
            remove();
            return;
        }
        if (r < (acc += cfg_.replace_ppt)) {
            const std::uint64_t new_ref = next_ref_++;
            const std::uint32_t shares = lot();
            const Price px = quote_price(s, o.side);
            e.replace(s.locate, tick(), o.ref, new_ref, shares, px);
            o.ref = new_ref;
            o.shares = shares;
            o.price = px;
            return;
        }
        if (r < (acc += cfg_.execute_ppt)) {
            const std::uint32_t q = 1 + static_cast<std::uint32_t>(rng_.below(o.shares));
            e.executed(s.locate, tick(), o.ref, q, next_match_++);
            o.shares -= q;
            if (o.shares == 0) remove();
            return;
        }
        if (r < (acc += cfg_.execute_price_ppt)) {
            const std::uint32_t q = 1 + static_cast<std::uint32_t>(rng_.below(o.shares));
            e.executed_with_price(s.locate, tick(), o.ref, q, next_match_++, 'Y', o.price);
            o.shares -= q;
            if (o.shares == 0) remove();
            return;
        }
        if (r < (acc += cfg_.cancel_ppt)) {
            if (o.shares < 2) {  // a partial cancel needs at least 2 shares; delete instead
                e.del(s.locate, tick(), o.ref);
                remove();
                return;
            }
            const std::uint32_t q = 1 + static_cast<std::uint32_t>(rng_.below(o.shares - 1));
            e.cancel(s.locate, tick(), o.ref, q);
            o.shares -= q;
            return;
        }
        if (r < (acc += cfg_.trade_ppt)) {
            e.trade(s.locate, tick(), 0, Side::Buy, lot(), s.symbol, s.mid, next_match_++);
            return;
        }
        add(e, s);
    }

    SyntheticConfig cfg_;
    SplitMix64 rng_;
    std::vector<Stock> stocks_;
    std::uint64_t ts_ = 34'200ull * 1'000'000'000ull;  // 09:30:00
    std::uint64_t next_ref_ = 1;
    std::uint64_t next_match_ = 1;
    std::uint64_t emitted_ = 0;
};

}  // namespace itch
