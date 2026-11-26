// book_replay: rebuild every stock's order book from an ITCH file.
//
//   book_replay <file> [--book map|fast|both] [--check] [--latency] [--reps N] [--show SYM,SYM]
//
//   (default)  time the replay (parse + book updates), best/median of N reps
//   --check    verify every touched level and the best bid/ask after every message,
//              plus a full audit at the end; non-zero exit on any mismatch
//   --latency  time every message individually; p50/p90/p99/p99.9/p99.99/max
//
// --book both runs map then fast and reports the speedup.
// JSON goes to results/: baseline.json / fast.json, *_check.json, latency_*.json, stage3.json.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "bench/latency.hpp"
#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "lob/book_builder.hpp"
#include "lob/checker.hpp"
#include "lob/fast_book.hpp"
#include "lob/map_book.hpp"
#include "lob/order_map.hpp"

namespace {

struct MapVariant {
    using Book = lob::MapBook;
    using Orders = lob::StdOrderMap;
    static constexpr const char* name = "map";
    static constexpr const char* label = "std::map book + std::unordered_map orders";
    static constexpr const char* json = "baseline";
};
struct FastVariant {
    using Book = lob::FastBook;
    using Orders = lob::FlatOrderMap;
    static constexpr const char* name = "fast";
    static constexpr const char* label = "flat price ladder + flat open-addressing order map";
    static constexpr const char* json = "fast";
};

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string t; std::getline(ss, t, ',');)
        if (!t.empty()) out.push_back(t);
    return out;
}

struct Summary {
    std::uint64_t live_orders = 0, levels = 0, books = 0, crossed = 0, overflow_adds = 0, map_grows = 0;
};

template <class V>
Summary summarize(const lob::BookBuilder<typename V::Book, typename V::Orders>& b) {
    Summary s;
    s.live_orders = b.orders().size();
    s.map_grows = b.orders().grows();
    for (std::size_t i = 0; i < lob::kMaxLocates; ++i) {
        const auto& bk = b.book(static_cast<std::uint16_t>(i));
        if constexpr (requires { bk.overflow_adds(); }) s.overflow_adds += bk.overflow_adds();
        const std::size_t n = bk.level_count(lob::Side::Buy) + bk.level_count(lob::Side::Sell);
        if (!n) continue;
        ++s.books;
        s.levels += n;
        const auto bid = bk.best_bid(), ask = bk.best_ask();
        if (bid.price && ask.price && bid.price >= ask.price) ++s.crossed;
    }
    return s;
}

template <class Builder>
void show_tops(const Builder& b, const std::vector<std::string>& syms) {
    std::printf("\n symbol      bid x size          ask x size     levels(b/a)\n");
    for (const auto& sym : syms) {
        const int loc = b.locate_of(sym);
        if (loc < 0) { std::printf(" %-8s  (not in file)\n", sym.c_str()); continue; }
        const auto& bk = b.book(static_cast<std::uint16_t>(loc));
        const auto bid = bk.best_bid(), ask = bk.best_ask();
        std::printf(" %-8s %9.4f x %-7llu %9.4f x %-7llu %zu/%zu\n", sym.c_str(), itch::price_to_double(bid.price),
                    static_cast<unsigned long long>(bid.shares), itch::price_to_double(ask.price),
                    static_cast<unsigned long long>(ask.shares), bk.level_count(lob::Side::Buy),
                    bk.level_count(lob::Side::Sell));
    }
}

struct Throughput {
    std::uint64_t messages = 0;
    double best = 0, median = 0;
};

template <class V>
Throughput run_throughput(const itch::MappedFile& file, int reps, const std::vector<std::string>& show) {
    using Builder = lob::BookBuilder<typename V::Book, typename V::Orders>;
    std::vector<double> secs;
    std::unique_ptr<Builder> last;
    itch::ParseResult pr{};
    for (int r = 0; r < reps; ++r) {
        auto b = std::make_unique<Builder>();  // allocation of the books and order map is outside the timer
        const auto t0 = std::chrono::steady_clock::now();
        pr = itch::parse_frames(file.data(), file.size(), *b);
        secs.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        last.reset();  // free the previous builder outside the timed region
        last = std::move(b);
    }
    std::sort(secs.begin(), secs.end());
    Throughput t{pr.messages, secs.front(), secs[secs.size() / 2]};
    const double msgs = static_cast<double>(t.messages);
    const Summary sum = summarize<V>(*last);

    std::printf("== %s: %s\n", V::name, V::label);
    std::printf("messages      %llu\n", static_cast<unsigned long long>(t.messages));
    std::printf("time best/med %.3f s / %.3f s over %d reps\n", t.best, t.median, reps);
    std::printf("throughput    %.2f M msgs/s (best), %.2f (median)\n", msgs / t.best / 1e6, msgs / t.median / 1e6);
    std::printf("ns/msg        %.1f (best), %.1f (median)\n", t.best * 1e9 / msgs, t.median * 1e9 / msgs);
    std::printf("end state     %llu live orders, %llu levels across %llu books, %llu crossed\n",
                static_cast<unsigned long long>(sum.live_orders), static_cast<unsigned long long>(sum.levels),
                static_cast<unsigned long long>(sum.books), static_cast<unsigned long long>(sum.crossed));
    if (std::strcmp(V::name, "fast") == 0)
        std::printf("fast path     %llu adds went to overflow maps; order map grew %llu times\n",
                    static_cast<unsigned long long>(sum.overflow_adds), static_cast<unsigned long long>(sum.map_grows));
    show_tops(*last, show);

    std::ofstream js(std::string("results/") + V::json + ".json");
    js << "{\n  \"book\": \"" << V::name << "\",\n  \"messages\": " << t.messages << ",\n  \"reps\": " << reps
       << ",\n  \"seconds_best\": " << t.best << ",\n  \"seconds_median\": " << t.median
       << ",\n  \"mmsgs_per_s_best\": " << msgs / t.best / 1e6 << ",\n  \"mmsgs_per_s_median\": " << msgs / t.median / 1e6
       << ",\n  \"ns_per_msg_best\": " << t.best * 1e9 / msgs << ",\n  \"ns_per_msg_median\": " << t.median * 1e9 / msgs
       << ",\n  \"live_orders_end\": " << sum.live_orders << ",\n  \"levels_end\": " << sum.levels
       << ",\n  \"overflow_adds\": " << sum.overflow_adds << ",\n  \"order_map_grows\": " << sum.map_grows << "\n}\n";
    std::printf("wrote results/%s.json\n\n", V::json);
    return t;
}

template <class V>
bool run_check(const itch::MappedFile& file, const std::vector<std::string>& show) {
    using Builder = lob::BookBuilder<typename V::Book, typename V::Orders>;
    auto b = std::make_unique<Builder>();
    lob::CheckedReplay<typename V::Book, typename V::Orders> chk(*b);
    const auto t0 = std::chrono::steady_clock::now();
    const auto pr = itch::parse_frames(file.data(), file.size(), chk);
    chk.full_audit();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto& st = chk.stats();
    const auto& an = b->anomalies();
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("== check %s: %llu messages in %.1f s\n", V::name, u(pr.messages), s);
    std::printf("per-message level checks  %llu, mismatches %llu\n", u(st.level_checks), u(st.level_mismatches));
    std::printf("per-message best bid/ask  %llu, mismatches %llu (vs reference std::map book)\n", u(st.best_checks),
                u(st.best_mismatches));
    std::printf("final audit levels        %llu, mismatches %llu\n", u(st.audit_levels), u(st.audit_mismatches));
    std::printf("feed anomalies            dup_add %llu, unknown_ref %llu, overfill %llu\n", u(an.duplicate_add),
                u(an.unknown_ref), u(an.overfill));
    std::printf("crossed-book observations %llu (informational)\n", u(st.crossed_checks));
    show_tops(*b, show);
    std::printf("\n%s\n\n", chk.ok() ? "CHECK PASSED" : "CHECK FAILED");
    std::ofstream js(std::string("results/") + V::json + "_check.json");
    js << "{\n  \"book\": \"" << V::name << "\",\n  \"messages\": " << pr.messages
       << ",\n  \"level_checks\": " << st.level_checks << ",\n  \"level_mismatches\": " << st.level_mismatches
       << ",\n  \"best_checks\": " << st.best_checks << ",\n  \"best_mismatches\": " << st.best_mismatches
       << ",\n  \"audit_levels\": " << st.audit_levels << ",\n  \"audit_mismatches\": " << st.audit_mismatches
       << ",\n  \"anomalies\": " << an.total() << ",\n  \"passed\": " << (chk.ok() ? "true" : "false") << "\n}\n";
    return chk.ok();
}

template <class V>
void run_latency(const itch::MappedFile& file) {
    using Builder = lob::BookBuilder<typename V::Book, typename V::Orders>;
    const double npt = bench::ns_per_tick();

    // Timer overhead: two back-to-back reads.
    bench::Histogram over;
    for (int i = 0; i < 1'000'000; ++i) {
        const std::uint64_t a = bench::ticks();
        const std::uint64_t b = bench::ticks();
        over.record(b - a);
    }

    auto b = std::make_unique<Builder>();
    bench::Histogram h;
    const std::uint8_t* d = file.data();
    const std::size_t n = file.size();
    std::size_t off = 0;
    while (n - off >= 2) {
        const std::size_t len = itch::load_be16(d + off);
        if (n - off - 2 < len) break;
        const std::uint8_t* m = d + off + 2;
        const std::size_t want = itch::kMessageLength[m[0]];
        if (len == 0 || (want && want != len)) { std::fprintf(stderr, "bad frame at %zu\n", off); break; }
        const std::uint64_t t0 = bench::ticks();
        itch::decode_message(m, len, *b);
        const std::uint64_t t1 = bench::ticks();
        h.record(t1 - t0);
        off += 2 + len;
    }

    const double ps[] = {50, 90, 99, 99.9, 99.99};
    const char* names[] = {"p50", "p90", "p99", "p99_9", "p99_99"};
    std::printf("== latency %s: %llu messages, clock resolution %.2f ns/tick\n", V::name,
                static_cast<unsigned long long>(h.count()), npt);
    std::printf("timer overhead p50 %.1f ns (included in every sample below)\n", over.percentile(50) * npt);
    std::printf("mean %.1f ns", h.mean() * npt);
    std::vector<double> vals;
    for (int i = 0; i < 5; ++i) {
        vals.push_back(static_cast<double>(h.percentile(ps[i])) * npt);
        std::printf(" | %s %.1f", names[i], vals.back());
    }
    std::printf(" | max %.1f ns\n", static_cast<double>(h.max()) * npt);

    std::ofstream js(std::string("results/latency_") + V::name + ".json");
    js << "{\n  \"book\": \"" << V::name << "\",\n  \"messages\": " << h.count() << ",\n  \"ns_per_tick\": " << npt
       << ",\n  \"timer_overhead_p50_ns\": " << over.percentile(50) * npt << ",\n  \"mean_ns\": " << h.mean() * npt;
    for (int i = 0; i < 5; ++i) js << ",\n  \"" << names[i] << "_ns\": " << vals[i];
    js << ",\n  \"max_ns\": " << static_cast<double>(h.max()) * npt << ",\n  \"histogram_ticks\": [";
    bool first = true;
    for (const auto& [v, c] : h.nonzero()) {
        js << (first ? "" : ",") << "[" << v << "," << c << "]";
        first = false;
    }
    js << "]\n}\n";
    std::printf("wrote results/latency_%s.json\n\n", V::name);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: book_replay <file> [--book map|fast|both] [--check] [--latency] [--reps N] [--show SYM,...]\n");
        return 2;
    }
    const std::string path = argv[1];
    std::string which = "both";
    bool check = false, latency = false;
    int reps = 3;
    std::string show = "AAPL,MSFT,SPY,QQQ,TSLA";
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--check")) check = true;
        else if (!std::strcmp(argv[i], "--latency")) latency = true;
        else if (!std::strcmp(argv[i], "--book") && i + 1 < argc) which = argv[++i];
        else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--show") && i + 1 < argc) show = argv[++i];
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (which != "map" && which != "fast" && which != "both") { std::fprintf(stderr, "--book must be map, fast or both\n"); return 2; }
    const bool do_map = which != "fast", do_fast = which != "map";
    const auto syms = split(show);

    itch::MappedFile file(path);
    volatile std::uint64_t pf = file.prefault();
    (void)pf;

    if (check) {
        bool ok = true;
        if (do_map) ok &= run_check<MapVariant>(file, syms);
        if (do_fast) ok &= run_check<FastVariant>(file, syms);
        return ok ? 0 : 1;
    }
    if (latency) {
        if (do_map) run_latency<MapVariant>(file);
        if (do_fast) run_latency<FastVariant>(file);
        return 0;
    }

    Throughput tm{}, tf{};
    if (do_map) tm = run_throughput<MapVariant>(file, reps, syms);
    if (do_fast) tf = run_throughput<FastVariant>(file, reps, syms);
    if (do_map && do_fast) {
        const double speedup = tm.best / tf.best;
        std::printf("speedup fast vs map: %.2fx (best), %.2fx (median)\n", speedup, tm.median / tf.median);
        std::ofstream js("results/stage3.json");
        js << "{\n  \"messages\": " << tm.messages << ",\n  \"map_ns_per_msg\": " << tm.best * 1e9 / tm.messages
           << ",\n  \"fast_ns_per_msg\": " << tf.best * 1e9 / tf.messages << ",\n  \"speedup_best\": " << speedup
           << ",\n  \"speedup_median\": " << tm.median / tf.median << "\n}\n";
        std::printf("wrote results/stage3.json\n");
    }
    return 0;
}
