#pragma once

#include "sim.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lapsim {

struct Prediction {
    ObjectId object = 0;
    Bytes size = 0;
};

// Prefetchers observe the demand stream only. They emit object predictions;
// cache residency, in-flight coalescing, fetch timing and admission remain the
// simulator's responsibility.
class Prefetcher {
public:
    virtual ~Prefetcher() = default;
    virtual std::string name() const = 0;
    virtual std::vector<Prediction> on_demand(const Request& request) = 0;
};

class NoPrefetcher final : public Prefetcher {
public:
    std::string name() const override { return "none"; }
    std::vector<Prediction> on_demand(const Request&) override { return {}; }
};

// One-block-lookahead. Object IDs are interpreted as adjacent logical blocks.
class OblPrefetcher final : public Prefetcher {
public:
    std::string name() const override { return "obl"; }
    std::vector<Prediction> on_demand(const Request& r) override {
        if (r.object == ~ObjectId{0}) return {};
        return {{r.object + 1, r.size}};
    }
};

// Lightweight last-delta stride detector. Two consecutive equal non-zero
// deltas establish a stride; the next logical object is then predicted.
class StridePrefetcher final : public Prefetcher {
public:
    std::string name() const override { return "stride"; }
    std::vector<Prediction> on_demand(const Request& r) override {
        std::vector<Prediction> out;
        if (have_last_) {
            const std::int64_t delta = signed_delta(r.object, last_);
            if (have_delta_ && delta != 0 && delta == last_delta_) {
                ObjectId next;
                if (add_delta(r.object, delta, next)) out.push_back({next, r.size});
            }
            last_delta_ = delta;
            have_delta_ = true;
        }
        last_ = r.object;
        have_last_ = true;
        return out;
    }
private:
    static std::int64_t signed_delta(ObjectId a, ObjectId b) {
        if (a >= b) {
            const auto d = a - b;
            return d > static_cast<ObjectId>(INT64_MAX) ? INT64_MAX : static_cast<std::int64_t>(d);
        }
        const auto d = b - a;
        return d > static_cast<ObjectId>(INT64_MAX) ? INT64_MIN : -static_cast<std::int64_t>(d);
    }
    static bool add_delta(ObjectId x, std::int64_t d, ObjectId& out) {
        if (d >= 0) {
            const auto u = static_cast<ObjectId>(d);
            if (x > ~ObjectId{0} - u) return false;
            out = x + u;
        } else {
            const auto u = static_cast<ObjectId>(-(d + 1)) + 1;
            if (x < u) return false;
            out = x - u;
        }
        return true;
    }
    bool have_last_ = false;
    bool have_delta_ = false;
    ObjectId last_ = 0;
    std::int64_t last_delta_ = 0;
};

// Compact first-order probability graph. For each object, keep bounded
// transition counts to successors and predict the most frequent successor.
// This is deliberately a transparent PG baseline, not a claim of bit-for-bit
// equivalence with any particular published implementation.
class ProbabilityGraphPrefetcher final : public Prefetcher {
public:
    explicit ProbabilityGraphPrefetcher(std::size_t max_successors = 8,
                                        std::uint32_t min_support = 2)
        : max_successors_(max_successors), min_support_(min_support) {}

    std::string name() const override { return "pg"; }
    std::vector<Prediction> on_demand(const Request& r) override {
        std::vector<Prediction> out;
        auto it = graph_.find(r.object);
        if (it != graph_.end()) {
            ObjectId best = 0;
            std::uint32_t best_count = 0;
            for (const auto& p : it->second) {
                if (p.second > best_count || (p.second == best_count && p.first < best)) {
                    best = p.first;
                    best_count = p.second;
                }
            }
            if (best_count >= min_support_) out.push_back({best, r.size});
        }
        if (have_last_) update(last_, r.object);
        last_ = r.object;
        have_last_ = true;
        return out;
    }
private:
    using Edge = std::pair<ObjectId, std::uint32_t>;
    void update(ObjectId from, ObjectId to) {
        auto& v = graph_[from];
        for (auto& p : v) {
            if (p.first == to) { if (p.second != UINT32_MAX) ++p.second; return; }
        }
        if (v.size() < max_successors_) { v.push_back({to, 1}); return; }
        // Space-saving style replacement keeps memory bounded per source.
        auto victim = v.begin();
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it->second < victim->second) victim = it;
        const auto inherited = victim->second;
        *victim = {to, inherited == UINT32_MAX ? UINT32_MAX : inherited + 1};
    }
    std::size_t max_successors_;
    std::uint32_t min_support_;
    bool have_last_ = false;
    ObjectId last_ = 0;
    std::unordered_map<ObjectId, std::vector<Edge>> graph_;
};

// Future-distance oracle for diagnostic upper bounds. The driver supplies the
// next request h positions ahead. Keeping it separate prevents accidental use
// as a deployable predictor.
class HorizonOraclePrefetcher final : public Prefetcher {
public:
    explicit HorizonOraclePrefetcher(std::size_t horizon) : horizon_(horizon) {}
    std::string name() const override { return "oracle-h" + std::to_string(horizon_); }
    std::vector<Prediction> on_demand(const Request&) override { return {}; }
    std::size_t horizon() const { return horizon_; }
private:
    std::size_t horizon_;
};

inline std::unique_ptr<Prefetcher> make_prefetcher(const std::string& name) {
    if (name == "none") return std::make_unique<NoPrefetcher>();
    if (name == "obl") return std::make_unique<OblPrefetcher>();
    if (name == "stride") return std::make_unique<StridePrefetcher>();
    if (name == "pg") return std::make_unique<ProbabilityGraphPrefetcher>();
    throw std::invalid_argument("unknown prefetcher: " + name);
}

} // namespace lapsim
