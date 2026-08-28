#!/usr/bin/env python3
"""Long-lived vLLM owner and scale-aware Count experiment scheduler.

The batch console starts (or attaches to) one OpenAI-compatible vLLM service.
Each planning job still receives an isolated ``hybrid_planner.console`` child
process, HTTP bridge, anytime registry and artifact directory.  Small jobs may
run concurrently.  A problem above the small-scale threshold is an exclusive
barrier: all previously submitted jobs finish before it starts, and no other
job runs beside it.

Jobs can mix ``live`` and ``off`` modes in one manifest.  The vLLM process
remains available while off-mode baselines run, but those planners set
``NLM_LLM_TRIGGER=0`` and perform no prompt or bridge work.
"""

import argparse
import concurrent.futures
import csv
import json
import math
import os
import pathlib
import re
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import asdict, dataclass

from .llm.vllm_service import VLLMService, VLLMServiceConfig


SCALE_PATTERN = re.compile(r"(?:^|[_-])scale[_-](\d+)(?:[_-]|$)", re.I)


@dataclass(frozen=True)
class JobSpec:
    index: int
    job_id: str
    problem: pathlib.Path
    mode: str
    scale: int
    time_limit_seconds: float
    max_requests_per_iteration: int
    exclusive: bool


@dataclass(frozen=True)
class JobResult:
    index: int
    job_id: str
    problem: str
    mode: str
    scale: int
    time_limit_seconds: float
    max_requests_per_iteration: int
    exclusive: bool
    status: str
    return_code: int
    elapsed_seconds: float
    output_dir: str
    error: str = ""


EXPECTED_PLANNER_STATUSES = {
    0: "plan_found",
    4: "unsolvable",
    5: "incomplete",
    7: "timeout",
}


def classify_return_code(return_code, error=""):
    """Map Fast Downward's normal experiment exits to readable statuses."""

    if error:
        return "failed"
    return EXPECTED_PLANNER_STATUSES.get(return_code, "failed")


def _safe_component(value):
    normalized = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(value).strip())
    return normalized[:120] or "job"


def infer_problem_scale(problem):
    """Infer ``scale_N`` from a generated problem filename."""

    match = SCALE_PATTERN.search(pathlib.Path(problem).stem)
    if not match:
        raise ValueError(
            "cannot infer scale from %s; add an explicit 'scale' to the job"
            % problem
        )
    return int(match.group(1))


def policy_for_scale(scale, args):
    """Return the default wall-time, budget and exclusivity policy."""

    if scale <= args.small_scale_max:
        return (
            args.small_time_limit,
            args.small_max_requests,
            False,
        )
    return (
        args.large_time_limit,
        args.large_max_requests,
        True,
    )


def _resolve_path(value, base_dir):
    path = pathlib.Path(value).expanduser()
    if not path.is_absolute():
        path = base_dir / path
    return path.resolve()


def load_jobs(args):
    """Load positional jobs or a JSON manifest and apply scale policies."""

    manifest = {}
    manifest_dir = pathlib.Path.cwd()
    if args.manifest:
        manifest_path = pathlib.Path(args.manifest).expanduser().resolve()
        manifest_dir = manifest_path.parent
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ValueError("failed to read batch manifest: %s" % exc) from exc
        if not isinstance(manifest, dict):
            raise ValueError("batch manifest must contain one JSON object")

    domain_value = args.domain or manifest.get("domain", "")
    if not domain_value:
        raise ValueError("domain is required as a positional argument or manifest field")
    domain = _resolve_path(domain_value, manifest_dir)
    if not domain.is_file():
        raise ValueError("domain file does not exist: %s" % domain)

    problem_dir_value = manifest.get("problem_dir", "")
    problem_base = (
        _resolve_path(problem_dir_value, manifest_dir)
        if problem_dir_value
        else manifest_dir
    )
    raw_jobs = list(manifest.get("jobs", [])) if args.manifest else []
    raw_jobs.extend(args.problems)
    if not raw_jobs:
        raise ValueError("at least one planning problem is required")

    default_mode = manifest.get("default_mode", args.default_mode)
    jobs = []
    for index, raw_job in enumerate(raw_jobs, start=1):
        if isinstance(raw_job, str):
            values = {"problem": raw_job}
        elif isinstance(raw_job, dict):
            values = dict(raw_job)
        else:
            raise ValueError("job %d must be a path string or JSON object" % index)

        if not values.get("problem"):
            raise ValueError("job %d is missing 'problem'" % index)
        problem = _resolve_path(values["problem"], problem_base)
        if not problem.is_file():
            raise ValueError("problem file does not exist: %s" % problem)

        mode = str(values.get("mode", default_mode)).lower()
        if mode not in ("live", "off"):
            raise ValueError("job %d mode must be 'live' or 'off'" % index)
        scale_value = values.get("scale")
        scale = int(
            scale_value if scale_value is not None else infer_problem_scale(problem)
        )
        if scale < 1:
            raise ValueError("job %d scale must be positive" % index)

        default_time, default_budget, exclusive = policy_for_scale(scale, args)
        time_limit = float(values.get("time_limit_seconds", default_time))
        max_requests = int(
            values.get("max_requests_per_iteration", default_budget)
        )
        if not math.isfinite(time_limit) or time_limit <= 0:
            raise ValueError("job %d time limit must be positive and finite" % index)
        if max_requests < 0:
            raise ValueError("job %d request budget must not be negative" % index)
        if mode == "off":
            max_requests = 0

        requested_id = values.get(
            "id", "%03d_%s_%s" % (index, problem.stem, mode)
        )
        jobs.append(
            JobSpec(
                index=index,
                job_id=_safe_component(requested_id),
                problem=problem,
                mode=mode,
                scale=scale,
                time_limit_seconds=time_limit,
                max_requests_per_iteration=max_requests,
                exclusive=exclusive,
            )
        )
    job_ids = [job.job_id for job in jobs]
    duplicates = sorted(
        job_id for job_id in set(job_ids) if job_ids.count(job_id) > 1
    )
    if duplicates:
        raise ValueError("duplicate job IDs: %s" % ", ".join(duplicates))
    return domain, jobs


def build_child_command(job, domain, job_dir, args, vllm_base_url):
    """Build one isolated single-problem console invocation."""

    plan_path = job_dir / "sas_plan"
    anytime_dir = job_dir / "anytime"
    command = [
        sys.executable,
        "-m",
        "hybrid_planner.console",
        str(domain),
        str(job.problem),
        str(plan_path),
        "--problem-id",
        job.problem.stem,
        "--run-id",
        job.job_id,
        "--anytime-log-dir",
        str(anytime_dir),
        "--build",
        args.build,
        # The option name is inherited from the old Fast Downward driver.  The
        # executable is intentionally Python 3 in modern Linux containers.
        "--python2",
        args.planner_python,
        "--search-time-limit",
        "%.12g" % job.time_limit_seconds,
        "--llm-mode",
        job.mode,
        "--prompt-domain",
        str(domain),
        "--prompt-problem-dir",
        str(job.problem.parent),
    ]
    if args.prompt_domain_code:
        command.extend(["--prompt-domain-code", args.prompt_domain_code])

    if job.mode == "live":
        command.extend(
            [
                "--external-vllm",
                "--vllm-base-url",
                vllm_base_url,
                "--llm-model",
                args.llm_model,
                "--llm-max-concurrency",
                str(args.llm_max_concurrency),
                "--llm-samples-per-state",
                str(args.llm_samples_per_state),
                "--llm-max-qps",
                str(args.llm_max_qps),
                "--llm-max-retries",
                str(args.llm_max_retries),
                "--llm-timeout",
                str(args.llm_timeout),
                "--llm-temperature",
                str(args.llm_temperature),
                "--llm-top-p",
                str(args.llm_top_p),
                "--llm-max-tokens",
                str(args.llm_max_tokens),
                "--http-workers",
                str(args.http_workers),
                "--prompt-workers",
                str(args.prompt_workers),
                "--validation-workers",
                str(args.validation_workers),
                "--pending-behavior",
                args.pending_behavior,
            ]
        )
        if args.llm_extra_params:
            command.extend(["--llm-extra-params", args.llm_extra_params])
    return command


class BatchJobRunner:
    """Run child consoles, stream prefixed logs and support batch shutdown."""

    def __init__(self, domain, output_dir, args, vllm_base_url):
        self.domain = domain
        self.output_dir = output_dir
        self.args = args
        self.vllm_base_url = vllm_base_url
        self._process_lock = threading.Lock()
        self._print_lock = threading.Lock()
        self._active_processes = set()

    def _print(self, message):
        with self._print_lock:
            print(message, flush=True)

    def stop_all(self):
        with self._process_lock:
            processes = list(self._active_processes)
        for process in processes:
            if process.poll() is None:
                process.terminate()

    def __call__(self, job):
        job_dir = self.output_dir / job.job_id
        job_dir.mkdir(parents=True, exist_ok=True)
        command = build_child_command(
            job, self.domain, job_dir, self.args, self.vllm_base_url
        )
        environment = os.environ.copy()
        budget = str(job.max_requests_per_iteration)
        environment["HYBRID_LLM_MAX_REQUESTS"] = budget
        environment["NLM_LLM_MAX_REQUESTS"] = budget
        if job.mode == "live":
            environment["HYBRID_LLM_TRIGGER"] = "1"
            environment["NLM_LLM_TRIGGER"] = "1"
        else:
            environment["HYBRID_LLM_TRIGGER"] = "0"
            environment["NLM_LLM_TRIGGER"] = "0"

        (job_dir / "job.json").write_text(
            json.dumps(
                {
                    **asdict(job),
                    "problem": str(job.problem),
                    "domain": str(self.domain),
                    "command": command,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )

        started = time.monotonic()
        process = None
        error = ""
        return_code = -1
        self._print(
            "[COUNT-BATCH] start job=%s scale=%d mode=%s limit=%ss budget=%d"
            % (
                job.job_id,
                job.scale,
                job.mode,
                "%.12g" % job.time_limit_seconds,
                job.max_requests_per_iteration,
            )
        )
        try:
            with (job_dir / "console.log").open(
                "w", encoding="utf-8", buffering=1
            ) as log_file:
                process = subprocess.Popen(
                    command,
                    cwd=str(pathlib.Path(__file__).resolve().parent.parent),
                    env=environment,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    universal_newlines=True,
                    bufsize=1,
                )
                with self._process_lock:
                    self._active_processes.add(process)
                for line in process.stdout:
                    log_file.write(line)
                    with self._print_lock:
                        print(
                            "[%s] %s" % (job.job_id, line),
                            end="",
                            flush=True,
                        )
                return_code = process.wait()
        except Exception as exc:
            error = "%s: %s" % (type(exc).__name__, exc)
            self._print("[COUNT-BATCH] job=%s error=%s" % (job.job_id, error))
        finally:
            if process is not None:
                with self._process_lock:
                    self._active_processes.discard(process)

        elapsed = time.monotonic() - started
        status = classify_return_code(return_code, error)
        self._print(
            "[COUNT-BATCH] end job=%s status=%s code=%d seconds=%.3f"
            % (job.job_id, status, return_code, elapsed)
        )
        return JobResult(
            index=job.index,
            job_id=job.job_id,
            problem=str(job.problem),
            mode=job.mode,
            scale=job.scale,
            time_limit_seconds=job.time_limit_seconds,
            max_requests_per_iteration=job.max_requests_per_iteration,
            exclusive=job.exclusive,
            status=status,
            return_code=return_code,
            elapsed_seconds=elapsed,
            output_dir=str(job_dir),
            error=error,
        )


def run_scheduled_jobs(jobs, small_parallelism, run_job):
    """Run small jobs concurrently and large jobs as exclusive barriers."""

    results = []
    pending = []
    executor = concurrent.futures.ThreadPoolExecutor(
        max_workers=max(1, int(small_parallelism)),
        thread_name_prefix="count-small",
    )

    def drain_pending():
        for future in concurrent.futures.as_completed(pending):
            results.append(future.result())
        pending.clear()

    completed_normally = False
    try:
        for job in jobs:
            if job.exclusive:
                drain_pending()
                results.append(run_job(job))
            else:
                pending.append(executor.submit(run_job, job))
        drain_pending()
        completed_normally = True
    finally:
        # On interruption, propagate promptly so the caller can terminate the
        # active child consoles before waiting for worker threads to unwind.
        executor.shutdown(wait=completed_normally, cancel_futures=True)
    return sorted(results, key=lambda result: result.index)


def _write_batch_records(output_dir, domain, jobs, results, args, base_url):
    config = {
        "domain": str(domain),
        "output_dir": str(output_dir),
        "small_scale_max": args.small_scale_max,
        "small_parallelism": args.small_parallelism,
        "small_time_limit_seconds": args.small_time_limit,
        "large_time_limit_seconds": args.large_time_limit,
        "small_max_requests_per_iteration": args.small_max_requests,
        "large_max_requests_per_iteration": args.large_max_requests,
        "vllm_base_url": base_url,
        "jobs": [
            {**asdict(job), "problem": str(job.problem)} for job in jobs
        ],
    }
    (output_dir / "batch_config.json").write_text(
        json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    fields = list(JobResult.__dataclass_fields__)
    with (output_dir / "batch_results.csv").open(
        "w", encoding="utf-8", newline=""
    ) as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for result in results:
            writer.writerow(asdict(result))


def _build_vllm_service(args, output_dir):
    log_path = args.vllm_log or str(output_dir / "vllm.log")
    return VLLMService(
        VLLMServiceConfig(
            model_path=args.vllm_model_path,
            served_model_name=args.llm_model,
            host=args.vllm_host,
            port=args.vllm_port,
            api_base_url=args.vllm_base_url,
            gpus=args.vllm_gpus,
            executable=args.vllm_executable,
            tensor_parallel_size=args.vllm_tensor_parallel_size,
            gpu_memory_utilization=args.vllm_gpu_memory_utilization,
            max_model_len=args.vllm_max_model_len,
            dtype=args.vllm_dtype,
            trust_remote_code=args.vllm_trust_remote_code,
            omp_num_threads=args.vllm_omp_threads,
            startup_timeout=args.vllm_startup_timeout,
            poll_interval=args.vllm_poll_interval,
            log_path=log_path,
            extra_args=tuple(args.vllm_extra_arg),
        )
    )


def build_argument_parser():
    parser = argparse.ArgumentParser(
        description=(
            "Run multiple Count problems while keeping one vLLM service alive."
        )
    )
    parser.add_argument("domain", nargs="?", help="Shared domain PDDL")
    parser.add_argument("problems", nargs="*", help="Problem PDDL files")
    parser.add_argument("--manifest", default="", help="JSON batch manifest")
    parser.add_argument("--default-mode", choices=("live", "off"), default="live")
    parser.add_argument("--output-dir", default="")

    parser.add_argument("--small-scale-max", type=int, default=30)
    parser.add_argument("--small-parallelism", type=int, default=2)
    parser.add_argument("--small-time-limit", type=float, default=1800.0)
    parser.add_argument("--large-time-limit", type=float, default=3600.0)
    parser.add_argument("--small-max-requests", type=int, default=10)
    parser.add_argument("--large-max-requests", type=int, default=15)

    parser.add_argument("--build", default="release64")
    parser.add_argument("--planner-python", default="python3")
    parser.add_argument("--prompt-domain-code", default="")
    parser.add_argument("--pending-behavior", default="normal")
    parser.add_argument("--http-workers", type=int, default=0)
    parser.add_argument("--prompt-workers", type=int, default=4)
    parser.add_argument("--validation-workers", type=int, default=4)

    parser.add_argument("--llm-model", default="Qwen3.5-9B")
    parser.add_argument("--llm-max-concurrency", type=int, default=6)
    parser.add_argument("--llm-samples-per-state", type=int, default=3)
    parser.add_argument("--llm-max-qps", type=float, default=0.0)
    parser.add_argument("--llm-max-retries", type=int, default=3)
    parser.add_argument("--llm-timeout", type=float, default=300.0)
    parser.add_argument("--llm-temperature", type=float, default=0.7)
    parser.add_argument("--llm-top-p", type=float, default=0.9)
    parser.add_argument("--llm-max-tokens", type=int, default=16384)
    parser.add_argument("--llm-extra-params", default="")

    parser.add_argument("--vllm-model-path", default=os.environ.get("NLM_VLLM_MODEL_PATH", ""))
    parser.add_argument("--vllm-base-url", default=os.environ.get("NLM_VLLM_BASE_URL", ""))
    parser.add_argument("--vllm-host", default=os.environ.get("NLM_VLLM_HOST", "127.0.0.1"))
    parser.add_argument("--vllm-port", type=int, default=int(os.environ.get("NLM_VLLM_PORT", "8091")))
    parser.add_argument("--vllm-gpus", default=os.environ.get("NLM_VLLM_GPUS", ""))
    parser.add_argument("--vllm-executable", default=os.environ.get("NLM_VLLM_EXECUTABLE", "vllm"))
    parser.add_argument("--vllm-command", default="")
    parser.add_argument("--external-vllm", action="store_true")
    parser.add_argument("--vllm-tensor-parallel-size", type=int, default=1)
    parser.add_argument("--vllm-gpu-memory-utilization", type=float, default=0.90)
    parser.add_argument("--vllm-max-model-len", type=int, default=32768)
    parser.add_argument("--vllm-dtype", default="bfloat16")
    parser.add_argument(
        "--vllm-trust-remote-code",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    parser.add_argument("--vllm-omp-threads", type=int, default=2)
    parser.add_argument("--vllm-startup-timeout", type=float, default=1200.0)
    parser.add_argument("--vllm-poll-interval", type=float, default=2.0)
    parser.add_argument("--vllm-log", default="")
    parser.add_argument("--vllm-extra-arg", action="append", default=[])
    return parser


def main():
    parser = build_argument_parser()
    args = parser.parse_args()
    if args.small_scale_max < 1:
        parser.error("--small-scale-max must be positive")
    if args.small_parallelism < 1:
        parser.error("--small-parallelism must be positive")
    if args.small_max_requests < 0 or args.large_max_requests < 0:
        parser.error("request budgets must not be negative")
    if args.llm_samples_per_state < 1 or args.llm_max_concurrency < 1:
        parser.error("LLM samples and concurrency must be positive")

    try:
        domain, jobs = load_jobs(args)
    except ValueError as exc:
        parser.error(str(exc))
    live_jobs = [job for job in jobs if job.mode == "live"]
    if live_jobs and not args.external_vllm:
        if not args.vllm_model_path and not args.vllm_command:
            parser.error(
                "live jobs require --vllm-model-path, --vllm-command, "
                "or --external-vllm"
            )

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    output_dir = (
        pathlib.Path(args.output_dir).expanduser().resolve()
        if args.output_dir
        else pathlib.Path.cwd() / "logs" / "batch" / timestamp
    )
    output_dir.mkdir(parents=True, exist_ok=True)

    service = None
    runner = None
    results = []
    base_url = ""
    try:
        if live_jobs:
            service = _build_vllm_service(args, output_dir)
            if not args.external_vllm:
                override = shlex.split(args.vllm_command) if args.vllm_command else None
                command = override or service.build_command()
                print(
                    "[COUNT-BATCH] launching one persistent vLLM: %s"
                    % " ".join(command),
                    flush=True,
                )
                service.start(command_override=override)
            print(
                "[COUNT-BATCH] waiting for vLLM at %s" % service.config.base_url,
                flush=True,
            )
            models = service.wait_until_ready()
            base_url = service.config.base_url
            print(
                "[COUNT-BATCH] vLLM ready models=%s" % ",".join(models),
                flush=True,
            )
        else:
            print(
                "[COUNT-BATCH] all jobs are off-mode; vLLM will not be started",
                flush=True,
            )

        _write_batch_records(output_dir, domain, jobs, [], args, base_url)
        runner = BatchJobRunner(domain, output_dir, args, base_url)
        results = run_scheduled_jobs(jobs, args.small_parallelism, runner)
    except KeyboardInterrupt:
        print("[COUNT-BATCH] interrupted; stopping active planners", flush=True)
        if runner is not None:
            runner.stop_all()
    finally:
        if service is not None:
            service.stop()
        _write_batch_records(output_dir, domain, jobs, results, args, base_url)

    failures = [result for result in results if result.status == "failed"]
    print(
        "[COUNT-BATCH] complete jobs=%d failures=%d results=%s"
        % (len(results), len(failures), output_dir / "batch_results.csv"),
        flush=True,
    )
    return 1 if failures or len(results) != len(jobs) else 0


if __name__ == "__main__":
    sys.exit(main())
