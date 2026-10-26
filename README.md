# Dynamic Region-Level Coherence for Multi-Agent Radix Prefix Trees

## Latest performance results

For a **hypothetical 50% reduction in KV coherence-event latency and control
traffic**, the refined model reports the following simulated execution times.
Metadata costs stay unchanged; fork barriers, dirty flushes, and expected
recovery costs are included.

| Configuration | Reference (us) | Reduced KV overhead (us) | Execution-time change |
| --- | ---: | ---: | --- |
| 4 KiB prefix fits in 64 KiB local cache | 38.447 | 38.960 | **1.34% slower** |
| 64 KiB prefix exceeds 4 KiB local cache | 427.808 | 406.436 | **5.00% faster** |
| Larger-than-cache workload with a 2 GB/s link | 1443.186 | 1309.666 | **9.25% faster** |

The fitting-cache reference performs **838 off-chip transactions from 8,520
logical accesses**. Local reuse can remove most of the opportunity for coherence
savings, while transition costs can outweigh the remaining benefit.

These are sensitivity estimates under configurable assumptions. The optimized
coherence policy is unspecified. CPU microbenchmark and PMU measurements are
reported separately and do not establish accelerator/CXL event latencies.
Cluster tests and local AddressSanitizer/UndefinedBehaviorSanitizer checks pass.

See [the complete results and limitations](PERFORMANCE_RESULTS.md) and
[the refined model design and commands](perf/README.md).

## Model overview

For the newer **finite-cache execution-time sensitivity model**, see
[perf/README.md](perf/README.md). Build it independently with
`cmake -S perf -B build-perf`; run `build-perf/architecture_perf`. It uses no
optimized coherence policy. It profiles baseline directory traffic with finite
caches, then times hypothetical event-cost reductions with fork costs and
bounded compute/memory overlap. See [PERFORMANCE_RESULTS.md](PERFORMANCE_RESULTS.md)
for the validated sensitivity results. The remainder of this README describes the original
state-machine experiment.

A small, dependency-free C++17 architectural proof of concept: one host and N
accelerators share metadata and KV cache regions. A history predictor switches
each KV region among coherent, private non-coherent, and shared read-only modes.
The same request trace runs under three policies with version-checked data.

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
ctest --test-dir build --output-on-failure
./build/radix_coherence
./build/radix_coherence --stress --csv stress.csv --transitions transitions.csv
./build/radix_coherence --help
```

No external libraries, Python packages, GPUs, MPI, or CXL hardware are needed.
GCC 8.5 and newer C++17 compilers are supported. The model runs in one process.

On Great Lakes, submit from the requested project directory:

```bash
cd /gpfs/accounts/eecs570f26s001_class_root/eecs570f26s001_class/nshreyas/project_prof
sbatch scripts/run_slurm.sh
```

The script requests two CPU cores and 1 GB for five minutes using
`eecs570f26s001_class` on `standard`. It builds, runs CTest, and writes nominal,
stress, transition, workload, and threshold-sweep results under `results/`.
The Slurm job log appears at `radix-coherence-JOBID.log`.
For a larger study, change the resource request and workload sizes.

For sanitizer validation:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
cmake --build build-asan -j 2
ctest --test-dir build-asan --output-on-failure
```

## Code map

| File | Responsibility |
| --- | --- |
| `include/coherence/types.hpp` | Modes, requests, system parameters, cycle costs |
| `include/coherence/metrics.hpp` | Counters and additive cost categories |
| `src/region.cpp` | All permission transitions, flush/recovery logic, line-cache behavior |
| `src/predictor.cpp` | Online history, confidence thresholds, cooldown |
| `src/workload.cpp` | Radix segments, forks/refcounts, interleaved memory trace |
| `src/simulator.cpp` | Policy replay, reference checks, reporting |
| `src/main.cpp` | CLI parameters and output files |
| `tests/tests.cpp` | Protocol, prediction, lifecycle, and adversarial correctness tests |

There is one state machine, one predictor, and a plain vector-based tree; no
class factory, plugin registry, or parallel runtime. Header declarations match
the source modules. Most experiments need edits in only `region.cpp`,
`predictor.cpp`, or `workload.cpp`.

## Workload and three policies

Node 0 is the host; nodes 1 through N are accelerators. The default is four
accelerators: a lead on node 1 and three subagents on nodes 2, 3, and 4. With
fewer accelerators, agents share nodes by round-robin assignment.

Each of four episodes allocates a lead path segment with two 4 KB KV regions.
The lead makes 96 writes per prefix region, then 16 review reads per region.
The host forks three child segments, increments parent references, and inserts
child pointers. Children interleave 128 rounds of shared-prefix reads and
private-leaf writes. Host and accelerator metadata updates contend on the same
64 B lines. Completion decrements references to zero. A `RadixNode` stores
parent/child indices, region IDs, agent placement, and a live reference count.
Each segment stands for a compressed token path; token matching, radix splits,
eviction, and real attention kernels are outside this proof of concept.

1. **Always coherent:** Every region stays in cache-line coherence. An ideal
   directory tracks E/M permission and shared copies. Local hits, including
   repeated owner writes, skip the directory; misses/upgrades look it up.
2. **Dynamic predictor:** Regions start coherent. Request history alone produces
   hints after accesses. A hint changes permissions for later requests. Metadata
   is explicitly pinned coherent by its region descriptor in every policy.
3. **Perfect oracle:** Lifecycle annotations select a valid mode before each
   request, with immediate private entry and exact knowledge of prefix freezing
   and stress accesses. It uses the same state machine and pays every barrier,
   flush, and data fill. It has zero permission mispredictions. This is a
   *perfect lifecycle oracle*, not a globally cost-minimizing lower bound;
   unnecessary short phases can still make it more expensive than baseline.

`--stress` inserts an owner read burst followed by more writes, a host read and
write to a child's private region, and a late write to a frozen prefix. These
events exercise false freezes, remote-private recovery, and read-only recovery.
The oracle labels those accesses in advance; the predictor cannot see labels,
oracle modes, expected data versions, or future requests. Predictor inputs are
individual node/operation/region observations and the current hardware mode.

## State machine

| From | Trigger / target | Required action |
| --- | --- | --- |
| Coherent | Single-owner hint -> Private(owner) | Fence region participants; flush dirty non-owner data; revoke all other cached copies; retain chosen owner's data |
| Coherent | Read-sharing hint -> Shared RO | Fence participants, flush dirty lines, drop exclusive permission, retain clean copies |
| Private | Owner read-phase hint -> Shared RO | Fence owner; flush every dirty line before publishing read-only permission |
| Private | Access by another node -> Coherent | Detect violation; stop/replay request; flush owner; revoke region permissions/copies; serve request coherently |
| Shared RO | Any write -> Coherent | Detect violation; broadcast revocation to every endpoint; discard cached copies; serve write coherently |
| Private / Shared RO | New private owner | Go through Coherent, then acquire Private(new owner) |

Transitions are atomic barriers in this serialized model. All old requests have
completed before a new mode is published. In hardware, draining in-flight
requests and synchronizing TLB/mode-table permissions would implement that
barrier. A local region-permission check detects every forbidden access before
it uses cached data. The check itself emits no interconnect event.

The baseline's cache model permits one E/M owner or multiple clean readers per
line. A read miss to an E/M line snoops and downgrades its owner; a dirty owner
writes back. A write upgrade/miss invalidates all other line copies. A dirty
invalidation response includes its writeback without a second snoop count.

Private owner accesses and shared-RO reads generate **zero steady-state
coherence events**. They still have data fills, local hit costs, and eventual
writebacks. Entering/leaving these modes costs control events and possibly
dirty flushes. Shared RO is modeled as permission to read clean backing memory,
not a physical multicast of each data request.

## Predictor

Per region, the predictor records consecutive same-node accesses, consecutive
reads, readers in the current read run, accessing-node changes, phase lengths,
and recovery cooldown. Histories use run lengths and a 64-bit reader set, not
an unbounded request log. There is one ideal predictor entry per region.
Accessing-node changes are an ownership/contention proxy, not line-directory
ownership transfers.

Default rules, in priority order:

- After a violation: clear confidence and suppress hints for 16 subsequent
  accesses **to that region**. Requests during cooldown update history.
- Metadata always remains coherent.
- Private: freeze after 16 consecutive owner reads, even if no second reader
  has appeared. This is a prediction and may be wrong.
- Coherent: after 8 consecutive reads with at least two distinct readers,
  enter shared RO.
- Otherwise, after 8 consecutive requests from one node, enter private mode
  for that node. Both read and write requests contribute to this confidence.

Phase lengths and accessing-node changes are measured for easy future policy
extensions; the current decision rules use run lengths, reader identity, and
cooldown. A private region with no review phase first recovers to coherent on
its first remote reader, then learns sharing.

Useful experiments:

```bash
# Sharing learned after the first private violation rather than a lead review.
./build/radix_coherence --review-reads 0 --stress

# Higher private threshold trades startup directory work for fewer false hints.
./build/radix_coherence --stress --private-threshold 32

# Smaller regions and more leaf allocation.
./build/radix_coherence --region-bytes 1024 --leaf-regions 4

# Request and transition traces for hand inspection.
./build/radix_coherence --episodes 1 --dump-workload requests.csv \
    --transitions transitions.csv --csv metrics.csv

# Small, reproducible parameter study, including a short-phase counterexample.
bash scripts/sweep.sh
```

## Metrics and cost model

The table and CSV separate directory lookups, downgrade snoops, line
invalidations, region controls, data fills, dirty writebacks, transitions,
recoveries, per-mode accesses, a transition matrix, and verified read checksums.

`coherence_events = directory_lookups + snoops + invalidations + region_controls`.
A directory lookup is a logical transaction; a snoop or line invalidation is
one target operation. A transition charges one broadcast mode-commit event plus
one logical fence/ack exchange per participant. Coherent regions derive their
participants from the line directory; private regions fence their owner.
**Leaving shared RO fences every system endpoint**, including endpoints with
no cached copy: RO reads never register directory sharers, so revocation cannot
assume a freely maintained reader set. Each exchange counts once. Region
revocations are a diagnostic subset of those controls, counted once per endpoint
per transition; they are not added to total events. Region-wide revocations do
not also charge line invalidations. These are logical event counts, not equal-sized messages
or measured CXL packets. Dirty writebacks and data fills each transfer one
64 B line and are reported separately from coherence events.

The default illustrative costs are:

| Event | Cycles | CLI override |
| --- | ---: | --- |
| Local cache hit | 2 | `--hit-cost` |
| Data fill | 80 | `--fill-cost` |
| Directory lookup | 24 | `--directory-cost` |
| Owner downgrade snoop | 32 | `--snoop-cost` |
| Line invalidation per target | 24 | `--invalidate-cost` |
| Dirty line writeback | 80 | `--writeback-cost` |
| Logical region control | 40 | `--control-cost` |
| Violation detection/replay | 160 | `--recovery-cost` |

`total_cycles = data_cycles + coherence_cycles + transition_cycles + recovery_cycles`.
Data cycles include hits, fills and ordinary coherent writebacks. Transition
cycles include region controls and transition flushes. Coherence cycles charge
line-directory/snoop/invalidation work. Recovery cycles charge detection/replay.
`misprediction_penalty_cycles` is detection/replay plus the recovery-triggered
transition cost, already included in the total. It excludes the subsequent
ordinary coherent access and later relearning. Do not add it to total again.
All costs are configurable assumptions, not measurements from Great Lakes.

## Correctness and scope

The generator maintains an independent per-line reference version. Every
simulated write advances a version; every read and write is checked against the
reference trace under all three policies. Per-region invariants also enforce
single dirty ownership, private exclusivity, clean shared RO, no stale valid
copies, and no lost dirty data. Read checksums must agree. Invariants are enabled
by default; `SystemConfig::check_invariants` can disable their extra checking.

Tests cover cache hits, clean/dirty owner downgrades, upgrades/invalidation,
both private violations, shared-RO writes, dirty flushes, ownership changes,
metadata pinning, cooldown, lack of oracle information in the predictor, bad
reference versions, complete lifecycles, and 10,000 deterministic adversarial
requests. CTest also runs one-accelerator, no-review, small-region and invalid
configuration cases. Test assertions remain active in Release builds.

The system is an abstract CXL-like shared-memory model, not an implementation
of CXL, MESI packet routing, SGLang, or RadixAttention. It has ideal unbounded
caches, an ideal per-region mode table/predictor, no replacement, no false
sharing below the chosen 64 B access unit, no speculative loads, no bandwidth
queueing, and no real CPU/GPU execution. Agent concurrency is request
interleaving, with summed serialized service/stall costs; those costs are not
wall-clock latency, throughput, or a speedup prediction. KV writebacks on final
completion are not forced: all policies end with whatever dirty cache data
their protocols legitimately retain. The same completion convention applies
to every policy. Resource reuse and tree reclamation are not simulated.
The predictor has ideal access to the serialized request observations, including
cache hits; the cost of collecting/distributing that history is not modeled.
A physical design must specify predictor placement and whether it trains only
on requests visible at a home controller or uses local summaries.

The experimental region policies are motivated by prefix reuse in
[SGLang's RadixAttention paper](https://arxiv.org/abs/2312.07104) and
[SGLang's KV cache design documentation](https://docs.sglang.io/docs/advanced_features/hicache_design).
The coherence mechanisms and cycle assumptions here are proposed model
choices; those sources do not validate the proposed hardware policy.
