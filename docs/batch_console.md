# Count batch console

`hybrid_planner.batch_console` owns one long-lived vLLM service and schedules
multiple isolated Count runs against it.  It is the formal Linux-container
entry point for mixed live-LLM and near-native baseline experiments.

## Scheduling contract

- Problems with `scale <= 30` use 1800 seconds, 10 LLM state requests per
  anytime iteration, and may run concurrently.  The default planner
  parallelism is 2 and can be changed with `--small-parallelism`.
- Problems with `scale > 30` use 3600 seconds and 15 LLM state requests per
  anytime iteration.  They are exclusive barriers: all previously submitted
  jobs finish before one starts, and no other problem runs beside it.
- `live` jobs connect to the one shared vLLM service.
- `off` jobs force `NLM_LLM_TRIGGER=0`, start no per-job HTTP bridge, build no
  prompts, and perform no state serialization for LLM requests.

The scale is inferred from names such as `problem_scale_40_id_222.pddl`.  A
manifest job may specify an explicit `scale` when the filename follows another
convention.  Per-job `time_limit_seconds` and
`max_requests_per_iteration` overrides are supported, but formal experiments
should normally keep the scale policy fixed.

## Manifest mode

Edit `experiments/batch/example_manifest.json` so its domain and problem paths
match the Linux container, then run:

```bash
bash scripts/run_batch_linux.sh \
  --manifest experiments/batch/example_manifest.json \
  --output-dir /data/count-results/run-001 \
  --vllm-model-path /data/models/Qwen3.5-9B \
  --vllm-gpus 0 \
  --small-parallelism 2
```

The batch starts vLLM once before the first problem and stops it after every
problem completes.  Off-mode jobs can run while that service remains alive.

To connect to an already-running OpenAI-compatible service instead:

```bash
bash scripts/run_batch_linux.sh \
  --manifest experiments/batch/example_manifest.json \
  --external-vllm \
  --vllm-base-url http://127.0.0.1:8091/v1 \
  --llm-model Qwen3.5-9B
```

## Positional mode

When every job uses the same mode, a manifest is optional:

```bash
bash scripts/run_batch_linux.sh \
  /data/depots/domain.pddl \
  /data/depots/problems/problem_scale_30_id_572.pddl \
  /data/depots/problems/problem_scale_40_id_176.pddl \
  --default-mode off \
  --output-dir /data/count-results/baseline
```

An all-off batch does not start or contact vLLM.

For a clean single-run performance baseline, use an all-off batch with
`--small-parallelism 1`.  Parallel off jobs are supported, but then their CPU
and memory contention is part of the measurement.  Live scale-30 throughput
experiments can keep the default parallelism of 2.

## Outputs

The batch output directory contains:

- `batch_config.json`: resolved policy and all jobs;
- `batch_results.csv`: exit code, runtime and result directory for every job;
- `vllm.log`: the one shared model-server log when the batch owns vLLM;
- one job directory containing `console.log`, `job.json`, plans, and the
  single-run console's anytime CSV/JSON artifacts.

Planner and controller lines are also streamed to the terminal with the job ID
as a prefix, so concurrent small jobs remain distinguishable.

`batch_results.csv` labels normal planner outcomes as `plan_found`,
`unsolvable`, `incomplete`, or `timeout`.  A planned 30/60-minute cutoff is
therefore not treated as a controller failure; only `failed` makes the batch
exit unsuccessfully.

## AutoDL validation pilot

The repository includes a thin wrapper with the current AutoDL project,
validation-dataset, checkpoint and result paths:

```bash
bash scripts/run_autodl_validation_pilot.sh
```

It deterministically selects the first five validation problems at scales 10,
20 and 30, plus the first three at scale 40.  Natural filename ordering is
used, so repeated launches select the same 18 problems.  Scale-40 jobs retain
the batch scheduler's one-at-a-time exclusive policy.

The result directory is deliberately stable rather than timestamped.  The
wrapper enables `--resume`: every child writes `job_result.json` atomically
when it exits.  `plan_found`, `unsolvable`, `incomplete`, and `timeout` markers
are skipped on the next launch.  A job interrupted before completion, or a
job marked `failed`, is retried.  This means a normal time-limit exhaustion
without a solution still counts as processed.

The editable defaults are grouped at the top of the wrapper and can also be
overridden without editing it.  For example:

```bash
COUNT_RUN_MODE=off \
COUNT_RUN_TAG=pilot-off \
bash scripts/run_autodl_validation_pilot.sh
```

Use a new `COUNT_RUN_TAG` or `COUNT_RESULTS_DIR` whenever the model,
LLM/off mode, sample selection, or experiment parameters change.  Resume is
intended for continuing the same experiment configuration.

## Full validation baseline and scale-aware live run

Run every scale-10/20/30/40 problem in the validation directory without
starting vLLM or enabling any LLM trigger/state probe:

```bash
bash scripts/run_validation_baseline_all.sh
```

The wrapper auto-detects the local WSL dataset or uses the AutoDL dataset by
default. It keeps stable output paths and enables `--resume`. The default
parallelism is two for scale 10/20/30 so that an AutoDL baseline can match the
live scheduler; use `COUNT_BASELINE_PARALLELISM=1` for an uncontended local
machine benchmark.

Run the complete AutoDL validation set with one persistent vLLM and
scale-aware LLM cadence:

```bash
bash scripts/run_validation_live_scale_aware_all.sh
```

The live wrapper applies expansion multipliers 1.0, 1.0, 0.5 and 0.25 to
scales 10, 20, 30 and 40. It scales global-stall spacing, the common request
gap, ancestor checks, plateau rearming and the per-plateau request gap. The
65,536-expansion plateau observation window is not scaled. Based on the pilot,
the shared plateau detector profile uses two confirmation windows, a minimum
bucket share of 0.25 and a maximum lower-h share of 0.15. Every job records its
resolved cadence and plateau settings in `job.json`.

The main overrides are:

```bash
COUNT_SCALE_30_EXPANSION_MULTIPLIER=0.5 \
COUNT_SCALE_40_EXPANSION_MULTIPLIER=0.25 \
COUNT_PLATEAU_CONFIRM_WINDOWS=2 \
COUNT_PLATEAU_MIN_SHARE=0.25 \
COUNT_PLATEAU_MAX_LOWER_SHARE=0.15 \
bash scripts/run_validation_live_scale_aware_all.sh
```

Changing any policy value requires a new `COUNT_RUN_TAG` or
`COUNT_RESULTS_DIR`; otherwise `--resume` intentionally trusts existing
completed markers.
