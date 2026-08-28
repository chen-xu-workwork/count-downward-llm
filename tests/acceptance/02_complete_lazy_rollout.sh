#!/usr/bin/env bash
# Core property: a complete action chain is processed edge-by-edge while native
# lazy successors remain in the ordinary open lists.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

section "测试 2/7：完整 10 步 Lazy rollout"
echo "期望：mock 收到完整状态；10 个动作逐边成功；普通后继数大于 0；最终有解。"

work_dir="$(new_workspace full-rollout)"
prepare_issue34 "$work_dir"
search_log="$work_dir/search.log"
server_log="$work_dir/mock-server.log"
port="$(free_port)"

python3 "$mock_server" \
    --port "$port" \
    --require-run-id acceptance-full-rollout \
    --require-iteration 1 \
    --require-init-substring '(connect a1 a2 s12)' \
    --require-init-substring '(on b11 a2)' \
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
        HYBRID_LLM_HTTP_PATH=/llm/request \
        HYBRID_LLM_HTTP_TIMEOUT_MS=5000 \
        HYBRID_LLM_HTTP_WORKERS=1 \
        HYBRID_LLM_MAX_PENDING=1 \
        HYBRID_LLM_MAX_REQUESTS=1 \
        HYBRID_LLM_REQUEST_INITIAL=1 \
        HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION=0 \
        HYBRID_LLM_ENABLE_GLOBAL_STALL=0 \
        HYBRID_LLM_RUN_ID=acceptance-full-rollout \
        "$downward" \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search 'lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,max_time=30)' \
        --internal-plan-file "$work_dir/sas_plan" \
        <output
) >"$search_log" 2>&1

wait "$server_pid"
trap - EXIT

require_log 'MOCK_LLM_REQUEST run_id=acceptance-full-rollout.*iteration=1.*actions=10' "$server_log" "mock 没有收到预期请求上下文"
require_log 'event=response_accepted.*actions=10' "$search_log" "搜索器没有接受 10 步响应"
require_log 'event=burst_finished.*result=completed' "$search_log" "rollout 没有完整结束"
require_log 'Solution found\.' "$search_log" "rollout 后没有产生有效计划"

stats="$(last_rollout_stats "$search_log")"
processed="$(stat_value "$stats" llm_actions_processed)"
new_states="$(stat_value "$stats" llm_states_new)"
reopened="$(stat_value "$stats" llm_states_reopened)"
duplicates="$(stat_value "$stats" llm_states_duplicate)"
normal_edges="$(stat_value "$stats" llm_normal_edges_generated)"
assert_eq "$processed" 10 "并非所有建议动作都经过了 Lazy 生命周期"
assert_eq "$((new_states + reopened + duplicates))" 10 "动作结果分类没有覆盖全部 10 步"
assert_gt "$new_states" 0 "rollout 没有产生任何新状态"
assert_gt "$normal_edges" 0 "中间状态没有保留普通 Lazy 后继"

show_evidence "$server_log" 'MOCK_LLM_REQUEST'
show_evidence "$search_log" 'event=(response_accepted|burst_finished)|Solution found\.|HYBRID-LLM-ROLLOUT-STATS'
pass "异步协议、完整状态导出、逐边 rollout、re-anchor 与普通后继保留均正常"
echo "关键统计：new=$new_states reopened=$reopened duplicate=$duplicates normal_edges=$normal_edges"
print_artifacts "$work_dir"
