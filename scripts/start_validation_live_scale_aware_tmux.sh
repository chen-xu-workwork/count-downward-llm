#!/usr/bin/env bash

set -euo pipefail

# Detached tmux launcher for the formal scale-aware live validation batch.
# The inner runner remains the single source of experiment policy; this file
# only owns terminal persistence and one batch-level transcript.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_project_root="$(cd "$script_dir/.." && pwd)"

COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$default_project_root}"
COUNT_RUN_TAG="${COUNT_RUN_TAG:-validation-all-live-scale-aware-v2}"
COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/$COUNT_RUN_TAG}"
COUNT_TMUX_SESSION="${COUNT_TMUX_SESSION:-count-validation-live-v2}"
COUNT_TMUX_LOG="${COUNT_TMUX_LOG:-$COUNT_RESULTS_DIR/tmux-run.log}"

if ! command -v tmux >/dev/null 2>&1; then
    echo "Missing required command: tmux" >&2
    exit 1
fi
if [[ ! -f "$COUNT_PROJECT_ROOT/scripts/run_validation_live_scale_aware_all.sh" ]]; then
    echo "Missing live runner under project root: $COUNT_PROJECT_ROOT" >&2
    exit 1
fi
if tmux has-session -t "$COUNT_TMUX_SESSION" 2>/dev/null; then
    echo "tmux session already exists: $COUNT_TMUX_SESSION" >&2
    echo "Attach with: tmux attach -t '$COUNT_TMUX_SESSION'" >&2
    exit 2
fi

mkdir -p "$COUNT_RESULTS_DIR"
mkdir -p "$(dirname "$COUNT_TMUX_LOG")"

# Forward every public live-run override even when a tmux server was already
# running before this SSH shell was opened. Computed path/tag values are always
# forwarded explicitly below.
forward_variables=(
    PATH
    PYTHONPATH
    LD_LIBRARY_PATH
    CONDA_PREFIX
    CONDA_DEFAULT_ENV
    VIRTUAL_ENV
    CUDA_VISIBLE_DEVICES
    NLM_VLLM_EXECUTABLE
    COUNT_DATASET_ROOT
    COUNT_MODEL_PATH
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
)

printf -v project_q '%q' "$COUNT_PROJECT_ROOT"
printf -v session_q '%q' "$COUNT_TMUX_SESSION"
printf -v tag_q '%q' "$COUNT_RUN_TAG"
printf -v results_q '%q' "$COUNT_RESULTS_DIR"
printf -v log_q '%q' "$COUNT_TMUX_LOG"

launch_command="cd $project_q; "
launch_command+="export COUNT_PROJECT_ROOT=$project_q; "
launch_command+="export COUNT_RUN_TAG=$tag_q; "
launch_command+="export COUNT_RESULTS_DIR=$results_q; "
for variable_name in "${forward_variables[@]}"; do
    if [[ -v "$variable_name" ]]; then
        printf -v value_q '%q' "${!variable_name}"
        launch_command+="export $variable_name=$value_q; "
    fi
done
launch_command+="printf '\n===== START %s session=%s tag=%s =====\n' \"\$(date --iso-8601=seconds)\" $session_q $tag_q | tee -a $log_q; "
launch_command+="set -o pipefail; bash scripts/run_validation_live_scale_aware_all.sh 2>&1 | tee -a $log_q; "
launch_command+="run_status=\${PIPESTATUS[0]}; "
launch_command+="printf '===== END %s status=%s =====\n' \"\$(date --iso-8601=seconds)\" \"\$run_status\" | tee -a $log_q; "
launch_command+="exit \"\$run_status\""

tmux new-session -d -s "$COUNT_TMUX_SESSION" "$launch_command"

echo "Started detached tmux session: $COUNT_TMUX_SESSION"
echo "Results: $COUNT_RESULTS_DIR"
echo "Batch transcript: $COUNT_TMUX_LOG"
echo "Attach: tmux attach -t '$COUNT_TMUX_SESSION'"
echo "Detach without stopping: Ctrl-b, then d"
