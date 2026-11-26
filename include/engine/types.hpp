// Shared types for the matching engines.
//
// Prices are integer ticks in [1, band). Both engines take the same commands
// and must produce identical fills, results and books; the randomized
// differential test checks this.
//
// Semantics:
//   limit(id, side, price, qty): matches against the opposite side while it is
//       marketable, at the resting order's price, best price first and oldest
//       order first within a price (price-time priority). Any remainder rests
//       at the back of its price level's queue.
//   market(id, side, qty): matches with no price limit; any remainder is
//       cancelled (it never rests).
//   cancel(id): removes a resting order.
#pragma once

#include <cstdint>
#include <vector>

#include "itch/messages.hpp"

namespace engine {

using Price = std::uint32_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;
using itch::Side;

enum class Status : std::uint8_t {
    Ok,
    DuplicateId,  // id already resting
    UnknownId,    // cancel of an id that isn't resting
    BadPrice,     // 0 or outside the price band
    BadQty,       // 0
    Full,         // fast engine's order pool is full; the unfilled remainder was cancelled
};

struct Fill {
    OrderId taker;
    OrderId maker;
    Price price;
    Qty qty;
    bool operator==(const Fill&) const = default;
};

struct Result {
    Status status = Status::Ok;
    Qty filled = 0;
    Qty rested = 0;  // limit only; for market orders the unfilled part is cancelled
    bool operator==(const Result&) const = default;
};

// price == 0 means the side is empty.
struct Quote {
    Price price = 0;
    std::uint64_t shares = 0;
    bool operator==(const Quote&) const = default;
};

// A full price level including its queue, oldest order first. For tests.
struct LevelSnapshot {
    Price price;
    std::uint64_t shares;
    std::vector<std::pair<OrderId, Qty>> queue;
    bool operator==(const LevelSnapshot&) const = default;
};

struct NullSink {
    void on_fill(const Fill&) {}
};

struct CountingSink {
    std::uint64_t fills = 0;
    std::uint64_t shares = 0;
    void on_fill(const Fill& f) {
        ++fills;
        shares += f.qty;
    }
};

struct VectorSink {
    std::vector<Fill> fills;
    void on_fill(const Fill& f) { fills.push_back(f); }
};

[[nodiscard]] inline const char* status_name(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::DuplicateId: return "duplicate_id";
        case Status::UnknownId: return "unknown_id";
        case Status::BadPrice: return "bad_price";
        case Status::BadQty: return "bad_qty";
        case Status::Full: return "full";
    }
    return "?";
}

}  // namespace engine
