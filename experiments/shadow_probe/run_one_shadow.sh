#!/usr/bin/env bash
# Run one real Count anytime search with a logging-only LLM trigger probe.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
count_root="$(cd "$script_dir/../.." && pwd)"
domain="${COUNT_SHADOW_DOMAIN:-/mnt/e/Python Projects/PyPACE/data/generated-pddl/depots-numeric-new/domain.pddl}"
python_bin="${COUNT_SHADOW_PYTHON:-/home/professorxu/miniconda3/envs/PyPACE_env/bin/python}"

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "用法：bash run_one_shadow.sh PROBLEM_PDDL RESULTS_ROOT [SECONDS]" >&2
    exit 2
fi

problem="$1"
results_root="$2"
time_limit="${3:-${COUNT_SHADOW_SECONDS:-1800}}"

[[ -f "$problem" ]] || { echo "找不到 problem：$problem" >&2; exit 2; }
[[ -f "$domain" ]] || { echo "找不到 domain：$domain" >&2; exit 2; }
[[ -x "$python_bin" ]] || { echo "找不到 PyPACE Python：$python_bin" >&2; exit 2; }
[[ -x "$count_root/builds/release64/bin/downward" ]] || {
    echo "Count release64 尚未编译" >&2
    exit 2
}
[[ "$time_limit" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
    echo "SECONDS 必须是正数" >&2
    exit 2
}

problem_file="$(basename "$problem")"
problem_id="${problem_file%.pddl}"
if [[ "$problem_id" =~ problem_scale_([0-9]+)_id_([0-9]+) ]]; then
    scale="${BASH_REMATCH[1]}"
else
    echo "无法从文件名识别规模：$problem_file" >&2
    exit 2
fi

run_id="shadow-${problem_id}"
run_dir="$results_root/$problem_id"
if [[ -e "$run_dir/planner.log" ]]; then
    echo "结果已存在，拒绝覆盖：$run_dir" >&2
    exit 2
fi
mkdir -p "$run_dir"

search="iterated([lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff),lazy_greedy(hff,preferred=hff,reopen_closed=false,llm_h=hff),lazy_wastar(hff,preferred=hff,w=5,llm_h=hff),lazy_wastar(hff,preferred=hff,w=3,llm_h=hff),lazy_wastar(hff,preferred=hff,w=2,llm_h=hff),lazy_wastar(hff,preferred=hff,w=1,llm_h=hff)],pass_bound=true,repeat_last=true,continue_on_solve=true,continue_on_fail=true,max_time=$time_limit)"

cat >"$run_dir/run_config.txt" <<EOF
problem=$problem
domain=$domain
run_id=$run_id
scale=$scale
search_time_limit_seconds=$time_limit
probe_mode=log
request_initial=0
global_stall=${COUNT_SHADOW_ENABLE_GLOBAL_STALL:-1}
ancestor_stagnation=${COUNT_SHADOW_ENABLE_ANCESTOR_STAGNATION:-1}
expansion_plateau=${COUNT_SHADOW_ENABLE_EXPANSION_PLATEAU:-1}
plateau_window_expansions=${COUNT_SHADOW_PLATEAU_WINDOW:-65536}
plateau_confirm_windows=${COUNT_SHADOW_PLATEAU_CONFIRM_WINDOWS:-3}
plateau_reset_windows=${COUNT_SHADOW_PLATEAU_RESET_WINDOWS:-2}
plateau_min_bucket_expansions=${COUNT_SHADOW_PLATEAU_MIN_BUCKET_EXPANSIONS:-16384}
plateau_min_since_request_expansions=${COUNT_SHADOW_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS:-65536}
plateau_min_share=${COUNT_SHADOW_PLATEAU_MIN_SHARE:-0.3}
plateau_max_lower_share=${COUNT_SHADOW_PLATEAU_MAX_LOWER_SHARE:-0.1}
plateau_h_bucket_width=${COUNT_SHADOW_PLATEAU_H_BUCKET_WIDTH:-0.001}
plateau_per_layer_gap=${COUNT_SHADOW_PLATEAU_PER_LAYER_GAP:-500000}
stall_expansions=${COUNT_SHADOW_STALL_EXPANSIONS:-500000}
min_request_gap_expansions=${COUNT_SHADOW_MIN_REQUEST_GAP:-500000}
ancestor_check_interval=${COUNT_SHADOW_ANCESTOR_INTERVAL:-100000}
ancestor_depth=${COUNT_SHADOW_ANCESTOR_DEPTH:-20}
min_depth=${COUNT_SHADOW_MIN_DEPTH:-30}
max_requests_per_phase=${COUNT_SHADOW_MAX_REQUESTS:-10}
emit_state=${COUNT_SHADOW_EMIT_STATE:-0}
EOF

echo
echo "== Shadow probe: $problem_id =="
echo "规模=$scale，搜索上限=${time_limit}s，结果目录=$run_dir"
echo "不启动 HTTP/LLM；可另开终端查看：tail -f '$run_dir/planner.log'"

start_seconds=$SECONDS
set +e
(
    cd "$run_dir"
    env \
        HYBRID_LLM_TRIGGER=1 \
        HYBRID_LLM_COMM_MODE=log \
        HYBRID_LLM_RUN_ID="$run_id" \
        NLM_LLM_RUN_ID="$run_id" \
        HYBRID_LLM_PROBLEM_ID="$problem_id" \
        HYBRID_LLM_REQUEST_INITIAL=0 \
        HYBRID_LLM_ENABLE_GLOBAL_STALL="${COUNT_SHADOW_ENABLE_GLOBAL_STALL:-1}" \
        HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION="${COUNT_SHADOW_ENABLE_ANCESTOR_STAGNATION:-1}" \
        HYBRID_LLM_ENABLE_EXPANSION_PLATEAU="${COUNT_SHADOW_ENABLE_EXPANSION_PLATEAU:-1}" \
        HYBRID_LLM_ENABLE_FRONTIER_PLATEAU=0 \
        HYBRID_LLM_PLATEAU_WINDOW_EXPANSIONS="${COUNT_SHADOW_PLATEAU_WINDOW:-65536}" \
        HYBRID_LLM_PLATEAU_CONFIRM_WINDOWS="${COUNT_SHADOW_PLATEAU_CONFIRM_WINDOWS:-3}" \
        HYBRID_LLM_PLATEAU_RESET_WINDOWS="${COUNT_SHADOW_PLATEAU_RESET_WINDOWS:-2}" \
        HYBRID_LLM_PLATEAU_MIN_BUCKET_EXPANSIONS="${COUNT_SHADOW_PLATEAU_MIN_BUCKET_EXPANSIONS:-16384}" \
        HYBRID_LLM_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS="${COUNT_SHADOW_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS:-65536}" \
        HYBRID_LLM_PLATEAU_MIN_SHARE="${COUNT_SHADOW_PLATEAU_MIN_SHARE:-0.3}" \
        HYBRID_LLM_PLATEAU_MAX_LOWER_SHARE="${COUNT_SHADOW_PLATEAU_MAX_LOWER_SHARE:-0.1}" \
        HYBRID_LLM_PLATEAU_H_BUCKET_WIDTH="${COUNT_SHADOW_PLATEAU_H_BUCKET_WIDTH:-0.001}" \
        HYBRID_LLM_PLATEAU_PER_LAYER_REQUEST_GAP_EXPANSIONS="${COUNT_SHADOW_PLATEAU_PER_LAYER_GAP:-500000}" \
        HYBRID_LLM_STALL_EXPANSIONS="${COUNT_SHADOW_STALL_EXPANSIONS:-500000}" \
        HYBRID_LLM_MIN_REQUEST_GAP_EXPANSIONS="${COUNT_SHADOW_MIN_REQUEST_GAP:-500000}" \
        HYBRID_LLM_ANCESTOR_CHECK_INTERVAL="${COUNT_SHADOW_ANCESTOR_INTERVAL:-100000}" \
        HYBRID_LLM_ANCESTOR_DEPTH="${COUNT_SHADOW_ANCESTOR_DEPTH:-20}" \
        HYBRID_LLM_MIN_DEPTH="${COUNT_SHADOW_MIN_DEPTH:-30}" \
        HYBRID_LLM_MAX_REQUESTS="${COUNT_SHADOW_MAX_REQUESTS:-10}" \
        HYBRID_LLM_EMIT_STATE="${COUNT_SHADOW_EMIT_STATE:-0}" \
        "$python_bin" "$count_root/fast-downward.py" \
            --build release64 \
            --plan-file "$run_dir/sas_plan" \
            "$domain" "$problem" \
            --heuristic 'hff=irhff(cost_type=one)' \
            --search "$search"
) >"$run_dir/planner.log" 2>&1
planner_exit_code=$?
set -e
runner_wall_seconds=$((SECONDS - start_seconds))

printf 'planner_exit_code=%s\nrunner_wall_seconds=%s\n' \
    "$planner_exit_code" "$runner_wall_seconds" >>"$run_dir/run_config.txt"

"$python_bin" "$script_dir/parse_shadow_log.py" \
    --log "$run_dir/planner.log" \
    --out "$run_dir" \
    --problem "$problem_file" \
    --scale "$scale" \
    --run-id "$run_id" \
    --time-limit "$time_limit" \
    --planner-exit-code "$planner_exit_code" \
    --runner-wall-seconds "$runner_wall_seconds"

echo "完成：planner_exit_code=$planner_exit_code runner_wall_seconds=$runner_wall_seconds"
echo "曲线：$run_dir/incumbents.csv"
echo "触发：$run_dir/trigger_events.csv"
