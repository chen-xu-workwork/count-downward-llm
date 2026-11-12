"""Rerun only the eleven audited initial-only jobs in a separate directory."""

import csv
import json
import pathlib
import sys

from . import initial_only


REPAIR_VERSION = "iteration-lifecycle-v1"
DEFAULT_ORIGINAL = "/root/autodl-tmp/count-results/depots-initial-only-30-40/seed_101"
DEFAULT_OUTPUT = "/root/autodl-tmp/count-results/depots-initial-only-rerun-11/seed_101"
# Counts refer to requests rejected before any model generation in the
# preserved 2026-11-11 run, not ordinary cancellations at the overall deadline.
AFFECTED_REQUESTS = {
    (30, 54): 5, (30, 316): 16, (30, 463): 4, (30, 481): 1,
    (30, 486): 3, (30, 718): 2, (30, 729): 2, (30, 737): 5,
    (30, 752): 6, (30, 989): 4, (40, 140): 2,
}


def read_json(path):
    return json.loads(pathlib.Path(path).read_text(encoding="utf-8-sig"))


def basename(value):
    return str(value).replace("\\", "/").rsplit("/", 1)[-1]


def validate_separate_output(original, output):
    original = pathlib.Path(original).expanduser().resolve()
    output = pathlib.Path(output).expanduser().resolve()
    if original == output or original in output.parents or output in original.parents:
        raise ValueError("rerun output and original results must be separate, non-nested directories")
    if not original.is_dir():
        raise ValueError("missing original result directory: %s; set --rerun-from / COUNT_ORIGINAL_RESULTS_DIR" % original)
    return original


def validate_repair_version():
    from . import anytime
    if getattr(anytime, "ITERATION_SYNC_VERSION", None) != REPAIR_VERSION:
        raise ValueError("rerun requires the Python iteration-lifecycle fix; sync the complete checkout")


def _validate_original_config(original_config, config):
    if original_config.get("experiment") != "depots_initial_only_lazy_30_40":
        raise ValueError("rerun-from must refer to the original full initial-only experiment")
    for name in (
        "target_domain", "llm_query_policy", "experiment_seed", "llm_model",
        "llm_partial_only", "inference", "time_limits_seconds_including_preparation",
        "request_budgets_per_iteration", "sampling_attempt_budget_per_iteration",
        "initial_state_policy", "preparation_policy",
    ):
        if original_config.get(name) != config.get(name):
            raise ValueError("rerun setting differs from original experiment: %s" % name)
    for name in ("sha256", "manual_sha256"):
        if original_config.get("domain_reference_check", {}).get(name) != config["domain_reference_check"][name]:
            raise ValueError("rerun domain or prompt manual differs from original: %s" % name)


def _validate_model_description(original, config):
    fingerprint = read_json(original / "execution_fingerprint.json")
    expected = fingerprint.get("model_description_sha256", {})
    if not expected:
        raise ValueError("original results have no model-description fingerprint")
    allowed = {"config.json", "tokenizer_config.json", "model.safetensors.index.json"}
    model = pathlib.Path(config["model_path"])
    for name, digest in expected.items():
        if name not in allowed or not (model / name).is_file():
            raise ValueError("missing model-description file required by original run: %s" % name)
        if initial_only.sha256(model / name) != digest:
            raise ValueError("model-description fingerprint differs from original: %s" % name)
    return expected


def _original_job_evidence(original, item, expected_rejected):
    job_id = "scale%d-id%d-initial-only" % (item["scale"], item["id"])
    directory = original / "initial_only_live" / job_id
    job = read_json(directory / "job.json")
    result = read_json(directory / "job_result.json")
    run = read_json(directory / "anytime/run.json")
    if (job.get("job_id") != job_id or basename(job.get("problem")) != item["name"]
            or job.get("llm_query_policy") != "initial-only"
            or run.get("llm_query_policy") != "initial-only"
            or run.get("status") != "finished" or run.get("return_code") != 0
            or result.get("status") != "plan_found" or result.get("return_code") != 0
            or run.get("incumbent_count", 0) < 1):
        raise ValueError("original job does not match the audited completed result: %s" % job_id)
    requests_path = directory / "anytime/llm_requests.csv"
    with requests_path.open(encoding="utf-8-sig", newline="") as stream:
        premature = [row["request_id"] for row in csv.DictReader(stream)
                     if row.get("status") == "stale_iteration"
                     and int(row.get("iteration") or 0) > 1
                     and int(row.get("model_generations_started") or 0) == 0
                     and "is no longer active" in row.get("error", "")]
    if len(premature) != expected_rejected:
        raise ValueError("original request evidence differs from the audited run: %s; expected %d, found %d" % (job_id, expected_rejected, len(premature)))
    return {
        "job_id": job_id, "problem": item["name"], "problem_sha256": item["sha256"],
        "original_job_dir": str(directory), "premature_request_ids": premature,
        "original_job_result_sha256": initial_only.sha256(directory / "job_result.json"),
        "original_run_sha256": initial_only.sha256(directory / "anytime/run.json"),
        "original_request_log_sha256": initial_only.sha256(requests_path),
    }


def prepare_rerun_subset(args, selection, manifest, config, result_dir):
    """Validate preserved evidence, then restrict an already audited full set."""
    validate_repair_version()
    original = validate_separate_output(args.rerun_from, result_dir.parent)
    original_config = read_json(original / "initial_only_config.json")
    original_selection = read_json(original / "selection.json")
    status = read_json(original / "pipeline_status.json")
    if status.get("status") != "complete" or status.get("completed_jobs") != 63:
        raise ValueError("rerun-from must contain the completed original 63-job experiment")
    _validate_original_config(original_config, config)
    model_description = _validate_model_description(original, config)
    if original_selection.get("counts") != {"30": 50, "40": 13}:
        raise ValueError("original selection is not the complete 50 + 13 problem set")
    original_inputs = {item["name"]: item["sha256"] for item in original_selection["selected"]}
    current_inputs = {item["name"]: item["sha256"] for item in selection["selected"]}
    if (len(original_inputs) != 63 or original_inputs != current_inputs
            or original_selection.get("domain_sha256") != selection["domain_sha256"]):
        raise ValueError("dataset contents differ from the preserved original selection")
    selected = [dict(item) for item in selection["selected"]
                if (item["scale"], item["id"]) in AFFECTED_REQUESTS]
    if len(selected) != 11:
        raise ValueError("audited rerun selection must contain exactly eleven problems")
    evidence = [_original_job_evidence(original, item, AFFECTED_REQUESTS[(item["scale"], item["id"])]) for item in selected]
    selected_names = {item["name"] for item in selected}
    new_selection = dict(selection, selected=selected, counts={"30": 10, "40": 1},
                         selection_policy="fixed_eleven_premature_iteration_rejections")
    new_manifest = dict(manifest, jobs=[dict(job) for job in manifest["jobs"] if job["problem"] in selected_names])
    config.update(
        experiment="depots_initial_only_rerun_11", repair_version=REPAIR_VERSION,
        rerun_policy="complete_problem_restart_preserve_original_job_id_and_seed",
        original_results_dir=str(original), original_model_path=original_config.get("model_path"),
        original_config_sha256=initial_only.sha256(original / "initial_only_config.json"),
        original_selection_sha256=initial_only.sha256(original / "selection.json"),
        original_model_description_sha256=model_description,
        original_parallelism=original_config.get("parallelism"),
        comparison_policy="read_only_original_52_plus_completed_replacements_11",
    )
    # This is a view specification, not a merge operation. A future analysis
    # must require every replacement to finish before using the repaired view.
    view = []
    for item in selection["selected"]:
        job_id = "scale%d-id%d-initial-only" % (item["scale"], item["id"])
        replace = item["name"] in selected_names
        original_job = original / "initial_only_live" / job_id
        if not (original_job / "job_result.json").is_file() or not (original_job / "anytime/run.json").is_file():
            raise ValueError("missing preserved original job: %s" % job_id)
        view.append({"job_id": job_id, "problem": item["name"], "scale": item["scale"],
                     "problem_sha256": item["sha256"], "source": "rerun" if replace else "original",
                     "result_dir": str(result_dir / job_id if replace else original_job),
                     "original_result_dir": str(original_job)})
    replacement = {
        "schema_version": 1, "repair_version": REPAIR_VERSION,
        "original_results_dir": str(original), "rerun_results_dir": str(result_dir.parent),
        "requires_all_eleven_replacements_complete": True, "original_modified": False,
        "original_job_count": 63, "retained_original_jobs": 52, "replacement_jobs": 11,
        "evidence": evidence, "view": view,
    }
    return new_selection, new_manifest, {"replacement_manifest.json": replacement}


def _nonempty_path(value):
    if not value.strip():
        raise ValueError("original result path must not be empty")
    return value


def build_argument_parser():
    parser = initial_only.build_argument_parser()
    parser.description = __doc__
    parser.set_defaults(output_dir=DEFAULT_OUTPUT)
    parser.add_argument("--rerun-from", default=DEFAULT_ORIGINAL, type=_nonempty_path,
                        help="Read-only root of the preserved original 63-job result set.")
    return parser


def run_regression_checks(plan, args, runner=initial_only.run_batch):
    """Container-only gate, after build and before loading any model."""
    build_root = plan.project_root / "builds" / args.build
    required = (build_root / "bin/downward", build_root / "bin/preprocess",
                build_root / "bin/translate/translate.py")
    for path in required:
        if not path.is_file():
            raise ValueError("missing native test prerequisite; refusing skipped integration checks: %s" % path)
    command = [
        sys.executable, "-m", "unittest", "tests.test_anytime", "tests.test_iteration_sync",
        "tests.test_iteration_sync_http", "tests.test_initial_only_control",
        "tests.test_initial_only_integration", "tests.test_initial_only_rerun", "-v",
    ]
    environment = dict(plan.environment, COUNT_BUILD_ROOT=str(build_root))
    initial_only.write_json(plan.output_dir / "test_command.json", {
        "command": command, "COUNT_BUILD_ROOT": str(build_root),
        "model_invoked": False,
    })
    initial_only.update_status(plan, status="testing", current_stage="regression_tests", error=None)
    print("[COUNT-INITIAL-RERUN] checking iteration sync and rerun entry before model startup", flush=True)
    result = runner(command, environment, plan.project_root)
    initial_only.write_json(plan.output_dir / "test_result.json", {
        "return_code": result, "repair_version": REPAIR_VERSION,
        "native_prerequisites_present": True, "model_invoked": False,
    })
    if result != 0:
        raise ValueError("targeted regression checks failed (exit %s); no experiment/model was started" % result)


def main(argv=None):
    return initial_only.main(argv, parser=build_argument_parser())


if __name__ == "__main__":
    sys.exit(main())
