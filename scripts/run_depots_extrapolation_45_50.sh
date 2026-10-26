#!/usr/bin/env bash

set -euo pipefail

# Public foreground entry point. The tmux launcher sources the setup functions
# below so both entry points use the same Python and shared-library environment.
count_extrapolation_defaults() {
    local runner_dir
    runner_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$(cd "$runner_dir/.." && pwd)}"
    COUNT_DATASET_ROOT="${COUNT_DATASET_ROOT:-/root/PyPACE/data/generated-pddl/depots-numeric-45and50}"
    COUNT_MODEL_PATH="${COUNT_MODEL_PATH:-/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface}"
    COUNT_EXPERIMENT_SEED="${COUNT_EXPERIMENT_SEED:-101}"
    COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-/root/autodl-tmp/count-results/depots45-50-extrapolation/seed_$COUNT_EXPERIMENT_SEED}"
    COUNT_SAMPLE_PER_SCALE="${COUNT_SAMPLE_PER_SCALE:-20}"
    COUNT_SAMPLE_SEED="${COUNT_SAMPLE_SEED:-20261025}"
    COUNT_EXTRAPOLATION_STAGE="${COUNT_EXTRAPOLATION_STAGE:-both}"
    COUNT_VLLM_GPUS="${COUNT_VLLM_GPUS:-0}"
    COUNT_LLM_MODEL_NAME="${COUNT_LLM_MODEL_NAME:-Qwen3.5-9B}"
    COUNT_VLLM_PORT="${COUNT_VLLM_PORT:-8091}"
    COUNT_RUNTIME_ENV_PREFIX="${COUNT_RUNTIME_ENV_PREFIX:-}"
    COUNT_RUNTIME_LIB_DIR="${COUNT_RUNTIME_LIB_DIR:-}"
}

count_extrapolation_prepare_only() {
    local argument
    for argument in "$@"; do
        [[ "$argument" == "--prepare-only" ]] && return 0
    done
    return 1
}

count_extrapolation_runtime_lib_is_compatible() {
    [[ -e "$1/libstdc++.so.6" ]] || return 1
    grep -aFq 'CXXABI_1.3.15' "$1/libstdc++.so.6"
}

count_extrapolation_configure_runtime() {
    local candidate prepare_only=0
    if count_extrapolation_prepare_only "$@"; then
        prepare_only=1
    fi
    # Select the same Python before freezing either a prepared or a live plan.
    # Dependency checks happen later, so preparing needs no model or binary.
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
        if ! count_extrapolation_runtime_lib_is_compatible "$COUNT_RUNTIME_LIB_DIR"; then
            echo "COUNT_RUNTIME_LIB_DIR needs libstdc++.so.6 with CXXABI_1.3.15: $COUNT_RUNTIME_LIB_DIR" >&2
            return 1
        fi
    else
        for candidate in "${CONDA_PREFIX:+$CONDA_PREFIX/lib}" /root/miniconda3/envs/verl_env/lib; do
            if [[ -n "$candidate" ]] && count_extrapolation_runtime_lib_is_compatible "$candidate"; then
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
    if [[ -n "${NLM_VLLM_EXECUTABLE:-}" ]]; then
        export NLM_VLLM_EXECUTABLE
    fi
}

count_extrapolation_main() {
    count_extrapolation_defaults
    count_extrapolation_configure_runtime "$@"
    cd "$COUNT_PROJECT_ROOT"

    local planner_python="${COUNT_PLANNER_PYTHON:-python3}"
    if ! command -v "$planner_python" >/dev/null 2>&1; then
        echo "Missing planner Python executable: $planner_python" >&2
        return 1
    fi
    local arguments=(
        -m hybrid_planner.extrapolation
        --dataset-root "$COUNT_DATASET_ROOT"
        --model-path "$COUNT_MODEL_PATH"
        --output-dir "$COUNT_RESULTS_DIR"
        --sample-per-scale "$COUNT_SAMPLE_PER_SCALE"
        --sample-seed "$COUNT_SAMPLE_SEED"
        --experiment-seed "$COUNT_EXPERIMENT_SEED"
        --stage "$COUNT_EXTRAPOLATION_STAGE"
        --vllm-gpus "$COUNT_VLLM_GPUS"
        --llm-model "$COUNT_LLM_MODEL_NAME"
        --vllm-port "$COUNT_VLLM_PORT"
        --planner-python "$planner_python"
    )
    if [[ -n "${NLM_VLLM_EXECUTABLE:-}" ]]; then
        arguments+=(--vllm-executable "$NLM_VLLM_EXECUTABLE")
    fi
    if [[ -n "${COUNT_BUILD:-}" ]]; then
        arguments+=(--build "$COUNT_BUILD")
    fi
    if [[ -n "${COUNT_PROMPT_DOMAIN_CODE:-}" ]]; then
        arguments+=(--prompt-domain-code "$COUNT_PROMPT_DOMAIN_CODE")
    fi
    # Keep policy fixed even if a caller passes another value among extra CLI
    # arguments. The module owns 3600 seconds, K=3 and full-response admission.
    arguments+=("$@" --parallelism 6)

    echo "[COUNT-EXTRAPOLATION] results=$COUNT_RESULTS_DIR stage=$COUNT_EXTRAPOLATION_STAGE"
    echo "[COUNT-EXTRAPOLATION] scales=45,50 sample_per_scale=$COUNT_SAMPLE_PER_SCALE sample_seed=$COUNT_SAMPLE_SEED experiment_seed=$COUNT_EXPERIMENT_SEED"
    echo "[COUNT-EXTRAPOLATION] parallelism=6 time_limit=3600 llm_partial_only=0 (COUNT_LLM_PARTIAL_ONLY is ignored)"
    exec "$planner_python" "${arguments[@]}"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    count_extrapolation_main "$@"
fi
