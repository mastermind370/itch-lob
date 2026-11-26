// Baseline order book for one stock: two std::maps of price -> level.
// Simple and obviously correct; the reference every faster book is checked against.
#pragma once

#include <cstdint>
#include <functional>
#include <map>

#include "itch/messages.hpp"

namespace lob {

using itch::Price;
using itch::Side;

struct Level {
    std::uint64_t shares = 0;
    std::uint32_t orders = 0;
};

// price == 0 means that side is empty (ITCH prices are always > 0).
struct Quote {
    Price price = 0;
    std::uint64_t shares = 0;
};

class MapBook {
public:
    void add(Side s, Price p, std::uint32_t q) {
        Level& l = s == Side::Buy ? bids_[p] : asks_[p];
        l.shares += q;
        ++l.orders;
    }

    // Removes q shares from the level; if order_gone, the order count drops too.
    void reduce(Side s, Price p, std::uint32_t q, bool order_gone) {
        if (s == Side::Buy) reduce_in(bids_, p, q, order_gone);
        else reduce_in(asks_, p, q, order_gone);
    }

    [[nodiscard]] Quote best_bid() const {
        return bids_.empty() ? Quote{} : Quote{bids_.begin()->first, bids_.begin()->second.shares};
    }
    [[nodiscard]] Quote best_ask() const {
        return asks_.empty() ? Quote{} : Quote{asks_.begin()->first, asks_.begin()->second.shares};
    }

    [[nodiscard]] Level level(Side s, Price p) const {
        if (s == Side::Buy) { auto it = bids_.find(p); return it == bids_.end() ? Level{} : it->second; }
        auto it = asks_.find(p);
        return it == asks_.end() ? Level{} : it->second;
    }

    [[nodiscard]] std::size_t level_count(Side s) const { return s == Side::Buy ? bids_.size() : asks_.size(); }

    // f(Price, const Level&) from best to worst.
    template <class F>
    void for_each_level(Side s, F&& f) const {
        if (s == Side::Buy) for (const auto& [p, l] : bids_) f(p, l);
        else for (const auto& [p, l] : asks_) f(p, l);
    }

private:
    template <class M>
    static void reduce_in(M& m, Price p, std::uint32_t q, bool order_gone) {
        auto it = m.find(p);
        if (it == m.end()) return;  // caller guarantees this can't happen; checker catches it if it does
        it->second.shares -= q;
        if (order_gone) --it->second.orders;
        if (it->second.orders == 0) m.erase(it);
    }

    std::map<Price, Level, std::greater<>> bids_;  // best (highest) first
    std::map<Price, Level> asks_;                  // best (lowest) first
};

}  // namespace lob
