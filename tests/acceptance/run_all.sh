#!/usr/bin/env bash
# Run every human-readable acceptance test sequentially.

set -euo pipefail
acceptance_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

tests=(
    01_llm_off_baseline.sh
    02_complete_lazy_rollout.sh
    03_invalid_proposal_fallback.sh
    04_rollout_budget_cap.sh
    05_pending_request_and_bound.sh
    06_anytime_bound_and_deadline.sh
    07_lazy_expansion_plateau.sh
)

echo "Count-Downward LLM/Lazy 迁移验收"
echo "将顺序运行 ${#tests[@]} 个独立测试；任意一个失败都会立即停止。"

for test_script in "${tests[@]}"; do
    bash "$acceptance_dir/$test_script"
done

printf '\nALL PASS：七项核心迁移性质均通过。\n'
