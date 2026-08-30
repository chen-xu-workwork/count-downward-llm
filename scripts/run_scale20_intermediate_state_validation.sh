#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"

wsl_dataset_root="/mnt/e/Python Projects/PyPACE/data/generated-pddl/depots-numeric-validation-original"
autodl_dataset_root="/root/PyPACE/data/generated-pddl/depots-numeric-validation-original"
if [[ -n "${COUNT_VALIDATION_DATASET_ROOT:-}" ]]; then
    dataset_root="$COUNT_VALIDATION_DATASET_ROOT"
elif [[ -d "$wsl_dataset_root" ]]; then
    dataset_root="$wsl_dataset_root"
else
    dataset_root="$autodl_dataset_root"
fi
problem_name="${COUNT_INTERMEDIATE_TEST_PROBLEM:-problem_scale_20_id_2.pddl}"
problem_id="${problem_name%.pddl}"
domain="$dataset_root/domain.pddl"
problem="$dataset_root/problems/$problem_name"

capture_time_limit="${COUNT_INTERMEDIATE_CAPTURE_SECONDS:-120}"
native_time_limit="${COUNT_INTERMEDIATE_NATIVE_SECONDS:-1800}"
reuse_capture="${COUNT_INTERMEDIATE_REUSE_CAPTURE:-0}"
timestamp="$(date +%Y%m%d-%H%M%S)"
if [[ "$dataset_root" == "$wsl_dataset_root" ]]; then
    default_output_root="$project_root/experiments/intermediate_state_validation/results"
else
    default_output_root="/root/autodl-tmp/count-results/intermediate-state-validation"
fi
output_dir="${COUNT_INTERMEDIATE_OUTPUT_DIR:-$default_output_root/${problem_id}-${timestamp}}"
prompt_debug_dir="$output_dir/raw_prompt_records"
review_dir="$output_dir/review"

for command in python3; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing required command: $command" >&2
        exit 1
    fi
done

for path in \
    "$project_root/fast-downward.py" \
    "$project_root/builds/release64/bin/downward" \
    "$project_root/tests/validate_intermediate_state_artifact.py" \
    "$domain" \
    "$problem"; do
    if [[ ! -f "$path" ]]; then
        echo "Missing required file: $path" >&2
        exit 1
    fi
done

mkdir -p "$prompt_debug_dir" "$review_dir"

echo "[INTERMEDIATE-STATE-TEST] problem=$problem"
echo "[INTERMEDIATE-STATE-TEST] output=$output_dir"
if [[ "$reuse_capture" == "1" ]]; then
    echo "[INTERMEDIATE-STATE-TEST] phase=1 reuse existing capture"
elif [[ "$reuse_capture" == "0" ]]; then
    echo "[INTERMEDIATE-STATE-TEST] phase=1 capture a real non-initial state"
    set +e
    (
        cd "$project_root"
        env \
            HYBRID_LLM_REQUEST_INITIAL=0 \
            HYBRID_LLM_ENABLE_EXPANSION_PLATEAU=0 \
            HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION=0 \
            HYBRID_LLM_ENABLE_GLOBAL_STALL=1 \
            HYBRID_LLM_STALL_EXPANSIONS=1 \
            HYBRID_LLM_MIN_REQUEST_GAP_EXPANSIONS=0 \
            HYBRID_LLM_MAX_REQUESTS=1 \
            HYBRID_LLM_MAX_PENDING=1 \
            HYBRID_LLM_H_EPSILON=1000000000 \
            HYBRID_LLM_H_RELATIVE_EPSILON=1000000000 \
            python3 -m hybrid_planner.console \
                "$domain" \
                "$problem" \
                "$output_dir/capture_sas_plan" \
                --problem-id "$problem_id" \
                --run-id "intermediate-state-${problem_id}" \
                --anytime-log-dir "$output_dir/capture_anytime" \
                --build release64 \
                --python2 python3 \
                --llm-mode mock \
                --single-pass \
                --search-time-limit "$capture_time_limit" \
                --prompt-domain "$domain" \
                --prompt-problem-dir "$dataset_root/problems" \
                --prompt-debug-dir "$prompt_debug_dir" \
                --http-workers 1 \
                --prompt-workers 1
    ) >"$output_dir/capture_console.log" 2>&1
    capture_status=$?
    set -e
    printf '%s\n' "$capture_status" >"$output_dir/capture_exit_code.txt"
else
    echo "COUNT_INTERMEDIATE_REUSE_CAPTURE must be 0 or 1" >&2
    exit 2
fi

shopt -s nullglob
records=("$prompt_debug_dir"/request_*.json)
if (( ${#records[@]} != 1 )); then
    echo "Expected exactly one captured prompt record, got ${#records[@]}" >&2
    echo "Inspect: $output_dir/capture_console.log" >&2
    exit 1
fi

echo "[INTERMEDIATE-STATE-TEST] phase=2 validate and unpack PDDL/prompt"
(
    cd "$project_root"
    python3 -m tests.validate_intermediate_state_artifact \
        --record "${records[0]}" \
        --domain "$domain" \
        --original-problem "$problem" \
        --output-dir "$review_dir"
) | tee "$output_dir/artifact_validation.log"

echo "[INTERMEDIATE-STATE-TEST] phase=3 solve saved runtime PDDL with LLM off"
native_search="lazy_greedy(hff,preferred=hff,cost_type=one,reopen_closed=false,llm_h=hff,max_time=${native_time_limit})"
set +e
(
    cd "$project_root"
    env \
        HYBRID_LLM_TRIGGER=0 \
        HYBRID_LLM_COMM_MODE=off \
        NLM_LLM_TRIGGER=0 \
        NLM_LLM_COMM_MODE=off \
        python3 fast-downward.py \
            --build release64 \
            --plan-file "$review_dir/native_sas_plan" \
            "$review_dir/domain.pddl" \
            "$review_dir/intermediate_problem.pddl" \
            --heuristic 'hff=irhff(cost_type=one)' \
            --search "$native_search"
) >"$review_dir/native_solver.log" 2>&1
native_status=$?
set -e
printf '%s\n' "$native_status" >"$review_dir/native_solver_exit_code.txt"

native_plans=("$review_dir"/native_sas_plan*)
plan_found=0
for plan in "${native_plans[@]}"; do
    if [[ -s "$plan" ]]; then
        plan_found=1
        break
    fi
done

if (( native_status != 0 || plan_found == 0 )); then
    echo "Native solver did not prove the runtime PDDL usable by finding a plan." >&2
    echo "exit_code=$native_status log=$review_dir/native_solver.log" >&2
    exit 1
fi

echo "[INTERMEDIATE-STATE-TEST] PASS"
echo "Captured request is non-initial, runtime PDDL parsed successfully, prompts were generated, and native Count found a plan."
echo "Human-review directory: $review_dir"
echo "Key files: intermediate_problem.pddl system_prompt.txt user_prompt.txt validation_report.json native_solver.log"
