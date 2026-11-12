#!/usr/bin/env bash

set -euo pipefail

rerun_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$rerun_script_dir/run_depots_initial_only_30_40.sh"

count_initial_only_rerun_defaults() {
    # A separately named setting prevents an exported old COUNT_RESULTS_DIR
    # from redirecting this repair back into the preserved original run.
    COUNT_RESULTS_DIR="${COUNT_RERUN_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-initial-only-rerun-11/seed_${COUNT_EXPERIMENT_SEED:-101}}"
    COUNT_ORIGINAL_RESULTS_DIR="${COUNT_ORIGINAL_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-initial-only-30-40/seed_101}"
    count_initial_only_defaults
    COUNT_RERUN_RESULTS_DIR="$COUNT_RESULTS_DIR"
    export COUNT_RERUN_RESULTS_DIR COUNT_ORIGINAL_RESULTS_DIR
}

count_initial_only_rerun_validate_paths() {
    local original_resolved output_resolved argument
    for argument in "$@"; do
        case "$argument" in
            --output-dir|--output-dir=*|--rerun-from|--rerun-from=*)
                echo "Use COUNT_RERUN_RESULTS_DIR and COUNT_ORIGINAL_RESULTS_DIR for paths in this shell entry." >&2
                return 1 ;;
        esac
    done
    if [[ ! -d "$COUNT_ORIGINAL_RESULTS_DIR" ]]; then
        echo "Missing preserved original results: $COUNT_ORIGINAL_RESULTS_DIR" >&2
        return 1
    fi
    original_resolved="$(realpath -m -- "$COUNT_ORIGINAL_RESULTS_DIR")"
    output_resolved="$(realpath -m -- "$COUNT_RESULTS_DIR")"
    original_resolved="${original_resolved%/}"
    output_resolved="${output_resolved%/}"
    case "$output_resolved/" in
        "$original_resolved/"*) echo "Rerun output must be outside original results." >&2; return 1 ;;
    esac
    case "$original_resolved/" in
        "$output_resolved/"*) echo "Rerun output must not contain original results." >&2; return 1 ;;
    esac
}

count_initial_only_rerun_main() {
    count_initial_only_rerun_defaults
    count_initial_only_rerun_validate_paths "$@"
    count_initial_only_configure_runtime "$@"
    cd "$COUNT_PROJECT_ROOT"
    local planner_python="${COUNT_PLANNER_PYTHON:-python3}"
    if ! command -v "$planner_python" >/dev/null 2>&1; then
        echo "Missing planner Python executable: $planner_python" >&2
        return 1
    fi
    case "$COUNT_AUTO_BUILD" in
        0|1) ;;
        *) echo "COUNT_AUTO_BUILD must be 0 or 1" >&2; return 1 ;;
    esac
    local arguments=(
        -m hybrid_planner.initial_only_rerun
        --rerun-from "$COUNT_ORIGINAL_RESULTS_DIR"
        --output-dir "$COUNT_RESULTS_DIR"
        --data-root "$COUNT_DATA_ROOT"
        --model-path "$COUNT_MODEL_PATH"
        --experiment-seed "$COUNT_EXPERIMENT_SEED"
        --small-parallelism "$COUNT_SMALL_PARALLELISM"
        --large-parallelism "$COUNT_LARGE_PARALLELISM"
        --vllm-gpus "$COUNT_VLLM_GPUS"
        --llm-model "$COUNT_LLM_MODEL_NAME"
        --vllm-port "$COUNT_VLLM_PORT"
        --planner-python "$planner_python"
        --build "$COUNT_BUILD"
        --build-jobs "$COUNT_BUILD_JOBS"
    )
    [[ "$COUNT_AUTO_BUILD" == 0 ]] && arguments+=(--skip-build)
    if [[ -n "${COUNT_DATASET_ROOT:-}" ]]; then arguments+=(--dataset-root "$COUNT_DATASET_ROOT"); fi
    if [[ -n "${COUNT_PROMPT_DOMAIN_CODE:-}" ]]; then arguments+=(--prompt-domain-code "$COUNT_PROMPT_DOMAIN_CODE"); fi
    if [[ -n "${NLM_VLLM_EXECUTABLE:-}" ]]; then arguments+=(--vllm-executable "$NLM_VLLM_EXECUTABLE"); fi
    arguments+=("$@")
    echo "[COUNT-INITIAL-RERUN] original_read_only=$COUNT_ORIGINAL_RESULTS_DIR"
    echo "[COUNT-INITIAL-RERUN] output=$COUNT_RESULTS_DIR selected=30:10,40:1 seed=$COUNT_EXPERIMENT_SEED"
    echo "[COUNT-INITIAL-RERUN] full_problem_restart=1 parallelism=30:$COUNT_SMALL_PARALLELISM,40:$COUNT_LARGE_PARALLELISM auto_build=$COUNT_AUTO_BUILD"
    exec "$planner_python" "${arguments[@]}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    count_initial_only_rerun_main "$@"
fi
