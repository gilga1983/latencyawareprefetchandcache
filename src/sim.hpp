#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lapsim {

using ObjectId = std::uint64_t;
using Bytes = std::uint64_t;
using Time = std::int64_t;

struct Request {
    Time arrival = 0;
    ObjectId object = 0;
    Bytes size = 0;
};

enum class FetchOrigin { Demand, Prefetch };
enum class AccessClass { ResidentHit, PrefetchDelayed, DemandCoalesced, NewMiss };
enum class PrefetchClass { Issued, ResidentRedundant, InflightRedundant, OversizeSuppressed };

struct AccessResult {
    Time latency = 0;
    AccessClass kind = AccessClass::NewMiss;
};

struct Metrics {
    std::uint64_t demands = 0;
    long double total_latency = 0;
    long double prefetch_delayed_latency = 0;
    long double demand_coalesced_latency = 0;
    long double new_miss_latency = 0;
    std::uint64_t resident_hits = 0;
    std::uint64_t prefetch_delayed_hits = 0;
    std::uint64_t demand_coalesced_hits = 0;
    std::uint64_t new_misses = 0;

    std::uint64_t backend_fetches = 0;
    Bytes backend_bytes = 0;
    std::uint64_t prefetch_fetches = 0;
    Bytes prefetch_bytes = 0;
    std::uint64_t redundant_prefetch_resident = 0;
    std::uint64_t redundant_prefetch_inflight = 0;
    std::uint64_t suppressed_prefetch_oversize = 0;

    double average_access_time() const {
        return demands ? static_cast<double>(total_latency / demands) : 0.0;
    }
    double average_demand_coalesced_wait() const {
        return demand_coalesced_hits
            ? static_cast<double>(demand_coalesced_latency / demand_coalesced_hits)
            : 0.0;
    }
    double average_prefetch_delayed_wait() const {
        return prefetch_delayed_hits
            ? static_cast<double>(prefetch_delayed_latency / prefetch_delayed_hits)
            : 0.0;
    }
};

class BackendModel {
public:
    virtual ~BackendModel() = default;
    virtual Time latency(ObjectId object, Bytes size, Time issue_time) const = 0;
};

class FixedLatencyBackend final : public BackendModel {
public:
    explicit FixedLatencyBackend(Time latency) : latency_(latency) {
        if (latency_ < 0) throw std::invalid_argument("backend latency must be non-negative");
    }
    Time latency(ObjectId, Bytes, Time) const override { return latency_; }
private:
    Time latency_;
};

class AffineLatencyBackend final : public BackendModel {
public:
    AffineLatencyBackend(Time base, Bytes bytes_per_time_unit)
        : base_(base), bandwidth_(bytes_per_time_unit) {
        if (base_ < 0) throw std::invalid_argument("base latency must be non-negative");
        if (bandwidth_ == 0) throw std::invalid_argument("bandwidth must be positive");
    }
    Time latency(ObjectId, Bytes size, Time) const override {
        const auto transfer = static_cast<Time>((size + bandwidth_ - 1) / bandwidth_);
        return base_ + transfer;
    }
private:
    Time base_;
    Bytes bandwidth_;
};

class CachePolicy {
public:
    virtual ~CachePolicy() = default;
    virtual Bytes capacity() const = 0;
    virtual Bytes used() const = 0;
    virtual bool contains(ObjectId object) const = 0;
    virtual bool on_demand(ObjectId object, Bytes size, Time now) = 0;
    virtual void on_fill(ObjectId object, Bytes size, Time now, FetchOrigin origin) = 0;
};

class ByteLru final : public CachePolicy {
public:
    explicit ByteLru(Bytes capacity) : capacity_(capacity) {}

    Bytes capacity() const override { return capacity_; }
    Bytes used() const override { return used_; }
    bool contains(ObjectId x) const override { return entries_.find(x) != entries_.end(); }
    std::size_t objects() const { return entries_.size(); }

    bool on_demand(ObjectId x, Bytes, Time) override {
        auto it = entries_.find(x);
        if (it == entries_.end()) return false;
        order_.splice(order_.end(), order_, it->second.pos);
        it->second.pos = std::prev(order_.end());
        return true;
    }

    void on_fill(ObjectId x, Bytes size, Time, FetchOrigin) override {
        if (size > capacity_) return;
        auto it = entries_.find(x);
        if (it != entries_.end()) {
            used_ -= it->second.size;
            order_.erase(it->second.pos);
            entries_.erase(it);
        }
        while (used_ + size > capacity_ && !order_.empty()) {
            ObjectId victim = order_.front();
            order_.pop_front();
            auto vit = entries_.find(victim);
            used_ -= vit->second.size;
            entries_.erase(vit);
        }
        order_.push_back(x);
        entries_.emplace(x, Entry{size, std::prev(order_.end())});
        used_ += size;
    }

private:
    struct Entry {
        Bytes size;
        std::list<ObjectId>::iterator pos;
    };

    Bytes capacity_ = 0;
    Bytes used_ = 0;
    std::list<ObjectId> order_;
    std::unordered_map<ObjectId, Entry> entries_;
};

class Simulator {
public:
    Simulator(std::unique_ptr<CachePolicy> cache, std::shared_ptr<const BackendModel> backend)
        : cache_(std::move(cache)), backend_(std::move(backend)) {
        if (!cache_ || !backend_) throw std::invalid_argument("cache and backend are required");
    }

    static Simulator fixed_lru(Bytes cache_capacity, Time backend_latency) {
        return Simulator(std::make_unique<ByteLru>(cache_capacity),
                         std::make_shared<FixedLatencyBackend>(backend_latency));
    }

    AccessResult demand(const Request& r) {
        advance_to(r.arrival);
        metrics_.demands++;

        if (cache_->on_demand(r.object, r.size, now_)) {
            metrics_.resident_hits++;
            return record({0, AccessClass::ResidentHit});
        }

        auto fit = inflight_.find(r.object);
        if (fit != inflight_.end()) {
            Time wait = fit->second.completion - now_;
            if (wait < 0) throw std::logic_error("in-flight completion is in the past");
            if (fit->second.origin == FetchOrigin::Prefetch) {
                metrics_.prefetch_delayed_hits++;
                return record({wait, AccessClass::PrefetchDelayed});
            }
            metrics_.demand_coalesced_hits++;
            return record({wait, AccessClass::DemandCoalesced});
        }

        Time latency = backend_->latency(r.object, r.size, now_);
        issue_fetch(r.object, r.size, FetchOrigin::Demand, latency);
        if (latency == 0) advance_to(now_);
        metrics_.new_misses++;
        return record({latency, AccessClass::NewMiss});
    }

    PrefetchClass prefetch(Time issue_time, ObjectId object, Bytes size) {
        advance_to(issue_time);

        if (cache_->contains(object)) {
            metrics_.redundant_prefetch_resident++;
            return PrefetchClass::ResidentRedundant;
        }
        if (inflight_.find(object) != inflight_.end()) {
            metrics_.redundant_prefetch_inflight++;
            return PrefetchClass::InflightRedundant;
        }
        if (size > cache_->capacity()) {
            metrics_.suppressed_prefetch_oversize++;
            return PrefetchClass::OversizeSuppressed;
        }

        Time latency = backend_->latency(object, size, now_);
        issue_fetch(object, size, FetchOrigin::Prefetch, latency);
        if (latency == 0) advance_to(now_);
        metrics_.prefetch_fetches++;
        metrics_.prefetch_bytes += size;
        return PrefetchClass::Issued;
    }

    void advance_to(Time t) {
        if (t < now_) throw std::invalid_argument("simulation time cannot move backwards");
        while (!completions_.empty() && completions_.top().completion <= t) {
            auto event = completions_.top();
            completions_.pop();
            now_ = event.completion;
            auto it = inflight_.find(event.object);
            if (it == inflight_.end()) continue;
            if (it->second.completion != event.completion) continue;
            auto fetch = it->second;
            inflight_.erase(it);
            cache_->on_fill(event.object, fetch.size, now_, fetch.origin);
        }
        now_ = t;
    }

    Time now() const { return now_; }
    const Metrics& metrics() const { return metrics_; }
    bool cache_contains(ObjectId x) const { return cache_->contains(x); }
    Bytes cache_used() const { return cache_->used(); }
    bool inflight(ObjectId x) const { return inflight_.find(x) != inflight_.end(); }

private:
    struct Inflight {
        Time issue;
        Time completion;
        Bytes size;
        FetchOrigin origin;
    };
    struct Completion {
        Time completion;
        ObjectId object;
        bool operator>(const Completion& other) const {
            if (completion != other.completion) return completion > other.completion;
            return object > other.object;
        }
    };

    AccessResult record(AccessResult r) {
        metrics_.total_latency += r.latency;
        switch (r.kind) {
            case AccessClass::PrefetchDelayed:
                metrics_.prefetch_delayed_latency += r.latency;
                break;
            case AccessClass::DemandCoalesced:
                metrics_.demand_coalesced_latency += r.latency;
                break;
            case AccessClass::NewMiss:
                metrics_.new_miss_latency += r.latency;
                break;
            case AccessClass::ResidentHit:
                break;
        }
        return r;
    }

    void issue_fetch(ObjectId object, Bytes size, FetchOrigin origin, Time latency) {
        if (latency < 0) throw std::logic_error("negative backend latency");
        Time completion = now_ + latency;
        auto [it, inserted] = inflight_.emplace(object, Inflight{now_, completion, size, origin});
        if (!inserted) throw std::logic_error("duplicate fetch issuance");
        completions_.push({completion, object});
        metrics_.backend_fetches++;
        metrics_.backend_bytes += size;
    }

    Time now_ = 0;
    std::unique_ptr<CachePolicy> cache_;
    std::shared_ptr<const BackendModel> backend_;
    Metrics metrics_;
    std::unordered_map<ObjectId, Inflight> inflight_;
    std::priority_queue<Completion, std::vector<Completion>, std::greater<Completion>> completions_;
};

} // namespace lapsim
