#!/usr/bin/env bash
#SBATCH --job-name=architecture-perf-v2
#SBATCH --account=eecs570f26s001_class
#SBATCH --partition=standard
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --hint=nomultithread
#SBATCH --mem=2G
#SBATCH --time=00:10:00
#SBATCH --output=architecture-perf-v2-%j.log
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${SLURM_SUBMIT_DIR:-}" ]]; then project_dir="$SLURM_SUBMIT_DIR"; fi
cd "$project_dir"
hostname
g++ --version
cmake -S perf -B build-perf -DCMAKE_BUILD_TYPE=Release
cmake --build build-perf -j "${SLURM_CPUS_PER_TASK:-2}"
ctest --test-dir build-perf --output-on-failure
bash perf/run_experiments.sh
bash perf/run_probe.sh
cat results/refined_perf/default.txt
cat results/refined_perf/capacity_streaming.txt
cat results/refined_perf/hardware/status.txt
