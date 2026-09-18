#!/usr/bin/env bash

set -euo pipefail

# One reproducible scale-30/40 repetition. The generic launcher still owns the
# tmux session, and the batch console still starts, checks and stops vLLM.

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_project_root="$(cd "$script_dir/.." && pwd)"

if [[ -z "${COUNT_EXPERIMENT_SEED:-}" ]]; then
    echo "COUNT_EXPERIMENT_SEED is required for a repetition run" >&2
    echo "Example: COUNT_EXPERIMENT_SEED=20260918 bash scripts/$(basename "$0")" >&2
    exit 1
fi
if [[ ! "$COUNT_EXPERIMENT_SEED" =~ ^[0-9]+$ ]]; then
    echo "COUNT_EXPERIMENT_SEED must be in [0, 2147483647]" >&2
    exit 1
fi

# Normalize leading zeroes without relying on shell integer parsing first.
normalized_seed="${COUNT_EXPERIMENT_SEED#"${COUNT_EXPERIMENT_SEED%%[!0]*}"}"
normalized_seed="${normalized_seed:-0}"
if (( ${#normalized_seed} > 10 )); then
    echo "COUNT_EXPERIMENT_SEED must be in [0, 2147483647]" >&2
    exit 1
fi
if (( ${#normalized_seed} == 10 )) \
    && [[ "$normalized_seed" > "2147483647" ]]; then
    echo "COUNT_EXPERIMENT_SEED must be in [0, 2147483647]" >&2
    exit 1
fi
COUNT_EXPERIMENT_SEED="$normalized_seed"

COUNT_PROJECT_ROOT="${COUNT_PROJECT_ROOT:-$default_project_root}"
COUNT_REPEAT_TAG="${COUNT_REPEAT_TAG:-validation-live-scale30-40-repeat-v1}"
COUNT_RUN_TAG="${COUNT_RUN_TAG:-$COUNT_REPEAT_TAG-seed-$COUNT_EXPERIMENT_SEED}"
COUNT_RESULTS_ROOT="${COUNT_RESULTS_ROOT:-/root/autodl-tmp/count-results/depots-numeric-validation-original/qwen3_5-9b-global_step_350/$COUNT_REPEAT_TAG}"
COUNT_RESULTS_DIR="${COUNT_RESULTS_DIR:-$COUNT_RESULTS_ROOT/seed_$COUNT_EXPERIMENT_SEED}"
COUNT_TMUX_SESSION="${COUNT_TMUX_SESSION:-count-validation-30-40-seed-$COUNT_EXPERIMENT_SEED}"
COUNT_TMUX_LOG="${COUNT_TMUX_LOG:-$COUNT_RESULTS_DIR/tmux-run.log}"

# This dedicated entry point deliberately fixes its experiment scope and keeps
# the proven owned-vLLM lifecycle. Model path/GPU/model-name overrides are
# forwarded by the generic launcher.
COUNT_SCALES="30,40"
COUNT_EXTERNAL_VLLM="0"

export \
    COUNT_PROJECT_ROOT \
    COUNT_RUN_TAG \
    COUNT_RESULTS_DIR \
    COUNT_TMUX_SESSION \
    COUNT_TMUX_LOG \
    COUNT_SCALES \
    COUNT_EXPERIMENT_SEED \
    COUNT_EXTERNAL_VLLM

exec bash "$COUNT_PROJECT_ROOT/scripts/start_validation_live_scale_aware_tmux.sh"
