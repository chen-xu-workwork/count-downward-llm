"""Black-box transfer-launcher checks using fake Python and fake tmux only."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_zenotravel_transfer_30_40.sh"
LAUNCHER = ROOT / "scripts" / "start_zenotravel_transfer_30_40_tmux.sh"


def bash_path(path: Path) -> str:
    value = path.resolve().as_posix()
    return "/" + value[0].lower() + value[2:] if os.name == "nt" else value


def find_bash() -> str | None:
    if os.name == "nt":
        candidate = Path("C:/Program Files/Git/bin/bash.exe")
        return str(candidate) if candidate.exists() else None
    return shutil.which("bash")


@unittest.skipUnless(find_bash(), "Bash is required to exercise the Linux entry points")
class TransferShellTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="transfer-shell-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.bin = self.work / "bin"
        self.bin.mkdir()
        self.capture = self.work / "arguments.txt"
        self.interpreter_capture = self.work / "interpreter.txt"
        self.policy_capture = self.work / "partial.txt"
        self.status_capture = self.work / "status.txt"
        self.bash = find_bash()
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("COUNT_", "CONDA_", "NLM_", "LD_"))}
        self.env.update(
            COUNT_PROJECT_ROOT=bash_path(ROOT), COUNT_DATA_ROOT="/data/new data root",
            COUNT_MODEL_PATH="/weights/frozen depots checkpoint",
            COUNT_RESULTS_DIR=bash_path(self.work / "results with spaces"),
            COUNT_PLANNER_PYTHON=bash_path(self.bin / "fake-python"),
            COUNT_RUNTIME_ENV_PREFIX="/missing/runtime", COUNT_RUNTIME_LIB_DIR="/missing/libraries",
            COUNT_LLM_PARTIAL_ONLY="1",
        )
        self.env["PATH"] = (bash_path(self.bin) + ":/usr/bin:/bin" if os.name == "nt"
                            else str(self.bin) + os.pathsep + self.env["PATH"])
        self.write_python_stub()

    def executable(self, name: str, source: str) -> Path:
        path = self.bin / name
        path.write_text(source, encoding="utf-8", newline="\n")
        path.chmod(0o755)
        return path

    def write_python_stub(self, exit_status: int = 0) -> None:
        self.executable(
            "fake-python", "#!/usr/bin/env bash\n"
            f"printf '%s\\n' \"$@\" > {shlex.quote(bash_path(self.capture))}\n"
            f"printf '%s\\n' \"$0\" > {shlex.quote(bash_path(self.interpreter_capture))}\n"
            f"printf '%s\\n' \"${{COUNT_LLM_PARTIAL_ONLY:-unset}}\" > {shlex.quote(bash_path(self.policy_capture))}\n"
            f"exit {exit_status}\n",
        )

    def run_script(self, path: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([self.bash, bash_path(path), *arguments], env=self.env,
                              capture_output=True, text=True, timeout=30, check=False)

    def captured_arguments(self) -> list[str]:
        return self.capture.read_text(encoding="utf-8").splitlines()

    def value(self, arguments: list[str], flag: str) -> str:
        return arguments[arguments.index(flag) + 1]

    def test_prepare_preserves_data_root_and_live_only_policy(self) -> None:
        result = self.run_script(RUNNER, "--prepare-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.captured_arguments()
        self.assertEqual(args[:2], ["-m", "hybrid_planner.transfer"])
        self.assertEqual(self.value(args, "--data-root"), "/data/new data root")
        self.assertEqual(self.value(args, "--model-path"), "/weights/frozen depots checkpoint")
        self.assertEqual(self.value(args, "--sample-30"), "20")
        self.assertEqual(self.value(args, "--sample-40"), "10")
        self.assertEqual(self.value(args, "--parallelism"), "6")
        self.assertNotIn("--dataset-root", args)
        self.assertNotIn("--prompt-domain-code", args)
        self.assertNotIn("--stage", args)
        self.assertNotIn("--llm-partial-only", args)
        self.assertEqual(self.policy_capture.read_text().strip(), "0")
        self.assertIn("baseline=not-run", result.stdout)

    def test_explicit_paths_and_sampling_overrides_are_forwarded(self) -> None:
        self.env.update(COUNT_DATASET_ROOT="/pddl/zeno validation", COUNT_PROMPT_DOMAIN_CODE="/manual/domain.txt",
                        COUNT_SAMPLE_30="7", COUNT_SAMPLE_40="4", COUNT_SAMPLE_SEED="303",
                        COUNT_EXPERIMENT_SEED="202", COUNT_PARALLELISM="3")
        result = self.run_script(RUNNER, "--prepare-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.captured_arguments()
        for flag, value in (("--dataset-root", "/pddl/zeno validation"), ("--prompt-domain-code", "/manual/domain.txt"),
                            ("--sample-30", "7"), ("--sample-40", "4"), ("--sample-seed", "303"),
                            ("--experiment-seed", "202"), ("--parallelism", "3")):
            self.assertEqual(self.value(args, flag), value)

    def test_tmux_drops_stale_paths_and_records_failure(self) -> None:
        self.env.update(COUNT_TMUX_SESSION="transfer-test", COUNT_EXPERIMENT_SEED="505",
                        COUNT_SAMPLE_30="9", COUNT_SAMPLE_40="5", COUNT_VLLM_GPUS="2,3")
        self.write_python_stub(exit_status=7)
        self.executable(
            "tmux", "#!/usr/bin/env bash\ncase \"$1\" in\n"
            "has-session) exit 1 ;;\nnew-session)\n"
            "env -i PATH=/usr/bin:/bin COUNT_DATA_ROOT=/old COUNT_DATASET_ROOT=/old/pddl COUNT_PROMPT_DOMAIN_CODE=/old/depots.txt bash -c \"${@: -1}\"\n"
            "run_status=$?\n"
            f"printf '%s\\n' \"$run_status\" > {shlex.quote(bash_path(self.status_capture))}\n"
            "exit 0 ;;\n*) exit 9 ;;\nesac\n",
        )
        result = self.run_script(LAUNCHER, "--prepare-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.captured_arguments()
        self.assertEqual(self.value(args, "--data-root"), "/data/new data root")
        self.assertEqual(self.value(args, "--sample-30"), "9")
        self.assertEqual(self.value(args, "--sample-40"), "5")
        self.assertEqual(self.value(args, "--experiment-seed"), "505")
        self.assertEqual(self.value(args, "--vllm-gpus"), "2,3")
        self.assertNotIn("--dataset-root", args)
        self.assertNotIn("--prompt-domain-code", args)
        self.assertEqual(self.status_capture.read_text().strip(), "7")
        transcript = (self.work / "results with spaces/tmux-run.log").read_text()
        self.assertIn("status=7", transcript)

    def test_prepare_and_run_select_same_runtime_python(self) -> None:
        prefix = self.work / "model environment"
        (prefix / "bin").mkdir(parents=True)
        (prefix / "lib").mkdir()
        (prefix / "lib/libstdc++.so.6").write_text("CXXABI_1.3.15", encoding="utf-8")
        for name, source in (("python3", (self.bin / "fake-python").read_text()),
                             ("vllm", "#!/usr/bin/env bash\nexit 0\n")):
            path = prefix / "bin" / name
            path.write_text(source, encoding="utf-8", newline="\n")
            path.chmod(0o755)
        for variable in ("COUNT_RUNTIME_ENV_PREFIX", "COUNT_RUNTIME_LIB_DIR", "COUNT_PLANNER_PYTHON"):
            self.env.pop(variable, None)
        self.env["CONDA_PREFIX"] = bash_path(prefix)
        result = self.run_script(RUNNER, "--prepare-only")
        self.assertEqual(result.returncode, 0, result.stderr)
        first_interpreter = self.interpreter_capture.read_text().strip()
        first_args = self.captured_arguments()
        result = self.run_script(RUNNER)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(first_interpreter, bash_path(prefix / "bin/python3"))
        self.assertEqual(self.interpreter_capture.read_text().strip(), first_interpreter)
        self.assertEqual(self.captured_arguments(), [arg for arg in first_args if arg != "--prepare-only"])

    def test_syntax_and_lf(self) -> None:
        for path in (RUNNER, LAUNCHER):
            with self.subTest(path=path.name):
                self.assertNotIn(b"\r", path.read_bytes())
                result = subprocess.run([self.bash, "-n", bash_path(path)], capture_output=True,
                                        text=True, check=False)
                self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
