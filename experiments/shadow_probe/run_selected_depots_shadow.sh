#!/usr/bin/env bash
# Sequentially run the fixed two-scale-30/two-scale-40 real-problem sample.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
problems_root="${COUNT_SHADOW_PROBLEMS_ROOT:-/mnt/e/Python Projects/PyPACE/data/generated-pddl/depots-numeric-new/problems}"
python_bin="${COUNT_SHADOW_PYTHON:-/home/professorxu/miniconda3/envs/PyPACE_env/bin/python}"
time_limit="${1:-${COUNT_SHADOW_SECONDS:-1800}}"
results_root="${2:-$script_dir/results/depots-shadow-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$results_root"
cp "$script_dir/selected_problems.txt" "$results_root/selected_problems.txt"

mapfile -t selected < <(
    sed -e 's/[[:space:]]*$//' -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' \
        "$script_dir/selected_problems.txt"
)

echo "Count 原生 anytime + logging-only LLM probe"
echo "问题数=${#selected[@]}，每题搜索上限=${time_limit}s，顺序运行"
echo "总结果目录：$results_root"

for problem_name in "${selected[@]}"; do
    bash "$script_dir/run_one_shadow.sh" \
        "$problems_root/$problem_name" "$results_root" "$time_limit"
done

"$python_bin" "$script_dir/aggregate_shadow_results.py" "$results_root"

echo
echo "ALL SHADOW RUNS FINISHED"
echo "总表：$results_root/all_runs_summary.csv"
echo "所有 cost-time 点：$results_root/all_incumbents.csv"
echo "所有触发：$results_root/all_trigger_events.csv"
echo "所有平台窗口：$results_root/all_plateau_events.csv"
echo "所有 phase：$results_root/all_phases.csv"
