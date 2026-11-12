#!/usr/bin/env bash

set -euo pipefail

async_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$async_script_dir/run_depots_initial_only_30_40.sh"

count_initial_async_defaults() {
    # Ignore a serial launcher's exported output path.
    COUNT_RESULTS_DIR="${COUNT_ASYNC_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-initial-only-async-30-40/seed_${COUNT_EXPERIMENT_SEED:-101}}"
    count_initial_only_defaults
    COUNT_ASYNC_RESULTS_DIR="$COUNT_RESULTS_DIR"
    # This entry is launch-only, even if a serial launcher exported auto-build.
    export COUNT_ASYNC_RESULTS_DIR COUNT_LLM_INITIAL_ASYNC=1 COUNT_AUTO_BUILD=0
}

count_initial_async_validate_paths() {
    local argument output_resolved protected protected_resolved candidate
    for argument in "$@"; do
        case "$argument" in
            --output-dir|--output-dir=*)
                echo "Use COUNT_ASYNC_RESULTS_DIR to keep the experiment and tmux log together." >&2
                return 1 ;;
        esac
    done
    output_resolved="$(realpath -m -- "$COUNT_RESULTS_DIR")"
    output_resolved="${output_resolved%/}"
    for protected in /root/autodl-tmp/count-results/depots-initial-only-30-40 /root/autodl-tmp/count-results/depots-initial-only-rerun-11; do
        protected_resolved="$(realpath -m -- "$protected")"
        case "$output_resolved/" in
            "$protected_resolved/"*) echo "Async output must be separate from preserved serial results." >&2; return 1 ;;
        esac
        case "$protected_resolved/" in
            "$output_resolved/"*) echo "Async output must not contain preserved serial results." >&2; return 1 ;;
        esac
    done
    candidate="$output_resolved"
    while [[ -n "$candidate" && "$candidate" != / ]]; do
        if [[ -f "$candidate/initial_only_config.json" ]]; then
            if [[ "$candidate" != "$output_resolved" ]] || ! grep -Eq '"experiment"[[:space:]]*:[[:space:]]*"depots_initial_only_async_lazy_30_40"' "$candidate/initial_only_config.json"; then
                echo "Async output overlaps an existing experiment; choose a new COUNT_ASYNC_RESULTS_DIR." >&2
                return 1
            fi
        fi
        candidate="$(dirname "$candidate")"
    done
}

count_initial_async_main() {
    count_initial_async_defaults
    count_initial_async_validate_paths "$@"
    count_initial_only_configure_runtime "$@"
    cd "$COUNT_PROJECT_ROOT"
    local planner_python="${COUNT_PLANNER_PYTHON:-python3}"
    if ! command -v "$planner_python" >/dev/null 2>&1; then
        echo "Missing planner Python executable: $planner_python" >&2
        return 1
    fi
    local arguments=(
        -m hybrid_planner.initial_only_async
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
    )
    if [[ -n "${COUNT_DATASET_ROOT:-}" ]]; then arguments+=(--dataset-root "$COUNT_DATASET_ROOT"); fi
    if [[ -n "${COUNT_PROMPT_DOMAIN_CODE:-}" ]]; then arguments+=(--prompt-domain-code "$COUNT_PROMPT_DOMAIN_CODE"); fi
    if [[ -n "${NLM_VLLM_EXECUTABLE:-}" ]]; then arguments+=(--vllm-executable "$NLM_VLLM_EXECUTABLE"); fi
    arguments+=("$@")
    echo "[COUNT-INITIAL-ASYNC] initial_state_only=1 native_search_concurrent=1 cross_phase_replies=discard"
    echo "[COUNT-INITIAL-ASYNC] output=$COUNT_RESULTS_DIR selected=30:50,40:13 seed=$COUNT_EXPERIMENT_SEED"
    echo "[COUNT-INITIAL-ASYNC] parallelism=30:$COUNT_SMALL_PARALLELISM,40:$COUNT_LARGE_PARALLELISM limits=30:1800s,40:3600s"
    echo "[COUNT-INITIAL-ASYNC] prebuilt_binary_required=1 build=$COUNT_BUILD auto_build=0 auto_test=0"
    exec "$planner_python" "${arguments[@]}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    count_initial_async_main "$@"
fi
