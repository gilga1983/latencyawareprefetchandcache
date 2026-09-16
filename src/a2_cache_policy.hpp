#pragma once
#include "sim.hpp"
#include "elastic_dual_lru.hpp"

namespace lapsim {

// Adapter that lets the latency event engine exercise A2's elastic C/P store.
// Fetch timing remains entirely in Simulator; this class owns only residency.
class A2CachePolicy final : public CachePolicy {
public:
    A2CachePolicy(Bytes capacity, Bytes cache_quota)
        : store_(capacity, cache_quota) {}

    Bytes capacity() const override { return store_.capacity(); }
    Bytes used() const override { return store_.used(); }
    bool contains(ObjectId x) const override {
        return store_.in_cache(x) || store_.in_prefetch(x);
    }

    bool on_demand(ObjectId x, Bytes size, Time) override {
        const auto k = store_.demand(x, size);
        last_demand_kind_ = k;
        return k != ElasticDualLru::DemandKind::Miss;
    }

    void on_fill(ObjectId x, Bytes size, Time, FetchOrigin origin) override {
        if (origin == FetchOrigin::Demand) store_.demand_fill(x, size);
        else store_.prefetch_fill(x, size);
    }

    void set_cache_quota(Bytes q) { store_.set_cache_quota(q); }
    Bytes cache_quota() const { return store_.cache_quota(); }
    Bytes prefetch_quota() const { return store_.prefetch_quota(); }
    Bytes cache_bytes() const { return store_.cache_bytes(); }
    Bytes prefetch_bytes() const { return store_.prefetch_bytes(); }
    bool in_cache(ObjectId x) const { return store_.in_cache(x); }
    bool in_prefetch(ObjectId x) const { return store_.in_prefetch(x); }
    ElasticDualLru::DemandKind last_demand_kind() const { return last_demand_kind_; }
    void check() const { store_.check(); }

private:
    ElasticDualLru store_;
    ElasticDualLru::DemandKind last_demand_kind_ = ElasticDualLru::DemandKind::Miss;
};

} // namespace lapsim
