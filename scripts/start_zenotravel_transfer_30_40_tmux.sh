#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$(cd "$script_dir/.." && pwd)}"
runner="$COUNT_PROJECT_ROOT/scripts/run_zenotravel_transfer_30_40.sh"
if [[ ! -f "$runner" ]]; then
    echo "Missing experiment runner: $runner" >&2
    exit 1
fi
source "$runner"
count_transfer_defaults
count_transfer_configure_runtime "$@"

COUNT_TMUX_SESSION="${COUNT_TMUX_SESSION:-count-depots-to-zenotravel-seed-$COUNT_EXPERIMENT_SEED}"
COUNT_TMUX_LOG="${COUNT_TMUX_LOG:-$COUNT_RESULTS_DIR/tmux-run.log}"
if ! command -v tmux >/dev/null 2>&1; then
    echo "Missing required command: tmux" >&2
    exit 1
fi
if tmux has-session -t "$COUNT_TMUX_SESSION" 2>/dev/null; then
    echo "tmux session already exists: $COUNT_TMUX_SESSION" >&2
    echo "Attach with: tmux attach -t '$COUNT_TMUX_SESSION'" >&2
    exit 2
fi
mkdir -p "$COUNT_RESULTS_DIR" "$(dirname "$COUNT_TMUX_LOG")"

# A long-lived tmux server can retain old experiment paths. Set or unset every
# supported input explicitly in the shell that starts the new job.
forward_variables=(
    PATH PYTHONPATH LD_LIBRARY_PATH CONDA_PREFIX CONDA_DEFAULT_ENV VIRTUAL_ENV
    CUDA_VISIBLE_DEVICES NLM_VLLM_EXECUTABLE
    COUNT_PROJECT_ROOT COUNT_DATA_ROOT COUNT_DATASET_ROOT COUNT_MODEL_PATH COUNT_RESULTS_DIR
    COUNT_SAMPLE_30 COUNT_SAMPLE_40 COUNT_SAMPLE_SEED COUNT_EXPERIMENT_SEED COUNT_PARALLELISM
    COUNT_RUNTIME_ENV_PREFIX COUNT_RUNTIME_LIB_DIR COUNT_LLM_PARTIAL_ONLY
    COUNT_VLLM_GPUS COUNT_LLM_MODEL_NAME COUNT_VLLM_PORT
    COUNT_PLANNER_PYTHON COUNT_BUILD COUNT_PROMPT_DOMAIN_CODE
)
printf -v project_q '%q' "$COUNT_PROJECT_ROOT"
printf -v runner_q '%q' "$runner"
printf -v log_q '%q' "$COUNT_TMUX_LOG"
launch_command="set -o pipefail; cd $project_q || exit; "
for variable_name in "${forward_variables[@]}"; do
    if [[ -v "$variable_name" ]]; then
        printf -v value_q '%q' "${!variable_name}"
        launch_command+="export $variable_name=$value_q; "
    else
        launch_command+="unset $variable_name; "
    fi
done
launch_command+="printf '\\n===== START %s =====\\n' \"\$(date --iso-8601=seconds)\" | tee -a $log_q; "
launch_command+="bash $runner_q"
for argument in "$@"; do
    printf -v argument_q '%q' "$argument"
    launch_command+=" $argument_q"
done
launch_command+=" 2>&1 | tee -a $log_q; statuses=(\"\${PIPESTATUS[@]}\"); "
launch_command+="run_status=\${statuses[0]}; if [[ \$run_status -eq 0 && \${statuses[1]} -ne 0 ]]; then run_status=\${statuses[1]}; fi; "
launch_command+="printf '===== END %s status=%s =====\\n' \"\$(date --iso-8601=seconds)\" \"\$run_status\" | tee -a $log_q; exit \"\$run_status\""
printf -v bash_command 'bash -c %q' "$launch_command"
tmux new-session -d -s "$COUNT_TMUX_SESSION" "$bash_command"

echo "Started detached tmux session: $COUNT_TMUX_SESSION"
echo "Results: $COUNT_RESULTS_DIR"
echo "Transcript: $COUNT_TMUX_LOG"
echo "Attach: tmux attach -t '$COUNT_TMUX_SESSION'"
echo "Detach without stopping: Ctrl-b, then d"
