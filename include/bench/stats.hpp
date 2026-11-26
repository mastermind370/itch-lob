// Summary statistics over repeated runs.
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace bench {

struct Spread {
    double median = 0, min = 0, max = 0;
    int n = 0;
    // Half the max-min range as a percentage of the median: a quick noise gauge.
    [[nodiscard]] double pct_range() const { return median > 0 ? 50.0 * (max - min) / median : 0; }
    [[nodiscard]] std::string json() const {
        char b[160];
        std::snprintf(b, sizeof b, "{\"median\": %.4f, \"min\": %.4f, \"max\": %.4f, \"n\": %d}", median, min, max, n);
        return b;
    }
};

inline Spread spread(std::vector<double> v) {
    Spread s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.n = static_cast<int>(v.size());
    s.min = v.front();
    s.max = v.back();
    s.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    return s;
}

}  // namespace bench
