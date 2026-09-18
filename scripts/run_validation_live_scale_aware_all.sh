#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_project_root="$(cd "$script_dir/.." && pwd)"

COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$default_project_root}"
COUNT_DATASET_ROOT="${COUNT_DATASET_ROOT:-/root/PyPACE/data/generated-pddl/depots-numeric-validation-original}"
COUNT_MODEL_PATH="${COUNT_MODEL_PATH:-/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface}"
COUNT_RUN_TAG="${COUNT_RUN_TAG:-validation-all-live-scale-aware-v2}"
COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/$COUNT_RUN_TAG}"
COUNT_SCALES="${COUNT_SCALES:-10,20,30,40}"
COUNT_EXPERIMENT_SEED="${COUNT_EXPERIMENT_SEED:-}"

COUNT_SMALL_PARALLELISM="${COUNT_SMALL_PARALLELISM:-8}"
COUNT_LARGE_PARALLELISM="${COUNT_LARGE_PARALLELISM:-2}"
COUNT_SMALL_TIME_LIMIT="${COUNT_SMALL_TIME_LIMIT:-1800}"
COUNT_LARGE_TIME_LIMIT="${COUNT_LARGE_TIME_LIMIT:-3600}"
COUNT_SMALL_MAX_REQUESTS="${COUNT_SMALL_MAX_REQUESTS:-10}"
COUNT_LARGE_MAX_REQUESTS="${COUNT_LARGE_MAX_REQUESTS:-15}"
COUNT_SCALE_30_EXPANSION_MULTIPLIER="${COUNT_SCALE_30_EXPANSION_MULTIPLIER:-0.5}"
COUNT_SCALE_40_EXPANSION_MULTIPLIER="${COUNT_SCALE_40_EXPANSION_MULTIPLIER:-0.25}"

# The pilot produced only two plateau requests. Keep the 65,536-expansion
# observation window, but require two rather than three qualifying windows.
# A bucket qualifies independently at 25% share; among confirmed qualifying
# buckets, only the busiest is selected as the next plateau request source.
# These values apply uniformly;
# only the request-cadence settings are multiplied by problem scale.
COUNT_PLATEAU_CONFIRM_WINDOWS="${COUNT_PLATEAU_CONFIRM_WINDOWS:-2}"
COUNT_PLATEAU_MIN_SHARE="${COUNT_PLATEAU_MIN_SHARE:-0.25}"

COUNT_VLLM_GPUS="${COUNT_VLLM_GPUS:-0}"
COUNT_LLM_MODEL_NAME="${COUNT_LLM_MODEL_NAME:-Qwen3.5-9B}"
COUNT_EXTERNAL_VLLM="${COUNT_EXTERNAL_VLLM:-0}"
COUNT_VLLM_BASE_URL="${COUNT_VLLM_BASE_URL:-http://127.0.0.1:8091/v1}"

case "${COUNT_EXTERNAL_VLLM,,}" in
    1|true|yes|on)
        use_external_vllm=1
        ;;
    0|false|no|off)
        use_external_vllm=0
        ;;
    *)
        echo "COUNT_EXTERNAL_VLLM must be a boolean (0/1, false/true, no/yes, off/on)" >&2
        exit 1
        ;;
esac

if [[ -n "$COUNT_EXPERIMENT_SEED" ]]; then
    if [[ ! "$COUNT_EXPERIMENT_SEED" =~ ^[0-9]+$ ]] \
        || (( ${#COUNT_EXPERIMENT_SEED} > 10 )); then
        echo "COUNT_EXPERIMENT_SEED must be in [0, 2147483647]" >&2
        exit 1
    fi
    COUNT_EXPERIMENT_SEED=$((10#$COUNT_EXPERIMENT_SEED))
    if (( COUNT_EXPERIMENT_SEED >= 2147483648 )); then
        echo "COUNT_EXPERIMENT_SEED must be in [0, 2147483647]" >&2
        exit 1
    fi
fi

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
    "$COUNT_DATASET_ROOT/problems"; do
    if [[ ! -e "$path" ]]; then
        echo "Missing required path: $path" >&2
        exit 1
    fi
done
if (( ! use_external_vllm )) && [[ ! -e "$COUNT_MODEL_PATH" ]]; then
    echo "Missing required path: $COUNT_MODEL_PATH" >&2
    exit 1
fi

selected_problems=()
read -r -a requested_scales <<< "${COUNT_SCALES//,/ }"
if (( ${#requested_scales[@]} == 0 )); then
    echo "COUNT_SCALES must select at least one problem scale" >&2
    exit 1
fi
declare -A seen_scales=()
for scale in "${requested_scales[@]}"; do
    if [[ ! "$scale" =~ ^[1-9][0-9]*$ ]]; then
        echo "Invalid scale in COUNT_SCALES: $scale" >&2
        exit 1
    fi
    if [[ -v "seen_scales[$scale]" ]]; then
        echo "Duplicate scale in COUNT_SCALES: $scale" >&2
        exit 1
    fi
    seen_scales[$scale]=1
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

mkdir -p "$COUNT_RESULTS_DIR"

echo "[COUNT-VALIDATION-LIVE] project=$COUNT_PROJECT_ROOT"
echo "[COUNT-VALIDATION-LIVE] dataset=$COUNT_DATASET_ROOT"
echo "[COUNT-VALIDATION-LIVE] model=$COUNT_MODEL_PATH"
echo "[COUNT-VALIDATION-LIVE] results=$COUNT_RESULTS_DIR"
echo "[COUNT-VALIDATION-LIVE] scales=${requested_scales[*]} experiment_seed=${COUNT_EXPERIMENT_SEED:-unfixed}"
echo "[COUNT-VALIDATION-LIVE] jobs=${#selected_problems[@]} mode=live resume=on"
echo "[COUNT-VALIDATION-LIVE] parallelism: scale<=30=$COUNT_SMALL_PARALLELISM scale>30=$COUNT_LARGE_PARALLELISM"
echo "[COUNT-VALIDATION-LIVE] cadence multipliers: scale10/20=1 scale30=$COUNT_SCALE_30_EXPANSION_MULTIPLIER scale40=$COUNT_SCALE_40_EXPANSION_MULTIPLIER"
echo "[COUNT-VALIDATION-LIVE] plateau: window=65536 confirm=$COUNT_PLATEAU_CONFIRM_WINDOWS min_share=$COUNT_PLATEAU_MIN_SHARE candidate_policy=busiest_qualifying_bucket"
if (( use_external_vllm )); then
    echo "[COUNT-VALIDATION-LIVE] vllm=external base_url=$COUNT_VLLM_BASE_URL model_name=$COUNT_LLM_MODEL_NAME"
else
    echo "[COUNT-VALIDATION-LIVE] vllm=owned gpus=$COUNT_VLLM_GPUS model_name=$COUNT_LLM_MODEL_NAME"
fi

batch_arguments=(
    "$COUNT_DATASET_ROOT/domain.pddl"
    "${selected_problems[@]}"
    --default-mode live
    --output-dir "$COUNT_RESULTS_DIR"
    --resume
    --small-parallelism "$COUNT_SMALL_PARALLELISM"
    --large-parallelism "$COUNT_LARGE_PARALLELISM"
    --small-time-limit "$COUNT_SMALL_TIME_LIMIT"
    --large-time-limit "$COUNT_LARGE_TIME_LIMIT"
    --small-max-requests "$COUNT_SMALL_MAX_REQUESTS"
    --large-max-requests "$COUNT_LARGE_MAX_REQUESTS"
    --scale-aware-llm-thresholds
    --scale-30-expansion-multiplier "$COUNT_SCALE_30_EXPANSION_MULTIPLIER"
    --scale-40-expansion-multiplier "$COUNT_SCALE_40_EXPANSION_MULTIPLIER"
    --llm-model "$COUNT_LLM_MODEL_NAME"
)

if [[ -n "$COUNT_EXPERIMENT_SEED" ]]; then
    batch_arguments+=(--experiment-seed "$COUNT_EXPERIMENT_SEED")
fi

if (( use_external_vllm )); then
    batch_arguments+=(
        --external-vllm
        --vllm-base-url "$COUNT_VLLM_BASE_URL"
    )
else
    batch_arguments+=(
        --vllm-model-path "$COUNT_MODEL_PATH"
        --vllm-gpus "$COUNT_VLLM_GPUS"
    )
fi

cd "$COUNT_PROJECT_ROOT"
exec bash scripts/run_batch_linux.sh "${batch_arguments[@]}" "$@"
