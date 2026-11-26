// itch_stats: parse an ITCH 5.0 file, count messages by type, measure parse
// throughput, and write results/parse.json.
//
//   itch_stats <file> [--reps N] [--json PATH]
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "itch/mapped_file.hpp"
#include "itch/parser.hpp"

namespace {

// Touches decoded fields so the compiler can't drop the decode work.
struct CountingHandler : itch::NullHandler {
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t sink = 0;
    std::uint64_t first_ts = 0, last_ts = 0;

    void note(const itch::Header& h) {
        ++counts[static_cast<std::uint8_t>(h.type)];
        if (first_ts == 0) first_ts = h.timestamp;
        last_ts = h.timestamp;
    }
    void on_system_event(const itch::SystemEvent& m) { note(m.hdr); sink += m.event_code; }
    void on_stock_directory(const itch::StockDirectory& m) { note(m.hdr); sink += m.round_lot_size; }
    void on_add(const itch::AddOrder& m) { note(m.hdr); sink += m.order_ref ^ m.shares ^ m.price; }
    void on_executed(const itch::OrderExecuted& m) { note(m.hdr); sink += m.order_ref ^ m.executed_shares; }
    void on_executed_with_price(const itch::OrderExecutedWithPrice& m) { note(m.hdr); sink += m.order_ref ^ m.execution_price; }
    void on_cancel(const itch::OrderCancel& m) { note(m.hdr); sink += m.order_ref ^ m.cancelled_shares; }
    void on_delete(const itch::OrderDelete& m) { note(m.hdr); sink += m.order_ref; }
    void on_replace(const itch::OrderReplace& m) { note(m.hdr); sink += m.new_order_ref ^ m.price; }
    void on_trade(const itch::Trade& m) { note(m.hdr); sink += m.shares; }
    void on_other(char t, const std::uint8_t*, std::size_t len) { ++counts[static_cast<std::uint8_t>(t)]; sink += len; }
};

const char* status_name(itch::ParseStatus s) {
    switch (s) {
        case itch::ParseStatus::Ok: return "ok";
        case itch::ParseStatus::Truncated: return "truncated";
        case itch::ParseStatus::BadLength: return "bad_length";
    }
    return "?";
}

std::string compiler_id() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#else
    return "unknown";
#endif
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) o.push_back(c);
    }
    return o;
}

void usage() {
    std::fprintf(stderr, "usage: itch_stats <file> [--reps N] [--json PATH]\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string path = argv[1];
    int reps = 5;
    std::string json_path = "results/parse.json";
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--json") && i + 1 < argc) json_path = argv[++i];
        else { usage(); return 2; }
    }

    itch::MappedFile file(path);
    volatile std::uint64_t pf = file.prefault();
    (void)pf;

    std::vector<double> secs;
    CountingHandler result;
    itch::ParseResult pr{};
    for (int r = 0; r < reps; ++r) {
        CountingHandler h;
        const auto t0 = std::chrono::steady_clock::now();
        pr = itch::parse_frames(file.data(), file.size(), h);
        const auto t1 = std::chrono::steady_clock::now();
        secs.push_back(std::chrono::duration<double>(t1 - t0).count());
        if (r == 0) result = h;
        else if (h.sink != result.sink) { std::fprintf(stderr, "non-deterministic parse!\n"); return 1; }
    }
    std::vector<double> sorted = secs;
    std::sort(sorted.begin(), sorted.end());
    const double best = sorted.front();
    const double median = sorted[sorted.size() / 2];
    const double msgs = static_cast<double>(pr.messages);

    std::printf("file           %s\n", path.c_str());
    std::printf("bytes          %zu (%.2f GB)\n", file.size(), file.size() / 1e9);
    std::printf("status         %s (consumed %zu bytes)\n", status_name(pr.status), pr.consumed);
    std::printf("messages       %llu\n", static_cast<unsigned long long>(pr.messages));
    std::printf("time best/med  %.4f s / %.4f s over %d reps\n", best, median, reps);
    std::printf("throughput     %.1f M msgs/s (best), %.1f M msgs/s (median)\n", msgs / best / 1e6, msgs / median / 1e6);
    std::printf("               %.2f GB/s, %.2f ns/msg (best)\n", pr.consumed / best / 1e9, best * 1e9 / msgs);
    std::printf("session span   %.3f s of market time\n", (result.last_ts - result.first_ts) / 1e9);
    std::printf("\n type  count          share\n");
    for (int t = 0; t < 256; ++t) {
        if (!result.counts[t]) continue;
        std::printf("  %c    %-14llu %6.2f%%\n", t, static_cast<unsigned long long>(result.counts[t]),
                    100.0 * result.counts[t] / msgs);
    }

    std::ofstream js(json_path);
    if (!js) { std::fprintf(stderr, "cannot write %s\n", json_path.c_str()); return 1; }
    js << "{\n"
       << "  \"stage\": 1,\n"
       << "  \"file\": \"" << json_escape(path) << "\",\n"
       << "  \"bytes\": " << file.size() << ",\n"
       << "  \"status\": \"" << status_name(pr.status) << "\",\n"
       << "  \"messages\": " << pr.messages << ",\n"
       << "  \"reps\": " << reps << ",\n"
       << "  \"seconds_best\": " << best << ",\n"
       << "  \"seconds_median\": " << median << ",\n"
       << "  \"mmsgs_per_s_best\": " << msgs / best / 1e6 << ",\n"
       << "  \"mmsgs_per_s_median\": " << msgs / median / 1e6 << ",\n"
       << "  \"gb_per_s_best\": " << pr.consumed / best / 1e9 << ",\n"
       << "  \"ns_per_msg_best\": " << best * 1e9 / msgs << ",\n"
       << "  \"compiler\": \"" << json_escape(compiler_id()) << "\",\n"
       << "  \"counts\": {";
    bool first = true;
    for (int t = 0; t < 256; ++t) {
        if (!result.counts[t]) continue;
        js << (first ? "" : ",") << "\n    \"" << static_cast<char>(t) << "\": " << result.counts[t];
        first = false;
    }
    js << "\n  }\n}\n";
    std::printf("\nwrote %s\n", json_path.c_str());
    return pr.status == itch::ParseStatus::BadLength ? 1 : 0;
}
