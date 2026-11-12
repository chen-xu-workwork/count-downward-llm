"""Exercise real HTTP admission when phase logs lag behind native requests.

No model/GPU/native executable is needed. Run explicitly in the container:
    python3 -m unittest tests.test_iteration_sync_http -v
"""

import concurrent.futures
import contextlib
from dataclasses import dataclass
from http.server import ThreadingHTTPServer
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock
import urllib.request

from hybrid_planner.anytime import ActiveIterationRegistry, AnytimeRunRecorder
from hybrid_planner.console import make_handler
from hybrid_planner.llm.client import LLMGenerationResult
from hybrid_planner.validation.response_processor import ProcessedModelResponse


@dataclass
class Prompts:
    system: str = 'test system'
    user: str = 'test initial state'
    runtime_problem: str = '(define (problem mock))'
    problem_path: Path = Path('mock.pddl')

    def as_messages(self):
        return [{'role': 'user', 'content': self.user}]


class PromptBuilder:
    def build(self, problem_id, init_text):
        return Prompts()


class Processor:
    def process(self, content, runtime_problem):
        return ProcessedModelResponse(
            status='partial', actions=('(move s0 s1)',),
            generated_action_count=1, legal_action_count=1, goal_reached=False,
        )


class RecordingRuntime:
    def __init__(self, automatic):
        self.automatic = automatic
        self.submitted = threading.Event()
        self.lock = threading.Lock()
        self.calls = []

    @staticmethod
    def finish(future, count):
        results = tuple(LLMGenerationResult(
            content='partial prefix', response={}, error=None,
            attempts=1, elapsed_seconds=0.01,
        ) for _ in range(count))
        try:
            future.set_result(results)
        except concurrent.futures.InvalidStateError:
            # A real phase END may have cancelled the request already.
            pass

    def submit_many(self, messages, count, request_id=''):
        future = concurrent.futures.Future()
        with self.lock:
            self.calls.append((request_id, count, future))
        self.submitted.set()
        if self.automatic:
            self.finish(future, count)
        return future

    def finish_all(self):
        with self.lock:
            # Late handlers must also finish if an assertion aborts a test.
            self.automatic = True
            calls = list(self.calls)
        for _, count, future in calls:
            self.finish(future, count)


class TestServer(ThreadingHTTPServer):
    request_queue_size = 32


class IterationSyncHTTPTests(unittest.TestCase):
    def setUp(self):
        limits = {
            prefix + name: value
            for prefix in ('HYBRID_LLM_', 'NLM_LLM_')
            for name, value in (
                ('MAX_PROPOSALS_PER_RESPONSE', '8'),
                ('MAX_ACTIONS_PER_PROPOSAL', '100'),
                ('MAX_BURST_ACTIONS', '100'),
            )
        }
        patcher = mock.patch.dict(os.environ, limits)
        patcher.start()
        self.addCleanup(patcher.stop)

    @contextlib.contextmanager
    def server(self, automatic=True):
        with tempfile.TemporaryDirectory() as directory:
            registry = ActiveIterationRegistry('sync-http')
            recorder = AnytimeRunRecorder(directory, 'sync-http', 'live', registry)
            runtime = RecordingRuntime(automatic)
            handler = make_handler(
                '/llm/request', PromptBuilder(), llm_runtime=runtime,
                response_processor=Processor(), samples_per_state=3,
                anytime_registry=registry, anytime_recorder=recorder,
            )
            server = TestServer(('127.0.0.1', 0), handler)
            thread = threading.Thread(
                target=lambda: server.serve_forever(poll_interval=0.01), daemon=True,
            )
            clients = concurrent.futures.ThreadPoolExecutor(max_workers=10)
            thread.start()
            try:
                yield server, registry, recorder, runtime, clients
            finally:
                runtime.finish_all()
                clients.shutdown(wait=True)
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)
                recorder.close(return_code=0)

    @staticmethod
    def post(server, iteration, index=0, run_id='sync-http'):
        request = urllib.request.Request(
            'http://127.0.0.1:%d/llm/request' % server.server_port,
            data=json.dumps({
                'run_id': run_id, 'iteration': iteration,
                'request_id': '%s-p%d-0-%d' % (run_id, iteration, index),
                'state_id': 0, 'state_label': '#0', 'reason': 'initial_batch',
                'problem_id': 'mock', 'init': '(:init (at s0))',
            }).encode('utf-8'),
            headers={'Content-Type': 'application/json'}, method='POST',
        )
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.loads(response.read().decode('utf-8'))

    def test_early_request_survives_delayed_previous_end_and_current_start(self):
        with self.server(automatic=False) as (server, registry, recorder, runtime, clients):
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=1\n')
            response = clients.submit(self.post, server, 2)
            self.assertTrue(runtime.submitted.wait(5), 'early phase 2 never reached the model')
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-END] iteration=1 result=solved\n')
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=1\n')
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=2\n')
            self.assertTrue(registry.is_active('sync-http', 2))
            runtime.finish_all()
            result = response.result(timeout=5)
            self.assertEqual(result['status'], 'partial')
            self.assertEqual(result['usable_sample_count'], 3)
            self.assertEqual(len(runtime.calls), 1)
            self.assertFalse(runtime.calls[0][2].cancelled())

    def test_ten_early_groups_reach_model_and_delayed_logs_do_not_roll_back(self):
        with self.server() as (server, registry, recorder, runtime, clients):
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=2\n')
            responses = [clients.submit(self.post, server, 3, index) for index in range(10)]
            results = [response.result(timeout=10) for response in responses]
            self.assertEqual([result['status'] for result in results], ['partial'] * 10)
            self.assertEqual(sum(result['usable_sample_count'] for result in results), 30)
            self.assertEqual(len({call[0] for call in runtime.calls}), 10)
            self.assertEqual(sum(call[1] for call in runtime.calls), 30)
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=2\n')
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-END] iteration=2 result=solved\n')
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=3\n')
            self.assertTrue(registry.is_active('sync-http', 3))
            self.assertEqual(self.post(server, 2, 99)['status'], 'stale_iteration')
            self.assertEqual(self.post(server, 99, run_id='wrong-run')['status'], 'stale_iteration')
            self.assertEqual(len(runtime.calls), 10)
            self.assertTrue(registry.is_active('sync-http', 3))

    def test_real_end_and_run_close_still_reject_late_requests(self):
        with self.server(automatic=False) as (server, registry, recorder, runtime, clients):
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=2\n')
            response = clients.submit(self.post, server, 2)
            self.assertTrue(runtime.submitted.wait(5))
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-END] iteration=2 result=timeout\n')
            self.assertEqual(response.result(timeout=5)['status'], 'stale_iteration')
            self.assertTrue(runtime.calls[0][2].cancelled())
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=2\n')
            self.assertEqual(self.post(server, 2, 1)['status'], 'stale_iteration')
            registry.close()
            self.assertEqual(self.post(server, 3)['status'], 'stale_iteration')
            self.assertEqual(len(runtime.calls), 1)

    def test_new_phase_request_invalidates_pending_reply_before_old_end_log(self):
        with self.server(automatic=False) as (server, registry, recorder, runtime, clients):
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=1\n')
            registered = threading.Event()
            original_register = registry.register_future

            def register(run_id, iteration, request_id, future):
                accepted = original_register(run_id, iteration, request_id, future)
                if iteration == 1 and accepted:
                    registered.set()
                return accepted

            with mock.patch.object(registry, 'register_future', side_effect=register):
                old = clients.submit(self.post, server, 1)
                # submit_many's event precedes registry registration; wait for
                # actual ownership before testing immediate cancellation.
                self.assertTrue(registered.wait(5))
                old_future = runtime.calls[0][2]
                runtime.submitted.clear()
                new = clients.submit(self.post, server, 2)
                self.assertTrue(runtime.submitted.wait(5), 'new phase was rejected before its START log')
                self.assertTrue(registry.is_active('sync-http', 2))
                self.assertTrue(old_future.cancelled())
                runtime.finish_all()
                self.assertEqual(old.result(timeout=5)['status'], 'stale_iteration')
                self.assertEqual(new.result(timeout=5)['status'], 'partial')
            # The old END arrives only after both HTTP replies. It must not
            # cancel/reject the newer phase or allow the old one to restart.
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-END] iteration=1 result=solved\n')
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=1\n')
            recorder.handle_planner_line('[NLM-ANYTIME-PHASE-START] iteration=2\n')
            self.assertTrue(registry.is_active('sync-http', 2))
            self.assertEqual(self.post(server, 1, 9)['status'], 'stale_iteration')
            self.assertEqual(len(runtime.calls), 2)

    def test_phase_change_during_validation_discards_already_generated_reply(self):
        entered = threading.Event()
        release = threading.Event()
        guard = threading.Lock()
        calls = []
        original_process = Processor.process

        def gated_process(processor, content, runtime_problem):
            with guard:
                calls.append(1)
                first = len(calls) == 1
            if first:
                entered.set()
                if not release.wait(5):
                    raise RuntimeError('validation test gate timed out')
            return original_process(processor, content, runtime_problem)

        with mock.patch.object(Processor, 'process', gated_process):
            with self.server() as (server, registry, recorder, runtime, clients):
                old = clients.submit(self.post, server, 1)
                try:
                    self.assertTrue(entered.wait(5))
                    # Model output exists, but validation has not finished.
                    # A newer HTTP request advances the phase without logs.
                    new = self.post(server, 2)
                    self.assertEqual(new['status'], 'partial')
                finally:
                    release.set()
                old_result = old.result(timeout=5)
                self.assertEqual(old_result['status'], 'stale_iteration')
                self.assertFalse(old_result.get('action_chains'))
                self.assertTrue(registry.is_active('sync-http', 2))


if __name__ == '__main__':
    unittest.main()
