"""Container-only checks for native initial-only request preparation.

Run after building release64, without a model or GPU:
    python3 -m unittest tests.test_initial_only_integration -v

COUNT_BUILD_ROOT may point at another existing native build directory. These
tests never build the planner and keep translator artifacts in temporary dirs.
"""

from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BUILD_ROOT = Path(os.environ.get(
    "COUNT_BUILD_ROOT", str(PROJECT_ROOT / "builds" / "release64")
)).expanduser().resolve()
BINARY = BUILD_ROOT / "bin" / "downward"
PREPROCESSOR = BUILD_ROOT / "bin" / "preprocess"
TRANSLATOR = BUILD_ROOT / "bin" / "translate" / "translate.py"
TOOLCHAIN_PRESENT = all(
    path.is_file() for path in (BINARY, PREPROCESSOR, TRANSLATOR)
)

DOMAIN = """(define (domain initial-only-chain)
  (:requirements :strips :typing)
  (:types location)
  (:predicates (at ?p - location) (next ?a ?b - location))
  (:action move
    :parameters (?a ?b - location)
    :precondition (and (at ?a) (next ?a ?b))
    :effect (and (not (at ?a)) (at ?b))))
"""
PROBLEM = """(define (problem initial-only-chain-test)
  (:domain initial-only-chain)
  (:objects s0 s1 s2 s3 - location)
  (:init (at s0) (next s0 s1) (next s1 s2) (next s2 s3))
  (:goal (at s3)))
"""
LAZY = (
    "lazy_greedy(hff,preferred=hff,cost_type=one,"
    "reopen_closed=false,llm_h=hff)"
)


@contextmanager
def mock_server(delay_seconds=0.0):
    """Record each group and return one partial prefix, never a direct goal."""
    requests = []
    lock = threading.Lock()
    stopping = threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(size).decode("utf-8"))
            with lock:
                requests.append(request)
            if stopping.wait(delay_seconds):
                return
            payload = json.dumps({
                "status": "partial",
                "actions": ["(move s0 s1)"],
                "action_chains": [["(move s0 s1)"]],
            }).encode("utf-8")
            try:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError):
                # An interrupted preparation deliberately closes its sockets.
                pass

        def log_message(self, _format, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_port, requests
    finally:
        stopping.set()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def records(output, marker):
    return [
        dict(re.findall(r"(\w+)=([^\s]+)", line))
        for line in output.splitlines()
        if marker in line
    ]


@unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux/WSL")
@unittest.skipUnless(TOOLCHAIN_PRESENT, "build release64 before integration test")
class InitialOnlyIntegrationTests(unittest.TestCase):
    def run_planner(self, budget, *, delay=0.01, phases=1, seconds=20):
        with tempfile.TemporaryDirectory(prefix="count-initial-only-") as tmp:
            work = Path(tmp)
            domain = work / "domain.pddl"
            problem = work / "problem.pddl"
            domain.write_text(DOMAIN, encoding="utf-8")
            problem.write_text(PROBLEM, encoding="utf-8")
            translated = subprocess.run(
                [sys.executable, str(TRANSLATOR), str(domain), str(problem)],
                cwd=work, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=30,
            )
            self.assertEqual(translated.returncode, 0, translated.stdout)
            with (work / "output.sas").open("rb") as source:
                preprocessed = subprocess.run(
                    [str(PREPROCESSOR)], cwd=work, stdin=source, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
                )
            self.assertEqual(preprocessed.returncode, 0, preprocessed.stdout)
            self.assertTrue((work / "output").is_file(), preprocessed.stdout)

            # Isolate tests from settings inherited from an experiment shell,
            # including the legacy prefix which otherwise acts as a fallback.
            env = {
                key: value for key, value in os.environ.items()
                if not key.startswith(("HYBRID_LLM_", "NLM_LLM_"))
            }
            env.update({
                "HYBRID_LLM_TRIGGER": "1",
                "HYBRID_LLM_QUERY_POLICY": "initial-only",
                "HYBRID_LLM_COMM_MODE": "http",
                "HYBRID_LLM_HTTP_HOST": "127.0.0.1",
                "HYBRID_LLM_HTTP_PATH": "/llm/request",
                "HYBRID_LLM_HTTP_TIMEOUT_MS": "10000",
                "HYBRID_LLM_HTTP_WORKERS": "2",
                "HYBRID_LLM_MAX_PENDING": "2",
                "HYBRID_LLM_MAX_REQUESTS": str(budget),
                "HYBRID_LLM_MAX_QUEUED_BURSTS": "8",
                "HYBRID_LLM_RUN_ID": "initial-only-integration",
                # Deliberately enable old triggers: the policy must override
                # them, including the legacy one-off initial request.
                "HYBRID_LLM_REQUEST_INITIAL": "1",
                "HYBRID_LLM_ENABLE_ANCESTOR_STAGNATION": "1",
                "HYBRID_LLM_ENABLE_GLOBAL_STALL": "1",
                "HYBRID_LLM_ENABLE_EXPANSION_PLATEAU": "1",
                "HYBRID_LLM_STALL_EXPANSIONS": "1",
                "HYBRID_LLM_MIN_REQUEST_GAP_EXPANSIONS": "0",
            })
            search = (
                "iterated([" + ",".join([LAZY] * phases) + "],"
                "pass_bound=false,repeat_last=false,continue_on_solve=true,"
                "continue_on_fail=true,max_time=" + str(seconds) + ")"
            )
            with mock_server(delay) as (port, requests):
                env["HYBRID_LLM_HTTP_PORT"] = str(port)
                with (work / "output").open("rb") as source:
                    started = time.monotonic()
                    completed = subprocess.run(
                        [str(BINARY), "--heuristic", "hff=irhff(cost_type=one)",
                         "--search", search, "--internal-plan-file",
                         str(work / "sas_plan")],
                        cwd=work, stdin=source, env=env, text=True,
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        timeout=seconds + 15,
                    )
                    elapsed = time.monotonic() - started
            return completed, list(requests), elapsed

    def assert_initial_requests(self, requests, expected_count):
        self.assertEqual(len(requests), expected_count, requests)
        self.assertEqual(len({item["request_id"] for item in requests}),
                         expected_count)
        self.assertEqual({item["reason"] for item in requests}, {"initial_batch"})
        self.assertEqual({item["search_expansions"] for item in requests}, {0})
        self.assertEqual({item["g"] for item in requests}, {0})
        self.assertEqual(len({item["state_id"] for item in requests}), 1)
        self.assertEqual(len({item["init"] for item in requests}), 1)
        self.assertIn("(at s0)", requests[0]["init"])

    def test_ten_groups_are_buffered_before_any_replay_without_queue_loss(self):
        completed, requests, _ = self.run_planner(10)
        output = completed.stdout
        self.assertEqual(completed.returncode, 0, output)
        self.assert_initial_requests(requests, 10)
        prep = records(output, "[HYBRID-LLM-INITIAL-PREP]")
        self.assertEqual([item["event"] for item in prep], ["start", "end"], output)
        self.assertEqual(prep[-1]["submitted_groups"], "10")
        self.assertEqual(prep[-1]["completed_groups"], "10")
        self.assertEqual(prep[-1]["buffered_responses"], "10")
        prep_end = output.index("[HYBRID-LLM-INITIAL-PREP] event=end")
        first_accept = output.index("[HYBRID-LLM-ROLLOUT] event=response_accepted")
        first_edge = output.index("[HYBRID-LLM-ROLLOUT] event=edge_processed")
        self.assertLess(prep_end, first_accept)
        self.assertLess(prep_end, first_edge)
        accepted = records(output, "[HYBRID-LLM-ROLLOUT] event=response_accepted")
        self.assertEqual({item["request_id"] for item in accepted},
                         {item["request_id"] for item in requests}, output)
        rollout = records(output, "[HYBRID-LLM-ROLLOUT-STATS]")[0]
        self.assertEqual(rollout["llm_actions_processed"], "10", output)
        self.assertEqual(rollout["llm_responses_rejected_budget"], "0", output)
        trigger = records(output, "[HYBRID-LLM-TRIGGER-STATS]")[0]
        self.assertEqual(trigger["usable_responses"], "10", output)
        self.assertGreater(int(trigger["base_expansions"]), 0, output)
        self.assertLessEqual(int(trigger["max_pending"]), 2, output)

    def test_shared_deadline_interrupts_preparation_before_native_expansion(self):
        completed, requests, elapsed = self.run_planner(10, delay=5, seconds=0.3)
        output = completed.stdout
        self.assertGreater(len(requests), 0, output)
        self.assertLessEqual(len(requests), 2, output)
        self.assert_initial_requests(requests, len(requests))
        self.assertIn("[NLM-ANYTIME-RUN-TIMEOUT]", output)
        prep = records(output, "[HYBRID-LLM-INITIAL-PREP]")
        self.assertEqual([item["event"] for item in prep],
                         ["start", "interrupted"], output)
        self.assertEqual(prep[-1]["completed_groups"], "0", output)
        self.assertNotIn("event=edge_processed", output)
        trigger = records(output, "[HYBRID-LLM-TRIGGER-STATS]")[0]
        self.assertEqual(trigger["base_expansions"], "0", output)
        self.assertGreater(int(trigger["active_cancelled"]), 0, output)
        # A blocked HTTP call must not force us to wait five seconds, let
        # alone its ten-second HTTP timeout, after the shared search deadline.
        self.assertLess(elapsed, 3, output)

    def test_each_anytime_phase_restarts_at_the_original_initial_state(self):
        completed, requests, _ = self.run_planner(3, phases=2)
        output = completed.stdout
        self.assertEqual(completed.returncode, 0, output)
        self.assert_initial_requests(requests, 6)
        self.assertEqual({item["iteration"] for item in requests}, {1, 2}, output)
        for iteration in (1, 2):
            self.assertEqual(sum(item["iteration"] == iteration
                                 for item in requests), 3)
        prep = records(output, "[HYBRID-LLM-INITIAL-PREP]")
        self.assertEqual([(item["iteration"], item["event"]) for item in prep],
                         [("1", "start"), ("1", "end"),
                          ("2", "start"), ("2", "end")], output)
        triggers = records(output, "[HYBRID-LLM-TRIGGER-STATS]")
        self.assertEqual(len(triggers), 2, output)
        self.assertEqual([item["requests_submitted"] for item in triggers],
                         ["3", "3"], output)


if __name__ == "__main__":
    unittest.main()
