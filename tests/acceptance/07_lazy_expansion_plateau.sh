#!/usr/bin/env bash
# Core property: Lazy plateau detection uses expanded-state h and requests the
# first matching state after activation, rather than the activation state.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

section "测试 7/7：Lazy 真实 h 展开流平台"
echo "期望：两个单扩展窗口确认平台；第 3 次扩展的同桶真实状态触发请求。"

work_dir="$(new_workspace lazy-plateau)"
prepare_issue34 "$work_dir"
search_log="$work_dir/search.log"

(
    cd "$work_dir"
    env \
        HYBRID_LLM_TRIGGER=1 \
        HYBRID_LLM_COMM_MODE=log \
        HYBRID_LLM_RUN_ID=acceptance-lazy-plateau \
        HYBRID_LLM_REQUEST_INITIAL=0 \
        HYBRID_LLM_ENABLE_EXPANSION_PLATEAU=1 \
        HYBRID_LLM_ENABLE_GLOBAL_STALL=0 \
        HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION=0 \
        HYBRID_LLM_PLATEAU_WINDOW_EXPANSIONS=1 \
        HYBRID_LLM_PLATEAU_CONFIRM_WINDOWS=2 \
        HYBRID_LLM_PLATEAU_RESET_WINDOWS=2 \
        HYBRID_LLM_PLATEAU_MIN_BUCKET_EXPANSIONS=1 \
        HYBRID_LLM_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS=1 \
        HYBRID_LLM_PLATEAU_MIN_SHARE=1 \
        HYBRID_LLM_PLATEAU_MAX_LOWER_SHARE=1 \
        HYBRID_LLM_PLATEAU_H_BUCKET_WIDTH=1000000 \
        HYBRID_LLM_PLATEAU_PER_LAYER_REQUEST_GAP_EXPANSIONS=0 \
        HYBRID_LLM_MIN_REQUEST_GAP_EXPANSIONS=0 \
        HYBRID_LLM_MAX_REQUESTS=1 \
        "$downward" \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search 'lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,max_time=30)' \
        --internal-plan-file "$work_dir/sas_plan" \
        <output
) >"$search_log" 2>&1

require_log '\[HYBRID-LLM-PLATEAU\] event=activated .*expansions=2' \
    "$search_log" "平台没有在连续两个窗口后激活"
require_log '\[NLM-LLM-TRIGGER\] request .*reason=expansion_plateau .*expansions=3 ' \
    "$search_log" "平台没有请求激活后的第一个同桶真实展开状态"
reject_log '\[NLM-LLM-TRIGGER\] request .*reason=expansion_plateau .*expansions=2 ' \
    "$search_log" "平台错误地请求了用于完成确认窗口的状态"

trigger_stats="$(last_trigger_stats "$search_log")"
assert_eq "$(stat_value "$trigger_stats" plateau_activations)" 1 \
    "平台激活次数不正确"
assert_eq "$(stat_value "$trigger_stats" plateau_requests)" 1 \
    "平台请求次数不正确"

show_evidence "$search_log" \
    'HYBRID-LLM-PLATEAU.*(window|activated|request_submitted)|NLM-LLM-TRIGGER.*request'
pass "Lazy 平台由真实展开 h 激活，并请求后续首个匹配状态"
print_artifacts "$work_dir"
