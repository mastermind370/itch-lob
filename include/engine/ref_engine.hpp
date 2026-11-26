// Reference matching engine: std::map of price -> std::deque FIFO queue.
// Written to be obviously correct, not fast. FastEngine is tested against it.
#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

#include "engine/types.hpp"

namespace engine {

class RefEngine {
public:
    // capacity is accepted for interface parity with FastEngine; the reference has no limit.
    explicit RefEngine(Price band = 1u << 16, std::uint32_t /*capacity*/ = 0) : band_(band) {}

    template <class Sink>
    Result limit(OrderId id, Side side, Price price, Qty qty, Sink& sink) {
        if (qty == 0) return {Status::BadQty};
        if (price == 0 || price >= band_) return {Status::BadPrice};
        if (where_.count(id)) return {Status::DuplicateId};
        Result r;
        Qty left = qty;
        if (side == Side::Buy) match(asks_, id, price, left, r, sink, [](Price lv, Price lim) { return lv <= lim; });
        else match(bids_, id, price, left, r, sink, [](Price lv, Price lim) { return lv >= lim; });
        if (left) {
            if (side == Side::Buy) bids_[price].push_back({id, left});
            else asks_[price].push_back({id, left});
            where_[id] = {side, price};
            r.rested = left;
        }
        return r;
    }

    template <class Sink>
    Result market(OrderId id, Side side, Qty qty, Sink& sink) {
        if (qty == 0) return {Status::BadQty};
        if (where_.count(id)) return {Status::DuplicateId};
        Result r;
        Qty left = qty;
        if (side == Side::Buy) match(asks_, id, 0, left, r, sink, [](Price, Price) { return true; });
        else match(bids_, id, 0, left, r, sink, [](Price, Price) { return true; });
        return r;
    }

    Status cancel(OrderId id) {
        auto it = where_.find(id);
        if (it == where_.end()) return Status::UnknownId;
        const auto [side, price] = it->second;
        where_.erase(it);
        if (side == Side::Buy) remove(bids_, price, id);
        else remove(asks_, price, id);
        return Status::Ok;
    }

    [[nodiscard]] Quote best_bid() const { return top(bids_); }
    [[nodiscard]] Quote best_ask() const { return top(asks_); }
    [[nodiscard]] std::size_t order_count() const { return where_.size(); }

    // Best to worst.
    [[nodiscard]] std::vector<LevelSnapshot> snapshot(Side s) const {
        return s == Side::Buy ? snap(bids_) : snap(asks_);
    }

private:
    struct O {
        OrderId id;
        Qty qty;
    };
    using Queue = std::deque<O>;

    template <class M, class Sink, class Crosses>
    void match(M& book, OrderId taker, Price limit, Qty& left, Result& r, Sink& sink, Crosses crosses) {
        while (left && !book.empty() && crosses(book.begin()->first, limit)) {
            const Price lp = book.begin()->first;
            Queue& q = book.begin()->second;
            while (left && !q.empty()) {
                O& m = q.front();
                const Qty x = std::min(left, m.qty);
                sink.on_fill(Fill{taker, m.id, lp, x});
                left -= x;
                m.qty -= x;
                r.filled += x;
                if (m.qty == 0) {
                    where_.erase(m.id);
                    q.pop_front();
                }
            }
            if (q.empty()) book.erase(book.begin());
        }
    }

    template <class M>
    static void remove(M& book, Price p, OrderId id) {
        auto it = book.find(p);
        Queue& q = it->second;
        q.erase(std::find_if(q.begin(), q.end(), [id](const O& o) { return o.id == id; }));
        if (q.empty()) book.erase(it);
    }

    template <class M>
    static Quote top(const M& book) {
        if (book.empty()) return {};
        std::uint64_t s = 0;
        for (const O& o : book.begin()->second) s += o.qty;
        return {book.begin()->first, s};
    }

    template <class M>
    static std::vector<LevelSnapshot> snap(const M& book) {
        std::vector<LevelSnapshot> out;
        for (const auto& [p, q] : book) {
            LevelSnapshot l{p, 0, {}};
            for (const O& o : q) {
                l.shares += o.qty;
                l.queue.emplace_back(o.id, o.qty);
            }
            out.push_back(std::move(l));
        }
        return out;
    }

    Price band_;
    std::map<Price, Queue, std::greater<>> bids_;
    std::map<Price, Queue> asks_;
    std::unordered_map<OrderId, std::pair<Side, Price>> where_;
};

}  // namespace engine
