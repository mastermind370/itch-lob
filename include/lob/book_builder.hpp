// Rebuilds per-stock order books from an ITCH stream.
//
// BookBuilder<Book, OrderMap> is an ITCH handler. It keeps an order-ID map
// (ref -> where the order rests and how many shares are left) and applies every
// add, execute, cancel, delete and replace to the right Book. Book and OrderMap
// are template parameters, so the baseline (MapBook + StdOrderMap) and the fast
// version (FastBook + FlatOrderMap) share this exact code.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "itch/parser.hpp"
#include "lob/map_book.hpp"
#include "lob/order_map.hpp"

namespace lob {

struct LevelKey {
    std::uint16_t locate;
    Side side;
    Price price;
};

// Anomalies that a correct feed + correct builder should never produce.
struct FeedAnomalies {
    std::uint64_t duplicate_add = 0;
    std::uint64_t unknown_ref = 0;
    std::uint64_t overfill = 0;  // executed/cancelled more shares than were left
    [[nodiscard]] std::uint64_t total() const { return duplicate_add + unknown_ref + overfill; }
};

inline constexpr std::size_t kMaxLocates = 65536;

template <class Book, class OrderMap = StdOrderMap>
class BookBuilder : public itch::NullHandler {
public:
    template <class... MapArgs>
    explicit BookBuilder(MapArgs&&... args)
        : books_(kMaxLocates), symbols_(kMaxLocates), orders_(std::forward<MapArgs>(args)...) {}

    void on_stock_directory(const itch::StockDirectory& m) {
        symbols_[m.hdr.stock_locate] = std::string(itch::trim_symbol(m.stock));
    }

    void on_add(const itch::AddOrder& m) {
        touched_ = 0;
        const RestingOrder o{m.hdr.stock_locate, m.side, m.price, m.shares};
        if (!orders_.insert(m.order_ref, o)) { ++anom_.duplicate_add; return; }
        books_[o.locate].add(o.side, o.price, o.shares);
        touch(o);
    }

    void on_executed(const itch::OrderExecuted& m) { reduce(m.order_ref, m.executed_shares); }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) { reduce(m.order_ref, m.executed_shares); }
    void on_cancel(const itch::OrderCancel& m) { reduce(m.order_ref, m.cancelled_shares); }

    void on_delete(const itch::OrderDelete& m) {
        touched_ = 0;
        RestingOrder* p = orders_.find(m.order_ref);
        if (!p) { ++anom_.unknown_ref; return; }
        const RestingOrder o = *p;
        books_[o.locate].reduce(o.side, o.price, o.shares, true);
        orders_.erase(m.order_ref);
        touch(o);
    }

    // Replace = full delete of the old order + add of the new one, same stock and side.
    // The new order loses time priority, which matters for the matching engine later.
    void on_replace(const itch::OrderReplace& m) {
        touched_ = 0;
        RestingOrder* p = orders_.find(m.original_order_ref);
        if (!p) { ++anom_.unknown_ref; return; }
        const RestingOrder old = *p;
        books_[old.locate].reduce(old.side, old.price, old.shares, true);
        orders_.erase(m.original_order_ref);
        touch(old);
        const RestingOrder nu{old.locate, old.side, m.price, m.shares};
        if (!orders_.insert(m.new_order_ref, nu)) { ++anom_.duplicate_add; return; }
        books_[nu.locate].add(nu.side, nu.price, nu.shares);
        touch(nu);
    }

    [[nodiscard]] const Book& book(std::uint16_t locate) const { return books_[locate]; }
    [[nodiscard]] const std::string& symbol(std::uint16_t locate) const { return symbols_[locate]; }
    [[nodiscard]] const OrderMap& orders() const { return orders_; }
    [[nodiscard]] const FeedAnomalies& anomalies() const { return anom_; }

    // Levels changed by the last order message (replace touches two).
    [[nodiscard]] std::size_t touched_count() const { return touched_; }
    [[nodiscard]] const LevelKey& touched(std::size_t i) const { return touched_keys_[i]; }

    [[nodiscard]] int locate_of(const std::string& sym) const {
        for (std::size_t i = 0; i < symbols_.size(); ++i)
            if (symbols_[i] == sym) return static_cast<int>(i);
        return -1;
    }

private:
    void reduce(std::uint64_t ref, std::uint32_t q) {
        touched_ = 0;
        RestingOrder* o = orders_.find(ref);
        if (!o) { ++anom_.unknown_ref; return; }
        if (q > o->shares) { ++anom_.overfill; q = o->shares; }
        o->shares -= q;
        const RestingOrder copy = *o;
        books_[copy.locate].reduce(copy.side, copy.price, q, copy.shares == 0);
        touch(copy);
        if (copy.shares == 0) orders_.erase(ref);
    }

    void touch(const RestingOrder& o) { touched_keys_[touched_++] = LevelKey{o.locate, o.side, o.price}; }

    std::vector<Book> books_;
    std::vector<std::string> symbols_;
    OrderMap orders_;
    FeedAnomalies anom_;
    LevelKey touched_keys_[2]{};
    std::size_t touched_ = 0;
};

}  // namespace lob
