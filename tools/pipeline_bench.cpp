// pipeline_bench: single thread vs two-thread SPSC pipeline on an ITCH file.
//
//   pipeline_bench <file> [--reps N] [--rate M]
//
//   M = paced rate in million ITCH msgs/s for the latency run. Default: half of
//   the measured pipeline throughput, so the consumer keeps up and the latency
//   measures the path rather than a backlog.
//
//   1. inline       one thread, parse + book update, no queue
//   2. saturated    two threads flat out: padded queue, then unpadded/uncached ablations
//   3. latency      per-message latency without a queue (inline) and end-to-end
//                   through the queue at a fixed rate the consumer can sustain
// Every run's final books are hashed and must match the inline run.
// Writes results/pipeline.json.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "bench/counters.hpp"
#include "bench/latency.hpp"
#include "bench/stats.hpp"
#include "itch/mapped_file.hpp"
#include "pipeline/pipeline.hpp"

using namespace pipeline;

namespace {

constexpr std::size_t kSlots = 1 << 12;  // 4096 x 40 B = 160 KB of slots
using Full = SpscQueue<Event, kSlots, true, true, 32>;        // padded + cached indices + batched head
using NoBatch = SpscQueue<Event, kSlots, true, true, 1>;      // padded + cached indices
using Unpadded = SpscQueue<Event, kSlots, false, true, 1>;    // cached indices only
using Naive = SpscQueue<Event, kSlots, false, false, 1>;      // none of the three

struct Pct {
    double mean, p50, p90, p99, p999, p9999, max;
};

Pct pct(bench::Histogram& h, double npt) {
    return {h.mean() * npt, h.percentile(50) * npt, h.percentile(90) * npt, h.percentile(99) * npt,
            h.percentile(99.9) * npt, h.percentile(99.99) * npt, static_cast<double>(h.max()) * npt};
}

void print_pct(const char* name, const Pct& p) {
    std::printf("   %-22s mean %7.1f | p50 %7.1f | p90 %7.1f | p99 %7.1f | p99.9 %8.1f | p99.99 %8.1f ns\n", name, p.mean,
                p.p50, p.p90, p.p99, p.p999, p.p9999);
}

std::string pct_json(const Pct& p) {
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "{\"mean_ns\": %.1f, \"p50_ns\": %.1f, \"p90_ns\": %.1f, \"p99_ns\": %.1f, \"p99_9_ns\": %.1f, "
                  "\"p99_99_ns\": %.1f, \"max_ns\": %.1f}",
                  p.mean, p.p50, p.p90, p.p99, p.p999, p.p9999, p.max);
    return buf;
}

std::string hist_json(bench::Histogram& h) {
    std::string s = "[";
    bool first = true;
    for (const auto& [v, c] : h.nonzero()) {
        s += (first ? "[" : ",[") + std::to_string(v) + "," + std::to_string(c) + "]";
        first = false;
    }
    return s + "]";
}

// One variant's repeated runs.
struct Series {
    const char* key;
    const char* label;
    RunStats (*run)(const std::uint8_t*, std::size_t);
    std::vector<double> mps;  // M msgs/s per rep
    RunStats last;
};

template <class Q>
RunStats run_sat(const std::uint8_t* d, std::size_t n) { return run_pipeline<Q>(d, n); }
RunStats run_in(const std::uint8_t* d, std::size_t n) { return run_inline(d, n); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pipeline_bench <file> [--reps N] [--rate MSGS_PER_SEC_IN_MILLIONS]\n");
        return 2;
    }
    int reps = 3;
    double rate_m = 0;  // 0 = auto
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--reps") && i + 1 < argc) reps = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate_m = std::atof(argv[++i]);
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    itch::MappedFile file(argv[1]);
    volatile std::uint64_t pf = file.prefault();
    (void)pf;
    const std::uint8_t* d = file.data();
    const std::size_t n = file.size();
    auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    bool all_match = true;

    // 1-2. Throughput: every variant, interleaved round-robin, `reps` times.
    Series ser[] = {
        {"inline", "inline (1 thread, no queue)", run_in, {}, {}},
        {"pipeline", "pipeline: padded+cached+batched", run_sat<Full>, {}, {}},
        {"no_head_batch", "  - without head batching", run_sat<NoBatch>, {}, {}},
        {"no_padding", "  - without padding either", run_sat<Unpadded>, {}, {}},
        {"naive", "  - without index caching (naive)", run_sat<Naive>, {}, {}},
    };
    constexpr int kS = sizeof ser / sizeof ser[0];
    std::uint64_t ref_digest = 0;
    double msgs = 0;
    for (int r = 0; r < reps; ++r) {
        for (auto& x : ser) {
            x.last = x.run(d, n);
            msgs = static_cast<double>(x.last.messages);
            x.mps.push_back(msgs / x.last.seconds / 1e6);
            if (!ref_digest) ref_digest = x.last.digest;
            all_match &= x.last.digest == ref_digest;
        }
    }
    std::printf("== throughput: %d reps interleaved, median [min .. max] (%llu ITCH messages)\n", reps, u(ser[0].last.messages));
    bench::Spread sp[kS];
    for (int i = 0; i < kS; ++i) {
        sp[i] = bench::spread(ser[i].mps);
        std::printf("   %-34s %7.2f M msgs/s [%5.2f .. %5.2f]  %6.1f ns/msg  %.2fx vs inline\n", ser[i].label, sp[i].median,
                    sp[i].min, sp[i].max, 1e3 / sp[i].median, sp[i].median / sp[0].median);
    }
    std::printf("   books identical in every run: %s; thread placement: %s\n\n", all_match ? "yes" : "NO", ser[1].last.placement);
    const RunStats in = ser[0].last;

    // 3. Latency.
    const double npt = bench::ns_per_tick(), res = bench::resolution_ns();
    bench::Histogram hi, hp;
    const RunStats li = run_inline(d, n, &hi);
    all_match &= li.digest == in.digest;
    if (rate_m <= 0) rate_m = 0.5 * sp[1].median;
    const std::uint64_t interval = static_cast<std::uint64_t>(1e3 / rate_m / npt);  // ticks per message
    bench::Counters ctr;
    ctr.start();
    const RunStats lp = run_pipeline<Full>(d, n, interval, &hp);
    const bench::CounterSample cs = ctr.stop();
    all_match &= lp.digest == in.digest;
    const Pct pi = pct(hi, npt), pp = pct(hp, npt);
    std::printf("== latency per order message (clock resolution %.1f ns)\n", res);
    print_pct("inline (no queue)", pi);
    char label[64];
    std::snprintf(label, sizeof label, "pipeline @ %.1fM msg/s", rate_m);
    print_pct(label, pp);
    std::printf("   queue adds %.1f ns at p50, %.1f ns at p99 (parse + enqueue + core-to-core transfer + dequeue)\n",
                pp.p50 - pi.p50, pp.p99 - pi.p99);
    const double late_frac = static_cast<double>(lp.paced_late) / static_cast<double>(lp.messages);
    std::printf("   paced run: %.2f s, producer late for %.2f%% of sends%s\n\n", lp.seconds, 100 * late_frac,
                late_frac > 0.01 ? "  ** the pipeline fell behind this rate: these numbers include queueing; rerun with a lower --rate **" : "");
    std::printf("   during the paced run: %ld involuntary + %ld voluntary context switches, %ld page faults\n\n",
                cs.involuntary_cs, cs.voluntary_cs, cs.minor_faults + cs.major_faults);

    std::printf("%s\n", all_match ? "ALL RUNS BUILT IDENTICAL BOOKS" : "BOOK MISMATCH BETWEEN RUNS");

    std::ofstream js("results/pipeline.json");
    js << "{\n  \"stage\": 5,\n  \"messages\": " << in.messages << ",\n  \"queue_slots\": " << kSlots
       << ",\n  \"event_bytes\": " << sizeof(Event) << ",\n  \"placement\": \"" << ser[1].last.placement << "\""
       << ",\n  \"throughput_mmsgs_per_s\": {";
    for (int i = 0; i < kS; ++i) js << (i ? ", " : "") << "\"" << ser[i].key << "\": " << sp[i].json();
    js << "}"
       << ",\n  \"pipeline_full_spins\": " << ser[1].last.full_spins << ",\n  \"pipeline_empty_spins\": " << ser[1].last.empty_spins
       << ",\n  \"latency_resolution_ns\": " << res << ",\n  \"paced_rate_mmsgs_per_s\": " << rate_m
       << ",\n  \"paced_late_fraction\": " << late_frac << ",\n  \"latency_inline\": " << pct_json(pi)
       << ",\n  \"latency_pipeline\": " << pct_json(pp) << ",\n  \"paced_counters\": " << cs.json(0)
       << ",\n  \"ns_per_tick\": " << npt << ",\n  \"hist_inline_ticks\": " << hist_json(hi)
       << ",\n  \"hist_pipeline_ticks\": " << hist_json(hp) << ",\n  \"books_match\": " << (all_match ? "true" : "false")
       << "\n}\n";
    std::printf("wrote results/pipeline.json\n");
    return all_match ? 0 : 1;
}
