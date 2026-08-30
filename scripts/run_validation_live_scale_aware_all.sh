#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_project_root="$(cd "$script_dir/.." && pwd)"

COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$default_project_root}"
COUNT_DATASET_ROOT="${COUNT_DATASET_ROOT:-/root/PyPACE/data/generated-pddl/depots-numeric-validation-original}"
COUNT_MODEL_PATH="${COUNT_MODEL_PATH:-/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface}"
COUNT_RUN_TAG="${COUNT_RUN_TAG:-validation-all-live-scale-aware-v1}"
COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/$COUNT_RUN_TAG}"

COUNT_SMALL_PARALLELISM="${COUNT_SMALL_PARALLELISM:-2}"
COUNT_SMALL_TIME_LIMIT="${COUNT_SMALL_TIME_LIMIT:-1800}"
COUNT_LARGE_TIME_LIMIT="${COUNT_LARGE_TIME_LIMIT:-3600}"
COUNT_SMALL_MAX_REQUESTS="${COUNT_SMALL_MAX_REQUESTS:-10}"
COUNT_LARGE_MAX_REQUESTS="${COUNT_LARGE_MAX_REQUESTS:-15}"
COUNT_SCALE_30_EXPANSION_MULTIPLIER="${COUNT_SCALE_30_EXPANSION_MULTIPLIER:-0.5}"
COUNT_SCALE_40_EXPANSION_MULTIPLIER="${COUNT_SCALE_40_EXPANSION_MULTIPLIER:-0.25}"

# The pilot produced only two plateau requests. Keep the 65,536-expansion
# observation window, but require two rather than three qualifying windows and
# relax the distribution thresholds slightly. These values apply uniformly;
# only the request-cadence settings are multiplied by problem scale.
COUNT_PLATEAU_CONFIRM_WINDOWS="${COUNT_PLATEAU_CONFIRM_WINDOWS:-2}"
COUNT_PLATEAU_MIN_SHARE="${COUNT_PLATEAU_MIN_SHARE:-0.25}"
COUNT_PLATEAU_MAX_LOWER_SHARE="${COUNT_PLATEAU_MAX_LOWER_SHARE:-0.15}"

COUNT_VLLM_GPUS="${COUNT_VLLM_GPUS:-0}"
COUNT_LLM_MODEL_NAME="${COUNT_LLM_MODEL_NAME:-Qwen3.5-9B}"

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
    "$COUNT_DATASET_ROOT/domain.pddl" \
    "$COUNT_DATASET_ROOT/problems" \
    "$COUNT_MODEL_PATH"; do
    if [[ ! -e "$path" ]]; then
        echo "Missing required path: $path" >&2
        exit 1
    fi
done

selected_problems=()
for scale in 10 20 30 40; do
    mapfile -t candidates < <(
        find "$COUNT_DATASET_ROOT/problems" \
            -maxdepth 1 -type f \
            -name "problem_scale_${scale}_id_*.pddl" \
            -print | sort -V
    )
    if (( ${#candidates[@]} == 0 )); then
        echo "No validation problems found for scale $scale" >&2
        exit 1
    fi
    echo "[COUNT-VALIDATION-LIVE] scale=$scale problems=${#candidates[@]}"
    selected_problems+=("${candidates[@]}")
done

# Set both aliases because Count accepts either spelling. The child-specific
# cadence policy later resolves and records its own scale-adjusted values.
export HYBRID_LLM_PLATEAU_CONFIRM_WINDOWS="$COUNT_PLATEAU_CONFIRM_WINDOWS"
export NLM_LLM_PLATEAU_CONFIRM_WINDOWS="$COUNT_PLATEAU_CONFIRM_WINDOWS"
export HYBRID_LLM_PLATEAU_MIN_SHARE="$COUNT_PLATEAU_MIN_SHARE"
export NLM_LLM_PLATEAU_MIN_SHARE="$COUNT_PLATEAU_MIN_SHARE"
export HYBRID_LLM_PLATEAU_MAX_LOWER_SHARE="$COUNT_PLATEAU_MAX_LOWER_SHARE"
export NLM_LLM_PLATEAU_MAX_LOWER_SHARE="$COUNT_PLATEAU_MAX_LOWER_SHARE"

mkdir -p "$COUNT_RESULTS_DIR"

echo "[COUNT-VALIDATION-LIVE] project=$COUNT_PROJECT_ROOT"
echo "[COUNT-VALIDATION-LIVE] dataset=$COUNT_DATASET_ROOT"
echo "[COUNT-VALIDATION-LIVE] model=$COUNT_MODEL_PATH"
echo "[COUNT-VALIDATION-LIVE] results=$COUNT_RESULTS_DIR"
echo "[COUNT-VALIDATION-LIVE] jobs=${#selected_problems[@]} mode=live resume=on"
echo "[COUNT-VALIDATION-LIVE] cadence multipliers: scale10/20=1 scale30=$COUNT_SCALE_30_EXPANSION_MULTIPLIER scale40=$COUNT_SCALE_40_EXPANSION_MULTIPLIER"
echo "[COUNT-VALIDATION-LIVE] plateau: window=65536 confirm=$COUNT_PLATEAU_CONFIRM_WINDOWS min_share=$COUNT_PLATEAU_MIN_SHARE max_lower_share=$COUNT_PLATEAU_MAX_LOWER_SHARE"

batch_arguments=(
    "$COUNT_DATASET_ROOT/domain.pddl"
    "${selected_problems[@]}"
    --default-mode live
    --output-dir "$COUNT_RESULTS_DIR"
    --resume
    --small-parallelism "$COUNT_SMALL_PARALLELISM"
    --small-time-limit "$COUNT_SMALL_TIME_LIMIT"
    --large-time-limit "$COUNT_LARGE_TIME_LIMIT"
    --small-max-requests "$COUNT_SMALL_MAX_REQUESTS"
    --large-max-requests "$COUNT_LARGE_MAX_REQUESTS"
    --scale-aware-llm-thresholds
    --scale-30-expansion-multiplier "$COUNT_SCALE_30_EXPANSION_MULTIPLIER"
    --scale-40-expansion-multiplier "$COUNT_SCALE_40_EXPANSION_MULTIPLIER"
    --vllm-model-path "$COUNT_MODEL_PATH"
    --llm-model "$COUNT_LLM_MODEL_NAME"
    --vllm-gpus "$COUNT_VLLM_GPUS"
)

cd "$COUNT_PROJECT_ROOT"
exec bash scripts/run_batch_linux.sh "${batch_arguments[@]}" "$@"
