// Correctness checker for book reconstruction.
//
// CheckedReplay<Book, OrderMap> wraps a BookBuilder. After every order message it
//  1. compares the shares and order count of each touched price level against an
//     independently maintained level-sum table (separate code, its own order map);
//  2. compares the touched stock's best bid and best ask (price and size) against
//     a reference std::map book fed the same message.
// full_audit() rebuilds every level from the builder's live orders and
// compares against every level in every book, in both directions.
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_map>

#include "lob/book_builder.hpp"

namespace lob {

struct CheckStats {
    std::uint64_t level_checks = 0;
    std::uint64_t level_mismatches = 0;
    std::uint64_t audit_levels = 0;
    std::uint64_t audit_mismatches = 0;
    std::uint64_t crossed_checks = 0;
    std::uint64_t best_checks = 0;
    std::uint64_t best_mismatches = 0;  // books whose best bid >= best ask after a message (reported, not an error)
};

namespace detail {
[[nodiscard]] inline std::uint64_t level_hash_key(std::uint16_t locate, Side s, Price p) noexcept {
    return (std::uint64_t{locate} << 33) | (std::uint64_t{s == Side::Buy} << 32) | p;
}
}  // namespace detail

template <class Book, class OrderMap = StdOrderMap>
class CheckedReplay : public itch::NullHandler {
public:
    explicit CheckedReplay(BookBuilder<Book, OrderMap>& b) : b_(b), ref_book_(std::make_unique<BookBuilder<MapBook>>()) {}

    void on_stock_directory(const itch::StockDirectory& m) { b_.on_stock_directory(m); }

    void on_add(const itch::AddOrder& m) {
        b_.on_add(m);
        ref_book_->on_add(m);
        ref_.emplace(m.order_ref, Ref{m.hdr.stock_locate, m.side, m.price, m.shares});
        bump(m.hdr.stock_locate, m.side, m.price, m.shares, 1);
        verify(m.hdr.stock_locate);
    }
    void on_executed(const itch::OrderExecuted& m) {
        b_.on_executed(m);
        ref_book_->on_executed(m);
        take(m.order_ref, m.executed_shares);
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        b_.on_executed_with_price(m);
        ref_book_->on_executed_with_price(m);
        take(m.order_ref, m.executed_shares);
    }
    void on_cancel(const itch::OrderCancel& m) {
        b_.on_cancel(m);
        ref_book_->on_cancel(m);
        take(m.order_ref, m.cancelled_shares);
    }
    void on_delete(const itch::OrderDelete& m) {
        b_.on_delete(m);
        ref_book_->on_delete(m);
        auto it = ref_.find(m.order_ref);
        if (it == ref_.end()) return;
        const Ref r = it->second;
        ref_.erase(it);
        bump(r.locate, r.side, r.price, -static_cast<std::int64_t>(r.shares), -1);
        verify(r.locate);
    }
    void on_replace(const itch::OrderReplace& m) {
        b_.on_replace(m);
        ref_book_->on_replace(m);
        auto it = ref_.find(m.original_order_ref);
        if (it == ref_.end()) return;
        const Ref r = it->second;
        ref_.erase(it);
        bump(r.locate, r.side, r.price, -static_cast<std::int64_t>(r.shares), -1);
        ref_.emplace(m.new_order_ref, Ref{r.locate, r.side, m.price, m.shares});
        bump(r.locate, r.side, m.price, m.shares, 1);
        verify(r.locate);
    }

    // Rebuilds every level from the builder's live orders and compares it with the books.
    void full_audit() {
        std::unordered_map<std::uint64_t, Sum> rebuilt;
        rebuilt.reserve(b_.orders().size());
        b_.orders().for_each([&](std::uint64_t, const RestingOrder& o) {
            Sum& s = rebuilt[detail::level_hash_key(o.locate, o.side, o.price)];
            s.shares += o.shares;
            ++s.orders;
        });
        std::uint64_t book_levels = 0;
        for (std::size_t loc = 0; loc < kMaxLocates; ++loc) {
            const Book& bk = b_.book(static_cast<std::uint16_t>(loc));
            for (Side side : {Side::Buy, Side::Sell}) {
                bk.for_each_level(side, [&](Price p, const Level& l) {
                    ++book_levels;
                    ++st_.audit_levels;
                    auto it = rebuilt.find(detail::level_hash_key(static_cast<std::uint16_t>(loc), side, p));
                    if (it == rebuilt.end() || it->second.shares != l.shares || it->second.orders != l.orders)
                        report_audit(loc, side, p);
                });
            }
        }
        if (book_levels != rebuilt.size()) ++st_.audit_mismatches;  // a level exists in the orders but not the book
    }

    [[nodiscard]] const CheckStats& stats() const { return st_; }
    [[nodiscard]] bool ok() const {
        return st_.level_mismatches == 0 && st_.audit_mismatches == 0 && st_.best_mismatches == 0 &&
               b_.anomalies().total() == 0;
    }

private:
    struct Ref {
        std::uint16_t locate;
        Side side;
        Price price;
        std::uint32_t shares;
    };
    struct Sum {
        std::uint64_t shares = 0;
        std::uint32_t orders = 0;
    };

    void take(std::uint64_t ref, std::uint32_t q) {
        auto it = ref_.find(ref);
        if (it == ref_.end()) return;
        Ref& r = it->second;
        if (q > r.shares) q = r.shares;
        r.shares -= q;
        const bool gone = r.shares == 0;
        const Ref copy = r;
        if (gone) ref_.erase(it);
        bump(copy.locate, copy.side, copy.price, -static_cast<std::int64_t>(q), gone ? -1 : 0);
        verify(copy.locate);
    }

    void bump(std::uint16_t loc, Side s, Price p, std::int64_t dq, int dn) {
        const std::uint64_t k = detail::level_hash_key(loc, s, p);
        Sum& sum = sums_[k];
        sum.shares = static_cast<std::uint64_t>(static_cast<std::int64_t>(sum.shares) + dq);
        sum.orders = static_cast<std::uint32_t>(static_cast<int>(sum.orders) + dn);
        if (sum.orders == 0) sums_.erase(k);
    }

    // Checks every level the builder says it touched, plus top-of-book sanity.
    void verify(std::uint16_t loc) {
        for (std::size_t i = 0; i < b_.touched_count(); ++i) {
            const LevelKey& k = b_.touched(i);
            const Level got = b_.book(k.locate).level(k.side, k.price);
            auto it = sums_.find(detail::level_hash_key(k.locate, k.side, k.price));
            const Sum want = it == sums_.end() ? Sum{} : it->second;
            ++st_.level_checks;
            if (got.shares != want.shares || got.orders != want.orders) {
                if (st_.level_mismatches++ < 10)
                    std::fprintf(stderr, "level mismatch locate=%u side=%c price=%u: book %llu/%u, expected %llu/%u\n",
                                 k.locate, static_cast<char>(k.side), k.price,
                                 static_cast<unsigned long long>(got.shares), got.orders,
                                 static_cast<unsigned long long>(want.shares), want.orders);
            }
        }
        const Book& bk = b_.book(loc);
        const Quote bid = bk.best_bid(), ask = bk.best_ask();
        if (bid.price && ask.price && bid.price >= ask.price) ++st_.crossed_checks;

        const MapBook& rb = ref_book_->book(loc);
        const Quote rbid = rb.best_bid(), rask = rb.best_ask();
        ++st_.best_checks;
        if (bid.price != rbid.price || bid.shares != rbid.shares || ask.price != rask.price ||
            ask.shares != rask.shares) {
            if (st_.best_mismatches++ < 10)
                std::fprintf(stderr, "best mismatch locate=%u: book %u x %llu / %u x %llu, reference %u x %llu / %u x %llu\n",
                             loc, bid.price, static_cast<unsigned long long>(bid.shares), ask.price,
                             static_cast<unsigned long long>(ask.shares), rbid.price,
                             static_cast<unsigned long long>(rbid.shares), rask.price,
                             static_cast<unsigned long long>(rask.shares));
        }
    }

    void report_audit(std::size_t loc, Side side, Price p) {
        if (st_.audit_mismatches++ < 10)
            std::fprintf(stderr, "audit mismatch locate=%zu side=%c price=%u\n", loc, static_cast<char>(side), p);
    }

    BookBuilder<Book, OrderMap>& b_;
    std::unique_ptr<BookBuilder<MapBook>> ref_book_;
    std::unordered_map<std::uint64_t, Ref> ref_;
    std::unordered_map<std::uint64_t, Sum> sums_;
    CheckStats st_;
};

}  // namespace lob
