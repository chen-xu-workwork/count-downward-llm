#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$(cd "$script_dir/.." && pwd)}"
runner="$COUNT_PROJECT_ROOT/scripts/run_depots_initial_only_rerun_11.sh"
source "$runner"
count_initial_only_rerun_defaults
# Check before mkdir/tee: even the transcript must never touch original data.
count_initial_only_rerun_validate_paths "$@"
count_initial_only_configure_runtime "$@"

rerun_session="${COUNT_RERUN_TMUX_SESSION:-count-depots-initial-only-rerun11-seed-$COUNT_EXPERIMENT_SEED}"
rerun_log="$COUNT_RESULTS_DIR/tmux-run.log"
if ! command -v tmux >/dev/null 2>&1; then echo "Missing required command: tmux" >&2; exit 1; fi
if tmux has-session -t "$rerun_session" 2>/dev/null; then
    echo "tmux session already exists: $rerun_session" >&2
    echo "Attach with: tmux attach -t '$rerun_session'" >&2
    exit 2
fi
mkdir -p "$COUNT_RESULTS_DIR"

forward_variables=(
    PATH PYTHONPATH LD_LIBRARY_PATH CONDA_PREFIX CONDA_DEFAULT_ENV VIRTUAL_ENV
    CUDA_VISIBLE_DEVICES NLM_VLLM_EXECUTABLE
    COUNT_PROJECT_ROOT COUNT_DATA_ROOT COUNT_DATASET_ROOT COUNT_MODEL_PATH
    COUNT_RERUN_RESULTS_DIR COUNT_ORIGINAL_RESULTS_DIR
    COUNT_EXPERIMENT_SEED COUNT_PARALLELISM COUNT_SMALL_PARALLELISM COUNT_LARGE_PARALLELISM
    COUNT_RUNTIME_ENV_PREFIX COUNT_RUNTIME_LIB_DIR COUNT_LLM_PARTIAL_ONLY COUNT_LLM_QUERY_POLICY
    COUNT_VLLM_GPUS COUNT_LLM_MODEL_NAME COUNT_VLLM_PORT
    COUNT_PLANNER_PYTHON COUNT_BUILD COUNT_AUTO_BUILD COUNT_BUILD_JOBS COUNT_PROMPT_DOMAIN_CODE
)
printf -v project_q '%q' "$COUNT_PROJECT_ROOT"
printf -v runner_q '%q' "$runner"
printf -v log_q '%q' "$rerun_log"
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
for argument in "$@"; do printf -v argument_q '%q' "$argument"; launch_command+=" $argument_q"; done
launch_command+=" 2>&1 | tee -a $log_q; statuses=(\"\${PIPESTATUS[@]}\"); "
launch_command+="run_status=\${statuses[0]}; if [[ \$run_status -eq 0 && \${statuses[1]} -ne 0 ]]; then run_status=\${statuses[1]}; fi; "
launch_command+="printf '===== END %s status=%s =====\\n' \"\$(date --iso-8601=seconds)\" \"\$run_status\" | tee -a $log_q; exit \"\$run_status\""
printf -v bash_command 'bash -c %q' "$launch_command"
tmux new-session -d -s "$rerun_session" "$bash_command"
echo "Started detached tmux session: $rerun_session"
echo "Original results remain read-only: $COUNT_ORIGINAL_RESULTS_DIR"
echo "Rerun results: $COUNT_RESULTS_DIR"
echo "Transcript: $rerun_log"
echo "Attach: tmux attach -t '$rerun_session'"
