// ablation: what each order-book optimization buys, on one ITCH file.
//
//   ablation <file> [--reps N]
//
// Variants, each a BookBuilder<Book, OrderMap> over the same code:
//   baseline        std::map book          + std::unordered_map orders
//   +flat orders    std::map book          + flat open-addressing order map
//   ladder only     flat ladder, recenter  + std::unordered_map orders
//   +flat ladder    flat ladder, no recenter + flat order map
//   +recenter       flat ladder, recenter  + flat order map        (2048-tick window)
//   window 8K/32K   the same with an 8,192- or 32,768-tick window
// Ladder variants also report how much of their mmap'd ladders is resident (mincore).
// Runs are interleaved round-robin (v0 v1 .. v4 v0 v1 ..) so drift in clock
// speed or temperature hits every variant equally. Reports median and range of
// ns/msg, counters, and the final book digest (must be identical for all).
// Writes results/ablation.json.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "bench/counters.hpp"
#include "bench/stats.hpp"
#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "lob/book_builder.hpp"
#include "lob/fast_book.hpp"
#include "lob/map_book.hpp"
#include "lob/order_map.hpp"

namespace {

template <class Builder>
std::uint64_t digest(const Builder& b) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    for (std::size_t i = 0; i < lob::kMaxLocates; ++i) {
        const auto& bk = b.book(static_cast<std::uint16_t>(i));
        mix(bk.best_bid().price);
        mix(bk.best_bid().shares);
        mix(bk.best_ask().price);
        mix(bk.best_ask().shares);
        mix(bk.level_count(itch::Side::Buy) + bk.level_count(itch::Side::Sell));
    }
    mix(b.orders().size());
    return h;
}

struct Result {
    std::vector<double> ns_per_msg;
    bench::CounterSample counters;  // from the median-ish (last) rep
    std::uint64_t digest = 0, overflow_adds = 0, recenters = 0, messages = 0;
    double ladder_mb = 0;  // resident ladder memory across all stocks (ladder variants only)
    double ladder_virtual_mb = 0;
};

template <class Book, class Orders>
void run_once(const itch::MappedFile& f, Result& r) {
    using Builder = lob::BookBuilder<Book, Orders>;
    auto b = std::make_unique<Builder>();
    bench::Counters c;
    c.start();
    const auto t0 = std::chrono::steady_clock::now();
    const auto pr = itch::parse_frames(f.data(), f.size(), *b);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r.counters = c.stop();
    r.messages = pr.messages;
    r.ns_per_msg.push_back(s * 1e9 / static_cast<double>(pr.messages));
    r.digest = digest(*b);
    r.overflow_adds = r.recenters = 0;
    if constexpr (requires { b->book(0).overflow_adds(); }) {
        for (std::size_t i = 0; i < lob::kMaxLocates; ++i) {
            r.overflow_adds += b->book(static_cast<std::uint16_t>(i)).overflow_adds();
            r.recenters += b->book(static_cast<std::uint16_t>(i)).recenters();
        }
        // Reserved = what mmap actually maps: the ladder size rounded up to whole pages.
        const std::size_t page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        const std::size_t mapped = (Book::ladder_bytes() + page - 1) / page * page;
        std::size_t res = 0, virt = 0;
        for (std::size_t i = 0; i < lob::kMaxLocates; ++i) {
            const auto& bk = b->book(static_cast<std::uint16_t>(i));
            if (!bk.has_ladder()) continue;
            res += bk.ladder_resident_bytes();
            virt += mapped;
        }
        r.ladder_mb = res / 1e6;
        r.ladder_virtual_mb = virt / 1e6;
    }
}

struct Variant {
    const char* key;
    const char* label;
    void (*run)(const itch::MappedFile&, Result&);
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ablation <file> [--reps N]\n");
        return 2;
    }
    int reps = 5;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    itch::MappedFile file(argv[1]);
    volatile std::uint64_t pf = file.prefault();
    (void)pf;

    using lob::FastBookT;
    using lob::FlatOrderMap;
    using lob::MapBook;
    using lob::StdOrderMap;
    const Variant vs[] = {
        {"baseline", "baseline: std::map book + std::unordered_map orders", run_once<MapBook, StdOrderMap>},
        {"flat_orders", "+ flat open-addressing order map", run_once<MapBook, FlatOrderMap>},
        {"ladder_only", "flat ladder (recentering) + std::unordered_map", run_once<FastBookT<true, 2048>, StdOrderMap>},
        {"flat_ladder", "+ flat price ladder (no recentering)", run_once<FastBookT<false, 2048>, FlatOrderMap>},
        {"recenter", "+ recenter window on the touch", run_once<FastBookT<true, 2048>, FlatOrderMap>},
        {"window_8k", "  window 8,192 ticks (+/- $41)", run_once<FastBookT<true, 8192>, FlatOrderMap>},
        {"window_32k", "  window 32,768 ticks (+/- $164)", run_once<FastBookT<true, 32768>, FlatOrderMap>},
    };
    constexpr int kV = sizeof vs / sizeof vs[0];
    std::vector<Result> res(kV);

    std::printf("ablation: %d variants x %d reps, interleaved\n", kV, reps);
    for (int r = 0; r < reps; ++r) {
        for (int v = 0; v < kV; ++v) vs[v].run(file, res[v]);
        std::printf("  rep %d/%d done\n", r + 1, reps);
        std::fflush(stdout);
    }

    const bool hw = res[0].counters.hw;
    const double msgs = static_cast<double>(res[0].messages);
    bool same = true;
    for (const auto& r : res) same &= r.digest == res[0].digest;
    std::vector<bench::Spread> sp;
    for (const auto& r : res) sp.push_back(bench::spread(r.ns_per_msg));

    std::printf("\n %-52s %9s %9s %8s %8s %11s %8s", "variant", "ns/msg", "range", "vs base", "vs prev", "overflow", "ladder MB");
    if (hw) std::printf(" %6s %9s %9s %9s", "IPC", "br-miss", "L1d-miss", "LLC-miss");
    std::printf("\n");
    // Ladder order for "vs prev": baseline -> flat_orders -> flat_ladder -> recenter; ladder_only is a side branch.
    const int prev[kV] = {-1, 0, 0, 1, 3, 4, 4};
    for (int v = 0; v < kV; ++v) {
        const auto& s = sp[v];
        std::printf(" %-52s %9.1f %8.1f%% %7.2fx", vs[v].label, s.median, s.pct_range(), sp[0].median / s.median);
        if (prev[v] >= 0) std::printf(" %7.2fx", sp[prev[v]].median / s.median);
        else std::printf(" %8s", "-");
        if (v >= 2) std::printf(" %11llu", static_cast<unsigned long long>(res[v].overflow_adds));
        else std::printf(" %11s", "-");
        if (v >= 2) std::printf(" %5.0f / %-6.0f", res[v].ladder_mb, res[v].ladder_virtual_mb);
        if (hw) {
            const auto& c = res[v].counters;
            std::printf(" %6.2f %9.3f %9.3f %9.3f", c.ipc(), c.branch_misses / msgs, c.l1d_misses / msgs, c.llc_misses / msgs);
        }
        std::printf("\n");
    }
    if (!hw) std::printf(" (hardware counters unavailable on this platform; see README)\n");
    std::printf(" ladder MB = resident / reserved (mmap'd; untouched pages cost no RAM)\n");
    std::printf("\n recenters: 2K window %llu, 8K %llu, 32K %llu\n", static_cast<unsigned long long>(res[4].recenters),
                static_cast<unsigned long long>(res[5].recenters), static_cast<unsigned long long>(res[6].recenters));
    std::printf(" final books identical across variants: %s\n", same ? "yes" : "NO");

    std::ofstream js("results/ablation.json");
    js << "{\n  \"stage\": 6,\n  \"messages\": " << res[0].messages << ",\n  \"reps\": " << reps
       << ",\n  \"books_identical\": " << (same ? "true" : "false") << ",\n  \"variants\": [";
    for (int v = 0; v < kV; ++v) {
        js << (v ? "," : "") << "\n    {\"key\": \"" << vs[v].key << "\", \"label\": \"" << vs[v].label
           << "\", \"ns_per_msg\": " << sp[v].json() << ", \"speedup_vs_baseline\": " << sp[0].median / sp[v].median
           << ", \"prev\": " << (prev[v] >= 0 ? "\"" + std::string(vs[prev[v]].key) + "\"" : "null")
           << ", \"overflow_adds\": " << res[v].overflow_adds << ", \"recenters\": " << res[v].recenters
           << ", \"ladder_resident_mb\": " << res[v].ladder_mb << ", \"ladder_reserved_mb\": " << res[v].ladder_virtual_mb
           << ", \"counters\": " << res[v].counters.json(msgs) << "}";
    }
    js << "\n  ]\n}\n";
    std::printf("wrote results/ablation.json\n");
    return same ? 0 : 1;
}
