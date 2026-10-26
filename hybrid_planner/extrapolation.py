"""Frozen Depots 45/50 experiment: native baseline, then full TRACS-Lazy."""

import argparse
import contextlib
import hashlib
import json
import os
import pathlib
import platform
import signal
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass

from . import batch_console
from .extrapolation_selection import prepare_selection


PROJECT_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_MODEL = "/root/autodl-tmp/Qwen3_5-9B/dapo/data_260811_resume_193/global_step_350/actor/huggingface"
TRIGGER_SETTINGS = {
    "REQUEST_INITIAL": "0",
    "ENABLE_EXPANSION_PLATEAU": "1",
    "ENABLE_GLOBAL_STALL": "1",
    "ENABLE_ANCESTOR_STAGNATION": "1",
    "PLATEAU_CONFIRM_WINDOWS": "2",
    "PLATEAU_MIN_SHARE": "0.25",
}
STAGE_DIRECTORIES = {"off": "baseline", "live": "tracs_lazy"}


def write_json(path, value):
    path = pathlib.Path(path)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def clean_environment(base=None):
    """Avoid inheriting old triggers, already-scaled intervals or model endpoints."""
    environment = dict(os.environ if base is None else base)
    for key in list(environment):
        if key.startswith(("HYBRID_LLM_", "NLM_LLM_", "NLM_VLLM_")):
            del environment[key]
    environment.pop("NLM_SEARCH_TIME_LIMIT_SECONDS", None)
    environment["COUNT_LLM_PARTIAL_ONLY"] = "0"
    for name, value in TRIGGER_SETTINGS.items():
        environment["HYBRID_LLM_" + name] = value
        environment["NLM_LLM_" + name] = value
    return environment


@dataclass
class ExperimentPlan:
    output_dir: pathlib.Path
    project_root: pathlib.Path
    selection: dict
    config: dict
    commands: dict
    environment: dict


def build_argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset-root", default="/root/PyPACE/data/generated-pddl/depots-numeric-45and50")
    parser.add_argument("--model-path", default=DEFAULT_MODEL)
    parser.add_argument("--output-dir", default="/root/autodl-tmp/count-results/depots45-50-extrapolation/seed_101")
    parser.add_argument("--sample-per-scale", type=int, default=20)
    parser.add_argument("--sample-seed", type=int, default=20261025)
    parser.add_argument("--experiment-seed", type=int, default=101)
    parser.add_argument("--parallelism", type=int, default=6)
    parser.add_argument("--stage", choices=("both", "off", "live"), default="both")
    parser.add_argument("--prepare-only", action="store_true", help="Freeze selections and commands without starting planners or vLLM.")
    parser.add_argument("--vllm-gpus", default="0")
    parser.add_argument("--vllm-port", type=int, default=8091)
    parser.add_argument("--llm-model", default="Qwen3.5-9B")
    parser.add_argument("--vllm-executable", default="vllm")
    parser.add_argument("--planner-python", default="python3")
    parser.add_argument("--build", default="release64")
    parser.add_argument("--prompt-domain-code", default=str(PROJECT_ROOT / "hybrid_planner/prompting/resources/depots/domain_NOassert.txt"))
    return parser


def experiment_output_directory(args):
    output_dir = pathlib.Path(args.output_dir).expanduser().resolve()
    dataset_root = pathlib.Path(args.dataset_root).expanduser().resolve()
    if output_dir == dataset_root or dataset_root in output_dir.parents:
        raise ValueError("output-dir must be outside dataset-root")
    output_dir.mkdir(parents=True, exist_ok=True)
    return output_dir


def prepare_experiment(args, project_root=PROJECT_ROOT):
    with pipeline_lock(experiment_output_directory(args)):
        return _prepare_experiment(args, project_root)


def _prepare_experiment(args, project_root):
    if args.parallelism < 1 or args.sample_per_scale < 1:
        raise ValueError("parallelism and sample-per-scale must be positive")
    if not 0 <= args.experiment_seed < 2 ** 31:
        raise ValueError("experiment-seed must be in [0, 2147483647]")
    if not 1 <= args.vllm_port <= 65535:
        raise ValueError("vllm-port must be in [1, 65535]")
    project_root = pathlib.Path(project_root).resolve()
    output_dir = pathlib.Path(args.output_dir).expanduser().resolve()
    selection = prepare_selection(args.dataset_root, output_dir, args.sample_per_scale, args.sample_seed)
    prompt_code = pathlib.Path(args.prompt_domain_code).expanduser().resolve()
    if not prompt_code.is_file():
        raise ValueError("missing Depots prompt domain code: %s" % prompt_code)
    model_path = str(pathlib.Path(args.model_path).expanduser().resolve())
    # Hash cheap source/description files, not the multi-gigabyte model weights.
    source_hashes = {}
    for source in sorted((project_root / "hybrid_planner").rglob("*.py")):
        source_hashes[source.relative_to(project_root).as_posix()] = sha256(source)
    config = {
        "schema_version": 1,
        "experiment": "depots_45_50_full_tracs_lazy",
        "dataset_root": selection["dataset_root"],
        "selection_sha256": sha256(output_dir / "selection.json"),
        "model_path": model_path,
        "llm_model": args.llm_model,
        "prompt_domain_code": str(prompt_code),
        "prompt_domain_code_sha256": sha256(prompt_code),
        "sample_seed": args.sample_seed,
        "sample_per_scale": args.sample_per_scale,
        "experiment_seed": args.experiment_seed,
        "parallelism": args.parallelism,
        "time_limit_seconds": 3600,
        "llm_partial_only": False,
        "llm_samples_per_state": 3,
        "max_requests_per_iteration": 15,
        "scale_40_expansion_multiplier": 0.25,
        "trigger_overrides": TRIGGER_SETTINGS,
        "inference": {"temperature": 0.7, "top_p": 0.9, "max_tokens": 16384, "max_concurrency_per_solver": 6, "timeout_seconds": 300},
        "runtime": {"python": sys.executable, "planner_python": args.planner_python, "build": args.build, "vllm_executable": args.vllm_executable, "vllm_gpus": args.vllm_gpus, "vllm_port": args.vllm_port},
        "source_sha256": source_hashes,
    }
    config_path = output_dir / "experiment_config.json"
    if config_path.exists():
        if json.loads(config_path.read_text(encoding="utf-8")) != config:
            raise ValueError("experiment configuration changed; use the original settings or a new output directory")
    else:
        if any((output_dir / name).exists() for name in STAGE_DIRECTORIES.values()):
            raise ValueError("existing stage results lack experiment_config.json; use a new output directory")
        write_json(config_path, config)

    commands = {}
    for stage in ("off", "live"):
        command = [sys.executable, "-m", "hybrid_planner.batch_console", "--manifest", selection["manifest_paths"][stage], "--output-dir", str(output_dir / STAGE_DIRECTORIES[stage]), "--resume", "--large-parallelism", str(args.parallelism), "--large-time-limit", "3600", "--large-max-requests", "15", "--experiment-seed", str(args.experiment_seed), "--build", args.build, "--planner-python", args.planner_python, "--prompt-domain-code", str(prompt_code), "--scale-aware-llm-thresholds", "--scale-40-expansion-multiplier", "0.25"]
        if stage == "live":
            command += ["--vllm-model-path", model_path, "--llm-model", args.llm_model, "--vllm-gpus", args.vllm_gpus, "--vllm-port", str(args.vllm_port), "--vllm-executable", args.vllm_executable, "--llm-samples-per-state", "3", "--llm-max-concurrency", "6", "--llm-temperature", "0.7", "--llm-top-p", "0.9", "--llm-max-tokens", "16384", "--llm-timeout", "300", "--llm-max-retries", "3"]
        commands[stage] = command
    write_json(output_dir / "commands.json", commands)
    return ExperimentPlan(output_dir, project_root, selection, config, commands, clean_environment())


def verify_stage_complete(plan, stage):
    args = batch_console.build_argument_parser().parse_args(plan.commands[stage][3:])
    batch_console.validate_resume_experiment_seed(
        plan.output_dir / STAGE_DIRECTORIES[stage], plan.config["experiment_seed"], False
    )
    _, jobs = batch_console.load_jobs(args)
    completed, pending = batch_console.partition_resumable_jobs(jobs, plan.output_dir / STAGE_DIRECTORIES[stage])
    if pending:
        raise ValueError("%s stage has %d missing or failed jobs; rerun to resume before continuing" % (stage, len(pending)))
    return len(completed)


def update_status(plan, **values):
    path = plan.output_dir / "pipeline_status.json"
    record = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    record.update(values)
    record["updated_at_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    write_json(path, record)


@contextlib.contextmanager
def pipeline_lock(output_dir):
    """An OS lock is released even if a container/process is killed."""
    with (output_dir / "pipeline.lock").open("a+b") as stream:
        try:
            if os.name == "nt":
                import msvcrt
                stream.seek(0)
                if not stream.read(1):
                    stream.write(b"0")
                    stream.flush()
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ValueError("another process is using this experiment directory") from exc
        try:
            yield
        finally:
            if os.name == "nt":
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def signal_process_group(process_group, signum):
    try:
        os.killpg(process_group, signum)
        return True
    except ProcessLookupError:
        return False


def wait_process_group(process, timeout):
    deadline = time.monotonic() + timeout
    while True:
        process.poll()  # Reap the leader even if grandchildren are still alive.
        if not signal_process_group(process.pid, 0):
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.1)


def stop_process_group(process):
    # The leader may already have exited after OOM or SIGINT. Its descendants
    # still belong to our private group and must not keep consuming resources.
    for signum, timeout in ((signal.SIGINT, 20), (signal.SIGTERM, 10)):
        if not signal_process_group(process.pid, signum):
            break
        if wait_process_group(process, timeout):
            break
    else:
        signal_process_group(process.pid, signal.SIGKILL)
    process.wait()


def run_batch(command, environment, project_root):
    process = subprocess.Popen(command, cwd=str(project_root), env=environment, start_new_session=True)
    try:
        return_code = process.wait()
    except BaseException:
        stop_process_group(process)
        raise
    # Also clean up after an externally killed batch or an orphaned worker.
    if signal_process_group(process.pid, 0):
        stop_process_group(process)
    return return_code


def execute_plan(plan, stage="both", runner=run_batch, *, _lock_held=False):
    stages = ("off", "live") if stage == "both" else (stage,)
    with contextlib.nullcontext() if _lock_held else pipeline_lock(plan.output_dir):
        try:
            for current in stages:
                if current == "live":
                    verify_stage_complete(plan, "off")
                update_status(plan, status="running", current_stage=current)
                print("[COUNT-EXTRAPOLATION] starting stage=%s jobs=%d parallelism=%d limit=3600s partial_only=0" % (current, len(plan.selection["selected"]), plan.config["parallelism"]), flush=True)
                return_code = runner(plan.commands[current], plan.environment, plan.project_root)
                if return_code != 0:
                    update_status(plan, status="failed", current_stage=current, return_code=return_code)
                    return return_code if return_code > 0 else 1
                verify_stage_complete(plan, current)
                update_status(plan, **{current + "_complete": True})
            update_status(plan, status="complete" if stage in ("both", "live") else "baseline_complete", current_stage=None, return_code=0)
            return 0
        except KeyboardInterrupt:
            update_status(plan, status="interrupted", return_code=130)
            print("[COUNT-EXTRAPOLATION] interrupted; rerun the same command to resume", flush=True)
            return 130
        except Exception as exc:
            update_status(plan, status="failed", error=str(exc))
            raise


def validate_runtime(plan, args):
    if not sys.platform.startswith("linux"):
        raise ValueError("actual execution requires Linux; use --prepare-only for local inspection")
    binary = plan.project_root / "builds" / args.build / "bin/downward"
    if not binary.is_file():
        raise ValueError("missing compiled planner: %s" % binary)
    if not shutil.which(args.planner_python, path=plan.environment.get("PATH")):
        raise ValueError("missing planner Python executable: %s" % args.planner_python)
    if args.stage in ("both", "live"):
        if not pathlib.Path(plan.config["model_path"]).is_dir():
            raise ValueError("missing model directory: %s" % plan.config["model_path"])
        if not shutil.which(args.vllm_executable, path=plan.environment.get("PATH")):
            raise ValueError("missing vLLM executable: %s" % args.vllm_executable)
    fingerprint = {"planner_sha256": sha256(binary)}
    if args.stage in ("both", "live"):
        model_root = pathlib.Path(plan.config["model_path"])
        fingerprint["model_description_sha256"] = {
            name: sha256(model_root / name)
            for name in ("config.json", "tokenizer_config.json", "model.safetensors.index.json")
            if (model_root / name).is_file()
        }
    fingerprint_path = plan.output_dir / "execution_fingerprint.json"
    stored = json.loads(fingerprint_path.read_text(encoding="utf-8")) if fingerprint_path.exists() else {}
    for key, value in fingerprint.items():
        if key in stored and stored[key] != value:
            raise ValueError("runtime %s changed; use a new output directory" % key)
    write_json(fingerprint_path, dict(stored, **fingerprint))


def main(argv=None):
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    try:
        with pipeline_lock(experiment_output_directory(args)):
            plan = _prepare_experiment(args, PROJECT_ROOT)
            print("[COUNT-EXTRAPOLATION] selected=45:%d,50:%d total=%d seed=%d" % (args.sample_per_scale, args.sample_per_scale, len(plan.selection["selected"]), args.experiment_seed), flush=True)
            print("[COUNT-EXTRAPOLATION] results=%s" % plan.output_dir, flush=True)
            if args.prepare_only:
                print("[COUNT-EXTRAPOLATION] prepared only; no planner/model started", flush=True)
                return 0
            validate_runtime(plan, args)
            write_json(plan.output_dir / "runtime_host.json", {"platform": platform.platform(), "cpu_count": os.cpu_count(), "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None})
            previous = signal.getsignal(signal.SIGTERM)
            def interrupted(signum, frame):
                raise KeyboardInterrupt
            signal.signal(signal.SIGTERM, interrupted)
            try:
                return execute_plan(plan, args.stage, _lock_held=True)
            finally:
                signal.signal(signal.SIGTERM, previous)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    sys.exit(main())
