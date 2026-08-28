#!/usr/bin/env bash
# Core property: switching the LLM off leaves native Count lazy search usable.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

section "测试 1/7：LLM-off 原生搜索回归"
echo "期望：问题被正常求解；LLM 请求数和 rollout 动作数都为 0。"

work_dir="$(new_workspace llm-off)"
prepare_issue34 "$work_dir"
search_log="$work_dir/search.log"

(
    cd "$work_dir"
    env HYBRID_LLM_TRIGGER=0 \
        "$downward" \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search 'lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,max_time=30)' \
        --internal-plan-file "$work_dir/sas_plan" \
        <output
) >"$search_log" 2>&1

require_log 'Solution found\.' "$search_log" "关闭 LLM 后 Count 没有找到计划"
require_log 'Plan cost: 18' "$search_log" "基线计划代价发生了非预期变化"
rollout_stats="$(last_rollout_stats "$search_log")"
reject_log '\[(NLM-LLM-TRIGGER|HYBRID-LLM-BRIDGE)\]' "$search_log" "LLM-off 仍产生了请求或通信事件"
assert_eq "$(stat_value "$rollout_stats" llm_actions_processed)" 0 "LLM-off 仍执行了 rollout"
[[ -s "$work_dir/sas_plan" ]] || fail "搜索报告成功但没有写出计划文件"

show_evidence "$search_log" 'Solution found\.|Plan cost:|HYBRID-LLM-ROLLOUT-STATS'
pass "原生 Lazy 搜索保持可用，LLM 开关确实关闭了混合路径"
print_artifacts "$work_dir"
