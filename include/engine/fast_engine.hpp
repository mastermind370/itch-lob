// Fast price-time matching engine.
//
// - Flat price ladder: one Level per tick in [0, band), indexed directly by price.
// - Each level is an intrusive doubly-linked FIFO of order nodes from one
//   preallocated pool, so adding at the back, filling from the front and
//   cancelling from the middle are all O(1).
// - A two-level bitmap per side (64-bit words + a summary bit per word) finds
//   the next best price in a handful of word scans when a level empties.
// - Order id -> pool index goes through a preallocated open-addressing map
//   sized at 2x the pool, so it never grows.
//
// Nothing is allocated after construction; when the pool is full, a limit
// order's remainder is cancelled with Status::Full.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "engine/types.hpp"
#include "lob/order_map.hpp"

namespace engine {

// Bitset over [0, n) with O(n / 4096) next/prev search.
class TwoLevelBits {
public:
    explicit TwoLevelBits(std::size_t n) : words_((n + 63) / 64, 0), summary_((words_.size() + 63) / 64, 0) {}

    void set(std::uint32_t i) {
        const std::size_t w = i >> 6;
        words_[w] |= bit(i);
        summary_[w >> 6] |= bit(static_cast<std::uint32_t>(w));
    }
    void clear(std::uint32_t i) {
        const std::size_t w = i >> 6;
        words_[w] &= ~bit(i);
        if (!words_[w]) summary_[w >> 6] &= ~bit(static_cast<std::uint32_t>(w));
    }

    // Lowest set index > i, or -1.
    [[nodiscard]] std::int64_t next_after(std::uint32_t i) const {
        std::size_t w = i >> 6;
        const unsigned b = i & 63;
        if (b != 63) {
            const std::uint64_t m = words_[w] & (~std::uint64_t{0} << (b + 1));
            if (m) return static_cast<std::int64_t>(w * 64 + __builtin_ctzll(m));
        }
        // Find the next non-empty word via the summary.
        std::size_t sw = (w + 1) >> 6;
        const unsigned sb = (w + 1) & 63;
        if (sw >= summary_.size()) return -1;
        std::uint64_t sm = sb ? summary_[sw] & (~std::uint64_t{0} << sb) : summary_[sw];
        while (!sm) {
            if (++sw >= summary_.size()) return -1;
            sm = summary_[sw];
        }
        const std::size_t nw = sw * 64 + __builtin_ctzll(sm);
        return static_cast<std::int64_t>(nw * 64 + __builtin_ctzll(words_[nw]));
    }

    // Highest set index < i, or -1.
    [[nodiscard]] std::int64_t prev_before(std::uint32_t i) const {
        const std::size_t w = i >> 6;
        const unsigned b = i & 63;
        if (b) {
            const std::uint64_t m = words_[w] & ((std::uint64_t{1} << b) - 1);
            if (m) return static_cast<std::int64_t>(w * 64 + 63 - __builtin_clzll(m));
        }
        if (w == 0) return -1;
        std::size_t pw = w - 1;  // highest word index to consider
        std::size_t sw = pw >> 6;
        const unsigned sb = pw & 63;
        std::uint64_t sm = summary_[sw] & (sb == 63 ? ~std::uint64_t{0} : ((std::uint64_t{1} << (sb + 1)) - 1));
        while (!sm) {
            if (sw == 0) return -1;
            sm = summary_[--sw];
        }
        const std::size_t nw = sw * 64 + 63 - __builtin_clzll(sm);
        return static_cast<std::int64_t>(nw * 64 + 63 - __builtin_clzll(words_[nw]));
    }

    [[nodiscard]] bool test(std::uint32_t i) const { return words_[i >> 6] & bit(i); }

private:
    static std::uint64_t bit(std::uint32_t i) { return std::uint64_t{1} << (i & 63); }
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
};

class FastEngine {
public:
    // capacity = max resting orders. Keep it near real depth: the id map is 2x
    // capacity, and a map that doesn't fit in cache costs ~2x per order
    // (measured: 23 ns/order at 16K, 27 at 64K, 41 at 1M).
    explicit FastEngine(Price band = 1u << 16, std::uint32_t capacity = 1u << 16)
        : band_(band), bids_(band), asks_(band), pool_(capacity), ids_(std::size_t{capacity} * 2) {
        free_.reserve(capacity);
        for (std::uint32_t i = capacity; i-- > 0;) free_.push_back(i);
    }

    template <class Sink>
    Result limit(OrderId id, Side side, Price price, Qty qty, Sink& sink) {
        if (qty == 0) [[unlikely]] return {Status::BadQty};
        if (price == 0 || price >= band_) [[unlikely]] return {Status::BadPrice};
        if (ids_.find(id)) [[unlikely]] return {Status::DuplicateId};
        Result r;
        Qty left = qty;
        if (side == Side::Buy) match<false>(asks_, id, price, left, r, sink);
        else match<true>(bids_, id, price, left, r, sink);
        if (left) {
            if (free_.empty()) [[unlikely]] {
                r.status = Status::Full;
                return r;
            }
            rest(side == Side::Buy ? bids_ : asks_, side == Side::Buy, id, price, left);
            r.rested = left;
        }
        return r;
    }

    template <class Sink>
    Result market(OrderId id, Side side, Qty qty, Sink& sink) {
        if (qty == 0) [[unlikely]] return {Status::BadQty};
        if (ids_.find(id)) [[unlikely]] return {Status::DuplicateId};
        Result r;
        Qty left = qty;
        if (side == Side::Buy) match<false>(asks_, id, band_, left, r, sink);
        else match<true>(bids_, id, 0, left, r, sink);
        return r;
    }

    Status cancel(OrderId id) {
        const std::uint32_t* pn = ids_.find(id);
        if (!pn) return Status::UnknownId;
        const std::uint32_t n = *pn;
        Node& o = pool_[n];
        BookSide& bs = o.side == Side::Buy ? bids_ : asks_;
        Level& L = bs.lv[o.price];
        if (o.prev != kNil) pool_[o.prev].next = o.next; else L.head = o.next;
        if (o.next != kNil) pool_[o.next].prev = o.prev; else L.tail = o.prev;
        L.shares -= o.qty;
        --L.count;
        if (L.head == kNil) empty_level(bs, o.side == Side::Buy, o.price);
        ids_.erase(id);
        free_.push_back(n);
        return Status::Ok;
    }

    [[nodiscard]] Quote best_bid() const { return quote(bids_); }
    [[nodiscard]] Quote best_ask() const { return quote(asks_); }
    [[nodiscard]] std::size_t order_count() const { return ids_.size(); }
    [[nodiscard]] std::size_t capacity() const { return pool_.size(); }

    // Best to worst.
    [[nodiscard]] std::vector<LevelSnapshot> snapshot(Side s) const {
        const BookSide& bs = s == Side::Buy ? bids_ : asks_;
        std::vector<LevelSnapshot> out;
        for (Price p = 1; p < band_; ++p) {
            if (!bs.bits.test(p)) continue;
            const Level& L = bs.lv[p];
            LevelSnapshot ls{p, L.shares, {}};
            for (std::uint32_t n = L.head; n != kNil; n = pool_[n].next) ls.queue.emplace_back(pool_[n].id, pool_[n].qty);
            out.push_back(std::move(ls));
        }
        if (s == Side::Buy) std::reverse(out.begin(), out.end());
        return out;
    }

private:
    static constexpr std::uint32_t kNil = ~std::uint32_t{0};

    struct Node {
        OrderId id;
        Qty qty;
        std::uint32_t prev, next;
        Price price;
        Side side;
    };

    struct Level {
        std::uint32_t head = kNil, tail = kNil;
        std::uint64_t shares = 0;
        std::uint32_t count = 0;
    };

    struct BookSide {
        explicit BookSide(Price band) : lv(band), bits(band) {}
        std::vector<Level> lv;
        TwoLevelBits bits;
        std::int64_t best = -1;  // bids: highest price; asks: lowest
    };

    // IsBid: the side being matched against (the passive side).
    template <bool IsBid, class Sink>
    void match(BookSide& bs, OrderId taker, Price limit, Qty& left, Result& r, Sink& sink) {
        while (left && bs.best >= 0) {
            const Price lp = static_cast<Price>(bs.best);
            if constexpr (IsBid) { if (lp < limit) break; }
            else { if (lp > limit) break; }
            Level& L = bs.lv[lp];
            while (left && L.head != kNil) {
                const std::uint32_t n = L.head;
                Node& m = pool_[n];
                const Qty x = std::min(left, m.qty);
                sink.on_fill(Fill{taker, m.id, lp, x});
                left -= x;
                m.qty -= x;
                L.shares -= x;
                r.filled += x;
                if (m.qty == 0) {
                    L.head = m.next;
                    if (L.head != kNil) pool_[L.head].prev = kNil; else L.tail = kNil;
                    --L.count;
                    ids_.erase(m.id);
                    free_.push_back(n);
                }
            }
            if (L.head == kNil) empty_level(bs, IsBid, lp);
        }
    }

    void rest(BookSide& bs, bool is_bid, OrderId id, Price p, Qty q) {
        const std::uint32_t n = free_.back();
        free_.pop_back();
        Level& L = bs.lv[p];
        pool_[n] = Node{id, q, L.tail, kNil, p, is_bid ? Side::Buy : Side::Sell};
        if (L.tail != kNil) pool_[L.tail].next = n; else L.head = n;
        L.tail = n;
        L.shares += q;
        if (L.count++ == 0) {
            bs.bits.set(p);
            if (bs.best < 0 || (is_bid ? p > bs.best : p < bs.best)) bs.best = p;
        }
        ids_.insert(id, n);
    }

    void empty_level(BookSide& bs, bool is_bid, Price p) {
        bs.bits.clear(p);
        if (bs.best == static_cast<std::int64_t>(p)) bs.best = is_bid ? bs.bits.prev_before(p) : bs.bits.next_after(p);
    }

    [[nodiscard]] static Quote quote(const BookSide& bs) {
        if (bs.best < 0) return {};
        return {static_cast<Price>(bs.best), bs.lv[bs.best].shares};
    }

    Price band_;
    BookSide bids_, asks_;
    std::vector<Node> pool_;
    std::vector<std::uint32_t> free_;
    lob::BasicFlatMap<std::uint32_t> ids_;
};

}  // namespace engine
