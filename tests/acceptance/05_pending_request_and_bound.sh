#!/usr/bin/env bash
# Core property: an empty base open list does not terminate the search while an
# asynchronous request is still pending; the returned edge is still bound-safe.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

section "测试 5/7：OpenList 暂空时等待在途请求 + 严格 bound"
echo "期望：基础边在 bound=1 下耗尽后等待约 0.35 秒；响应到达并因 bound 中止，然后才宣告失败。"

work_dir="$(new_workspace pending-bound)"
prepare_issue34 "$work_dir"
search_log="$work_dir/search.log"
server_log="$work_dir/mock-server.log"
port="$(free_port)"

python3 "$mock_server" \
    --port "$port" \
    --delay-seconds 0.35 \
    --action '(pop-start s12 b11 a1 a2 b0 gasoleo lco)' \
    >"$server_log" 2>&1 &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.2

start_ns="$(date +%s%N)"
set +e
(
    cd "$work_dir"
    env \
        HYBRID_LLM_TRIGGER=1 \
        HYBRID_LLM_COMM_MODE=http \
        HYBRID_LLM_HTTP_HOST=127.0.0.1 \
        HYBRID_LLM_HTTP_PORT="$port" \
        HYBRID_LLM_HTTP_TIMEOUT_MS=2000 \
        HYBRID_LLM_HTTP_WORKERS=1 \
        HYBRID_LLM_MAX_PENDING=1 \
        HYBRID_LLM_MAX_REQUESTS=1 \
        HYBRID_LLM_REQUEST_INITIAL=1 \
        HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION=0 \
        HYBRID_LLM_ENABLE_GLOBAL_STALL=0 \
        HYBRID_LLM_RUN_ID=acceptance-pending \
        "$downward" \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search 'lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,bound=1,max_time=3)' \
        --internal-plan-file "$work_dir/sas_plan" \
        <output
) >"$search_log" 2>&1
search_status=$?
set -e
elapsed_ms=$((($(date +%s%N) - start_ns) / 1000000))

wait "$server_pid"
trap - EXIT

require_log 'event=response_accepted.*actions=1' "$search_log" "搜索在响应到达前提前退出，或没有接收响应"
require_log 'event=proposal_aborted.*reason=5' "$search_log" "严格 bound 没有中止等于边界的动作"
require_log 'Completely explored state space -- no solution!' "$search_log" "响应处理后没有正常结束无解搜索"
stats="$(last_rollout_stats "$search_log")"
assert_eq "$(stat_value "$stats" llm_bound_aborts)" 1 "bound 中止统计错误"
if (( elapsed_ms < 250 )); then
    fail "搜索只运行了 ${elapsed_ms}ms，没有等待延迟响应"
fi

show_evidence "$server_log" 'MOCK_LLM_REQUEST'
show_evidence "$search_log" 'event=(response_accepted|proposal_aborted)|Completely explored|HYBRID-LLM-(ROLLOUT|TRIGGER)-STATS'
pass "pending 请求阻止了过早失败；响应到达后仍执行严格 bound 检查"
echo "搜索退出码=$search_status（无解搜索允许非零），实测等待=${elapsed_ms}ms"
print_artifacts "$work_dir"
