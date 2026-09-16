#include "../src/sim.hpp"
#include "../src/time_model.hpp"
#include "../src/prefetchers.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <vector>

using namespace lapsim;

static void test_latency_saving_curve() {
    const std::vector<Time> leads{0, 2, 5, 10, 20};
    const std::vector<Time> expected{0, 2, 5, 10, 10};
    for (std::size_t i = 0; i < leads.size(); ++i) {
        auto with_pf = Simulator::fixed_lru(1024, 10);
        assert(with_pf.prefetch(0, 7, 1) == PrefetchClass::Issued);
        auto p = with_pf.demand({leads[i], 7, 1});
        auto baseline = Simulator::fixed_lru(1024, 10);
        auto b = baseline.demand({leads[i], 7, 1});
        assert(b.latency - p.latency == expected[i]);
    }
}

static void test_demand_coalescing() {
    auto sim = Simulator::fixed_lru(1024, 10);
    auto first = sim.demand({0, 11, 4});
    auto second = sim.demand({3, 11, 4});
    assert(first.kind == AccessClass::NewMiss && first.latency == 10);
    assert(second.kind == AccessClass::DemandCoalesced && second.latency == 7);
    assert(sim.metrics().backend_fetches == 1);
}

static void test_prefetch_delayed_hit() {
    auto sim = Simulator::fixed_lru(1024, 10);
    sim.prefetch(0, 12, 4);
    auto r = sim.demand({6, 12, 4});
    assert(r.kind == AccessClass::PrefetchDelayed && r.latency == 4);
}

static void test_completion_precedes_equal_time_demand() {
    auto sim = Simulator::fixed_lru(1024, 10);
    sim.prefetch(0, 13, 4);
    auto r = sim.demand({10, 13, 4});
    assert(r.kind == AccessClass::ResidentHit && r.latency == 0);
}

static void test_fill_time_controls_lru_and_pollution() {
    auto sim = Simulator::fixed_lru(1, 10);
    sim.prefetch(0, 1, 1);
    auto r2 = sim.demand({5, 2, 1});
    assert(r2.kind == AccessClass::NewMiss);
    sim.advance_to(10); assert(sim.cache_contains(1));
    sim.advance_to(15); assert(!sim.cache_contains(1)); assert(sim.cache_contains(2));
}

static void test_redundant_prefetch_does_not_touch_resident_lru() {
    auto sim = Simulator::fixed_lru(2, 1);
    sim.demand({0, 1, 1}); sim.advance_to(1);
    sim.demand({1, 2, 1}); sim.advance_to(2);
    assert(sim.prefetch(2, 1, 1) == PrefetchClass::ResidentRedundant);
    sim.demand({2, 3, 1}); sim.advance_to(3);
    assert(!sim.cache_contains(1)); assert(sim.cache_contains(2)); assert(sim.cache_contains(3));
}

static void test_oversize_prefetch_is_suppressed_but_demand_fetches() {
    auto sim = Simulator::fixed_lru(8, 5);
    assert(sim.prefetch(0, 99, 9) == PrefetchClass::OversizeSuppressed);
    auto r = sim.demand({0, 99, 9});
    assert(r.kind == AccessClass::NewMiss && r.latency == 5);
}

static void test_zero_latency_completes_immediately() {
    auto sim = Simulator::fixed_lru(8, 0);
    auto first = sim.demand({0, 41, 1});
    assert(first.kind == AccessClass::NewMiss && first.latency == 0);
    assert(sim.cache_contains(41));
    assert(sim.demand({0, 41, 1}).kind == AccessClass::ResidentHit);
}

static void test_affine_backend() {
    auto backend = std::make_shared<AffineLatencyBackend>(3, 4);
    Simulator sim(std::make_unique<ByteLru>(100), backend);
    assert(sim.demand({0, 52, 9}).latency == 6);
}

static void test_time_reconstruction() {
    std::vector<Time> coarse{5, 5, 5, 6};
    assert((spread_equal_timestamp_buckets(coarse, 1000) == std::vector<Time>{5250, 5500, 5750, 6500}));
    assert((request_clock(4, 7, 3) == std::vector<Time>{3, 10, 17, 24}));
}

static void test_obl() {
    OblPrefetcher p;
    auto v = p.on_demand({0, 10, 4});
    assert(v.size() == 1 && v[0].object == 11 && v[0].size == 4);
}

static void test_stride() {
    StridePrefetcher p;
    assert(p.on_demand({0, 10, 4}).empty());
    assert(p.on_demand({1, 12, 4}).empty());
    auto v = p.on_demand({2, 14, 4});
    assert(v.size() == 1 && v[0].object == 16);
}

static void test_probability_graph() {
    ProbabilityGraphPrefetcher p(4, 2);
    p.on_demand({0, 1, 4}); p.on_demand({1, 2, 4});
    p.on_demand({2, 1, 4}); p.on_demand({3, 2, 4});
    auto v = p.on_demand({4, 1, 4});
    assert(v.size() == 1 && v[0].object == 2);
}

static void test_prefetcher_with_latency() {
    OblPrefetcher p;
    auto sim = Simulator::fixed_lru(100, 10);
    Request r0{0, 1, 1};
    sim.demand(r0);
    for (const auto& pred : p.on_demand(r0)) sim.prefetch(r0.arrival, pred.object, pred.size);
    auto r1 = sim.demand({5, 2, 1});
    assert(r1.kind == AccessClass::PrefetchDelayed && r1.latency == 5);
}

int main() {
    test_time_reconstruction(); test_zero_latency_completes_immediately(); test_affine_backend();
    test_latency_saving_curve(); test_demand_coalescing(); test_prefetch_delayed_hit();
    test_completion_precedes_equal_time_demand(); test_fill_time_controls_lru_and_pollution();
    test_redundant_prefetch_does_not_touch_resident_lru(); test_oversize_prefetch_is_suppressed_but_demand_fetches();
    test_obl(); test_stride(); test_probability_graph(); test_prefetcher_with_latency();
    std::cout << "all latency model and prefetcher tests passed\n";
    return 0;
}
