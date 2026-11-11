"""Control-plane regressions; no planner, model server or GPU is required."""

import csv
import io
import json
import os
import pathlib
import tempfile
import types
import unittest
from unittest import mock

from hybrid_planner import batch_console, console
from hybrid_planner.anytime import ActiveIterationRegistry, AnytimeRunRecorder
from hybrid_planner.batch_console import (
    JobSpec, _write_batch_records, build_argument_parser, build_child_command,
    build_job_environment, validate_output_query_policy,
    validate_resume_experiment_seed,
)


class InitialOnlyControlTests(unittest.TestCase):
    def test_batch_default_and_child_policy_are_explicit(self):
        parser = build_argument_parser()
        self.assertEqual(parser.parse_args([]).llm_query_policy, "adaptive")
        args = parser.parse_args(["--llm-query-policy", "initial-only"])
        job = JobSpec(0, "p30", pathlib.Path("pddl/problem_scale_30_id_1.pddl"),
                      "live", 30, 1800, 10, False)
        command = build_child_command(
            job, pathlib.Path("domain.pddl"), pathlib.Path("out"), args, "http://local/v1"
        )
        self.assertEqual(command[command.index("--llm-query-policy") + 1], "initial-only")
        self.assertEqual(command[command.index("--llm-samples-per-state") + 1], "3")
        environment, _ = build_job_environment(job, args, base_environment={})
        for prefix in ("HYBRID_LLM_", "NLM_LLM_"):
            self.assertEqual(environment[prefix + "QUERY_POLICY"], "initial-only")
            self.assertEqual(environment[prefix + "MAX_REQUESTS"], "10")
        self.assertEqual(environment["NLM_LLM_MAX_PENDING"], "2")

    def test_policy_overrides_aliases_and_keeps_response_caps(self):
        environment = {"HYBRID_LLM_MAX_REQUESTS": "10", "NLM_LLM_MAX_PENDING": "2",
                       "NLM_LLM_MAX_PROPOSALS_PER_RESPONSE": "8",
                       "NLM_LLM_MAX_ACTIONS_PER_PROPOSAL": "100"}
        flags = ("REQUEST_INITIAL", "ENABLE_ANCESTOR_STAGNATION",
                 "ENABLE_EXPANSION_PLATEAU", "ENABLE_GLOBAL_STALL")
        for prefix in ("HYBRID_LLM_", "NLM_LLM_"):
            environment[prefix + "QUERY_POLICY"] = "adaptive"
            for name in flags:
                environment[prefix + name] = "1"
        console.configure_query_policy(environment, "initial-only")
        for prefix in ("HYBRID_LLM_", "NLM_LLM_"):
            self.assertEqual(environment[prefix + "QUERY_POLICY"], "initial-only")
            for name in flags:
                self.assertEqual(environment[prefix + name], "0")
        self.assertEqual(environment["NLM_LLM_MAX_PROPOSALS_PER_RESPONSE"], "8")
        self.assertEqual(environment["NLM_LLM_MAX_ACTIONS_PER_PROPOSAL"], "100")

    def test_initial_only_requires_positive_effective_group_and_pending_limits(self):
        for suffix in ("MAX_REQUESTS", "MAX_PENDING"):
            for invalid in ("0", "-1", "abc"):
                with self.subTest(suffix=suffix, invalid=invalid):
                    environment = {"NLM_LLM_MAX_REQUESTS": "10", "NLM_LLM_MAX_PENDING": "2",
                                   "HYBRID_LLM_" + suffix: invalid}
                    with self.assertRaisesRegex(ValueError, suffix):
                        console.configure_query_policy(environment, "initial-only")

    def test_adaptive_cli_overrides_inherited_policy_without_changing_triggers(self):
        environment = {"HYBRID_LLM_QUERY_POLICY": "initial-only",
                       "NLM_LLM_QUERY_POLICY": "initial-only", "NLM_LLM_REQUEST_INITIAL": "1"}
        console.configure_query_policy(environment, "adaptive")
        self.assertEqual(environment["HYBRID_LLM_QUERY_POLICY"], "adaptive")
        self.assertEqual(environment["NLM_LLM_QUERY_POLICY"], "adaptive")
        self.assertEqual(environment["NLM_LLM_REQUEST_INITIAL"], "1")

    def test_console_allows_general_sample_count_and_preserves_native_caps(self):
        args = types.SimpleNamespace(
            llm_mode="live", llm_query_policy="initial-only", host="127.0.0.1",
            actual_port=1234, path="/llm/request", pending_behavior="normal",
            emit_state="1", llm_timeout=300, http_workers=0,
            llm_max_concurrency=6, llm_samples_per_state=5,
        )
        with mock.patch.dict(os.environ, {}, clear=True):
            environment = console.configure_planner_environment(args, "p30")
        self.assertEqual(environment["NLM_LLM_MAX_PENDING"], "1")
        self.assertEqual(environment["NLM_LLM_MAX_QUEUED_BURSTS"], "8")
        self.assertEqual(environment["NLM_LLM_MAX_ACTIONS_PER_PROPOSAL"], "100")

    def test_old_binary_rejected_and_marker_across_read_boundary_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            binary_dir = root / "builds" / "release64" / "bin"
            binary_dir.mkdir(parents=True)
            executable = binary_dir / ("downward.exe" if os.name == "nt" else "downward")
            executable.write_bytes(b"old native search")
            with self.assertRaisesRegex(ValueError, "rebuild"):
                console.validate_initial_only_binary(root, "release64", root / "job")
            executable.write_bytes(b"x" * (1024 * 1024 - 10) + b"TRACS_INITIAL_ONLY_PREPARATION_V1")
            self.assertEqual(
                console.validate_initial_only_binary(root, "release64", root / "job"),
                executable,
            )

    def test_console_checks_capability_before_starting_model_service(self):
        argv = ["console", "domain.pddl", "problem.pddl", "plan",
                "--llm-mode", "live", "--external-vllm",
                "--llm-query-policy", "initial-only"]
        with mock.patch("sys.argv", argv), mock.patch("sys.stderr", new_callable=io.StringIO), \
                mock.patch.object(console, "validate_initial_only_binary", side_effect=ValueError("stale binary")) as guard, \
                mock.patch.object(console, "VLLMService") as service:
            with self.assertRaises(SystemExit):
                console.main()
        guard.assert_called_once()
        service.assert_not_called()

    def test_missing_policy_remains_adaptive_and_cannot_be_relabelled(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "batch_config.json").write_text('{"experiment_seed": 7}', encoding="utf-8")
            validate_resume_experiment_seed(root, 7)
            with self.assertRaisesRegex(ValueError, "query policy mismatch"):
                validate_resume_experiment_seed(root, 7, False, "initial-only")
            (root / "run.json").write_text("{}", encoding="utf-8")
            console.validate_run_query_policy(root, "adaptive")
            with self.assertRaisesRegex(ValueError, "query policy mismatch"):
                console.validate_run_query_policy(root, "initial-only")

    def test_old_result_without_manifest_cannot_resume_initial_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            job_dir = root / "p30"
            job_dir.mkdir()
            (job_dir / "job_result.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "without batch_config"):
                validate_output_query_policy(root, "initial-only")

    def test_per_job_policy_catches_mixed_outputs_even_if_batch_policy_matches(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            args = build_argument_parser().parse_args(["--llm-query-policy", "initial-only"])
            _write_batch_records(root, pathlib.Path("domain.pddl"), [], [], args, "")
            config = json.loads((root / "batch_config.json").read_text(encoding="utf-8"))
            self.assertEqual(config["llm_query_policy"], "initial-only")
            validate_output_query_policy(root, "initial-only")
            (root / "p30").mkdir()
            (root / "p30" / "job.json").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "query policy mismatch"):
                validate_output_query_policy(root, "initial-only")

    def test_batch_rejects_old_output_before_rewriting_config_or_starting_service(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            config_path = root / "batch_config.json"
            config_path.write_text("{}", encoding="utf-8")
            argv = ["batch", "--output-dir", directory, "--llm-query-policy", "initial-only"]
            with mock.patch("sys.argv", argv), mock.patch("sys.stderr", new_callable=io.StringIO), \
                    mock.patch.object(batch_console, "load_jobs", return_value=(pathlib.Path("domain.pddl"), [])), \
                    mock.patch.object(batch_console, "_write_batch_records") as writer, \
                    mock.patch.object(batch_console, "_build_vllm_service") as service:
                with self.assertRaises(SystemExit):
                    batch_console.main()
            writer.assert_not_called()
            service.assert_not_called()
            self.assertEqual(config_path.read_text(encoding="utf-8"), "{}")


class InitialPreparationRecorderTests(unittest.TestCase):
    def test_preparation_metadata_does_not_replace_overall_phase_time(self):
        with tempfile.TemporaryDirectory() as directory:
            registry = ActiveIterationRegistry("initial")
            recorder = AnytimeRunRecorder(directory, "initial", "live", registry,
                                          metadata={"llm_query_policy": "initial-only"})
            for iteration, event, duration, submitted in ((1, "end", 120, 10), (2, "interrupted", 15, 2)):
                recorder.handle_planner_line("[NLM-ANYTIME-PHASE-START] iteration=%d bound=100\n" % iteration)
                recorder.handle_planner_line(
                    "[HYBRID-LLM-INITIAL-PREP] event=start iteration=%d budget_groups=10 "
                    "submitted_groups=0 completed_groups=0 elapsed_seconds=0\n" % iteration
                )
                recorder.handle_planner_line(
                    "[HYBRID-LLM-INITIAL-PREP] event=%s iteration=%d budget_groups=10 "
                    "submitted_groups=%d completed_groups=%d elapsed_seconds=%d\n"
                    % (event, iteration, submitted, submitted, duration)
                )
                recorder.handle_planner_line(
                    "[NLM-ANYTIME-PHASE-END] iteration=%d result=timeout elapsed_seconds=1800 phase_seconds=900\n" % iteration
                )
            recorder.close(return_code=7)
            with (pathlib.Path(directory) / "phases.csv").open(encoding="utf-8", newline="") as stream:
                phases = list(csv.DictReader(stream))
            self.assertEqual(phases[0]["elapsed_seconds"], "1800")
            self.assertEqual(phases[0]["initial_preparation_seconds"], "120")
            self.assertEqual(phases[0]["initial_preparation_status"], "completed")
            self.assertEqual(phases[0]["initial_budget_groups"], "10")
            self.assertEqual(phases[1]["initial_submitted_groups"], "2")
            self.assertEqual(phases[1]["initial_completed_groups"], "2")
            self.assertEqual(phases[1]["initial_preparation_status"], "interrupted")
            run = json.loads((pathlib.Path(directory) / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(run["initial_preparation_seconds"], 135)
            self.assertEqual(run["llm_query_policy"], "initial-only")

    def test_killed_preparation_leaves_unknown_duration(self):
        with tempfile.TemporaryDirectory() as directory:
            recorder = AnytimeRunRecorder(directory, "killed", "live", ActiveIterationRegistry("killed"))
            recorder.handle_planner_line("[NLM-ANYTIME-PHASE-START] iteration=1 bound=100\n")
            recorder.handle_planner_line(
                "[HYBRID-LLM-INITIAL-PREP] event=start iteration=1 budget_groups=10 "
                "submitted_groups=0 completed_groups=0 elapsed_seconds=0\n"
            )
            recorder.close(return_code=-9)
            with (pathlib.Path(directory) / "phases.csv").open(encoding="utf-8", newline="") as stream:
                phase = next(csv.DictReader(stream))
            self.assertEqual(phase["initial_preparation_status"], "incomplete")
            self.assertEqual(phase["initial_preparation_seconds"], "")
            run = json.loads((pathlib.Path(directory) / "run.json").read_text(encoding="utf-8"))
            self.assertEqual(run["initial_preparation_incomplete_phases"], 1)


if __name__ == "__main__":
    unittest.main()
