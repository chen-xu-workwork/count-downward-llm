"""Frozen Depots-checkpoint to Zenotravel transfer; only the live Lazy run."""

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

from . import batch_console
from .extrapolation import (
    DEFAULT_MODEL, TRIGGER_SETTINGS, clean_environment, pipeline_lock,
    run_batch, sha256, update_status, validate_runtime, write_json,
)
from .transfer_selection import prepare_selection


PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
DATASET_RELATIVE = pathlib.Path("generated-pddl/zenotravel-numeric-validation-original")
MANUAL_RELATIVE = pathlib.Path("converted-python-style/zenotravel-numeric-segmented/validation/domain.txt")
RESOURCE_RELATIVE = pathlib.Path("hybrid_planner/prompting/resources/zenotravel")


@dataclass
class TransferPlan:
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
    parser.add_argument("--output-dir", default="/root/autodl-tmp/count-results/depots-to-zenotravel-30-40/seed_101")
    parser.add_argument("--sample-30", type=int, default=20)
    parser.add_argument("--sample-40", type=int, default=10)
    parser.add_argument("--sample-seed", type=int, default=20261027)
    parser.add_argument("--experiment-seed", type=int, default=101)
    parser.add_argument("--parallelism", type=int, default=6)
    parser.add_argument("--prepare-only", action="store_true", help="Freeze the live-only sample and configuration; start no model or planner.")
    parser.add_argument("--vllm-gpus", default="0")
    parser.add_argument("--vllm-port", type=int, default=8091)
    parser.add_argument("--llm-model", default="Qwen3.5-9B")
    parser.add_argument("--vllm-executable", default="vllm")
    parser.add_argument("--planner-python", default="python3")
    parser.add_argument("--build", default="release64")
    # Shared runtime validation uses this internal value. There is no Off stage.
    parser.set_defaults(stage="live")
    return parser


def resolve_inputs(args, project_root=PROJECT_ROOT):
    data_root = pathlib.Path(args.data_root).expanduser().resolve()
    dataset = pathlib.Path(args.dataset_root).expanduser().resolve() if args.dataset_root else data_root / DATASET_RELATIVE
    if args.prompt_domain_code:
        manual = pathlib.Path(args.prompt_domain_code).expanduser().resolve()
        origin = "explicit"
    else:
        external = data_root / MANUAL_RELATIVE
        manual = external if external.is_file() else pathlib.Path(project_root) / RESOURCE_RELATIVE / "domain.txt"
        origin = "data_root" if external.is_file() else "bundled"
    if not manual.is_file():
        raise ValueError("missing Zenotravel prompt domain code: %s" % manual)
    return data_root, dataset, manual.resolve(), origin


def normalized_pddl(text):
    return "".join(re.sub(r";[^\r\n]*", "", text).lower().split())


def validate_domain_reference(domain_path, manual_path, project_root=PROJECT_ROOT):
    """Reject another domain/version or a manual exposing different APIs."""
    reference = pathlib.Path(project_root) / RESOURCE_RELATIVE / "domain.pddl"
    if not reference.is_file():
        raise ValueError("missing bundled Zenotravel domain reference: %s" % reference)
    domain = pathlib.Path(domain_path).read_text(encoding="utf-8-sig")
    if normalized_pddl(domain) != normalized_pddl(reference.read_text(encoding="utf-8-sig")):
        raise ValueError("target domain differs from the audited Zenotravel domain; verify its action semantics before using this experiment")
    manual = pathlib.Path(manual_path).read_text(encoding="utf-8-sig")
    actions = set(re.findall(r"\(\s*:action\s+([^\s()]+)", domain.lower()))
    exposed = {name.lower() for name in re.findall(r"\baction_([A-Za-z][A-Za-z0-9_]*)\s*\(", manual)}
    queries = {"is_at", "is_in", "get_fuel", "get_distance", "get_slow_burn", "get_fast_burn", "get_capacity", "get_onboard", "get_zoom_limit", "get_total_fuel_used"}
    missing = sorted(query for query in queries if not re.search(r"\b" + query + r"\s*\(", manual))
    if exposed != actions or missing:
        raise ValueError("prompt manual does not match Zenotravel actions/queries (actions=%s; missing=%s)" % (sorted(exposed), missing))
    return {
        "domain_name": "zeno-travel",
        "domain_normalized_sha256": hashlib.sha256(normalized_pddl(domain).encode("utf-8")).hexdigest(),
        "audited_domain_sha256": sha256(reference),
        "manual_sha256": sha256(manual_path),
        "manual_actions": sorted(exposed),
    }


def output_directory(args, project_root=PROJECT_ROOT):
    _, dataset, manual, _ = resolve_inputs(args, project_root)
    output = pathlib.Path(args.output_dir).expanduser().resolve()
    for source in (dataset, manual.parent):
        if output == source or source in output.parents:
            raise ValueError("output-dir must be outside the dataset and prompt-material directories")
    output.mkdir(parents=True, exist_ok=True)
    return output


def load_baseline_availability(project_root=PROJECT_ROOT):
    path = pathlib.Path(project_root) / RESOURCE_RELATIVE / "baseline_availability.json"
    if not path.is_file():
        raise ValueError("missing bundled baseline availability list: %s" % path)
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_version") != 1 or payload.get("target_domain") != "zeno-travel":
        raise ValueError("baseline availability list must describe Zenotravel")
    return payload


def prepare_experiment(args, project_root=PROJECT_ROOT):
    with pipeline_lock(output_directory(args, project_root)):
        return _prepare_experiment(args, project_root)


def _prepare_experiment(args, project_root):
    if args.parallelism < 1:
        raise ValueError("parallelism must be positive")
    if not 0 <= args.experiment_seed < 2 ** 31:
        raise ValueError("experiment-seed must be in [0, 2147483647]")
    if not 1 <= args.vllm_port <= 65535:
        raise ValueError("vllm-port must be in [1, 65535]")
    project_root = pathlib.Path(project_root).resolve()
    data_root, dataset, manual, origin = resolve_inputs(args, project_root)
    domain = dataset / "domain.pddl"
    domain_check = validate_domain_reference(domain, manual, project_root)
    output = pathlib.Path(args.output_dir).expanduser().resolve()
    availability = load_baseline_availability(project_root)
    selection = prepare_selection(dataset, output, args.sample_30, args.sample_40, args.sample_seed, baseline_availability=availability)
    model_path = str(pathlib.Path(args.model_path).expanduser().resolve())
    result_dir = output / "transfer_live"
    config = {
        "schema_version": 1, "experiment": "depots_to_zenotravel_full_tracs_lazy",
        "source_training_domain": "depots", "target_domain": "zeno-travel",
        "baseline_policy": "reuse_existing_results_no_baseline_execution",
        "baseline_availability_sha256": hashlib.sha256(json.dumps(availability, sort_keys=True).encode("utf-8")).hexdigest(),
        "data_root": str(data_root), "dataset_root": str(dataset),
        "model_path": model_path, "llm_model": args.llm_model,
        "prompt_domain_code": str(manual), "prompt_domain_code_origin": origin,
        "domain_reference_check": domain_check,
        "selection_sha256": sha256(output / "selection.json"),
        "sample_30": args.sample_30, "sample_40": args.sample_40,
        "sample_seed": args.sample_seed, "experiment_seed": args.experiment_seed,
        "parallelism": args.parallelism, "time_limits": {"30": 1800, "40": 3600},
        "request_budgets_per_iteration": {"30": 10, "40": 15},
        "expansion_multipliers": {"30": 0.5, "40": 0.25},
        "llm_partial_only": False, "trigger_overrides": TRIGGER_SETTINGS,
        "inference": {"samples_per_state": 3, "temperature": 0.7, "top_p": 0.9, "max_tokens": 16384, "max_concurrency_per_solver": 6, "timeout_seconds": 300, "max_retries": 3},
        "runtime": {"python": sys.executable, "planner_python": args.planner_python, "build": args.build, "vllm_executable": args.vllm_executable, "vllm_gpus": args.vllm_gpus, "vllm_port": args.vllm_port},
        "source_sha256": {source.relative_to(project_root).as_posix(): sha256(source) for source in sorted((project_root / "hybrid_planner").rglob("*.py"))},
    }
    config_path = output / "transfer_config.json"
    if config_path.exists():
        if json.loads(config_path.read_text(encoding="utf-8")) != config:
            raise ValueError("transfer configuration changed; use the original settings or a new output directory")
    else:
        if result_dir.exists():
            raise ValueError("existing live results lack transfer_config.json; use a new output directory")
        write_json(config_path, config)
    command = [
        sys.executable, "-m", "hybrid_planner.batch_console",
        "--manifest", selection["manifest_path"], "--output-dir", str(result_dir), "--resume",
        "--small-parallelism", str(args.parallelism), "--large-parallelism", str(args.parallelism),
        "--small-time-limit", "1800", "--large-time-limit", "3600",
        "--small-max-requests", "10", "--large-max-requests", "15",
        "--experiment-seed", str(args.experiment_seed), "--build", args.build,
        "--planner-python", args.planner_python, "--prompt-domain-code", str(manual),
        "--scale-aware-llm-thresholds", "--scale-30-expansion-multiplier", "0.5", "--scale-40-expansion-multiplier", "0.25",
        "--vllm-model-path", model_path, "--llm-model", args.llm_model,
        "--vllm-gpus", args.vllm_gpus, "--vllm-port", str(args.vllm_port), "--vllm-executable", args.vllm_executable,
        "--llm-samples-per-state", "3", "--llm-max-concurrency", "6",
        "--llm-temperature", "0.7", "--llm-top-p", "0.9", "--llm-max-tokens", "16384",
        "--llm-timeout", "300", "--llm-max-retries", "3",
    ]
    write_json(output / "command.json", command)
    write_json(output / "comparison_keys.json", [{"problem": item["name"], "scale": item["scale"], "sha256": item["sha256"]} for item in selection["selected"]])
    return TransferPlan(output, project_root, result_dir, selection, config, command, clean_environment())


def validate_prompt_pipeline(plan):
    """Check two target prompts and UP parsing before paying for model startup."""
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
            raise ValueError("target runtime PDDL could not be checked: %s" % item["name"])
        review = plan.output_dir / "prompt_preflight" / pathlib.Path(item["name"]).stem
        review.mkdir(parents=True, exist_ok=True)
        for name, contents in (("system.txt", built.system), ("user.txt", built.user), ("runtime_problem.pddl", built.runtime_problem)):
            (review / name).write_text(contents, encoding="utf-8")
        checks.append({"problem": item["name"], "scale": scale, "goal_initially_satisfied": parsed.goal_reached, "system_sha256": hashlib.sha256(built.system.encode("utf-8")).hexdigest(), "user_sha256": hashlib.sha256(built.user.encode("utf-8")).hexdigest()})
    write_json(plan.output_dir / "prompt_preflight.json", {"target_domain": "zeno-travel", "model_invoked": False, "checks": checks})
    return checks


def job_progress(plan):
    args = batch_console.build_argument_parser().parse_args(plan.command[3:])
    batch_console.validate_resume_experiment_seed(plan.result_dir, plan.config["experiment_seed"], False)
    _, jobs = batch_console.load_jobs(args)
    if any(job.mode != "live" or job.scale not in (30, 40) for job in jobs):
        raise ValueError("transfer manifest must contain only scale 30/40 live jobs")
    return batch_console.partition_resumable_jobs(jobs, plan.result_dir)


def verify_complete(plan):
    completed, pending = job_progress(plan)
    if pending:
        raise ValueError("%d transfer jobs are missing or failed; rerun the same command to resume" % len(pending))
    return len(completed)


def validate_service_port(port):
    # A same-alias service left by an earlier experiment must not be mistaken
    # for this run's newly launched Depots checkpoint during readiness checks.
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(("127.0.0.1", port))
    except OSError as exc:
        raise ValueError("vLLM port %d is already in use or unavailable; stop the previous service or choose --vllm-port" % port) from exc


def execute_plan(plan, runner=run_batch, *, _lock_held=False):
    with contextlib.nullcontext() if _lock_held else pipeline_lock(plan.output_dir):
        try:
            update_status(plan, status="running", current_stage="transfer_live", error=None, completed_jobs=None, return_code=None)
            print("[COUNT-TRANSFER] starting %d live jobs; no baseline will run" % len(plan.selection["selected"]), flush=True)
            result = runner(plan.command, plan.environment, plan.project_root)
            if result != 0:
                update_status(plan, status="failed", return_code=result)
                return result if result > 0 else 1
            completed = verify_complete(plan)
            update_status(plan, status="complete", current_stage=None, completed_jobs=completed, return_code=0, error=None)
            return 0
        except KeyboardInterrupt:
            update_status(plan, status="interrupted", return_code=130)
            print("[COUNT-TRANSFER] interrupted; rerun the same command to resume", flush=True)
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
            print("[COUNT-TRANSFER] selected=30:%d,40:%d total=%d parallelism=%d" % (args.sample_30, args.sample_40, len(plan.selection["selected"]), args.parallelism), flush=True)
            print("[COUNT-TRANSFER] manual=%s results=%s" % (plan.config["prompt_domain_code"], plan.output_dir), flush=True)
            for replacement in plan.selection.get("replacements", []):
                print("[COUNT-TRANSFER] baseline-matched replacement: %s -> %s" % (replacement["removed"], replacement["added"]), flush=True)
            if args.prepare_only:
                print("[COUNT-TRANSFER] prepared only; no model/planner/baseline started", flush=True)
                return 0
            try:
                completed, pending = job_progress(plan)
                if not pending:
                    update_status(plan, status="complete", current_stage=None, completed_jobs=len(completed), return_code=0, error=None)
                    print("[COUNT-TRANSFER] all selected jobs already complete; no model/planner started", flush=True)
                    return 0
                previous = signal.getsignal(signal.SIGTERM)
                def interrupted(signum, frame):
                    raise KeyboardInterrupt
                signal.signal(signal.SIGTERM, interrupted)
                try:
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
