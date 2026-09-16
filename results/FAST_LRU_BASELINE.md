# FAST trace LRU latency baseline

Initial validation and exploratory results for latency-aware LRU. The trace reader consumes the same 24-byte libCacheSim OracleGeneral records (`<IQIq>`) used by the submitted FAST/RAP evaluation. Cache capacity is 256 MiB. All valid requests are included, matching the paper harness.

## Zero-latency equivalence

The simulator at backend latency `L=0` is required to reproduce an immediate-fill byte-LRU exactly. For the complete selected traces below, the independent immediate-LRU implementation also reproduces the frozen FAST paper hit ratios (differences are only decimal-rounding noise).

| Trace | Requests | FAST LRU HR | New `L=0` HR |
|---|---:|---:|---:|
| `msr_hm_0` | 3,993,316 | 0.634662270654 | 0.634662270654263 |
| `w90` | 4,493,515 | 0.232112277360 | 0.232112277359706 |
| `w91` | 4,316,605 | 0.436925778476 | 0.436925778476372 |
| `w92` | 4,284,658 | 0.169992797558 | 0.169992797558172 |
| `w93` | 3,351,357 | 0.134939369336 | 0.134939369336063 |
| `w94` | 4,118,188 | 0.037890450849 | 0.037890450848771 |
| `w95` | 3,937,240 | 0.652779866099 | 0.652779866099095 |

The one-million-request screen over all 20 FAST traces also passes the exact internal invariant: the event-driven simulator at `L=0` has the same resident hits, misses, and byte-LRU state transitions as the independent immediate-LRU reference.

## What changes at nonzero latency?

The default timing model spreads requests deterministically within each one-second trace bucket while preserving request order. The table below uses fixed backend latency `L=10 ms` on complete traces.

| Trace | Instant HR | Resident HR | Demand-coalesced | I/O avoidance (`resident + coalesced`) | AAT |
|---|---:|---:|---:|---:|---:|
| `msr_hm_0` | 63.4662% | 63.3608% | 0.1057% | 63.4666% | 3.659 ms |
| `w90` | 23.2112% | 23.2084% | 0.0039% | 23.2123% | 7.679 ms |
| `w91` | 43.6926% | 43.4586% | 0.2359% | 43.6945% | 5.644 ms |
| `w92` | 16.9993% | 16.9492% | 0.0501% | 16.9993% | 8.302 ms |
| `w93` | 13.4939% | 12.8412% | 0.6527% | 13.4939% | 8.698 ms |
| `w94` | 3.7890% | 3.7877% | 0.0014% | 3.7891% | 9.621 ms |
| `w95` | 65.2780% | 65.2076% | 0.0709% | 65.2785% | 3.475 ms |

The main effect is a reclassification of some accesses that an instantaneous simulator called hits. They arrive before the earlier miss has completed, so they become demand-coalesced delayed hits. Consequently:

- resident hit ratio falls as latency grows;
- almost the same request mass appears as demand-coalesced hits;
- I/O avoidance remains extremely close to the old instantaneous hit ratio;
- backend fetch counts/bytes therefore barely change;
- average access time increases because the coalesced accesses now expose their residual wait.

At 10 ms on these complete traces, a coalesced request arrives about halfway through its outstanding fetch on average, although the workload-to-workload variation is large.

A useful derived baseline is the **fill-delay tax** relative to the fictitious instantaneous-fill estimate `(1-H_instant)*L`. At 10 ms it is small but measurable: approximately 0.55% extra AAT on `w93`, 0.24% on `w91`, and 0.15% on `msr_hm_0`. This is latency that ordinary hit ratio cannot represent.

## Broad 1M-request screen

The first one million requests of every FAST trace were run at `L = 0, 1, 5, 10, 20 ms`. At 10 ms, mean demand coalescing across the 20 equally sized screens is 0.2593% of requests. The strongest early-phase examples are:

| Trace | Instant HR | Resident HR @ 10 ms | Coalesced @ 10 ms | AAT @ 10 ms |
|---|---:|---:|---:|---:|
| `msr_prn_0` | 66.2468% | 63.7892% | 2.4576% | 3.459 ms |
| `msr_prn_1` | 29.4785% | 28.0674% | 1.4115% | 7.110 ms |
| `w93` | 13.9314% | 13.3563% | 0.5752% | 8.654 ms |
| `w91` | 47.9648% | 47.7409% | 0.2257% | 5.216 ms |
| `msr_proj_0` | 68.8398% | 68.6990% | 0.1409% | 3.123 ms |
| `msr_prxy_1` | 97.4371% | 97.3331% | 0.1040% | 0.262 ms |

The strong `prn` early-phase behavior also shows that delayed-hit prevalence is phase/locality dependent, not simply a function of requests per second.

## Coarse timestamp sensitivity

The MSR/CloudPhysics OracleGeneral clock is one-second granularity for these traces. Treating every request with the same recorded second as literally simultaneous is therefore inappropriate for millisecond-scale latency.

As a sensitivity test, the same 20 one-million-request screens were run with raw equal timestamps. At 10 ms:

- mean demand-coalesced ratio rises from **0.2593%** under deterministic within-second spreading to **1.6881%** with raw equal timestamps;
- mean AAT rises from **6.435 ms** to **6.589 ms**;
- `msr_prn_0` coalescing rises from **2.4576%** to **14.5877%**.

Under raw timestamps, the coalesced fraction is effectively identical for 1, 5, 10, and 20 ms as long as the latency remains below one second, because all requests in a bucket are artificially simultaneous. This is a clock-resolution artifact. The default experiments should therefore use within-second reconstruction and later add randomized reconstruction as an uncertainty/sensitivity band.

## Takeaways for the next simulator stage

1. **The zero-latency bridge is clean.** The new event model recovers the old FAST byte-LRU result when latency is removed.
2. **Resident hit ratio is no longer enough.** `H_R`, demand-coalesced `H_C`, I/O-avoidance `H_IO`, and AAT expose different phenomena.
3. **Latency mostly changes waiting, not backend traffic, for demand-only LRU.** This makes AAT a genuinely new objective instead of a renamed miss ratio.
4. **There is meaningful room for latency-aware prefetching even when backend traffic is unchanged.** A prefetch can turn a demand-coalesced wait into a shorter delayed wait or a resident hit.
5. **Timestamp reconstruction must be part of experimental methodology.** Millisecond conclusions cannot be derived from one-second clocks without a stated within-bucket model and sensitivity analysis.

The next experiment should pair this reactive LRU execution with the RAP predictor stream and score predictions by marginal latency saved, while retaining the demand-only run as the counterfactual baseline.
