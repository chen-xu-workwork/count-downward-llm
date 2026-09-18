#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_project_root="$(cd "$script_dir/.." && pwd)"

count_end_records() {
    local log_path="$1"
    local count
    count="$(grep -c '^===== END .* status=.* =====$' "$log_path" 2>/dev/null || true)"
    printf '%s' "${count:-0}"
}

if [[ "${1:-}" == "--watch" ]]; then
    if (( $# != 6 )); then
        echo "Internal queue invocation requires five arguments" >&2
        exit 2
    fi
    current_session="$2"
    current_log="$3"
    initial_end_count="$4"
    next_seed="$5"
    queue_log="$6"
    poll_seconds="${COUNT_QUEUE_POLL_SECONDS:-60}"

    mkdir -p "$(dirname "$queue_log")"
    exec > >(tee -a "$queue_log") 2>&1

    echo "[COUNT-QUEUE] waiting session=$current_session next_seed=$next_seed"
    while tmux has-session -t "$current_session" 2>/dev/null; do
        sleep "$poll_seconds"
    done

    final_end_count="$(count_end_records "$current_log")"
    if (( final_end_count <= initial_end_count )); then
        echo "[COUNT-QUEUE] current session ended without a new completion record; next seed will not start" >&2
        exit 1
    fi
    last_end_record="$(grep '^===== END .* status=.* =====$' "$current_log" | tail -n 1)"
    if [[ ! "$last_end_record" =~ status=0[[:space:]]+=====$ ]]; then
        echo "[COUNT-QUEUE] current run was not successful: $last_end_record" >&2
        echo "[COUNT-QUEUE] next seed will not start" >&2
        exit 1
    fi

    echo "[COUNT-QUEUE] current run completed successfully; launching seed=$next_seed"
    unset \
        COUNT_RUN_TAG \
        COUNT_RESULTS_DIR \
        COUNT_TMUX_SESSION \
        COUNT_TMUX_LOG \
        COUNT_SCALES \
        COUNT_EXTERNAL_VLLM
    export COUNT_EXPERIMENT_SEED="$next_seed"
    exec bash "$COUNT_PROJECT_ROOT/scripts/start_validation_live_scale30_40_repeat_tmux.sh"
fi

normalize_seed() {
    local raw_value="$1"
    local normalized
    if [[ ! "$raw_value" =~ ^[0-9]+$ ]]; then
        return 1
    fi
    normalized="${raw_value#"${raw_value%%[!0]*}"}"
    normalized="${normalized:-0}"
    if (( ${#normalized} > 10 )); then
        return 1
    fi
    if (( ${#normalized} == 10 )) \
        && [[ "$normalized" > "2147483647" ]]; then
        return 1
    fi
    printf '%s' "$normalized"
}

if [[ -z "${COUNT_CURRENT_SEED:-}" || -z "${COUNT_NEXT_SEED:-}" ]]; then
    echo "COUNT_CURRENT_SEED and COUNT_NEXT_SEED are required" >&2
    echo "Example: COUNT_CURRENT_SEED=101 COUNT_NEXT_SEED=202 bash scripts/$(basename "$0")" >&2
    exit 1
fi
if ! current_seed="$(normalize_seed "$COUNT_CURRENT_SEED")"; then
    echo "COUNT_CURRENT_SEED must be in [0, 2147483647]" >&2
    exit 1
fi
if ! next_seed="$(normalize_seed "$COUNT_NEXT_SEED")"; then
    echo "COUNT_NEXT_SEED must be in [0, 2147483647]" >&2
    exit 1
fi
if [[ "$current_seed" == "$next_seed" ]]; then
    echo "COUNT_CURRENT_SEED and COUNT_NEXT_SEED must differ" >&2
    exit 1
fi

COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$default_project_root}"
COUNT_REPEAT_TAG="${COUNT_REPEAT_TAG:-validation-live-scale30-40-repeat-v1}"
COUNT_RESULTS_ROOT="${COUNT_RESULTS_ROOT:-/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/$COUNT_REPEAT_TAG}"
COUNT_CURRENT_TMUX_SESSION="${COUNT_CURRENT_TMUX_SESSION:-count-validation-30-40-seed-$current_seed}"
COUNT_CURRENT_RESULTS_DIR="${COUNT_CURRENT_RESULTS_DIR:-$COUNT_RESULTS_ROOT/seed_$current_seed}"
COUNT_CURRENT_TMUX_LOG="${COUNT_CURRENT_TMUX_LOG:-$COUNT_CURRENT_RESULTS_DIR/tmux-run.log}"
COUNT_QUEUE_TMUX_SESSION="${COUNT_QUEUE_TMUX_SESSION:-count-validation-queue-$current_seed-to-$next_seed}"
COUNT_QUEUE_LOG="${COUNT_QUEUE_LOG:-$COUNT_RESULTS_ROOT/queue_${current_seed}_to_${next_seed}.log}"

if ! command -v tmux >/dev/null 2>&1; then
    echo "Missing required command: tmux" >&2
    exit 1
fi
if [[ ! -f "$COUNT_PROJECT_ROOT/scripts/start_validation_live_scale30_40_repeat_tmux.sh" ]]; then
    echo "Missing repetition launcher under project root: $COUNT_PROJECT_ROOT" >&2
    exit 1
fi
if ! tmux has-session -t "$COUNT_CURRENT_TMUX_SESSION" 2>/dev/null; then
    echo "Current experiment tmux session was not found: $COUNT_CURRENT_TMUX_SESSION" >&2
    exit 1
fi
if tmux has-session -t "$COUNT_QUEUE_TMUX_SESSION" 2>/dev/null; then
    echo "Queue tmux session already exists: $COUNT_QUEUE_TMUX_SESSION" >&2
    exit 2
fi
if [[ ! -f "$COUNT_CURRENT_TMUX_LOG" ]]; then
    echo "Current experiment transcript was not found: $COUNT_CURRENT_TMUX_LOG" >&2
    exit 1
fi

initial_end_count="$(count_end_records "$COUNT_CURRENT_TMUX_LOG")"
mkdir -p "$(dirname "$COUNT_QUEUE_LOG")"

forward_variables=(
    PATH
    PYTHONPATH
    LD_LIBRARY_PATH
    CONDA_PREFIX
    CONDA_DEFAULT_ENV
    VIRTUAL_ENV
    CUDA_VISIBLE_DEVICES
    NLM_VLLM_EXECUTABLE
    COUNT_PROJECT_ROOT
    COUNT_DATASET_ROOT
    COUNT_MODEL_PATH
    COUNT_RUNTIME_LIB_DIR
    COUNT_RESULTS_ROOT
    COUNT_REPEAT_TAG
    COUNT_SMALL_PARALLELISM
    COUNT_LARGE_PARALLELISM
    COUNT_SMALL_TIME_LIMIT
    COUNT_LARGE_TIME_LIMIT
    COUNT_SMALL_MAX_REQUESTS
    COUNT_LARGE_MAX_REQUESTS
    COUNT_SCALE_30_EXPANSION_MULTIPLIER
    COUNT_SCALE_40_EXPANSION_MULTIPLIER
    COUNT_PLATEAU_CONFIRM_WINDOWS
    COUNT_PLATEAU_MIN_SHARE
    COUNT_VLLM_GPUS
    COUNT_LLM_MODEL_NAME
    COUNT_QUEUE_POLL_SECONDS
)

watch_command=(env)
for variable_name in "${forward_variables[@]}"; do
    if [[ -v "$variable_name" ]]; then
        watch_command+=("$variable_name=${!variable_name}")
    fi
done
watch_command+=(
    bash
    "$script_dir/$(basename "$0")"
    --watch
    "$COUNT_CURRENT_TMUX_SESSION"
    "$COUNT_CURRENT_TMUX_LOG"
    "$initial_end_count"
    "$next_seed"
    "$COUNT_QUEUE_LOG"
)
printf -v watch_command_q '%q ' "${watch_command[@]}"

tmux new-session -d -s "$COUNT_QUEUE_TMUX_SESSION" "$watch_command_q"

echo "Queued next repetition in tmux session: $COUNT_QUEUE_TMUX_SESSION"
echo "Waiting for current session: $COUNT_CURRENT_TMUX_SESSION"
echo "Next experiment seed: $next_seed"
echo "Queue transcript: $COUNT_QUEUE_LOG"
echo "Attach to watcher: tmux attach -t '$COUNT_QUEUE_TMUX_SESSION'"
