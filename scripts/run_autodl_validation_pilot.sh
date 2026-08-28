#!/usr/bin/env bash

set -euo pipefail

# AutoDL paths for the first validation pilot. Override any value by exporting
# the corresponding variable before launching this script.
COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-/root/autodl-tmp/count-downward-llm}"
COUNT_DATASET_ROOT="${COUNT_DATASET_ROOT:-/root/PyPACE/data/generated-pddl/depots-numeric-validation-original}"
COUNT_MODEL_PATH="${COUNT_MODEL_PATH:-/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350}"
COUNT_RUN_MODE="${COUNT_RUN_MODE:-live}"
COUNT_RUN_TAG="${COUNT_RUN_TAG:-pilot-${COUNT_RUN_MODE}}"
COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/${COUNT_RUN_TAG}}"

COUNT_SMALL_PARALLELISM="${COUNT_SMALL_PARALLELISM:-2}"
COUNT_VLLM_GPUS="${COUNT_VLLM_GPUS:-0}"
COUNT_LLM_MODEL_NAME="${COUNT_LLM_MODEL_NAME:-Qwen3.5-9B}"

case "$COUNT_RUN_MODE" in
    live|off) ;;
    *)
        echo "COUNT_RUN_MODE must be 'live' or 'off', got: $COUNT_RUN_MODE" >&2
        exit 2
        ;;
esac

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

if [[ "$COUNT_RUN_MODE" == "live" && ! -d "$COUNT_MODEL_PATH" ]]; then
    echo "Missing model directory: $COUNT_MODEL_PATH" >&2
    exit 1
fi

selected_problems=()
for scale in 10 20 30 40; do
    required=5
    if [[ "$scale" == "40" ]]; then
        required=3
    fi

    mapfile -t candidates < <(
        find "$COUNT_DATASET_ROOT/problems" \
            -maxdepth 1 -type f \
            -name "problem_scale_${scale}_id_*.pddl" \
            -print | sort -V
    )
    if (( ${#candidates[@]} < required )); then
        echo "Scale $scale has ${#candidates[@]} problems; need $required" >&2
        exit 1
    fi
    for ((index = 0; index < required; ++index)); do
        selected_problems+=("${candidates[index]}")
    done
done

mkdir -p "$COUNT_RESULTS_DIR"

echo "[COUNT-AUTODL-PILOT] project=$COUNT_PROJECT_ROOT"
echo "[COUNT-AUTODL-PILOT] dataset=$COUNT_DATASET_ROOT"
echo "[COUNT-AUTODL-PILOT] model=$COUNT_MODEL_PATH"
echo "[COUNT-AUTODL-PILOT] results=$COUNT_RESULTS_DIR"
echo "[COUNT-AUTODL-PILOT] mode=$COUNT_RUN_MODE jobs=${#selected_problems[@]} resume=on"
printf '[COUNT-AUTODL-PILOT] selected=%s\n' "${selected_problems[@]}"

batch_arguments=(
    "$COUNT_DATASET_ROOT/domain.pddl"
    "${selected_problems[@]}"
    --default-mode "$COUNT_RUN_MODE"
    --output-dir "$COUNT_RESULTS_DIR"
    --resume
    --small-parallelism "$COUNT_SMALL_PARALLELISM"
    --small-time-limit 1800
    --large-time-limit 3600
    --small-max-requests 10
    --large-max-requests 15
)

if [[ "$COUNT_RUN_MODE" == "live" ]]; then
    batch_arguments+=(
        --vllm-model-path "$COUNT_MODEL_PATH"
        --llm-model "$COUNT_LLM_MODEL_NAME"
        --vllm-gpus "$COUNT_VLLM_GPUS"
    )
fi

cd "$COUNT_PROJECT_ROOT"
exec bash scripts/run_batch_linux.sh "${batch_arguments[@]}" "$@"
