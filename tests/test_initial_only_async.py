"""Async experiment entry checks; never launch a model or native planner."""

import io
import json
import os
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from hybrid_planner import initial_only, initial_only_async as async_entry


class AsyncFixture:
    def __init__(self, root):
        self.root = root
        self.dataset = root / "data/generated-pddl/depots-numeric-validation-original"
        (self.dataset / "problems").mkdir(parents=True)
        (self.dataset / "domain.pddl").write_text("(define (domain depot))", encoding="utf-8")
        self.manual = root / "prompt-materials" / "domain_NOassert.txt"
        self.manual.parent.mkdir()
        self.manual.write_text("test manual", encoding="utf-8")
        for scale, count in ((30, 50), (40, 13)):
            for number in range(1, count + 1):
                (self.dataset / "problems" / ("problem_scale_%d_id_%d.pddl" % (scale, number))).write_text(
                    "(define (problem p) (:domain depot))", encoding="utf-8")
        self.output = root / "async-results"
        self.argv = ["--dataset-root", str(self.dataset), "--prompt-domain-code", str(self.manual),
                     "--model-path", str(root / "model"), "--output-dir", str(self.output)]

    def args(self, asynchronous=True):
        parser = async_entry.build_argument_parser() if asynchronous else initial_only.build_argument_parser()
        return parser.parse_args(self.argv)

    def prepare(self, asynchronous=True):
        with mock.patch.object(initial_only, "validate_domain", return_value={"sha256": "domain", "manual_sha256": "manual"}):
            return initial_only.prepare_experiment(self.args(asynchronous), self.root)


class InitialOnlyAsyncEntryTests(unittest.TestCase):
    def test_async_default_is_new_output_while_original_entry_stays_serial(self):
        serial = initial_only.build_argument_parser().parse_args([])
        asynchronous = async_entry.build_argument_parser().parse_args([])
        self.assertFalse(getattr(serial, "llm_initial_async", False))
        self.assertTrue(asynchronous.llm_initial_async)
        self.assertNotEqual(serial.output_dir, asynchronous.output_dir)
        self.assertEqual((asynchronous.small_parallelism, asynchronous.large_parallelism), (8, 4))

    def test_full_sixty_three_jobs_keep_ids_budgets_and_enable_async_explicitly(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            with mock.patch.dict(os.environ, {"HYBRID_LLM_INITIAL_ASYNC": "0", "NLM_LLM_INITIAL_ASYNC": "0"}):
                plan = f.prepare()
            manifest = json.loads((f.output / "manifest_initial_only.json").read_text())
            self.assertEqual(plan.selection["counts"], {"30": 50, "40": 13})
            self.assertEqual(len(manifest["jobs"]), 63)
            self.assertTrue(all(job["mode"] == "live" for job in manifest["jobs"]))
            self.assertEqual(manifest["jobs"][0]["id"], "scale30-id1-initial-only")
            for job in manifest["jobs"]:
                self.assertEqual(job["time_limit_seconds"], 1800 if job["scale"] == 30 else 3600)
                self.assertEqual(job["max_requests_per_iteration"], 10 if job["scale"] == 30 else 15)
            self.assertIn("--llm-initial-async", plan.command)
            self.assertEqual(plan.config["experiment"], async_entry.EXPERIMENT)
            self.assertEqual(plan.config["initial_async_protocol"], async_entry.PROTOCOL_VERSION)
            self.assertEqual(plan.config["experiment_seed"], 101)
            self.assertTrue(plan.config["llm_initial_async"])
            for prefix in ("HYBRID_LLM_", "NLM_LLM_"):
                self.assertEqual(plan.environment[prefix + "INITIAL_ASYNC"], "1")
                self.assertEqual(plan.environment[prefix + "QUERY_POLICY"], "initial-only")
                self.assertEqual(plan.environment[prefix + "ENABLE_GLOBAL_STALL"], "0")

    def test_serial_entry_cannot_inherit_async_from_shell(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            with mock.patch.dict(os.environ, {"HYBRID_LLM_INITIAL_ASYNC": "1", "NLM_LLM_INITIAL_ASYNC": "1"}):
                plan = f.prepare(asynchronous=False)
            self.assertNotIn("--llm-initial-async", plan.command)
            self.assertFalse(plan.config["llm_initial_async"])
            self.assertEqual(plan.environment["HYBRID_LLM_INITIAL_ASYNC"], "0")
            self.assertEqual(plan.environment["NLM_LLM_INITIAL_ASYNC"], "0")

    def test_serial_output_and_its_descendants_are_read_only_to_async_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            original = Path(directory) / "serial"
            original.mkdir()
            path = original / "initial_only_config.json"
            contents = '{"experiment":"depots_initial_only_lazy_30_40"}'
            path.write_text(contents, encoding="utf-8")
            for output in (original, original / "child"):
                with self.assertRaises(ValueError):
                    async_entry.validate_async_output(output)
            self.assertEqual(path.read_text(), contents)
            self.assertFalse((original / "child").exists())

    def test_async_config_is_frozen_and_rejects_serial_relabeling(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            first = f.prepare()
            stored = (f.output / "initial_only_config.json").read_bytes()
            self.assertEqual(f.prepare().config, first.config)
            with self.assertRaisesRegex(ValueError, "frozen"):
                f.prepare(asynchronous=False)
            self.assertEqual((f.output / "initial_only_config.json").read_bytes(), stored)

    def test_eleven_job_serial_repair_cannot_be_combined_with_async(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            args = f.args()
            args.rerun_from = str(f.root / "old")
            with self.assertRaisesRegex(ValueError, "new full experiment"):
                initial_only._prepare_experiment(args, f.root)

    def test_missing_iteration_sync_fix_is_rejected(self):
        from hybrid_planner import anytime
        with mock.patch.object(anytime, "ITERATION_SYNC_VERSION", "old"):
            with self.assertRaisesRegex(ValueError, "iteration-lifecycle fix"):
                async_entry.configure_async_experiment({})
        with mock.patch.object(anytime, "ITERATION_ASYNC_SYNC_VERSION", "old"):
            with self.assertRaisesRegex(ValueError, "immediate old-phase cancellation"):
                async_entry.configure_async_experiment({})

    def test_prepare_only_never_builds_runs_tests_or_starts_model(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            with mock.patch.object(initial_only, "PROJECT_ROOT", f.root), \
                    mock.patch.object(initial_only, "validate_domain", return_value={}), \
                    mock.patch.object(initial_only, "build_and_validate_binary") as build, \
                    mock.patch.object(async_entry, "validate_existing_binary") as binary, \
                    mock.patch("hybrid_planner.extrapolation.subprocess.Popen") as subprocess, \
                    mock.patch.object(initial_only, "execute_plan") as execute, \
                    mock.patch("sys.stdout", new_callable=io.StringIO):
                result = async_entry.main(f.argv + ["--prepare-only"])
            self.assertEqual(result, 0)
            build.assert_not_called(); binary.assert_not_called(); subprocess.assert_not_called(); execute.assert_not_called()

    def test_completed_batch_does_not_build_test_or_load_model_again(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            with mock.patch.object(initial_only, "PROJECT_ROOT", f.root), \
                    mock.patch.object(initial_only, "validate_domain", return_value={}), \
                    mock.patch.object(initial_only, "job_progress", return_value=([None] * 63, [])), \
                    mock.patch.object(initial_only, "build_and_validate_binary") as build, \
                    mock.patch.object(async_entry, "validate_existing_binary") as binary, \
                    mock.patch("hybrid_planner.extrapolation.subprocess.Popen") as subprocess, \
                    mock.patch.object(initial_only, "execute_plan") as execute, \
                    mock.patch("sys.stdout", new_callable=io.StringIO):
                result = async_entry.main(f.argv)
            self.assertEqual(result, 0)
            build.assert_not_called(); binary.assert_not_called(); subprocess.assert_not_called(); execute.assert_not_called()

    def test_existing_binary_requires_async_capability_without_executing_it(self):
        plan = SimpleNamespace(project_root=Path("repo"), config={"llm_initial_async": True})
        args = SimpleNamespace(build="release64")
        with mock.patch.object(initial_only.console, "validate_initial_only_binary") as serial_check, \
                mock.patch.object(initial_only.console, "validate_initial_only_async_binary", side_effect=ValueError("old async binary")) as async_check, \
                mock.patch("hybrid_planner.extrapolation.subprocess.Popen") as subprocess:
            with self.assertRaisesRegex(ValueError, "old async binary"):
                async_entry.validate_existing_binary(plan, args)
        serial_check.assert_called_once(); async_check.assert_called_once()
        subprocess.assert_not_called()

    def test_pending_batch_launches_experiment_without_build_or_test_subprocess(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            with mock.patch.object(initial_only, "PROJECT_ROOT", f.root), \
                    mock.patch.object(initial_only, "validate_domain", return_value={}), \
                    mock.patch.object(initial_only, "job_progress", return_value=([], [object()])), \
                    mock.patch.object(initial_only, "build_and_validate_binary") as build, \
                    mock.patch.object(async_entry, "validate_existing_binary") as binary, \
                    mock.patch.object(initial_only, "validate_runtime") as runtime, \
                    mock.patch.object(initial_only, "validate_prompt_pipeline") as prompt, \
                    mock.patch.object(initial_only, "validate_service_port") as port, \
                    mock.patch.object(initial_only, "execute_plan", return_value=0) as execute, \
                    mock.patch.object(initial_only.platform, "platform", return_value="test-platform"), \
                    mock.patch("hybrid_planner.extrapolation.subprocess.Popen") as subprocess, \
                    mock.patch.dict(os.environ, {"COUNT_AUTO_BUILD": "1"}), \
                    mock.patch("sys.stdout", new_callable=io.StringIO):
                result = async_entry.main(f.argv)
            self.assertEqual(result, 0)
            build.assert_not_called(); subprocess.assert_not_called()
            binary.assert_called_once(); runtime.assert_called_once()
            prompt.assert_called_once(); port.assert_called_once(); execute.assert_called_once()
            plan = execute.call_args.args[0]
            self.assertIn("hybrid_planner.batch_console", plan.command)
            self.assertTrue(execute.call_args.kwargs["_lock_held"])
            self.assertFalse((f.output / "build_command.json").exists())
            self.assertFalse((f.output / "test_command.json").exists())

    def test_old_binary_preflight_failure_never_starts_model_or_experiment(self):
        with tempfile.TemporaryDirectory() as directory:
            f = AsyncFixture(Path(directory))
            with mock.patch.object(initial_only, "PROJECT_ROOT", f.root), \
                    mock.patch.object(initial_only, "validate_domain", return_value={}), \
                    mock.patch.object(initial_only, "job_progress", return_value=([], [object()])), \
                    mock.patch.object(initial_only, "build_and_validate_binary") as build, \
                    mock.patch.object(initial_only.console, "validate_initial_only_binary"), \
                    mock.patch.object(initial_only.console, "validate_initial_only_async_binary", side_effect=ValueError("old async binary")) as binary, \
                    mock.patch.object(initial_only, "validate_runtime") as runtime, \
                    mock.patch.object(initial_only, "validate_prompt_pipeline") as prompt, \
                    mock.patch.object(initial_only, "validate_service_port") as port, \
                    mock.patch.object(initial_only, "execute_plan") as execute, \
                    mock.patch("hybrid_planner.extrapolation.subprocess.Popen") as subprocess, \
                    mock.patch("sys.stdout", new_callable=io.StringIO), \
                    mock.patch("sys.stderr", new_callable=io.StringIO) as errors:
                with self.assertRaises(SystemExit) as stopped:
                    async_entry.main(f.argv)
            self.assertEqual(stopped.exception.code, 2)
            binary.assert_called_once()
            self.assertIn("old async binary", errors.getvalue())
            build.assert_not_called(); subprocess.assert_not_called()
            runtime.assert_not_called(); prompt.assert_not_called(); port.assert_not_called(); execute.assert_not_called()


if __name__ == "__main__":
    unittest.main()
