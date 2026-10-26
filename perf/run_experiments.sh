#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
simulator="${1:-$project_dir/build-perf/architecture_perf}"
mkdir -p results/refined_perf
run_case() {
    local case_name="$1"
    shift
    "$simulator" "$@" --csv "results/refined_perf/${case_name}.csv" \
        --kinds "results/refined_perf/${case_name}_kinds.csv" \
        --phases "results/refined_perf/${case_name}_phases.csv" > "results/refined_perf/${case_name}.txt"
}
run_case default
run_case no_overlap --overlap-trigger 1
run_case eager_overlap --overlap-trigger 0
run_case no_candidate_costs --fork-barrier-ns 0 --region-barrier-ns 0 --flush-at-fork 0 --recovery-rate 0
run_case candidate_check_cost --added-latency-ns 2
run_case recovery_stress --recovery-rate 0.1 --recovery-ns 1000
run_case transition_stress --fork-barrier-ns 5000 --region-barrier-ns 400
run_case metadata_only --prefix-reads 0 --leaf-writes 0
run_case zero_event_costs --directory-ns 0 --snoop-ns 0 --invalidate-ns 0 \
    --directory-bytes 0 --snoop-bytes 0 --invalidate-bytes 0
streaming=(--cache-bytes 4096 --prefix-lines 1024 --leaf-lines 1024 --lead-steps 512 --prefix-reads 128)
run_case capacity_streaming "${streaming[@]}"
run_case streaming_no_overlap "${streaming[@]}" --overlap-trigger 1
run_case bandwidth_limited "${streaming[@]}" --bandwidth-gbps 2
run_case compute_heavy "${streaming[@]}" --compute-ns 10000
for capacity in 0 4096 16384 65536; do run_case "cache_${capacity}" --cache-bytes "$capacity"; done
run_case one_store_per_line --stores-per-line 1
printf '%s\n' 'Wrote 18 configurations x 7 scenarios under results/refined_perf'
