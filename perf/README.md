# Finite-cache performance sensitivity model (v2)

This revision replaces the original flat `40 ns / 32 B` tax on every request.
It estimates traffic with the existing **baseline** line-directory engine,
adds finite per-node caches, and times the resulting services with bounded
compute/memory overlap. Hypothetical reductions apply to KV coherence events;
no optimized coherence policy or predictor is implemented.

## Build and run

From the project root:

```bash
cmake -S perf -B build-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j 4
ctest --test-dir build-perf --output-on-failure
./build-perf/architecture_perf
./build-perf/architecture_perf --help
```

On Great Lakes, submit from
`/gpfs/accounts/eecs570f26s001_class_root/eecs570f26s001_class/nshreyas/project_prof`:

```bash
sbatch perf/run_slurm.sh
```

The job builds and tests the model, runs 18 configurations with seven overhead
assumptions each, and collects a four-core CPU probe. New results are under
`results/refined_perf/`; earlier `results/perf/` files retain the v1 results.
The root `PERFORMANCE_RESULTS.md` describes the latest run.

## What changed

| Earlier simplifying assumption | Revised treatment |
| --- | --- |
| Every generated access goes off chip | Logical accesses use a finite, fully associative, per-node LRU cache |
| Every access has the same coherence tax | Directory lookups, owner downgrades, invalidations, and writebacks are counted separately by access kind |
| Private writes repeatedly pay a tax | A first write acquires permission; subsequent stores to a resident exclusive line hit locally |
| A prefix is fetched on every round | Resident clean prefix copies are reused locally; working-set/cache-size sweeps expose capacity misses |
| Reductions have no fork cost | Candidates pay a fork barrier, a per-prefix-region cost, and explicit resident dirty-prefix writebacks |
| Compute waits for the full batch | One next-step compute stage can overlap the current batch's tail, with a configurable dependency threshold |
| No recovery budget | Candidates pay a configurable expected recovery latency/traffic budget per KV permission transaction |

## Traffic profiling and cache semantics

`profile.cpp` reuses `RegionStateMachine` from `src/region.cpp`, always in
`Policy::Baseline`. It never calls dynamic hints or the sharing predictor.
The baseline engine has explicit single-writer permissions and clean sharers;
finite LRU capacity is implemented by calling its new `evict()` hook. Dirty
evictions update backing data before discarding copies. Stores use write allocation.
Invalidation removes
stale entries from the affected node's LRU accounting before its next access.
A clean eviction sends an assumed 8-byte directory notice and costs 4 ns;
dirty eviction additionally transfers a 64-byte writeback. These small notice
costs are currently constants in `model.cpp`.

On a read miss, an exclusive remote owner is downgraded, and a dirty owner
writes back. Later clean readers require directory registration, without another
owner snoop. A write upgrade/miss invalidates other copies. Local shared reads
and exclusive writes have no directory lookup. Data hits that still need a
write-permission upgrade retain their coherence costs. Repeated accesses to the
same line within an agent wait for the previous access/fill to complete in the
timing model.

The counters are exact for this abstract reference engine and its chosen
logical trace, **not hardware ground truth or a complete MESI implementation**.
The profile uses fixed step/access round-robin interleaving across streams.
Timing changes do not feed back to reorder that cache/directory trace. This
avoids attributing different work to the candidate, but misses timing-dependent
replacement and contention effects. Replacement, cache capacity, data versions,
and dirty ownership are checked functionally in the profiling pass; the timing
pass is not a full coherence/network simulator.

## Workload

There are lead, parallel-children, and completion phases. The lead uses
accelerator 1; children use accelerators 1..N, so the first child retains the
lead's local prefix when it remains resident. The host is node 0.

Defaults:

- Host plus three accelerators; 32 lead steps and 64 child rounds.
- Each child step: 32 prefix reads, eight leaf stores, two metadata accesses.
- Each lead step: eight prefix stores and two metadata accesses.
- Host metadata work runs alongside children, followed by completion bookkeeping.
- Prefix working set: 64 lines (4 KiB); leaf working set per child: 128 lines
  (8 KiB); local cache per node: 64 KiB.
- Four logical stores update each 64-byte KV line before advancing. The lead
  therefore initializes all 64 prefix lines. Leaf addresses advance and wrap
  within their configured working set. Metadata addresses are shared.
- Prefix reads scan across `prefix_lines`, rather than repeatedly naming just
  the first batch. Logical access count remains 8,520 by default.

Defaults intentionally include a fitting-cache case. No claim is made that all
KV traffic misses accelerator L2. `--cache-bytes` is an available cache budget,
not a claim about a particular processor's total L2 size. If a prefix fits and
isn't displaced, subsequent reads hit; the capacity-streaming experiment uses a
64 KiB prefix with only 4 KiB cache per node and a longer lead initialization.

## Operation-sensitive cost model

| Parameter | Default assumption |
| --- | ---: |
| Local data hit | 2 ns (not two hardware cycles) |
| Memory data service | 150 ns |
| Peer-owner data service | 150 ns |
| Directory lookup | 20 ns + 16 control bytes |
| Owner downgrade snoop | 20 ns + 16 control bytes |
| Invalidation per target | 12 ns + 16 control bytes |
| Dirty writeback | 10 ns additional service + 64 payload bytes |
| Link bandwidth | 32 decimal GB/s = 32 bytes/ns |

All table costs have CLI overrides. These values are **unmeasured assumptions**.
There is no single cost per PrefixKV or LeafKV request: the profile's event
counts determine its cost. The `--kinds` CSV separates metadata, prefix, and leaf
accesses, hits, fills, directory lookups, snoops, invalidations, writebacks, and
evictions. Eviction/writeback costs are attributed to the access triggering
that work, which may differ from the victim line's category.

The seven comparisons retain 100/75/50/25/0% of KV directory/snoop/invalidation
latency and control bytes, plus separate latency-only and traffic-only cases.
Metadata event costs, cache residency, data fills, dirty writeback payloads,
and eviction notices are preserved. Reducing events does not establish protocol
safety; the fractions remain hypothetical achievable savings.

For an access with profiled traffic:

```text
earliest = max(issue time, previous same-node/same-line completion)
link_end = max(earliest, link_available) + total transferred bytes / bandwidth
finish = link_end + data-service-or-hit latency
       + retained directory/snoop/invalidation latency
       + writeback/eviction cost + candidate check/recovery budget
```

A local access with no network traffic skips the link entirely. Memory service
is pipelined; there is no separate finite-bandwidth DRAM-controller model.

## Fork, flush, and recovery costs

At the lead-to-children boundary, each candidate pays:

- `--setup-ns` (default 0) plus `--fork-barrier-ns` (200 ns).
- `--region-barrier-ns` (40 ns) per touched prefix region (4 KiB by default).
- A control budget of one directory-sized exchange per endpoint and region.
- With `--flush-at-fork 1`, all **resident dirty prefix lines** are written back,
  using the same link and a bounded number of concurrent flushes.

The functional profiler cleans those lines at the fork. Consequently they do
not write back again at the first child read: this moves a required writeback
rather than counting it twice. The reference may instead serve that dirty data
on demand. The `fork_overhead` phase makes the critical-path penalty explicit.
Region transitions for later allocations, permission-table replacement, and
arbitrary protocol recovery sequences are not implemented.

The default expected recovery rate is 1% of KV accesses needing directory
permission. Each expected recovery adds 200 ns plus 32 bytes. This is a user
budget, not a predictor measurement or an assertion about an actual error rate.
`--added-latency-ns` adds a candidate check cost to every logical KV access,
including local hits. These knobs support honest break-even studies.

## Within-agent overlap

`--overlap-trigger 0.5` starts the next step's compute after half the current
batch's accesses finish, assuming that compute is independent of the remaining
requests. At most one next compute stage is buffered. Its memory batch cannot
start until the preceding batch finishes, and the outstanding-access window
remains bounded (`--outstanding 8` by default). The host retains sequential
compute/batch dependencies.

Trigger 1 disables within-agent overlap; trigger 0 permits eager independent
next-step compute. This is a dependency assumption to sweep, not a measured
property of attention kernels. Increasing outstanding requests alone is a
different experiment. Aggregate compute, memory-wait, and overlap durations
must not be added as independent components of elapsed time. Execution time is
the last completion of dependent phases, including candidate fork penalties.

## CPU measurements and limits

`cpu_probe.cpp` is built on Linux x86-64 and needs four allocated CPUs. It pins
one producer and three consumers to CPUs from the job's affinity mask, and
reports pinning success, per-reader/per-repetition dependent-walk time, and a
checksum. A shuffled 64-byte linked cycle limits memory-level parallelism and
reduces simple streaming prefetch effects. Two working sets (64 KiB and 4 MiB)
are tested for:

- `warm`: an unflushed consumer warm-up pass before timing.
- `cold`: producer flushes all lines with `CLFLUSH` and an ordering fence.
- `handoff`: producer writes, then consumers walk without flushing.
- `private`: each consumer updates its own warmed private buffer.

These are loop-inclusive CPU measurements, not isolated event latencies. An
unflushed 4 MiB working set may exceed local cache; producer handoff does not
ensure every timed line stays modified until it is read. Three simultaneous
readers also interfere. Differences between these cases must not be copied
into a directory/snoop/CXL/GPU latency knob as if they were a direct calibration.
A flushed line has already written back dirty data, so flushed reads do not
measure a dirty-owner handoff.

`run_probe.sh` records CPU/cache topology, affinity, kernel PMU restrictions,
and available `perf` events. It attempts generic counters only when available;
permission failures are retained without changing kernel settings. Whole-process
counters include initialization and synchronization. Model-specific offcore
names must be selected for the actual CPU, following
[Intel's PMU event documentation](https://perfmon-events.intel.com/).
[The Intel manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
provide cache-maintenance and ordering semantics. CPU measurements are not
accelerator/CXL measurements and are not automatically imported as parameters.

## Validation and interpretation

Twelve focused tests cover analytic serial/parallel timings, finite capacity,
dirty eviction, private store reuse, first/second reader asymmetry, fork placement
and no duplicate writebacks, bounded compute overlap, in-flight fill dependencies,
fitting-prefix reuse, accounting, zero-headroom cases, and candidate regressions.
The original directory tests also remain available in the complete root build.

The old 6.76% and 14.62% figures describe v1 assumptions. They are not promised
speedups, and are not a formally established upper bound across hardware or all
workloads. This refinement can produce gains, zero benefit, or losses. Real
calibration still requires representative logical traces, cache budgets,
dependency information, and measured event costs on the target architecture.
