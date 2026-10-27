import importlib.util
import contextlib
import io
import json
import pathlib
import shutil
import tempfile
import unittest
from unittest import mock

from hybrid_planner import batch_console, transfer


PROBLEM = """(define (problem fixture)
  (:domain zeno-travel)
  (:objects city0 city1 - city plane0 - aircraft person0 - person)
  (:init (at plane0 city0) (at person0 city0)
    (= (fuel plane0) 100) (= (capacity plane0) 100)
    (= (distance city0 city0) 0) (= (distance city0 city1) 10)
    (= (distance city1 city0) 10) (= (distance city1 city1) 0)
    (= (slow-burn plane0) 4) (= (fast-burn plane0) 2)
    (= (onboard plane0) 0) (= (zoom-limit plane0) 1)
    (= (total-fuel-used) 0))
  (:goal (at person0 city1))
  (:metric minimize (total-fuel-used)))
"""


class TransferPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.data = self.root / "data with spaces"
        self.dataset = self.data / transfer.DATASET_RELATIVE
        (self.dataset / "problems").mkdir(parents=True)
        resource = transfer.PROJECT_ROOT / transfer.RESOURCE_RELATIVE
        shutil.copyfile(resource / "domain.pddl", self.dataset / "domain.pddl")
        self.manual = self.data / transfer.MANUAL_RELATIVE
        self.manual.parent.mkdir(parents=True)
        shutil.copyfile(resource / "domain.txt", self.manual)
        for scale in (10, 20, 30, 40):
            for number in range(1, 4):
                (self.dataset / "problems" / ("problem_scale_%d_id_%d.pddl" % (scale, number))).write_text(PROBLEM, encoding="utf-8")
        self.output = self.root / "results with spaces"
        self.flags = ["--data-root", str(self.data), "--output-dir", str(self.output),
                      "--sample-30", "2", "--sample-40", "2", "--model-path", str(self.root / "missing model")]
        self.args = transfer.build_argument_parser().parse_args(self.flags)
        fixture_availability = {
            "schema_version": 1, "target_domain": "zeno-travel",
            "baseline_archive": "fixture", "eligible_problem_names": [
                "problem_scale_%d_id_%d.pddl" % (scale, number)
                for scale in (30, 40) for number in range(1, 4)
            ],
        }
        patcher = mock.patch.object(transfer, "load_baseline_availability", return_value=fixture_availability)
        patcher.start()
        self.addCleanup(patcher.stop)

    def plan(self):
        return transfer.prepare_experiment(self.args)

    def fake_batch(self, command, environment, project_root):
        args = batch_console.build_argument_parser().parse_args(command[3:])
        _, jobs = batch_console.load_jobs(args)
        completed, pending = batch_console.partition_resumable_jobs(jobs, pathlib.Path(args.output_dir))
        for job in pending:
            self.assertEqual(job.mode, "live")
            directory = pathlib.Path(args.output_dir) / job.job_id
            directory.mkdir(parents=True, exist_ok=True)
            batch_console.write_job_result(directory, batch_console.JobResult(
                job.index, job.job_id, str(job.problem), job.mode, job.scale,
                job.time_limit_seconds, job.max_requests_per_iteration, job.exclusive,
                "incomplete", 5, job.time_limit_seconds, str(directory),
            ))
        return len(completed), len(pending)

    def test_only_target_live_jobs_with_correct_model_manual_and_budgets(self):
        plan = self.plan()
        args = batch_console.build_argument_parser().parse_args(plan.command[3:])
        _, jobs = batch_console.load_jobs(args)
        self.assertEqual({job.mode for job in jobs}, {"live"})
        self.assertEqual({job.scale for job in jobs}, {30, 40})
        self.assertEqual({(j.scale, j.time_limit_seconds, j.max_requests_per_iteration) for j in jobs}, {(30, 1800, 10), (40, 3600, 15)})
        self.assertEqual((args.small_parallelism, args.large_parallelism), (6, 6))
        self.assertEqual(args.prompt_domain_code, str(self.manual.resolve()))
        self.assertEqual(args.vllm_model_path, str((self.root / "missing model").resolve()))
        self.assertEqual(plan.config["source_training_domain"], "depots")
        self.assertEqual(plan.config["target_domain"], "zeno-travel")
        self.assertEqual(args.llm_samples_per_state, 3)
        self.assertFalse(args.llm_partial_only)
        self.assertFalse((self.output / "baseline_manifest.json").exists())
        self.assertFalse((self.output / "baseline").exists())

    def test_prepare_only_needs_no_baseline_model_planner_or_validation_dependencies(self):
        with mock.patch.object(transfer, "validate_runtime") as runtime, \
             mock.patch.object(transfer, "validate_prompt_pipeline") as prompts, \
             mock.patch.object(transfer, "execute_plan") as execute:
            self.assertEqual(transfer.main(self.flags + ["--prepare-only"]), 0)
        runtime.assert_not_called()
        prompts.assert_not_called()
        execute.assert_not_called()

    def test_domain_manual_is_checked_before_freezing_sample(self):
        self.manual.write_text("action_Lift(hoist, crate)\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "manual does not match"):
            self.plan()
        self.assertFalse((self.output / "selection.json").exists())

    def test_modified_target_domain_semantics_are_rejected(self):
        domain = self.dataset / "domain.pddl"
        domain.write_text(domain.read_text(encoding="utf-8").replace("(increase (onboard ?a) 1)", "(increase (onboard ?a) 2)"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "audited Zenotravel"):
            self.plan()

    def test_missing_explicit_manual_does_not_fall_back(self):
        self.args.prompt_domain_code = str(self.root / "not present.txt")
        with self.assertRaisesRegex(ValueError, "missing Zenotravel"):
            self.plan()

    def test_implicit_missing_external_manual_uses_bundled_reference(self):
        self.manual.unlink()
        plan = self.plan()
        self.assertEqual(plan.config["prompt_domain_code_origin"], "bundled")
        self.assertEqual(pathlib.Path(plan.config["prompt_domain_code"]).parent.name, "zenotravel")

    def test_resume_refuses_changed_model_or_manual(self):
        self.plan()
        original = (self.output / "transfer_config.json").read_bytes()
        self.args.model_path = str(self.root / "other model")
        with self.assertRaisesRegex(ValueError, "configuration changed"):
            self.plan()
        self.assertEqual((self.output / "transfer_config.json").read_bytes(), original)

    def test_lock_prevents_prepare_overwriting_running_plan(self):
        self.plan()
        original = (self.output / "command.json").read_bytes()
        with transfer.pipeline_lock(self.output):
            with self.assertRaisesRegex(ValueError, "another process"):
                self.plan()
        self.assertEqual((self.output / "command.json").read_bytes(), original)

    def test_inherited_partial_and_old_expansion_settings_do_not_contaminate_run(self):
        with mock.patch.dict(transfer.os.environ, {"COUNT_LLM_PARTIAL_ONLY": "1", "HYBRID_LLM_PARTIAL_ONLY": "1", "NLM_LLM_STALL_EXPANSIONS": "1", "CUDA_VISIBLE_DEVICES": "2"}):
            plan = self.plan()
        self.assertNotIn("HYBRID_LLM_PARTIAL_ONLY", plan.environment)
        self.assertNotIn("NLM_LLM_STALL_EXPANSIONS", plan.environment)
        self.assertEqual(plan.environment["COUNT_LLM_PARTIAL_ONLY"], "0")
        self.assertEqual(plan.environment["CUDA_VISIBLE_DEVICES"], "2")

    def test_execute_calls_one_live_batch_and_resume_keeps_completed_attempts(self):
        plan = self.plan()
        calls = []
        def runner(command, env, root):
            calls.append(self.fake_batch(command, env, root))
            return 0
        self.assertEqual(transfer.execute_plan(plan, runner=runner), 0)
        self.assertEqual(transfer.execute_plan(self.plan(), runner=runner), 0)
        self.assertEqual(calls, [(0, 4), (4, 0)])
        self.assertEqual(json.loads((self.output / "pipeline_status.json").read_text(encoding="utf-8"))["completed_jobs"], 4)
        self.assertFalse((self.output / "baseline").exists())

    def test_batch_failure_does_not_mark_experiment_complete(self):
        plan = self.plan()
        runner = mock.Mock(return_value=1)
        self.assertEqual(transfer.execute_plan(plan, runner=runner), 1)
        runner.assert_called_once()
        self.assertEqual(json.loads((self.output / "pipeline_status.json").read_text(encoding="utf-8"))["status"], "failed")

    def test_zero_exit_without_result_markers_is_not_complete(self):
        with self.assertRaisesRegex(ValueError, "missing or failed"):
            transfer.execute_plan(self.plan(), runner=mock.Mock(return_value=0))

    def test_interrupt_releases_directory_lock(self):
        self.assertEqual(transfer.execute_plan(self.plan(), runner=mock.Mock(side_effect=KeyboardInterrupt)), 130)
        with transfer.pipeline_lock(self.output):
            pass

    def test_no_stage_or_off_cli_option(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            transfer.build_argument_parser().parse_args(["--stage", "off"])

    def test_occupied_service_port_is_rejected_before_model_start(self):
        with mock.patch.object(transfer.socket, "socket") as factory:
            factory.return_value.__enter__.return_value.bind.side_effect = OSError("busy")
            with self.assertRaisesRegex(ValueError, "already in use"):
                transfer.validate_service_port(8091)

    def test_completed_cli_resume_needs_no_model_or_runtime(self):
        plan = self.plan()
        self.fake_batch(plan.command, plan.environment, plan.project_root)
        with mock.patch.object(transfer, "validate_runtime") as runtime, \
             mock.patch.object(transfer, "validate_prompt_pipeline") as prompts, \
             mock.patch.object(transfer, "execute_plan") as execute:
            self.assertEqual(transfer.main(self.flags), 0)
        runtime.assert_not_called()
        prompts.assert_not_called()
        execute.assert_not_called()

    def test_preflight_failure_status_is_written_while_directory_is_locked(self):
        original = transfer.update_status
        observed = []
        def locked_status(plan, **values):
            with self.assertRaisesRegex(ValueError, "another process"):
                with transfer.pipeline_lock(plan.output_dir):
                    self.fail("status update lost the experiment lock")
            observed.append(values["status"])
            original(plan, **values)
        with mock.patch.object(transfer, "update_status", side_effect=locked_status), \
             mock.patch.object(transfer, "validate_runtime", side_effect=ValueError("runtime fixture failure")), \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            transfer.main(self.flags)
        self.assertEqual(observed, ["checking", "failed"])

    def test_successful_retry_clears_previous_error(self):
        plan = self.plan()
        transfer.update_status(plan, status="failed", error="old error")
        def runner(command, env, root):
            self.fake_batch(command, env, root)
            return 0
        self.assertEqual(transfer.execute_plan(plan, runner=runner), 0)
        self.assertIsNone(json.loads((self.output / "pipeline_status.json").read_text(encoding="utf-8"))["error"])

    @unittest.skipUnless(importlib.util.find_spec("pddl") and importlib.util.find_spec("unified_planning"), "requires target PDDL runtime")
    def test_real_zenotravel_prompt_and_up_preflight_without_model(self):
        checks = transfer.validate_prompt_pipeline(self.plan())
        self.assertEqual([row["scale"] for row in checks], [30, 40])
        for item in self.plan().selection["selected"][:1]:
            directory = self.output / "prompt_preflight" / pathlib.Path(item["name"]).stem
            system = (directory / "system.txt").read_text(encoding="utf-8")
            user = (directory / "user.txt").read_text(encoding="utf-8")
            self.assertIn("action_Zoom", system)
            self.assertNotIn("action_Lift", system)
            self.assertIn("get_fuel(plane0) = 100", user)
            self.assertIn("get_total_fuel_used() = 0", user)


if __name__ == "__main__":
    unittest.main()
