#!/usr/bin/env bash
#SBATCH --job-name=architecture-perf
#SBATCH --account=eecs570f26s001_class
#SBATCH --partition=standard
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=1G
#SBATCH --time=00:05:00
#SBATCH --output=architecture-perf-%j.log
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
cat results/perf/default.txt
echo "Performance sensitivity experiments complete: $project_dir/results/perf"
