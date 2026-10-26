# Refined performance model results (v2)

Updated October 26, 2026. These results supersede the v1 flat-tax sensitivity examples.

## Main outcome

The revised model no longer assumes all prefix accesses go off chip or that hypothetical savings are free. At 50% lower KV event overhead, the fitting-cache configuration becomes **1.34% slower** after fork/flush and expected recovery costs. The capacity-streaming configuration improves by **5.00%**, and its bandwidth-limited variant improves by **9.25%**. These remain conditional simulations, not validated hardware speedups.

| Configuration | Reference (us) | Half KV overhead (us) | Time reduction | Speedup |
| --- | ---: | ---: | ---: | ---: |
| 4 KiB prefix fits in 64 KiB cache | 38.447 | 38.960 | -1.34% | 0.987x |
| 64 KiB prefix; 4 KiB cache | 427.808 | 406.436 | +5.00% | 1.053x |
| Capacity streaming; 2 GB/s link | 1443.186 | 1309.666 | +9.25% | 1.102x |
| Capacity streaming; 10,000 ns compute | 5975.217 | 5962.002 | +0.22% | 1.002x |
| Fitting cache; sequential compute/memory | 51.346 | 51.863 | -1.01% | 0.990x |
| Fitting cache; eager next-step compute | 33.123 | 33.883 | -2.30% | 0.978x |
| Fitting; fixed barrier/flush/recovery disabled | 38.447 | 37.300 | +2.98% | 1.031x |
| Fitting; 2 ns extra check per KV access | 38.447 | 39.676 | -3.20% | 0.969x |
| Fitting; 10% expected recovery rate | 38.447 | 49.167 | -27.88% | 0.782x |
| Fitting; higher fork/region barriers | 38.447 | 44.120 | -14.76% | 0.871x |
| Only metadata accesses | 22.852 | 22.852 | +0.00% | 1.000x |

Positive time reduction means faster; negative means slower. The fixed-barrier/flush/recovery-disabled case still pays the 2.5 ns control-distribution transfer at the fork. Within-agent overlap lowers total execution cost, but its effect on the relative benefit of the hypothetical optimization need not be monotonic.

## Where the default costs come from

The logical workload still has 8,520 accesses, but only **838** require off-chip transactions in this configuration. There are **7,682 data hits**. The baseline profiler reuses the original directory engine with finite LRU caches; it reports:

| Kind | Logical accesses | Data hits | Fills | Directory lookups | Snoops | Invalidations | Writebacks |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| metadata | 584 | 322 | 262 | 262 | 1 | 257 | 257 |
| prefix_kv | 6,400 | 6,208 | 192 | 192 | 64 | 0 | 64 |
| leaf_kv | 1,536 | 1,152 | 384 | 384 | 0 | 0 | 0 |

Only 128 of the 6,144 child prefix reads miss: the child on the original lead accelerator retains its prefix, and the two new endpoints fill 64 lines each. The prefix category also includes 64 lead fills. Private leaves incur 384 first-line acquisitions across 1,536 stores; the other 1,152 stores hit locally with no repeated directory lookup. These cache effects were absent from v1.

The candidate flushes **64 resident dirty prefix lines** at the fork and pays **1,552.5 ns** of critical-path fork overhead. Cleaning the profiled copies removes their later dirty-owner writebacks, so the total writeback payload is conserved rather than charged twice. The reference serves those writebacks on demand.

The 50% candidate also budgets 5.76 expected recoveries (1% of 576 KV permission transactions), adding 1,152 ns of aggregate service latency plus control bytes. This aggregate latency overlaps across agents and is not added directly to elapsed execution time. The error rate and costs are user assumptions; no sharing predictor is simulated.

## Model details and remaining assumptions

- Per-node finite, fully associative LRU caches; configurable cache and working-set sizes.
- Event costs come from baseline directory, snoop, invalidation and writeback counts, separated by access kind.
- Two-stage bounded compute lookahead starts after a configurable fraction of the current batch completes. Next-step memory waits for the preceding batch, preserving the configured memory window.
- Fork costs occur exactly at the lead/children boundary and include dirty transfer traffic, barrier budgets, and per-region costs.
- Profiles use fixed logical round-robin ordering. Timing changes do not feed back into replacement or invalidation ordering.
- Event counts are properties of the abstract baseline engine, not exact physical MESI or hardware ground truth.
- Nanosecond and message-size parameters remain assumptions. Baseline memory is pipelined without an additional finite-bandwidth DRAM controller.
- The old 6.76%/14.62% numbers are archived v1 examples, not guaranteed speedups or a formally proven universal upper bound. The v2 workload also has a more explicit line-reuse/address pattern, so absolute v1/v2 runtime differences are not a controlled single-factor experiment.

## CPU probe measurements

Collected on `gl3068.arc-ts.umich.edu`, Intel Xeon Gold 6154. The environment report shows 1 MiB L2 and 25,344 KiB L3. Three consumer threads were pinned successfully to CPUs 27, 31, and 35. Each table row pools five repetitions from each of three consumers (15 values). These are dependent pointer-walk times, including load/store and loop overhead, not isolated coherence-event latencies.

| Case and working set | Median ns/line | Min | Max |
| --- | ---: | ---: | ---: |
| cold_4194304 | 91.259 | 89.812 | 92.705 |
| cold_65536 | 29.850 | 28.191 | 42.123 |
| handoff_4194304 | 42.074 | 30.336 | 77.855 |
| handoff_65536 | 16.395 | 15.502 | 24.169 |
| private_4194304 | 80.083 | 44.440 | 87.793 |
| private_65536 | 5.401 | 5.377 | 5.525 |
| warm_4194304 | 36.769 | 32.511 | 53.392 |
| warm_65536 | 5.803 | 5.764 | 5.852 |

Unflushed 4 MiB data exceeds this CPU's local L2. A flushed read has already forced a dirty writeback and cannot be interpreted as a dirty-owner read. Handoff does not guarantee every line remains dirty in its producer cache until consumption. Readers interfere, and NUMA placement, prefetching, TLB effects, and scheduling can affect measurements. Accordingly these measurements are saved separately and **not automatically treated as CXL or accelerator calibration**.

Generic `perf stat` counters were available. The recorded whole-process run reports 192,333,081 user cycles, 25,237,905 instructions, 2,105,586 cache references and 284,396 cache misses. These include initialization, warm-up, synchronization and all threads, and are not per-operation coherence counts. Available model-specific events are recorded for future targeted experiments.

## Validation and reproduction

- Slurm job `64792954`: COMPLETED, exit `0:0`, GCC 8.5.0, four allocated CPU cores.
- Standalone CTest: 3/3 cases passed, including 12 focused analytical/cache/overlap/accounting tests.
- Complete local CTest: 10/10 cases passed under AddressSanitizer and UndefinedBehaviorSanitizer; original directory tests remain passing.
- Independent artifact audit: all 126 comparison rows, 378 per-kind rows and phase timing records checked for access partitions, event costs, writeback conservation, link accounting, overlap bounds, and speedup arithmetic.
- CPU probe: eight conditions, 15 timing samples each; generic PMU counters collected.

```bash
cd /gpfs/accounts/eecs570f26s001_class_root/eecs570f26s001_class/nshreyas/project_prof
sbatch perf/run_slurm.sh
```

[Full model contract](perf/README.md) · [Default metrics](results/refined_perf/default.csv) · [Default per-kind events](results/refined_perf/default_kinds.csv) · [Capacity streaming](results/refined_perf/capacity_streaming.csv) · [Slurm log](architecture-perf-v2-64792954.log)
