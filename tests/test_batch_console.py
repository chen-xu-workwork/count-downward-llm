import json
import pathlib
import tempfile
import threading
import time
import types
import unittest

from hybrid_planner.batch_console import (
    JobSpec,
    build_child_command,
    classify_return_code,
    infer_problem_scale,
    load_jobs,
    policy_for_scale,
    run_scheduled_jobs,
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


if __name__ == "__main__":
    unittest.main()
