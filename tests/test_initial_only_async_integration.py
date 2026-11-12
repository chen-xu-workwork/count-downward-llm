"""Container-only native tests for asynchronous initial-state guidance.

These use an existing release64 build and a controlled HTTP server, never a
model/GPU. The old serial initial-only tests remain in their original module.
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

from tests.test_initial_only_integration import (
    BINARY, PREPROCESSOR, TOOLCHAIN_PRESENT, TRANSLATOR, records,
)


BITS = tuple("b%02d" % index for index in range(22))
DOMAIN = """(define (domain initial-async-switches)
  (:requirements :strips :typing)
  (:types bit)
  (:predicates (off ?b - bit) (on ?b - bit))
  (:action switch-on
    :parameters (?b - bit)
    :precondition (off ?b)
    :effect (and (not (off ?b)) (on ?b))))
"""
PROBLEM = """(define (problem initial-async-switches-test)
  (:domain initial-async-switches)
  (:objects %s - bit)
  (:init %s)
  (:goal (and %s)))
""" % (" ".join(BITS), " ".join("(off %s)" % bit for bit in BITS),
       " ".join("(on %s)" % bit for bit in BITS))
CHAIN = ["(switch-on %s)" % bit for bit in BITS]


def lazy(max_time=None):
    suffix = ",max_time=%s" % max_time if max_time is not None else ""
    return "lazy_greedy(h,preferred=h,cost_type=one,reopen_closed=false,llm_h=h%s)" % suffix


class Trace:
    def __init__(self):
        self.lines = []
        self.requests = []
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.phase_started = {phase: threading.Event() for phase in (1, 2)}
        self.native_progress = {phase: threading.Event() for phase in (1, 2)}
        self.initial_slots_received = {phase: threading.Event() for phase in (1, 2)}
        self.old_reply_attempted = threading.Event()

    def observe(self, line):
        self.lines.append(line)
        if "[NLM-ANYTIME-PHASE-START]" in line:
            phase = int(re.search(r"iteration=(\d+)", line).group(1))
            self.phase_started.setdefault(phase, threading.Event()).set()
        if "[HYBRID-LLM-INITIAL-ASYNC] event=native_progress" in line:
            phase = int(re.search(r"iteration=(\d+)", line).group(1))
            self.native_progress.setdefault(phase, threading.Event()).set()

    def wait_for(self, event):
        while not self.stopping.is_set():
            if event.wait(0.02):
                return True
        return False


@contextmanager
def controlled_server(trace, scenario):
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(size).decode("utf-8"))
            phase = int(request["iteration"])
            index = int(request["request_id"].rsplit("-", 1)[1])
            with trace.lock:
                trace.requests.append(request)
                if sum(item["iteration"] == phase for item in trace.requests) >= 2:
                    trace.initial_slots_received[phase].set()
            if scenario == "never_reply":
                trace.stopping.wait()
                return
            if scenario == "complete_budget":
                if not trace.wait_for(trace.native_progress[phase]):
                    return
            elif scenario == "late_old_reply" and phase == 1:
                if not trace.wait_for(trace.phase_started[2]):
                    return
                # Deliberately attempt an old response only after native search
                # announced phase 2. Its old bridge socket must already be shut.
                trace.old_reply_attempted.set()
            elif index == 0:
                if not trace.wait_for(trace.native_progress[phase]):
                    return
                if not trace.wait_for(trace.initial_slots_received[phase]):
                    return
                if scenario == "late_old_reply" and not trace.wait_for(trace.old_reply_attempted):
                    return
            else:
                # One response is enough to solve. Other requests deliberately
                # stay outstanding so the test detects an accidental barrier.
                trace.stopping.wait()
                return
            actions = CHAIN[:1] if scenario == "complete_budget" else CHAIN
            payload = json.dumps({"status": "ok", "actions": actions,
                                  "action_chains": [actions]}).encode("utf-8")
            try:
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass

        def log_message(self, _format, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=lambda: server.serve_forever(poll_interval=0.02), daemon=True)
    worker.start()
    try:
        yield server.server_port
    finally:
        trace.stopping.set()
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)


@unittest.skipUnless(sys.platform.startswith("linux"), "requires Linux/WSL")
@unittest.skipUnless(TOOLCHAIN_PRESENT, "build release64 before integration test")
class InitialOnlyAsyncIntegrationTests(unittest.TestCase):
    def run_planner(self, scenario, *, seconds=20, phases=None):
        trace = Trace()
        with tempfile.TemporaryDirectory(prefix="count-initial-async-") as temporary:
            work = Path(temporary)
            domain, problem = work / "domain.pddl", work / "problem.pddl"
            domain.write_text(DOMAIN, encoding="utf-8")
            problem.write_text(PROBLEM, encoding="utf-8")
            translated = subprocess.run(
                [sys.executable, str(TRANSLATOR), str(domain), str(problem)], cwd=work,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30,
            )
            self.assertEqual(translated.returncode, 0, translated.stdout)
            with (work / "output.sas").open("rb") as source:
                preprocessed = subprocess.run(
                    [str(PREPROCESSOR)], cwd=work, stdin=source, text=True,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
                )
            self.assertEqual(preprocessed.returncode, 0, preprocessed.stdout)
            self.assertTrue((work / "output").is_file(), preprocessed.stdout)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("HYBRID_LLM_", "NLM_LLM_"))}
            env.update({
                "HYBRID_LLM_TRIGGER": "1", "HYBRID_LLM_QUERY_POLICY": "initial-only",
                "HYBRID_LLM_INITIAL_ASYNC": "1", "HYBRID_LLM_COMM_MODE": "http",
                "HYBRID_LLM_HTTP_HOST": "127.0.0.1", "HYBRID_LLM_HTTP_PATH": "/llm/request",
                "HYBRID_LLM_HTTP_TIMEOUT_MS": "30000", "HYBRID_LLM_HTTP_WORKERS": "2",
                "HYBRID_LLM_MAX_PENDING": "2", "HYBRID_LLM_MAX_REQUESTS": "10",
                "HYBRID_LLM_MAX_QUEUED_BURSTS": "8", "HYBRID_LLM_RUN_ID": "initial-async-test",
            })
            searches = phases or [lazy()]
            search = ("iterated([" + ",".join(searches) + "],pass_bound=false,repeat_last=false,"
                      "continue_on_solve=true,continue_on_fail=true,max_time=" + str(seconds) + ")")
            with controlled_server(trace, scenario) as port:
                env["HYBRID_LLM_HTTP_PORT"] = str(port)
                with (work / "output").open("rb") as source:
                    process = subprocess.Popen(
                        [str(BINARY), "--heuristic", "h=blind(cost_type=one)", "--search", search,
                         "--internal-plan-file", str(work / "sas_plan")], cwd=work, env=env,
                        stdin=source, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                        text=True, bufsize=1,
                    )
                    # Consume complete lines immediately: whole-output buffering
                    # would deadlock the controlled HTTP response release.
                    def read_lines():
                        for line in process.stdout:
                            trace.observe(line)
                    reader = threading.Thread(target=read_lines, daemon=True)
                    reader.start()
                    started = time.monotonic()
                    try:
                        code = process.wait(timeout=seconds + 15)
                    finally:
                        trace.stopping.set()
                        if process.poll() is None:
                            process.kill()
                            process.wait(timeout=5)
                        reader.join(timeout=5)
                        process.stdout.close()
                    elapsed = time.monotonic() - started
            self.assertFalse(reader.is_alive(), "native stdout reader failed to finish")
        return code, "".join(trace.lines), trace, elapsed

    def assert_only_initial_sources(self, requests):
        self.assertTrue(requests)
        self.assertEqual({item["reason"] for item in requests}, {"initial_batch"})
        self.assertEqual({item["g"] for item in requests}, {0})
        self.assertEqual(len({item["state_id"] for item in requests}), 1)
        self.assertEqual(len({item["init"] for item in requests}), 1)

    def test_native_expands_while_initial_requests_have_no_replies(self):
        _, output, trace, _ = self.run_planner("never_reply", seconds=1)
        self.assert_only_initial_sources(trace.requests)
        self.assertTrue(trace.native_progress[1].is_set(), output)
        trigger = records(output, "[HYBRID-LLM-TRIGGER-STATS]")[0]
        self.assertGreater(int(trigger["base_expansions"]), 0, output)
        self.assertEqual(trigger["responses_completed"], "0", output)
        self.assertGreater(int(trigger["active_cancelled"]), 0, output)
        self.assertNotIn("event=response_accepted", output)
        self.assertNotIn("[HYBRID-LLM-INITIAL-PREP]", output)
        self.assertIn("[NLM-ANYTIME-RUN-TIMEOUT]", output)
        events = records(output, "[HYBRID-LLM-INITIAL-ASYNC]")
        self.assertEqual(events[0]["event"], "start")
        self.assertEqual(events[-1]["event"], "interrupted")

    def test_async_collection_replenishes_all_ten_initial_groups_with_bounded_pending(self):
        _, output, trace, _ = self.run_planner("complete_budget", seconds=1)
        self.assert_only_initial_sources(trace.requests)
        self.assertEqual(len(trace.requests), 10, output)
        self.assertEqual(len({item["request_id"] for item in trace.requests}), 10, output)
        first = next(item for item in trace.requests
                     if item["request_id"].endswith("-0"))
        self.assertEqual(first["search_expansions"], 0, output)
        self.assertTrue(any(item["search_expansions"] > 0
                            for item in trace.requests), output)
        trigger = records(output, "[HYBRID-LLM-TRIGGER-STATS]")[0]
        self.assertEqual(trigger["requests_submitted"], "10", output)
        self.assertEqual(trigger["responses_completed"], "10", output)
        self.assertEqual(trigger["usable_responses"], "10", output)
        self.assertLessEqual(int(trigger["max_pending"]), 2, output)
        accepted = records(output, "[HYBRID-LLM-ROLLOUT] event=response_accepted")
        self.assertEqual({item["request_id"] for item in accepted},
                         {item["request_id"] for item in trace.requests}, output)
        events = records(output, "[HYBRID-LLM-INITIAL-ASYNC]")
        self.assertEqual([item["event"] for item in events],
                         ["start", "native_progress", "end"], output)
        self.assertEqual(events[-1]["completed_groups"], "10", output)
        self.assertNotIn("[HYBRID-LLM-INITIAL-PREP]", output)

    def test_first_available_reply_is_replayed_without_waiting_for_full_budget(self):
        code, output, trace, _ = self.run_planner("timely_reply")
        self.assertEqual(code, 0, output)
        self.assert_only_initial_sources(trace.requests)
        self.assertTrue(trace.native_progress[1].is_set(), output)
        self.assertLess(output.index("event=native_progress"), output.index("event=response_accepted"))
        accepted = records(output, "[HYBRID-LLM-ROLLOUT] event=response_accepted")
        self.assertTrue(accepted, output)
        self.assertTrue(all("-p1-" in row["request_id"] for row in accepted), output)
        trigger = records(output, "[HYBRID-LLM-TRIGGER-STATS]")[0]
        self.assertGreaterEqual(int(trigger["requests_submitted"]), 2, output)
        self.assertLess(int(trigger["requests_submitted"]), 10, output)
        self.assertGreater(int(trigger["active_cancelled"]), 0, output)
        self.assertEqual(records(output, "[NLM-ANYTIME-PHASE-END]")[0]["result"], "solved", output)
        rollout = records(output, "[HYBRID-LLM-ROLLOUT-STATS]")[0]
        self.assertGreaterEqual(int(rollout["llm_actions_processed"]), len(CHAIN), output)

    def test_old_phase_reply_cannot_be_injected_after_next_phase_starts(self):
        code, output, trace, _ = self.run_planner("late_old_reply", phases=[lazy(1), lazy()])
        self.assertEqual(code, 0, output)
        self.assert_only_initial_sources(trace.requests)
        self.assertTrue(trace.old_reply_attempted.is_set(), output)
        self.assertEqual({item["iteration"] for item in trace.requests}, {1, 2}, output)
        ended = records(output, "[NLM-ANYTIME-PHASE-END]")
        self.assertEqual([(row["iteration"], row["result"]) for row in ended],
                         [("1", "timeout"), ("2", "solved")], output)
        triggers = records(output, "[HYBRID-LLM-TRIGGER-STATS]")
        self.assertGreater(int(triggers[0]["active_cancelled"]), 0, output)
        self.assertEqual(triggers[0]["usable_responses"], "0", output)
        accepted = records(output, "[HYBRID-LLM-ROLLOUT] event=response_accepted")
        self.assertTrue(accepted, output)
        self.assertTrue(all("-p2-" in row["request_id"] for row in accepted), output)
        self.assertNotRegex(output, r"event=edge_processed request_id=initial-async-test-p1-")


if __name__ == "__main__":
    unittest.main()
