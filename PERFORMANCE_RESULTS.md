# General performance simulation results

This simulator estimates execution-time headroom from reducing an assumed coherence overhead budget. It implements no coherence protocol, policy, predictor, or state transitions.

## Result

With the default assumptions, halving KV-related latency and control overhead reduces simulated execution time from **107.013 us to 99.779 us**: **6.76% less time**, or **1.072x speedup**. Metadata costs stay fixed. This motivates investigating coherence overhead; it does not show that a particular design achieves that reduction.

## Default model

- One host and three accelerators; lead, fork/parallel children, completion phases.
- 200 ns accelerator compute per step; 150 ns memory latency; eight outstanding requests per agent.
- Shared aggregate interconnect: 32 GB/s. Each off-chip transaction transfers 64 payload bytes.
- Assumed reference coherence budget: 40 ns plus 32 control bytes per off-chip transaction.
- Identical 8,520 transactions and 545,280 payload bytes in each default comparison.
- Requests represent traffic remaining after any cache hits; no cache/coherence behavior is inferred.

| Assumption | Execution (us) | Speedup | Average queue (ns) | Control KiB |
| --- | ---: | ---: | ---: | ---: |
| Reference overhead | 107.013 | 1.000x | 2.422 | 266.250 |
| 25% less KV overhead | 103.401 | 1.035x | 2.206 | 204.250 |
| 50% less KV overhead | 99.779 | 1.072x | 1.991 | 142.250 |
| 75% less KV overhead | 96.181 | 1.113x | 1.838 | 80.250 |
| Zero KV overhead | 92.558 | 1.156x | 1.641 | 18.250 |
| 50% less KV latency only | 99.967 | 1.070x | 2.366 | 266.250 |
| 50% less KV traffic only | 106.832 | 1.002x | 2.028 | 142.250 |

The default link is lightly utilized. Halving latency alone captures most of the improvement; halving control bytes alone changes execution time very little. Removing all KV overhead leaves metadata, compute, payload transfer, and memory latency in place.

## Sensitivity to bottlenecks

The following compares the reference against 50% lower KV latency/control overhead in each configuration.

| Configuration | Reference (us) | Half KV overhead (us) | Time reduction | Speedup |
| --- | ---: | ---: | ---: | ---: |
| Default | 107.013 | 99.779 | 6.76% | 1.072x |
| 10,000 ns compute per step | 1047.792 | 1040.566 | 0.69% | 1.007x |
| 2 GB/s link | 434.154 | 370.666 | 14.62% | 1.171x |
| 64 outstanding requests per agent | 47.706 | 46.258 | 3.04% | 1.031x |
| No reference coherence cost | 88.352 | 88.352 | 0.00% | 1.000x |
| Metadata-only traffic | 39.018 | 39.018 | 0.00% | 1.000x |
| 80 ns added cost per KV transaction | 107.013 | 127.926 | -19.54% | 0.837x |

At 2 GB/s the shared link is saturated: traffic reduction helps, while reducing fixed coherence latency alone gives no execution-time improvement. More compute or memory-level parallelism reduces exposed latency headroom. Zero-cost and metadata-only checks give exactly zero speedup. The added-cost case regresses, so the simulator does not force a favorable result.

## Validation and files

Great Lakes job `64779218` completed with exit code `0:0` on `gl3033.arc-ts.umich.edu` using GCC 8.5.0. All 3 standalone CTest cases passed, including 9 focused tests with hand-derived serial, parallel, and memory-level-parallelism timings. The complete local project passed 10/10 CTest cases with AddressSanitizer and UndefinedBehaviorSanitizer.

All 77 comparison rows across 11 configurations were independently audited for request counts, unchanged payload traffic, retained metadata overhead, link accounting, speedup arithmetic, and phase dependencies.

- [Model documentation and commands](perf/README.md)
- [Default metrics](results/perf/default.csv) and [phase timings](results/perf/default_phases.csv)
- [Bandwidth-limited metrics](results/perf/bandwidth_limited.csv)
- [Completed Slurm log](architecture-perf-64779218.log)

## Rerun on Great Lakes

```bash
cd /gpfs/accounts/eecs570f26s001_class_root/eecs570f26s001_class/nshreyas/project_prof
sbatch perf/run_slurm.sh
```

For a direct small run after building: `./build-perf/architecture_perf`. The original policy simulator remains a separate executable. All reported times here are simulated makespans from illustrative inputs, not measured Great Lakes/CXL/GPU application runtimes.
