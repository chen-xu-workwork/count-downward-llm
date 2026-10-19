"""HTTP-level regression checks for filtering goal-reaching LLM samples."""

import contextlib
import csv
import importlib.util
import json
import pathlib
import tempfile
import threading
import unittest
import urllib.request
from dataclasses import dataclass
from http.server import ThreadingHTTPServer
from unittest.mock import patch

from hybrid_planner.console import make_handler
from hybrid_planner.anytime import ActiveIterationRegistry, AnytimeRunRecorder
from hybrid_planner.llm.client import LLMGenerationResult
from hybrid_planner.validation.response_processor import (
    PlanResponseProcessor,
    ProcessedModelResponse,
    UnifiedPlanningPrefixValidator,
)


HAS_UNIFIED_PLANNING = importlib.util.find_spec("unified_planning") is not None


@dataclass
class FakePrompts:
    system: str = "system"
    user: str = "user"
    runtime_problem: str = "(define (problem runtime))"
    problem_description: str = "test problem"
    problem_path = pathlib.Path("problem.pddl")

    def as_messages(self):
        return [
            {"role": "system", "content": self.system},
            {"role": "user", "content": self.user},
        ]


class FakePromptBuilder:
    def __init__(self, runtime_problem=None):
        self.runtime_problem = runtime_problem

    def build(self, problem_id, init_text):
        if self.runtime_problem is None:
            return FakePrompts()
        return FakePrompts(runtime_problem=self.runtime_problem)


class CountingRuntime:
    """One predetermined set of samples; a refill is observable to the test."""

    def __init__(self, contents):
        self.contents = contents
        self.calls = []

    def generate_many(self, messages, count, request_id=""):
        self.calls.append((count, request_id))
        return tuple(
            LLMGenerationResult(
                content=content,
                response={"choices": []},
                error=None,
                attempts=1,
                elapsed_seconds=0.01,
                sampling_seed=100 + index,
            )
            for index, content in enumerate(self.contents)
        )


class MappedProcessor:
    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def process(self, content, runtime_problem):
        self.calls.append(content)
        return self.responses[int(content)]


def processed(actions, goal_reached, status="ok", generated_count=None):
    return ProcessedModelResponse(
        status=status,
        actions=tuple(actions),
        generated_action_count=(
            len(actions) if generated_count is None else generated_count
        ),
        legal_action_count=len(actions),
        goal_reached=goal_reached,
    )


class PartialOnlyHandlerTests(unittest.TestCase):
    @contextlib.contextmanager
    def handler(self, responses, **options):
        runtime = CountingRuntime([str(index) for index in range(len(responses))])
        processor = MappedProcessor(responses)
        server = ThreadingHTTPServer(
            ("127.0.0.1", 0),
            make_handler(
                "/llm/request",
                FakePromptBuilder(),
                llm_runtime=runtime,
                response_processor=processor,
                samples_per_state=len(responses),
                **options,
            ),
        )
        thread = threading.Thread(
            target=lambda: server.serve_forever(poll_interval=0.01),
            daemon=True,
        )
        thread.start()
        try:
            yield server, runtime, processor
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

    @staticmethod
    def post_json(server):
        request = urllib.request.Request(
            "http://127.0.0.1:%d/llm/request" % server.server_address[1],
            data=json.dumps(
                {
                    "request_id": "partial-only-1",
                    "state_id": 17,
                    "problem_id": "problem",
                    "init": "(:init)",
                }
            ).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.loads(response.read().decode("utf-8"))

    def test_mixed_samples_keep_original_indices_and_select_retained_primary(self):
        responses = [
            processed(["(finish goal)"], True),
            processed(["(step retained)"], False),
            processed(["(step partial)"], False, status="partial", generated_count=2),
        ]
        with self.handler(responses, llm_partial_only=True) as (server, runtime, processor):
            result = self.post_json(server)

        self.assertTrue(result["llm_partial_only"])
        self.assertEqual(result["actions"], ["(step retained)"])
        self.assertEqual(result["action_chains"], [[], ["(step retained)"], ["(step partial)"]])
        self.assertEqual(result["sample_count"], 3)
        self.assertEqual(result["usable_sample_count"], 2)
        self.assertEqual(result["pre_filter_usable_sample_count"], 3)
        self.assertEqual(result["goal_reaching_sample_count"], 1)
        self.assertEqual(result["non_goal_sample_count"], 2)
        self.assertEqual(result["goal_unknown_sample_count"], 0)
        self.assertEqual(result["filtered_goal_sample_count"], 1)
        self.assertEqual(result["filtered_unknown_sample_count"], 0)
        self.assertEqual(result["returned_action_count"], 2)
        self.assertEqual([sample["sample_index"] for sample in result["samples"]], [0, 1, 2])
        self.assertEqual(result["samples"][0]["admission_status"], "filtered_goal")
        self.assertEqual(result["samples"][0]["actions"], [])
        self.assertEqual(result["samples"][0]["legal_action_count"], 1)
        self.assertEqual(result["samples"][1]["admission_status"], "retained")
        self.assertFalse(result["goal_reached"])
        # Filtering must not ask the model or validator for replacements.
        self.assertEqual(runtime.calls, [(3, "partial-only-1")])
        self.assertEqual(processor.calls, ["0", "1", "2"])

    def test_all_goal_samples_leave_no_top_level_fallback_actions(self):
        responses = [
            processed(["(finish a)"], True),
            processed(["(finish b)"], True),
        ]
        with self.handler(responses, llm_partial_only=True) as (server, runtime, _):
            result = self.post_json(server)

        self.assertEqual(result["actions"], [])
        self.assertFalse(any(result["action_chains"]))
        self.assertTrue(all(sample["actions"] == [] for sample in result["samples"]))
        self.assertEqual(result["usable_sample_count"], 0)
        self.assertEqual(result["returned_action_count"], 0)
        self.assertEqual(result["filtered_goal_sample_count"], 2)
        self.assertEqual(result["status"], "filtered_goal")
        self.assertEqual(runtime.calls, [(2, "partial-only-1")])

    def test_unknown_goal_is_filtered_while_ok_non_goal_is_retained(self):
        responses = [
            processed(["(step unknown)"], None),
            processed(["(step legal-but-incomplete)"], False, status="ok"),
        ]
        with self.handler(responses, llm_partial_only=True) as (server, _, _):
            result = self.post_json(server)

        self.assertEqual(result["actions"], ["(step legal-but-incomplete)"])
        self.assertEqual(result["action_chains"], [[], ["(step legal-but-incomplete)"]])
        self.assertEqual(result["samples"][0]["admission_status"], "filtered_goal_unknown")
        self.assertEqual(result["samples"][1]["admission_status"], "retained")
        self.assertEqual(result["goal_unknown_sample_count"], 1)
        self.assertEqual(result["filtered_unknown_sample_count"], 1)
        self.assertEqual(result["non_goal_sample_count"], 1)

    def test_unknown_empty_error_does_not_count_as_an_unknown_filtered_chain(self):
        responses = [
            processed([], None, status="parse_error"),
            processed(["(step unknown)"], None),
        ]
        with self.handler(responses, llm_partial_only=True) as (server, _, _):
            result = self.post_json(server)

        self.assertEqual(result["goal_unknown_sample_count"], 2)
        self.assertEqual(result["filtered_unknown_sample_count"], 1)
        self.assertEqual(result["pre_filter_usable_sample_count"], 1)
        self.assertEqual(result["status"], "goal_unknown")
        self.assertEqual(result["actions"], [])
        self.assertFalse(any(result["action_chains"]))

    def test_default_off_preserves_goal_and_unknown_samples(self):
        responses = [
            processed(["(finish a)"], True),
            processed(["(step unknown)"], None),
        ]
        with self.handler(responses) as (server, _, _):
            result = self.post_json(server)

        self.assertFalse(result["llm_partial_only"])
        self.assertEqual(result["actions"], ["(finish a)"])
        self.assertEqual(result["action_chains"], [["(finish a)"], ["(step unknown)"]])
        self.assertEqual(result["usable_sample_count"], 2)
        self.assertEqual(result["filtered_goal_sample_count"], 0)
        self.assertEqual(result["filtered_unknown_sample_count"], 0)

    def test_goal_after_rollout_limit_is_still_filtered_before_budgeting(self):
        # The complete sample reaches its goal only at action 101. It must not
        # become an admissible partial prefix merely because rollout caps at 100.
        goal_actions = ["(step goal-%d)" % index for index in range(101)]
        responses = [
            processed(goal_actions, True),
            processed(["(step retained)"], False),
        ]
        with patch.dict(
            "os.environ",
            {
                "HYBRID_LLM_MAX_PROPOSALS_PER_RESPONSE": "8",
                "HYBRID_LLM_MAX_ACTIONS_PER_PROPOSAL": "100",
                "HYBRID_LLM_MAX_BURST_ACTIONS": "100",
            },
        ):
            with self.handler(responses, llm_partial_only=True) as (server, _, processor):
                result = self.post_json(server)

        self.assertEqual(result["actions"], ["(step retained)"])
        self.assertEqual(result["action_chains"], [[], ["(step retained)"]])
        self.assertEqual(result["samples"][0]["legal_action_count"], 101)
        self.assertEqual(result["returned_action_count"], 1)
        self.assertEqual(processor.calls, ["0", "1"])

    def test_debug_preserves_validated_goal_chain_and_admission_decision(self):
        responses = [
            processed(["(finish goal)"], True),
            processed(["(step retained)"], False),
        ]
        with tempfile.TemporaryDirectory() as debug_dir:
            with self.handler(
                responses, llm_partial_only=True, prompt_debug_dir=debug_dir
            ) as (server, _, _):
                result = self.post_json(server)
            record = json.loads(
                pathlib.Path(result["prompt_debug_file"]).read_text(encoding="utf-8")
            )

        filtered, retained = record["samples"]
        self.assertEqual(filtered["sample_index"], 0)
        self.assertEqual(filtered["processed_response"]["actions"], ["(finish goal)"])
        self.assertTrue(filtered["processed_response"]["goal_reached"])
        self.assertEqual(filtered["admission"]["admission_status"], "filtered_goal")
        self.assertEqual(filtered["admission"]["original_action_count"], 1)
        self.assertEqual(filtered["admission"]["returned_action_count"], 0)
        self.assertEqual(filtered["admission"]["actions"], [])
        self.assertEqual(retained["admission"]["admission_status"], "retained")
        self.assertEqual(retained["admission"]["actions"], ["(step retained)"])

    def test_final_phase_expiry_clears_payload_and_debug_returned_actions(self):
        class ExpiringRegistry:
            run_id = "standalone"
            checks = 0

            def accept_request(self, run_id, iteration):
                return True

            def is_active(self, run_id, iteration):
                self.checks += 1
                # Expire between the final processing check and dispatch.
                return self.checks < 5

        with tempfile.TemporaryDirectory() as debug_dir:
            with self.handler(
                [processed(["(step retained)"], False)],
                llm_partial_only=True,
                prompt_debug_dir=debug_dir,
                anytime_registry=ExpiringRegistry(),
            ) as (server, _, _):
                result = self.post_json(server)
            record = json.loads(
                pathlib.Path(result["prompt_debug_file"]).read_text(encoding="utf-8")
            )

        self.assertEqual(result["status"], "stale_iteration")
        self.assertEqual(result["returned_action_count"], 0)
        self.assertEqual(result["actions"], [])
        self.assertFalse(any(result["action_chains"]))
        self.assertEqual(record["response_status"], "stale_iteration")
        self.assertEqual(
            record["samples"][0]["admission"]["admission_status"],
            "discarded_stale",
        )
        self.assertEqual(record["samples"][0]["admission"]["returned_action_count"], 0)
        self.assertEqual(record["samples"][0]["admission"]["actions"], [])

    def test_handler_records_filter_counts_in_csv_and_run_metadata(self):
        responses = [
            processed(["(finish goal)"], True),
            processed(["(step retained)"], False),
        ]
        with tempfile.TemporaryDirectory() as output_dir:
            recorder = AnytimeRunRecorder(
                output_dir, "standalone", "replay",
                ActiveIterationRegistry("standalone"),
                metadata={"llm_partial_only": True},
            )
            with self.handler(
                responses, llm_partial_only=True, anytime_recorder=recorder,
            ) as (server, _, _):
                self.post_json(server)
            recorder.close(return_code=0)
            with (pathlib.Path(output_dir) / "llm_requests.csv").open(
                encoding="utf-8", newline="",
            ) as stream:
                row = next(csv.DictReader(stream))
            metadata = json.loads(
                (pathlib.Path(output_dir) / "run.json").read_text(encoding="utf-8")
            )

        self.assertEqual(row["sample_count"], "2")
        self.assertEqual(row["pre_filter_usable_sample_count"], "2")
        self.assertEqual(row["usable_sample_count"], "1")
        self.assertEqual(row["goal_reaching_sample_count"], "1")
        self.assertEqual(row["filtered_goal_sample_count"], "1")
        self.assertEqual(row["returned_action_count"], "1")
        self.assertTrue(metadata["llm_partial_only"])
        self.assertEqual(metadata["filtered_goal_sample_count"], 1)

    @unittest.skipUnless(HAS_UNIFIED_PLANNING, "requires unified-planning")
    def test_real_validator_filters_goal_reached_before_invalid_tail(self):
        domain = """(define (domain demo)
          (:requirements :strips :typing)
          (:types item)
          (:predicates (ready ?x - item) (done ?x - item))
          (:action finish
            :parameters (?x - item)
            :precondition (ready ?x)
            :effect (and (not (ready ?x)) (done ?x))))
        """
        problem = """(define (problem p1)
          (:domain demo)
          (:objects a - item)
          (:init (ready a))
          (:goal (done a)))
        """
        with tempfile.TemporaryDirectory() as temp_dir:
            domain_path = pathlib.Path(temp_dir) / "domain.pddl"
            domain_path.write_text(domain, encoding="utf-8")
            processor = PlanResponseProcessor(UnifiedPlanningPrefixValidator(domain_path))
            text = "action_Finish(a)\naction_Unknown(a)"
            validated = processor.process(text, problem)
            self.assertTrue(validated.goal_reached)
            self.assertEqual(validated.generated_action_count, 2)
            self.assertEqual(validated.legal_action_count, 1)

            runtime = CountingRuntime([text])
            server = ThreadingHTTPServer(
                ("127.0.0.1", 0),
                make_handler(
                    "/llm/request",
                    FakePromptBuilder(problem),
                    llm_runtime=runtime,
                    response_processor=processor,
                    samples_per_state=1,
                    llm_partial_only=True,
                ),
            )
            thread = threading.Thread(
                target=lambda: server.serve_forever(poll_interval=0.01),
                daemon=True,
            )
            thread.start()
            try:
                result = self.post_json(server)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)

        self.assertEqual(result["filtered_goal_sample_count"], 1)
        self.assertEqual(result["actions"], [])
        self.assertFalse(any(result["action_chains"]))


if __name__ == "__main__":
    unittest.main()
