# Experiment and validation results

Completed on Great Lakes on October 25, 2026 (America/Detroit).

- Project: `/gpfs/accounts/eecs570f26s001_class_root/eecs570f26s001_class/nshreyas/project_prof`
- Final Slurm job: `64778344`, state `COMPLETED`, exit code `0:0`.
- Compute node: `gl3296.arc-ts.umich.edu`; GCC 8.5.0; CMake Release build.
- Remote validation: 7/7 CTest cases, including 14/14 focused protocol/predictor tests.
- Local validation: the same 7/7 CTest cases under AddressSanitizer and UndefinedBehaviorSanitizer.
- A separate artifact audit checked CSV accounting, transition costs/flush counts, and reference traces.

## Nominal lifecycle

Default configuration: host + 4 accelerators, 4 KB regions, 64 B lines, four episodes, one lead and three subagents per episode, two prefix regions and one leaf region per child. Each lead writes 96 times per prefix region, reviews it 16 times, and forks. Children interleave 128 rounds.

| Metric | Always coherent | Dynamic | Perfect lifecycle oracle |
| --- | ---: | ---: | ---: |
| Requests | 6,252 | 6,252 | 6,252 |
| Directory lookups | 3,349 | 693 | 533 |
| Downgrade snoops | 572 | 60 | 60 |
| Line invalidations | 460 | 460 | 460 |
| Region control events | 0 | 56 | 36 |
| Total coherence events | 4,381 | 1,269 | 1,089 |
| Data fills (64 B lines) | 3,293 | 3,293 | 3,293 |
| Dirty writebacks (64 B lines) | 960 | 960 | 960 |
| Transition flushes (subset) | 0 | 512 | 512 |
| State transitions | 0 | 28 | 28 |
| Permission recoveries | 0 | 0 | 0 |
| Misprediction penalty cycles (subset) | 0 | 0 | 0 |
| Estimated service cycles | 455,878 | 377,990 | 373,350 |

Dynamic mode reduces total logical coherence events by **71.03%**, including region controls, and reduces modeled serialized service cycles by **17.09%**. There are no permission mispredictions in this lifecycle. Every access matches the reference version; all policies produce checksum `18320345134203162197`.

## Stress lifecycle

The same base workload adds temporary owner read bursts followed by writes, remote host access to a private leaf, and writes to frozen prefix regions.

| Metric | Always coherent | Dynamic | Perfect lifecycle oracle |
| --- | ---: | ---: | ---: |
| Requests | 6,392 | 6,392 | 6,392 |
| Directory lookups | 3,377 | 977 | 541 |
| Downgrade snoops | 580 | 84 | 60 |
| Line invalidations | 480 | 460 | 460 |
| Region control events | 0 | 200 | 84 |
| Total coherence events | 4,437 | 1,721 | 1,145 |
| Data fills (64 B lines) | 3,313 | 4,497 | 4,241 |
| Dirty writebacks (64 B lines) | 972 | 1,396 | 1,140 |
| Transition flushes (subset) | 0 | 944 | 692 |
| State transitions | 0 | 68 | 44 |
| Permission recoveries | 0 | 16 | 0 |
| Misprediction penalty cycles (subset) | 0 | 19,520 | 0 |
| Estimated service cycles | 460,086 | 522,966 | 464,086 |

Dynamic mode reduces coherence events by **61.21%**, but increases modeled serialized service cycles by **13.67%**. Its 16 recoveries comprise four remote-private accesses and 12 writes to shared RO. Conservative region revocation discards cached data and causes extra fills and dirty writebacks. Lower coherence traffic does not guarantee a lower overall cost. The oracle also pays transition costs; exact lifecycle knowledge is not a guarantee of globally optimal mode selection.

All policies produce the verified stress checksum `18064092195983666841`.

## Parameter sweeps

These are deterministic stress runs, holding all other defaults fixed. All CSVs contain all three policies; the table below shows the dynamic policy.

| Parameter | Threshold | Coherence events | Recoveries | Service cycles |
| --- | ---: | ---: | ---: | ---: |
| private | 1 | 1,581 | 16 | 519,606 |
| private | 4 | 1,641 | 16 | 521,046 |
| private | 8 | 1,721 | 16 | 522,966 |
| private | 16 | 1,881 | 16 | 526,806 |
| private | 32 | 2,385 | 16 | 538,838 |
| private | 64 | 2,897 | 12 | 523,654 |
| freeze | 4 | 1,721 | 16 | 522,966 |
| freeze | 8 | 1,721 | 16 | 522,966 |
| freeze | 16 | 1,721 | 16 | 522,966 |
| freeze | 32 | 1,721 | 16 | 482,134 |
| freeze | 64 | 1,721 | 16 | 482,134 |

The additional short-phase experiment uses one episode, eight lead writes, two review reads, eight child rounds, private/freeze thresholds of one, and no cooldown. Dynamic service cost is 18,232 cycles versus 14,738 for baseline. Frequent mode changes can erase the benefit of bypassing line coherence.

## Inspect and rerun

- [Model, parameters, assumptions, and build instructions](README.md)
- [Completed Slurm build/test log](radix-coherence-64778344.log)
- [Nominal metrics](results/nominal.csv) and [stress metrics](results/stress.csv)
- [Nominal transition trace](results/nominal_transitions.csv) and [stress transition trace](results/stress_transitions.csv)
- [Nominal request trace](results/nominal_workload.csv) and [stress request trace](results/stress_workload.csv)

```bash
sbatch scripts/run_slurm.sh
```

The model uses ideal unbounded caches and predictor entries, atomic transitions, and illustrative configurable cycle costs. It is not cycle-accurate, a CXL protocol implementation, or a measurement of application throughput. Its oracle is perfect with respect to lifecycle annotations, not a globally cost-optimal lower bound. See README.md for the full model contract.
