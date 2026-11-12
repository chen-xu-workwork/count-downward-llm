#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$(cd "$script_dir/.." && pwd)}"
runner="$COUNT_PROJECT_ROOT/scripts/run_depots_initial_only_async_30_40.sh"
source "$runner"
count_initial_async_defaults
# Validate before creating even a transcript in a preserved serial directory.
count_initial_async_validate_paths "$@"
count_initial_only_configure_runtime "$@"

async_session="${COUNT_ASYNC_TMUX_SESSION:-count-depots-initial-only-async-seed-$COUNT_EXPERIMENT_SEED}"
async_log="$COUNT_RESULTS_DIR/tmux-run.log"
if ! command -v tmux >/dev/null 2>&1; then echo "Missing required command: tmux" >&2; exit 1; fi
if tmux has-session -t "$async_session" 2>/dev/null; then
    echo "tmux session already exists: $async_session" >&2
    echo "Attach with: tmux attach -t '$async_session'" >&2
    exit 2
fi
mkdir -p "$COUNT_RESULTS_DIR"

forward_variables=(
    PATH PYTHONPATH LD_LIBRARY_PATH CONDA_PREFIX CONDA_DEFAULT_ENV VIRTUAL_ENV
    CUDA_VISIBLE_DEVICES NLM_VLLM_EXECUTABLE
    COUNT_PROJECT_ROOT COUNT_DATA_ROOT COUNT_DATASET_ROOT COUNT_MODEL_PATH COUNT_ASYNC_RESULTS_DIR
    COUNT_EXPERIMENT_SEED COUNT_PARALLELISM COUNT_SMALL_PARALLELISM COUNT_LARGE_PARALLELISM
    COUNT_RUNTIME_ENV_PREFIX COUNT_RUNTIME_LIB_DIR COUNT_LLM_PARTIAL_ONLY COUNT_LLM_QUERY_POLICY COUNT_LLM_INITIAL_ASYNC
    COUNT_VLLM_GPUS COUNT_LLM_MODEL_NAME COUNT_VLLM_PORT
    COUNT_PLANNER_PYTHON COUNT_BUILD COUNT_PROMPT_DOMAIN_CODE
)
printf -v project_q '%q' "$COUNT_PROJECT_ROOT"
printf -v runner_q '%q' "$runner"
printf -v log_q '%q' "$async_log"
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
tmux new-session -d -s "$async_session" "$bash_command"
echo "Started detached tmux session: $async_session"
echo "Async results: $COUNT_RESULTS_DIR"
echo "Transcript: $async_log"
echo "Attach: tmux attach -t '$async_session'"
