import os
import types
import unittest
from unittest import mock

from hybrid_planner.console import (
    DEFAULT_SATISFICING_HEURISTIC,
    DEFAULT_SATISFICING_SEARCH,
    DEFAULT_SEARCH_TIME_LIMIT_SECONDS,
    bound_action_chains,
    build_satisficing_search,
    build_single_pass_search,
    configure_planner_environment,
)


class ConsoleEnvironmentTests(unittest.TestCase):
    def test_default_search_is_repeated_satisficing_anytime(self):
        self.assertTrue(
            DEFAULT_SATISFICING_SEARCH.startswith("iterated([lazy_greedy(")
        )
        self.assertEqual(DEFAULT_SATISFICING_HEURISTIC, "hff=irhff(cost_type=one)")
        self.assertIn("lazy_wastar(hff,preferred=hff,w=5", DEFAULT_SATISFICING_SEARCH)
        self.assertIn("lazy_wastar(hff,preferred=hff,w=1", DEFAULT_SATISFICING_SEARCH)
        self.assertIn("llm_h=hff", DEFAULT_SATISFICING_SEARCH)
        self.assertIn("pass_bound=true", DEFAULT_SATISFICING_SEARCH)
        self.assertIn("repeat_last=true", DEFAULT_SATISFICING_SEARCH)
        self.assertIn("continue_on_fail=true", DEFAULT_SATISFICING_SEARCH)
        self.assertIn("max_time=7200", DEFAULT_SATISFICING_SEARCH)
        self.assertNotIn("eager(", DEFAULT_SATISFICING_SEARCH)

    def test_search_time_limit_is_applied_at_the_controlling_level(self):
        anytime = build_satisficing_search(17.5)
        self.assertTrue(anytime.startswith("iterated(["))
        self.assertTrue(anytime.endswith("max_time=17.5)"))
        self.assertEqual(anytime.count("max_time="), 1)

        single_pass = build_single_pass_search(9)
        self.assertTrue(single_pass.startswith("lazy_greedy("))
        self.assertTrue(single_pass.endswith("max_time=9)"))
        self.assertEqual(single_pass.count("max_time="), 1)
        self.assertEqual(DEFAULT_SEARCH_TIME_LIMIT_SECONDS, 7200.0)

    def test_live_defaults_are_bounded_and_conservative(self):
        args = types.SimpleNamespace(
            host="127.0.0.1",
            actual_port=8765,
            path="/llm/request",
            pending_behavior="skip",
            emit_state="0",
            llm_timeout=300.0,
            http_workers=0,
            llm_max_concurrency=100,
            llm_samples_per_state=3,
            llm_mode="live",
        )
        with mock.patch.dict(os.environ, {}, clear=True):
            env = configure_planner_environment(args, "problem-1")

        self.assertEqual(env["NLM_LLM_HTTP_WORKERS"], "100")
        self.assertEqual(env["HYBRID_LLM_TRIGGER"], "1")
        self.assertEqual(env["HYBRID_LLM_HTTP_PORT"], "8765")
        self.assertEqual(env["NLM_LLM_MAX_PENDING"], "33")
        self.assertEqual(env["NLM_LLM_MAX_REQUESTS"], "10")
        self.assertEqual(env["NLM_LLM_MAX_PROPOSALS_PER_RESPONSE"], "8")
        self.assertEqual(env["NLM_LLM_MAX_ACTIONS_PER_PROPOSAL"], "100")
        self.assertEqual(env["NLM_LLM_MAX_BURST_ACTIONS"], "100")
        self.assertEqual(env["NLM_LLM_RUN_ID"], "problem-1")
        self.assertEqual(env["NLM_LLM_ENABLE_EXPANSION_PLATEAU"], "1")
        self.assertEqual(env["NLM_LLM_PLATEAU_WINDOW_EXPANSIONS"], "65536")
        self.assertEqual(env["NLM_LLM_PLATEAU_CONFIRM_WINDOWS"], "3")
        self.assertEqual(env["NLM_LLM_PLATEAU_RESET_WINDOWS"], "2")
        self.assertEqual(
            env["NLM_LLM_PLATEAU_MIN_BUCKET_EXPANSIONS"], "16384"
        )
        self.assertEqual(
            env["NLM_LLM_PLATEAU_MIN_SINCE_REQUEST_EXPANSIONS"], "65536"
        )
        self.assertEqual(env["NLM_LLM_PLATEAU_MIN_SHARE"], "0.25")
        self.assertNotIn("NLM_LLM_PLATEAU_MAX_LOWER_SHARE", env)
        self.assertEqual(env["NLM_LLM_PLATEAU_H_BUCKET_WIDTH"], "0.001")
        self.assertEqual(
            env["NLM_LLM_PLATEAU_PER_LAYER_REQUEST_GAP_EXPANSIONS"],
            "500000",
        )
        self.assertEqual(env["NLM_LLM_ANALYSIS_INTERVAL"], "8192")
        self.assertEqual(env["NLM_LLM_ACTIVITY_WINDOWS"], "4")
        self.assertEqual(env["NLM_LLM_GROWTH_CONFIRM_WINDOWS"], "2")
        self.assertEqual(env["NLM_LLM_LAYER_RESET_WINDOWS"], "4")
        self.assertEqual(
            env["NLM_LLM_LAYER_MIN_RECENT_EXPANDED"], "4096"
        )
        self.assertEqual(
            env["NLM_LLM_LAYER_MIN_RECENT_NET_GROWTH"], "1024"
        )
        self.assertEqual(
            env["NLM_LLM_LAYER_MIN_SINCE_REQUEST_EXPANDED"], "8192"
        )
        self.assertEqual(
            env["NLM_LLM_LAYER_MIN_SINCE_REQUEST_NET_GROWTH"], "2048"
        )
        self.assertEqual(env["NLM_LLM_PLATEAU_GROWTH_RATIO"], "1.05")
        self.assertEqual(env["NLM_LLM_STALL_EXPANSIONS"], "500000")
        self.assertEqual(env["NLM_LLM_ANCESTOR_CHECK_INTERVAL"], "100000")
        self.assertEqual(env["NLM_LLM_ANCESTOR_DEPTH"], "20")
        self.assertEqual(env["NLM_LLM_MIN_DEPTH"], "30")
        self.assertEqual(
            env["NLM_LLM_MIN_REQUEST_GAP_EXPANSIONS"], "500000"
        )
        self.assertEqual(
            env["NLM_LLM_PER_LAYER_REQUEST_GAP_EXPANSIONS"], "500000"
        )
        self.assertEqual(env["NLM_LLM_CANDIDATE_LAYERS"], "3")
        self.assertEqual(env["NLM_LLM_REQUESTS_PER_SLOT"], "1")
        self.assertEqual(env["NLM_LLM_HEARTBEAT_INTERVAL"], "100000")
        self.assertEqual(env["NLM_LLM_H_RELATIVE_EPSILON"], "0.005")

    def test_existing_trigger_overrides_are_preserved(self):
        args = types.SimpleNamespace(
            host="127.0.0.1",
            actual_port=8765,
            path="/llm/request",
            pending_behavior="normal",
            emit_state="0",
            llm_timeout=120.0,
            http_workers=12,
            llm_max_concurrency=100,
            llm_samples_per_state=3,
            llm_mode="live",
        )
        with mock.patch.dict(
            os.environ,
            {
                "HYBRID_LLM_TRIGGER": "0",
                "HYBRID_LLM_HTTP_PORT": "9999",
                "NLM_LLM_MAX_PENDING": "7",
                "NLM_LLM_MAX_REQUESTS": "5",
                "NLM_LLM_ANALYSIS_INTERVAL": "9",
                "NLM_LLM_PLATEAU_MIN_SHARE": "0.7",
            },
            clear=True,
        ):
            env = configure_planner_environment(args, "problem-1")

        self.assertEqual(env["NLM_LLM_HTTP_WORKERS"], "12")
        self.assertEqual(env["HYBRID_LLM_TRIGGER"], "1")
        self.assertEqual(env["HYBRID_LLM_HTTP_PORT"], "8765")
        self.assertEqual(env["NLM_LLM_MAX_PENDING"], "7")
        self.assertEqual(env["NLM_LLM_MAX_REQUESTS"], "5")
        self.assertEqual(env["NLM_LLM_ANALYSIS_INTERVAL"], "9")
        self.assertEqual(env["NLM_LLM_PLATEAU_MIN_SHARE"], "0.7")

    def test_off_mode_forces_a_true_no_bridge_baseline(self):
        args = types.SimpleNamespace(
            llm_mode="off",
            run_id="baseline-run",
        )
        with mock.patch.dict(
            os.environ,
            {
                "HYBRID_LLM_TRIGGER": "1",
                "HYBRID_LLM_COMM_MODE": "http",
                "HYBRID_LLM_MAX_REQUESTS": "88",
                "NLM_LLM_TRIGGER": "1",
                "NLM_LLM_COMM_MODE": "http",
                "NLM_LLM_MAX_REQUESTS": "99",
            },
            clear=True,
        ):
            env = configure_planner_environment(args, "problem-1")

        self.assertEqual(env["NLM_LLM_TRIGGER"], "0")
        self.assertEqual(env["HYBRID_LLM_TRIGGER"], "0")
        self.assertEqual(env["NLM_LLM_COMM_MODE"], "off")
        self.assertEqual(env["HYBRID_LLM_COMM_MODE"], "off")
        self.assertEqual(env["NLM_LLM_MAX_REQUESTS"], "0")
        self.assertEqual(env["HYBRID_LLM_MAX_REQUESTS"], "0")
        self.assertEqual(env["NLM_LLM_EMIT_STATE"], "0")
        self.assertEqual(env["NLM_LLM_RUN_ID"], "baseline-run")

    def test_action_chain_response_budget_is_global_across_samples(self):
        chains = [["a"] * 6, ["b"] * 6, ["c"] * 6]
        self.assertEqual(
            bound_action_chains(
                chains,
                max_proposals=2,
                max_actions_per_proposal=5,
                max_total_actions=8,
            ),
            [["a"] * 5, ["b"] * 3],
        )


if __name__ == "__main__":
    unittest.main()
