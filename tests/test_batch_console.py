import json
import pathlib
import tempfile
import threading
import time
import types
import unittest

from hybrid_planner.batch_console import (
    JobResult,
    JobSpec,
    build_job_environment,
    build_child_command,
    classify_return_code,
    infer_problem_scale,
    llm_expansion_multiplier_for_scale,
    load_jobs,
    parse_resume_problem_path_maps,
    partition_resumable_jobs,
    policy_for_scale,
    run_scheduled_jobs,
    write_job_result,
)


def make_args(**overrides):
    values = {
        "manifest": "",
        "domain": "",
        "problems": [],
        "default_mode": "live",
        "small_scale_max": 30,
        "small_time_limit": 1800.0,
        "large_time_limit": 3600.0,
        "small_max_requests": 10,
        "large_max_requests": 15,
        "build": "release64",
        "planner_python": "python3",
        "prompt_domain_code": "",
        "llm_model": "Qwen3.5-9B",
        "llm_max_concurrency": 6,
        "llm_samples_per_state": 3,
        "llm_max_qps": 0.0,
        "llm_max_retries": 3,
        "llm_timeout": 300.0,
        "llm_temperature": 0.7,
        "llm_top_p": 0.9,
        "llm_max_tokens": 16384,
        "http_workers": 0,
        "prompt_workers": 4,
        "validation_workers": 4,
        "pending_behavior": "normal",
        "llm_extra_params": "",
        "scale_aware_llm_thresholds": False,
        "scale_30_expansion_multiplier": 0.5,
        "scale_40_expansion_multiplier": 0.25,
    }
    values.update(overrides)
    return types.SimpleNamespace(**values)


class BatchPolicyTests(unittest.TestCase):
    def test_normal_time_limit_exit_is_not_reported_as_a_batch_failure(self):
        self.assertEqual(classify_return_code(0), "plan_found")
        self.assertEqual(classify_return_code(5), "incomplete")
        self.assertEqual(classify_return_code(7), "timeout")
        self.assertEqual(classify_return_code(1), "failed")

    def test_scale_policy_matches_experiment_contract(self):
        args = make_args()
        self.assertEqual(policy_for_scale(30, args), (1800.0, 10, False))
        self.assertEqual(policy_for_scale(40, args), (3600.0, 15, True))
        self.assertEqual(infer_problem_scale("problem_scale_40_id_222.pddl"), 40)

    def test_manifest_can_mix_live_and_off_jobs(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            domain = root / "domain.pddl"
            small = root / "problem_scale_30_id_1.pddl"
            large = root / "problem_scale_40_id_2.pddl"
            for path in (domain, small, large):
                path.write_text("(define)", encoding="utf-8")
            manifest = root / "batch.json"
            manifest.write_text(
                json.dumps(
                    {
                        "domain": domain.name,
                        "jobs": [
                            {"problem": small.name, "mode": "off"},
                            {"problem": large.name, "mode": "live"},
                        ],
                    }
                ),
                encoding="utf-8",
            )

            loaded_domain, jobs = load_jobs(make_args(manifest=str(manifest)))

        self.assertEqual(loaded_domain, domain.resolve())
        self.assertEqual(jobs[0].mode, "off")
        self.assertEqual(jobs[0].time_limit_seconds, 1800.0)
        self.assertEqual(jobs[0].max_requests_per_iteration, 0)
        self.assertFalse(jobs[0].exclusive)
        self.assertEqual(jobs[1].mode, "live")
        self.assertEqual(jobs[1].time_limit_seconds, 3600.0)
        self.assertEqual(jobs[1].max_requests_per_iteration, 15)
        self.assertTrue(jobs[1].exclusive)

    def test_manifest_explicit_scale_supports_other_filename_conventions(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            domain = root / "domain.pddl"
            problem = root / "hard-instance.pddl"
            domain.write_text("(define)", encoding="utf-8")
            problem.write_text("(define)", encoding="utf-8")
            manifest = root / "batch.json"
            manifest.write_text(
                json.dumps(
                    {
                        "domain": domain.name,
                        "jobs": [{"problem": problem.name, "scale": 40}],
                    }
                ),
                encoding="utf-8",
            )

            _, jobs = load_jobs(make_args(manifest=str(manifest)))

        self.assertEqual(jobs[0].scale, 40)
        self.assertTrue(jobs[0].exclusive)

    def test_child_commands_share_external_vllm_only_in_live_mode(self):
        args = make_args()
        problem = pathlib.Path("/tmp/problem_scale_30_id_1.pddl")
        domain = pathlib.Path("/tmp/domain.pddl")
        output = pathlib.Path("/tmp/job")
        live = JobSpec(1, "live", problem, "live", 30, 1800, 10, False)
        off = JobSpec(2, "off", problem, "off", 30, 1800, 0, False)

        live_command = build_child_command(
            live, domain, output, args, "http://127.0.0.1:8091/v1"
        )
        off_command = build_child_command(
            off, domain, output, args, "http://127.0.0.1:8091/v1"
        )

        self.assertIn("--external-vllm", live_command)
        self.assertIn("http://127.0.0.1:8091/v1", live_command)
        self.assertNotIn("--external-vllm", off_command)
        self.assertEqual(off_command[off_command.index("--llm-mode") + 1], "off")

    def test_scale_aware_llm_policy_changes_only_expansion_cadence(self):
        args = make_args(scale_aware_llm_thresholds=True)
        problem = pathlib.Path("/tmp/problem.pddl")
        expected = {
            20: (1.0, 500000, 100000, 65536),
            30: (0.5, 250000, 50000, 32768),
            40: (0.25, 125000, 25000, 16384),
        }

        for scale, values in expected.items():
            with self.subTest(scale=scale):
                multiplier, gap, ancestor_interval, plateau_rearm = values
                job = JobSpec(
                    scale,
                    "scale-%d" % scale,
                    problem,
                    "live",
                    scale,
                    1,
                    15,
                    scale > 30,
                )
                environment, policy = build_job_environment(
                    job,
                    args,
                    base_environment={
                        # Statistical plateau evidence must pass through without
                        # being multiplied by the cadence policy.
                        "NLM_LLM_PLATEAU_WINDOW_EXPANSIONS": "65536",
                    },
                )

                self.assertEqual(
                    llm_expansion_multiplier_for_scale(scale, args), multiplier
                )
                self.assertEqual(policy["expansion_multiplier"], multiplier)
                self.assertEqual(
                    environment["NLM_LLM_MIN_REQUEST_GAP_EXPANSIONS"], str(gap)
                )
                self.assertEqual(
                    environment["HYBRID_LLM_STALL_EXPANSIONS"], str(gap)
                )
                self.assertEqual(
                    environment["NLM_LLM_ANCESTOR_CHECK_INTERVAL"],
                    str(ancestor_interval),
                )
                self.assertEqual(
                    environment[
                        "NLM_LLM_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS"
                    ],
                    str(plateau_rearm),
                )
                self.assertEqual(
                    environment["NLM_LLM_PLATEAU_WINDOW_EXPANSIONS"], "65536"
                )
                self.assertEqual(
                    policy["plateau_detector_settings"][
                        "PLATEAU_WINDOW_EXPANSIONS"
                    ],
                    "65536",
                )

    def test_off_job_forces_trigger_off_without_scale_policy(self):
        args = make_args(scale_aware_llm_thresholds=True)
        job = JobSpec(
            1,
            "off",
            pathlib.Path("/tmp/problem.pddl"),
            "off",
            40,
            3600,
            0,
            True,
        )

        environment, policy = build_job_environment(job, args, {})

        self.assertEqual(environment["HYBRID_LLM_TRIGGER"], "0")
        self.assertEqual(environment["NLM_LLM_TRIGGER"], "0")
        self.assertEqual(environment["HYBRID_LLM_MAX_REQUESTS"], "0")
        self.assertFalse(policy["enabled"])

    def test_large_job_is_an_exclusive_barrier(self):
        problem = pathlib.Path("/tmp/problem.pddl")
        jobs = [
            JobSpec(1, "small-1", problem, "off", 30, 1, 0, False),
            JobSpec(2, "small-2", problem, "off", 30, 1, 0, False),
            JobSpec(3, "large", problem, "live", 40, 1, 15, True),
            JobSpec(4, "small-3", problem, "off", 30, 1, 0, False),
        ]
        active = set()
        lock = threading.Lock()
        large_observation = []

        def run_job(job):
            with lock:
                active.add(job.job_id)
                if job.exclusive:
                    large_observation.append(set(active))
            time.sleep(0.03)
            with lock:
                active.remove(job.job_id)
            return types.SimpleNamespace(index=job.index, job_id=job.job_id)

        results = run_scheduled_jobs(jobs, 2, run_job)

        self.assertEqual([result.job_id for result in results], [
            "small-1", "small-2", "large", "small-3"
        ])
        self.assertEqual(large_observation, [{"large"}])

    def test_resume_skips_timeout_but_retries_failed_job(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_dir = pathlib.Path(temp_dir)
            problem = output_dir / "problem_scale_40_id_2.pddl"
            problem.write_text("(define)", encoding="utf-8")
            job = JobSpec(
                1, "large-live", problem.resolve(), "live", 40, 3600, 15, True
            )
            job_dir = output_dir / job.job_id
            job_dir.mkdir()
            timeout_result = {
                "index": 1,
                "job_id": job.job_id,
                "problem": str(job.problem),
                "mode": job.mode,
                "scale": job.scale,
                "time_limit_seconds": job.time_limit_seconds,
                "max_requests_per_iteration": job.max_requests_per_iteration,
                "exclusive": job.exclusive,
                "status": "timeout",
                "return_code": 7,
                "elapsed_seconds": 3600.1,
                "output_dir": str(job_dir),
                "error": "",
            }
            write_job_result(job_dir, JobResult(**timeout_result))
            completed, pending = partition_resumable_jobs([job], output_dir)
            self.assertEqual([result.status for result in completed], ["timeout"])
            self.assertEqual(pending, [])

            failed_result = dict(timeout_result)
            failed_result.update(status="failed", return_code=1)
            write_job_result(job_dir, JobResult(**failed_result))
            completed, pending = partition_resumable_jobs([job], output_dir)
            self.assertEqual(completed, [])
            self.assertEqual(pending, [job])

    def test_resume_accepts_an_explicit_cross_machine_problem_path_map(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output_dir = pathlib.Path(temp_dir)
            remote_root = output_dir / "remote-data"
            remote_root.mkdir()
            problem = remote_root / "problem_scale_20_id_2.pddl"
            problem.write_text("(define)", encoding="utf-8")
            job = JobSpec(
                1,
                "relocated",
                problem.resolve(),
                "off",
                20,
                1800,
                0,
                False,
            )
            job_dir = output_dir / job.job_id
            job_dir.mkdir()
            write_job_result(
                job_dir,
                JobResult(
                    index=1,
                    job_id=job.job_id,
                    problem=(
                        "/mnt/e/local-data/problem_scale_20_id_2.pddl"
                    ),
                    mode="off",
                    scale=20,
                    time_limit_seconds=1800,
                    max_requests_per_iteration=0,
                    exclusive=False,
                    status="plan_found",
                    return_code=0,
                    elapsed_seconds=1,
                    output_dir=str(job_dir),
                    error="",
                ),
            )
            mappings = parse_resume_problem_path_maps(
                ["/mnt/e/local-data=%s" % remote_root]
            )

            completed, pending = partition_resumable_jobs(
                [job], output_dir, mappings
            )

        self.assertEqual([result.status for result in completed], ["plan_found"])
        self.assertEqual(pending, [])


if __name__ == "__main__":
    unittest.main()
