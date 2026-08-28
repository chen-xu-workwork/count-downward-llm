#!/usr/bin/env bash
# Core property: an invalid LLM action aborts only that proposal and native
# search resumes immediately.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

section "测试 3/7：非法建议安全回退"
echo "期望：非法动作被拒绝、burst 标记 aborted，但基础搜索仍找到代价 18 的计划。"

work_dir="$(new_workspace invalid-fallback)"
prepare_issue34 "$work_dir"
search_log="$work_dir/search.log"
server_log="$work_dir/mock-server.log"
port="$(free_port)"

python3 "$mock_server" \
    --port "$port" \
    --require-run-id acceptance-invalid \
    --require-iteration 1 \
    --action '(this-operator-does-not-exist)' \
    >"$server_log" 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.2

(
    cd "$work_dir"
    env \
        HYBRID_LLM_TRIGGER=1 \
        HYBRID_LLM_COMM_MODE=http \
        HYBRID_LLM_HTTP_HOST=127.0.0.1 \
        HYBRID_LLM_HTTP_PORT="$port" \
        HYBRID_LLM_HTTP_TIMEOUT_MS=5000 \
        HYBRID_LLM_HTTP_WORKERS=1 \
        HYBRID_LLM_MAX_PENDING=1 \
        HYBRID_LLM_MAX_REQUESTS=1 \
        HYBRID_LLM_REQUEST_INITIAL=1 \
        HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION=0 \
        HYBRID_LLM_ENABLE_GLOBAL_STALL=0 \
        HYBRID_LLM_RUN_ID=acceptance-invalid \
        "$downward" \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search 'lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,max_time=30)' \
        --internal-plan-file "$work_dir/sas_plan" \
        <output
) >"$search_log" 2>&1

wait "$server_pid"
trap - EXIT

require_log 'event=proposal_aborted.*reason=3' "$search_log" "未知动作没有按 inapplicable 分类中止"
require_log 'event=burst_finished.*result=aborted' "$search_log" "非法 proposal 没有结束 burst"
require_log 'Solution found\.' "$search_log" "非法建议破坏了基础搜索恢复"
require_log 'Plan cost: 18' "$search_log" "回退后的基线解代价异常"

stats="$(last_rollout_stats "$search_log")"
assert_eq "$(stat_value "$stats" llm_inapplicable_aborts)" 1 "非法动作中止计数错误"
assert_eq "$(stat_value "$stats" llm_actions_processed)" 0 "非法动作被错误计为已处理"

show_evidence "$search_log" 'event=(proposal_aborted|burst_finished)|Solution found\.|Plan cost:|HYBRID-LLM-ROLLOUT-STATS'
pass "动作解析/适用性防线生效，proposal 失败后基础搜索无条件恢复"
print_artifacts "$work_dir"
