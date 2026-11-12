"""Regression cases for HTTP requests that overtake native stdout phase logs.

These tests use only Python's standard library and never start the planner,
model server or GPU. They are intentionally separate from the native mock-HTTP
tests: those mocks do not exercise ActiveIterationRegistry.
"""

import concurrent.futures
import csv
import pathlib
import tempfile
import threading
import unittest

from hybrid_planner.anytime import ActiveIterationRegistry, AnytimeRunRecorder


class IterationSyncTests(unittest.TestCase):
    def test_next_request_can_precede_old_end_and_new_start(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)

        self.assertTrue(registry.accept_request("run", 2))
        self.assertTrue(registry.is_active("run", 2))
        self.assertFalse(registry.is_active("run", 1))
        registry.end_iteration(1)
        self.assertFalse(registry.start_iteration(1))
        self.assertTrue(registry.start_iteration(2))
        self.assertTrue(registry.is_active("run", 2))
        self.assertTrue(registry.accept_request("run", 2))

    def test_request_after_old_end_does_not_need_new_start_log(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        registry.end_iteration(1)
        self.assertTrue(registry.accept_request("run", 2))
        self.assertTrue(registry.is_active("run", 2))
        registry.start_iteration(2)
        self.assertTrue(registry.is_active("run", 2))

    def test_ten_concurrent_third_phase_requests_survive_delayed_logs(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(2)
        barrier = threading.Barrier(10)

        def accept(index):
            barrier.wait(timeout=5)
            accepted = registry.accept_request("run", 3)
            future = concurrent.futures.Future()
            registered = registry.register_future("run", 3, "p3-%d" % index, future)
            return accepted, registered, future

        with concurrent.futures.ThreadPoolExecutor(max_workers=10) as executor:
            results = list(executor.map(accept, range(10)))
        self.assertTrue(all(accepted and registered for accepted, registered, _ in results))
        registry.end_iteration(1)
        registry.end_iteration(2)
        registry.start_iteration(1)
        registry.start_iteration(2)
        self.assertTrue(registry.is_active("run", 3))
        self.assertTrue(registry.start_iteration(3))
        self.assertTrue(all(not future.cancelled() for _, _, future in results))
        self.assertEqual(len(registry.end_iteration(3)), 10)
        self.assertTrue(all(future.cancelled() for _, _, future in results))

    def test_ended_phase_cannot_be_revived_by_duplicate_start_or_request(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        registry.end_iteration(1)
        self.assertFalse(registry.accept_request("run", 1))
        self.assertFalse(registry.start_iteration(1))
        self.assertFalse(registry.is_active("run", 1))
        self.assertFalse(registry.register_future("run", 1, "late", concurrent.futures.Future()))
        self.assertTrue(registry.accept_request("run", 2))

    def test_end_seen_before_start_still_prevents_resurrection(self):
        registry = ActiveIterationRegistry("run")
        registry.end_iteration(2)
        for iteration in (1, 2):
            self.assertFalse(registry.start_iteration(iteration))
            self.assertFalse(registry.accept_request("run", iteration))
        self.assertTrue(registry.accept_request("run", 3))

    def test_closed_run_is_terminal_even_for_newer_phase(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        future = concurrent.futures.Future()
        registry.register_future("run", 1, "p1", future)
        self.assertEqual(registry.close(), (1, [("p1", True)]))
        for iteration in (1, 2, 10):
            self.assertFalse(registry.accept_request("run", iteration))
            self.assertFalse(registry.start_iteration(iteration))
            self.assertFalse(registry.is_active("run", iteration))
            self.assertFalse(registry.register_future("run", iteration, "late", concurrent.futures.Future()))
        self.assertEqual(registry.close(), (None, []))

    def test_close_before_any_lifecycle_cannot_bootstrap_again(self):
        registry = ActiveIterationRegistry("run")
        self.assertEqual(registry.close(), (None, []))
        self.assertFalse(registry.accept_request("run", 1))
        self.assertFalse(registry.start_iteration(1))

    def test_wrong_run_and_nonpositive_iteration_do_not_change_active_phase(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        self.assertFalse(registry.accept_request("other-run", 999))
        for iteration in (0, -1):
            self.assertFalse(registry.accept_request("run", iteration))
            self.assertFalse(registry.start_iteration(iteration))
        self.assertTrue(registry.is_active("run", 1))
        self.assertTrue(registry.accept_request("run", 2))

    def test_new_phase_does_not_allow_old_requests_or_late_responses(self):
        registry = ActiveIterationRegistry("run")
        self.assertTrue(registry.accept_request("run", 1))
        self.assertTrue(registry.accept_request("run", 2))
        self.assertFalse(registry.accept_request("run", 1))
        self.assertFalse(registry.is_active("run", 1))
        self.assertFalse(registry.register_future("run", 1, "late-old", concurrent.futures.Future()))
        self.assertTrue(registry.is_active("run", 2))

    def test_delayed_end_cancels_only_its_own_futures(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        first = concurrent.futures.Future()
        second = concurrent.futures.Future()
        registry.register_future("run", 1, "p1", first)
        registry.accept_request("run", 2)
        registry.register_future("run", 2, "p2", second)

        # An overtaking request cancels old model work immediately, while
        # each phase's END still owns its cancellation statistics.
        self.assertFalse(registry.is_active("run", 1))
        self.assertTrue(first.cancelled())
        registry.unregister_future("p1")
        self.assertEqual(registry.end_iteration(1), [("p1", True)])
        self.assertTrue(first.cancelled())
        self.assertFalse(second.cancelled())
        self.assertTrue(registry.is_active("run", 2))
        self.assertEqual(registry.end_iteration(1), [])
        self.assertEqual(registry.end_iteration(2), [("p2", True)])

    def test_cancel_callbacks_can_reenter_registry_without_deadlock(self):
        for action in ("request", "start", "end", "close"):
            with self.subTest(action=action):
                registry = ActiveIterationRegistry("run")
                registry.start_iteration(1)
                future = concurrent.futures.Future()
                callback_states = []

                def callback(_future):
                    registry.unregister_future("p1")
                    callback_states.append(registry.is_active("run", 2))

                future.add_done_callback(callback)
                registry.register_future("run", 1, "p1", future)
                result = []
                errors = []

                def finish():
                    try:
                        if action == "request":
                            result.append(registry.accept_request("run", 2))
                        elif action == "start":
                            result.append(registry.start_iteration(2))
                        elif action == "end":
                            result.append(registry.end_iteration(1))
                        else:
                            result.append(registry.close())
                    except BaseException as exc:
                        errors.append(exc)

                # A broken implementation must fail this test rather than hang
                # the whole suite inside Future.cancel()'s synchronous callback.
                worker = threading.Thread(target=finish, daemon=True)
                worker.start()
                worker.join(timeout=2)
                self.assertFalse(worker.is_alive(), "registry lock held during future.cancel()")
                self.assertEqual(errors, [])
                self.assertEqual(callback_states, [action in {"request", "start"}])
                self.assertTrue(result)

    def test_new_start_cancels_old_work_and_preserves_end_accounting(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        first = concurrent.futures.Future()
        registry.register_future("run", 1, "p1", first)
        self.assertTrue(registry.start_iteration(2))
        self.assertTrue(first.cancelled())
        self.assertFalse(registry.start_iteration(1))
        self.assertFalse(registry.accept_request("run", 1))
        self.assertEqual(registry.end_iteration(1), [("p1", True)])
        self.assertEqual(registry.end_iteration(1), [])
        self.assertTrue(registry.is_active("run", 2))

    def test_close_keeps_retired_futures_attributed_to_original_phase(self):
        registry = ActiveIterationRegistry("run")
        futures = []
        for iteration in (1, 2, 3):
            registry.accept_request("run", iteration)
            future = concurrent.futures.Future()
            futures.append(future)
            registry.register_future("run", iteration, "p%d" % iteration, future)
        registry.unregister_future("p1")
        registry.unregister_future("p2")
        self.assertTrue(all(future.cancelled() for future in futures[:2]))
        self.assertFalse(futures[2].cancelled())
        self.assertEqual(
            registry.close(by_iteration=True),
            (3, {1: [("p1", True)], 2: [("p2", True)], 3: [("p3", True)]}),
        )
        self.assertEqual(registry.close(by_iteration=True), (None, {}))
        self.assertEqual(registry.end_iteration(1), [])
        self.assertEqual(registry.end_iteration(2), [])
        self.assertEqual(registry.end_iteration(3), [])

    def test_end_then_close_does_not_count_retired_future_twice(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        first = concurrent.futures.Future()
        registry.register_future("run", 1, "p1", first)
        registry.start_iteration(2)
        second = concurrent.futures.Future()
        registry.register_future("run", 2, "p2", second)
        self.assertEqual(registry.end_iteration(1), [("p1", True)])
        self.assertEqual(registry.close(by_iteration=True), (2, {2: [("p2", True)]}))
        self.assertEqual(registry.end_iteration(1), [])

    def test_completed_future_is_recorded_as_late_cancellation(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(1)
        future = concurrent.futures.Future()
        registry.register_future("run", 1, "p1", future)
        future.set_result([])
        registry.accept_request("run", 2)
        self.assertFalse(future.cancelled())
        self.assertEqual(registry.end_iteration(1), [("p1", False)])
        self.assertEqual(registry.close(by_iteration=True), (2, {}))

    def test_end_or_close_during_phase_advance_cancellation_keeps_accounting(self):
        for action in ("end", "close"):
            with self.subTest(action=action):
                entered_cancel = threading.Event()
                release_cancel = threading.Event()

                class BlockingFirstCancelFuture(concurrent.futures.Future):
                    def __init__(self):
                        super().__init__()
                        self.cancel_calls = 0
                        self.cancel_lock = threading.Lock()

                    def cancel(self):
                        with self.cancel_lock:
                            self.cancel_calls += 1
                            first_call = self.cancel_calls == 1
                        if first_call:
                            entered_cancel.set()
                            if not release_cancel.wait(timeout=5):
                                raise RuntimeError("test did not release cancellation")
                        return super().cancel()

                registry = ActiveIterationRegistry("run")
                registry.start_iteration(1)
                first = BlockingFirstCancelFuture()
                registry.register_future("run", 1, "p1", first)
                errors = []
                results = []

                def advance():
                    try:
                        registry.accept_request("run", 2)
                    except BaseException as exc:
                        errors.append(exc)

                def finish():
                    try:
                        if action == "end":
                            results.append(registry.end_iteration(1))
                        else:
                            results.append(registry.close(by_iteration=True))
                    except BaseException as exc:
                        errors.append(exc)

                advancing = threading.Thread(target=advance, daemon=True)
                finishing = threading.Thread(target=finish, daemon=True)
                advancing.start()
                try:
                    self.assertTrue(entered_cancel.wait(timeout=2))
                    finishing.start()
                    finishing.join(timeout=2)
                    self.assertFalse(finishing.is_alive(), "cancellation held the registry lock")
                finally:
                    release_cancel.set()
                    advancing.join(timeout=2)
                    if finishing.ident is not None:
                        finishing.join(timeout=2)
                self.assertFalse(advancing.is_alive())
                self.assertEqual(errors, [])
                expected = [("p1", True)] if action == "end" else (2, {1: [("p1", True)]})
                self.assertEqual(results, [expected])
                self.assertTrue(first.cancelled())
                self.assertEqual(registry.end_iteration(1), [])
                self.assertEqual(registry.close(by_iteration=True), (2 if action == "end" else None, {}))

    def test_phase_end_and_same_phase_request_race_cannot_reopen_phase(self):
        registry = ActiveIterationRegistry("run")
        registry.start_iteration(2)
        barrier = threading.Barrier(2)

        def end():
            barrier.wait(timeout=5)
            registry.end_iteration(2)

        def request():
            barrier.wait(timeout=5)
            return registry.accept_request("run", 2)

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            ended = executor.submit(end)
            accepted = executor.submit(request)
            ended.result(timeout=5)
            accepted.result(timeout=5)
        self.assertFalse(registry.is_active("run", 2))
        self.assertFalse(registry.accept_request("run", 2))
        self.assertTrue(registry.accept_request("run", 3))

    def test_direct_search_can_start_from_request_without_stdout_markers(self):
        registry = ActiveIterationRegistry("single-pass")
        self.assertTrue(registry.accept_request("single-pass", 1))
        self.assertTrue(registry.accept_request("single-pass", 1))
        self.assertTrue(registry.is_active("single-pass", 1))
        registry.close()
        self.assertFalse(registry.accept_request("single-pass", 1))


class IterationSyncRecorderTests(unittest.TestCase):
    def test_delayed_old_logs_do_not_stale_new_phase_request_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            registry = ActiveIterationRegistry("recorded")
            recorder = AnytimeRunRecorder(temporary, "recorded", "live", registry)
            recorder.handle_planner_line("[NLM-ANYTIME-PHASE-START] iteration=1 bound=999\n")
            request = {"run_id": "recorded", "request_id": "p2-initial", "iteration": 2,
                       "state_id": 0, "reason": "initial_batch"}
            recorder.request_received(request)
            self.assertTrue(registry.accept_request("recorded", 2))
            future = concurrent.futures.Future()
            self.assertTrue(registry.register_future("recorded", 2, "p2-initial", future))
            recorder.model_started("p2-initial", 3)

            recorder.handle_planner_line("[NLM-ANYTIME-PHASE-END] iteration=1 result=solved\n")
            recorder.handle_planner_line("[NLM-ANYTIME-PHASE-START] iteration=2 bound=100\n")
            self.assertTrue(registry.is_active("recorded", 2))
            self.assertFalse(future.cancelled())
            future.set_result([])
            registry.unregister_future("p2-initial")
            recorder.request_finished("p2-initial", "partial", sample_count=3, usable_sample_count=3)
            recorder.handle_planner_line("[NLM-ANYTIME-PHASE-END] iteration=2 result=solved\n")
            recorder.close(return_code=0)

            with (pathlib.Path(temporary) / "llm_requests.csv").open(encoding="utf-8", newline="") as stream:
                row = next(csv.DictReader(stream))
            self.assertEqual(row["status"], "partial")
            self.assertEqual(row["model_generations_started"], "3")
            with (pathlib.Path(temporary) / "phases.csv").open(encoding="utf-8", newline="") as stream:
                phases = {int(row["iteration"]): row for row in csv.DictReader(stream)}
            self.assertEqual(phases[1]["python_cancel_requested"], "0")
            self.assertEqual(phases[2]["python_cancel_requested"], "0")
            self.assertEqual(phases[2]["model_generations"], "3")


if __name__ == "__main__":
    unittest.main()
