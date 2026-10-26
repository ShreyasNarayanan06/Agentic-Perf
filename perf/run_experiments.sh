#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
simulator="${1:-$project_dir/build-perf/architecture_perf}"
mkdir -p results/perf
run_case() {
    case_name="$1"
    shift
    "$simulator" "$@" --csv "results/perf/${case_name}.csv" \
        --phases "results/perf/${case_name}_phases.csv" > "results/perf/${case_name}.txt"
}
run_case default
run_case compute_heavy --compute-ns 10000
run_case bandwidth_limited --bandwidth-gbps 2
run_case high_parallelism --outstanding 64
run_case zero_coherence --coherence-ns 0 --coherence-bytes 0
run_case metadata_only --prefix-reads 0 --leaf-writes 0
run_case added_cost --added-latency-ns 80
for reads in 0 8 32 64; do run_case "prefix_reads_${reads}" --prefix-reads "$reads"; done
echo "Wrote 11 configurations x 7 overhead assumptions, with phase timings, under results/perf"
