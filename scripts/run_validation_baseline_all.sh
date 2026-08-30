#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_project_root="$(cd "$script_dir/.." && pwd)"

COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$default_project_root}"
wsl_dataset_root="/mnt/e/Python Projects/PyPACE/data/generated-pddl/depots-numeric-validation-original"
autodl_dataset_root="/root/PyPACE/data/generated-pddl/depots-numeric-validation-original"
if [[ -n "${COUNT_DATASET_ROOT:-}" ]]; then
    dataset_root="$COUNT_DATASET_ROOT"
elif [[ -d "$wsl_dataset_root" ]]; then
    dataset_root="$wsl_dataset_root"
else
    dataset_root="$autodl_dataset_root"
fi

COUNT_RUN_TAG="${COUNT_RUN_TAG:-validation-all-off}"
if [[ "$dataset_root" == "$wsl_dataset_root" ]]; then
    default_results_dir="$COUNT_PROJECT_ROOT/experiments/validation_baseline/results/$COUNT_RUN_TAG"
else
    default_results_dir="/root/autodl-tmp/count-results/depots-numeric-validation-original/baseline/$COUNT_RUN_TAG"
fi
COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-$default_results_dir}"

COUNT_BASELINE_PARALLELISM="${COUNT_BASELINE_PARALLELISM:-2}"
COUNT_SMALL_TIME_LIMIT="${COUNT_SMALL_TIME_LIMIT:-1800}"
COUNT_LARGE_TIME_LIMIT="${COUNT_LARGE_TIME_LIMIT:-3600}"

for command in python3 find sort; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing required command: $command" >&2
        exit 1
    fi
done

for path in \
    "$COUNT_PROJECT_ROOT/scripts/run_batch_linux.sh" \
    "$COUNT_PROJECT_ROOT/fast-downward.py" \
    "$COUNT_PROJECT_ROOT/builds/release64/bin/downward" \
    "$dataset_root/domain.pddl" \
    "$dataset_root/problems"; do
    if [[ ! -e "$path" ]]; then
        echo "Missing required path: $path" >&2
        exit 1
    fi
done

selected_problems=()
for scale in 10 20 30 40; do
    mapfile -t candidates < <(
        find "$dataset_root/problems" \
            -maxdepth 1 -type f \
            -name "problem_scale_${scale}_id_*.pddl" \
            -print | sort -V
    )
    if (( ${#candidates[@]} == 0 )); then
        echo "No validation problems found for scale $scale" >&2
        exit 1
    fi
    echo "[COUNT-VALIDATION-BASELINE] scale=$scale problems=${#candidates[@]}"
    selected_problems+=("${candidates[@]}")
done

mkdir -p "$COUNT_RESULTS_DIR"

echo "[COUNT-VALIDATION-BASELINE] project=$COUNT_PROJECT_ROOT"
echo "[COUNT-VALIDATION-BASELINE] dataset=$dataset_root"
echo "[COUNT-VALIDATION-BASELINE] results=$COUNT_RESULTS_DIR"
echo "[COUNT-VALIDATION-BASELINE] jobs=${#selected_problems[@]} mode=off resume=on"
echo "[COUNT-VALIDATION-BASELINE] small_parallelism=$COUNT_BASELINE_PARALLELISM limits=${COUNT_SMALL_TIME_LIMIT}s/${COUNT_LARGE_TIME_LIMIT}s"
echo "[COUNT-VALIDATION-BASELINE] LLM trigger, bridge, prompt construction and state emission are forced off"

batch_arguments=(
    "$dataset_root/domain.pddl"
    "${selected_problems[@]}"
    --default-mode off
    --output-dir "$COUNT_RESULTS_DIR"
    --resume
    --small-parallelism "$COUNT_BASELINE_PARALLELISM"
    --small-time-limit "$COUNT_SMALL_TIME_LIMIT"
    --large-time-limit "$COUNT_LARGE_TIME_LIMIT"
    --small-max-requests 0
    --large-max-requests 0
)

cd "$COUNT_PROJECT_ROOT"
exec bash scripts/run_batch_linux.sh "${batch_arguments[@]}" "$@"
