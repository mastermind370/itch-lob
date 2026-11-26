// engine_bench: benchmark and verify the matching engines.
//
//   engine_bench [--orders N] [--seed S] [--diff-seeds K] [--diff-orders M]
//
// 1. Differential test: K seeds x M random orders, fast engine vs reference,
//    every result, fill and best quote compared after every order.
// 2. Throughput: the same pre-generated N orders through each engine.
// 3. Latency: every order through the fast engine timed individually.
// Writes results/engine.json.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#include "bench/latency.hpp"
#include "engine/diff.hpp"
#include "engine/fast_engine.hpp"
#include "engine/order_flow.hpp"
#include "engine/ref_engine.hpp"

using namespace engine;

namespace {

struct Run {
    double seconds = 0;
    std::uint64_t fills = 0, shares = 0;
};

template <class E>
Run run(const std::vector<Op>& ops, int reps) {
    std::vector<double> t;
    Run out;
    for (int r = 0; r < reps; ++r) {
        auto e = std::make_unique<E>();
        CountingSink sink;
        const auto t0 = std::chrono::steady_clock::now();
        for (const Op& op : ops) apply(*e, op, sink);
        t.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        out.fills = sink.fills;
        out.shares = sink.shares;
    }
    std::sort(t.begin(), t.end());
    out.seconds = t.front();
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t n = 10'000'000, seed = 42, diff_seeds = 10, diff_orders = 1'000'000;
    int reps = 3;
    for (int i = 1; i < argc; ++i) {
        auto num = [&] { return std::strtoull(argv[++i], nullptr, 10); };
        if (!std::strcmp(argv[i], "--orders") && i + 1 < argc) n = num();
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = num();
        else if (!std::strcmp(argv[i], "--diff-seeds") && i + 1 < argc) diff_seeds = num();
        else if (!std::strcmp(argv[i], "--diff-orders") && i + 1 < argc) diff_orders = num();
        else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };

    // 1. Differential test.
    std::printf("== differential test: %llu seeds x %llu random orders, fast vs reference\n", u(diff_seeds), u(diff_orders));
    std::uint64_t passed = 0, diff_fills = 0;
    for (std::uint64_t s = 1; s <= diff_seeds; ++s) {
        FlowConfig cfg;
        cfg.seed = 1000 + s;
        if (s % 2 == 0) {  // alternate seeds use thin, aggressive books
            cfg.target_open = 100;
            cfg.cross_ppt = 300;
            cfg.market_ppt = 100;
        }
        const DiffReport r = run_diff(cfg, diff_orders, 10'000);
        diff_fills += r.fills;
        if (r.ok()) ++passed;
        else std::printf("   seed %llu FAILED at order %llu: %s\n", u(cfg.seed), u(r.first_bad_op), r.first_error.c_str());
        std::printf("   seed %llu: %s, %llu fills, %llu full-book snapshots compared\n", u(cfg.seed), r.ok() ? "passed" : "FAILED",
                    u(r.fills), u(r.snapshots));
    }
    std::printf("   %llu / %llu passed (%llu orders, %llu fills checked)\n\n", u(passed), u(diff_seeds),
                u(diff_seeds * diff_orders), u(diff_fills));

    // 2. Throughput.
    FlowConfig cfg;
    cfg.seed = seed;
    OrderFlow flow(cfg);
    const std::vector<Op> ops = flow.take(n);
    std::uint64_t limits = 0, markets = 0, cancels = 0;
    for (const Op& op : ops) (op.kind == Op::Limit ? limits : op.kind == Op::Market ? markets : cancels)++;
    std::printf("== throughput: %llu orders (%llu limit, %llu market, %llu cancel), best of %d\n", u(n), u(limits),
                u(markets), u(cancels), reps);
    const Run rf = run<FastEngine>(ops, reps);
    const Run rr = run<RefEngine>(ops, reps);
    const double fast_ops = n / rf.seconds, ref_ops = n / rr.seconds;
    std::printf("   fast  %7.2f M orders/s  %6.1f ns/order  %6.2f M fills/s\n", fast_ops / 1e6, rf.seconds * 1e9 / n,
                rf.fills / rf.seconds / 1e6);
    std::printf("   ref   %7.2f M orders/s  %6.1f ns/order  %6.2f M fills/s\n", ref_ops / 1e6, rr.seconds * 1e9 / n,
                rr.fills / rr.seconds / 1e6);
    std::printf("   speedup %.2fx; %llu fills, %llu shares matched (identical in both: %s)\n\n", rr.seconds / rf.seconds,
                u(rf.fills), u(rf.shares), rf.fills == rr.fills && rf.shares == rr.shares ? "yes" : "NO");

    // 3. Latency.
    bench::Histogram h;
    {
        auto e = std::make_unique<FastEngine>();
        CountingSink sink;
        for (const Op& op : ops) {
            const std::uint64_t t0 = bench::ticks();
            apply(*e, op, sink);
            const std::uint64_t t1 = bench::ticks();
            h.record(t1 - t0);
        }
    }
    const double npt = bench::ns_per_tick(), res = bench::resolution_ns();
    const double p50 = h.percentile(50) * npt, p90 = h.percentile(90) * npt, p99 = h.percentile(99) * npt,
                 p999 = h.percentile(99.9) * npt, p9999 = h.percentile(99.99) * npt, mx = h.max() * npt;
    std::printf("== fast engine per-order latency (clock resolution %.1f ns)\n", res);
    std::printf("   mean %.1f | p50 %.1f | p90 %.1f | p99 %.1f | p99.9 %.1f | p99.99 %.1f | max %.1f ns\n",
                h.mean() * npt, p50, p90, p99, p999, p9999, mx);

    std::ofstream js("results/engine.json");
    js << "{\n  \"stage\": 4,\n  \"diff_seeds\": " << diff_seeds << ",\n  \"diff_orders_per_seed\": " << diff_orders
       << ",\n  \"diff_passed\": " << passed << ",\n  \"diff_fills_checked\": " << diff_fills << ",\n  \"orders\": " << n
       << ",\n  \"limits\": " << limits << ",\n  \"markets\": " << markets << ",\n  \"cancels\": " << cancels
       << ",\n  \"fills\": " << rf.fills << ",\n  \"shares_matched\": " << rf.shares
       << ",\n  \"fast_orders_per_s\": " << fast_ops << ",\n  \"fast_ns_per_order\": " << rf.seconds * 1e9 / n
       << ",\n  \"fast_fills_per_s\": " << rf.fills / rf.seconds << ",\n  \"ref_orders_per_s\": " << ref_ops
       << ",\n  \"ref_ns_per_order\": " << rr.seconds * 1e9 / n << ",\n  \"speedup\": " << rr.seconds / rf.seconds
       << ",\n  \"latency_resolution_ns\": " << res << ",\n  \"latency_mean_ns\": " << h.mean() * npt
       << ",\n  \"latency_p50_ns\": " << p50 << ",\n  \"latency_p90_ns\": " << p90 << ",\n  \"latency_p99_ns\": " << p99
       << ",\n  \"latency_p99_9_ns\": " << p999 << ",\n  \"latency_p99_99_ns\": " << p9999 << ",\n  \"latency_max_ns\": " << mx
       << "\n}\n";
    std::printf("\nwrote results/engine.json\n");
    return passed == diff_seeds ? 0 : 1;
}
