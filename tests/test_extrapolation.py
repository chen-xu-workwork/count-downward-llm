import json
import pathlib
import signal
import tempfile
import unittest
from unittest import mock

from hybrid_planner import batch_console, extrapolation


class ExtrapolationPipelineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.dataset = self.root / "dataset with spaces"
        problems = self.dataset / "problems"
        problems.mkdir(parents=True)
        (self.dataset / "domain.pddl").write_text("(define (domain Depot))", encoding="utf-8")
        for scale in (40, 45, 50):
            for number in range(1, 5):
                (problems / ("problem_scale_%d_id_%d.pddl" % (scale, number))).write_text(
                    "(define (problem p%d_%d) (:domain Depot))" % (scale, number), encoding="utf-8"
                )
        self.output = self.root / "results with spaces"
        self.flags = ["--dataset-root", str(self.dataset), "--output-dir", str(self.output),
                      "--sample-per-scale", "2", "--model-path", str(self.root / "missing model")]
        self.args = extrapolation.build_argument_parser().parse_args(self.flags)

    def plan(self):
        return extrapolation.prepare_experiment(self.args)

    def finish_jobs(self, command, environment, project_root):
        args = batch_console.build_argument_parser().parse_args(command[3:])
        _, jobs = batch_console.load_jobs(args)
        stage_output = pathlib.Path(args.output_dir)
        completed, pending = batch_console.partition_resumable_jobs(jobs, stage_output)
        for job in pending:
            job_dir = stage_output / job.job_id
            job_dir.mkdir(parents=True, exist_ok=True)
            result = batch_console.JobResult(
                job.index, job.job_id, str(job.problem), job.mode, job.scale,
                job.time_limit_seconds, job.max_requests_per_iteration, job.exclusive,
                "timeout", 7, 3600.0, str(job_dir),
            )
            batch_console.write_job_result(job_dir, result)
        return len(completed), len(pending)

    def test_prepare_without_model_or_binary_builds_paired_full_lazy_commands(self):
        plan = self.plan()
        selected = {}
        for stage, command in plan.commands.items():
            args = batch_console.build_argument_parser().parse_args(command[3:])
            _, jobs = batch_console.load_jobs(args)
            selected[stage] = [job.problem for job in jobs]
            self.assertEqual({job.scale for job in jobs}, {45, 50})
            self.assertEqual({job.time_limit_seconds for job in jobs}, {3600})
            self.assertEqual(args.large_parallelism, 6)
            self.assertFalse(args.llm_partial_only)
            self.assertTrue(args.resume)
            self.assertEqual(args.experiment_seed, 101)
            self.assertEqual(args.llm_samples_per_state, 3)
            self.assertNotIn("--external-vllm", command)
            self.assertEqual({job.mode for job in jobs}, {stage})
        self.assertEqual(selected["off"], selected["live"])
        self.assertFalse(plan.config["llm_partial_only"])

    def test_cli_prepare_only_never_starts_runtime(self):
        with mock.patch.object(extrapolation, "validate_runtime") as validate, \
             mock.patch.object(extrapolation, "execute_plan") as execute:
            self.assertEqual(extrapolation.main(self.flags + ["--prepare-only"]), 0)
        validate.assert_not_called()
        execute.assert_not_called()
        self.assertTrue((self.output / "selection.json").is_file())

    def test_configuration_change_refuses_to_mix_results(self):
        self.plan()
        original = (self.output / "experiment_config.json").read_bytes()
        self.args.parallelism = 5
        with self.assertRaisesRegex(ValueError, "configuration changed"):
            self.plan()
        self.assertEqual((self.output / "experiment_config.json").read_bytes(), original)

    def test_environment_cleans_old_ablation_and_intervals(self):
        env = extrapolation.clean_environment({
            "HYBRID_LLM_PARTIAL_ONLY": "1", "NLM_LLM_TRIGGER": "0",
            "HYBRID_LLM_STALL_EXPANSIONS": "123", "NLM_VLLM_BASE_URL": "old",
            "NLM_SEARCH_TIME_LIMIT_SECONDS": "5", "COUNT_LLM_PARTIAL_ONLY": "1",
            "CUDA_VISIBLE_DEVICES": "2", "LD_LIBRARY_PATH": "/libraries",
        })
        self.assertNotIn("HYBRID_LLM_PARTIAL_ONLY", env)
        self.assertNotIn("NLM_LLM_TRIGGER", env)
        self.assertNotIn("HYBRID_LLM_STALL_EXPANSIONS", env)
        self.assertNotIn("NLM_VLLM_BASE_URL", env)
        self.assertNotIn("NLM_SEARCH_TIME_LIMIT_SECONDS", env)
        self.assertEqual(env["COUNT_LLM_PARTIAL_ONLY"], "0")
        self.assertEqual(env["HYBRID_LLM_PLATEAU_CONFIRM_WINDOWS"], "2")
        self.assertEqual(env["CUDA_VISIBLE_DEVICES"], "2")
        self.assertEqual(env["LD_LIBRARY_PATH"], "/libraries")

    def test_complete_timeouts_are_accepted_and_second_run_has_no_pending_jobs(self):
        plan = self.plan()
        calls = []
        def runner(command, env, root):
            counts = self.finish_jobs(command, env, root)
            calls.append(("live" if "--vllm-model-path" in command else "off", counts))
            return 0
        self.assertEqual(extrapolation.execute_plan(plan, runner=runner), 0)
        self.assertEqual(calls, [("off", (0, 4)), ("live", (0, 4))])
        self.assertEqual(extrapolation.execute_plan(self.plan(), runner=runner), 0)
        self.assertEqual(calls[2:], [("off", (4, 0)), ("live", (4, 0))])
        status = json.loads((self.output / "pipeline_status.json").read_text(encoding="utf-8"))
        self.assertEqual(status["status"], "complete")

    def test_failed_baseline_stops_before_live(self):
        runner = mock.Mock(return_value=1)
        self.assertEqual(extrapolation.execute_plan(self.plan(), runner=runner), 1)
        self.assertEqual(runner.call_count, 1)

    def test_missing_markers_stop_live_even_when_batch_returns_zero(self):
        runner = mock.Mock(return_value=0)
        with self.assertRaisesRegex(ValueError, "missing or failed"):
            extrapolation.execute_plan(self.plan(), runner=runner)
        self.assertEqual(runner.call_count, 1)

    def test_interrupt_does_not_advance_and_releases_lock(self):
        plan = self.plan()
        runner = mock.Mock(side_effect=KeyboardInterrupt)
        self.assertEqual(extrapolation.execute_plan(plan, runner=runner), 130)
        self.assertEqual(runner.call_count, 1)
        with extrapolation.pipeline_lock(self.output):
            pass

    def test_live_requires_matching_completed_baseline(self):
        plan = self.plan()
        self.finish_jobs(plan.commands["off"], plan.environment, plan.project_root)
        (self.output / "baseline/batch_config.json").write_text(
            json.dumps({"experiment_seed": 999, "llm_partial_only": False}), encoding="utf-8"
        )
        runner = mock.Mock(return_value=0)
        with self.assertRaisesRegex(ValueError, "seed mismatch"):
            extrapolation.execute_plan(plan, "live", runner=runner)
        runner.assert_not_called()

    def test_prepare_is_locked_before_touching_configuration(self):
        self.plan()
        original = (self.output / "commands.json").read_bytes()
        with extrapolation.pipeline_lock(self.output):
            with self.assertRaisesRegex(ValueError, "another process"):
                self.plan()
        self.assertEqual((self.output / "commands.json").read_bytes(), original)

    def test_output_directory_inside_dataset_is_rejected_without_creation(self):
        self.args.output_dir = str(self.dataset / "new results")
        with self.assertRaisesRegex(ValueError, "outside"):
            self.plan()
        self.assertFalse((self.dataset / "new results").exists())

    def test_runtime_refuses_changed_planner_before_resuming(self):
        plan = self.plan()
        plan.project_root = self.root / "project"
        binary = plan.project_root / "builds/release64/bin/downward"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"frozen planner")
        model = pathlib.Path(plan.config["model_path"])
        model.mkdir()
        (model / "config.json").write_text("{}", encoding="utf-8")
        with mock.patch.object(extrapolation.sys, "platform", "linux"), \
             mock.patch.object(extrapolation.shutil, "which", return_value="/fake/executable"):
            extrapolation.validate_runtime(plan, self.args)
            binary.write_bytes(b"changed planner")
            with self.assertRaisesRegex(ValueError, "planner_sha256 changed"):
                extrapolation.validate_runtime(plan, self.args)

    def test_missing_vllm_is_detected_before_starting_baseline(self):
        plan = self.plan()
        plan.project_root = self.root / "project"
        binary = plan.project_root / "builds/release64/bin/downward"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"planner")
        pathlib.Path(plan.config["model_path"]).mkdir()
        with mock.patch.object(extrapolation.sys, "platform", "linux"), \
             mock.patch.object(extrapolation.shutil, "which", side_effect=["/python", None]):
            with self.assertRaisesRegex(ValueError, "missing vLLM executable"):
                extrapolation.validate_runtime(plan, self.args)
        self.assertFalse((self.output / "execution_fingerprint.json").exists())


class ProcessCleanupTests(unittest.TestCase):
    def test_cleanup_targets_descendants_after_leader_has_exited(self):
        process = mock.Mock(pid=123, returncode=-9)
        process.poll.return_value = -9
        with mock.patch.object(extrapolation, "signal_process_group", return_value=True) as send, \
             mock.patch.object(extrapolation.signal, "SIGKILL", 9, create=True), \
             mock.patch.object(extrapolation, "wait_process_group", side_effect=[False, False]):
            extrapolation.stop_process_group(process)
        self.assertEqual([call.args[1] for call in send.call_args_list],
                         [signal.SIGINT, signal.SIGTERM, getattr(signal, "SIGKILL", 9)])
        process.wait.assert_called_once()

    def test_external_kill_still_cleans_remaining_group(self):
        process = mock.Mock(pid=123)
        process.wait.return_value = -9
        with mock.patch.object(extrapolation.subprocess, "Popen", return_value=process), \
             mock.patch.object(extrapolation, "signal_process_group", return_value=True), \
             mock.patch.object(extrapolation, "stop_process_group") as stop:
            self.assertEqual(extrapolation.run_batch(["fake"], {}, pathlib.Path(".")), -9)
        stop.assert_called_once_with(process)


if __name__ == "__main__":
    unittest.main()
