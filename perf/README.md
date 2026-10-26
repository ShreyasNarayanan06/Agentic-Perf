# Policy-neutral performance simulator

This independent C++17 discrete-event model asks: **how much execution-time
headroom is available if a future coherence mechanism reduces KV-related
latency and control traffic?** It implements no coherence policy, state machine,
predictor, directory, ownership protocol, or cache hierarchy. The executable
does not link to the project's original coherence simulator.

## Build and run

From the Great Lakes project root:

```bash
cmake -S perf -B build-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j 2
ctest --test-dir build-perf --output-on-failure
./build-perf/architecture_perf
./build-perf/architecture_perf --help
```

For a scheduled compute-node run, submit from
`/gpfs/accounts/eecs570f26s001_class_root/eecs570f26s001_class/nshreyas/project_prof`:

```bash
sbatch perf/run_slurm.sh
```

Results go under `results/perf/`. The script runs 11 workload/system
configurations, each with seven overhead assumptions. Build and execution need
only a C++17 compiler and CMake. `perf/model.cpp` contains the workload and event
loop; `model.hpp` contains plain data structures; `main.cpp` handles parameters
and reporting; `tests.cpp` checks independent analytical cases.

## Workload and timing

The workload has three sequential phases:

1. A lead accelerator appends a prefix for 32 steps.
2. At a fork barrier, three child accelerators begin 64 rounds of reading that
   shared prefix and appending separate leaves. Host metadata work runs at the
   same time as the children, on a separate stream.
3. Host completion bookkeeping starts after all children finish.

Each child step computes for 200 ns, then issues a batch of 32 prefix reads,
eight leaf writes, and two metadata transactions, with at most eight memory
requests outstanding per stream. The lead writes eight prefix lines and issues
two metadata transactions per step. The host computes 50 ns per step and issues
two metadata transactions. These counts, compute durations, and concurrency are
configurable. Each accelerator has one stream; the lead uses node 1 before the
fork, and children use nodes 1..N afterward. The host is node 0.

All accesses are modeled **off-chip transactions**, rather than all program
loads/stores. Their counts are input assumptions representing traffic remaining
after any cache hits or coalescing. Prefix addresses are common across agents,
leaf addresses are separate, and metadata addresses are common. Addresses and
read/write labels describe the workload; the simulator does not infer coherence
messages, hits, sharers, or dependencies from them. It does not execute KV data
or implement a functional RadixTree.

A single shared, FIFO, aggregate interconnect serializes request data plus a
generic control-traffic budget. The memory service itself is pipelined, with no
additional memory-controller capacity limit. The default memory latency is
150 ns and the default link rate is 32 decimal GB/s, equal to 32 bytes/ns.
Every transaction transfers 64 data bytes and, in the reference case, an assumed
32 control bytes and 40 ns of additional coherence-related latency.

For a request issued at `t`:

```text
link_start = max(t, link_available)
link_end = link_start + (64 + control_bytes) / bandwidth_bytes_per_ns
completion = link_end + memory_latency + coherence_latency + optional_added_latency
```

Completion opens an outstanding-request slot. A stream starts its next compute
step after its current batch finishes. Separate agents' compute and memory
activity overlap; a single agent's compute and memory batch are sequential.
Phase duration is the last completion across its streams, and phases are joined
by barriers. Execution time is the actual simulated makespan, **not a sum of
all agents' memory penalties**. There is no interconnect queueing delay hidden
inside the user-supplied coherence latency; queueing is modeled explicitly.

## Comparisons without a policy

Every comparison replays exactly the same compute steps, off-chip requests,
data bytes, agents, and dependencies. Only two assumed KV overhead multipliers
change. The default sweep retains 100%, 75%, 50%, 25%, and 0% of both overhead
terms, plus two cases that halve latency alone or control traffic alone.
Metadata overhead always remains unchanged.

The reference is an **assumed overhead budget**, not an implemented baseline
coherence protocol. A reduction fraction is a hypothetical input; the simulator
does not establish that any mechanism can achieve it. The zero-KV-overhead case
is optimistic headroom under these assumptions when added/setup costs are zero.
It retains memory latency, all payload traffic, metadata overhead, barriers,
and compute. No protocol safety or data-consistency conclusion follows from it.

`--added-latency-ns` optionally adds latency to every KV request in the
counterfactual cases; `--setup-ns` adds a one-time startup cost to them. Both
default to zero. They let a future mechanism's own costs consume its potential
benefit, without implementing that mechanism. The reference pays neither.

## Useful metrics

- **Simulated execution time and speedup:** critical-path makespan and its ratio
  to the reference.
- **Critical-path time saved:** reference makespan minus scenario makespan,
  rather than the sum of latency reductions across overlapping requests.
- **Average queue wait and link utilization:** whether control-traffic reduction
  relieves the interconnect bottleneck.
- **Payload/control bytes:** verifies that workload data stays fixed while the
  assumed control budget changes.
- **Aggregate worker compute/wait and assumed coherence latency:** attribution
  across workers; overlapping totals are not additive components of elapsed
  execution time.
- **Per-phase timing:** exposes whether lead, parallel agents, or host completion
  dominates execution.

CSV files include all input parameters for reproducibility.

## Sensitivity experiments and checks

```bash
# Export the complete default comparison.
./build-perf/architecture_perf --csv comparison.csv --phases phases.csv

# More compute reduces the relative value of coherence optimization.
./build-perf/architecture_perf --compute-ns 10000

# Constrained bandwidth makes control bytes more expensive.
./build-perf/architecture_perf --bandwidth-gbps 2

# More outstanding requests hide more fixed latency.
./build-perf/architecture_perf --outstanding 64

# With no initial coherence cost, reduction fractions give no speedup.
./build-perf/architecture_perf --coherence-ns 0 --coherence-bytes 0

# Metadata costs are preserved, so this comparison also gives no speedup.
./build-perf/architecture_perf --prefix-reads 0 --leaf-writes 0

# A sufficiently expensive future mechanism can lose performance.
./build-perf/architecture_perf --added-latency-ns 80

# Reproduce all 11 configurations.
bash perf/run_experiments.sh
```

The nine focused tests include hand-calculated serial and concurrent timings,
memory-level parallelism, phase dependencies, unchanged payload and metadata
costs, zero-benefit cases, compute-only timing, added-cost regressions, and
validation errors. Configure with `-DENABLE_SANITIZERS=ON` for ASan/UBSan.

All numerical costs and transaction rates are illustrative assumptions. This
is an execution-time sensitivity study, not a benchmark of Great Lakes, CXL,
GPU kernels, or an actual coherence implementation. To argue for a particular
design, first calibrate traffic and costs using measurements, then show that
the design achieves the assumed reductions within the added-cost budget.
