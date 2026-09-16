# Latency-Aware Prefetch and Cache

Research simulator for studying cache replacement and prefetching when backend accesses take time. The central distinction from a hit-ratio simulator is that an access can arrive while its object is already being fetched: the request then experiences a **delayed hit** and waits only for the residual fetch time.

The initial model is frozen in [`MODEL.md`](MODEL.md). The C++17 kernel implements explicit completion events, shared demand/prefetch cache fills, request coalescing, delayed-hit attribution, byte-LRU, pluggable cache-policy hooks, and pluggable backend-latency models.

## Build and test

```bash
make test
```

The deterministic unit tests currently check:

- the fixed-latency lead-value curve: with backend latency 10, prediction leads `0,2,5,10,20` save `0,2,5,10,10`;
- demand coalescing;
- delayed hits on in-flight prefetches;
- completion-before-demand ordering at equal timestamps;
- fill-time LRU pollution/eviction;
- redundant predictions do not refresh resident LRU state;
- oversize-prefetch suppression;
- deterministic reconstruction of coarse equal-timestamp buckets;
- zero-latency immediate completion;
- affine size-dependent backend latency.

## Current layout

- `MODEL.md` — versioned simulator semantics / research model.
- `src/sim.hpp` — event-driven simulator core, byte-LRU policy interface, backend models, metrics.
- `src/time_model.hpp` — request-clock and coarse-timestamp reconstruction helpers.
- `tests/test_sim.cpp` — deterministic model/invariant tests.

## Next step

Wire the FAST/RAP trace format and predictor interface into this core while keeping the old hit-ratio run as a regression mode. Then add paired reactive-only shadow execution so predictors can be scored by marginal latency saved rather than binary residual hits.
