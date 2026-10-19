import csv
import json
import pathlib
import tempfile
import unittest

from hybrid_planner.anytime import ActiveIterationRegistry, AnytimeRunRecorder


class FakeFuture:
    def __init__(self):
        self.cancel_calls = 0

    def cancel(self):
        self.cancel_calls += 1
        return True


class AnytimeLifecycleTests(unittest.TestCase):
    def test_budget_scope_uses_independent_phase_lifecycles(self):
        registry = ActiveIterationRegistry("run-1")
        registry.start_iteration(1)
        first = FakeFuture()
        self.assertTrue(
            registry.register_future("run-1", 1, "request-1", first)
        )
        cancelled = registry.end_iteration(1)
        self.assertEqual(cancelled, [("request-1", True)])
        self.assertEqual(first.cancel_calls, 1)

        registry.start_iteration(2)
        self.assertTrue(registry.is_active("run-1", 2))
        self.assertFalse(registry.is_active("run-1", 1))

    def test_phase_end_writes_curve_and_stale_request_records(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            registry = ActiveIterationRegistry("run-2")
            recorder = AnytimeRunRecorder(
                temp_dir,
                "run-2",
                "live",
                registry,
                plan_file="sas_plan",
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-START] iteration=1 bound=999\n"
            )
            recorder.request_received(
                {
                    "run_id": "run-2",
                    "iteration": 1,
                    "request_id": "run-2-p1-1",
                    "state_id": 1,
                    "state_label": "#1",
                    "reason": "global_stall",
                }
            )
            future = FakeFuture()
            registry.register_future(
                "run-2", 1, "run-2-p1-1", future
            )
            recorder.model_started("run-2-p1-1", 3)
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-END] iteration=1 result=solved "
                "elapsed_seconds=12 phase_seconds=12 plan_cost=80 "
                "plan_length=30 phase_expanded=1000\n"
            )
            recorder.handle_planner_line(
                "[NLM-LLM-TRIGGER-STATS] run_id=run-2 iteration=1 "
                "submitted=10 responses=8 discarded_phase_end=1 "
                "discarded_queued=1 cancelled_inflight=1\n"
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-INCUMBENT] iteration=1 incumbent=1 "
                "elapsed_seconds=12 plan_cost=80 plan_length=30 "
                "plan_number=1 cumulative_expanded=1000\n"
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-RUN-TIMEOUT] elapsed_seconds=7200 "
                "completed_iterations=1 incumbent_count=1 best_bound=80\n"
            )
            recorder.close(return_code=0)

            with (pathlib.Path(temp_dir) / "llm_requests.csv").open(
                encoding="utf-8", newline=""
            ) as stream:
                requests = list(csv.DictReader(stream))
            self.assertEqual(requests[0]["status"], "stale_iteration")

            with (pathlib.Path(temp_dir) / "incumbents.csv").open(
                encoding="utf-8", newline=""
            ) as stream:
                incumbents = list(csv.DictReader(stream))
            self.assertEqual(incumbents[0]["plan_file"], "sas_plan.1")
            self.assertEqual(incumbents[0]["phase_state_requests"], "10")
            self.assertEqual(
                incumbents[0]["cumulative_state_requests"], "10"
            )
            self.assertEqual(
                incumbents[0]["phase_model_generations"], "3"
            )

            metadata = json.loads(
                (pathlib.Path(temp_dir) / "run.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(metadata["incumbent_count"], 1)
            self.assertEqual(metadata["return_code"], 0)
            self.assertEqual(
                metadata["termination_reason"], "search_wall_time_limit"
            )

    def test_lazy_rollout_statistics_are_written_to_phase_and_request_csv(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            registry = ActiveIterationRegistry("run-lazy")
            recorder = AnytimeRunRecorder(
                temp_dir, "run-lazy", "replay", registry
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-START] iteration=1 bound=999\n"
            )
            recorder.request_received(
                {
                    "run_id": "run-lazy",
                    "iteration": 1,
                    "request_id": "run-lazy-p1-0-0",
                    "state_id": 0,
                }
            )
            recorder.handle_planner_line(
                "[HYBRID-LLM-ROLLOUT] event=edge_processed "
                "request_id=run-lazy-p1-0-0 outcome=0\n"
            )
            recorder.handle_planner_line(
                "[HYBRID-LLM-ROLLOUT-STATS] iteration=1 "
                "llm_bursts_started=1 llm_bursts_completed=1 "
                "llm_actions_processed=3 llm_states_new=1 "
                "llm_states_reopened=0 llm_states_duplicate=2 "
                "llm_normal_edges_generated=5\n"
            )
            recorder.handle_planner_line(
                "[HYBRID-LLM-TRIGGER-STATS] iteration=1 "
                "requests_submitted=1 responses_completed=1 "
                "usable_responses=1\n"
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-END] iteration=1 result=failed "
                "phase_expanded=10 phase_evaluated=11 phase_generated=20\n"
            )
            recorder.close(return_code=0)

            with (pathlib.Path(temp_dir) / "phases.csv").open(
                encoding="utf-8", newline=""
            ) as stream:
                phase = list(csv.DictReader(stream))[0]
            self.assertEqual(phase["llm_bursts_completed"], "1")
            self.assertEqual(phase["llm_actions_processed"], "3")
            self.assertEqual(phase["llm_normal_edges_generated"], "5")
            self.assertEqual(phase["submitted"], "1")

            with (pathlib.Path(temp_dir) / "llm_requests.csv").open(
                encoding="utf-8", newline=""
            ) as stream:
                request = list(csv.DictReader(stream))[0]
            self.assertEqual(request["applied_actions"], "1")
            self.assertEqual(request["inserted_states"], "1")

    def test_partial_only_counts_are_written_per_request_phase_and_run(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            registry = ActiveIterationRegistry("run-partial")
            recorder = AnytimeRunRecorder(
                temp_dir, "run-partial", "replay", registry
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-START] iteration=1 bound=999\n"
            )
            recorder.request_received(
                {"request_id": "mixed", "iteration": 1, "state_id": 1}
            )
            recorder.request_finished(
                "mixed", "partial", llm_partial_only=True,
                sample_count=5, usable_sample_count=2,
                goal_reaching_sample_count=2, non_goal_sample_count=2,
                goal_unknown_sample_count=1, filtered_goal_sample_count=2,
                filtered_unknown_sample_count=1,
                pre_filter_usable_sample_count=4, returned_action_count=7,
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-END] iteration=1 result=failed\n"
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-START] iteration=2 bound=999\n"
            )
            for request_id, status, counts in [
                ("goal", "filtered_goal", {
                    "sample_count": 2, "goal_reaching_sample_count": 2,
                    "filtered_goal_sample_count": 2,
                    "pre_filter_usable_sample_count": 2,
                }),
                ("unknown", "goal_unknown", {
                    "sample_count": 1, "goal_unknown_sample_count": 1,
                    "filtered_unknown_sample_count": 1,
                    "pre_filter_usable_sample_count": 1,
                }),
            ]:
                recorder.request_received(
                    {"request_id": request_id, "iteration": 2}
                )
                recorder.request_finished(
                    request_id, status, llm_partial_only=1,
                    usable_sample_count=0, returned_action_count=0,
                    **counts,
                )
                recorder.handle_planner_line(
                    "[NLM-LLM-BRIDGE] reason=phase_end "
                    "request_id=%s\n" % request_id
                )
            # Older callers need not provide any of the new counters.
            recorder.request_received(
                {"request_id": "legacy", "iteration": 2}
            )
            recorder.request_finished("legacy", "ok")
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-END] iteration=2 result=failed\n"
            )
            recorder.close(return_code=0)

            def read_csv(name):
                with (pathlib.Path(temp_dir) / name).open(
                    encoding="utf-8", newline=""
                ) as stream:
                    return list(csv.DictReader(stream))

            requests = {
                row["request_id"]: row for row in read_csv("llm_requests.csv")
            }
            self.assertEqual(requests["mixed"]["usable_sample_count"], "2")
            self.assertEqual(
                requests["mixed"]["pre_filter_usable_sample_count"], "4"
            )
            self.assertEqual(requests["mixed"]["returned_action_count"], "7")
            self.assertEqual(requests["goal"]["status"], "filtered_goal")
            self.assertEqual(requests["unknown"]["status"], "goal_unknown")
            self.assertEqual(requests["legacy"]["llm_partial_only"], "0")
            for name in AnytimeRunRecorder.FILTER_COUNT_FIELDS:
                self.assertEqual(requests["legacy"][name], "0")

            phases = read_csv("phases.csv")
            self.assertEqual(phases[0]["usable_samples"], "2")
            self.assertEqual(phases[0]["filtered_goal_sample_count"], "2")
            self.assertEqual(phases[1]["filtered_goal_sample_count"], "2")
            self.assertEqual(phases[1]["filtered_unknown_sample_count"], "1")
            self.assertEqual(phases[1]["goal_unknown_sample_count"], "1")
            self.assertEqual(phases[1]["pre_filter_usable_sample_count"], "3")
            self.assertEqual(phases[1]["returned_action_count"], "0")
            self.assertEqual(phases[1]["llm_partial_only"], "1")
            self.assertEqual(phases[1]["stale_requests"], "")

            metadata = json.loads(
                (pathlib.Path(temp_dir) / "run.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertTrue(metadata["llm_partial_only"])
            expected = {
                "goal_reaching_sample_count": 4,
                "non_goal_sample_count": 2,
                "goal_unknown_sample_count": 2,
                "filtered_goal_sample_count": 4,
                "filtered_unknown_sample_count": 2,
                "pre_filter_usable_sample_count": 7,
                "returned_action_count": 7,
            }
            for name, count in expected.items():
                self.assertEqual(metadata[name], count)
            self.assertEqual(metadata["usable_sample_count"], 2)

    def test_partial_only_counters_default_to_zero_without_requests(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            registry = ActiveIterationRegistry("run-off")
            recorder = AnytimeRunRecorder(
                temp_dir, "run-off", "off", registry,
                metadata={"llm_partial_only": True},
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-START] iteration=1 bound=999\n"
            )
            recorder.close(return_code=0)
            with (pathlib.Path(temp_dir) / "phases.csv").open(
                encoding="utf-8", newline=""
            ) as stream:
                phase = list(csv.DictReader(stream))[0]
            metadata = json.loads(
                (pathlib.Path(temp_dir) / "run.json").read_text(
                    encoding="utf-8"
                )
            )
            for name in AnytimeRunRecorder.FILTER_COUNT_FIELDS:
                self.assertEqual(phase[name], "0")
                self.assertEqual(metadata[name], 0)
            # Preserve the configured switch even when no LLM request ran.
            self.assertTrue(metadata["llm_partial_only"])

    def test_late_completion_cannot_restore_stale_return_counts(self):
        for status in ("stale_iteration", "discarded_phase_end"):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as temp_dir:
                recorder = AnytimeRunRecorder(
                    temp_dir, "run-late", "replay",
                    ActiveIterationRegistry("run-late"),
                )
                recorder.request_received(
                    {"request_id": "r", "iteration": 1}
                )
                recorder.request_finished(
                    "r", status, returned_action_count=0,
                    usable_sample_count=0,
                )
                recorder.request_finished(
                    "r", "ok", returned_action_count=7,
                    usable_sample_count=1, sample_count=3,
                    goal_reaching_sample_count=2, non_goal_sample_count=1,
                )
                recorder.close(return_code=0)
                with (pathlib.Path(temp_dir) / "llm_requests.csv").open(
                    encoding="utf-8", newline=""
                ) as stream:
                    request = list(csv.DictReader(stream))[0]
                self.assertEqual(request["status"], status)
                self.assertEqual(request["returned_action_count"], "0")
                self.assertEqual(request["usable_sample_count"], "0")
                # Generation diagnostics survive even though search cannot
                # consume a response from the ended phase.
                self.assertEqual(request["goal_reaching_sample_count"], "2")
                self.assertEqual(request["non_goal_sample_count"], "1")
                metadata = json.loads(
                    (pathlib.Path(temp_dir) / "run.json").read_text(
                        encoding="utf-8"
                    )
                )
                self.assertEqual(metadata["returned_action_count"], 0)
                self.assertEqual(metadata["usable_sample_count"], 0)

    def test_phase_end_clears_pending_return_counts(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            recorder = AnytimeRunRecorder(
                temp_dir, "run-pending", "replay",
                ActiveIterationRegistry("run-pending"),
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-START] iteration=1 bound=999\n"
            )
            recorder.request_received({"request_id": "r", "iteration": 1})
            recorder.request_finished(
                "r", "pending", returned_action_count=7,
                usable_sample_count=1, non_goal_sample_count=1,
            )
            recorder.handle_planner_line(
                "[NLM-ANYTIME-PHASE-END] iteration=1 result=failed\n"
            )
            recorder.close(return_code=0)
            with (pathlib.Path(temp_dir) / "llm_requests.csv").open(
                encoding="utf-8", newline=""
            ) as stream:
                request = list(csv.DictReader(stream))[0]
            self.assertEqual(request["status"], "stale_iteration")
            self.assertEqual(request["returned_action_count"], "0")
            self.assertEqual(request["usable_sample_count"], "0")
            self.assertEqual(request["non_goal_sample_count"], "1")
            with (pathlib.Path(temp_dir) / "phases.csv").open(
                encoding="utf-8", newline=""
            ) as stream:
                phase = list(csv.DictReader(stream))[0]
            self.assertEqual(phase["returned_action_count"], "0")
            self.assertEqual(phase["usable_samples"], "0")


if __name__ == "__main__":
    unittest.main()
