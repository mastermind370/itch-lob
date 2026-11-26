// Two-thread ITCH pipeline: parser thread -> SPSC queue -> book thread.
//
// The producer walks the mapped file, decodes each frame and pushes a compact
// 40-byte Event for every order message (A, F, E, C, X, D, U) and stock
// directory entry. The consumer pops events and applies them to a
// BookBuilder<FastBook, FlatOrderMap>. Each event carries the tick at which the
// producer started on it, so the consumer can measure end-to-end latency:
// parse + enqueue + cross-core transfer + dequeue + book update.
//
// run_inline() does the same work on one thread with no queue, for comparison.
// book_digest() hashes every book's top of book and depth, so the two runs can
// be checked for identical results.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <thread>

#include "bench/latency.hpp"
#include "itch/parser.hpp"
#include "lob/book_builder.hpp"
#include "lob/fast_book.hpp"
#include "lob/order_map.hpp"
#include "pipeline/cpu.hpp"
#include "pipeline/spsc_queue.hpp"

namespace pipeline {

using Builder = lob::BookBuilder<lob::FastBook, lob::FlatOrderMap>;

struct Event {
    std::uint64_t t0;      // bench::ticks() when the producer started on this message
    std::uint64_t ref;     // order ref (original ref for replace)
    std::uint64_t ref2;    // replace: new ref; stock directory: raw 8-byte symbol
    std::uint32_t shares;
    std::uint32_t price;
    std::uint16_t locate;
    char type;             // ITCH type, or 0 = end of stream
    itch::Side side;
};
static_assert(sizeof(Event) == 40);

[[nodiscard]] inline bool is_order_type(char t) {
    return t == 'A' || t == 'F' || t == 'E' || t == 'C' || t == 'X' || t == 'D' || t == 'U';
}

// Applies one event to the book builder.
inline void apply(Builder& b, const Event& e) {
    const itch::Header h{e.type, e.locate, 0, 0};
    switch (e.type) {
        case 'A':
        case 'F': b.on_add(itch::AddOrder{h, e.ref, e.side, e.shares, {}, e.price, {}}); return;
        case 'E': b.on_executed(itch::OrderExecuted{h, e.ref, e.shares, 0}); return;
        case 'C': b.on_executed_with_price(itch::OrderExecutedWithPrice{h, e.ref, e.shares, 0, 'Y', e.price}); return;
        case 'X': b.on_cancel(itch::OrderCancel{h, e.ref, e.shares}); return;
        case 'D': b.on_delete(itch::OrderDelete{h, e.ref}); return;
        case 'U': b.on_replace(itch::OrderReplace{h, e.ref, e.ref2, e.shares, e.price}); return;
        case 'R': {
            char sym[8];
            std::memcpy(sym, &e.ref2, 8);
            b.on_stock_directory(itch::StockDirectory{h, std::string_view(sym, 8), 'Q', 'N', 100, 'N'});
            return;
        }
        default: return;
    }
}

// ITCH handler that turns decoded messages into Events and pushes them.
template <class Queue>
struct Producer : itch::NullHandler {
    Queue& q;
    std::uint64_t t0 = 0;
    std::uint64_t full_spins = 0;
    std::uint64_t pushed = 0;

    explicit Producer(Queue& queue) : q(queue) {}

    void push(const Event& e) {
        while (!q.try_push(e)) [[unlikely]] {
            ++full_spins;
            cpu_relax();
        }
        ++pushed;
    }
    void on_add(const itch::AddOrder& m) { push({t0, m.order_ref, 0, m.shares, m.price, m.hdr.stock_locate, m.hdr.type, m.side}); }
    void on_executed(const itch::OrderExecuted& m) {
        push({t0, m.order_ref, 0, m.executed_shares, 0, m.hdr.stock_locate, 'E', itch::Side::Buy});
    }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) {
        push({t0, m.order_ref, 0, m.executed_shares, m.execution_price, m.hdr.stock_locate, 'C', itch::Side::Buy});
    }
    void on_cancel(const itch::OrderCancel& m) {
        push({t0, m.order_ref, 0, m.cancelled_shares, 0, m.hdr.stock_locate, 'X', itch::Side::Buy});
    }
    void on_delete(const itch::OrderDelete& m) { push({t0, m.order_ref, 0, 0, 0, m.hdr.stock_locate, 'D', itch::Side::Buy}); }
    void on_replace(const itch::OrderReplace& m) {
        push({t0, m.original_order_ref, m.new_order_ref, m.shares, m.price, m.hdr.stock_locate, 'U', itch::Side::Buy});
    }
    void on_stock_directory(const itch::StockDirectory& m) {
        std::uint64_t raw = 0;
        std::memcpy(&raw, m.stock.data(), 8);
        push({t0, 0, raw, 0, 0, m.hdr.stock_locate, 'R', itch::Side::Buy});
    }
};

// FNV-1a over every book's best bid/ask (price and size) and level counts, plus the live-order count.
[[nodiscard]] inline std::uint64_t book_digest(const Builder& b) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    for (std::size_t i = 0; i < lob::kMaxLocates; ++i) {
        const auto& bk = b.book(static_cast<std::uint16_t>(i));
        const auto bid = bk.best_bid(), ask = bk.best_ask();
        mix(i);
        mix(bid.price);
        mix(bid.shares);
        mix(ask.price);
        mix(ask.shares);
        mix(bk.level_count(itch::Side::Buy));
        mix(bk.level_count(itch::Side::Sell));
    }
    mix(b.orders().size());
    return h;
}

struct RunStats {
    double seconds = 0;
    std::uint64_t messages = 0;        // ITCH frames read
    std::uint64_t events = 0;          // order + directory messages applied
    std::uint64_t full_spins = 0;      // producer found the queue full
    std::uint64_t empty_spins = 0;     // consumer found the queue empty
    std::uint64_t paced_late = 0;      // paced mode: producer was already past the send time
    std::uint64_t digest = 0;
    const char* placement = "";
};

// One thread, no queue. If h is given, each order message is timed individually.
inline RunStats run_inline(const std::uint8_t* data, std::size_t len, bench::Histogram* h = nullptr) {
    auto b = std::make_unique<Builder>();
    RunStats s;
    const auto w0 = std::chrono::steady_clock::now();
    if (!h) {
        const auto r = itch::parse_frames(data, len, *b);
        s.messages = r.messages;
    } else {
        std::size_t off = 0;
        while (len - off >= 2) {
            const std::size_t n = itch::load_be16(data + off);
            if (len - off - 2 < n) break;
            const std::uint8_t* m = data + off + 2;
            const bool timed = is_order_type(static_cast<char>(m[0]));
            const std::uint64_t t0 = bench::ticks();
            itch::decode_message(m, n, *b);
            if (timed) h->record(bench::ticks() - t0);
            off += 2 + n;
            ++s.messages;
        }
    }
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    s.events = s.messages;
    s.digest = book_digest(*b);
    return s;
}

// Two threads joined by Queue. interval_ticks > 0 paces the producer to one ITCH
// message per interval; 0 runs flat out. If h is given, the consumer records the
// end-to-end latency of every event.
template <class Queue>
RunStats run_pipeline(const std::uint8_t* data, std::size_t len, std::uint64_t interval_ticks = 0,
                      bench::Histogram* h = nullptr) {
    auto q = std::make_unique<Queue>();
    auto b = std::make_unique<Builder>();
    RunStats s;
    std::atomic<int> ready{0};
    std::uint64_t empty_spins = 0, events = 0;

    std::thread consumer([&] {
        place_thread(1);
        ready.fetch_add(1);
        while (ready.load() < 2) cpu_relax();
        Event e;
        while (true) {
            if (!q->try_pop(e)) {
                ++empty_spins;
                cpu_relax();
                continue;
            }
            if (e.type == 0) break;
            apply(*b, e);
            if (h) h->record(bench::ticks() - e.t0);
            ++events;
        }
    });

    s.placement = place_thread(0);
    Producer<Queue> prod(*q);
    ready.fetch_add(1);
    while (ready.load() < 2) cpu_relax();

    const auto w0 = std::chrono::steady_clock::now();
    std::uint64_t next_send = bench::ticks();
    std::size_t off = 0;
    while (len - off >= 2) {
        const std::size_t n = itch::load_be16(data + off);
        if (len - off - 2 < n) break;
        if (interval_ticks) {
            next_send += interval_ticks;
            std::uint64_t now = bench::ticks();
            if (now > next_send) ++s.paced_late;
            while (now < next_send) {
                cpu_relax();
                now = bench::ticks();
            }
        }
        prod.t0 = bench::ticks();
        itch::decode_message(data + off + 2, n, prod);
        off += 2 + n;
        ++s.messages;
    }
    prod.push(Event{0, 0, 0, 0, 0, 0, 0, itch::Side::Buy});
    consumer.join();
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    s.events = events;
    s.full_spins = prod.full_spins;
    s.empty_spins = empty_spins;
    s.digest = book_digest(*b);
    return s;
}

}  // namespace pipeline
