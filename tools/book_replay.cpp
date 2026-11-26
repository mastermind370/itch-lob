// book_replay: rebuild every stock's order book from an ITCH file.
//
//   book_replay <file> [--check] [--reps N] [--show AAPL,MSFT] [--json PATH]
//
// Without --check: times the replay (parse + book updates) and writes the
// throughput to results/baseline.json.
// With --check: replays once, verifying every touched price level after every
// message plus a full audit at the end. Exits non-zero on any mismatch.
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

#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"
#include "lob/book_builder.hpp"
#include "lob/checker.hpp"
#include "lob/map_book.hpp"

namespace {

using Book = lob::MapBook;
using Builder = lob::BookBuilder<Book>;

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string t; std::getline(ss, t, ',');)
        if (!t.empty()) out.push_back(t);
    return out;
}

struct Summary {
    std::uint64_t live_orders = 0, levels = 0, books = 0, crossed = 0;
};

Summary summarize(const Builder& b) {
    Summary s;
    s.live_orders = b.orders().size();
    for (std::size_t i = 0; i < lob::kMaxLocates; ++i) {
        const Book& bk = b.book(static_cast<std::uint16_t>(i));
        const std::size_t n = bk.level_count(lob::Side::Buy) + bk.level_count(lob::Side::Sell);
        if (!n) continue;
        ++s.books;
        s.levels += n;
        const auto bid = bk.best_bid(), ask = bk.best_ask();
        if (bid.price && ask.price && bid.price >= ask.price) ++s.crossed;
    }
    return s;
}

void show_tops(const Builder& b, const std::vector<std::string>& syms) {
    std::printf("\n symbol      bid x size          ask x size     levels(b/a)\n");
    for (const auto& sym : syms) {
        const int loc = b.locate_of(sym);
        if (loc < 0) { std::printf(" %-8s  (not in file)\n", sym.c_str()); continue; }
        const Book& bk = b.book(static_cast<std::uint16_t>(loc));
        const auto bid = bk.best_bid(), ask = bk.best_ask();
        std::printf(" %-8s %9.4f x %-7llu %9.4f x %-7llu %zu/%zu\n", sym.c_str(), itch::price_to_double(bid.price),
                    static_cast<unsigned long long>(bid.shares), itch::price_to_double(ask.price),
                    static_cast<unsigned long long>(ask.shares), bk.level_count(lob::Side::Buy),
                    bk.level_count(lob::Side::Sell));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: book_replay <file> [--check] [--reps N] [--show SYM,SYM] [--json PATH]\n");
        return 2;
    }
    const std::string path = argv[1];
    bool check = false;
    int reps = 3;
    std::string show = "AAPL,MSFT,SPY,QQQ,TSLA";
    std::string json_path = "results/baseline.json";
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--check")) check = true;
        else if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--show") && i + 1 < argc) show = argv[++i];
        else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }

    itch::MappedFile file(path);
    volatile std::uint64_t pf = file.prefault();
    (void)pf;

    if (check) {
        auto b = std::make_unique<Builder>();
        lob::CheckedReplay<Book> chk(*b);
        const auto t0 = std::chrono::steady_clock::now();
        const auto pr = itch::parse_frames(file.data(), file.size(), chk);
        chk.full_audit();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto& st = chk.stats();
        const auto& an = b->anomalies();
        std::printf("checked %llu messages in %.1f s\n", static_cast<unsigned long long>(pr.messages), s);
        std::printf("per-message level checks  %llu, mismatches %llu\n",
                    static_cast<unsigned long long>(st.level_checks), static_cast<unsigned long long>(st.level_mismatches));
        std::printf("final audit levels        %llu, mismatches %llu\n",
                    static_cast<unsigned long long>(st.audit_levels), static_cast<unsigned long long>(st.audit_mismatches));
        std::printf("feed anomalies            dup_add %llu, unknown_ref %llu, overfill %llu\n",
                    static_cast<unsigned long long>(an.duplicate_add), static_cast<unsigned long long>(an.unknown_ref),
                    static_cast<unsigned long long>(an.overfill));
        std::printf("crossed-book observations %llu (informational)\n", static_cast<unsigned long long>(st.crossed_checks));
        show_tops(*b, split(show));
        std::printf("\n%s\n", chk.ok() ? "CHECK PASSED" : "CHECK FAILED");
        std::ofstream js("results/baseline_check.json");
        js << "{\n  \"stage\": 2,\n  \"messages\": " << pr.messages << ",\n  \"level_checks\": " << st.level_checks
           << ",\n  \"level_mismatches\": " << st.level_mismatches << ",\n  \"audit_levels\": " << st.audit_levels
           << ",\n  \"audit_mismatches\": " << st.audit_mismatches << ",\n  \"anomalies\": " << an.total()
           << ",\n  \"passed\": " << (chk.ok() ? "true" : "false") << "\n}\n";
        return chk.ok() ? 0 : 1;
    }

    std::vector<double> secs;
    std::unique_ptr<Builder> last;
    itch::ParseResult pr{};
    for (int r = 0; r < reps; ++r) {
        auto b = std::make_unique<Builder>();  // allocation of the 65536 empty books is outside the timer
        const auto t0 = std::chrono::steady_clock::now();
        pr = itch::parse_frames(file.data(), file.size(), *b);
        secs.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        last = std::move(b);  // previous builder is freed here, outside the next timed region
    }
    std::sort(secs.begin(), secs.end());
    const double best = secs.front(), median = secs[secs.size() / 2];
    const double msgs = static_cast<double>(pr.messages);
    const Summary sum = summarize(*last);

    std::printf("file          %s\n", path.c_str());
    std::printf("book          std::map baseline\n");
    std::printf("messages      %llu\n", static_cast<unsigned long long>(pr.messages));
    std::printf("time best/med %.3f s / %.3f s over %d reps\n", best, median, reps);
    std::printf("throughput    %.2f M msgs/s (best), %.2f (median)\n", msgs / best / 1e6, msgs / median / 1e6);
    std::printf("ns/msg        %.1f (best), %.1f (median)\n", best * 1e9 / msgs, median * 1e9 / msgs);
    std::printf("end state     %llu live orders, %llu levels across %llu books, %llu crossed\n",
                static_cast<unsigned long long>(sum.live_orders), static_cast<unsigned long long>(sum.levels),
                static_cast<unsigned long long>(sum.books), static_cast<unsigned long long>(sum.crossed));
    show_tops(*last, split(show));

    std::ofstream js(json_path);
    js << "{\n  \"stage\": 2,\n  \"book\": \"std::map\",\n  \"file\": \"" << path << "\",\n  \"messages\": "
       << pr.messages << ",\n  \"reps\": " << reps << ",\n  \"seconds_best\": " << best
       << ",\n  \"seconds_median\": " << median << ",\n  \"mmsgs_per_s_best\": " << msgs / best / 1e6
       << ",\n  \"mmsgs_per_s_median\": " << msgs / median / 1e6 << ",\n  \"ns_per_msg_best\": " << best * 1e9 / msgs
       << ",\n  \"ns_per_msg_median\": " << median * 1e9 / msgs << ",\n  \"live_orders_end\": " << sum.live_orders
       << ",\n  \"levels_end\": " << sum.levels << "\n}\n";
    std::printf("\nwrote %s\n", json_path.c_str());
    return 0;
}
