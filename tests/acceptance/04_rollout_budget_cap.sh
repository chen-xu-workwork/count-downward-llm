#!/usr/bin/env bash
# Core property: a long response cannot exceed the configured burst action cap.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

section "测试 4/7：rollout 动作预算硬上限"
echo "期望：mock 返回 10 步，但搜索器只接受并处理前 3 步，然后恢复基础搜索。"

work_dir="$(new_workspace budget-cap)"
prepare_issue34 "$work_dir"
search_log="$work_dir/search.log"
server_log="$work_dir/mock-server.log"
port="$(free_port)"

python3 "$mock_server" \
    --port "$port" \
    --action '(pop-start s12 b11 a1 a2 b0 gasoleo lco)' \
    --action '(pop-end s12 a1 a2 b5 b0)' \
    --action '(pop-start s12 b2 a1 a2 b11 rat-a gasoleo)' \
    --action '(pop-end s12 a1 a2 b0 b11)' \
    --action '(push-start s12 b6 a1 a2 b11 rat-a gasoleo)' \
    --action '(push-end s12 a1 a2 b2 b11)' \
    --action '(push-start s12 b0 a1 a2 b6 lco rat-a)' \
    --action '(push-end s12 a1 a2 b11 b6)' \
    --action '(push-start s12 b1 a1 a2 b0 gasoleo lco)' \
    --action '(push-end s12 a1 a2 b6 b0)' \
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
        HYBRID_LLM_MAX_ACTIONS_PER_PROPOSAL=100 \
        HYBRID_LLM_MAX_BURST_ACTIONS=3 \
        HYBRID_LLM_RUN_ID=acceptance-budget \
        "$downward" \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search 'lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,max_time=30)' \
        --internal-plan-file "$work_dir/sas_plan" \
        <output
) >"$search_log" 2>&1

wait "$server_pid"
trap - EXIT

require_log 'event=response_accepted.*actions=3' "$search_log" "响应没有被硬截断为 3 步"
require_log 'Solution found\.' "$search_log" "预算截断后基础搜索没有恢复"
stats="$(last_rollout_stats "$search_log")"
assert_eq "$(stat_value "$stats" llm_actions_requested)" 10 "没有正确记录模型原始动作数"
assert_eq "$(stat_value "$stats" llm_actions_prevalidated)" 3 "搜索侧接受动作数没有遵守预算"
assert_eq "$(stat_value "$stats" llm_actions_processed)" 3 "实际 rollout 动作数没有遵守预算"
assert_eq "$(stat_value "$stats" llm_responses_rejected_budget)" 1 "预算截断事件没有进入统计"

show_evidence "$search_log" 'event=response_accepted|Solution found\.|HYBRID-LLM-ROLLOUT-STATS'
pass "响应级总预算生效，过长建议不会无限抢占 Lazy 搜索"
print_artifacts "$work_dir"
