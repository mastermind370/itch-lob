// Deterministic random order flow for the matching engines.
//
// Mostly limit orders near a slowly drifting mid (some priced through the
// spread so they match), a few market orders, and cancels of previously sent
// ids. Some cancels target orders that have already filled, which both engines
// must reject identically. The cancel rate rises with the number of open ids,
// so the book stays at a steady depth.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/types.hpp"
#include "itch/synthetic.hpp"

namespace engine {

struct Op {
    enum Kind : std::uint8_t { Limit, Market, Cancel } kind;
    Side side;
    Price price;
    Qty qty;
    OrderId id;
};

struct FlowConfig {
    std::uint64_t seed = 1;
    Price band = 1u << 16;
    std::size_t target_open = 4000;  // ids the generator tries to keep open
    std::uint32_t market_ppt = 30;   // market orders, parts per 1000
    std::uint32_t cross_ppt = 150;   // limits priced through the touch
};

class OrderFlow {
public:
    explicit OrderFlow(FlowConfig cfg) : cfg_(cfg), rng_(cfg.seed), mid_(cfg.band / 2) {}

    Op next() {
        // Cancel more often when many ids are open.
        const std::uint32_t cancel_ppt = open_.size() > cfg_.target_open ? 550 : 300;
        const std::uint64_t r = rng_.below(1000);
        if (r < cancel_ppt && !open_.empty()) {
            const std::size_t k = rng_.below(open_.size());
            const OrderId id = open_[k];
            open_[k] = open_.back();
            open_.pop_back();
            return Op{Op::Cancel, Side::Buy, 0, 0, id};
        }
        drift();
        const Side side = rng_.below(2) ? Side::Buy : Side::Sell;
        const OrderId id = next_id_++;
        if (r < cancel_ppt + cfg_.market_ppt) return Op{Op::Market, side, 0, qty(), id};

        // Passive: 0..40 ticks behind the mid on our side; crossing: up to 10 ticks through it.
        std::int64_t off = static_cast<std::int64_t>(rng_.below(41));
        if (rng_.below(1000) < cfg_.cross_ppt) off = -static_cast<std::int64_t>(1 + rng_.below(10));
        const std::int64_t p = side == Side::Buy ? static_cast<std::int64_t>(mid_) - 1 - off
                                                 : static_cast<std::int64_t>(mid_) + 1 + off;
        open_.push_back(id);
        return Op{Op::Limit, side, static_cast<Price>(p), qty(), id};
    }

    std::vector<Op> take(std::size_t n) {
        std::vector<Op> v;
        v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) v.push_back(next());
        return v;
    }

private:
    void drift() {
        const std::uint64_t r = rng_.below(64);
        if (r == 0 && mid_ > cfg_.band / 4) --mid_;
        else if (r == 1 && mid_ < cfg_.band / 4 * 3) ++mid_;
    }

    Qty qty() {
        static constexpr Qty kSizes[] = {1, 10, 100, 100, 100, 200, 300, 500, 1000, 37};
        return kSizes[rng_.below(std::size(kSizes))];
    }

    FlowConfig cfg_;
    itch::SplitMix64 rng_;
    Price mid_;
    OrderId next_id_ = 1;
    std::vector<OrderId> open_;
};

// Applies one op to an engine.
template <class Engine, class Sink>
inline Result apply(Engine& e, const Op& op, Sink& sink) {
    switch (op.kind) {
        case Op::Limit: return e.limit(op.id, op.side, op.price, op.qty, sink);
        case Op::Market: return e.market(op.id, op.side, op.qty, sink);
        case Op::Cancel: return Result{e.cancel(op.id)};
    }
    return {};
}

}  // namespace engine
