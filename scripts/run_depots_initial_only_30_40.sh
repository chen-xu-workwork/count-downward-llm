#!/usr/bin/env bash

set -euo pipefail

# The tmux wrapper sources these same defaults and runtime setup functions.
count_initial_only_defaults() {
    local runner_dir
    runner_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$(cd "$runner_dir/.." && pwd)}"
    COUNT_DATA_ROOT="${COUNT_DATA_ROOT:-/root/PyPACE/data}"
    COUNT_MODEL_PATH="${COUNT_MODEL_PATH:-/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface}"
    COUNT_EXPERIMENT_SEED="${COUNT_EXPERIMENT_SEED:-101}"
    COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-initial-only-30-40/seed_$COUNT_EXPERIMENT_SEED}"
    COUNT_SMALL_PARALLELISM="${COUNT_SMALL_PARALLELISM:-${COUNT_PARALLELISM:-8}}"
    COUNT_LARGE_PARALLELISM="${COUNT_LARGE_PARALLELISM:-${COUNT_PARALLELISM:-4}}"
    COUNT_VLLM_GPUS="${COUNT_VLLM_GPUS:-0}"
    COUNT_LLM_MODEL_NAME="${COUNT_LLM_MODEL_NAME:-Qwen3.5-9B}"
    COUNT_VLLM_PORT="${COUNT_VLLM_PORT:-8091}"
    COUNT_RUNTIME_ENV_PREFIX="${COUNT_RUNTIME_ENV_PREFIX:-}"
    COUNT_RUNTIME_LIB_DIR="${COUNT_RUNTIME_LIB_DIR:-}"
    COUNT_BUILD="${COUNT_BUILD:-release64}"
    COUNT_BUILD_JOBS="${COUNT_BUILD_JOBS:-4}"
    COUNT_AUTO_BUILD="${COUNT_AUTO_BUILD:-1}"
    export COUNT_LLM_PARTIAL_ONLY=0
    export COUNT_LLM_QUERY_POLICY=initial-only
}

count_initial_only_prepare_only() {
    local argument
    for argument in "$@"; do
        [[ "$argument" == "--prepare-only" ]] && return 0
    done
    return 1
}

count_initial_only_runtime_lib_is_compatible() {
    [[ -e "$1/libstdc++.so.6" ]] || return 1
    grep -aFq 'CXXABI_1.3.15' "$1/libstdc++.so.6"
}

count_initial_only_configure_runtime() {
    local candidate prepare_only=0
    if count_initial_only_prepare_only "$@"; then
        prepare_only=1
    fi
    # Preparing and executing must select the same Python for frozen configs.
    if [[ -z "$COUNT_RUNTIME_ENV_PREFIX" ]]; then
        for candidate in "${CONDA_PREFIX:-}" /root/miniconda3/envs/verl_env; do
            if [[ -n "$candidate" && -x "$candidate/bin/vllm" ]]; then
                COUNT_RUNTIME_ENV_PREFIX="$candidate"
                break
            fi
        done
    fi
    if [[ -n "$COUNT_RUNTIME_ENV_PREFIX" ]]; then
        if [[ ! -x "$COUNT_RUNTIME_ENV_PREFIX/bin/python3" ]]; then
            if (( ! prepare_only )); then
                echo "Runtime environment does not contain bin/python3: $COUNT_RUNTIME_ENV_PREFIX" >&2
                return 1
            fi
        else
            export PATH="$COUNT_RUNTIME_ENV_PREFIX/bin:$PATH"
            export CONDA_PREFIX="$COUNT_RUNTIME_ENV_PREFIX"
            export CONDA_DEFAULT_ENV="$(basename "$COUNT_RUNTIME_ENV_PREFIX")"
        fi
        COUNT_RUNTIME_LIB_DIR="${COUNT_RUNTIME_LIB_DIR:-$COUNT_RUNTIME_ENV_PREFIX/lib}"
    fi
    export COUNT_RUNTIME_ENV_PREFIX COUNT_RUNTIME_LIB_DIR
    if (( prepare_only )); then
        return 0
    fi
    if [[ -n "$COUNT_RUNTIME_LIB_DIR" ]]; then
        if ! count_initial_only_runtime_lib_is_compatible "$COUNT_RUNTIME_LIB_DIR"; then
            echo "COUNT_RUNTIME_LIB_DIR needs libstdc++.so.6 with CXXABI_1.3.15: $COUNT_RUNTIME_LIB_DIR" >&2
            return 1
        fi
    else
        for candidate in "${CONDA_PREFIX:+$CONDA_PREFIX/lib}" /root/miniconda3/envs/verl_env/lib; do
            if [[ -n "$candidate" ]] && count_initial_only_runtime_lib_is_compatible "$candidate"; then
                COUNT_RUNTIME_LIB_DIR="$candidate"
                break
            fi
        done
    fi
    if [[ -n "$COUNT_RUNTIME_LIB_DIR" ]]; then
        case ":${LD_LIBRARY_PATH:-}:" in
            *":$COUNT_RUNTIME_LIB_DIR:"*) ;;
            *) export LD_LIBRARY_PATH="$COUNT_RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
        esac
    fi
    export COUNT_RUNTIME_ENV_PREFIX COUNT_RUNTIME_LIB_DIR
}

count_initial_only_main() {
    count_initial_only_defaults
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
        -m hybrid_planner.initial_only
        --data-root "$COUNT_DATA_ROOT"
        --model-path "$COUNT_MODEL_PATH"
        --output-dir "$COUNT_RESULTS_DIR"
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
    if [[ "$COUNT_AUTO_BUILD" == 0 ]]; then
        arguments+=(--skip-build)
    fi
    if [[ -n "${COUNT_DATASET_ROOT:-}" ]]; then
        arguments+=(--dataset-root "$COUNT_DATASET_ROOT")
    fi
    if [[ -n "${COUNT_PROMPT_DOMAIN_CODE:-}" ]]; then
        arguments+=(--prompt-domain-code "$COUNT_PROMPT_DOMAIN_CODE")
    fi
    if [[ -n "${NLM_VLLM_EXECUTABLE:-}" ]]; then
        arguments+=(--vllm-executable "$NLM_VLLM_EXECUTABLE")
    fi
    arguments+=("$@")

    echo "[COUNT-INITIAL-ONLY] domain=depots policy=initial-only baseline=not-run adaptive=not-run"
    echo "[COUNT-INITIAL-ONLY] data_root=$COUNT_DATA_ROOT results=$COUNT_RESULTS_DIR"
    echo "[COUNT-INITIAL-ONLY] scale_30=50 scale_40=13 seed=$COUNT_EXPERIMENT_SEED"
    echo "[COUNT-INITIAL-ONLY] parallelism=30:$COUNT_SMALL_PARALLELISM,40:$COUNT_LARGE_PARALLELISM limits=30:1800s,40:3600s partial_only=0"
    echo "[COUNT-INITIAL-ONLY] auto_build=$COUNT_AUTO_BUILD build=$COUNT_BUILD compiler_jobs=$COUNT_BUILD_JOBS (prepare-only always skips build)"
    exec "$planner_python" "${arguments[@]}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    count_initial_only_main "$@"
fi
