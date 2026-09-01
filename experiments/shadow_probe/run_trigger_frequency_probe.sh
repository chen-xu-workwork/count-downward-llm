#!/usr/bin/env bash
# Run the fixed four-problem shadow sample and report trigger intervals.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python_bin="${COUNT_SHADOW_PYTHON:-/home/professorxu/miniconda3/envs/PyPACE_env/bin/python}"
time_limit="${1:-${COUNT_SHADOW_SECONDS:-1800}}"
results_root="${2:-$script_dir/results/trigger-frequency-$(date +%Y%m%d-%H%M%S)}"
target_seconds="${COUNT_SHADOW_TARGET_INTERVAL_SECONDS:-120}"
min_request_gap="${COUNT_SHADOW_MIN_REQUEST_GAP:-500000}"
stall_expansions="${COUNT_SHADOW_STALL_EXPANSIONS:-500000}"
ancestor_interval="${COUNT_SHADOW_ANCESTOR_INTERVAL:-100000}"
ancestor_depth="${COUNT_SHADOW_ANCESTOR_DEPTH:-20}"
ancestor_min_depth="${COUNT_SHADOW_MIN_DEPTH:-30}"
max_requests="${COUNT_SHADOW_MAX_REQUESTS:-10}"
plateau_window="${COUNT_SHADOW_PLATEAU_WINDOW:-65536}"
plateau_confirm_windows="${COUNT_SHADOW_PLATEAU_CONFIRM_WINDOWS:-3}"
plateau_reset_windows="${COUNT_SHADOW_PLATEAU_RESET_WINDOWS:-2}"
plateau_min_bucket="${COUNT_SHADOW_PLATEAU_MIN_BUCKET_EXPANSIONS:-16384}"
plateau_min_since_request="${COUNT_SHADOW_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS:-65536}"
plateau_min_share="${COUNT_SHADOW_PLATEAU_MIN_SHARE:-0.25}"

echo "Count LLM 触发频率 shadow probe"
echo "每 phase 请求预算=${max_requests}；目标相邻触发间隔约=${target_seconds}s"
echo "共享间隔=${min_request_gap} expansions；global stall=${stall_expansions} expansions"
echo "ancestor: interval=${ancestor_interval} depth=${ancestor_depth} min_depth=${ancestor_min_depth}"
echo "plateau: window=${plateau_window} confirm=${plateau_confirm_windows} reset=${plateau_reset_windows} min_bucket=${plateau_min_bucket} rearm_evidence=${plateau_min_since_request} min_share=${plateau_min_share} candidate_policy=busiest_qualifying_bucket"

COUNT_SHADOW_MIN_REQUEST_GAP="$min_request_gap" \
COUNT_SHADOW_STALL_EXPANSIONS="$stall_expansions" \
COUNT_SHADOW_ANCESTOR_INTERVAL="$ancestor_interval" \
COUNT_SHADOW_ANCESTOR_DEPTH="$ancestor_depth" \
COUNT_SHADOW_MIN_DEPTH="$ancestor_min_depth" \
COUNT_SHADOW_MAX_REQUESTS="$max_requests" \
COUNT_SHADOW_PLATEAU_WINDOW="$plateau_window" \
COUNT_SHADOW_PLATEAU_CONFIRM_WINDOWS="$plateau_confirm_windows" \
COUNT_SHADOW_PLATEAU_RESET_WINDOWS="$plateau_reset_windows" \
COUNT_SHADOW_PLATEAU_MIN_BUCKET_EXPANSIONS="$plateau_min_bucket" \
COUNT_SHADOW_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS="$plateau_min_since_request" \
COUNT_SHADOW_PLATEAU_MIN_SHARE="$plateau_min_share" \
bash "$script_dir/run_selected_depots_shadow.sh" "$time_limit" "$results_root"

"$python_bin" "$script_dir/analyze_trigger_frequency.py" \
    "$results_root" --target-seconds "$target_seconds"

echo
echo "TRIGGER FREQUENCY PROBE FINISHED"
echo "逐次触发间隔：$results_root/trigger_intervals.csv"
echo "逐 phase 频率：$results_root/trigger_frequency_summary.csv"
