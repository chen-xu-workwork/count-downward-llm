import os
from pathlib import Path
import re
import socket
import subprocess
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BINARY = PROJECT_ROOT / "builds" / "release64" / "bin" / "downward"
SCRIPT = PROJECT_ROOT / "tests" / "run_mock_rollout_wsl.sh"


@unittest.skipUnless(os.name == "posix", "integration test runs in Linux/WSL")
@unittest.skipUnless(BINARY.is_file(), "build release64 before integration test")
class LazyRolloutIntegrationTests(unittest.TestCase):
    def test_preemptive_rollout_reanchors_and_preserves_normal_edges(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        env = os.environ.copy()
        env["HYBRID_TEST_PORT"] = str(port)
        completed = subprocess.run(
            ["bash", str(SCRIPT)],
            cwd=PROJECT_ROOT,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=60,
            check=True,
        )
        output = completed.stdout
        self.assertIn("MOCK_LLM_REQUEST", output)
        self.assertIn("burst_finished", output)
        self.assertIn("result=completed", output)
        self.assertIn("llm_actions_processed=10", output)
        match = re.search(
            r"llm_states_new=(\d+).*llm_states_reopened=(\d+)"
            r".*llm_states_duplicate=(\d+).*llm_normal_edges_generated=(\d+)",
            output,
        )
        self.assertIsNotNone(match)
        new_states, reopened, duplicates, normal_edges = map(int, match.groups())
        self.assertEqual(new_states + reopened + duplicates, 10)
        self.assertGreater(new_states, 0)
        self.assertGreater(normal_edges, 0)
        self.assertIn("Solution found.", output)


if __name__ == "__main__":
    unittest.main()
