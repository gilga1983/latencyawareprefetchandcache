#include "sim.hpp"
#include "oracle_trace.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lapsim;

namespace {

struct Args {
    std::string trace;
    std::string name = "trace";
    Bytes capacity = 256ULL * 1024 * 1024;
    std::vector<Time> latencies_us{0, 1000, 5000, 10000, 20000};
    std::uint64_t limit = 0;
    Time ticks_per_second = 1000000;
    bool spread_within_second = true;
};

std::vector<Time> parse_latencies(const std::string& text) {
    std::vector<Time> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        const long long v = std::stoll(item);
        if (v < 0) throw std::invalid_argument("latencies must be non-negative");
        out.push_back(static_cast<Time>(v));
    }
    if (out.empty()) throw std::invalid_argument("at least one latency is required");
    if (std::find(out.begin(), out.end(), 0) == out.end()) out.insert(out.begin(), 0);
    return out;
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto need = [&]() -> std::string {
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + s);
            return argv[++i];
        };
        if (s == "--trace") a.trace = need();
        else if (s == "--name") a.name = need();
        else if (s == "--capacity") a.capacity = std::stoull(need());
        else if (s == "--latencies-us") a.latencies_us = parse_latencies(need());
        else if (s == "--limit") a.limit = std::stoull(need());
        else if (s == "--ticks-per-second") a.ticks_per_second = std::stoll(need());
        else if (s == "--time-model") {
            const auto v = need();
            if (v == "spread") a.spread_within_second = true;
            else if (v == "raw") a.spread_within_second = false;
            else throw std::invalid_argument("time model must be spread or raw");
        } else {
            throw std::invalid_argument("unknown argument: " + s);
        }
    }
    if (a.trace.empty())
        throw std::invalid_argument("usage: trace_bench --trace FILE [--name NAME] [--capacity BYTES] [--latencies-us 0,1000,...] [--limit N] [--time-model spread|raw]");
    if (a.capacity == 0) throw std::invalid_argument("capacity must be positive");
    if (a.ticks_per_second <= 0) throw std::invalid_argument("ticks-per-second must be positive");
    return a;
}

struct Scenario {
    Time latency_us;
    Simulator sim;
    explicit Scenario(Time latency, Bytes capacity)
        : latency_us(latency), sim(Simulator::fixed_lru(capacity, latency)) {}
};

void json_string(std::ostream& out, const std::string& s) {
    out << '"';
    for (char c : s) {
        switch (c) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << c;
        }
    }
    out << '"';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Args a = parse_args(argc, argv);
        std::ifstream file;
        std::istream* in = nullptr;
        if (a.trace == "-") {
            std::cin.sync_with_stdio(false);
            in = &std::cin;
        } else {
            file.open(a.trace, std::ios::binary);
            if (!file) throw std::runtime_error("cannot open trace: " + a.trace);
            in = &file;
        }

        ByteLru instant(a.capacity);
        std::vector<Scenario> scenarios;
        scenarios.reserve(a.latencies_us.size());
        for (Time l : a.latencies_us) scenarios.emplace_back(l, a.capacity);

        std::uint64_t requests = 0;
        std::uint64_t demand_bytes = 0;
        std::uint64_t instant_hits = 0;
        std::uint64_t instant_miss_bytes = 0;
        std::uint64_t bucket_count = 0;
        std::uint64_t max_bucket_requests = 0;
        bool have_first_clock = false;
        std::uint32_t first_clock = 0;
        std::uint32_t last_clock = 0;
        Time last_arrival = 0;

        std::vector<OracleRecord> bucket;
        bucket.reserve(4096);

        auto process_bucket = [&](const std::vector<OracleRecord>& records) {
            if (records.empty()) return;
            ++bucket_count;
            max_bucket_requests = std::max<std::uint64_t>(max_bucket_requests, records.size());
            const auto clock = records.front().clock_time;
            if (!have_first_clock) {
                first_clock = clock;
                have_first_clock = true;
            }
            if (clock < first_clock)
                throw std::runtime_error("trace timestamp precedes first timestamp");
            const std::uint64_t sec_delta = static_cast<std::uint64_t>(clock - first_clock);
            if (sec_delta > static_cast<std::uint64_t>(std::numeric_limits<Time>::max() / a.ticks_per_second))
                throw std::overflow_error("trace time overflows simulator Time");
            const Time base = static_cast<Time>(sec_delta) * a.ticks_per_second;
            const std::size_t m = records.size();

            for (std::size_t k = 0; k < m; ++k) {
                const auto& rec = records[k];
                if (rec.size == 0 || static_cast<Bytes>(rec.size) > a.capacity)
                    throw std::runtime_error("FAST equivalence mode requires 0 < object_size <= cache capacity");
                Time arrival = base;
                if (a.spread_within_second) {
                    const __int128 numerator = static_cast<__int128>(k + 1) * a.ticks_per_second;
                    const Time offset = static_cast<Time>(numerator / static_cast<__int128>(m + 1));
                    arrival += offset;
                }
                if (arrival < last_arrival) throw std::runtime_error("reconstructed time moved backwards");
                last_arrival = arrival;

                ++requests;
                demand_bytes += rec.size;

                const bool hit = instant.on_demand(rec.object, rec.size, arrival);
                if (hit) {
                    ++instant_hits;
                } else {
                    instant_miss_bytes += rec.size;
                    instant.on_fill(rec.object, rec.size, arrival, FetchOrigin::Demand);
                }

                const Request req{arrival, rec.object, rec.size};
                for (auto& s : scenarios) s.sim.demand(req);
            }
            last_clock = clock;
        };

        OracleRecord rec;
        bool have_bucket = false;
        std::uint32_t bucket_clock = 0;
        while ((!a.limit || requests + bucket.size() < a.limit) && read_oracle_record(*in, rec)) {
            if (!have_bucket) {
                bucket_clock = rec.clock_time;
                have_bucket = true;
            }
            if (rec.clock_time < bucket_clock) throw std::runtime_error("OracleGeneral timestamps are not nondecreasing");
            if (rec.clock_time != bucket_clock) {
                process_bucket(bucket);
                bucket.clear();
                if (a.limit && requests >= a.limit) break;
                bucket_clock = rec.clock_time;
            }
            if (!a.limit || requests + bucket.size() < a.limit) bucket.push_back(rec);
        }
        process_bucket(bucket);

        if (requests == 0) throw std::runtime_error("trace contained no requests");

        const double reference_hr = static_cast<double>(instant_hits) / static_cast<double>(requests);
        std::cout << std::setprecision(15);
        std::cout << "{";
        std::cout << "\"trace\":"; json_string(std::cout, a.name);
        std::cout << ",\"requests\":" << requests
                  << ",\"demand_bytes\":" << demand_bytes
                  << ",\"capacity_bytes\":" << a.capacity
                  << ",\"time_model\":"; json_string(std::cout, a.spread_within_second ? "spread" : "raw");
        std::cout << ",\"ticks_per_second\":" << a.ticks_per_second
                  << ",\"coarse_buckets\":" << bucket_count
                  << ",\"max_requests_in_second\":" << max_bucket_requests
                  << ",\"first_clock\":" << first_clock
                  << ",\"last_clock\":" << last_clock
                  << ",\"instant_lru_hits\":" << instant_hits
                  << ",\"instant_lru_hit_ratio\":" << reference_hr
                  << ",\"instant_lru_miss_bytes\":" << instant_miss_bytes
                  << ",\"scenarios\":[";

        bool first = true;
        for (const auto& s : scenarios) {
            const auto& m = s.sim.metrics();
            if (!first) std::cout << ',';
            first = false;
            const double n = static_cast<double>(m.demands);
            const double resident_hr = n ? static_cast<double>(m.resident_hits) / n : 0.0;
            const double coalesced = n ? static_cast<double>(m.demand_coalesced_hits) / n : 0.0;
            const double no_new_io = n ? static_cast<double>(m.resident_hits + m.demand_coalesced_hits) / n : 0.0;
            const double mean_nonresident = (m.demands > m.resident_hits)
                ? static_cast<double>(m.total_latency / (m.demands - m.resident_hits)) : 0.0;
            std::cout << "{\"latency_us\":" << s.latency_us
                      << ",\"resident_hits\":" << m.resident_hits
                      << ",\"resident_hit_ratio\":" << resident_hr
                      << ",\"resident_hit_ratio_delta_vs_instant\":" << (resident_hr - reference_hr)
                      << ",\"demand_coalesced_hits\":" << m.demand_coalesced_hits
                      << ",\"demand_coalesced_ratio\":" << coalesced
                      << ",\"new_misses\":" << m.new_misses
                      << ",\"served_without_new_io_ratio\":" << no_new_io
                      << ",\"backend_fetches\":" << m.backend_fetches
                      << ",\"backend_bytes\":" << m.backend_bytes
                      << ",\"average_access_time_us\":" << m.average_access_time()
                      << ",\"mean_nonresident_wait_us\":" << mean_nonresident;
            if (s.latency_us > 0)
                std::cout << ",\"aat_fraction_of_backend_latency\":" << (m.average_access_time() / static_cast<double>(s.latency_us));
            std::cout << "}";
        }
        std::cout << "]}\n";

        const auto zero_it = std::find_if(scenarios.begin(), scenarios.end(), [](const Scenario& s) { return s.latency_us == 0; });
        if (zero_it == scenarios.end()) throw std::logic_error("zero-latency scenario missing");
        const auto& z = zero_it->sim.metrics();
        if (z.resident_hits != instant_hits || z.new_misses != requests - instant_hits || z.demand_coalesced_hits != 0)
            throw std::runtime_error("zero-latency simulator does not reproduce immediate byte-LRU");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "trace_bench: " << e.what() << '\n';
        return 2;
    }
}
