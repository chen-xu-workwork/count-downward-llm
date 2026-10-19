# Partial-only Lazy experiment

The optional `--llm-partial-only` switch removes goal-reaching LLM samples
before their action chains are returned to Count Lazy search. It is disabled
by default, so existing experiments keep their original behavior. The C++
search engine and the sampling budget are unchanged.

## Filtering policy

Filtering is applied independently to each generated sample after symbolic
replay from the requested residual state, **before** truncating its validated
action chain to the response action budget.

- `goal_reached=True`: reject the entire sample, including its earlier prefix.
  This also covers samples that reach the goal before later invalid actions.
- `goal_reached=False`: keep the validated incomplete prefix for normal native
  applicability, bound, duplicate and cycle checks.
- Unknown goal status: reject the sample when partial-only mode is enabled.
  Failure to determine the goal status cannot establish incompleteness.

The existing response `status="partial"` describes validation stopping at an
invalid action. It is **not** the condition for this experiment: a fully valid
sample can also fail to reach the goal and must remain eligible.

Rejected samples still consume their original sampling/request budget. They
do not cause replacement sampling. Other incomplete samples from the same
request may still be returned. If every sample is rejected, the response has
no actions to inject and native search continues.

## Running

For a direct console or batch invocation, append `--llm-partial-only`:

```bash
python3 -m hybrid_planner.batch_console DOMAIN PROBLEM \
    --output-dir /path/to/partial-only-results --llm-partial-only \
    --external-vllm --vllm-base-url http://127.0.0.1:8091/v1
```

The validation shell entry points accept `COUNT_LLM_PARTIAL_ONLY=1`; use `0`
or omit it for the full proposal policy. For example:

```bash
COUNT_LLM_PARTIAL_ONLY=1 \
COUNT_RUN_TAG=validation-partial-only \
COUNT_RESULTS_DIR=/path/to/partial-only-results \
    bash scripts/start_validation_live_scale_aware_tmux.sh
```

The generic tmux launcher, scale-30/40 repetition launcher, and next-seed queue
all forward this setting. For repeated runs, choose a separate
`COUNT_REPEAT_TAG`, `COUNT_RESULTS_ROOT` and tmux session as appropriate so
different proposal policies do not share output directories or active sessions.

Keep the checkpoint, prompt, seeds, request/sample budgets, trigger settings,
search schedule, hardware limits and evaluation instances aligned with the
full TRACS-Lazy comparison. Off-mode baseline children do not receive the
filter switch and continue to disable LLM requests.

## Recording and resume

`batch_config.json` records `llm_partial_only`. Batch resume rejects a different
requested setting before resuming jobs or rewriting the batch configuration.
An older `batch_config.json` without this field means `false` and can resume
under the default policy; it cannot resume with partial-only enabled.

Always use a separate results directory for the partial-only experiment.
The switch-specific resume check is not a general check of all experiment
settings, such as checkpoint or prompt identity.

The request CSV, phase CSV and `run.json` include:

- `goal_reaching_sample_count`, `non_goal_sample_count`,
  `goal_unknown_sample_count`: symbolic goal classifications before filtering.
  Unknown includes parse/validation/generation failures with no goal verdict.
- `filtered_goal_sample_count`, `filtered_unknown_sample_count`: samples
  rejected by this switch; the latter counts only otherwise usable chains.
- `pre_filter_usable_sample_count` versus the existing `usable_sample_count`
  (or phase `usable_samples`): usable samples before and after filtering.
- `returned_action_count`: actions in the response after the shared length cap,
  not states admitted by the C++ search. Stale replies count zero returned
  actions and usable samples.

With `--prompt-debug-dir`, each sample also retains its original
`processed_response` plus an `admission` record describing the filter decision
and outgoing action chain. Native duplicate, cycle and bound filtering can
still reject that chain after it reaches C++.
