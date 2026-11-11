"""Frozen Depots 30/40 initial-state-only LLM control for Lazy search."""

import argparse
import contextlib
import hashlib
import json
import os
import pathlib
import platform
import re
import signal
import socket
import sys
from dataclasses import dataclass

from . import batch_console, console
from .extrapolation import (
    DEFAULT_MODEL, clean_environment, pipeline_lock, run_batch, sha256,
    update_status, validate_runtime, write_json,
)


PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
DATASET_RELATIVE = pathlib.Path("generated-pddl/depots-numeric-validation-original")
MANUAL_RELATIVE = pathlib.Path("hybrid_planner/prompting/resources/depots/domain_NOassert.txt")
PROBLEM_PATTERN = re.compile(r"^problem_scale_(30|40)_id_([0-9]+)\.pddl$")
EXPECTED_COUNTS = {"30": 50, "40": 13}
# Audited original validation domain, ignoring whitespace, case and comments.
DEPOTS_DOMAIN_SHA256 = "cdd27bdf0382a664336a844c061e9734288ccdee9f1af4315cdcf38d2cbd8afb"
QUERY_POLICY = "initial-only"
TRIGGER_SETTINGS = {
    "QUERY_POLICY": QUERY_POLICY,
    "REQUEST_INITIAL": "0",
    "ENABLE_EXPANSION_PLATEAU": "0",
    "ENABLE_GLOBAL_STALL": "0",
    "ENABLE_ANCESTOR_STAGNATION": "0",
    "PLATEAU_CONFIRM_WINDOWS": "2",
    "PLATEAU_MIN_SHARE": "0.25",
}


@dataclass
class InitialOnlyPlan:
    output_dir: pathlib.Path
    project_root: pathlib.Path
    result_dir: pathlib.Path
    selection: dict
    config: dict
    command: list
    environment: dict


def build_argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", default="/root/PyPACE/data")
    parser.add_argument("--dataset-root", default="")
    parser.add_argument("--prompt-domain-code", default="")
    parser.add_argument("--model-path", default=DEFAULT_MODEL)
    parser.add_argument("--output-dir", default="/root/autodl-tmp/count-results/depots-initial-only-30-40/seed_101")
    parser.add_argument("--experiment-seed", type=int, default=101)
    parser.add_argument("--small-parallelism", type=int, default=8)
    parser.add_argument("--large-parallelism", type=int, default=4)
    parser.add_argument("--prepare-only", action="store_true", help="Freeze inputs/configuration without building or starting a model/planner.")
    parser.add_argument("--skip-build", action="store_true", help="Use an already built binary; the initial-only capability check is still required.")
    parser.add_argument("--build-jobs", type=int, default=4)
    parser.add_argument("--vllm-gpus", default="0")
    parser.add_argument("--vllm-port", type=int, default=8091)
    parser.add_argument("--llm-model", default="Qwen3.5-9B")
    parser.add_argument("--vllm-executable", default="vllm")
    parser.add_argument("--planner-python", default="python3")
    parser.add_argument("--build", default="release64", help="Build profile name from build_configs.py, not a binary-directory path.")
    # Required by the shared live runtime validator. There is no Off stage.
    parser.set_defaults(stage="live")
    return parser


def resolve_inputs(args, project_root=PROJECT_ROOT):
    data_root = pathlib.Path(args.data_root).expanduser().resolve()
    dataset = pathlib.Path(args.dataset_root).expanduser().resolve() if args.dataset_root else data_root / DATASET_RELATIVE
    manual = pathlib.Path(args.prompt_domain_code).expanduser().resolve() if args.prompt_domain_code else pathlib.Path(project_root) / MANUAL_RELATIVE
    if not manual.is_file():
        raise ValueError("missing Depots prompt manual: %s; sync the bundled prompting resources with Git" % manual)
    return data_root, dataset, manual.resolve()


def output_directory(args, project_root=PROJECT_ROOT):
    _, dataset, manual = resolve_inputs(args, project_root)
    output = pathlib.Path(args.output_dir).expanduser().resolve()
    for source in (dataset, manual.parent):
        if output == source or source in output.parents:
            raise ValueError("output-dir must be outside the dataset and prompt-material directories")
    output.mkdir(parents=True, exist_ok=True)
    return output


def _json_digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False).encode("utf-8")).hexdigest()


def validate_domain(domain_path, manual_path):
    domain = domain_path.read_text(encoding="utf-8-sig")
    normalized = "".join(re.sub(r";[^\r\n]*", "", domain).lower().split())
    digest = hashlib.sha256(normalized.encode("utf-8")).hexdigest()
    if digest != DEPOTS_DOMAIN_SHA256:
        raise ValueError("dataset domain differs from the audited original Depots validation domain; verify the dataset before running this control")
    manual = manual_path.read_text(encoding="utf-8-sig")
    actions = set(re.findall(r"\(\s*:action\s+([^\s()]+)", domain.lower()))
    exposed = {name.lower() for name in re.findall(r"\baction_([A-Za-z][A-Za-z0-9_]*)\s*\(", manual)}
    if exposed != actions:
        raise ValueError("prompt manual action APIs do not match the Depots domain")
    return {"domain_name": "depot", "normalized_sha256": digest, "sha256": sha256(domain_path), "manual_sha256": sha256(manual_path), "manual_actions": sorted(exposed)}


def select_problems(dataset, output):
    domain = dataset / "domain.pddl"
    problems = dataset / "problems"
    if not domain.is_file() or not problems.is_dir():
        raise ValueError("dataset-root must contain domain.pddl and problems/: %s" % dataset)
    selected = []
    seen = set()
    for source in problems.iterdir():
        match = PROBLEM_PATTERN.fullmatch(source.name)
        if match is None or not source.is_file():
            continue
        scale, numeric_id = map(int, match.groups())
        if (scale, numeric_id) in seen:
            raise ValueError("duplicate numeric problem ID at scale %d: %d" % (scale, numeric_id))
        seen.add((scale, numeric_id))
        text = re.sub(r";[^\r\n]*", "", source.read_text(encoding="utf-8-sig"))
        declared = re.search(r"\(\s*:domain\s+([^\s()]+)\s*\)", text, re.IGNORECASE)
        if declared is None or declared.group(1).lower() != "depot":
            raise ValueError("problem does not declare the audited Depot domain: %s" % source)
        selected.append({"scale": scale, "id": numeric_id, "name": source.name, "sha256": sha256(source)})
    selected.sort(key=lambda item: (item["scale"], item["id"], item["name"]))
    counts = {str(scale): sum(item["scale"] == scale for item in selected) for scale in (30, 40)}
    if counts != EXPECTED_COUNTS:
        raise ValueError("expected the complete original scale 30/40 validation set (50 + 13 problems); found %s; check dataset-root" % counts)
    selection = {
        "schema_version": 1, "selection_policy": "all_original_validation_scale_30_40",
        "dataset_root": str(dataset), "domain_sha256": sha256(domain),
        "counts": counts, "selected": selected,
        "manifest_path": str(output / "manifest_initial_only.json"),
    }
    manifest = {
        "domain": str(domain), "problem_dir": str(problems), "default_mode": "live",
        "jobs": [{
            "id": "scale%d-id%d-initial-only" % (item["scale"], item["id"]),
            "problem": item["name"], "scale": item["scale"], "mode": "live",
            "time_limit_seconds": 1800 if item["scale"] == 30 else 3600,
            "max_requests_per_iteration": 10 if item["scale"] == 30 else 15,
        } for item in selected],
    }
    return selection, manifest


def source_fingerprints(project_root):
    sources = set((project_root / "hybrid_planner").rglob("*.py"))
    for source in (project_root / "src").rglob("*"):
        if source.is_file() and (source.suffix in (".cc", ".h", ".cmake") or source.name == "CMakeLists.txt"):
            sources.add(source)
    for name in ("build.py", "build_configs.py", "scripts/run_depots_initial_only_30_40.sh", "scripts/start_depots_initial_only_30_40_tmux.sh"):
        source = project_root / name
        if source.is_file():
            sources.add(source)
    return {source.relative_to(project_root).as_posix(): sha256(source) for source in sorted(sources)}


def _check_frozen(path, value):
    if path.exists() and json.loads(path.read_text(encoding="utf-8")) != value:
        raise ValueError("frozen %s changed; restore the original inputs/settings or use a new output directory" % path.name)


def prepare_experiment(args, project_root=PROJECT_ROOT):
    with pipeline_lock(output_directory(args, project_root)):
        return _prepare_experiment(args, project_root)


def _prepare_experiment(args, project_root):
    if args.small_parallelism < 1 or args.large_parallelism < 1 or args.build_jobs < 1:
        raise ValueError("parallelism and build-jobs must be positive")
    if not 0 <= args.experiment_seed < 2 ** 31:
        raise ValueError("experiment-seed must be in [0, 2147483647]")
    if not 1 <= args.vllm_port <= 65535:
        raise ValueError("vllm-port must be in [1, 65535]")
    if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", args.build) is None:
        raise ValueError("build must be a profile name such as release64, not a directory path")
    project_root = pathlib.Path(project_root).resolve()
    data_root, dataset, manual = resolve_inputs(args, project_root)
    output = pathlib.Path(args.output_dir).expanduser().resolve()
    selection, manifest = select_problems(dataset, output)
    domain_check = validate_domain(dataset / "domain.pddl", manual)
    model_path = str(pathlib.Path(args.model_path).expanduser().resolve())
    result_dir = output / "initial_only_live"
    config = {
        "schema_version": 1, "experiment": "depots_initial_only_lazy_30_40",
        "target_domain": "depot", "llm_query_policy": QUERY_POLICY,
        "comparison_policy": "reuse_existing_baseline_and_adaptive_results_no_other_stages",
        "data_root": str(data_root), "dataset_root": str(dataset),
        "model_path": model_path, "llm_model": args.llm_model,
        "prompt_domain_code": str(manual), "domain_reference_check": domain_check,
        "selection_sha256": _json_digest(selection), "experiment_seed": args.experiment_seed,
        "parallelism": {"30": args.small_parallelism, "40": args.large_parallelism},
        "time_limits_seconds_including_preparation": {"30": 1800, "40": 3600},
        "request_budgets_per_iteration": {"30": 10, "40": 15},
        "sampling_attempt_budget_per_iteration": {"30": 30, "40": 45},
        "initial_state_policy": "original_problem_s0_each_anytime_iteration",
        "preparation_policy": "complete_initial_requests_before_native_and_rollout_search",
        "budget_interpretation": "equal_per_iteration_sampling_cap_not_equal_total_tokens_or_calls",
        "llm_partial_only": False, "trigger_overrides": TRIGGER_SETTINGS,
        "inference": {"samples_per_state": 3, "temperature": 0.7, "top_p": 0.9, "max_tokens": 16384, "max_concurrency_per_solver": 6, "timeout_seconds": 300, "max_retries": 3},
        "runtime": {"python": sys.executable, "planner_python": args.planner_python, "build": args.build, "vllm_executable": args.vllm_executable, "vllm_gpus": args.vllm_gpus, "vllm_port": args.vllm_port},
        "source_sha256": source_fingerprints(project_root),
    }
    command = [
        sys.executable, "-m", "hybrid_planner.batch_console",
        "--manifest", selection["manifest_path"], "--output-dir", str(result_dir), "--resume",
        "--small-parallelism", str(args.small_parallelism), "--large-parallelism", str(args.large_parallelism),
        "--small-time-limit", "1800", "--large-time-limit", "3600",
        "--small-max-requests", "10", "--large-max-requests", "15",
        "--experiment-seed", str(args.experiment_seed), "--build", args.build,
        "--planner-python", args.planner_python, "--prompt-domain-code", str(manual),
        "--llm-query-policy", QUERY_POLICY,
        "--scale-aware-llm-thresholds", "--scale-30-expansion-multiplier", "0.5", "--scale-40-expansion-multiplier", "0.25",
        "--vllm-model-path", model_path, "--llm-model", args.llm_model,
        "--vllm-gpus", args.vllm_gpus, "--vllm-port", str(args.vllm_port), "--vllm-executable", args.vllm_executable,
        "--llm-samples-per-state", "3", "--llm-max-concurrency", "6",
        "--llm-temperature", "0.7", "--llm-top-p", "0.9", "--llm-max-tokens", "16384",
        "--llm-timeout", "300", "--llm-max-retries", "3",
    ]
    payloads = {
        "selection.json": selection, "manifest_initial_only.json": manifest,
        "initial_only_config.json": config, "command.json": command,
        "comparison_keys.json": [{"problem": item["name"], "scale": item["scale"], "sha256": item["sha256"]} for item in selection["selected"]],
    }
    if result_dir.exists() and not (output / "initial_only_config.json").is_file():
        raise ValueError("existing results lack initial_only_config.json; use a new output directory")
    # Check everything before committing any missing files, so a rejected
    # resume never partly overwrites the original experiment description.
    for name, value in payloads.items():
        _check_frozen(output / name, value)
    for name, value in payloads.items():
        if not (output / name).exists():
            write_json(output / name, value)
    environment = clean_environment()
    for prefix in ("HYBRID_LLM_", "NLM_LLM_"):
        environment.update({prefix + name: value for name, value in TRIGGER_SETTINGS.items()})
    environment["COUNT_LLM_QUERY_POLICY"] = QUERY_POLICY
    environment["COUNT_LLM_PARTIAL_ONLY"] = "0"
    return InitialOnlyPlan(output, project_root, result_dir, selection, config, command, environment)


def job_progress(plan):
    args = batch_console.build_argument_parser().parse_args(plan.command[3:])
    batch_console.validate_resume_experiment_seed(plan.result_dir, plan.config["experiment_seed"], False, QUERY_POLICY)
    _, jobs = batch_console.load_jobs(args)
    if len(jobs) != sum(EXPECTED_COUNTS.values()) or any(job.mode != "live" or job.scale not in (30, 40) for job in jobs):
        raise ValueError("initial-only manifest must contain exactly the original 63 scale 30/40 live jobs")
    return batch_console.partition_resumable_jobs(jobs, plan.result_dir, llm_query_policy=QUERY_POLICY)


def verify_complete(plan):
    completed, pending = job_progress(plan)
    if pending:
        raise ValueError("%d initial-only jobs are missing or failed; rerun the same command to resume" % len(pending))
    return len(completed)


def validate_prompt_pipeline(plan):
    from .prompting.builder import HybridPromptBuilder, PromptBuilderConfig, _find_balanced_section
    from .validation.response_processor import UnifiedPlanningPrefixValidator
    dataset = pathlib.Path(plan.config["dataset_root"])
    builder = HybridPromptBuilder(PromptBuilderConfig(dataset / "domain.pddl", dataset / "problems", pathlib.Path(plan.config["prompt_domain_code"])))
    builder.validate()
    validator = UnifiedPlanningPrefixValidator(dataset / "domain.pddl")
    checks = []
    for scale in (30, 40):
        item = next(item for item in plan.selection["selected"] if item["scale"] == scale)
        original = (dataset / "problems" / item["name"]).read_text(encoding="utf-8")
        start, end = _find_balanced_section(original, "init")
        built = builder.build(pathlib.Path(item["name"]).stem, original[start:end])
        parsed = validator.validate(built.runtime_problem, ())
        if parsed.goal_reached is None:
            raise ValueError("initial-state runtime PDDL could not be checked: %s" % item["name"])
        review = plan.output_dir / "prompt_preflight" / pathlib.Path(item["name"]).stem
        review.mkdir(parents=True, exist_ok=True)
        for name, contents in (("system.txt", built.system), ("user.txt", built.user), ("runtime_problem.pddl", built.runtime_problem)):
            (review / name).write_text(contents, encoding="utf-8")
        checks.append({"problem": item["name"], "scale": scale, "goal_initially_satisfied": parsed.goal_reached, "system_sha256": hashlib.sha256(built.system.encode("utf-8")).hexdigest(), "user_sha256": hashlib.sha256(built.user.encode("utf-8")).hexdigest()})
    write_json(plan.output_dir / "prompt_preflight.json", {"target_domain": "depot", "model_invoked": False, "checks": checks})


def validate_service_port(port):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(("127.0.0.1", port))
    except OSError as exc:
        raise ValueError("vLLM port %d is already in use or unavailable; stop the previous service or choose --vllm-port" % port) from exc


def build_and_validate_binary(plan, args, runner=run_batch):
    if not sys.platform.startswith("linux"):
        raise ValueError("actual execution requires Linux; --prepare-only starts no build/model/planner")
    if not args.skip_build:
        update_status(plan, status="building", current_stage="build", error=None, return_code=None)
        command = [sys.executable, str(plan.project_root / "build.py"), args.build, "-j%d" % args.build_jobs]
        write_json(plan.output_dir / "build_command.json", command)
        print("[COUNT-INITIAL-ONLY] building %s with %d compiler jobs" % (args.build, args.build_jobs), flush=True)
        return_code = runner(command, plan.environment, plan.project_root)
        if return_code != 0:
            raise ValueError("planner build failed (exit %s); fix the container build before resuming" % return_code)
    # Never silently use an old binary, including with --skip-build.
    console.validate_initial_only_binary(plan.project_root, args.build)


def execute_plan(plan, runner=run_batch, *, _lock_held=False):
    with contextlib.nullcontext() if _lock_held else pipeline_lock(plan.output_dir):
        try:
            update_status(plan, status="running", current_stage="initial_only_live", error=None, completed_jobs=None, return_code=None)
            print("[COUNT-INITIAL-ONLY] starting 63 live jobs; no baseline/adaptive stage will run", flush=True)
            result = runner(plan.command, plan.environment, plan.project_root)
            if result != 0:
                update_status(plan, status="failed", return_code=result)
                return result if result > 0 else 1
            completed = verify_complete(plan)
            update_status(plan, status="complete", current_stage=None, completed_jobs=completed, return_code=0, error=None)
            return 0
        except KeyboardInterrupt:
            update_status(plan, status="interrupted", return_code=130)
            print("[COUNT-INITIAL-ONLY] interrupted; rerun the same command to resume", flush=True)
            return 130
        except Exception as exc:
            update_status(plan, status="failed", error=str(exc))
            raise


def main(argv=None):
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        with pipeline_lock(output_directory(args)):
            plan = _prepare_experiment(args, PROJECT_ROOT)
            print("[COUNT-INITIAL-ONLY] selected=30:50,40:13 total=63 parallelism=30:%d,40:%d seed=%d" % (args.small_parallelism, args.large_parallelism, args.experiment_seed), flush=True)
            print("[COUNT-INITIAL-ONLY] manual=%s results=%s" % (plan.config["prompt_domain_code"], plan.output_dir), flush=True)
            if args.prepare_only:
                print("[COUNT-INITIAL-ONLY] prepared only; no build/model/planner started", flush=True)
                return 0
            try:
                completed, pending = job_progress(plan)
                if not pending:
                    update_status(plan, status="complete", current_stage=None, completed_jobs=len(completed), return_code=0, error=None)
                    print("[COUNT-INITIAL-ONLY] all jobs already complete; no build/model/planner started", flush=True)
                    return 0
                previous = signal.getsignal(signal.SIGTERM)
                def interrupted(signum, frame):
                    raise KeyboardInterrupt
                signal.signal(signal.SIGTERM, interrupted)
                try:
                    build_and_validate_binary(plan, args)
                    update_status(plan, status="checking", current_stage="preflight", error=None, return_code=None)
                    validate_runtime(plan, args)
                    validate_prompt_pipeline(plan)
                    validate_service_port(args.vllm_port)
                    write_json(plan.output_dir / "runtime_host.json", {"platform": platform.platform(), "cpu_count": os.cpu_count(), "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None})
                    return execute_plan(plan, _lock_held=True)
                finally:
                    signal.signal(signal.SIGTERM, previous)
            except KeyboardInterrupt:
                update_status(plan, status="interrupted", return_code=130)
                return 130
            except (OSError, ValueError, RuntimeError) as exc:
                update_status(plan, status="failed", error=str(exc))
                raise
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, RuntimeError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    sys.exit(main())
