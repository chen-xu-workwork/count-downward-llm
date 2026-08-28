#!/usr/bin/env bash
# Core property: phases share one absolute deadline and each improvement becomes
# the strict bound of the next phase.

set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/_common.sh"

section "测试 6/7：Anytime incumbent、严格 bound 与共享 deadline"
echo "期望：至少产生两个 incumbent；代价严格下降；下一阶段继承当前最好代价；总计时约 0.2 秒。"

work_dir="$(new_workspace anytime)"
prepare_issue34 "$work_dir"
search_log="$work_dir/search.log"
time_limit="${COUNT_ACCEPTANCE_ANYTIME_SECONDS:-0.2}"
search="iterated([lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff),lazy_greedy(hff,preferred=hff,reopen_closed=false,llm_h=hff),lazy_wastar(hff,preferred=hff,w=5,llm_h=hff)],pass_bound=true,repeat_last=true,continue_on_solve=true,continue_on_fail=true,max_time=$time_limit)"

(
    cd "$work_dir"
    env HYBRID_LLM_TRIGGER=0 \
        "$downward" \
        --heuristic 'hff=irhff(cost_type=one)' \
        --search "$search" \
        --internal-plan-file "$work_dir/sas_plan" \
        <output
) >"$search_log" 2>&1

phase_count="$(grep -c '\[NLM-ANYTIME-PHASE-START\]' "$search_log")"
incumbent_count="$(grep -c '\[NLM-ANYTIME-INCUMBENT\]' "$search_log")"
(( phase_count >= 2 )) || fail "只启动了 $phase_count 个 phase"
(( incumbent_count >= 2 )) || fail "只产生了 $incumbent_count 个 incumbent"
require_log '\[NLM-ANYTIME-PHASE-START\] iteration=2 bound=18([ .]|$)' "$search_log" "第二阶段没有继承第一阶段代价 18 作为严格 bound"
require_log '\[NLM-ANYTIME-RUN-TIMEOUT\]' "$search_log" "没有在共享 deadline 处停止"

costs="$(grep '\[NLM-ANYTIME-INCUMBENT\]' "$search_log" | sed -n 's/.*plan_cost=\([^ ]*\).*/\1/p')"
if ! awk 'NR == 1 {previous=$1; next} {if ($1 >= previous) exit 1; previous=$1} END {if (NR < 2) exit 1}' <<<"$costs"; then
    fail "incumbent 代价不是严格递减：$(tr '\n' ' ' <<<"$costs")"
fi

show_evidence "$search_log" 'NLM-ANYTIME-(PHASE-START|INCUMBENT|RUN-TIMEOUT)'
pass "Anytime 阶段传递严格 bound，incumbent 持续改善，并服从一个共享 wall-clock deadline"
echo "phase 数=$phase_count，incumbent 代价序列：$(tr '\n' ' ' <<<"$costs")"
print_artifacts "$work_dir"
