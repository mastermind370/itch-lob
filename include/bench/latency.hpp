// Low-overhead timestamps and a latency histogram.
//
// Clock: arm64 reads the virtual counter (cntvct_el0; 24 MHz on Apple Silicon,
// i.e. 41.7 ns per tick); x86-64 reads the TSC. Other targets fall back to
// steady_clock. Recorded values are in ticks and converted to ns on output, so
// on Apple Silicon every single-message sample is a multiple of ~41.7 ns.
// Percentiles above the median are still meaningful; the median isn't.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace bench {

[[nodiscard]] inline std::uint64_t ticks() noexcept {
#if defined(__aarch64__)
    std::uint64_t v;
    asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(v)::"memory");
    return v;
#elif defined(__x86_64__)
    unsigned aux;
    return __rdtscp(&aux);
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Nanoseconds per tick.
[[nodiscard]] inline double ns_per_tick() {
#if defined(__aarch64__)
    std::uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return 1e9 / static_cast<double>(f);
#elif defined(__x86_64__)
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t c0 = ticks();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::uint64_t c1 = ticks();
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(c1 - c0);
#else
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::duration(1)).count();
#endif
}

// Smallest observable step of ticks(), in ns. On Apple Silicon cntfrq_el0 may
// report 1 GHz while the counter actually advances in ~41.7 ns steps.
[[nodiscard]] inline double resolution_ns() {
    std::uint64_t best = ~std::uint64_t{0};
    for (int i = 0; i < 200; ++i) {
        const std::uint64_t t = ticks();
        std::uint64_t u;
        while ((u = ticks()) == t) {}
        if (u - t < best) best = u - t;
    }
    return static_cast<double>(best) * ns_per_tick();
}

// Exact counts for values below kLinear ticks, plus a sorted overflow vector.
// Exact percentiles, no bucketing error.
class Histogram {
public:
    static constexpr std::size_t kLinear = 1 << 16;

    Histogram() : counts_(kLinear, 0) { big_.reserve(1 << 20); }  // no allocation while recording

    void record(std::uint64_t v) noexcept {
        if (v < kLinear) [[likely]] ++counts_[v];
        else big_.push_back(v);
        ++n_;
        if (v > max_) max_ = v;
    }

    [[nodiscard]] std::uint64_t count() const { return n_; }
    [[nodiscard]] std::uint64_t max() const { return max_; }

    // p in [0, 100]. Returns ticks.
    [[nodiscard]] std::uint64_t percentile(double p) {
        if (n_ == 0) return 0;
        const std::uint64_t rank = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(p / 100.0 * n_ + 0.5));
        std::uint64_t seen = 0;
        for (std::size_t v = 0; v < kLinear; ++v) {
            seen += counts_[v];
            if (seen >= rank) return v;
        }
        std::sort(big_.begin(), big_.end());
        return big_[std::min<std::size_t>(rank - seen - 1, big_.size() - 1)];
    }

    [[nodiscard]] double mean() const {
        if (n_ == 0) return 0;
        double s = 0;
        for (std::size_t v = 0; v < kLinear; ++v) s += static_cast<double>(v) * counts_[v];
        for (auto v : big_) s += static_cast<double>(v);
        return s / static_cast<double>(n_);
    }

    // (ticks, count) pairs with count > 0, for plotting.
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::uint64_t>> nonzero() {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
        for (std::size_t v = 0; v < kLinear; ++v)
            if (counts_[v]) out.emplace_back(v, counts_[v]);
        std::sort(big_.begin(), big_.end());
        for (std::size_t i = 0; i < big_.size();) {
            std::size_t j = i;
            while (j < big_.size() && big_[j] == big_[i]) ++j;
            out.emplace_back(big_[i], j - i);
            i = j;
        }
        return out;
    }

private:
    std::vector<std::uint64_t> counts_;
    std::vector<std::uint64_t> big_;
    std::uint64_t n_ = 0;
    std::uint64_t max_ = 0;
};

}  // namespace bench
