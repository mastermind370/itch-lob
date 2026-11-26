// Differential test: drives FastEngine and RefEngine with the same random
// order flow and checks, after every op, that the result, the exact fill
// sequence (taker, maker, price, qty) and both best quotes agree. Every
// `snapshot_every` ops it also compares both full books, including the order
// of every queue, which is where a price-time priority bug would show up.
// It also checks invariants: the book is never crossed after an op, and
// shares are conserved (incoming = filled + rested + cancelled).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "engine/fast_engine.hpp"
#include "engine/order_flow.hpp"
#include "engine/ref_engine.hpp"

namespace engine {

struct DiffReport {
    std::uint64_t ops = 0;
    std::uint64_t fills = 0;
    std::uint64_t filled_shares = 0;
    std::uint64_t snapshots = 0;
    std::uint64_t mismatches = 0;
    std::uint64_t first_bad_op = 0;
    std::string first_error;
    [[nodiscard]] bool ok() const { return mismatches == 0; }
};

inline DiffReport run_diff(const FlowConfig& cfg, std::uint64_t n_ops, std::uint64_t snapshot_every = 10'000,
                           std::uint32_t capacity = 1u << 16) {
    DiffReport rep;
    auto fast = std::make_unique<FastEngine>(cfg.band, capacity);
    auto ref = std::make_unique<RefEngine>(cfg.band);
    OrderFlow flow(cfg);
    VectorSink fs, rs;
    fs.fills.reserve(1024);
    rs.fills.reserve(1024);

    auto fail = [&](std::uint64_t i, const std::string& what) {
        if (rep.mismatches++ == 0) {
            rep.first_bad_op = i;
            rep.first_error = what;
        }
    };

    for (std::uint64_t i = 0; i < n_ops; ++i) {
        const Op op = flow.next();
        fs.fills.clear();
        rs.fills.clear();
        const Result a = apply(*fast, op, fs);
        const Result b = apply(*ref, op, rs);
        ++rep.ops;
        rep.fills += rs.fills.size();
        if (!(a == b)) fail(i, std::string("result differs: fast ") + status_name(a.status) + " vs ref " + status_name(b.status));
        if (fs.fills != rs.fills) fail(i, "fill sequence differs (" + std::to_string(fs.fills.size()) + " vs " +
                                              std::to_string(rs.fills.size()) + " fills)");
        std::uint64_t sh = 0;
        for (const Fill& f : rs.fills) sh += f.qty;
        rep.filled_shares += sh;
        if (sh != b.filled) fail(i, "fills don't add up to result.filled");
        if (op.kind != Op::Cancel && b.status == Status::Ok && b.filled + b.rested > op.qty)
            fail(i, "more shares filled+rested than sent");
        if (op.kind == Op::Limit && b.status == Status::Ok && b.filled + b.rested != op.qty)
            fail(i, "limit order shares not conserved");
        if (!(fast->best_bid() == ref->best_bid()) || !(fast->best_ask() == ref->best_ask())) fail(i, "best quote differs");
        const Quote bb = ref->best_bid(), ba = ref->best_ask();
        if (bb.price && ba.price && bb.price >= ba.price) fail(i, "book crossed after matching");
        if (fast->order_count() != ref->order_count()) fail(i, "open order count differs");
        if ((i + 1) % snapshot_every == 0 || i + 1 == n_ops) {
            ++rep.snapshots;
            if (fast->snapshot(Side::Buy) != ref->snapshot(Side::Buy) || fast->snapshot(Side::Sell) != ref->snapshot(Side::Sell))
                fail(i, "full book or queue order differs");
        }
    }
    return rep;
}

}  // namespace engine
