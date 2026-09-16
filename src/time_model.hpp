#pragma once

#include "sim.hpp"

#include <limits>
#include <stdexcept>
#include <vector>

namespace lapsim {

inline std::vector<Time> request_clock(std::size_t n, Time spacing, Time start = 0) {
    if (spacing < 0) throw std::invalid_argument("request-clock spacing must be non-negative");
    std::vector<Time> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(start + static_cast<Time>(i) * spacing);
    return out;
}

inline std::vector<Time> spread_equal_timestamp_buckets(
    const std::vector<Time>& coarse_time,
    Time ticks_per_bucket) {
    if (ticks_per_bucket <= 0) throw std::invalid_argument("ticks_per_bucket must be positive");
    std::vector<Time> out(coarse_time.size());
    std::size_t i = 0;
    while (i < coarse_time.size()) {
        if (coarse_time[i] < 0) throw std::invalid_argument("coarse timestamps must be non-negative");
        if (i && coarse_time[i] < coarse_time[i - 1])
            throw std::invalid_argument("coarse timestamps must be nondecreasing");
        std::size_t j = i + 1;
        while (j < coarse_time.size() && coarse_time[j] == coarse_time[i]) ++j;
        const std::size_t m = j - i;
        for (std::size_t k = 0; k < m; ++k) {
            if (coarse_time[i] > std::numeric_limits<Time>::max() / ticks_per_bucket)
                throw std::overflow_error("reconstructed timestamp overflows Time");
            const Time base = coarse_time[i] * ticks_per_bucket;
            const long double fraction = static_cast<long double>(k + 1) / static_cast<long double>(m + 1);
            const Time offset = static_cast<Time>(fraction * static_cast<long double>(ticks_per_bucket));
            if (base > std::numeric_limits<Time>::max() - offset)
                throw std::overflow_error("reconstructed timestamp overflows Time");
            out[i + k] = base + offset;
        }
        i = j;
    }
    return out;
}

} // namespace lapsim
