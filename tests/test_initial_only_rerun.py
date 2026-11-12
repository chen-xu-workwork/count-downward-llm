"""Rerun selection and preservation checks; no model or planner is launched."""

import csv
import io
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from hybrid_planner import initial_only, initial_only_rerun as rerun


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


class RerunFixture:
    def __init__(self, root):
        self.original = root / "original"
        self.output = root / "rerun"
        self.model = root / "model"
        self.model.mkdir()
        (self.model / "config.json").write_text("model description", encoding="utf-8")
        self.config = {
            "experiment": "depots_initial_only_lazy_30_40", "target_domain": "depot",
            "llm_query_policy": "initial-only", "experiment_seed": 101,
            "llm_model": "Qwen3.5-9B", "model_path": str(self.model),
            "llm_partial_only": False, "inference": {"samples_per_state": 3},
            "time_limits_seconds_including_preparation": {"30": 1800, "40": 3600},
            "request_budgets_per_iteration": {"30": 10, "40": 15},
            "sampling_attempt_budget_per_iteration": {"30": 30, "40": 45},
            "initial_state_policy": "original_problem_s0_each_anytime_iteration",
            "preparation_policy": "complete_initial_requests_before_native_and_rollout_search",
            "domain_reference_check": {"sha256": "domain", "manual_sha256": "manual"},
            "parallelism": {"30": 8, "40": 4},
        }
        items = []
        for scale, count in ((30, 50), (40, 13)):
            ids = sorted({ident for s, ident in rerun.AFFECTED_REQUESTS if s == scale})
            candidate = 1000
            while len(ids) < count:
                ids.append(candidate)
                candidate += 1
            for ident in ids:
                items.append({"scale": scale, "id": ident,
                              "name": "problem_scale_%d_id_%d.pddl" % (scale, ident),
                              "sha256": "task-%d-%d" % (scale, ident)})
        self.selection = {"selected": items, "counts": {"30": 50, "40": 13},
                          "domain_sha256": "domain", "manifest_path": str(self.output / "manifest_initial_only.json")}
        self.manifest = {"default_mode": "live", "jobs": [
            {"id": "scale%d-id%d-initial-only" % (i["scale"], i["id"]),
             "problem": i["name"], "scale": i["scale"], "mode": "live",
             "time_limit_seconds": 1800 if i["scale"] == 30 else 3600,
             "max_requests_per_iteration": 10 if i["scale"] == 30 else 15} for i in items]}
        write_json(self.original / "initial_only_config.json", self.config)
        write_json(self.original / "selection.json", self.selection)
        write_json(self.original / "pipeline_status.json", {"status": "complete", "completed_jobs": 63})
        write_json(self.original / "execution_fingerprint.json", {
            "model_description_sha256": {"config.json": initial_only.sha256(self.model / "config.json")}})
        for item, job in zip(items, self.manifest["jobs"]):
            job_dir = self.original / "initial_only_live" / job["id"]
            write_json(job_dir / "job.json", dict(job, job_id=job["id"], problem="/old-container/tasks/" + item["name"], llm_query_policy="initial-only"))
            write_json(job_dir / "job_result.json", {"status": "plan_found", "return_code": 0})
            write_json(job_dir / "anytime/run.json", {"status": "finished", "return_code": 0,
                       "llm_query_policy": "initial-only", "incumbent_count": 1})
            count = rerun.AFFECTED_REQUESTS.get((item["scale"], item["id"]), 0)
            if count:
                with (job_dir / "anytime/llm_requests.csv").open("w", newline="", encoding="utf-8") as stream:
                    writer = csv.DictWriter(stream, fieldnames=("request_id", "status", "iteration", "model_generations_started", "error"))
                    writer.writeheader()
                    for number in range(count):
                        writer.writerow({"request_id": job["id"] + "-p2-0-" + str(number),
                            "status": "stale_iteration", "iteration": 2,
                            "model_generations_started": 0, "error": "iteration 2 is no longer active"})
        self.args = SimpleNamespace(rerun_from=str(self.original))

    def prepare(self):
        return rerun.prepare_rerun_subset(self.args, self.selection, self.manifest,
                                         self.config, self.output / "initial_only_live")

    def snapshot(self):
        return {p.relative_to(self.original).as_posix(): initial_only.sha256(p)
                for p in self.original.rglob("*") if p.is_file()}


class InitialOnlyRerunTests(unittest.TestCase):
    def test_only_fixed_eleven_restart_with_original_ids_and_inputs_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            f = RerunFixture(Path(directory))
            before = f.snapshot()
            selection, manifest, payloads = f.prepare()
            self.assertEqual({(i["scale"], i["id"]) for i in selection["selected"]}, set(rerun.AFFECTED_REQUESTS))
            self.assertEqual(selection["counts"], {"30": 10, "40": 1})
            self.assertEqual(len(manifest["jobs"]), 11)
            for job in manifest["jobs"]:
                self.assertEqual(job["mode"], "live")
                self.assertEqual(job["id"], "scale%d-id%s-initial-only" % (job["scale"], job["problem"].split("_id_")[1].split(".")[0]))
                self.assertEqual(job["max_requests_per_iteration"], 10 if job["scale"] == 30 else 15)
                self.assertEqual(job["time_limit_seconds"], 1800 if job["scale"] == 30 else 3600)
            view = payloads["replacement_manifest.json"]["view"]
            self.assertEqual(len(view), 63)
            self.assertEqual(sum(i["source"] == "rerun" for i in view), 11)
            self.assertEqual(sum(i["source"] == "original" for i in view), 52)
            self.assertEqual(f.config["experiment_seed"], 101)
            self.assertEqual(f.snapshot(), before)
            self.assertFalse(f.output.exists())  # No merge or result copy.

    def test_original_output_cannot_equal_contain_or_be_inside_old_results(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            original = root / "original"
            original.mkdir()
            for output in (original, original / "rerun", root):
                with self.subTest(output=output), self.assertRaisesRegex(ValueError, "non-nested"):
                    rerun.validate_separate_output(original, output)

    def test_changed_dataset_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            f = RerunFixture(Path(directory))
            f.selection["selected"][0]["sha256"] = "changed"
            with self.assertRaisesRegex(ValueError, "dataset contents"):
                f.prepare()

    def test_changed_seed_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            f = RerunFixture(Path(directory))
            f.config["experiment_seed"] = 102
            with self.assertRaisesRegex(ValueError, "experiment_seed"):
                f.prepare()

    def test_original_must_be_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            f = RerunFixture(Path(directory))
            write_json(f.original / "pipeline_status.json", {"status": "failed", "completed_jobs": 62})
            with self.assertRaisesRegex(ValueError, "completed original"):
                f.prepare()

    def test_request_evidence_must_match_audited_jobs(self):
        with tempfile.TemporaryDirectory() as directory:
            f = RerunFixture(Path(directory))
            log = f.original / "initial_only_live/scale30-id54-initial-only/anytime/llm_requests.csv"
            log.write_text("request_id,status,iteration,model_generations_started,error\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "request evidence"):
                f.prepare()

    def test_model_description_must_match_preserved_checkpoint_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            f = RerunFixture(Path(directory))
            (f.model / "config.json").write_text("different", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "model-description fingerprint"):
                f.prepare()

    def test_old_python_implementation_is_rejected(self):
        from hybrid_planner import anytime
        with mock.patch.object(anytime, "ITERATION_SYNC_VERSION", "old"):
            with self.assertRaisesRegex(ValueError, "iteration-lifecycle fix"):
                rerun.validate_repair_version()

    def test_rerun_parser_keeps_full_entry_default_separate(self):
        original = initial_only.build_argument_parser().parse_args([])
        repaired = rerun.build_argument_parser().parse_args([])
        self.assertNotEqual(original.output_dir, repaired.output_dir)
        self.assertFalse(hasattr(original, "rerun_from"))
        self.assertEqual(repaired.rerun_from, rerun.DEFAULT_ORIGINAL)
        self.assertEqual((repaired.small_parallelism, repaired.large_parallelism), (8, 4))

    def test_empty_original_path_cannot_fall_back_to_full_sixty_three_jobs(self):
        with mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                rerun.build_argument_parser().parse_args(["--rerun-from", ""])

    def test_native_test_prerequisites_cannot_silently_skip(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            plan = SimpleNamespace(project_root=root, output_dir=root, environment={})
            runner = mock.Mock()
            with self.assertRaisesRegex(ValueError, "refusing skipped"):
                rerun.run_regression_checks(plan, SimpleNamespace(build="release64"), runner)
            runner.assert_not_called()

    def test_test_failure_blocks_launch_and_build_root_is_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for relative in ("bin/downward", "bin/preprocess", "bin/translate/translate.py"):
                path = root / "builds/debug64" / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("existing tool", encoding="utf-8")
            plan = SimpleNamespace(project_root=root, output_dir=root, environment={"COUNT_BUILD_ROOT": "old"})
            runner = mock.Mock(return_value=1)
            with self.assertRaisesRegex(ValueError, "no experiment/model"):
                rerun.run_regression_checks(plan, SimpleNamespace(build="debug64"), runner)
            command, environment, cwd = runner.call_args.args
            self.assertIn("tests.test_iteration_sync_http", command)
            self.assertEqual(environment["COUNT_BUILD_ROOT"], str(root / "builds/debug64"))
            self.assertEqual(cwd, root)
            self.assertEqual(json.loads((root / "test_result.json").read_text())["return_code"], 1)


if __name__ == "__main__":
    unittest.main()
