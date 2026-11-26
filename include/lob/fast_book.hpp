// Fast order book for one stock.
//
// Prices near where the stock first trades live in a flat array (the price
// ladder): level i holds price base + i * tick. Price -> level is a subtract and
// a constant divide, with no tree walk and no allocation. A bitmap per side
// finds the next best level with count-leading/trailing-zero instructions when
// the best level empties.
//
// Anything outside the window (or off the tick grid, e.g. sub-penny prices on
// a >$1 stock) goes to a small std::map overflow, so the book is always exact.
// overflow_adds() reports how often that happens.
//
// The window is first centered on the stock's first add. A stock's first order
// of the day can be far from where it really trades (pre-market stub quotes),
// so after every kRecenterAfter overflow adds the book checks whether its best
// bid or ask has left the window. If so, it recenters on the touch: every level
// is moved between ladder and overflow as needed. This is O(levels), rare, and
// counted in recenters().
//
// The ladder (~33 KB) is allocated on a stock's first add, once per stock,
// never per message.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "lob/map_book.hpp"

namespace lob {

class FastBook {
public:
    static constexpr int kWindow = 2048;  // ticks: +/- $10.24 around the center for a >$1 stock
    static constexpr std::uint32_t kRecenterAfter = 256;  // overflow adds between touch checks

    void add(Side s, Price p, std::uint32_t q) {
        if (!lad_) [[unlikely]] anchor(p);
        int i;
        if (index(p, i)) [[likely]] {
            if (s == Side::Buy) lad_->bid.add(i, q, true);
            else lad_->ask.add(i, q, false);
            return;
        }
        ++overflow_adds_;
        Level& l = s == Side::Buy ? obids_[p] : oasks_[p];
        l.shares += q;
        ++l.orders;
        if (++misses_ >= kRecenterAfter) [[unlikely]] maybe_recenter();
    }

    void reduce(Side s, Price p, std::uint32_t q, bool order_gone) {
        int i;
        if (index(p, i)) [[likely]] {
            if (s == Side::Buy) lad_->bid.reduce(i, q, order_gone, true);
            else lad_->ask.reduce(i, q, order_gone, false);
            return;
        }
        if (s == Side::Buy) reduce_overflow(obids_, p, q, order_gone);
        else reduce_overflow(oasks_, p, q, order_gone);
    }

    [[nodiscard]] Quote best_bid() const {
        Quote q;
        if (lad_ && lad_->bid.best >= 0) q = {price_at(lad_->bid.best), lad_->bid.lv[lad_->bid.best].shares};
        if (!obids_.empty() && obids_.begin()->first > q.price) q = {obids_.begin()->first, obids_.begin()->second.shares};
        return q;
    }

    [[nodiscard]] Quote best_ask() const {
        Quote q;
        if (lad_ && lad_->ask.best >= 0) q = {price_at(lad_->ask.best), lad_->ask.lv[lad_->ask.best].shares};
        if (!oasks_.empty() && (q.price == 0 || oasks_.begin()->first < q.price))
            q = {oasks_.begin()->first, oasks_.begin()->second.shares};
        return q;
    }

    [[nodiscard]] Level level(Side s, Price p) const {
        int i;
        if (index(p, i)) {
            const Slot& l = (s == Side::Buy ? lad_->bid : lad_->ask).lv[i];
            return Level{l.shares, l.orders};
        }
        const auto find = [p](const auto& m) {
            auto it = m.find(p);
            return it == m.end() ? Level{} : it->second;
        };
        return s == Side::Buy ? find(obids_) : find(oasks_);
    }

    [[nodiscard]] std::size_t level_count(Side s) const {
        const std::size_t n = lad_ ? (s == Side::Buy ? lad_->bid.nlevels : lad_->ask.nlevels) : 0;
        return n + (s == Side::Buy ? obids_.size() : oasks_.size());
    }

    // f(Price, const Level&) for every non-empty level, in unspecified order.
    template <class F>
    void for_each_level(Side s, F&& f) const {
        if (lad_) {
            const SideLadder& sl = s == Side::Buy ? lad_->bid : lad_->ask;
            for (int w = 0; w < kWords; ++w)
                for (std::uint64_t bits = sl.bits[w]; bits; bits &= bits - 1) {
                    const int i = w * 64 + __builtin_ctzll(bits);
                    f(price_at(i), Level{sl.lv[i].shares, sl.lv[i].orders});
                }
        }
        if (s == Side::Buy) for (const auto& [p, l] : obids_) f(p, l);
        else for (const auto& [p, l] : oasks_) f(p, l);
    }

    [[nodiscard]] std::uint64_t overflow_adds() const { return overflow_adds_; }
    [[nodiscard]] std::uint64_t recenters() const { return recenters_; }
    [[nodiscard]] Price window_low() const { return lad_ ? base_ : 0; }
    [[nodiscard]] Price window_high() const { return lad_ ? price_at(kWindow - 1) : 0; }

private:
    static constexpr int kWords = kWindow / 64;

    struct Slot {
        std::uint32_t shares;
        std::uint32_t orders;
    };

    struct SideLadder {
        Slot lv[kWindow];
        std::uint64_t bits[kWords];
        int best = -1;  // bids: highest non-empty index; asks: lowest
        std::uint32_t nlevels = 0;

        void add(int i, std::uint32_t q, bool is_bid) {
            Slot& l = lv[i];
            if (l.orders == 0) {
                bits[i >> 6] |= std::uint64_t{1} << (i & 63);
                ++nlevels;
                if (best < 0 || (is_bid ? i > best : i < best)) best = i;
            }
            l.shares += q;
            ++l.orders;
        }

        void reduce(int i, std::uint32_t q, bool order_gone, bool is_bid) {
            Slot& l = lv[i];
            if (l.orders == 0) [[unlikely]] return;  // caller bug; the checker would report it
            l.shares -= q;
            if (order_gone) --l.orders;
            if (l.orders != 0) return;
            l.shares = 0;
            bits[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
            --nlevels;
            if (i == best) best = is_bid ? prev_set(i) : next_set(i);
        }

        // Places a whole level into an empty slot (used when recentering).
        void put(int i, const Level& l, bool is_bid) {
            lv[i] = Slot{static_cast<std::uint32_t>(l.shares), l.orders};
            bits[i >> 6] |= std::uint64_t{1} << (i & 63);
            ++nlevels;
            if (best < 0 || (is_bid ? i > best : i < best)) best = i;
        }

        // Highest set index below i, or -1.
        [[nodiscard]] int prev_set(int i) const {
            int w = i >> 6;
            std::uint64_t m = bits[w] & ((std::uint64_t{1} << (i & 63)) - 1);
            while (true) {
                if (m) return w * 64 + 63 - __builtin_clzll(m);
                if (--w < 0) return -1;
                m = bits[w];
            }
        }

        // Lowest set index above i, or -1.
        [[nodiscard]] int next_set(int i) const {
            int w = i >> 6;
            std::uint64_t m = (i & 63) == 63 ? 0 : bits[w] & (~std::uint64_t{0} << ((i & 63) + 1));
            while (true) {
                if (m) return w * 64 + __builtin_ctzll(m);
                if (++w >= kWords) return -1;
                m = bits[w];
            }
        }
    };

    struct Ladder {
        SideLadder bid;
        SideLadder ask;
    };

    void anchor(Price p) {
        set_window(p);
        lad_ = std::make_unique<Ladder>();  // value-initialized: all levels and bits zero
    }

    void set_window(Price p) {
        tick_ = p >= 10'000 ? 100 : 1;  // Reg NMS: penny ticks at or above $1.00
        const Price center = p / tick_;
        base_ = (center > kWindow / 2 ? center - kWindow / 2 : 0) * tick_;
    }

    void maybe_recenter() {
        misses_ = 0;
        const Quote b = best_bid(), a = best_ask();
        int i;
        const bool bid_ok = b.price == 0 || index(b.price, i);
        const bool ask_ok = a.price == 0 || index(a.price, i);
        if (bid_ok && ask_ok) return;  // the touch is in the window; overflow is just deep orders
        const Price c = b.price && a.price ? b.price / 2 + a.price / 2 : (b.price ? b.price : a.price);
        recenter(c);
    }

    void recenter(Price center) {
        struct Saved { Side side; Price price; Level level; };
        std::vector<Saved> all;
        all.reserve(level_count(Side::Buy) + level_count(Side::Sell));
        for (Side sd : {Side::Buy, Side::Sell})
            for_each_level(sd, [&](Price p, const Level& l) { all.push_back({sd, p, l}); });
        obids_.clear();
        oasks_.clear();
        lad_->bid = SideLadder{};
        lad_->ask = SideLadder{};
        set_window(center);
        for (const Saved& sv : all) {
            int i;
            if (index(sv.price, i)) {
                if (sv.side == Side::Buy) lad_->bid.put(i, sv.level, true);
                else lad_->ask.put(i, sv.level, false);
            } else {
                (sv.side == Side::Buy ? obids_[sv.price] : oasks_[sv.price]) = sv.level;
            }
        }
        ++recenters_;
    }

    [[nodiscard]] bool index(Price p, int& i) const {
        if (!lad_ || p < base_) return false;
        Price d = p - base_;
        if (tick_ == 100) {
            if (d % 100) return false;
            d /= 100;
        }
        if (d >= static_cast<Price>(kWindow)) return false;
        i = static_cast<int>(d);
        return true;
    }

    [[nodiscard]] Price price_at(int i) const { return base_ + static_cast<Price>(i) * tick_; }

    template <class M>
    static void reduce_overflow(M& m, Price p, std::uint32_t q, bool order_gone) {
        auto it = m.find(p);
        if (it == m.end()) return;
        it->second.shares -= q;
        if (order_gone) --it->second.orders;
        if (it->second.orders == 0) m.erase(it);
    }

    std::unique_ptr<Ladder> lad_;
    Price base_ = 0;
    Price tick_ = 1;
    std::map<Price, Level, std::greater<>> obids_;
    std::map<Price, Level> oasks_;
    std::uint64_t overflow_adds_ = 0;
    std::uint64_t recenters_ = 0;
    std::uint32_t misses_ = 0;
};

}  // namespace lob
